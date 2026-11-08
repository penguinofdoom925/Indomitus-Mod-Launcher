# GEM ContactDamage 0.1.0

Vehicle component hp damage from contact handlers (melee contactors) for Call to Arms: Gates of Hell 1.067 (game, editor, server).

**What it does:** in a vehicle's `{on contact}` handler (effector = the melee contactor),

    {tags add "melee_body_25"} {vehicle_health_damage_pierce} {tags remove "melee_body_25"}

takes 25 hp off the vehicle's `body` component the same way a piercing round does. It uses the engine's own subtract (hp first, then hp_broken), its break / destroy state changes with their script events, and in MP the host's damage command to the clients. The number comes from a `melee_body_<N>` tag on the vehicle (self), or on the effector if the vehicle has none. Decimals are allowed (`melee_body_12.5`).

**Without the dll** that script line crashes a host or single-player game: vanilla `vehicle_health_damage_pierce` reads the bullet with no null check, and a contact has no bullet. Only use it where the dll is guaranteed (launcher payload, and `gem_aa_childinject` for hosted games).

## Engine
- Hooked: `Interaction::eVehicleHealthDamagePierce` vtable (RTTI) slot 9 `Apply(this, ctx, actor)` (game exe+0x745960, editor 0x5C11F0, server 0x55B990) and slot 10 `GetDamage` (0x745DF0 / 0x5C1670 / 0x55BE20). Blast and every other action are untouched.
- When the ctx carries a bullet (flag `0x400000`), it is vanilla.
- With no bullet and no amount tag, nothing happens.
- With an amount: ctx+0x10 is pointed at a volume of the body component through the engine's `SetVolume(ctx, vol)` (0x76DDF0 / 0x5EC3D0 / 0x583E80). The original Apply runs, GetDamage answers N, and the ctx volume is restored afterwards.
- Layout: actor components = inline vector +0xB90, stride 0x28, `[desc+0x328]` = name Tag, text at +0x40; component +0x14 hp, +0x18 hp_broken, +0x1C hp max, +0x20 hp_broken max, +0x24 state (0 working / 1 broken / 2 destroyed). Actor volumes = vector +0x510, volume+0x38 = component.
- `armor_damage_modifier` is not applied on this path: N is raw hp.

## Multiplayer
Apply returns before GetDamage on MP clients, so only the host needs the dll (`call_to_arms_server.exe` for hosted games). Clients receive the result through the engine's `eVehicleHealthDamageCommand`.

## Settings (`gem_contactdamage.ini`)
| key | default | meaning |
|---|---|---|
| Enabled | 1 | install the hook |
| TagPrefix | melee_body_ | tag carrying the hp amount |
| Component | body | component that takes the damage |
| ReadEffector | 1 | fall back to the effector's tags |
| MaxDamage | 100000 | cap per hit |
| Log | 1 | `gem_contactdamage.log`: install line + first 40 hits (hp before/after, state) |

## Build
`build.bat` (zig cc: `pip install ziglang`). `build.bat test` runs the offline finder test on the three exes plus 7 logic cases. `inject.bat` injects the dll into a running game/editor for in-house tests.

## Status
Offline test ALL OK on game/editor/server 1.067 (Nov 7 2026). Not run in game. Untested: the actor `vt+0x1E8` notify that Apply calls with a bullet-less ctx, and who gets the kill credit when the body is destroyed this way.
