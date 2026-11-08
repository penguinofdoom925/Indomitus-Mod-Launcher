# GEM MeleeStance 0.7.13

Dawn of War style melee toggle + melee charge for Call to Arms: Gates of Hell 1.067 (game, editor, server).

**Stance:** while a human carries the script tag `melee_stance`, the engine's weapon/ammo choice (`set\target\ammo.set`)
only accepts melee ammo. The unit draws its melee weapon and keeps using it until the tag is removed.
Untagged units behave exactly like vanilla.

**Charge:** a human tagged `has_melee_weapon` with the melee weapon in hand (or in stance) who sees an enemy
beyond melee reach gets the engine's own attack order on that enemy (the double-click order), so it runs in.
Nearest seen enemy wins; settings in `[Charge]`. No mission triggers involved. One order per enemy: a new attack order
restarts the running one (the unit stops and re-paths), so nothing is re-issued while the unit is still closing on the enemy
it was sent at; only a stalled unit, a dead enemy or a much closer one gets a new order.
Beyond `CloseDistance` the charge is the player's double-click move (an `eMovementChange` mode order, then `Order::eMoveTask`;
the single-click move is `Order::eAdvance`, the slow weapon-up walk that forces its own mode 1); inside it the `attack_melee` order (`Order::eFight`, game exe+0x8E7550). A plain attack order on an enemy out of
weapon reach does not make a stance unit approach.

**Always-on / from triggers:** the tag `melee_always` (ini `AlwaysTag`) counts as stance but the button never
removes it. Put it in a breed's `{tags}` for permanent berserkers, or add/remove it from a mission trigger
(`{"entity_state" {selector â€¦} {tag_add melee_always}}`). A trigger can also add/remove `melee_stance` itself
(then the button can switch it back). Charge range = `[Charge] MaxDistance` (2000 units = 100 m); no charge
orders closer than `MinDistance` (60 units), where the unit is already fighting.

**Multi-select instant buttons:** vanilla runs an `{instant}` action button only when exactly one selected unit can
use it (`GetActor`, game exe+0xB73E90), so with a squad selected the button just sits pending. For actions with
`{apply_to_all}` the dll runs the vanilla `ExecuteInstant` (exe+0xB5E260, patched in the 9 action vtables) once per
eligible selected unit, each time as if only that unit were selected. `[MultiSelect] Enabled`.

**Melee through shields:** before a paired blow starts, the engine casts a ray attacker -> target and drops the
blow unless the first hit is the target (game exe+0x95E5C2). A forcefield is its own entity linked to the wearer,
so it was always hit first. `[ShieldMelee]` skips hits on entities linked to the target or tagged `forcefield`;
walls and other units still block.

**Blow reach:** `[BlowReach]` overwrites the engine's paired-melee start distances (vanilla `set\blow` fists/kick
= near 17, rifle butt/knife = medium 29, bayonet/sword = far 35 world units, centre to centre) with 45 / 45 / 45 (ini),
for every paired fight in the game. Table found by its reader `GetBlowDistance` (game exe+0x95EC10, table +0x10EDC80).

## 0.7.0 (Nov 7 2026, offline-tested only; staged in `out\v0.7\`, 0.6.10 stays installed)
**Auto-draw `[AutoDraw]`:** a human that carries a melee weapon and has enemy infantry within `Distance` (100 units = the 5 m
`fight/distance`) is held in a temporary stance for `HoldMs` (3000) after the last such sighting: guns refused, one take-item
order for `{item "melee"}` (a TagContainer built with the engine's ctor + FromString, found through the `{tags}` action).
When nobody is that close the hold ends and the gun comes back. A unit whose sword came out this way only charges within 2 x Distance.
**Vehicles `[Vehicles]`:** stance units also melee vehicles. Hand-melee ammo (`VehicleMeleeTags` melee_vs_human/melee_vs_all)
against a non-human actor is refused for every non-stance unit, and for stance units while they saw enemy infantry within
`[Charge] MaxDistance` in the last `InfantryMs` (infantry first). Charge: infantry always beats a queued/current vehicle target;
vehicles get the eMoveTask run, then the attack order (eAttack; the engine's hand fight only works on humans). **Needs ammo.set rows**
for those tags in the vehicle target classes (not added yet).
**Explicit orders:** the unit's current order = front of its brain queue (brain = entity+0xDC8; `[brain+0x39C] & 0x1000` = queue
active: deque map +0x70, size +0x78, offset +0x80, count +0x88; else override order brain+0xF0; engine getter game exe+0x9056C0).
AI battle fights don't use that queue. An eAttack / eAttackBasic / eFight there that the dll did not add (target ref at order+0x40,
entity +0x58) and that targets a vehicle = the player's order: melee allowed, infantry-first skipped, no charge retarget.
Order vtables found by RTTI (game 0x10E8F78 / 0x10E8E18 / 0x10E88F8).

