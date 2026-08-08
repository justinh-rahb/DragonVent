# Shared component policy

DragonVent consumes board-neutral firmware services from
[`justinh-rahb/dragon-core`](https://github.com/justinh-rahb/dragon-core) through
the ESP-IDF component manager.

Every dependency in `firmware/main/idf_component.yml` is pinned to an exact Git
commit. Updating the core is therefore an intentional source change: update all
pins together, inspect the upstream diff, rebuild the classic ESP32 target, and
repeat the hardware smoke test before merging.

`firmware/dependencies.lock` is committed as part of the application so CI and
local builds resolve the same core commit and transitive registry components.
When changing a manifest pin, regenerate and review the lockfile in the same
change.

The vent-specific board, motor, hall-sensor, button, LED, policy, and portal
components remain in this repository and use the `dv_*` DragonVent-owned
prefix. Shared components use `dc_*`. Compatibility-sensitive persisted and
external identifiers—including the existing NVS namespace and keys—remain
unchanged unless an explicit migration is provided.
