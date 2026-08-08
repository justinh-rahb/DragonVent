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
trap 'rm -f "$MAPPING_TMP" "$MISMATCH_TMP"' EXIT

extract_exact_refs() {
  awk '
    /^  [A-Za-z0-9_.-]+:$/ { name=$1; sub(/:$/, "", name); next }
    name != "" && /^    version:/ {
      value=$2; gsub(/["'\'' ]/, "", value)
      if (length(value) == 40 && value !~ /[^0-9a-f]/) print name, value
      name=""
    }
  ' "$1"
}

check_lock() {
  : > "$MISMATCH_TMP"
  [[ -f "$MANIFEST" && -f "$LOCK" ]] || return 0
  extract_exact_refs "$LOCK" > "$MAPPING_TMP"
  while read -r component expected; do
    actual=$(awk -v name="$component" '$1 == name { print $2; exit }' "$MAPPING_TMP")
    if [[ "$actual" != "$expected" ]]; then
      printf '%s expected %s, lock has %s\n' "$component" "$expected" "${actual:-missing}" >> "$MISMATCH_TMP"
    fi
  done < <(extract_exact_refs "$MANIFEST")
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
echo "Dependency lock matches every exact manifest ref."