## 0.7.1 (Nov 7 2026, offline test ALL OK on game/editor/server; built into Alex's local launcher payload)
**Stall fix:** the 0.7.0 editor log showed stance marines standing 7-17 s after `IN HAND` while the enemy walked up: the draw
(or the wait for one) sat under the same cooldown + progress test as a real charge order, and that test compared distances, so
the ENEMY approaching read as the unit closing in. Now a unit whose last order was the draw gets its run / melee order the moment
the melee weapon is in hand, and the stall test uses the unit's own position (moved less than `ProgressUnits` itself and the
distance closed by less, two one-second samples in a row, after `CooldownMs`, default now 2000) before it re-orders.
**Reload cancel `[ReloadCancel]`:** `Order::eTakeItem::Update` (game exe+0x8EA920) keeps waiting while `Human::InventoryBusy`
(exe+0x7CCEB0: the human's inventory op list `[ops+0x80..0x88]` is not empty), and a running reload is such an op (the magazine
transfer, `eInventoryTransferCommand`), so a melee draw given mid-reload stood until the reload ended. Before every draw (toggle,
charge, auto-draw) the dll calls `Human::CancelInventoryOps` (exe+0xB67C30: brain event 0x707, then `Inventory::CancelAll`
exe+0x7CABB0 on `Human::InvOps` exe+0x7CCD50; the host sends `eInventoryTransferCancelCommand`) unless the weapon in hand
(`GetCurrentWeapon` exe+0x806340 on the holder at human+0xDD8, params +0x98, tags +0xEC8) carries one of `KeepTags` (heavy weapons:
`mgun,hmg_shell,stubber,lascannon,plasmacannon,sm_weapon_plasmacannon,sm_weapon_heavybolter,rocketlauncher,heavy_flamethrower,grenadelauncher,at_rifle,ogryn_weapon`).
Finders: three function-start signatures with the rip-relative displacements wildcarded (editor 0x64D6C0 / 0xA96BB0 / 0x6889B0,
server 0x5E32D0 / 0x93B2D0 / 0x61B910).
**Vehicles:** a unit the dll itself sent at a vehicle now stays committed: its own attack order counts like a player order in the
weapon choice (infantry-first no longer pulls the melee weapon away while it stands beside the tank) and in the charge (other
infantry only takes over when closer than `SwitchRatio` x the vehicle's distance; 0.7.0 dropped a dll vehicle target for any
infantry seen within `MaxDistance`). The close order on a vehicle is the double-click attack: `Action::e_attack` (game exe+0xB712E0)
passes bit 2 of the click byte (global 0x15A199C, set from the action's begin parameter) to `MakeAttack`, stored at order+0xC1;
`Order::eAttack::Update` (exe+0x8EDFD0) reads it only for non-human actors (pushes an `eLookAt` child, exe+0x8D68E0) - for humans a
single and a double right-click attack are the same order. Settings `[ReloadCancel] Enabled / KeepTags`.

## 0.7.2 (Nov 7 2026, Alex: units kept getting move orders around the tank)
An attack order on a vehicle (the dll's own or the player's) is left alone until the engine ends it (vehicle dead / gone): no
stall re-order, no run refresh. A unit beside a hull is never "in reach" of the vehicle's centre, so in 0.7.1 every stall
sample re-issued the attack and the unit re-pathed around the tank. When the order ends the normal rules pick the next target.

## 0.7.3 (Nov 7 2026, Alex: units get stuck against vehicles "trying to move inside them", and ignore cannons)
**Hull contact `[Vehicles] AdjacentMs / AdjacentExtra`:** the engine's range test runs unit -> target point, and for a vehicle that
point is the body volume's CENTRE (ammo.set `{volume "body"}` picks the volume, not its surface). A 3.3 m sword (66 units) can
never be "in range" from the front of a tank (0.7.2 log: stalled at 88 units), so the attack order keeps pushing the unit into the
hull and the weapon never fires. The dll has no cheap way to the hull surface, so it uses the physical fact: a unit under a vehicle
attack order that stands still (moved less than `ProgressUnits` in a second) while still beyond reach but within reach +
`AdjacentExtra` (200 units) of the centre is "pressed against the hull". For `AdjacentMs` (1500, re-armed every second it keeps
standing there) the stance filter hands the engine's match a target point at 0.8 x reach in front of the unit (restored right
after), so the weapon fires and the swing lands on the plate it touches. Log: `vehicles: ... pressed against ... range measured
from the hull`.
**Cannons:** cannons are actors like vehicles (entity flag +0x30 bit 1 is set in the eActor ctor, bit 2 = human), so they go
through the vehicle path - where their own crew counted as "infantry seen" and blocked the melee for as long as the crew lived.
Linked humans (crews, passengers) no longer count as infantry seen and are never charged (they can't be fought while linked).
## 0.7.4 (Nov 7 2026, Alex: "stop when they hit the ram volume, then swing at the ram volume")
**`[Vehicles] ContactTag`** (`hull_contact`): the mod's human `{on contact}` (40k_human.inc) already receives the touch of a
vehicle's `"ram"` volume (vanilla skips it with `{if not volume "ram"}`); a unit tagged `has_melee_weapon` (melee weapon in hand)
now gets `hull_contact` for 1.5 s when it touches one. The dll takes the tag as hull contact at once for the vehicle the unit is
attacking (or any vehicle within 2 x reach): the range shift of 0.7.3 starts on the touch instead of after the one-second stall
sample, which stays as the fallback. Log: `vehicles: ... touches ... (hull_contact tag)`.

## 0.7.5 (Nov 7 2026, from the 0.7.4 editor log; Alex: "it worked pretty well")
The hull contact worked through the stall fallback (`pressed against` at 46-91 units from the centre, five units on one tank), but
`reach 0`: the engine's `Range()` of the matched ammo params is 0 for melee ammo (the range sits on the weapon set, the ammo has
none), so the shifted point collapsed onto the unit itself - it worked by accident. Now deliberate: the point is `ADJ_AHEAD` = 10
units in front of the unit (it is touching the plate), and "pressed" = stalled closer than `[Vehicles] AdjacentMax` (300, a
Baneblade's half-length) to the centre; `AdjacentExtra` is gone. The `hull_contact` tag path (ram-volume touch) never fired in that
test, so the stall sample is doing the work; the tag stays as an accelerator. Reload cancel: `InventoryBusy` was true for all 8
toggled units in the same millisecond, so the op list holds more than reloads; the cancel now also needs the held weapon's own
reload record (weapon+0xB0, +0x10 = reload start clock, the field `eWeaponPreload::Update` and `StopLongReload` test) to be
running, else the inventory is left alone (log: `inventory busy but the held weapon is not reloading`).
## 0.7.6 (Nov 7 2026, Alex: guardsmen and cannon slaughtered, the Russ died fast but they "ran around it a lot")
0.7.5 re-armed the hull contact only from a fresh one-second stall, so every shove by the engine's approach (the unit moved 10
units) dropped the range again and the unit went back to pathing around the tank. Now a unit stays armed for as long as its attack
order on that vehicle runs and it is within 2 x `AdjacentMax` of the centre. `AdjacentMax` default 120 (was 300): a Russ from any
side (0.7.5 log: 48-96 units), a Baneblade's side; a unit queued behind a friend at 155 does not arm and swing at air. Without an
attack order the ram-touch tag only arms within `AdjacentMax` / 2.

