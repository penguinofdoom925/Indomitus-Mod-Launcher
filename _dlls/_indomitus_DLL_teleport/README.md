# GEM Teleport 0.1.1

Engine extension DLL for Call to Arms: Gates of Hell (GEM, 1.067) that adds a **`{teleport ...}` instruction** to the
custom interface actions (`/interface/action/*.set`, the control-panel buttons). A soldier given such an action is
locked, waits, is moved to the clicked ground point, waits again and is released - the "jump pack" ability, with the
flight masked by the mod's own animations and FX.

Loaded by the Indomitus launcher (any file name). Same DLL for `call_to_arms.exe`, `call_to_arms_ed.exe` and
`call_to_arms_server.exe`; every address is found at startup from strings, RTTI and byte patterns, and a finder that
fails only turns its feature off (see `gem_teleport.log`). **It changes the simulation: every peer of a multiplayer
match needs the DLL**, and action files that use `{teleport}` must live where only the launcher loads them (without the
DLL the engine reports "Unknown instruction" and the action is broken).

Status: 0.1.1 finders pass offline on game 6AB3DCC7, editor 6AB3DD83, server 6AB3DD21, both on the vanilla exe and
with the patches of the DLLs that load before it simulated; `dist\gem_teleport.dll` built (clang / llvm-mingw, the
toolchain `zig cc` wraps, same flags as build.bat, no warnings; imports only KERNEL32 + the Windows UCRT).
**Not yet run in game** - 0.1.0 never switched on in a launcher run (see Changes).

## Changes

**0.1.2** (Nov 8 2026, Alex: a squad jumped onto one point)
- New `{spread M}` parameter (ini `default_spread`, 0 = off): each soldier lands at a point scattered uniformly within
  M metres of the click. MP-safe: the seed is a hash of the unit's start position and the clicked point (lockstep
  state every peer shares), and the point comes from rejection sampling with plain IEEE arithmetic (no RNG state, no
  libm trig). The `max_radius` check still uses the clicked point; z stays the clicked ground height (on a slope the
  unit may land slightly above/below ground until it moves - not checked yet). No obstacle/water check for the
  scattered point either.

**0.1.1** (review of 0.1.0 against the launcher logs of 29-30 Oct)
- *Fix - the DLL was OFF in every launcher run.* `ReadFloat` was taken from `eHealthDamageCount` vtable slot 5, which
  GEM ScriptExt (loaded earlier) replaces: `reader: eHealthDamageCount not found`. Now found by its own code
  (unique signature + the reader helper `Enter` calls).
- *Fix - the order lock would have been OFF next.* `Entity::GiveOrder` was read from the move action's call site, which
  GEM Fauna's squad pace redirects. Now taken from `eCustom::Execute`'s tail jump (the path our actions use), the move
  site is the fallback.
- *Fix* - with `lock_orders=0`, `teleport_end` never fired and `invulnerable` did nothing (both hung on the lock).
  The "teleporting" set is now separate; the two options are independent.
- Sub-order installed with the engine's own `SetSubOrder` instead of a hand-made copy; eSingleOrder's empty `Start`
  is no longer overridden.
- eCustom slot 15 / damage slot 9 already hooked by another DLL: chained instead of refused; any number of damage
  execute functions.
- Exit trace removed (GEM Fauna owns those imports, 0.1.0 logged "0 import(s) hooked"); C++-throw logging is opt-in
  (`log_cpp_exceptions`), so the engine's load-time throws are no longer logged three times.
- Dead code removed (call-site redirection). Offline test runs every finder twice: vanilla, then with ScriptExt's
  vtable swaps, Fauna's call redirection and pre-hooked slots simulated.
- `mod_side`: tag pattern to tell abilities apart (`{effect "<ability>_start"}` in the action file).

## The action file

```
{action
	{target ground}            ; ground clicks only - a click on an entity (house, wall, vehicle, tree) is refused
	{cursor "jump"}
	{notifyselect}
	{item "jetpack_ability"} {item_in_box}
	{button "jetpack"} {Key "actions/jetpack"}
	{apply_to_all}
	{instructions
		{effect "jetpack_start"}   ; optional: tags the soldier so the shared events know which ability this is
		{teleport {max_radius 40} {pre_use_delay 0.8} {post_use_delay 0.6}}
	}
}
```

