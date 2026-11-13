# GEM TeamSound 0.2

Lets each player hear a different sound for the same script event, e.g. "friendly support incoming" for the caller's team and "enemy support incoming" for the other team.

Status (Nov 6 2026): builds; finders + logic tested offline on game / editor / server 1.067 (`build.bat test` = ALL OK x3). **Not run in game yet.**

## Usage in entity scripts

Put the token `@side` in a `{start_sound}` path or a `{play_sound}` link name. On every machine the dll swaps it for a word that depends on how the **local player** relates to the **player who owns the entity**:

| local player is…              | word (ini)                |
|-------------------------------|---------------------------|
| the owner                     | `ally` (`OwnWord`)        |
| an ally of the owner          | `ally` (`AllyWord`)       |
| an enemy of the owner         | `enemy` (`EnemyWord`)     |
| unknown (no owner, spectator, editor) | `neutral` (`NeutralWord`) |

```
{on spawn
	{start_sound "world/call-ins/support/ig_@side"}
}
```
plays the folder `world/call-ins/support/ig_ally` for the caller's team, `ig_enemy` for the other team, `ig_neutral` otherwise. Each folder needs its `folder.set`.

With links:
```
{link_sound "support_ally"    "world/call-ins/support/ig_ally"}
{link_sound "support_enemy"   "world/call-ins/support/ig_enemy"}
{link_sound "support_neutral" "world/call-ins/support/ig_neutral"}
{play_sound "support_@side"}
```

Pick the voice by the caller's faction in the path (an IG call-in's `_enemy` variant = a TG-voiced "enemy support" line, because only the enemies hear it).

**Owner** = the script entity's player (what `{player N}` sets; only actors have one: vehicles, humans, guns, droppod spawners) or, if the entity has none, the **effector's** player (`UseEffector=1`). Ownerless entities (e.g. the vanilla `offmap_dummy` artillery helpers are plain `game_entity`) fall to `neutral`. Check with `Log=1`: every rewrite is logged with owner, source (entity/effector/none), local player and relation.

Without the dll the token is not replaced: `play_sound` finds no link (silent), `start_sound` asks for a path that does not exist.

## ini (`gem_teamsound.ini`, next to the dll)

`Enabled`, `Token` (default `@side`), `OwnWord`, `AllyWord`, `EnemyWord`, `NeutralWord`, `UseEffector`, `Log`. Set `OwnWord=own` for a third variant only the caller hears.

## Multiplayer

Only the choice of sound file changes, nothing in the simulation, so machines cannot desync from it. Untested risk: if the engine's sound variant pick (random file in a folder) uses the synced random generator, different machines playing different folders could still drift. Vanilla already plays sounds per machine (distance culling), so this is unlikely.

The dll does nothing on the dedicated server (no sound there).

## Engine (game 1.067; all found at runtime by RTTI / byte patterns)

- `eAction_SoundPlay` (`play_sound`) / `eAction_SoundSpawn` (`start_sound`): vtable slot 1 = `Execute(action, ctx)`, game exe+0x755280 / +0x755120. `action+0x30` = name/path `std::string`. `ctx+0` = entity, `ctx+8` = effector. The dll patches the two vtable slots (no code patch) and swaps the string for the duration of the call.
- `eAction_ProcessView` Execute (exe+0x75F490), the vanilla `{view show|hide|start|stop|pause "x" my|ally|enemy|neutral}` per-player filter, supplies: actor flag `[entity+0x30] & 2`, player byte `entity+0xDA8` (`eActor`, 0xFF = none), local player byte (exe+0x1487A88 via `GetLocalPlayer` exe+0xA464F0), `Relation(table exe+0x159BA10, &a, &b)` exe+0xA46AC0 → 2 same/ally, 1 enemy, 0 neutral (table int[18×18], stride at table+0x10).
- Editor: play +0x5D24E0, spawn +0x5D2350, local +0x12FEA58, relation +0x8CF630. Server: play +0x56B310, spawn +0x56B1B0.

## Build / test

`build.bat` → `out\gem_teamsound.dll`, `out\gem_inject.exe`. `build.bat test` → finder + logic test on the three exes. `inject.bat` loads the dll into a running game/editor (main menu, before loading a map) for in-house tests.

## Launcher integration

Plain dll, hooks in `DllMain`, reads `<dll name>.ini` next to it, logs to `<dll name>.log` (`_editor` / `_server` suffix). Chains with any dll that patched the same vtable slots earlier (it calls whatever pointer was there).

## 0.2: the listener's army (Nov 11 2026, offline-tested only)

- `{play_sound "@army=ig"}`: a name starting with `ArmyHint` is never played. If the entity belongs to the local player, the dll stores the word as his army. Human scripts send it 1 s after `breed_applied`.
- `@army` in a name becomes that word (`ArmyDefault` until a hint was seen).
- `@who` becomes `enemy_<army>` when the local player is an enemy of the entity's owner, otherwise the own / ally / neutral word. Example: `{play_sound "vo_w_@who"}` picks `vo_w_enemy_ig`, `vo_w_enemy_tg`, `vo_w_enemy_sm`, `vo_w_ally` or `vo_w_neutral`.
- Every token in a name is replaced. ini keys: `ArmyHint`, `ArmyToken`, `WhoToken`, `ArmyDefault`.
- Build without the ziglang pip module: `zig cc -target x86_64-windows-gnu -mcpu=baseline -O2 -shared -s -o out\gem_teamsound.dll gem_teamsound.c`.