## 0.7.7 (Nov 8 2026, Alex: "some problems with changing weapons in cqc" + video: a marine stands point-blank with his bolter for 14 s)
The 0.7.5 reload gate was wrong: a reloading unit was logged `inventory busy but the held weapon is not reloading: left alone` four
times in 10 s and never drew (the gate tested only the record's start clock; `eWeaponPreload::Update` also waits on its +0x14 flag
bits). The cancel is back to "whenever the inventory is busy" (0.7.4, which swapped fine); the weapon record is only logged
(`weapon reload record: flags 0x.. start ..`) so the next log shows what a real reload looks like.

## 0.7.8 (Nov 8 2026, Alex: "killed everyone but then stood really far from the tank watching it before one guy decided to run in")
Log: after the infantry died one unit got `attack-vehicle` at 198; another ran at the tank, `stalled at 239` from its centre (phase
1 = run) and was sent running again to the same spot; three units never got a vehicle charge. Two causes: (1) the run goal is the
hull centre, which the pather cannot reach - a Russ stops the unit ~240 out, beyond `[Charge] CloseDistance` 200, so the run never
handed over to the attack order; (2) infantry-first counted any live enemy infantry within `[Charge] MaxDistance` (100 m).
Now `[Vehicles] CloseDistance` (400) is the hand-over distance for vehicles and a run that stalls short of a vehicle goes straight
to the attack order; `[Vehicles] InfantryRange` (400 = 20 m) bounds infantry-first; each infantry-first refusal is logged
(`vehicles: ... refused: enemy infantry within 400 seen N ms ago`, 20 lines); the charge / held / hull log caps went 40 -> 200
because Alex runs several simulations per editor session (the 0.7.7 log hit the cap after the first).

