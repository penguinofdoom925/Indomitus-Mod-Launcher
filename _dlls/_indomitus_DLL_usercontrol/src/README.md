# GEM UserControl 0.1

Replaces the mission trigger `40k_triggers/user_control` (the loop that ran every 1 s in battle zones /
frontlines and every 3 s in conquest).

A unit that carries the entity tag `user_control` is handed to its player at once:

- brain control = user (exactly what the trigger action `actor_state {control user}` writes)
- tags `player` and `_user` added
- tag `user_control` removed

Unit scripts keep doing what they do today: `{tags add "user_control"}` in `{on spawn}` (call-in spawners in
`40k_spawners\impguard.inc`, `heavy_turret_hb` / `heavy_turret_rocket` / `beacon` in `turret_specifics.inc`).
No mission trigger is needed.

**Status: offline-tested only (finders + logic on game, editor and server 1.067). Not run in game yet.**
Do not delete the trigger from the `.mi` files until it is confirmed in game.

## When it acts
- The moment a `{tags add}` script action adds the tag (also the 0.1 s delayed add of `beacon`).
- The moment the unit gets an owner, if it was tagged before it had one.

So both orders (tag first, owner first) are covered, with no timer and no per-quant work.

## Who gets the unit
| Owner of the tagged unit | Default | Ini |
|---|---|---|
| human player | handed over | |
| bot player | **not** handed over: the tag is removed, the unit stays AI controlled, no `player` / `_user` | `SkipBots=0` hands bots' units over too |
| owner number listed in `SkipPlayers` | not handed over (tag removed) | `SkipPlayers=0` = the old conquest rule |
| owner the engine has no MP player record for (editor, maybe single-player) | handed over | `UnknownOwner=0` |
| no owner yet | tag kept, handed over when the owner is set | `NoOwner=1` hands over right away |
| not an actor (no owner, no brain) | tags only | |

How this differs from the triggers:
- Battle zones / frontlines trigger: never looked at the owner, so a bot's turrets and call-ins also got
  control user + `player` + `_user`. With `SkipBots=1` they no longer do. `SkipBots=0` gives the old result.
- Conquest trigger: skipped player 0 only (the hostile AI). With `SkipBots=1` every bot player is skipped,
  if the engine marks the conquest AI players as bots (to be confirmed in game: the log line shows
  `owner N (human|bot|unknown)`). If it shows `unknown` for player 0 in conquest, set `SkipPlayers=0`.
- A unit tagged while it has no owner and that never gets one keeps the tag and stays AI (the trigger gave
  it control user anyway). `NoOwner=1` restores that.
- A unit saved in a map file with the tag already on it is not handled at load (only when its owner is set
  again or the tag is added again).

## Files
| File | |
|---|---|
| `gem_usercontrol.dll` | the dll (game, editor and dedicated server) |
| `gem_usercontrol.ini` | `Enabled`, `Tag`, `AddTags`, `SkipBots`, `SkipPlayers`, `UnknownOwner`, `NoOwner`, `LateTagsHook`, `Log` |
| `gem_usercontrol.log` | next to the dll (`_editor` / `_server` suffix); `Log=1` writes one line per handled unit |

## Install
- Launcher: copy `gem_usercontrol.dll` + `gem_usercontrol.ini` into `launcher\payload\` and rebuild
  (`_tools\launcher_build\build_local.py`). `gem_aa_childinject` copies the payload into `<payload>\server\`
  and injects it into the dedicated server by itself.
- In-house test: start the game or editor, stay in the main menu, run `inject.bat` (in `out\`).

## Multiplayer
Brain control and tags are simulation state. **Every machine in a multiplayer game and the dedicated server
must run the dll** (the sealed launcher payload does that). A machine without it never hands the units over.

## Load order
- Loads after `gem_teamtags` (alphabetical). Both patch the same two `SetPlayer` vtable slots; this dll takes
  whatever is in the slot as the original, so the hooks chain.
- The `{tags}` action slot is patched on the first `SetPlayer` call, not at start-up, because other dlls
  (BreedTags, TeamTags, SensorTags, MeleeStance, weaponscript) find the tag functions by reading that slot's
  code while they start. `LateTagsHook=0` patches at start-up.

## How it works (game 1.067, everything found at start-up)
- `actor_state {control user}` (eHelperCommandActor.cpp, apply exe+0x99F940) only does
  `flags = [[entity+0xDC8]+0x398]; flags = (flags & ~3) | control` (AI = 0, user = 1) on entities with a brain.
  There is no engine function behind it, so the dll writes the same two bits. The offsets come from
  `eActor::IsUserControl` exe+0x7EF670 (reached through the RTTI vtable of `eCondition_UserControl`, the
  `{if user_control}` condition).
- Tag added: `{tags}` action `Execute(action, ctx)`, vtable slot 1 (exe+0x74E040). Entity = `[ctx]`, the
  action's own tag list at action+0x28, add flag at action+0x358. The hook runs the original, then looks at
  the entity only if this action adds the watched tag. Cost for every other `{tags}` call: one byte test and
  a scan of the action's 1-3 tags, 2.4 ns measured in the offline test.
- Owner set: `eActor::SetPlayer`, vtable slot +0x1D0 of eActor and eHuman (exe+0x7EEEC0).
- Human or bot: `mp_player.cpp` keeps one object per MP player: player number byte at +4, bot byte at +0x1E1
  (written from the lobby slot kind `bot`, exe+0xD0E315). The engine's own "bless actor to player"
  (exe+0xD02740) uses that byte to give bots control AI and humans control user. The dll asks
  `GetPlayerByNumber(game, uint8_t*)` exe+0xD0C3D0 on the game singleton `[exe+0x15A83C0]`.
  The editor exe has no mp_player code: every owner is `unknown` there.
- Tag changes go through the `{tags add}` action's own functions (copy, add_all, remove_all,
  `Entity::SetTags`, dtor), in one SetTags call per unit. Same derivation as GEM TeamTags.

## Build / test
`build.bat` -> `out\gem_usercontrol.dll`. `build.bat test` -> finders on game, editor and server, the
decision table, the control-bit write, the tag scan, a "slots already hooked by TeamTags" case and the
hook-cost measurement. Needs `pip install ziglang`, or `set ZIG_EXE=<path>\zig.exe`
(e.g. `Documents\IndomitusLauncher\zig\zig.exe`).

## After the in-game test: trigger blocks to delete
In each file the whole block from the line `{"40k_triggers/user_control"` to its closing `}`, i.e. up to the
line before the next trigger:

| File | Block ends before |
|---|---|
| `resource\map\multi\40k_triggers.mi` | `{"40k_triggers/forcefield_checker"` |
| `resource\map\multi\0_frontlines\40k_triggers.mi` (mirror, same edit) | `{"40k_triggers/forcefield_checker"` |
| `resource\map\multi\40k_conq_triggers.mi` | `{"40k_triggers/tag_checker"` |

`map\test\40k_conq_triggers.mi` and `map\test\test_conq.mi` carry copies of the trigger too.