| parameter | unit | meaning | default (ini) |
|---|---|---|---|
| `max_radius` | metres | a ground click farther from the soldier shows the forbidden cursor; the order also fails if the unit is farther when it executes | `default_max_radius=30` |
| `pre_use_delay` | seconds | from the click (lock, `teleport_begin`) to the move (`teleport_jump`) | `default_pre_use_delay=0.5` |
| `post_use_delay` | seconds | from the move to the release (`teleport_end`) | `default_post_use_delay=0.5` |

Timing runs on the engine's own order countdown (simulation time, milliseconds), so all multiplayer peers agree.
Other instructions can precede or follow `{teleport}` in the same `{instructions}` block (`{fsm_request}`,
`{animations ... totaltime}`, `{take_item}`...); the custom order runs them in sequence. `nocheck` is accepted like
for any instruction (`{teleport nocheck {...}}` skips the per-tick re-check of the action's conditions).

### While teleporting (pre_use_delay + post_use_delay)

- `Entity::GiveOrder` refuses every new order for the soldier (move, attack, another jump...) - `lock_orders=1`.
- The `health_damage_*` interaction actions (pierce, blast, blow, count, crush: all infantry health loss) do nothing
  to the soldier - `invulnerable=1`. Independent of `lock_orders`.
- Not covered: the soldier's own AI (it may still aim/fire), fire/burning.
- Humans only (the unit's human flag); fauna and vehicles are refused with a log line.

### Events for the visuals (`events=1`)

The DLL fires three named interaction events on the soldier (`{on "..."}` in its interaction_entity script):

| event | when |
|---|---|
| `teleport_begin` | at the click, after the lock |
| `teleport_jump` | right after the position change |
| `teleport_end` | at the release |

All `{teleport}` actions fire the same three events. To tell abilities apart, start each action's `{instructions}`
with `{effect "<ability>_start"}` - the engine's own instruction, it fires `{on "<ability>_start"}` on the soldier at
once - tag the soldier there, and branch on the tag in the shared events (`{if tagged "tp_jetpack"}`).
`mod_side/teleport_events.inc` shows it for two abilities.

Put animations in the events, not before `{teleport}` in the action file: instructions before `{teleport}` run before
the lock starts. A take-off animation started from `teleport_begin` (length ~ `pre_use_delay`) and a landing animation
from `teleport_jump` (length ~ `post_use_delay`) make the teleport read as a jump; the entity never leaves the ground.

### Landing point

- `{target ground}` makes the engine refuse a click on an entity, so the point is never inside a house, wall, vehicle
  or tree. Terrain passability (deep water, steep slopes, map edge) is **not** checked in 0.1.0.
- `{apply_to_all}` sends every selected soldier to the same point (they land on top of each other).

## ini

`gem_teleport.ini` next to the DLL: `enabled`, `log`, `console`, `verbose`, `crash_log`, `log_cpp_exceptions`,
`default_max_radius`, `default_pre_use_delay`, `default_post_use_delay`, `events`, `lock_orders`, `invulnerable`.
Everything from `default_max_radius` down changes the simulation: in multiplayer all players need the same values (the
sealed launcher payload guarantees it).

## Build

`build.bat` - zig cc, mingw-w64 target, unity build of `src/gem_teleport.c`. Uses `.\zig\zig.exe`, else
`..\_indomitus_DLL_scriptext\zig\zig.exe`, else zig on PATH / `python -m ziglang`. Output `dist\gem_teleport.dll` +
`gem_teleport.ini`; copy both into the launcher payload and rebuild the launcher.

Offline check (Linux/WSL, after a game update): `sh tests/run_offline_check.sh <game.exe> <editor.exe> <server.exe>`
maps each exe and runs every finder twice (`TP_TEST` build: vanilla, then with the earlier DLLs' patches simulated),
then syntax-checks the Windows build with the stub headers (which define `near`/`far` like the real ones).

## Source layout

```
src/gem_teleport.c   unity file: version, ini/log names, include order
src/tp/core.c        log, ini, PE sections, .pdata lookup, byte/string/RIP search, RTTI vtables   (GEM Fauna core)
src/tp/patch.c       alloc_near, thread suspension, install_jmp5, vt_hook                        (GEM Fauna patch)
src/tp/engine.c      finders: instruction registry + reader API, ReadFloat, eCustom / eCustomActionOrder,
                     eSingleOrder + SetSubOrder, Entity::SetPos, Entity::GiveOrder, damage classes, world scale, events
src/tp/teleport.c    TpInstr (parse / execute), TpOrder (the sub-order: Update / dtor), teleporting set,
                     hooks (IsApplicable, GiveOrder, damage execute), registry insertion
src/tp/crashlog.c    vectored exception log
src/tp/main.c        init order, registry watch thread, DllMain
src/tp/test_main.c   TP_TEST main
mod_side/            example action file and interaction snippet
```

## How it works (engine facts, GoH 1.067)

- `{instructions}` parser (`eInterfaceActionInstruction.cpp`): a linked registry `{next, {name, ctor}}` with a
  function-local static head (game `0x1415a0480`, accessor `0x140b5fc50`), walked with `strcmp`; no match = "Unknown
  instruction". The DLL prepends its node once the registry is built (watch thread, compare-exchange, 2 s re-check) -
  no code patch, same mechanism as ScriptExt's `{if}` conditions. Nodes come from the engine's `operator new`
  because the registry's atexit destructor frees them.
- Instruction vtable: `[0]` deleting dtor `[1]` parse(reader) `[2]` ready() `[3]` execute(action, order, entity,
  Target*, flag*). The engine writes the `nocheck` byte at `+8`.
- `Order::eCustomActionOrder::Update` (vt 19) runs the instructions; an instruction that takes time installs a
  sub-order at `order+0x30`; the parent ticks it (`eSingleOrder::Tick`, vt 3) and moves on when it returns 1.
  `TpOrder` is an `eSingleOrder` clone (vtable copied, slots 0 and 19 replaced) installed with the engine's
  `SetSubOrder` (`0x140901940`), and it uses the base class's own wait:
  flag bit 20 + a millisecond countdown at `+0x24` that `Tick` decrements by the simulation quant.
  `Finish` (vt 4) calls `Update(this, 2)` on a started order - the release point.
- Move: `Entity::SetPos(entity, pos[3], 0)` (`0x1406fdd20`, the editor's `e_teleport` action), which rebuilds the
  world matrix (entity `+0x50`, position `+0x74`) and notifies the world (vt 33, reason 3).
- Cursor / order validation: `Interface::Action::eCustom` vt 15 `IsApplicable(this, flag*, entity, Target*)`,
  hooked; Target = `{int type (1 ground, 2 entity); float pos[3]; ...}`. The action's instructions sit in a
  `vector<Instruction*>` at `action+0x4A8`.
- Lock: inline hook on `Entity::GiveOrder` (`0x1407efa10`, the tail jump of `eCustom::Execute`; refused orders are
  Finished + deleted like the engine does); vtable slot 9 of `eHealthDamage{,Pierce,Blast,Blow,Count,Crush}` (two
  execute functions in vanilla) skips teleporting units.
- Coexistence (load order Fauna, BreedTags, MeleeChase, ScriptExt, NoProne, then Teleport): ScriptExt swaps
  eHealthDamageCount/Pierce/Blast slots 5/10/12, Fauna redirects the move action's MovementChange/GiveOrder calls -
  no finder reads those. No other payload DLL touches the instruction registry, eCustom, GiveOrder's entry or slot 9.
- World scale 20 units/m (`.data` 20.0, 1000, 1000, 3600); named events through `Tag(cstr)` / `Ctx(entity)` /
  `FireNamed` as in ScriptExt.

## Open

1. Run in the editor via the launcher: the log must show every feature ON, `{teleport} instruction ON (registered)`,
   then `{teleport} parsed: ...` when the action loads. Click test: forbidden cursor outside the radius and on
   entities; the soldier jumps after `pre_use_delay`; a move order during the jump logs `lock: order refused`.
   Also try it on a soldier inside a vehicle or building (not handled specially - see what the engine does).
2. Animations / FX on the three events (mod side).
3. Possible 0.2: terrain passability check (pather network), spread for `{apply_to_all}`, AI-side immobility.