## 0.7.9 (Nov 8 2026, Alex: "the move order cycle is a bit slow, they don't react to changes in the enemy position very quickly")
A running charge at the same enemy was re-aimed (run goal moved, or close enough for the melee / attack order) only after
`RetargetMs` 1500 like every other re-order. Now that re-aim has its own gate `[Charge] RefreshMs` (400) and `MoveRefreshUnits`
is 40 (2 m, was 80). RetargetMs still gates enemy switches and stall re-orders. Assault-vs-tactical swap test read from the 0.7.8
log: every PUT AWAY -> IN HAND in 0.3-0.9 s; the weapon record's +0x10 reads ~270 ms in the future right after a shot, so it is a
"ready at" clock (recovery), not a reload start.

## 0.7.10 (Nov 8 2026, Alex: "all three got stuck staring at the cannon for like 5 s")
Log: last infantry melee order at 49.6 s, first `attack-vehicle` on the cannon at 53.8 s, 20 refusals "infantry seen 5 ms ago"
in between. `[Vehicles] InfantryMs` 3000 held the vehicle melee for 3 s after the last infantry sighting, then the 1.5 s retarget
gate. The memory only has to bridge the gap between two scans, so the default is 500 now.

## 0.7.11 (Nov 8 2026, Alex: "when players are in direct control I don't want the melee behaviour to be active, same with the CQC weapon pull out")
A unit under direct (manual) control is left alone: no stance ammo filter, no charge order, no auto-draw, no reload cancel, until
the player releases it (`[MeleeStance] SkipDirectControl`, default 1). The test is the engine's own `eActor::IsManualControlled`
(game exe+0x7EF8A0, editor 0x671970, server 0x605E10; found by signature, logged at install): the manual-mode property at
actor+0xDE0 (+0x18 != 0) or bit 1 of [[actor+0xDD8]+0x70] - the same test the `reset_manual_control` script action and the
manual-control manager use. Not `{if user_control}`: that is brain+0x398 & 3 == 1 = "belongs to a human player" (PlayerUnits).
A charge or auto-draw already queued when the player takes the unit over is dropped at flush time. Editor-tested OK (Alex, Nov 8 2026).

## 0.7.12 (Nov 8 2026, Alex: in direct control "it takes my weapon away randomly in CQC" with 0.7.11)
Second direct-control test: the manual-control manager itself (`eManualControl` global, game exe+0x159E218 / editor 0x140A930,
by signature on its `IsActive` / `GetActor` call pair; the server exe has none and logs it). The actor it drives sits at +0x3B0,
so the unit the player is in right now counts as manual whether or not the per-actor property (0.7.11's
`IsManualControlled`) says so. Every skip is logged (`manual: [ms] unit X under direct control (property P, manager M)`, per unit
every 3 s, 40 lines), so the log shows what the dll did or did not do to the driven unit.

