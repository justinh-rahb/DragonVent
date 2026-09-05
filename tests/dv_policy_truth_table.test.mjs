// Truth-table test for the AUTO vent decision (dv_policy `decide_auto_target`).
// It extracts the real function from the firmware source and compiles it against a
// small harness, so the intended behavior is locked in rather than asserted by eye.
//
// Intended policy (product decision):
//   1. no reliable data / ERROR            -> hold whatever we're commanding
//   2. DragonBreath heating (soak/hold/dry) -> CLOSED, ahead of print state
//   3. printing + heat-retaining material   -> CLOSED
//   4. printing + venting/unknown material  -> OPEN
//   5. not printing + not heating           -> OPEN  (cooldown; a cold chamber has
//                                                      nothing to retain, so never reseal)
import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';

const root = fileURLToPath(new URL('../', import.meta.url));

// Extract the balanced-brace body of a named function from C source.
function block(src, needle) {
  const a = src.indexOf(needle);
  assert.ok(a >= 0, `not found: ${needle}`);
  let i = src.indexOf('{', a), depth = 1;
  for (i++; depth && i < src.length; i++) {
    if (src[i] === '{') depth++;
    if (src[i] === '}') depth--;
  }
  assert.equal(depth, 0);
  return src.slice(a, i);
}

test('decide_auto_target covers the AUTO truth table', () => {
  const policy = readFileSync(join(root, 'firmware/components/dv_policy/dv_policy.c'), 'utf8');
  const decide = block(policy, 'static dv_motor_target_t decide_auto_target');

  const code = [
    '#include <assert.h>',
    '#include <string.h>',
    // Types the extracted function closes over.
    'typedef enum { DV_MOTOR_TARGET_OPEN, DV_MOTOR_TARGET_CLOSED } dv_motor_target_t;',
    'typedef enum { MAT_PREFER_UNKNOWN, MAT_PREFER_OPEN, MAT_PREFER_SEALED } material_pref_t;',
    'typedef struct { int reliable, error, active; int chamber_heating; float bed_temp;',
    '                 char material[16]; const char *state; } auto_input_t;',
    'static dv_motor_target_t s_current_target;',
    // Stub the material rule: ABS seals, PLA vents, anything else is unknown. (The
    // real rule table is exercised separately; here we only pin decide_auto_target.)
    'static material_pref_t material_preference(const char *m){',
    '  if(!strcmp(m,"ABS")) return MAT_PREFER_SEALED;',
    '  if(!strcmp(m,"PLA")) return MAT_PREFER_OPEN;',
    '  return MAT_PREFER_UNKNOWN; }',
    decide,
    'int main(void){',
    '  auto_input_t st;',
    // 1. not reliable -> hold current
    '  s_current_target=DV_MOTOR_TARGET_CLOSED;',
    '  st=(auto_input_t){.reliable=0}; assert(decide_auto_target(&st)==DV_MOTOR_TARGET_CLOSED);',
    '  s_current_target=DV_MOTOR_TARGET_OPEN;',
    '  st=(auto_input_t){.reliable=0}; assert(decide_auto_target(&st)==DV_MOTOR_TARGET_OPEN);',
    // 2. error -> hold current
    '  s_current_target=DV_MOTOR_TARGET_CLOSED;',
    '  st=(auto_input_t){.reliable=1,.error=1}; assert(decide_auto_target(&st)==DV_MOTOR_TARGET_CLOSED);',
    // 3. Breath heating -> CLOSED, regardless of print state / material
    '  st=(auto_input_t){.reliable=1,.chamber_heating=1,.active=0};',
    '  assert(decide_auto_target(&st)==DV_MOTOR_TARGET_CLOSED);',
    '  st=(auto_input_t){.reliable=1,.chamber_heating=1,.active=1}; strcpy(st.material,"PLA");',
    '  assert(decide_auto_target(&st)==DV_MOTOR_TARGET_CLOSED);',
    // 4. printing + sealed material -> CLOSED
    '  st=(auto_input_t){.reliable=1,.active=1}; strcpy(st.material,"ABS");',
    '  assert(decide_auto_target(&st)==DV_MOTOR_TARGET_CLOSED);',
    // 5. printing + venting/unknown material -> OPEN
    '  st=(auto_input_t){.reliable=1,.active=1}; strcpy(st.material,"PLA");',
    '  assert(decide_auto_target(&st)==DV_MOTOR_TARGET_OPEN);',
    '  st=(auto_input_t){.reliable=1,.active=1}; strcpy(st.material,"NYLONX");',
    '  assert(decide_auto_target(&st)==DV_MOTOR_TARGET_OPEN);',
    // 6. idle + not heating -> OPEN (cooldown; never reseal a cold chamber)
    '  s_current_target=DV_MOTOR_TARGET_CLOSED;',
    '  st=(auto_input_t){.reliable=1,.active=0,.chamber_heating=0};',
    '  assert(decide_auto_target(&st)==DV_MOTOR_TARGET_OPEN);',
    '  return 0; }',
  ].join('\n');

  const dir = mkdtempSync(join(tmpdir(), 'dv-policy-'));
  try {
    writeFileSync(join(dir, 'test.c'), code);
    execFileSync(process.env.CC || 'cc',
      ['-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
       join(dir, 'test.c'), '-o', join(dir, 'test')], { stdio: 'pipe' });
    execFileSync(join(dir, 'test'), { stdio: 'pipe' });
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
