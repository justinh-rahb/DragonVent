#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR=${1:-.}
TARGET=${2:-esp32}
BUILD_DIR=${3:-build}
if [[ ! -d "$PROJECT_DIR" ]]; then echo "error: project directory not found: $PROJECT_DIR" >&2; exit 2; fi
PROJECT_DIR=$(cd "$PROJECT_DIR" && pwd)
if [[ "$BUILD_DIR" != /* ]]; then BUILD_DIR="$PROJECT_DIR/$BUILD_DIR"; fi

IDF_ROOT=${IDF_PATH:-"${HOME}/esp/esp-idf"}
IDF_TOOLS_ROOT=${IDF_TOOLS_PATH:-"${HOME}/.espressif"}
if [[ ! -f "$IDF_ROOT/export.sh" ]]; then
  echo "error: ESP-IDF export.sh not found at $IDF_ROOT/export.sh" >&2
  echo "set IDF_PATH to the ESP-IDF checkout to use" >&2
  exit 2
fi

# export.sh can leave a locally installed compiler off PATH. Add both compiler
# families up front, then prove the target-specific executable is callable.
for family in xtensa-esp-elf riscv32-esp-elf; do
  family_root="$IDF_TOOLS_ROOT/tools/$family"
  if [[ -d "$family_root" ]]; then
    while IFS= read -r bin_dir; do PATH="$bin_dir:$PATH"; done < <(find "$family_root" -type d -name bin | sort)
  fi
done
export PATH
# shellcheck disable=SC1090
source "$IDF_ROOT/export.sh" >/dev/null

case "$TARGET" in
  esp32) compiler=xtensa-esp32-elf-gcc ;;
  esp32c2|esp32c3|esp32c5|esp32c6|esp32h2|esp32p4) compiler=riscv32-esp-elf-gcc ;;
  esp32s2) compiler=xtensa-esp32s2-elf-gcc ;;
  esp32s3) compiler=xtensa-esp32s3-elf-gcc ;;
  *) echo "error: unsupported target for toolchain preflight: $TARGET" >&2; exit 2 ;;
esac
if ! command -v "$compiler" >/dev/null 2>&1; then
  echo "error: $compiler is not available after ESP-IDF environment setup" >&2
  echo "run: $IDF_ROOT/install.sh $TARGET" >&2
  exit 2
fi
echo "ESP-IDF: $IDF_ROOT"
echo "Python:  $(command -v python)"
echo "Compiler: $(command -v "$compiler")"

MANIFEST="$PROJECT_DIR/main/idf_component.yml"
LOCK="$PROJECT_DIR/dependencies.lock"
MAPPING_TMP=$(mktemp)
MISMATCH_TMP=$(mktemp)
RESOLVE_CACHE=$(mktemp)
trap 'rm -f "$MAPPING_TMP" "$MISMATCH_TMP" "$RESOLVE_CACHE"' EXIT
CHECKED_REFS=0

# The lock always records a resolved 40-hex object id under each component.
extract_lock_refs() {
  awk '
    /^  [A-Za-z0-9_.-]+:$/ { name=$1; sub(/:$/, "", name); next }
    name != "" && /^    version:/ {
      value=$2; gsub(/["'\'' ]/, "", value)
      if (length(value) == 40 && value !~ /[^0-9a-f]/) print name, value
      name=""
    }
  ' "$1"
}

# The manifest may pin either a raw commit id or a tag/branch name, so emit the
# git remote alongside the ref and let the caller resolve it. Filtering to
# 40-hex here (as an earlier version did) silently dropped every tag pin, which
# left the whole comparison a no-op that still reported success.
extract_manifest_refs() {
  awk '
    /^  [A-Za-z0-9_.-]+:$/ { name=$1; sub(/:$/, "", name); url=""; next }
    name != "" && /^    git:/ { url=$2; gsub(/["'\'' ]/, "", url); next }
    name != "" && /^    version:/ {
      value=$2; gsub(/["'\'' ]/, "", value)
      if (url != "") print name, url, value
      name=""; url=""
    }
  ' "$1"
}

# ref -> object id the lock would record. A raw commit id passes through; a tag
# or branch is resolved against the remote. Annotated tags resolve to the TAG
# object (refs/tags/<x>, not the ^{} peel) because that is what the component
# manager writes into the lock.
resolve_ref() {
  local url=$1 ref=$2 cached out sha
  if [[ ${#ref} -eq 40 && $ref =~ ^[0-9a-f]+$ ]]; then printf '%s' "$ref"; return 0; fi
  cached=$(awk -v u="$url" -v r="$ref" '$1 == u && $2 == r { print $3; exit }' "$RESOLVE_CACHE")
  if [[ -n $cached ]]; then printf '%s' "$cached"; return 0; fi
  out=$(git ls-remote "$url" "refs/tags/$ref" "refs/heads/$ref" 2>/dev/null || true)
  sha=$(printf '%s\n' "$out" | awk -v r="refs/tags/$ref" '$2 == r { print $1; exit }')
  [[ -z $sha ]] && sha=$(printf '%s\n' "$out" | awk -v r="refs/heads/$ref" '$2 == r { print $1; exit }')
  [[ -n $sha ]] && printf '%s %s %s\n' "$url" "$ref" "$sha" >> "$RESOLVE_CACHE"
  printf '%s' "$sha"
}

check_lock() {
  : > "$MISMATCH_TMP"
  CHECKED_REFS=0
  [[ -f "$MANIFEST" && -f "$LOCK" ]] || return 0
  extract_lock_refs "$LOCK" > "$MAPPING_TMP"
  local component url ref expected actual
  while read -r component url ref; do
    expected=$(resolve_ref "$url" "$ref")
    if [[ -z $expected ]]; then
      echo "warning: cannot resolve $component ref '$ref' from $url (offline?); not verified" >&2
      continue
    fi
    CHECKED_REFS=$((CHECKED_REFS + 1))
    actual=$(awk -v name="$component" '$1 == name { print $2; exit }' "$MAPPING_TMP")
    if [[ "$actual" != "$expected" ]]; then
      printf '%s pinned %s (%s), lock has %s\n' \
        "$component" "$ref" "$expected" "${actual:-missing}" >> "$MISMATCH_TMP"
    fi
  done < <(extract_manifest_refs "$MANIFEST")
  [[ ! -s "$MISMATCH_TMP" ]]
}

mkdir -p "$BUILD_DIR"
if ! check_lock; then
  stamp=$(date +%Y%m%d-%H%M%S)
  backup="$BUILD_DIR/dependency-cache-$stamp"
  mkdir -p "$backup"
  echo "Stale ESP-IDF dependency cache detected:"
  sed 's/^/  /' "$MISMATCH_TMP"
  mv "$LOCK" "$backup/dependencies.lock"
  if [[ -d "$PROJECT_DIR/managed_components" ]]; then mv "$PROJECT_DIR/managed_components" "$backup/managed_components"; fi
  echo "Quarantined stale dependency state under $backup"
fi

if [[ "${IDF_PREFLIGHT_ONLY:-0}" == "1" ]]; then
  echo "Toolchain and dependency-cache preflight passed."
  exit 0
fi

idf.py -C "$PROJECT_DIR" -B "$BUILD_DIR" -D "IDF_TARGET=$TARGET" build
if ! check_lock; then
  echo "error: resolved dependency lock does not match the manifest:" >&2
  sed 's/^/  /' "$MISMATCH_TMP" >&2
  exit 1
fi
if [[ "$CHECKED_REFS" -eq 0 ]]; then
  # Nothing verified is not the same as everything matching. Say so, so a
  # manifest the parser cannot read never masquerades as a clean check.
  echo "warning: no pinned manifest refs were verified against the lock" >&2
else
  echo "Dependency lock matches all $CHECKED_REFS pinned manifest ref(s)."
fi