## 0.7.13 (Nov 8 2026, Alex: "units aren't charging in melee mode")
His game log: a unit ran at an enemy that was itself charging about; the enemy moved 40+ units every RefreshMs 400, so the run was
re-issued every 440 ms, every re-issued move restarts the run (stop, re-path), and the unit hung 400-1000 units off for 90 s. The
run is now re-aimed when the enemy moved `MoveRefreshUnits` (40) OR `[Charge] MoveRefreshFraction` (0.4) of the current distance,
whichever is larger (500 units off: 200, about 2 s of a running enemy; inside CloseDistance the attack order as before). Log line
caps (charge 200, held 200, 40 for the rest) restart on every mission (sim clock going backwards): the first mission of that run had
used them all and the melee-mode test after it left no lines.

## Files
- `gem_meleestance.dll` - plain dll, hooks in DllMain (inject before the engine runs, or into a running game for tests)
- `gem_meleestance.ini` - settings (next to the dll)
- `gem_inject.exe` + `inject.bat` - in-house test: injects the dll into a running game/editor

## Settings
| key | default | meaning |
|---|---|---|
| Enabled | 1 | install the hook |
| Tag | melee_stance | script tag that turns the stance on |
| MeleeAmmoTags | melee_vs_human, melee_vs_all, medium_inf_melee_target, medium_veh_melee_target, heavy_veh_melee_target | ammo `{tag}`s that count as melee |
| SkipLinked | 1 | crews, gunners and passengers ignore the stance (a tagged man on a vehicle gun still fires it) |
| DrawOnToggle / DrawButton / DrawCooldownMs | 1 / melee_toggle / 3000 | toggling the stance on gives the unit the engine take-item order for the button's `{item}`; one order per unit per cooldown |
| DrawFromBelt | 1 | ammo.set melee rows marked `{ready_for_shot}` also allow drawing the weapon from the belt |
| Log | 1 | `gem_meleestance.log`: install line + first 40 stance decisions |

| [Charge] Enabled | 1 | melee units charge seen enemies |
| [Charge] Tag | has_melee_weapon | tag a unit needs to charge (stance units always qualify) |
| [Charge] MeleeInHand | 1 | only while the melee weapon is in hand |
| [Charge] PlayerUnits | 1 | 0 = AI-controlled units only |
| [Charge] CooldownMs / RetargetMs | 4000 / 1500 | order spacing per unit (same enemy / any enemy) |
| [Charge] CloseDistance / MoveRefreshUnits / RunMode | 200 / 80 / 2 | farther than CloseDistance: eMovementChange(RunMode) + eMoveTask (the double-click run) to the enemy (re-issued when it moved MoveRefreshUnits); inside it the attack_melee order (eFight) |
| [Charge] SwitchRatio / ProgressUnits | 0.6 / 10 | one order per enemy: no re-order while the unit is still closing on it; another enemy only when closer than SwitchRatio x the current one |
| AlwaysTag | melee_always | second stance tag for breeds / triggers, never removed by the button |
| [Charge] MaxDistance | 2000 | farthest enemy in world units (20 = 1 m), 0 = whatever the unit sees |
| [Charge] MinDistance | 60 | no new order closer than this (unit is already fighting) |

## Engine hook
`MatchAmmo(ctx)` (game exe+0x914180, editor +0x79AFB0, server +0x729370; found by pattern) is called once per
candidate ammo by every weapon choice path. The dll moves its first 14 bytes to a trampoline:
tagged + non-melee ammo -> no match; tagged + melee ammo -> original with the "in hand" flag set.
Charge: only calls from the engine's enemy scan (game exe+0x9580BA) count. A melee match against an enemy
beyond `Range(ammo)` queues `MakeAttack(targetRef)` (exe+0x8EA230) and `eActor::AddOrder` (exe+0x7EFA10) with the
UI's parameters; the order is issued on a later AI tick from another unit's scan, never inside the scan that found it.
Unit and target are held as engine target refs, so a deleted entity drops the order.

## Multiplayer
Weapon choice runs on every machine, so every player needs the dll (launcher-forced, like the other GEM dlls).

## Build
`build.bat` (zig cc: `pip install ziglang`), `build.bat test` = offline finder + filter test on the three exes.
