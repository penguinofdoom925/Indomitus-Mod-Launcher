# GEM SensorTags 0.3

While a unit (human, vehicle, gun) carries the entity tag `sensor_off`, its sensor is off:
it sees nothing, spots nothing for its side, and its seen list is dropped. This is the same state
the engine uses for a passenger in a `{turnoff {sensor}}` seat or a `senseless` unit.
Remove the tag and the sensor comes back in whatever state the engine wants for it at that moment
(still off if the unit sits in a turnoff seat or is senseless, otherwise on).

```
{tags add "sensor_off"}      ; blind
{tags remove "sensor_off"}   ; see again
```

- Any way of setting the tag works: script `{tags add/remove}`, spawn defines, triggers, other dlls.
- The tag is read on a unit's first tick and then once per `CheckMs` (ini, default 1000 ms; 0 = every tick,
  as 0.1 did). A unit that spawns tagged is blind at once; a tag added or removed later acts within `CheckMs`.
- Each actor has its own sensor. Tagging a vehicle switches off the vehicle's visors (driver, gunner,
  commander sights). Crew humans have their own sensors (usually already off in closed seats).
- Units without a `{sensor}` block are not affected.

## no_target (0.3)
While a unit carries the entity tag `no_target`, enemies do not target it: their sensors do not mark
it as attackable, so the AI neither shoots at it nor walks up to it. Remove the tag and it is a target again.

```
{tags add "no_target"}       ; not a target
{tags remove "no_target"}    ; a target again
```

- Read in the same pass as `sensor_off`: at once on a unit's first tick, then within `CheckMs`.
- Engine: an enemy's seen record gets its "may attack" flag only while bit 15 of the unit's brain flags
  (`[[actor+0xDC8]+0x398]`) is set. That bit is the SDL `targetable` state a seat with
  `{turnoff {targetable}}` clears for its crew. The dll clears it while tagged and sets it back on untag,
  only for units it cleared itself, so a crew seat's own state is kept.
- ini `[NoTarget] Enabled` / `Tag`. Host only in MP, like the sensor tag. Units without their own sensor are not looked at.
- Used by the drop pods until they land (enemies shot at pods being brought in). Works in game (Alex, Nov 13 2026).

## Files
| File | |
|---|---|
| `gem_sensortags.dll` | the dll (game, editor and dedicated server) |
| `gem_sensortags.ini` | `Enabled`, `Tag` (default `sensor_off`), `Log`, `CheckMs` (default 1000); `[NoTarget]` `Enabled`, `Tag` (default `no_target`) |
| `gem_sensortags.log` | written next to the dll (`_editor` / `_server` suffix for those exes) |

## Install
- Launcher: copy `gem_sensortags.dll` + `gem_sensortags.ini` into `launcher\payload\` (and the
  `server\` payload, for hosting).
- In-house test: start the game or editor, stay in the main menu, run `inject.bat` (in `out\`).
  Then check `gem_sensortags.log` for the `installed` line. Set `Log=1` to see every switch.

## Multiplayer
Sensing runs on the host only: clients get the seen lists from the host. So the host must run the dll
(the game when hosting from the game, `call_to_arms_server.exe` for a hosted MP game). Clients need it
only if they can host. The dll leaves the client-side sensor class alone.

## How it works (game 1.067, everything found at startup)
- `eSensor` (one per actor at actor+0xDD0, owner actor at sensor+0x260) has an "off" byte at +0x18.
  Every engine reader tests it against 0; every engine writer does "compare, write if different,
  call the clear-seen-lists slot (vtable +0x58)". Writers: place enter/exit (`{turnoff {sensor}}`),
  SetSenseless, control-mode changes.
- The dll patches two `eSensor` vtable slots: the sensor quant (+0x68, called every actor tick) and
  the clear slot (+0x58).
  The quant hook only looks at the unit's tags once per `CheckMs` (0.2); the clear hook looks every time.
- While tagged, the byte holds 2 + the engine's value (2 or 3). Readers see non-zero = off. Writers
  compare 2/3 with 0/1, so they always write and call clear; the clear hook keeps the value the engine
  just wrote and holds the sensor off again. When the tag goes, the quant hook writes the engine's
  value back. So the engine's own state is never lost.

Offline test: `build.bat test` runs the finders on game, editor and server (ALL OK on 1.067) plus
the hold/restore logic on a fake sensor. Works in game (Nov 7 2026, local launcher build).
