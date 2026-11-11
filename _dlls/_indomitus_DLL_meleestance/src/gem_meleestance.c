/* GEM MeleeStance 0.2 - Dawn of War style melee toggle + melee charge
 * Call to Arms: Gates of Hell (GEM 2, 1.067)
 *
 * 1. Stance. While a human carries the script tag <Tag> (default "melee_stance"), the engine's ammo choice
 *    (set\target\ammo.set) only accepts ammo whose {tag} list holds one of <MeleeAmmoTags>, so the unit
 *    draws its melee weapon and keeps using it; guns, pistols and grenades are refused. Untagged = vanilla.
 *    The {ready_for_shot} limit is lifted for melee ammo of a tagged unit, so ammo.set rows that only allow
 *    melee "when already in hand" also let a tagged unit draw it from the belt.
 * 2. Charge. A human tagged <ChargeTag> (default "has_melee_weapon") whose melee weapon is in hand (or who is
 *    in stance) and who sees an enemy beyond melee reach gets the same attack order a double-click gives
 *    (Order::eAttack on that enemy), so he runs in and fights. Nearest enemy wins; one order per
 *    <ChargeCooldownMs> per unit unless the target changes.
 * A human linked to something (vehicle crew / gunner, passenger) is left alone by both.
 *
 * Engine (game exe offsets, found by pattern at runtime, same code in editor/server):
 *   exe+0x914180 MatchAmmo(ctx) -> ammo.set row or NULL, called per candidate ammo by every weapon
 *   choice site. ctx (built by exe+0x9140A0): +0 ammo eStuff*, +8 shooter entity, +0x10 target info
 *   (+0x18 target entity, +0x28 float3 position), +0x18 byte "item in hand", +0x19 charging.
 *   The enemy scan exe+0x9580BA (the only function that calls MatchAmmo twice) calls it for every seen
 *   enemy BEFORE its own range check; only calls from there feed the charge (the UI also calls MatchAmmo,
 *   locally, which must not change the simulation in MP).
 *   Range(ammo stuff) exe+0x7E0AC0 -> float, compared with |shooter+0x74 - target pos| in MatchAmmo.
 *   Attack order (as the UI's attack click, exe+0xB71374):
 *     ref[0x80]: vtable, +8 type (1 entity), +0xC pos, +0x18 entity, +0x20 Tag (volume)
 *       built as MakeRef exe+0xB74420 does: vt, TagCtor(ref+0x20) exe+0x556220, RefInit(ref) exe+0xB8D30,
 *       SetEntity(ref, ent, &tag) exe+0x83DFA0 (registers the ref on the entity); freed by RefDtor exe+0x83E4F0
 *       + TagDtor(ref+0x20) exe+0x5564E0.
 *       RefDtor unlinks the ref from the entity's back-ref list (entity+0x198 in the 1.067 exes, read from RefDtor's
 *       "add rcx,imm32": a vector of 16-byte entries at list+8 / list+0x10, entry+8 = the ref) without any check.
 *     order = MakeAttack(&ref, 0) exe+0x8EA230 (new 0x108-byte Order::eAttack, copies the ref)
 *     eActor::AddOrder(actor, order, {int 1, uint [exe+0x1556DE8]}) exe+0x7EFA10 (frees the order on refusal)
 *   ammo {tag} Tags object at stuff+0xEC8 (read from the code), its tag vector at Tags+0x318.
 *   name Tags at stuff+0x868. Entity script tags vector at entity+0x4E8. entity byte +0x30: 2 actor, 4 human.
 *   entity dword +0x1C0 bit 28 = linked (engine: mov eax,[r+1C0h]; shr eax,1Ch; test al,1).
 *   brain = entity+0xDC8, [brain+0x398]&3 == 1 = player-controlled (MatchAmmo's enable_for_user test).
 * The MatchAmmo prologue (14 bytes: mov [rsp+20h],rbx; push rbp/rsi/rdi/r12/r13/r14) is moved to a
 * trampoline and replaced by an absolute jump to ms_hook().
 * Charge orders are queued during the scan and issued on a later AI tick (from the next scan of another
 * unit), never inside the scan that found the enemy, so the scanned unit's inventory loop is not disturbed.
 *
 * Build: build.bat      Offline finder + filter test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define MS_VERSION "0.7.35"
#define MAXT 24
#define TLEN 48

/* ---------------------------------------------------------------- config */
static int  cfg_enabled = 1, cfg_log = 1, cfg_skip_linked = 1, cfg_lift_ready = 1;
static char cfg_tag[TLEN] = "melee_stance", cfg_atag[TLEN] = "melee_always";
static char cfg_mtags[MAXT][TLEN] = { "melee_light", "melee_heavy", "medium_inf_melee_target",
                                      "medium_veh_melee_target", "heavy_veh_melee_target" };
static int  n_mtags = 5;
static int  cfg_charge = 1, cfg_charge_inhand = 1, cfg_charge_player = 1;
static char cfg_ctag[TLEN] = "has_melee_weapon";
static uint32_t cfg_charge_cd = 4000, cfg_retarget = 1500;
static uint32_t cfg_refresh_ms = 400;    /* 0.7.9 [Charge] RefreshMs: a running charge at the SAME enemy may be re-aimed (run goal moved, or close enough for the
                                          * attack / melee order) this soon after the last order; RetargetMs still gates every other re-order (Alex: "the move
                                          * order cycle is a bit slow, they don't react to changes in the enemy position very quickly" - both sat behind 1500) */
static float cfg_charge_max = 2000.0f;    /* world units (20 = 1 m), 0 = any enemy the scan sees */
static float cfg_charge_min = 60.0f;      /* world units: closer than this = already fighting, no new order */
static int   cfg_down_on = 1;             /* [Downed]: no weapon is chosen against a target carrying the downed tag */
static char  cfg_downtag[1][TLEN] = { "start_personage" };   /* 40k_human.inc start_personage / stop_personage */
static uint32_t st_down;
static int   cfg_blow_on = 1;             /* [BlowReach]: engine paired-melee start distance per blow class */
static float cfg_blow[3] = { 45.0f, 45.0f, 45.0f };   /* near / medium / far, world units (vanilla 17 / 29 / 35) */
static uint32_t cfg_enttags = 0x4E8;      /* entity script tag vector */
static uint32_t cfg_ammotags = 0xEC8;     /* stuff {tag} Tags object; replaced by the offset in the engine code */
static uint32_t cfg_namtags = 0x868;      /* stuff name Tags object */
#define TAGS_VEC 0x318                    /* vector inside a Tags object (0x868->0xB80, 0xEC8->0x11E0, 0x1D0->0x4E8) */
static uint32_t cfg_linkoff = 0x1C0, cfg_linkbit = 28;

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines, g_clines, g_hlines;
static void log_epoch(uint32_t now);     /* 0.7.13: the line caps are per mission (sim clock went backwards = new mission) */
static void logf_(const char *fmt, ...)
{
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = fopen(g_logpath, "a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------------------------------------------------------- tags */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* MSVC std::string at p: buf/ptr +0, size +0x10, cap +0x18 */
static const char *str_get(const uint8_t *p, size_t *n)
{
    uint64_t size = *(const uint64_t *)(p + 0x10);
    uint64_t cap  = *(const uint64_t *)(p + 0x18);
    if (cap < size || cap > 0x10000 || size == 0) return NULL;
    *n = (size_t)size;
    return cap > 15 ? *(const char *const *)p : (const char *)p;
}

static int eq_ci(const char *s, size_t n, const char *z)
{
    size_t m = strlen(z);
    if (n != m) return 0;
    for (size_t i = 0; i < n; i++) if (lc(s[i]) != lc(z[i])) return 0;
    return 1;
}

/* tag vector at v (begin,end of 0x60-byte Tags, std::string at +0x40):
 * returns 1 if any tag equals one of list[0..n), 0 if none, -1 on a broken vector */
static int vec_has(const uint8_t *v, char list[][TLEN], int n)
{
    const uint8_t *b = *(const uint8_t *const *)v;
    const uint8_t *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e) return 0;
    if (e < b) return -1;
    uintptr_t len = (uintptr_t)(e - b);
    if (len % 0x60 || len > 0x60 * 1024) return -1;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        for (int i = 0; i < n; i++) if (eq_ci(s, k, list[i])) return 1;
    }
    return 0;
}

/* "name1 name2 ..." of a tag vector, for the log */
static void vec_str(const uint8_t *v, char *out, size_t cap)
{
    out[0] = 0;
    const uint8_t *b = *(const uint8_t *const *)v, *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e || e < b || (uintptr_t)(e - b) % 0x60 || (uintptr_t)(e - b) > 0x60 * 64) return;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        size_t l = strlen(out);
        if (l + k + 2 >= cap) break;
        if (l) out[l++] = ' ';
        memcpy(out + l, s, k); out[l + k] = 0;
    }
}

/* ---------------------------------------------------------------- engine calls */
typedef void *(*match_t)(uint8_t *ctx);
typedef void  (*vfn1_t)(void *);
typedef void  (*setent_t)(void *ref, void *ent, void *tag);
typedef void *(*mkatk_t)(void *ref, uint8_t flag);
typedef void *(*mkmove_t)(const float *xy, int flag);   /* Order::eMove to a ground point (game exe+0x8F3600) */
typedef void *(*addord_t)(void *actor, void *order, void *params);
typedef float (*range_t)(void *stuff);
/* 0.6.9: the player's own orders. Order::eFight (game exe+0x8E7550, (entity *target) -> order) is what the attack_melee
 * action issues (Action::e_attack_melee slot 17, game exe+0xB71F40). The UI's click move is the 0xF8-byte order from game
 * exe+0x8D2B20 ((float *xy, byte kind=1) -> order, then word order+0x18 = 4, as Action::e_move slot 17 game exe+0xB65F50
 * does), preceded by Order::eMovementChange(pose, mode, wait) (game exe+0x8F3930; mode table {0,1,1,2,3,0,1,2}: 2 = run,
 * the human battle state's mode, 3 = the fastest) for the sprint. */
typedef void *(*fight_t)(void *target);
typedef void *(*mvchange_t)(int pose, int mode, uint8_t wait);
typedef void *(*uimove_t)(const float *xy, uint8_t kind);
static match_t  g_orig;
static fight_t    g_fight;
static mvchange_t g_mvchange;
static uimove_t   g_uimove;
static int cfg_runmode = 2;              /* [Charge] RunMode: eMovementChange mode queued before the far move order (2 = run, Guga-tested), -1 = none */
static vfn1_t   g_tagctor, g_tagdtor, g_refinit, g_refdtor;
static setent_t g_setent;
static mkatk_t  g_mkatk;
static mkmove_t g_mkmove;
static addord_t g_addorder;
static range_t  g_range;
static void    *g_refvt;
static volatile uint32_t *g_ordstamp;
static uint8_t *g_scan_lo, *g_scan_hi;     /* the enemy scan (return-address window) */

/* ---------------------------------------------------------------- stance filter */
static char g_tagl[2][TLEN], g_ctagl[1][TLEN];
static int  n_tagl = 1;
static uint32_t st_reject, st_allow, st_charge_q, st_charge_go, st_charge_drop;
/* 0.7.21 heartbeat (Alex's 0.7.20 session: hook in place, checked in the live process, yet the dll tracked no unit and logged
 * nothing from a fight): how often the engine calls the hook at all. A background thread only reads these and writes the log
 * (no engine calls) every 5 s while they change, so a silent log now says whether the engine asked anything. */
static volatile LONG st_hk_all, st_hk_scan, st_hk_human, st_hk_enemy, st_hk_polls;
static volatile LONG st_fast;              /* 0.7.35: picks passed straight to the engine (fast_nomelee) */

enum { PASS = 0, REJECT = 1, MELEE = 2 };

/* units whose weapon choice offered melee ammo recently (any scan lists every carried item, belt included);
 * the stance is only enforced on them, so a toggled unit without a melee weapon keeps shooting */
typedef struct { const void *unit; uint32_t ms; } seen_t;
#define NSEEN 512
#define SEEN_MS 20000
static seen_t g_seen[NSEEN];
static volatile uint32_t *g_clock;
static uint32_t now_ms(void) { return g_clock ? *g_clock : 0; }
static void melee_seen(const void *u)
{
    uint32_t h = (uint32_t)(((uintptr_t)u >> 4) * 2654435761u) % NSEEN, now = now_ms();
    seen_t *slot = &g_seen[h];
    for (int i = 0; i < 16; i++) {
        seen_t *s = &g_seen[(h + i) % NSEEN];
        if (s->unit == u || !s->unit || (uint32_t)(now - s->ms) > SEEN_MS) { slot = s; break; }
    }
    slot->unit = u; slot->ms = now;
}
static int g_seenlines, g_addiag;   /* 0.7.15 auto-draw diagnostic line caps (per mission) */
static int carries_melee(const void *u)
{
    if (!g_clock) return 1;                                  /* no clock found: old behaviour (always enforce) */
    uint32_t h = (uint32_t)(((uintptr_t)u >> 4) * 2654435761u) % NSEEN, now = now_ms();
    for (int i = 0; i < 16; i++) {
        seen_t *s = &g_seen[(h + i) % NSEEN];
        if (s->unit == u) return (uint32_t)(now - s->ms) <= SEEN_MS;
    }
    return 0;
}

static int is_linked(const uint8_t *ent) { return cfg_skip_linked && ((*(const uint32_t *)(ent + cfg_linkoff) >> cfg_linkbit) & 1); }

/* ---------------------------------------------------------------- 0.7: auto-draw + vehicle targets, per-unit state */
/* [AutoDraw] a human that carries a melee weapon and has an enemy infantry unit within Distance (default 100 world units
 * = the 5 m `fight/distance` of 40k_human.inc) is held in a temporary stance for HoldMs after the last such sighting:
 * guns are refused, the weapon choice draws the melee weapon, and the dll gives one take-item order ({item "melee"}),
 * so a unit does not stand there holding a rifle while it is being cut down. When nobody is close any more the hold runs
 * out and the engine picks its gun again.
 * [Vehicles] stance units (button or always tag) may also melee vehicles: hand-melee ammo (VehicleMeleeTags) against a
 * non-human actor is allowed for them only (ammo.set needs rows for those tags in the vehicle classes), and refused
 * while they have seen enemy infantry within [Charge] MaxDistance in the last InfantryMs, so infantry comes first.
 * An attack / melee order on a vehicle at the front of the unit's order queue that the dll did not add (the player's
 * click) overrides that, and the charge leaves that unit on its vehicle. */
static int   cfg_ad_on = 1, cfg_ad_player = 1;
static float cfg_ad_dist = 100.0f;
static uint32_t cfg_ad_hold = 3000;
/* 0.7.16 probe (Alex's Nov 8 dump: the engine never offered a belt melee weapon while a gun was in hand - the ammo.set gun rows
 * win before the melee rows are reached - so carries_melee never became true and the auto-draw never armed): an enemy inside
 * Distance and no melee seen on the unit -> its gun ammo is refused for one weapon pick (ProbeMs) so the engine falls through to
 * the melee rows. Melee offered = armed (normal auto-draw); only guns offered = no melee weapon, guns kept for NoMeleeMs. */
static int cfg_probe_on = 1;
static int cfg_probe_any = 1;      /* 0.7.18 [AutoDraw] ProbeAnyRange: probe on the first scanned enemy at any range (the order poll needs the answer before fight range) */
static uint32_t cfg_probe_ms = 300, cfg_probe_cd = 4000, cfg_nomelee_ms = 20000;   /* 0.7.25: 20 s (Alex) */
static uint32_t st_probe, st_probe_ok, st_probe_fail, st_probe_rej;
static int g_plines;
/* 0.7.16 fight order (Alex: "when units are in fight range they should be forced to their melee weapon and use the fight order"):
 * inside [AutoDraw] Distance a melee unit gets the attack_melee order (eFight) on the enemy, even in reach (before: no order in
 * reach / under MinDistance, the engine's own blows = punches with the gun in hand). Once per enemy while eFight runs on it. */
static int cfg_fight_on = 1;
static uint32_t cfg_fight_cd = 1500;
static uint32_t st_fight_q;
static int   cfg_veh_on = 1;
static char  cfg_vtags[MAXT][TLEN] = { "melee_light", "melee_heavy" };
static int   n_vtags = 2;
static uint32_t cfg_inf_ms = 500;      /* 0.7.10: was 3000 - the three marines stared at a cannon for ~5 s after the last guardsman died (Alex); the memory only bridges scan gaps */
static float    cfg_inf_range = 400.0f;  /* 0.7.8 [Vehicles] InfantryRange: enemy infantry farther than this (units) does not hold a stance unit off a vehicle
                                          * (was [Charge] MaxDistance 2000: one live guardsman 90 m away kept whole squads staring at a tank) */
static float    cfg_close_veh = 400.0f;  /* 0.7.8 [Vehicles] CloseDistance: the run at a vehicle hands over to the attack order here (was [Charge] CloseDistance
                                          * 200: the run goal is the hull CENTRE, the pather stops at the tank's obstacle edge, ~240 for a Russ, and the
                                          * unit stood there "watching the tank"; a stalled run at a vehicle also goes straight to the attack order) */
static int g_vreflines;
static int   cfg_veh_attack = 1;     /* [Vehicles] CloseOrder: 1 = the close part of a vehicle charge is the attack order (eAttack), 0 = eFight */
/* 0.7.3 hull contact (Alex: units pressed into tanks, never swinging; "target the body volume instead of the centre"): the engine's
 * range test runs unit -> target point (the body volume's centre), and a 3.3 m sword can't get within 3.3 m of a hull centre from
 * the front of a tank (log: stalled at 88 units). A unit under a vehicle attack order that stops moving while still out of reach is
 * "pressed against the hull": for AdjacentMs the stance filter moves the target point to reach x 0.8 in front of the unit before
 * the engine's match, so the weapon fires and the swing lands on the plate it is touching. */
/* 0.7.5: the engine's Range() of the matched ammo params is 0 for melee ammo (the range sits on the weapon set, the ammo has none;
 * 0.7.4 log "reach 0"), so nothing here depends on it any more: the shifted target point is ADJ_AHEAD units in front of the unit
 * (it is touching the hull, any point between it and the centre is on the plate), and "pressed" = stalled within AdjacentMax
 * units of the centre (the vehicle's own half-length; 300 covers a Baneblade's 10 m). */
#define ADJ_AHEAD 10.0f
static uint32_t cfg_adj_ms = 1500;    /* [Vehicles] AdjacentMs */
static float    cfg_adj_extra = 120.0f; /* [Vehicles] AdjacentMax: stalled closer than this (units) to the vehicle centre = pressed against its hull.
                                         * 0.7.6: 120 (was 300): a Russ from any side (0.7.5 log: 48-96), a Baneblade's side; a unit queued behind a
                                         * friend at 155 must not arm and swing at air. Once armed the unit stays armed while its attack order on
                                         * that vehicle runs and it is within 2 x AdjacentMax (0.7.5 re-armed only from a fresh stall, so each shove
                                         * by the engine's approach dropped the range again and the unit ran around the tank, Alex). */
static uint32_t st_adj, st_adj_shift; static int g_adjlines;
static char cfg_contacttag[1][TLEN] = { "hull_contact" };   /* 0.7.4 [Vehicles] ContactTag: set by 40k_human.inc {on contact} when a unit with its melee
                                                            * weapon out touches a vehicle's "ram" volume (Alex: stop at the ram volume, swing at it) */
static int n_contacttag = 1;
static int linked_raw(const uint8_t *e) { return (*(const uint32_t *)(e + cfg_linkoff) >> cfg_linkbit) & 1; }
static uint32_t st_ad_hold, st_ad_draw, st_veh_reject, st_veh_pref;

typedef struct {
    const void *unit;
    uint32_t ad_until;        /* auto-draw hold ends (clock ms), 0 = none */
    uint32_t inf_ms;          /* last scan that saw enemy infantry within [Charge] MaxDistance */
    uint32_t ad_draw_ms;      /* last auto take-item order */
    int auto_drawn;           /* the melee weapon came out through the auto-draw (limits the charge) */
    uint32_t manual_ms;       /* 0.7.12: last "under direct control" log line for this unit */
    uint32_t diag_ms;         /* 0.7.14: last direct-control diagnostic look at this unit (keeps the slot alive) */
    int diag_seen, diag_code; /* 0.7.14: last logged flag state (bit0 prop DE0, bit1 DD8 bit, bit2 IsManualControlled, bit3 manager actor) */
    int ad_diag;              /* 0.7.15: last logged auto-draw state + 1 (0 = none yet) */
    uint32_t probe_ms;        /* 0.7.16: probe running since (0 = none) */
    uint32_t probe_last;      /* 0.7.16: last probe start (ProbeCooldownMs) */
    uint32_t nomelee_until;   /* 0.7.16: probe found no melee weapon: no new probe before this */
    int probe_rej;            /* 0.7.16: guns refused during the running probe */
    uint32_t melee_until;     /* 0.7.18: melee ammo was offered for this unit: it carries a melee weapon until then (KnownMs) */
    uint32_t poll_ms;         /* 0.7.18: last order-poll log line for this unit */
    uint32_t blind_ms;        /* 0.7.19: the poll drew without knowing (no probe / button answer): checked after BlindCheckMs */
    const void *ovt;          /* 0.7.20: front order vtable last logged by the poll */
    uint32_t touch;           /* 0.7.26: last use of this slot (staleness; see unit_of) */
    char gun[72];             /* 0.7.26: name words of the weapon in hand when the dll drew the melee weapon (drawn again when no enemy is in fight range) */
    uint32_t gun_ms;          /* 0.7.26: last ranged re-draw order */
    int blind_ord;            /* 0.7.26: the blind draw replaced an attack order on that enemy (given back if no melee weapon shows) */
    uint32_t fr_ms;           /* 0.7.24: last poll that found an enemy in fight range (or the unit fighting) */
} unit_t;
#define NUNIT 512
static unit_t g_unit[NUNIT];
static unit_t *unit_of(const void *u, int make)
{
    uint32_t h = (uint32_t)(((uintptr_t)u >> 4) * 2654435761u) % NUNIT, now = now_ms();
    unit_t *old = NULL;
    for (int i = 0; i < 16; i++) {
        unit_t *s = &g_unit[(h + i) % NUNIT];
        if (s->unit == u) { s->touch = now ? now : 1; return s; }
        /* 0.7.26: a slot is stale 60 s after its last USE. Before, staleness was the newest of inf_ms / ad_until / diag_ms /
         * probe_last: a unit that only had the newer fields set (fr_ms, ad_draw_ms, melee_until, gun) looked stale as soon as the
         * clock passed 60 s, and ad_until (a future time) made the difference wrap. Any other unit hashing nearby then took the
         * slot: Alex's 0.7.25 log after the first minute - draw cooldown, fight-range mark and auto_drawn all forgotten. */
        if (!old && (!s->unit || (uint32_t)(now - s->touch) > 60000)) old = s;
    }
    if (!make) return NULL;
    if (!old) old = &g_unit[h];
    memset(old, 0, sizeof *old);
    old->unit = u; old->touch = now ? now : 1;
    return old;
}
/* 0.7.18: carries a melee weapon - offered in the last 20 s, or found (by the probe or any melee offer) within KnownMs. The 20 s
 * window alone forgot a unit's knife between fights, and the engine only offers it when no gun row wins, so most units were
 * "unknown" exactly when it mattered (Alex's attack-order test). */
static uint32_t cfg_known_ms = 300000;     /* [AutoDraw] KnownMs */
static int melee_by_button(const void *u);   /* 0.7.19: the melee_toggle button's own "unit carries a melee item" test (below) */
static void melee_known(const void *u)
{
    unit_t *s = unit_of(u, 1);
    s->melee_until = now_ms() + cfg_known_ms; if (!s->melee_until) s->melee_until = 1;
    s->nomelee_until = 0;
}
static int has_melee(const void *u)
{
    if (carries_melee(u)) return 1;
    uint32_t now = now_ms();
    unit_t *s = unit_of(u, 0);
    if (s && s->melee_until && (int32_t)(s->melee_until - now) > 0) return 1;
    if (s && s->nomelee_until && (int32_t)(s->nomelee_until - now) > 0) return 0;
    /* 0.7.19 (Alex: "if the melee stance button can force the switch, why not run that in fight range?"): ask the button. Its
     * CanApply is what shows it for a unit ({item "melee"} {item_in_box}); cached like the probe's answer. */
    int b = melee_by_button(u);
    if (b == 1) { melee_known(u); return 1; }
    if (b == 0) { s = unit_of(u, 1); s->nomelee_until = now + cfg_nomelee_ms; if (!s->nomelee_until) s->nomelee_until = 1; }
    return 0;
}
static int auto_active(const void *u)
{
    if (!cfg_ad_on) return 0;
    unit_t *s = unit_of(u, 0);
    return s && s->ad_until && (int32_t)(s->ad_until - now_ms()) > 0;
}
static int is_vehicle(const uint8_t *t) { return t && (t[0x30] & 0x02) && !(t[0x30] & 0x04); }
static int veh_ordered(const uint8_t *u, const uint8_t *tgt);      /* explicit attack order on that vehicle */
static int tag_stance(const uint8_t *ent) { return vec_has(ent + cfg_enttags, g_tagl, n_tagl) == 1; }
/* 0.7.11 (Alex: no melee behaviour and no CQC weapon pull-out while the player drives the unit): a unit under direct control is left
 * alone - the stance ammo filter, the charge, the auto-draw and the reload cancel all skip it. The test is the engine's own
 * eActor::IsManualControlled (game exe+0x7EF8A0: the manual-mode property at actor+0xDE0 (+0x18 != 0) or bit 1 of [[actor+0xDD8]+0x70];
 * the same test the `reset_manual_control` action and the manual-control manager use), found by signature. Not {if user_control}:
 * that is brain+0x398 & 3 == 1 = "belongs to a human player", the PlayerUnits test. */
typedef unsigned char (*ismanual_t)(const void *actor);
static ismanual_t g_ismanual;
/* 0.7.32: eActor::IsManualControlled reads [actor+0xDE0] and [actor+0xDD8] unchecked (see SIG_ISMANUAL). Alex's Nov 10 hosted game:
 * the dedicated server died 26 min in (ACCESS_VIOLATION read at 0x18 in server exe+0x6549A0 <- IsManualControlled <- the dll) on a
 * unit whose +0xDE0 was NULL (a human half torn down). A unit without both components is not under direct control. */
static int ismanual_safe(const uint8_t *ent)
{
    if (!g_ismanual || !ent) return 0;
    if (!*(const void *const *)(ent + 0xDE0) || !*(const void *const *)(ent + 0xDD8)) return 0;
    return g_ismanual(ent) != 0;
}
static int cfg_manual_skip = 1;         /* [MeleeStance] SkipDirectControl */
static uint32_t st_manual;
/* 0.7.12 (Alex: in direct control "it takes my weapon away randomly in CQC" with 0.7.11): second test = the manual-control manager
 * itself (eManualControl, global game exe+0x159E218 / editor 0x140A930, found by signature; none in the server exe): the actor it
 * drives sits at +0x3B0 (the engine getter `mov rax,[rcx+3B0h]`), so the unit the player is in right now is manual whatever the
 * property says. Every skip is logged (per unit every 3 s) so the log shows what the dll did to the driven unit. */
static uint8_t **g_mgr;
static int g_mlines;
static int mgr_actor_is(const uint8_t *ent) { return g_mgr && *g_mgr && *(const uint8_t *const *)(*g_mgr + 0x3B0) == ent; }
static void manual_log(const uint8_t *ent, int prop, int mgr)
{
    if (!cfg_log || g_mlines >= 40) return;
    unit_t *s = unit_of(ent, 1); uint32_t now = now_ms();
    if (s->manual_ms && (uint32_t)(now - s->manual_ms) < 3000) return;
    s->manual_ms = now ? now : 1; g_mlines++;
    logf_("manual: [%u ms] unit %p under direct control (property %d, manager %d): stance filter / charge / auto-draw / reload cancel skipped (skips %u)", now, (const void *)ent, prop, mgr, st_manual);
}
/* 0.7.14 diagnostic (Alex, Nov 8 2026: after leaving direct control the unit still "acted like I was in it"; the 0.7.13 log only ever
 * showed "property 1, manager 0", even while driving): for every unit owned by a human player, log each change of the four inputs
 * separately - the DE0 property value (+0x18), the DD8 +0x70 bit 1 (the two halves of IsManualControlled), IsManualControlled itself,
 * and whether the manager's +0x3B0 is this unit. When IsManualControlled is on but the manager does not name the unit, the manager
 * object's first 0x1000 bytes are searched for the unit pointer (to find the right offset). [MeleeStance] DiagManual, 300 lines/mission. */
static int cfg_manual_diag = 0;          /* 0.7.35: off by default - manual_diag() makes a VirtualQuery call per weapon pick */
static int g_mdiag;
static int readable(const void *p, size_t n)
{
    const uint8_t *a = (const uint8_t *)p;
    while (n) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mi.Protect) return 0;
        size_t left = (size_t)((const uint8_t *)mi.BaseAddress + mi.RegionSize - a);
        if (left >= n) return 1;
        a += left; n -= left;
    }
    return 1;
}
static void manual_diag(const uint8_t *ent)
{
    if (!cfg_log || !cfg_manual_diag) return;
    const uint8_t *br = *(const uint8_t *const *)(ent + 0xDC8);
    if (!br || !readable(br + 0x398, 1) || (br[0x398] & 3) != 1) return;        /* human player's units only */
    const uint8_t *pde0 = *(const uint8_t *const *)(ent + 0xDE0);
    unsigned long long v18 = (pde0 && readable(pde0 + 0x18, 8)) ? *(const unsigned long long *)(pde0 + 0x18) : 0;
    const uint8_t *dd8 = *(const uint8_t *const *)(ent + 0xDD8);
    int bit = (dd8 && readable(dd8 + 0x70, 4)) ? (int)((*(const uint32_t *)(dd8 + 0x70) >> 1) & 1) : 0;
    int ism = g_ismanual ? ismanual_safe(ent) : -1;
    const uint8_t *mo = g_mgr ? *g_mgr : NULL;
    const uint8_t *ma = (mo && readable(mo + 0x3B0, 8)) ? *(const uint8_t *const *)(mo + 0x3B0) : NULL;
    int code = (v18 != 0) | (bit << 1) | ((ism == 1) << 2) | ((ma == ent) << 3);
    unit_t *s = unit_of(ent, 1);
    uint32_t now = now_ms();
    s->diag_ms = now ? now : 1;
    int first = !s->diag_seen, prev = s->diag_code;
    if (!first && prev == code) return;
    s->diag_seen = 1; s->diag_code = code;
    if (first && !code) return;                                                 /* plain unit, never manual: nothing to say */
    if (g_mdiag >= 300) return;
    g_mdiag++;
    logf_("mdiag: [%u ms] unit %p state %d -> %d: prop DE0(+18)=%llX, DD8+70 bit1=%d, IsManualControlled=%d | manager %p, +3B0=%p%s",
          now, (const void *)ent, first ? -1 : prev, code, v18, bit, ism, (const void *)mo, (const void *)ma, ma == ent ? " (THIS unit)" : "");
    if (mo && (code & 4) && !(code & 8) && readable(mo, 0x1000)) {
        char b[256]; int n = 0, hits = 0;
        for (int off = 0; off < 0x1000 && hits < 8; off += 8)
            if (*(const uint8_t *const *)(mo + off) == ent) { n += snprintf(b + n, sizeof b - n, " +0x%X", off); hits++; }
        if (hits) logf_("mdiag:   unit pointer found in the manager at%s", b);
        else logf_("mdiag:   unit pointer not in the manager's first 0x1000 bytes");
    }
}
/* 0.7.17 (jetpack session, Nov 9 2026): units another dll is driving carry a script tag (teleport: `tp_jetpack` from jetpack_start
 * to teleport_end, while its GiveOrder lock refuses every order) - left alone like direct control. [MeleeStance] SkipTags */
/* 0.7.28 (Alex): also `hidden` and `melee_disabled` - script tags that switch the dll off for a unit */
static char cfg_skiptags[8][TLEN] = { "tp_jetpack", "hidden", "melee_disabled", "disable_melee", "valkyrie", "sensor_off" };
static int n_skiptags = 6, g_skiplines;
/* 0.7.28 (Alex): the same units are no targets for the dll either, and neither are call-ins: every Valkyrie carries `valkyrie`
 * (airborne_main.inc), every dormant call-in / spawner `sensor_off` (40k_spawners). A weapon pick against such a target goes
 * straight to the engine (no stance filter, no charge, no auto-draw), and the fight-range poll does not see it as an enemy.
 * tp_jetpack is not in this list: a jumping marine is still an enemy. [MeleeStance] IgnoreTargetTags */
static char cfg_tgttags[8][TLEN] = { "hidden", "melee_disabled", "valkyrie", "sensor_off" };
static int n_tgttags = 4;
static volatile LONG st_tgt_ignored;
static int ignored_target(const uint8_t *t)
{
    return t && n_tgttags && vec_has(t + cfg_enttags, cfg_tgttags, n_tgttags) == 1;
}
static int in_manual(const uint8_t *ent)
{
    if (!ent || !(ent[0x30] & 0x02)) return 0;
    if (n_skiptags && vec_has(ent + cfg_enttags, cfg_skiptags, n_skiptags) == 1) {
        if (cfg_log && g_skiplines < 20) { g_skiplines++; logf_("skip: [%u ms] unit %p carries a SkipTags tag: left alone", now_ms(), (const void *)ent); }
        return 1;
    }
    manual_diag(ent);                                                           /* 0.7.14 */
    if (!cfg_manual_skip) return 0;
    int prop = ismanual_safe(ent), mgr = mgr_actor_is(ent);
    if (!prop && !mgr) return 0;
    st_manual++;
    manual_log(ent, prop, mgr);
    return 1;
}
/* Explicit orders: the unit's current order = the front of its brain's order queue (brain = entity+0xDC8; queue active
 * when [brain+0x39C] & 0x1000: deque map +0x70, map size +0x78, offset +0x80, count +0x88; otherwise the override order
 * at brain+0xF0, which MP client brains use for every order; = the engine getter game exe+0x9056C0). AI battle fights do
 * not use this queue (eOutdoorsBattle keeps its own), so an eAttack / eAttackBasic / eFight at the front that the dll did
 * not add came from the player (or a script command). Target ref at order+0x40: type +0x48 (1 = entity), entity +0x58. */
static void **g_vt_attack, **g_vt_attackb, **g_vt_fight;
#define NMINE 64
static void *g_mine[NMINE];                 /* orders the dll added (charge), newest overwrite oldest */
static int g_mine_i, g_vlines;
static void mine_add(void *o) { if (o) { g_mine[g_mine_i] = o; g_mine_i = (g_mine_i + 1) % NMINE; } }
static void *front_order(const uint8_t *ent)
{
    const uint8_t *b = *(const uint8_t *const *)(ent + 0xDC8);
    if (!b) return NULL;
    if (*(const uint32_t *)(b + 0x39C) & 0x1000) {
        if (*(const int32_t *)(b + 0x88) <= 0) return NULL;
        uint64_t off = *(const uint64_t *)(b + 0x80), ms = *(const uint64_t *)(b + 0x78);
        void **const *map = *(void **const *const *)(b + 0x70);
        if (!map || !ms || (ms & (ms - 1)) || ms > 0x100000) return NULL;
        void **blk = map[(off >> 1) & (ms - 1)];
        return blk ? blk[off & 1] : NULL;
    }
    return *(void *const *)(b + 0xF0);
}
/* target entity of the attack / melee order the unit is running, else NULL; own = 0: only explicit orders (player / script),
 * not the dll's own charge orders */
static const uint8_t *order_target(const uint8_t *ent, int own)
{
    if (!g_vt_attack && !g_vt_fight) return NULL;
    const uint8_t *o = (const uint8_t *)front_order(ent);
    if (!o) return NULL;
    void **vt = *(void **const *)o;
    if (vt != g_vt_attack && vt != g_vt_attackb && vt != g_vt_fight) return NULL;
    if (!own) for (int i = 0; i < NMINE; i++) if (g_mine[i] == o) return NULL;
    if (*(const uint32_t *)(o + 0x48) != 1) return NULL;
    return *(const uint8_t *const *)(o + 0x58);
}
static const uint8_t *explicit_target(const uint8_t *ent) { return order_target(ent, 0); }
/* 0.7.16: the unit is running the melee order (eFight) on this enemy right now */
static int fight_running(const uint8_t *ent, const uint8_t *tgt)
{
    if (!g_vt_fight) return 0;
    const uint8_t *o = (const uint8_t *)front_order(ent);
    if (!o || *(void **const *)o != g_vt_fight) return 0;
    return *(const uint32_t *)(o + 0x48) == 1 && *(const uint8_t *const *)(o + 0x58) == tgt;
}
static int veh_ordered(const uint8_t *u, const uint8_t *tgt)
{
    const uint8_t *t = explicit_target(u);
    int r = t && t == tgt;
    if (r && cfg_log && g_vlines < 20) { g_vlines++; logf_("vehicles: unit %p was ordered onto vehicle %p, melee allowed, no retarget", (const void *)u, (const void *)tgt); }
    return r;
}

/* 0.7.29 zerg (Alex: replaces the melee_inf_atk mission triggers - "charge from 80m, I don't want the lua logic getting them
 * pinned down, they should zerg in"). The triggers took every human with a melee weapon in hand (`melee_capable`) and a located
 * enemy human within 74 m, dropped its orders and gave it an attack order. Here: an AI-owned human (not a human player's unit:
 * his switch is the melee toggle) that HOLDS its melee weapon, not one the dll's fight-range draw put in its hand, counts as in
 * melee stance against an enemy human within [Zerg] Distance: guns refused, run, then the fight order (the stance charge), and
 * the charge order is given again whenever something else (the bot's Lua squad orders) replaced it. */
static int cfg_zerg_on = 1;                /* [Zerg] Enabled */
static float cfg_zerg_dist = 1600.0f;      /* [Zerg] Distance (20 units = 1 m) */
static uint32_t cfg_zerg_reorder = 700;    /* [Zerg] ReorderMs: how soon a replaced charge order is given again */
static char cfg_zerg_tag[1][TLEN] = { "melee_infantry" };   /* [Zerg] Tag (empty = any AI unit holding melee, the 0.7.29 rule) */
static volatile LONG st_zerg, st_zerg_reorder;
static int g_zlines;
static int zerg_unit(const uint8_t *ent, const uint8_t *ti)
{
    if (!cfg_zerg_on || !ent || !ti || !(ent[0x30] & 0x04)) return 0;
    const uint8_t *tg = *(const uint8_t *const *)(ti + 0x18);
    if (!tg || tg == ent || !(tg[0x30] & 0x04) || tg[0xDA8] == ent[0xDA8] || linked_raw(tg)) return 0;
    if (vec_has(ent + cfg_enttags, g_ctagl, 1) != 1) return 0;                   /* melee weapon in hand */
    /* 0.7.30 (Alex: "this will take over the units that pull swords out in combat"): only units carrying the breed tag
     * (melee-only breeds: `melee_infantry`, copied onto the entity by BreedTags) */
    if (cfg_zerg_tag[0][0] && vec_has(ent + cfg_enttags, cfg_zerg_tag, 1) != 1) return 0;
    const uint8_t *br = *(const uint8_t *const *)(ent + 0xDC8);
    if (!br || (br[0x398] & 3) == 1) return 0;                                  /* a human player's unit */
    unit_t *us = unit_of(ent, 0);
    if (us && us->auto_drawn) return 0;                                         /* the dll drew it for a close fight: gun comes back */
    float dx = *(const float *)(ent + 0x74) - *(const float *)(ti + 0x28);
    float dy = *(const float *)(ent + 0x78) - *(const float *)(ti + 0x2C);
    return dx * dx + dy * dy <= cfg_zerg_dist * cfg_zerg_dist;
}
static int front_is_mine(const uint8_t *ent)
{
    const void *o = front_order(ent);
    if (!o) return 0;
    for (int i = 0; i < NMINE; i++) if (g_mine[i] == o) return 1;
    return 0;
}

/* what to do with this candidate: PASS (vanilla), REJECT (not melee, unit in stance), MELEE (melee ammo, unit in stance) */
static int ms_decide(const uint8_t *ctx)
{
    if (!cfg_enabled || !ctx) return PASS;
    const uint8_t *ammo = *(const uint8_t *const *)ctx;
    const uint8_t *ent  = *(const uint8_t *const *)(ctx + 8);
    if (!ammo || !ent) return PASS;
    if (!(ent[0x30] & 0x04)) return PASS;                                       /* humans only */
    if (in_manual(ent)) return PASS;                                            /* 0.7.11: under direct control, the player chooses */
    if (is_linked(ent)) return PASS;                                            /* crew/passenger */
    int m = vec_has(ammo + cfg_ammotags + TAGS_VEC, cfg_mtags, n_mtags);
    if (m < 0) return PASS;
    if (m) {
        if (cfg_log && g_seenlines < 60 && !carries_melee(ent)) {                /* 0.7.15: first melee offer per unit (auto-draw arms on it) */
            g_seenlines++;
            char nm[200]; vec_str(ammo + cfg_namtags + TAGS_VEC, nm, sizeof nm);
            logf_("seen: [%u ms] unit %p offered melee ammo \"%s\" (stance %d): auto-draw armed for %d ms", now_ms(), (const void *)ent, nm, tag_stance(ent), SEEN_MS);
        }
        melee_seen(ent);                                                        /* any human: carried-melee record (auto-draw) */
        melee_known(ent);                                                       /* 0.7.18: long memory */
    }
    int ts = tag_stance(ent);
    if (!ts && zerg_unit(ent, *(const uint8_t *const *)(ctx + 0x10))) {         /* 0.7.29: AI melee holder, enemy human within 80 m */
        st_zerg++;
        if (cfg_log && g_zlines < 40) { g_zlines++; logf_("zerg: [%u ms] unit %p holds melee, enemy %p within %.0f: treated as melee stance (picks %ld)", now_ms(), (const void *)ent, *(void *const *)(*(const uint8_t *const *)(ctx + 0x10) + 0x18), cfg_zerg_dist, (long)st_zerg); }
        return m ? MELEE : REJECT;
    }
    if (cfg_veh_on && m) {                                                      /* hand melee against a vehicle */
        const uint8_t *ti = *(const uint8_t *const *)(ctx + 0x10);
        const uint8_t *tg = ti ? *(const uint8_t *const *)(ti + 0x18) : NULL;
        if (is_vehicle(tg) && vec_has(ammo + cfg_ammotags + TAGS_VEC, cfg_vtags, n_vtags) == 1) {
            if (veh_ordered(ent, tg)) return ts ? MELEE : PASS;                 /* told to attack this vehicle */
            if (!ts) { st_veh_reject++; return REJECT; }                        /* only stance units melee vehicles */
            if (order_target(ent, 1) == tg) return MELEE;                       /* 0.7.1: already charging it (dll order): infantry-first no longer pulls the weapon (units stood beside tanks) */
            unit_t *s = unit_of(ent, 0);
            if (s && s->inf_ms && (uint32_t)(now_ms() - s->inf_ms) < cfg_inf_ms) {                                    /* infantry first */
                st_veh_pref++;
                if (cfg_log && g_vreflines < 20) { g_vreflines++; logf_("vehicles: [%u ms] unit %p melee vs vehicle %p refused: enemy infantry within %.0f seen %u ms ago (refusals %u)", now_ms(), (void *)ent, (void *)tg, cfg_inf_range, now_ms() - s->inf_ms, st_veh_pref); }
                return REJECT;
            }
            return MELEE;
        }
    }
    if (!ts && !auto_active(ent) && cfg_probe_on) {                             /* 0.7.16 probe running: guns refused for this pick */
        unit_t *ps = unit_of(ent, 0);
        uint32_t pnow = now_ms();
        if (ps && ps->probe_ms && (uint32_t)(pnow - ps->probe_ms) < cfg_probe_ms * 4) {
            if (m) {
                ps->probe_ms = 0; ps->nomelee_until = 0; st_probe_ok++;
                if (cfg_log && g_plines < 60) {
                    g_plines++;
                    char nm[200]; vec_str(ammo + cfg_namtags + TAGS_VEC, nm, sizeof nm);
                    logf_("probe: [%u ms] unit %p melee found (\"%s\", %d guns refused): auto-draw armed (found %u, none %u)", pnow, (const void *)ent, nm, ps->probe_rej, st_probe_ok, st_probe_fail);
                }
                /* 0.7.18: a far probe only learns the answer - taking the melee pick at 50 m would swap the weapon twice */
                const uint8_t *pti = *(const uint8_t *const *)(ctx + 0x10);
                if (pti) {
                    float dx = *(const float *)(ent + 0x74) - *(const float *)(pti + 0x28);
                    float dy = *(const float *)(ent + 0x78) - *(const float *)(pti + 0x2C);
                    if (dx * dx + dy * dy > cfg_ad_dist * cfg_ad_dist) return REJECT;
                }
                return MELEE;
            }
            ps->probe_rej++; st_probe_rej++;
            return REJECT;
        }
    }
    if (!ts && !auto_active(ent)) return PASS;                                  /* not in melee stance (toggle / always tag / auto-draw hold) */
    if (m) return MELEE;
    return has_melee(ent) ? REJECT : PASS;                  /* no melee weapon seen on him: keep his guns (0.7.18: long memory) */
}

/* ---------------------------------------------------------------- charge */
#define REFSZ 0x100
typedef struct {
    __attribute__((aligned(16))) uint8_t uref[REFSZ];   /* weak refs: the engine clears them if the entity goes */
    __attribute__((aligned(16))) uint8_t tref[REFSZ];
    uint8_t *unit, *tgt;
    float dist;
    uint32_t clock;
    int ff;                 /* 0.7.24: fight order even when the melee weapon is not in hand */
    int used, veh, stalled; /* veh: target is a vehicle (0.7); stalled: re-order after a stall (0.7.8: vs a vehicle = attack order, no second run) */
} pend_t;
#define NPEND 64
static pend_t g_pend[NPEND];

/* per unit: last charge order (ms, target as a weak ref, distance when ordered / last seen closing) and last take-item order */
typedef struct {
    const void *unit, *tgt; uint32_t ms, draw_ms, chk_ms; float lastd; int has_ref, held;   /* held: 0 unknown, 1 melee in hand, 2 not */
    int phase; float ox, oy;                 /* phase: 1 = move order running at the enemy (ox/oy = its position then), 2 = attack order */
    float ux, uy; int stalls;                /* 0.7.1: the unit's own position at the last stall sample, stalled samples in a row */
    uint32_t vchk_ms, adj_until; float vux, vuy; const void *adj_tgt;   /* 0.7.3: hull-contact sample vs a vehicle, and the "pressed against it" window */
    const void *otgt; int ogo;               /* 0.7.17: last logged order-path pick (enemy, decision + 1) */
    __attribute__((aligned(16))) uint8_t tref[REFSZ];
} last_t;
static float cfg_close = 150.0f;         /* [Charge] CloseDistance: inside it the attack order, outside a move order to the enemy */
static float cfg_refresh = 40.0f;        /* [Charge] MoveRefreshUnits: the move order is re-issued when the enemy moved this far from the ordered spot (0.7.9: 40, was 80) */
static float cfg_refresh_frac = 0.4f;    /* 0.7.13 [Charge] MoveRefreshFraction: ... or this fraction of the current distance, whichever is larger. Alex's Nov 8 game
                                          * log: a unit ran at an enemy that was itself charging about; its goal moved 40+ every RefreshMs 400, every re-issued
                                          * move restarted the run (the unit stops and re-paths), and it hung 400-1000 units off for 90 s. At 500 units the
                                          * goal now has to move 200 (a running enemy: ~2 s) before the run is re-aimed; inside CloseDistance the attack order. */
static int goal_moved(float d, float mx, float my)
{
    float thr = cfg_refresh;
    if (cfg_refresh_frac > 0.0f && d * cfg_refresh_frac > thr) thr = d * cfg_refresh_frac;
    return mx * mx + my * my > thr * thr;
}
#define PROGRESS_SAMPLE_MS 1000          /* the stall test samples a second apart, not tick to tick */
#define CLINES 200                       /* charge log lines per session (0.7.8: was 40, Alex runs several sims per editor session) */
#define STALL_SAMPLES 2                  /* 0.7.1: stalled samples in a row before the order is re-issued */
#define NLAST 256
static last_t g_last[NLAST];
static float cfg_switch = 0.6f;          /* [Charge] SwitchRatio: another enemy only when closer than this x the current target's distance */
static float cfg_progress = 10.0f;       /* world units the unit must have closed on its target between checks to count as still charging */

static void ref_make(uint8_t *ref, void *ent)
{
    memset(ref, 0, REFSZ);
    *(void **)ref = g_refvt;
    g_tagctor(ref + 0x20);
    g_refinit(ref);
    __attribute__((aligned(16))) uint8_t tag[0x80];
    memset(tag, 0, sizeof tag);
    g_tagctor(tag);
    g_setent(ref, ent, tag);
    g_tagdtor(tag);
}
/* 0.7.33: a ref is only trusted while the entity it names still lists it. A player's Nov 10 crash (skirmish, 32 min in, one
 * mission, game exe+0x7E6E90 under RefDtor exe+0x83E4F0, called by known_note's eviction): RefDtor walks the back-ref list of
 * the entity at ref+0x18 to unlink the ref, and that entity's memory had been freed and reused (list begin inside the exe
 * image, end 0), so the walk ran off the image. The engine had freed the entity without clearing our ref (ref+8 still 1), as
 * it does for a whole scene (0.7.31); why for one entity mid-mission is not known. Such a ref also looked alive to every
 * ref_ent() caller. Now: the entity's list must be readable, sane and hold an entry for this ref; if not, ref_ent() says
 * "gone" and ref_free() zeroes the ref first, so RefDtor has nothing to unlink. g_reflist_off 0 (not found) = no check. */
static uint32_t g_reflist_off;
static uint32_t st_ref_stale; static int g_reflines;
static int ref_listed(const uint8_t *ref)
{
    const uint8_t *ent = *(const uint8_t *const *)(ref + 0x18);
    if (!ent || !g_reflist_off) return 1;
    const uint8_t *l = ent + g_reflist_off;
    if (!readable(l, 0x28)) return 0;
    const uint8_t *b = *(const uint8_t *const *)(l + 8), *e = *(const uint8_t *const *)(l + 0x10);
    if (!b || e <= b || ((size_t)(e - b) & 15) || (size_t)(e - b) > 0x100000 || !readable(b, (size_t)(e - b))) return 0;
    for (; b < e; b += 16) if (*(const uint8_t *const *)(b + 8) == ref) return 1;
    return 0;
}
static void ref_free(uint8_t *ref)
{
    if (!ref_listed(ref)) {
        st_ref_stale++;
        if (cfg_log && g_reflines < 20) {
            g_reflines++;
            logf_("ref: weak ref %p names entity %p (state %u) but is not in its ref list: entity freed without notice, dropped with no unlink",
                  (void *)ref, *(void **)(ref + 0x18), *(uint32_t *)(ref + 8));
        }
        *(uint32_t *)(ref + 8) = 0; *(void **)(ref + 0x18) = NULL;
    }
    *(void **)ref = g_refvt;
    g_refdtor(ref);
    g_tagdtor(ref + 0x20);
}
/* ref_ent() runs per weapon pick (up to 15000 a second in Alex's Nov 10 game with 256 units listed) and ref_listed() costs two
 * VirtualQuery calls, so its answer is kept for the sim clock value it was made at, in the slot's spare bytes past the engine's
 * 0x80-byte ref (ref_make clears them): +0xF0 clock + 1, +0xF4 answer, +0xF8 the entity it was made for. ref_free() always checks. */
static void *ref_ent(const uint8_t *ref)
{
    if (*(const uint32_t *)(ref + 8) != 1) return NULL;
    void *ent = *(void *const *)(ref + 0x18);
    if (!ent || !g_reflist_off) return ent;
    uint8_t *w = (uint8_t *)(uintptr_t)ref;
    uint32_t stamp = g_clock ? *g_clock + 1 : 0;
    if (stamp && *(uint32_t *)(w + 0xF0) == stamp && *(void **)(w + 0xF8) == ent) return *(uint32_t *)(w + 0xF4) ? ent : NULL;
    int ok = ref_listed(ref);
    *(uint32_t *)(w + 0xF0) = stamp; *(uint32_t *)(w + 0xF4) = (uint32_t)ok; *(void **)(w + 0xF8) = ent;
    return ok ? ent : NULL;
}

static void last_reset(last_t *l, const void *unit)
{
    if (l->has_ref) ref_free(l->tref);
    l->unit = unit; l->tgt = NULL; l->ms = 0; l->draw_ms = 0; l->chk_ms = 0; l->lastd = 0; l->has_ref = 0; l->held = 0;
    l->phase = 0; l->ox = l->oy = 0; l->ux = l->uy = 0; l->stalls = 0;
    l->otgt = NULL; l->ogo = 0;                                                     /* 0.7.17 */
    l->vchk_ms = 0; l->adj_until = 0; l->vux = l->vuy = 0; l->adj_tgt = NULL;
}
static last_t *last_of(const void *unit, int make)
{
    uint32_t h = (uint32_t)(((uintptr_t)unit >> 4) * 2654435761u) % NLAST;
    for (int i = 0; i < 16; i++) {
        last_t *l = &g_last[(h + i) % NLAST];
        if (l->unit == unit) return l;
        if (make && (!l->unit || (uint32_t)(*g_clock - l->ms) > 60000)) { last_reset(l, unit); return l; }
    }
    if (!make) return NULL;
    last_reset(&g_last[h], unit);
    return &g_last[h];
}
/* remember the enemy a charge order went to */
static void last_order(last_t *l, const void *tgt, float d, uint32_t now)
{
    if (l->has_ref) ref_free(l->tref);
    ref_make(l->tref, (void *)tgt); l->has_ref = 1;
    l->tgt = tgt; l->lastd = d; l->ms = now; l->chk_ms = now; l->stalls = 0;
    if (l->unit) { l->ux = *(const float *)((const uint8_t *)l->unit + 0x74); l->uy = *(const float *)((const uint8_t *)l->unit + 0x78); }
}
static float dist3(const uint8_t *a, const uint8_t *b)
{
    float dx = *(const float *)(a + 0x74) - *(const float *)(b + 0x74);
    float dy = *(const float *)(a + 0x78) - *(const float *)(b + 0x78);
    float dz = *(const float *)(a + 0x7C) - *(const float *)(b + 0x7C);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* issue the queued orders recorded on an earlier tick, except for the unit being scanned right now */
static uint8_t *g_draw_action;
static int draw_melee(uint8_t *action, void **vt, uint8_t *u);
static int reload_cancel(uint8_t *u);                    /* 0.7.1 */

static void charge_flush(const uint8_t *scanning)
{
    uint32_t now = *g_clock;
    for (int i = 0; i < NPEND; i++) {
        pend_t *p = &g_pend[i];
        if (!p->used || p->clock == now || p->unit == scanning) continue;
        void *u = ref_ent(p->uref), *t = ref_ent(p->tref);
        if (u == p->unit && t == p->tgt && !is_linked(p->unit) && !in_manual(p->unit)) {   /* 0.7.11: taken over since it was queued -> dropped */
            /* a direct attack order fires whatever the unit HOLDS (it bypasses the ammo.set choice the stance filters),
             * so a unit whose melee weapon is not in hand yet (no charge tag, set by skin_weapon.inc for the held
             * melee weapon) gets a plain move order to the enemy instead; near it, the stance choice draws the melee
             * weapon, the tag appears and the next charge order is the attack order (0.7.1 video: stance marines ran
             * in and fired flamers / bolt pistols) */
            /* 0.6.7 video + log: an attack order on an enemy 800 units out does not make a stance marine run at it (no
             * usable weapon at that range: guns refused, sword out of reach), the unit shuffles and stands. So the far part
             * of the charge is a plain move order to the enemy's position (units run), refreshed when the enemy moves;
             * the attack order (the double-click) is given only inside CloseDistance, where the engine fights with the sword. */
            int noweap = vec_has(p->unit + cfg_enttags, g_ctagl, 1) != 1;   /* melee weapon not in hand */
            float xy[2] = { *(const float *)(p->tgt + 0x74), *(const float *)(p->tgt + 0x78) };
            float close = p->veh ? cfg_close_veh : cfg_close;
            int mv = (g_uimove || g_mkmove) && close > 0 && p->dist > close && !(p->veh && p->stalled);
            if (mv && (xy[0] * xy[0] + xy[1] * xy[1]) < 1.0f) mv = 0;        /* engine asserts "Moving to zero bug!" */
            void *ok;
            const char *kind;
            int phase = 0;
            if (noweap && g_draw_action && !p->ff) {                    /* draw the melee weapon first (take-item order); next charge = move/attack */
                int issued = draw_melee(g_draw_action, *(void ***)g_draw_action, p->unit);
                ok = (void *)1; kind = issued ? "draw" : "draw-wait"; mv = 0;   /* draw-wait: a take-item order is still running (DrawCooldownMs) */
            } else {
                uint32_t params[2] = { 1, *g_ordstamp };
                void *order;
                if (mv) {                                     /* the player's double-click: movement mode, then eMoveTask */
                    /* 0.6.9 test: the UI click-move order is Order::eAdvance, which queues its own eMovementChange(mode 1, wait)
                     * = the slow weapon-up walk, overriding any mode set before it. The double-click path of Action::e_move
                     * uses MakeMove = Order::eMoveTask (the AI's run order) instead, so that is the far charge now. */
                    if (g_mvchange && cfg_runmode >= 0) {
                        void *mc = g_mvchange(-1, cfg_runmode, 0);
                        if (mc && g_addorder(p->unit, mc, params)) mine_add(mc);   /* 0.7.29: ours too (zerg re-order test) */
                    }
                    order = g_mkmove ? g_mkmove(xy, 0) : g_uimove(xy, 1);
                    if (order) *(uint16_t *)((uint8_t *)order + 0x18) = 4;   /* as Action::e_move does on every click move */
                    kind = g_mkmove ? "run" : "advance"; phase = 1;
                } else {                                      /* the attack_melee order (eFight); attack order if the finder failed */
                    int usefight = g_fight && !(p->veh && cfg_veh_attack);   /* vehicles: the attack order (eFight is the hand fight) */
                    order = usefight ? g_fight(p->tgt) : g_mkatk(p->tref, p->veh ? 1 : 0);   /* 0.7.1: vehicles get the double-click attack (flag -> order+0xC1) */
                    kind = usefight ? "melee" : (p->veh ? "attack-vehicle" : "attack"); phase = 2;
                }
                ok = order ? g_addorder(p->unit, order, params) : NULL;
                if (ok) mine_add(order);
            }
            last_t *l = last_of(p->unit, 1);
            last_order(l, p->tgt, p->dist, now);
            l->phase = phase;
            l->ox = xy[0]; l->oy = xy[1];
            if (ok) st_charge_go++; else st_charge_drop++;
            if (cfg_log && g_clines < CLINES) {
                g_clines++;
                logf_("charge: [%u ms] unit %p -> enemy %p at %.0f %s %s (orders %u, refused %u)", now, (void *)p->unit, (void *)p->tgt,
                      p->dist, kind, ok ? "ordered" : "REFUSED by AddOrder", st_charge_go, st_charge_drop);
            }
        } else st_charge_drop++;
        ref_free(p->uref); ref_free(p->tref);
        p->used = 0;
    }
}

static int g_pend_ff;
/* queue a charge / fight order for a later tick (0.7.18: shared by charge_note and the order poll) */
static void pend_push(uint8_t *ent, uint8_t *tgt, float d, int veh, int stalled, uint32_t now)
{
    pend_t *free_ = NULL;
    for (int i = 0; i < NPEND; i++) {
        pend_t *p = &g_pend[i];
        if (p->used && p->unit == ent) {
            if (veh && !p->veh) return;                       /* infantry already queued: it wins */
            if (veh == p->veh && d >= p->dist) return;        /* already queued a nearer enemy */
            ref_free(p->tref); ref_make(p->tref, tgt);
            p->tgt = tgt; p->dist = d; p->clock = now; p->veh = veh; p->stalled = stalled; p->ff = g_pend_ff;
            return;
        }
        if (!p->used && !free_) free_ = p;
    }
    if (!free_) return;
    ref_make(free_->uref, ent); ref_make(free_->tref, tgt);
    free_->unit = ent; free_->tgt = tgt; free_->dist = d; free_->clock = now; free_->used = 1; free_->veh = veh; free_->stalled = stalled; free_->ff = g_pend_ff;
    st_charge_q++;
}

/* a scan found melee ammo usable vs this enemy: queue an attack order if the enemy is out of reach */
static void charge_note(const uint8_t *ctx, int inhand, int stance)
{
    uint8_t *ent = *(uint8_t *const *)(ctx + 8);
    const uint8_t *ti = *(const uint8_t *const *)(ctx + 0x10);
    const uint8_t *ammo = *(const uint8_t *const *)ctx;
    if (!ent || !ti || !ammo || !(ent[0x30] & 0x04) || is_linked(ent)) return;
    if (in_manual(ent)) return;                                /* 0.7.11: no charge orders for a unit under direct control */
    uint8_t *tgt = *(uint8_t *const *)(ti + 0x18);
    if (!tgt || tgt == ent || !(tgt[0x30] & 0x02)) return;
    if (tgt[0xDA8] == ent[0xDA8]) return;                    /* same owner player: never charge a friend */
    if ((tgt[0x30] & 0x04) && linked_raw(tgt)) return;        /* 0.7.3: crew / passengers can't be fought, don't charge them */
    int veh = is_vehicle(tgt), ts = tag_stance(ent), stalled = 0;
    if (veh && !(cfg_veh_on && ts)) return;                  /* vehicles: stance units only (ms_decide already applied infantry-first) */
    if (cfg_down_on && vec_has(tgt + cfg_enttags, cfg_downtag, 1) == 1) return;   /* downed */
    if (!stance && vec_has(ent + cfg_enttags, g_ctagl, 1) != 1) return;
    if (!stance && cfg_charge_inhand && !inhand) return;
    if (!cfg_charge_player) {
        const uint8_t *br = *(const uint8_t *const *)(ent + 0xDC8);
        if (br && (br[0x398] & 3) == 1) return;
    }
    if (vec_has(ammo + cfg_ammotags + TAGS_VEC, cfg_mtags, n_mtags) != 1) return;
    float dx = *(const float *)(ent + 0x74) - *(const float *)(ti + 0x28);
    float dy = *(const float *)(ent + 0x78) - *(const float *)(ti + 0x2C);
    float dz = *(const float *)(ent + 0x7C) - *(const float *)(ti + 0x30);
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (cfg_veh_on) {                                        /* on a vehicle (player order, or 0.7.1: the dll's own): stay on it, infantry only when much closer */
        const uint8_t *et = order_target(ent, 1);
        if (et && is_vehicle(et) && et != tgt && (veh || d > dist3(ent, et) * cfg_switch)) return;
        /* 0.7.2 (Alex: units kept getting move orders around the tank): an attack order on THIS vehicle is left alone until the
         * engine ends it (vehicle dead / gone): no stall re-order (the unit beside a hull is never "in reach" of the vehicle's
         * centre, so every stall sample re-issued the order and the unit re-pathed), no run refresh. Then the normal rules pick
         * the next target. */
        if (veh && et == tgt) {
            /* 0.7.3 hull contact: sampled once a second; the unit stands still but is still out of reach of the centre */
            last_t *l = last_of(ent, 1);
            uint32_t now2 = *g_clock;
            if ((uint32_t)(now2 - l->vchk_ms) >= PROGRESS_SAMPLE_MS) {
                float ux = *(const float *)(ent + 0x74), uy = *(const float *)(ent + 0x78);
                float mx = ux - l->vux, my = uy - l->vuy; int had = l->vchk_ms != 0;
                l->vchk_ms = now2; l->vux = ux; l->vuy = uy;
                if (had && mx * mx + my * my < cfg_progress * cfg_progress && d < cfg_adj_extra) {
                    if (l->adj_tgt != tgt || (int32_t)(l->adj_until - now2) <= 0) {
                        st_adj++;
                        if (cfg_log && g_adjlines < CLINES) { g_adjlines++; logf_("vehicles: [%u ms] unit %p pressed against %p at %.0f from its centre: range measured from the hull for %u ms", now2, (void *)ent, (void *)tgt, d, cfg_adj_ms); }
                    }
                    l->adj_tgt = tgt; l->adj_until = now2 + cfg_adj_ms; if (!l->adj_until) l->adj_until = 1;
                }
            }
            return;
        }
    }
    {   /* log every change of "melee weapon in hand" on a charging unit (0.6.6 test: units holstered mid-charge) */
        last_t *lh = last_of(ent, 1);
        int h = vec_has(ent + cfg_enttags, g_ctagl, 1) == 1 ? 1 : 2;
        if (lh->held != h) {
            lh->held = h;
            if (cfg_log && g_hlines < CLINES) { g_hlines++; logf_("held: [%u ms] unit %p melee weapon %s at %.0f", *g_clock, (void *)ent, h == 1 ? "IN HAND" : "PUT AWAY", d); }
        }
    }
    if (!ts) {                                                /* melee in hand through the auto-draw: fight what is close, no long charges */
        unit_t *us = unit_of(ent, 0);
        if (us && us->auto_drawn) {
            if (vec_has(ent + cfg_enttags, g_ctagl, 1) != 1 && !auto_active(ent)) us->auto_drawn = 0;
            else if (d > cfg_ad_dist) return;                /* 0.7.26: fight range only (was 2 x): beyond it the unit goes back to its gun */
        }
    }
    uint32_t now = *g_clock;
    if (cfg_fight_on && !veh && d <= cfg_ad_dist) {          /* 0.7.16: fight range -> the melee order (eFight), in reach too */
        if (fight_running(ent, tgt)) return;                 /* already fighting this enemy */
        const uint8_t *xt = explicit_target(ent);
        if (xt && xt != tgt) return;                         /* the player sent it at another enemy: his order stands */
        last_t *fl = last_of(ent, 0);
        if (fl && fl->ms && fl->tgt == tgt && (uint32_t)(now - fl->ms) < cfg_fight_cd) return;   /* just ordered (or drawing) */
        st_fight_q++;
        goto queue;                                          /* flush: draw first if not in hand, else eFight (d < CloseDistance) */
    }
    float reach = g_range((void *)ammo);
    if (d <= reach * 1.1f + 1.0f || d < cfg_charge_min) return;   /* in reach / already fighting: the engine attacks on its own */
    if (cfg_charge_max > 0 && d > cfg_charge_max) return;
    last_t *l = last_of(ent, 0);
    if (l && l->ms) {
        /* one order per enemy: a new AddOrder restarts the running attack order (the unit stops and re-paths), so while
         * the unit is still closing on the enemy it was sent at, nothing is re-issued. A re-order only when the unit has
         * stalled (no progress since the last check, after CooldownMs), the enemy is gone, or another enemy is much closer. */
        uint32_t age = now - l->ms;
        const uint8_t *cur = l->has_ref ? (const uint8_t *)ref_ent(l->tref) : NULL;
        /* 0.7.1: the last order was a draw (or a wait for one) and the melee weapon is in hand now: the run / melee order
         * at once. Before, the draw sat under the same cooldown + progress test as a real order, and that test read the
         * ENEMY walking up as the unit closing in: 0.7.0 log, five marines stood 7-17 s after IN HAND (stop-and-stare). */
        int drawn = l->phase == 0 && age >= 200 && vec_has(ent + cfg_enttags, g_ctagl, 1) == 1;
        /* 0.7.29 zerg: the charge order is no longer in front (the bot's Lua gave the squad a move / hold order, or the run
         * ended): give it again at once, no stall wait */
        if (!drawn && !ts && !veh && age >= cfg_zerg_reorder && zerg_unit(ent, ti) && !front_is_mine(ent)) {
            drawn = 1; st_zerg_reorder++;
            if (cfg_log && g_zlines < 80) { g_zlines++; logf_("zerg: [%u ms] unit %p charge order replaced (front %s): ordered again (re-orders %ld)", now, (void *)ent, front_order(ent) ? "another order" : "none", (long)st_zerg_reorder); }
        }
        if (!drawn) {
            int renew = 0;
            if (cur && cur == tgt && l->phase == 1) {                       /* running at it under a move order */
                float mx = *(const float *)(tgt + 0x74) - l->ox, my = *(const float *)(tgt + 0x78) - l->oy;
                if (d <= (veh ? cfg_close_veh : cfg_close)) renew = 1;       /* close enough: switch to the attack order (0.7.8: vehicles earlier) */
                else if (goal_moved(d, mx, my)) renew = 1;                 /* enemy moved: new destination (0.7.13: by a share of the distance) */
            }
            if (renew) { if (age < cfg_refresh_ms) return; }               /* 0.7.9: re-aim at the same enemy on its own, faster gate */
            else if (age < cfg_retarget) return;
            if (cur && cur == tgt) {
                if (!renew) {
                    /* stall test: sampled once a second; the unit counts as charging while IT moved ProgressUnits or the
                     * distance closed by as much (0.7.1: its own position, so an approaching enemy is no longer "progress");
                     * STALL_SAMPLES stalled samples in a row and CooldownMs since the order -> re-issue */
                    if ((uint32_t)(now - l->chk_ms) < PROGRESS_SAMPLE_MS) return;
                    float ux = *(const float *)(ent + 0x74), uy = *(const float *)(ent + 0x78);
                    float mx = ux - l->ux, my = uy - l->uy, prev = l->lastd;
                    l->lastd = d; l->chk_ms = now; l->ux = ux; l->uy = uy;
                    if (mx * mx + my * my >= cfg_progress * cfg_progress || d < prev - cfg_progress) { l->stalls = 0; return; }
                    if (++l->stalls < STALL_SAMPLES) return;
                    if (age < cfg_charge_cd) return;
                    if (cfg_log && g_clines < CLINES) { g_clines++; logf_("charge: [%u ms] unit %p stalled at %.0f from enemy %p (phase %d, %d samples): re-ordering", now, (void *)ent, d, (void *)tgt, l->phase, l->stalls); }
                    l->stalls = 0; stalled = 1;
                }
            } else if (cur) {
                int cveh = is_vehicle(cur);
                if (veh && !cveh) return;                                   /* never leave infantry for a vehicle */
                if (d > dist3(ent, cur) * cfg_switch) return;               /* current enemy still there and not much farther (0.7.1: vehicles too; "gives way at once" sent units off) */
            }
        }
    }
queue:;
    pend_push(ent, tgt, d, veh, stalled, now);
}

/* ---------------------------------------------------------------- auto-draw queue */
/* take-item orders for the auto-draw are queued in the scan and added on a later tick from another unit's scan, like
 * the charge orders. Item tags = a TagContainer holding "melee" (the word the melee_toggle button's {item} matches;
 * it comes from the stuff\melee folder name), built once with the engine's own TagContainer ctor + FromString. */
typedef void (*tcctor_t)(void *tc);
typedef void (*tcfromstr_t)(void *tc, void *str, char sep);
typedef void *(*takeitem_t)(void *itemtags);
static takeitem_t g_takeitem;
static uint32_t cfg_draw_cd = 3000;            /* ms: no second take-item order for a unit while the first may still be running */
static tcctor_t g_tcctor;
static tcfromstr_t g_tcfromstr;
static uint8_t *g_melee_tc;
static char cfg_ad_item[TLEN] = "melee";
typedef struct { __attribute__((aligned(16))) uint8_t uref[REFSZ]; uint8_t *unit; uint32_t clock; int used; } dpend_t;
#define NDPEND 32
static dpend_t g_dpend[NDPEND];
static int g_adlines;

static void *melee_tags(void)
{
    if (g_melee_tc) return g_melee_tc;
    if (!g_tcctor || !g_tcfromstr) return NULL;
    uint8_t *tc = (uint8_t *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   /* kept for the session */
    if (!tc) return NULL;
    g_tcctor(tc);
    __attribute__((aligned(16))) uint8_t s[0x20];                   /* MSVC std::string, small-buffer form */
    memset(s, 0, sizeof s);
    size_t n = strlen(cfg_ad_item); if (n > 15) n = 15;
    memcpy(s, cfg_ad_item, n);
    *(uint64_t *)(s + 0x10) = n; *(uint64_t *)(s + 0x18) = 15;
    g_tcfromstr(tc, s, ' ');
    char got[200]; vec_str(tc + TAGS_VEC, got, sizeof got);
    if (cfg_log) logf_("auto-draw: item tags built: \"%s\"", got);
    if (!got[0]) return NULL;
    g_melee_tc = tc;
    return tc;
}

/* queue one take-item (draw) order for a later tick, unless the melee weapon is in hand, one is queued, or one went out within
 * DrawCooldownMs (0.7.18: shared by the scan's auto-draw and the order poll); 1 = queued */
static int dpend_push(uint8_t *ent, unit_t *s, uint32_t now)
{
    if (vec_has(ent + cfg_enttags, g_ctagl, 1) == 1) return 0;                  /* melee already in hand */
    if (s->ad_draw_ms && (uint32_t)(now - s->ad_draw_ms) < cfg_draw_cd) return 0;
    if (!g_takeitem) return 0;
    dpend_t *free_ = NULL;
    for (int i = 0; i < NDPEND; i++) {
        if (g_dpend[i].used && g_dpend[i].unit == ent) return 0;
        if (!g_dpend[i].used && !free_) free_ = &g_dpend[i];
    }
    if (!free_) return 0;
    ref_make(free_->uref, ent); free_->unit = ent; free_->clock = now; free_->used = 1;
    s->ad_draw_ms = now;                                                        /* one queued order per cooldown */
    return 1;
}

static int g_rc_force;                     /* 0.7.24: reload_cancel ignores KeepTags (fight-range draws) */
static int held_weapon_name(uint8_t *u, char *out, size_t cap);
static const char *queue_info(const uint8_t *u, char *buf, size_t cap);
static void autodraw_flush(const uint8_t *scanning)
{
    uint32_t now = *g_clock;
    for (int i = 0; i < NDPEND; i++) {
        dpend_t *p = &g_dpend[i];
        if (!p->used || p->clock == now || p->unit == scanning) continue;
        void *u = ref_ent(p->uref);
        int done = 0;
        if (u == p->unit && !is_linked(p->unit) && !in_manual(p->unit) && vec_has(p->unit + cfg_enttags, g_ctagl, 1) != 1) {   /* still no melee in hand (0.7.11: not under direct control) */
            void *tags = melee_tags();
            unit_t *fs = unit_of(p->unit, 1);
            { char gn[72]; if (held_weapon_name(p->unit, gn, sizeof gn)) memcpy(fs->gun, gn, sizeof fs->gun); }   /* 0.7.26 */
            g_rc_force = fs->fr_ms && (uint32_t)(now - fs->fr_ms) < 2000;   /* 0.7.24: fight range - heavy weapons give way too (Alex) */
            if (tags && g_takeitem) reload_cancel(p->unit);          /* 0.7.1 */
            g_rc_force = 0;
            void *o = tags && g_takeitem ? g_takeitem(tags) : NULL;
            uint32_t params[2] = { 1, *g_ordstamp };
            void *ok = o ? g_addorder(p->unit, o, params) : NULL;
            unit_t *s = unit_of(p->unit, 1);
            if (ok) { st_ad_draw++; s->ad_draw_ms = now; s->auto_drawn = 1; }
            done = 1;
            if (cfg_log && g_adlines < 40) { g_adlines++; char qi[160]; logf_("auto-draw: [%u ms] unit %p take-item order %s (auto draws %u, holds %u) | unit now has %s | gun to go back to: \"%s\"", now, (void *)p->unit, ok ? "added" : "REFUSED / no item tags", st_ad_draw, st_ad_hold, queue_info(p->unit, qi, sizeof qi), s->gun); }
        }
        (void)done;
        ref_free(p->uref);
        p->used = 0;
    }
}

/* every enemy the scan looks at (before the ammo decision): infantry sightings for the vehicle rule, and the auto-draw */
static void scan_note(const uint8_t *ctx)
{
    uint8_t *ent = *(uint8_t *const *)(ctx + 8);
    const uint8_t *ti = *(const uint8_t *const *)(ctx + 0x10);
    if (g_clock) log_epoch(*g_clock);                                            /* 0.7.13 */
    if (!ent || !ti || !(ent[0x30] & 0x04) || is_linked(ent)) return;
    if (in_manual(ent)) return;                                                 /* 0.7.11: no auto-draw under direct control */
    const uint8_t *tgt = *(const uint8_t *const *)(ti + 0x18);
    if (!tgt || tgt == ent || !(tgt[0x30] & 0x04)) return;                     /* enemy infantry only */
    if (tgt[0xDA8] == ent[0xDA8]) return;
    if (linked_raw(tgt)) return;                                                /* 0.7.3: a gun's / vehicle's crew is not "infantry seen" (it kept stance units off cannons) */
    if (cfg_down_on && vec_has(tgt + cfg_enttags, cfg_downtag, 1) == 1) return;
    float dx = *(const float *)(ent + 0x74) - *(const float *)(ti + 0x28);
    float dy = *(const float *)(ent + 0x78) - *(const float *)(ti + 0x2C);
    float dz = *(const float *)(ent + 0x7C) - *(const float *)(ti + 0x30);
    float d2 = dx * dx + dy * dy + dz * dz;
    uint32_t now = *g_clock;
    int in_inf = cfg_veh_on && d2 <= cfg_inf_range * cfg_inf_range;
    int in_ad = cfg_ad_on && d2 <= cfg_ad_dist * cfg_ad_dist && has_melee(ent);   /* 0.7.18: long memory */
    if (in_ad && !cfg_ad_player) {
        const uint8_t *br = *(const uint8_t *const *)(ent + 0xDC8);
        if (br && (br[0x398] & 3) == 1) in_ad = 0;
    }
    if (cfg_log && cfg_ad_on && d2 <= cfg_ad_dist * cfg_ad_dist) {             /* 0.7.15: enemy infantry in auto-draw range - log each state change */
        const uint8_t *br = *(const uint8_t *const *)(ent + 0xDC8);
        int st = (has_melee(ent) ? 1 : 0) | (vec_has(ent + cfg_enttags, g_ctagl, 1) == 1 ? 2 : 0) | (in_ad ? 4 : 0)
               | (br && (br[0x398] & 3) == 1 ? 8 : 0) | (tag_stance(ent) ? 16 : 0);
        unit_t *ds = unit_of(ent, 1);
        if (ds->ad_diag != st + 1 && g_addiag < 120) {
            ds->ad_diag = st + 1; g_addiag++;
            logf_("adiag: [%u ms] unit %p enemy %p at %.0f (draw range %.0f): melee offered recently %d, melee in hand %d, player unit %d, stance %d -> %s",
                  now, (void *)ent, (const void *)tgt, sqrtf(d2), cfg_ad_dist, st & 1, (st >> 1) & 1, (st >> 3) & 1, (st >> 4) & 1,
                  !(st & 1) ? "NO DRAW (engine never offered this unit's melee ammo in the last 20 s)"
                  : (st & 2) ? "nothing to do (in hand)" : (st & 4) ? "draw" : "NO DRAW (PlayerUnits=0)");
        }
    }
    if (cfg_ad_on && cfg_probe_on && (cfg_probe_any || d2 <= cfg_ad_dist * cfg_ad_dist) && !has_melee(ent) && !tag_stance(ent)
        && vec_has(ent + cfg_enttags, g_ctagl, 1) != 1) {                       /* 0.7.16: enemy close (0.7.18: any enemy), melee unknown -> probe */
        const uint8_t *br = *(const uint8_t *const *)(ent + 0xDC8);
        if (cfg_ad_player || !(br && (br[0x398] & 3) == 1)) {
            unit_t *ps = unit_of(ent, 1);
            if (ps->probe_ms) {
                uint32_t age = now - ps->probe_ms;
                if (age >= cfg_probe_ms && ps->probe_rej) {                       /* only guns offered during the probe */
                    ps->probe_ms = 0; st_probe_fail++;
                    ps->nomelee_until = now + cfg_nomelee_ms; if (!ps->nomelee_until) ps->nomelee_until = 1;
                    if (cfg_log && g_plines < 60) { g_plines++; logf_("probe: [%u ms] unit %p no melee weapon (%d guns refused, none offered): guns kept for %u ms (found %u, none %u)", now, (void *)ent, ps->probe_rej, cfg_nomelee_ms, st_probe_ok, st_probe_fail); }
                } else if (age >= cfg_probe_ms * 4) ps->probe_ms = 0;            /* no weapon pick seen at all: drop it quietly */
            } else if (!(ps->nomelee_until && (int32_t)(ps->nomelee_until - now) > 0)
                       && !(ps->probe_last && (uint32_t)(now - ps->probe_last) < cfg_probe_cd)) {
                ps->probe_ms = now ? now : 1; ps->probe_last = ps->probe_ms; ps->probe_rej = 0; st_probe++;
                if (cfg_log && g_plines < 60) { g_plines++; logf_("probe: [%u ms] unit %p enemy %p at %.0f, no melee seen: guns refused for one pick (probes %u)", now, (void *)ent, (const void *)tgt, sqrtf(d2), st_probe); }
            }
        }
    }
    if (!in_inf && !in_ad) return;
    unit_t *s = unit_of(ent, 1);
    if (in_inf) s->inf_ms = now;
    if (!in_ad) return;
    if (!s->ad_until || (int32_t)(s->ad_until - now) <= 0) st_ad_hold++;
    s->ad_until = now + cfg_ad_hold; if (!s->ad_until) s->ad_until = 1;
    dpend_push(ent, s, now);
}

/* 0.7.3: while the unit is pressed against this vehicle, the engine's range test gets a target point at reach x 0.8 in front of
 * the unit (the hull is where it stands); the ti point is restored right after the match. 1 = shifted, saved[] holds the original. */
static int adj_shift(uint8_t *ctx, float *saved)
{
    if (!cfg_veh_on || !cfg_adj_ms || !g_range || !g_clock) return 0;
    uint8_t *ent = *(uint8_t **)(ctx + 8), *ti = *(uint8_t **)(ctx + 0x10); const uint8_t *ammo = *(const uint8_t *const *)ctx;
    if (!ent || !ti || !ammo) return 0;
    const uint8_t *tgt = *(const uint8_t *const *)(ti + 0x18);
    if (!is_vehicle(tgt)) return 0;
    float *pt = (float *)(ti + 0x28);
    float dx = pt[0] - *(float *)(ent + 0x74), dy = pt[1] - *(float *)(ent + 0x78), dz = pt[2] - *(float *)(ent + 0x7C);
    float dist = sqrtf(dx * dx + dy * dy + dz * dz), want = ADJ_AHEAD;
    (void)ammo;
    last_t *l = last_of(ent, 0);
    int marked = l && l->adj_tgt == tgt && ((int32_t)(l->adj_until - *g_clock) > 0
                                            || (dist < cfg_adj_extra * 2.0f && order_target(ent, 1) == tgt));   /* 0.7.6: armed for the whole attack */
    if (!marked && n_contacttag && dist < cfg_adj_extra && vec_has(ent + cfg_enttags, cfg_contacttag, n_contacttag) == 1
        && (order_target(ent, 1) == tgt || dist < cfg_adj_extra * 0.5f)) {
        /* 0.7.4: the script saw this unit touch a vehicle's ram volume (tag): hull contact at once, no stall sample needed */
        l = last_of(ent, 1);
        if (l->adj_tgt != tgt) { st_adj++; if (cfg_log && g_adjlines < 40) { g_adjlines++; logf_("vehicles: [%u ms] unit %p touches %p at %.0f from its centre (%s tag): range measured from the hull", *g_clock, (void *)ent, (void *)tgt, dist, cfg_contacttag[0]); } }
        l->adj_tgt = tgt; l->adj_until = *g_clock + cfg_adj_ms; if (!l->adj_until) l->adj_until = 1;
        marked = 1;
    }
    if (!marked) return 0;
    if (dist <= want || dist < 1.0f) return 0;
    saved[0] = pt[0]; saved[1] = pt[1]; saved[2] = pt[2];
    float k = want / dist;
    pt[0] = *(float *)(ent + 0x74) + dx * k; pt[1] = *(float *)(ent + 0x78) + dy * k; pt[2] = *(float *)(ent + 0x7C) + dz * k;
    st_adj_shift++;
    return 1;
}

/* 0.7.17 weapon picks from ORDERS (Alex's Nov 9 snapshot: under his right-click attack order the Sternguard punched with the
 * bolter within 5 m; the dll had logged nothing for him after 82 s because the auto-draw / probe / fight order only ran from the
 * engine's enemy scan, which a unit with an explicit order does not run). Calls from anywhere but the scan are checked here. */
static int cfg_order_on = 1;         /* [AutoDraw] OrderPicks */
static int g_olines, g_rawlines;
static uint8_t *g_exe_lo;            /* exe base, for the caller RVA in the log (set at install) */

/* Whether a weapon pick made for an ORDER (not the AI scan) against enemy infantry inside fight range goes melee (probe /
 * auto-draw hold / eFight) instead of keeping the engine's own choice (the gun).
 *   ent            the unit;   tgt  the enemy;   d  distance in world units (20 = 1 m), already <= [AutoDraw] Distance
 *   player_ordered 1 = the player's (or a script's) own attack order is on THIS enemy (not a dll order)
 *   stance         1 = the unit is in melee stance (toggle / AlwaysTag)
 *   heavy          1 = the gun in hand carries a [ReloadCancel] KeepTags tag (heavy bolter, flamer, lascannon, ...)
 * Alex (Nov 9 2026): "always pull out a melee weapon if possible when ordered to attack within fight range or when the AI is
 * using them" - every unit, ordered or not, heavy weapons too. "If possible" = the probe: no melee weapon -> the gun stays.
 * The inputs stay (logged on every order: line) in case a unit class needs an exception later. */
static int order_goes_melee(const uint8_t *ent, const uint8_t *tgt, float d, int player_ordered, int stance, int heavy)
{
    (void)ent; (void)tgt; (void)d; (void)player_ordered; (void)stance; (void)heavy;
    return 1;
}

static int holds_keep_weapon(uint8_t *u);   /* [ReloadCancel] KeepTags test on the weapon in hand (below) */
/* 1 = this order-path pick is against enemy infantry in fight range and the policy says melee; logs each new (unit, enemy) */
static int order_pick(const uint8_t *ctx, const uint8_t *ret)
{
    if (!cfg_order_on || !cfg_ad_on || !g_clock) return 0;
    uint8_t *ent = *(uint8_t *const *)(ctx + 8);
    const uint8_t *ti = *(const uint8_t *const *)(ctx + 0x10);
    if (!ent || !ti || !(ent[0x30] & 0x04) || is_linked(ent) || in_manual(ent)) return 0;
    const uint8_t *tgt = *(const uint8_t *const *)(ti + 0x18);
    if (!tgt || tgt == ent || !(tgt[0x30] & 0x04) || tgt[0xDA8] == ent[0xDA8] || linked_raw(tgt)) return 0;
    float dx = *(const float *)(ent + 0x74) - *(const float *)(ti + 0x28);
    float dy = *(const float *)(ent + 0x78) - *(const float *)(ti + 0x2C);
    float dz = *(const float *)(ent + 0x7C) - *(const float *)(ti + 0x30);
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d > cfg_ad_dist) return 0;
    int po = explicit_target(ent) == tgt, st = tag_stance(ent), hv = holds_keep_weapon(ent);
    int go = order_goes_melee(ent, tgt, d, po, st, hv);
    last_t *l = last_of(ent, 1);
    if (cfg_log && g_olines < 60 && (l->otgt != tgt || l->ogo != go + 1)) {
        g_olines++;
        const uint8_t *ammo = *(const uint8_t *const *)ctx; char nm[200]; vec_str(ammo + cfg_namtags + TAGS_VEC, nm, sizeof nm);
        logf_("order: [%u ms] unit %p weapon pick outside the scan (caller exe+0x%llX) vs enemy %p at %.0f, \"%s\", player order %d stance %d heavy %d -> %s",
              *g_clock, (void *)ent, (unsigned long long)(g_exe_lo && ret > g_exe_lo ? ret - g_exe_lo : 0), (const void *)tgt, d, nm, po, st, hv,
              go ? "melee (probe / hold / fight order)" : "engine's choice");
    }
    l->otgt = tgt; l->ogo = go + 1;
    return go;
}

/* 0.7.18 order poll. Alex's 0.7.17 test: a right-click attack order inside fight range still punched with the bolter, and the
 * dll logged no weapon pick at all for the unit (no `order:` line, snapshot: one unit ever tracked). Under an explicit attack
 * order the engine does not re-pick the weapon through MatchAmmo once it has one, so nothing the dll hooks runs for that unit.
 * So the dll looks at the units itself: every human that ran an enemy scan is remembered (weak refs, KnownUnits slots, least
 * recently seen replaced), and every PollMs, from some other unit's scan, each one whose front order is an explicit attack
 * (eAttack / eAttackBasic, not the dll's own) on enemy infantry inside [AutoDraw] Distance is handled like a scan pick in
 * fight range: melee in hand -> the fight order (eFight); known to carry melee -> drawn (take-item, then the scan path gives
 * the fight order); melee unknown / none -> left alone (the engine's choice; the probe answers that from the unit's scans). */
/* 0.7.20: class name of an engine object's vtable through its MSVC RTTI (vt[-1] = CompleteObjectLocator: +0x0C type descriptor
 * RVA, +0x14 its own RVA; name at type descriptor +0x10, e.g. ".?AUeAttack@Order@@"), every read checked; cached per vtable */
#define NVTNAME 64
static void **g_vtn_vt[NVTNAME]; static const char *g_vtn_nm[NVTNAME]; static int g_vtn_n;
static const char *vt_name(void **vt)
{
    if (!vt) return NULL;
    for (int i = 0; i < g_vtn_n; i++) if (g_vtn_vt[i] == vt) return g_vtn_nm[i];
    const char *nm = NULL;
    if (readable(vt - 1, 8)) {
        const uint8_t *col = (const uint8_t *)vt[-1];
        if (col && readable(col, 0x18) && *(const uint32_t *)col == 1) {
            const uint8_t *base = col - *(const uint32_t *)(col + 0x14);
            const char *n = (const char *)(base + *(const uint32_t *)(col + 0x0C) + 0x10);
            if (readable(n, 64) && n[0] == '.' && n[1] == '?') nm = n;
        }
    }
    if (g_vtn_n < NVTNAME) { g_vtn_vt[g_vtn_n] = vt; g_vtn_nm[g_vtn_n] = nm; g_vtn_n++; }
    return nm;
}

static uint32_t g_poll_last;
#define NKNOWN 256
typedef struct { __attribute__((aligned(16))) uint8_t ref[REFSZ]; uint8_t *unit; uint32_t ms; int used; } known_t;
static known_t g_known[NKNOWN];
static int cfg_poll_on = 1;                /* [AutoDraw] OrderPoll */
static uint32_t cfg_poll_ms = 250;         /* [AutoDraw] PollMs */
static uint32_t cfg_blind_ms = 8000;       /* 0.7.19 [AutoDraw] BlindCheckMs: a draw tried without knowing is judged after this */
static uint32_t st_poll_fight, st_poll_draw, st_poll_unknown;
static int g_polllines, g_ovtlines;
/* 0.7.35: known_note() runs twice per weapon pick (the scanning unit and its target) and searched all NKNOWN slots each time
 * (288 bytes apart: ~70 KB walked twice a call; Alex's Nov 10 Beta 3 match: 2.6 million picks, 22 us of dll time each against
 * 0.6 us for the engine's own MatchAmmo). A direct-mapped table of "noted at" times in front of it: a unit noted less than
 * KCACHE_MS ago is not looked up again (its slot's time is at most that stale, which the least-recently-seen eviction does not
 * notice). A unit dropped from the table inside that window is simply noted again after it. */
#define NKCACHE 1024
#define KCACHE_MS 200
static struct { const uint8_t *unit; uint32_t ms; } g_kcache[NKCACHE];
static int cfg_kcache = 1;                 /* [MeleeStance] FastPass */
static void known_note(uint8_t *ent, uint32_t now)
{
    if (cfg_kcache) {
        uint32_t ch = (uint32_t)(((uintptr_t)ent >> 4) * 2654435761u) % NKCACHE;
        if (g_kcache[ch].unit == ent && (uint32_t)(now - g_kcache[ch].ms) < KCACHE_MS) return;
        g_kcache[ch].unit = ent; g_kcache[ch].ms = now;
    }
    known_t *free_ = NULL, *old = NULL;
    for (int i = 0; i < NKNOWN; i++) {
        known_t *k = &g_known[i];
        if (k->used && k->unit == ent) { k->ms = now; return; }
        if (!k->used) { if (!free_) free_ = k; }
        else if (!old || (int32_t)(k->ms - old->ms) < 0) old = k;
    }
    known_t *k = free_ ? free_ : old;
    if (!k) return;
    if (k->used) ref_free(k->ref);
    ref_make(k->ref, ent); k->unit = ent; k->ms = now; k->used = 1;
}
/* 0.7.22 wanted fights: the poll drew a melee weapon for a unit ordered onto an enemy in fight range; as soon as the weapon is
 * in hand the unit gets the fight order on that enemy, whatever became of the attack order meanwhile (the take-item order may
 * replace it). Weak refs; dropped after WANT_MS or when the unit / enemy is gone. */
#define NWANT 32
#define WANT_MS 12000
typedef struct { __attribute__((aligned(16))) uint8_t uref[REFSZ]; __attribute__((aligned(16))) uint8_t tref[REFSZ];
                 uint8_t *unit, *tgt; uint32_t ms; int used, esc, kind; } want_t;   /* kind 1 (0.7.26): attack order once the melee weapon is away */
static want_t g_want[NWANT];
static uint32_t st_want_fight, st_want_esc;
/* 0.7.24: which owner players have been seen hostile to each other (an enemy scan's target, an attack order's target): the
 * fight-range search only pairs units of such owners (a different owner alone may be an ally) */
static uint8_t g_hostile[256][32];
static void hostile_set(uint8_t a, uint8_t b) { if (a != b) { g_hostile[a][b >> 3] |= (uint8_t)(1 << (b & 7)); g_hostile[b][a >> 3] |= (uint8_t)(1 << (a & 7)); } }
static int hostile_is(uint8_t a, uint8_t b) { return a != b && (g_hostile[a][b >> 3] >> (b & 7)) & 1; }
static int cfg_prox_on = 1;                /* [AutoDraw] Proximity */
static float cfg_leave = 1.5f;             /* 0.7.27 [AutoDraw] LeaveFactor: a unit the dll put into melee stays in it until the enemy is beyond
                                            * Distance x this (Alex's 0.7.26 log: target at 116 -> gun, 3 s later at 96 -> knife again) */
static uint32_t cfg_esc_ms = 1500;         /* [AutoDraw] EscalateMs: knife still not in hand this long after the draw -> the fight order anyway */
/* g_pend_ff (declared above pend_push): the next pend_push gives the fight order even without the melee weapon in hand */
static const char *queue_info(const uint8_t *u, char *buf, size_t cap)
{
    const uint8_t *b = *(const uint8_t *const *)(u + 0xDC8);
    const uint8_t *o = (const uint8_t *)front_order(u);
    const char *nm = o ? vt_name(*(void **const *)o) : NULL;
    int cnt = b && (*(const uint32_t *)(b + 0x39C) & 0x1000) ? *(const int32_t *)(b + 0x88) : (o ? 1 : 0);
    snprintf(buf, cap, "%d order(s), front %s%s", cnt, o ? (nm ? nm : "?") : "none", holds_keep_weapon((uint8_t *)u) ? ", heavy weapon in hand" : "");
    return buf;
}
static void want_drop(const uint8_t *u)
{
    for (int i = 0; i < NWANT; i++)
        if (g_want[i].used && g_want[i].unit == u) { ref_free(g_want[i].uref); ref_free(g_want[i].tref); g_want[i].used = 0; }
}
static void want_add_k(uint8_t *u, uint8_t *tgt, uint32_t now, int kind)
{
    want_t *free_ = NULL;
    for (int i = 0; i < NWANT; i++) {
        want_t *w = &g_want[i];
        if (w->used && w->unit == u) {
            if (w->tgt != tgt) { ref_free(w->tref); ref_make(w->tref, tgt); w->tgt = tgt; }
            if (w->kind != kind) { w->kind = kind; w->esc = 0; }
            w->ms = now; return;
        }
        if (!w->used && !free_) free_ = w;
    }
    if (!free_) return;
    ref_make(free_->uref, u); ref_make(free_->tref, tgt);
    free_->unit = u; free_->tgt = tgt; free_->ms = now; free_->used = 1; free_->esc = 0; free_->kind = kind;
}
static void want_add(uint8_t *u, uint8_t *tgt, uint32_t now) { want_add_k(u, tgt, now, 0); }
static uint32_t st_gun_try, st_gun_draw, st_atk_back;
static int g_rblines;
static int ranged_back(uint8_t *u, uint32_t now, uint8_t *atk_tgt);
static void want_poll(const uint8_t *scanning, uint32_t now)
{
    for (int i = 0; i < NWANT; i++) {
        want_t *w = &g_want[i];
        if (!w->used) continue;
        uint8_t *u = (uint8_t *)ref_ent(w->uref), *t = (uint8_t *)ref_ent(w->tref);
        if (u != w->unit || t != w->tgt || (uint32_t)(now - w->ms) > WANT_MS) { ref_free(w->uref); ref_free(w->tref); w->used = 0; continue; }
        if (u == scanning || is_linked(u) || in_manual(u)) continue;
        if (cfg_down_on && vec_has(t + cfg_enttags, cfg_downtag, 1) == 1) { ref_free(w->uref); ref_free(w->tref); w->used = 0; continue; }
        if (w->kind == 1) {                                                     /* 0.7.26: the attack order the dll took away comes back */
            if (vec_has(u + cfg_enttags, g_ctagl, 1) == 1 && (uint32_t)(now - w->ms) < 4000) continue;   /* melee weapon still in hand */
            uint32_t params[2] = { 1, *g_ordstamp };
            void *o = g_mkatk ? g_mkatk(w->tref, 0) : NULL, *ok = o ? g_addorder(u, o, params) : NULL;
            st_atk_back++;
            if (cfg_log && g_rblines < 60) { g_rblines++; logf_("ranged: [%u ms] unit %p attack order on enemy %p at %.0f given back %s (%u)", now, (void *)u, (void *)t, dist3(u, t), ok ? "" : "- REFUSED", st_atk_back); }
            ref_free(w->uref); ref_free(w->tref); w->used = 0; continue;
        }
        unit_t *s = unit_of(u, 1);
        s->ad_until = now + cfg_ad_hold; if (!s->ad_until) s->ad_until = 1;     /* guns stay refused while the draw runs */
        if (vec_has(u + cfg_enttags, g_ctagl, 1) != 1) {                        /* not in hand yet */
            /* 0.7.24: 0.7.23 log - take-item "added" under an eAttack order, nothing in hand 2.6 s later, eAttack still in
             * front. If the draw has not shown after EscalateMs, give the fight order anyway (it replaces the gun attack) and
             * log what the unit's order queue looks like, so the next log says what the take-item order was waiting behind. */
            if (!w->esc && cfg_esc_ms && (uint32_t)(now - w->ms) >= cfg_esc_ms && dist3(u, t) <= cfg_ad_dist * 2.0f) {
                char qi[160]; w->esc = 1; st_want_esc++;
                g_pend_ff = 1; pend_push(u, t, dist3(u, t), 0, 0, now); g_pend_ff = 0;
                if (cfg_log && g_polllines < 80) { g_polllines++; logf_("poll: [%u ms] unit %p melee weapon still not in hand %u ms after the draw (%s): fight order on %p anyway (escalations %u)", now, (void *)u, now - w->ms, queue_info(u, qi, sizeof qi), (void *)t, st_want_esc); }
            }
            continue;
        }
        const uint8_t *xt = explicit_target(u);
        float d = dist3(u, t);
        if (!fight_running(u, t) && (!xt || xt == t) && d <= cfg_ad_dist * 3.0f) {
            pend_push(u, t, d, 0, 0, now); st_want_fight++; st_fight_q++;
            if (cfg_log && g_polllines < 80) { g_polllines++; logf_("poll: [%u ms] unit %p melee weapon in hand: fight order on enemy %p at %.0f queued (wanted fights %u)", now, (void *)u, (void *)t, d, st_want_fight); }
        }
        if (s->blind_ms) { s->blind_ms = 0; melee_known(u); }
        ref_free(w->uref); ref_free(w->tref); w->used = 0;
    }
}
static void order_poll(const uint8_t *scanning, uint32_t now)
{
    if (!cfg_poll_on || !cfg_ad_on || (!g_vt_attack && !g_vt_attackb)) return;
    if (g_poll_last && (uint32_t)(now - g_poll_last) < cfg_poll_ms) return;
    g_poll_last = now ? now : 1;
    st_hk_polls++;
    want_poll(scanning, now);                                                   /* 0.7.22 */
    static uint8_t *live[NKNOWN];                                               /* 0.7.25: each unit resolved once per poll */
    for (int i = 0; i < NKNOWN; i++) {
        known_t *k = &g_known[i]; live[i] = NULL;
        if (!k->used) continue;
        uint8_t *v = (uint8_t *)ref_ent(k->ref);
        if (v != k->unit) { ref_free(k->ref); k->used = 0; continue; }          /* entity gone */
        if (!readable(v, 0xDB0)) { k->used = 0; continue; }                     /* 0.7.31: freed with its scene (ref not cleared) */
        live[i] = v;
    }
    for (int i = 0; i < NKNOWN; i++) {
        uint8_t *u = live[i];
        if (!u) continue;
        if (u == scanning || !(u[0x30] & 0x04) || is_linked(u) || in_manual(u)) continue;
        const uint8_t *o = (const uint8_t *)front_order(u);
        void **vt = o ? *(void **const *)o : NULL;
        /* 0.7.20 (0.7.19 test: no poll line for a unit on a right-click attack order): log every change of a unit's front order
         * class (RTTI name) with its target, and accept any class named *Attack* / *Fight* (not only eAttack / eAttackBasic) */
        const char *onm = vt_name(vt);
        int tty = o && readable(o + 0x48, 0x18) ? (int)*(const uint32_t *)(o + 0x48) : -1;
        uint8_t *tgt = tty == 1 ? *(uint8_t *const *)(o + 0x58) : NULL;
        if (tgt && !readable(tgt, 0xDB0)) tgt = NULL;
        {
            unit_t *ls = unit_of(u, 1);
            if (ls->ovt != (const void *)vt && cfg_log && g_ovtlines < 150) {
                g_ovtlines++;
                logf_("orders: [%u ms] unit %p front order now %s (vt exe+0x%llX)%s, target type %d %p%s, at %.0f",
                      now, (void *)u, onm ? onm : (o ? "?" : "none"), (unsigned long long)(vt && g_exe_lo && (uint8_t *)vt > g_exe_lo ? (uint8_t *)vt - g_exe_lo : 0),
                      vt == g_vt_attack ? " = eAttack" : vt == g_vt_attackb ? " = eAttackBasic" : vt == g_vt_fight ? " = eFight" : "",
                      tty, (void *)tgt, tgt && (tgt[0x30] & 0x04) ? " (human)" : "", tgt ? dist3(u, tgt) : -1.0f);
            }
            ls->ovt = (const void *)vt;
        }
        /* 0.7.24 fight-range stance (Alex: "put them in that stance when they are in fight range, then after there's no target
         * in fight range they go back to range stance"). The enemy is the unit's attack-order target when that is within
         * [AutoDraw] Distance, else the nearest known hostile human within it - for a unit that is idle, on an attack order, or
         * on one of the dll's own orders (a player / AI move order is left alone). In range: guns held off, melee weapon drawn,
         * then the fight order; nobody in range any more: the hold runs out after HoldMs and the unit shoots again. */
        if (cfg_down_on && vec_has(u + cfg_enttags, cfg_downtag, 1) == 1) continue;
        if (vt && vt == g_vt_fight) {                                           /* fighting: stay in the stance */
            unit_t *fs = unit_of(u, 1); fs->ad_until = now + cfg_ad_hold; if (!fs->ad_until) fs->ad_until = 1; fs->fr_ms = now;
            continue;
        }
        /* order classes in the 1.067 exes that attack a target (eAirAttack / eAttackProximityCharge / eSuppressionFire left out) */
        int attackish = vt == g_vt_attack || vt == g_vt_attackb
                     || (onm && (!strcmp(onm, ".?AUeAttack@Order@@") || !strcmp(onm, ".?AUeAttackBasic@Order@@")
                                 || !strcmp(onm, ".?AUeAttackBySelector@Order@@") || !strcmp(onm, ".?AUeWeaponFire@Order@@")
                                 || !strcmp(onm, ".?AUeWeaponAimTarget@Order@@")));
        int mine = 0; if (o) for (int j = 0; j < NMINE; j++) if (g_mine[j] == o) mine = 1;
        if (o && !attackish && !mine) { ranged_back(u, now, NULL); continue; }   /* moving / other order: not ours to interrupt (0.7.26: but the gun comes back) */
        uint8_t *otgt = (o && attackish && !mine && tgt && tgt != u && (tgt[0x30] & 0x04) && tgt[0xDA8] != u[0xDA8] && !linked_raw(tgt)
                         && !(cfg_down_on && vec_has(tgt + cfg_enttags, cfg_downtag, 1) == 1) && !ignored_target(tgt)) ? tgt : NULL;
        if (otgt) { hostile_set(u[0xDA8], otgt[0xDA8]); known_note(otgt, now); }
        float d = 1e9f; tgt = NULL;
        /* 0.7.27: in at Distance, out at Distance x LeaveFactor (only for a unit holding the melee weapon the dll drew) */
        unit_t *hs = unit_of(u, 0);
        float rng = cfg_ad_dist * ((cfg_leave > 1.0f && hs && hs->gun[0] && vec_has(u + cfg_enttags, g_ctagl, 1) == 1) ? cfg_leave : 1.0f);
        if (otgt) { float od = dist3(u, otgt); if (od <= rng) { tgt = otgt; d = od; } else if (!mine) { ranged_back(u, now, otgt); continue; } }   /* ordered onto a far enemy: his call, with his gun (0.7.26) */
        if (!tgt && cfg_prox_on) {
            for (int j = 0; j < NKNOWN; j++) {
                uint8_t *v = live[j];
                if (!v || j == i || !hostile_is(u[0xDA8], v[0xDA8]) || !(v[0x30] & 0x04) || linked_raw(v)) continue;
                float dd = dist3(u, v);
                if (dd > rng || dd >= d) continue;
                if (cfg_down_on && vec_has(v + cfg_enttags, cfg_downtag, 1) == 1) continue;
                if (ignored_target(v)) continue;                                /* 0.7.28 */
                tgt = v; d = dd;
            }
        }
        if (!tgt) { ranged_back(u, now, NULL); continue; }
        unit_of(u, 1)->fr_ms = now;
        if (!order_goes_melee(u, tgt, d, 1, tag_stance(u), holds_keep_weapon(u))) continue;
        unit_t *s = unit_of(u, 1);
        int inhand = vec_has(u + cfg_enttags, g_ctagl, 1) == 1;
        const char *what;
        if (inhand) {
            if (s->blind_ms) { s->blind_ms = 0; melee_known(u); }                 /* 0.7.19: the blind draw worked */
            s->ad_until = now + cfg_ad_hold; if (!s->ad_until) s->ad_until = 1;   /* 0.7.24: stance held while an enemy is in range */
            if (fight_running(u, tgt)) continue;
            last_t *fl = last_of(u, 0);
            if (fl && fl->ms && fl->tgt == tgt && (uint32_t)(now - fl->ms) < cfg_fight_cd) continue;
            pend_push(u, tgt, d, 0, 0, now); st_poll_fight++; st_fight_q++;
            what = "melee in hand: fight order queued";
        } else if (has_melee(u)) {
            s->ad_until = now + cfg_ad_hold; if (!s->ad_until) s->ad_until = 1;   /* guns refused if the engine does re-pick */
            want_add(u, tgt, now);                                                /* 0.7.22: fight order once it is in hand */
            if (!dpend_push(u, s, now)) continue;                                 /* in hand / queued / within DrawCooldownMs */
            st_poll_draw++;
            what = "carries melee: draw queued (then the fight order)";
        } else if (s->nomelee_until && (int32_t)(s->nomelee_until - now) > 0) {
            st_poll_unknown++;
            if (s->poll_ms && (uint32_t)(now - s->poll_ms) < 5000) continue;      /* log it once in a while only */
            what = "no melee weapon (probe / button / draw): keeps the gun";
        } else if (!s->blind_ms) {
            /* 0.7.19: nothing knows yet (never scanned, no toggle click this session): do what the toggle does - order the draw -
             * and look again after BlindCheckMs */
            if (!dpend_push(u, s, now)) continue;
            want_add(u, tgt, now);                                                /* 0.7.22 */
            s->blind_ord = otgt == tgt;
            s->blind_ms = now ? now : 1; st_poll_unknown++; st_poll_draw++;
            what = "melee unknown: draw tried like the toggle (checked again in BlindCheckMs)";
        } else {
            /* 0.7.26: over as soon as the take-item order has left the queue (1.2 s minimum), not only after BlindCheckMs */
            int taking = onm && !strcmp(onm, ".?AUeTakeItem@Order@@");
            if ((uint32_t)(now - s->blind_ms) < cfg_blind_ms && ((uint32_t)(now - s->blind_ms) < 1200 || taking)) continue;
            if (s->blind_ord) want_add_k(u, tgt, now, 1); else want_drop(u);       /* the draw dropped his attack order: give it back */
            s->blind_ord = 0;
            s->blind_ms = 0; s->nomelee_until = now + cfg_nomelee_ms; if (!s->nomelee_until) s->nomelee_until = 1;
            what = "draw tried, nothing in hand: no melee weapon, keeps the gun";
        }
        s->poll_ms = now ? now : 1;
        if (cfg_log && g_polllines < 80) {
            g_polllines++;
            logf_("poll: [%u ms] unit %p enemy %p in fight range at %.0f (%s): %s (fights %u, draws %u, unknown %u)",
                  now, (void *)u, (void *)tgt, d, otgt == tgt ? "his attack order's target" : "nearest hostile", what, st_poll_fight, st_poll_draw, st_poll_unknown);
        }
    }
}

/* 0.7.22 own tick. Alex's 0.7.21 heartbeat: ONE hook call in a whole session with a unit under an attack order - the engine
 * asks for a weapon so rarely there that nothing hanging off the hook (queue flushes, the order poll) ever ran. The dll now
 * ticks itself on the game's main thread: a WH_GETMESSAGE hook on the thread that owns the game window, woken by a posted
 * thread message every TickMs from the helper thread. The message pump runs between frames, the place the engine handles UI
 * clicks from (the melee toggle gives its take-item order from there). Interlock: the tick only acts on a thread the dll has
 * seen the engine use (the scan hook or a button click), and only while the sim clock advances. */
static int cfg_tick_on = 1;                /* [AutoDraw] Tick */
static uint32_t cfg_tick_ms = 100;         /* [AutoDraw] TickMs */
static volatile DWORD g_sim_tid, g_ui_tid;
static volatile LONG g_in_hook, g_in_tick, st_ticks, st_tick_wrongthread;
static DWORD g_tick_wall; static uint32_t g_tick_clock;
static int cfg_gate = 1;                   /* 0.7.35 [MeleeStance] ClientGate */
static volatile LONG st_tick_gated;
static volatile LONG64 st_tick_us;         /* 0.7.25: time spent in the tick's work, microseconds (heartbeat) */
static volatile LONG st_tick_us_max;
static void ms_tick(void)
{
    if (!cfg_tick_on || !g_clock || g_in_hook || g_in_tick) return;
    /* 0.7.35 client gate: the engine has not asked this process for one weapon pick = the simulation runs somewhere else (the
     * client of a hosted game: Alex's Nov 10 client log, hook calls 0, yet 19000 polls fed by 72000 order events cost 1.5 ms a
     * tick). Nothing the tick does can have an effect there. The first pick opens the gate for the rest of the session. */
    if (cfg_gate && !st_hk_all) { st_tick_gated++; return; }
    DWORD t = GetTickCount();
    if (t - g_tick_wall < cfg_tick_ms / 2) return;
    g_tick_wall = t;
    uint32_t now = *g_clock;
    if (!now || now == g_tick_clock) return;                    /* no mission running / paused */
    DWORD tid = GetCurrentThreadId();
    if (!g_sim_tid && !g_ui_tid) return;                        /* engine thread not seen yet */
    if (tid != g_sim_tid && tid != g_ui_tid) { st_tick_wrongthread++; return; }
    g_tick_clock = now;
    g_in_tick = 1; st_ticks++;
    LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0);
    log_epoch(now);
    if (cfg_charge && g_addorder && g_ordstamp) charge_flush(NULL);
    if ((cfg_ad_on || cfg_veh_on) && g_addorder && g_ordstamp) autodraw_flush(NULL);
    order_poll(NULL, now);
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
    LONG us = (LONG)((q1.QuadPart - q0.QuadPart) * 1000000 / qf.QuadPart);
    st_tick_us += us; if (us > st_tick_us_max) st_tick_us_max = us;
    g_in_tick = 0;
}
/* 0.7.23 frame hook. 0.7.22's message-pump tick never ran: the window thread (22432 in Alex's log) is not the engine thread
 * (32836: scan hook, button clicks and order events all arrive there). The engine thread is a worker loop (editor exe+0x93F80:
 * wait, `call [rax+8]`, signal, repeat) whose job is one frame (editor exe+0x40CAB0, the "main_loop" scopes), reached through a
 * vtable slot. That slot is found at run time from the engine thread's own stack, with no signature: the three outermost exe
 * return addresses are thread start / worker loop / frame function; the worker's must follow an indirect `call [reg+d8]`, and
 * the frame function's address must sit in exactly one pointer slot of the image with a vtable header before it. The slot is
 * then pointed at frame_hook: tick first (frame start, nothing being iterated), then the engine's function. */
typedef void *(__attribute__((ms_abi)) *frame_fn_t)(void *, void *, void *, void *);
static frame_fn_t g_frame_orig;
static volatile LONG g_frame_state;        /* 0 not tried, 1 hooked, 2 failed */
/* 0.7.24 export for the other GEM dlls (GEM Teleport 0.2.7 asked: it needs an engine-thread call every quant to notice the
 * jump cursor being cancelled): callbacks run once per quant on the engine thread, right after this dll's own tick, from the
 * quant hook (so only once that hook is installed: after the first enemy scan or order event of the session). Up to 4,
 * NULL / duplicates ignored, registration lock-free and allowed at any time. Callbacks must not give orders. */
typedef void (__attribute__((ms_abi)) *quant_cb_t)(void);
#define NQUANTCB 4
static void *volatile g_quant_cb[NQUANTCB];
__declspec(dllexport) __attribute__((ms_abi)) void gem_quant_listen(quant_cb_t cb)
{
    if (!cb) return;
    for (int i = 0; i < NQUANTCB; i++) if (g_quant_cb[i] == (void *)cb) return;
    for (int i = 0; i < NQUANTCB; i++)
        if (InterlockedCompareExchangePointer((void *volatile *)&g_quant_cb[i], (void *)cb, NULL) == NULL) {
            logf_("quant listeners: callback %p registered (slot %d of %d); live %s", (void *)cb, i + 1, NQUANTCB,
                  g_frame_state == 1 ? "now" : "once the quant hook is installed");
            return;
        }
}
static __attribute__((ms_abi, used, noinline)) void *frame_hook(void *a, void *b, void *c, void *d)
{
    ms_tick();
    for (int i = 0; i < NQUANTCB; i++) { quant_cb_t cb = (quant_cb_t)g_quant_cb[i]; if (cb) cb(); }
    return g_frame_orig(a, b, c, d);
}
static int ret_after_call(const uint8_t *r, int *indirect_d8)
{
    *indirect_d8 = 0;
    if (r[-5] == 0xE8) return 1;
    if (r[-6] == 0xFF && r[-5] == 0x15) return 1;
    if (r[-3] == 0xFF && (r[-2] & 0xF8) == 0x50) { *indirect_d8 = 1; return 1; }      /* call [reg+d8] */
    if (r[-2] == 0xFF && ((r[-1] & 0xF8) == 0xD0 || (r[-1] & 0xF8) == 0x10)) return 1;
    if (r[-6] == 0xFF && (r[-5] & 0xF8) == 0x90) return 1;
    return 0;
}
static void frame_hook_try(void)
{
    if (!cfg_tick_on || InterlockedCompareExchange(&g_frame_state, 2, 0) != 0) return;   /* once; 2 unless it succeeds */
    uint8_t *img = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    uint8_t *img_end = img + nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    uint8_t *tlo = NULL, *thi = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 5)) { tlo = img + sec[i].VirtualAddress; thi = tlo + sec[i].Misc.VirtualSize; }
    if (!tlo) { logf_("tick: no .text section, frame hook off"); return; }
    volatile uintptr_t marker = 0;
    uintptr_t *sp = (uintptr_t *)(((uintptr_t)&marker + 7) & ~(uintptr_t)7), *base = (uintptr_t *)__readgsqword(0x08);
    uint8_t *ret[3] = { 0, 0, 0 }; int ind[3] = { 0, 0, 0 };                              /* [0] outermost */
    for (uintptr_t *p = base - 1; p >= sp; p--) {                                          /* from the stack base inwards */
        uint8_t *v = (uint8_t *)*p; int d8;
        if (v < tlo + 8 || v >= thi) continue;
        DWORD64 ib; if (!RtlLookupFunctionEntry((DWORD64)v, &ib, NULL)) continue;
        if (!ret_after_call(v, &d8)) continue;
        int n = !ret[0] ? 0 : !ret[1] ? 1 : 2;
        ret[n] = v; ind[n] = d8;
        if (n == 2) break;
    }
    if (!ret[2]) { logf_("tick: engine thread stack has fewer than 3 exe frames, frame hook off"); return; }
    DWORD64 ib = 0; RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry((DWORD64)ret[2], &ib, NULL);
    uint8_t *fn = rf ? (uint8_t *)ib + rf->BeginAddress : NULL;
    if (!fn || !ind[1]) {
        logf_("tick: outer frames exe+0x%llX / exe+0x%llX / exe+0x%llX do not look like thread start / worker loop (indirect call %d) / frame function, frame hook off",
              (unsigned long long)(ret[0] - img), (unsigned long long)(ret[1] - img), (unsigned long long)(ret[2] - img), ind[1]);
        return;
    }
    uint8_t **slot = NULL; int hits = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        uint8_t **a = (uint8_t **)(img + sec[i].VirtualAddress), **e = a + sec[i].Misc.VirtualSize / 8;
        if (!readable(a, (size_t)((uint8_t *)e - (uint8_t *)a))) continue;
        for (; a < e; a++) if (*a == fn) { slot = a; hits++; }
    }
    if (hits != 1 || slot[-2] < img || slot[-2] >= img_end) {
        logf_("tick: frame function exe+0x%llX has %d pointer slots (need 1 with a vtable header), frame hook off", (unsigned long long)(fn - img), hits);
        return;
    }
    /* the slot's class must be the engine's quant executor (Scene::eQuantExecutor in all three 1.067 exes: vt[1] = run one quant) */
    const char *cls = vt_name((void **)(slot - 1));
    if (!cls || !strstr(cls, "QuantExecutor")) {
        logf_("tick: slot exe+0x%llX belongs to %s, not the quant executor, frame hook off", (unsigned long long)((uint8_t *)slot - img), cls ? cls : "an unnamed class");
        return;
    }
    DWORD old;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { logf_("tick: cannot write the vtable slot (%lu), frame hook off", GetLastError()); return; }
    g_frame_orig = (frame_fn_t)(void *)fn;
    *slot = (uint8_t *)(void *)frame_hook;
    VirtualProtect(slot, 8, old, &old);
    g_frame_state = 1;
    logf_("tick: %s run-quant function exe+0x%llX (worker loop exe+0x%llX, thread start exe+0x%llX), vtable slot exe+0x%llX hooked: the dll ticks at the start of each quant on engine thread %lu",
          cls, (unsigned long long)(fn - img), (unsigned long long)(ret[1] - img), (unsigned long long)(ret[0] - img),
          (unsigned long long)((uint8_t *)slot - img), GetCurrentThreadId());
}
/* order events from GEM Teleport's GiveOrder hook, when that dll exports gem_order_listen (0.2.3+): the unit is put on the poll
 * list the moment it is given any order and the next tick polls at once. Nothing is ordered from inside the callback. */
static volatile LONG st_order_events;
static void order_event(void *entity, void *order)
{
    (void)order;
    uint8_t *e = (uint8_t *)entity;
    if (!e || g_in_tick || g_in_hook || !g_clock || !cfg_poll_on) return;
    if (!readable(e, 0xDB0) || !(e[0x30] & 0x04) || is_linked(e)) return;
    if (!g_ui_tid) g_ui_tid = GetCurrentThreadId();
    frame_hook_try();                                           /* 0.7.23: first call on the engine thread finds the frame function */
    st_order_events++;
    if (cfg_gate && !st_hk_all) return;                         /* 0.7.35: see ms_tick (the quant hook above still goes in: other dlls' quant callbacks) */
    known_note(e, *g_clock);
    g_poll_last = 0;                                            /* poll on the next tick, not after PollMs */
}
/* 0.7.34 hook timing ([MeleeStance] HookTiming). The heartbeat timed the tick only; in Alex's Nov 10 hosted games the server
 * took 6700 MatchAmmo hook calls a second (24000 at the peak) and nothing said what one costs. Now: the dll's own time per
 * outermost hook call (QueryPerformanceCounter around the body, minus the time inside the engine's MatchAmmo it calls on),
 * the share of it spent in the order poll run from the scan, and the engine's share, all on the hb: line. Counter units
 * are kept raw and converted when the line is written. Cost: 4-6 counter reads a call (about 0.15 us). */
static int cfg_hooktime = 1;
static volatile LONG64 st_hook_qpc, st_orig_qpc, st_hpoll_qpc;
static volatile LONG st_hook_qpc_max, st_hook_timed;
static inline LONGLONG qpc_now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static void *orig_timed(uint8_t *ctx)
{
    if (!cfg_hooktime) return g_orig(ctx);
    LONGLONG a = qpc_now();
    void *r = g_orig(ctx);
    st_orig_qpc += qpc_now() - a;
    return r;
}
/* helper thread: wakes the main thread for the tick, connects to GEM Teleport, writes the heartbeat. No engine calls here. */
static DWORD WINAPI hb_thread(LPVOID p)
{
    (void)p;
    LONG last = -1, lastt = -1; int lines = 0, listen = 0, tries = 0; DWORD hbwall = GetTickCount();
    for (;;) {
        Sleep(100);
        if (!listen && tries < 600 && (++tries % 10) == 0) {    /* teleport loads after this dll: look for it for a minute */
            HMODULE m = GetModuleHandleA("gem_teleport.dll");
            void (*reg)(void (*)(void *, void *)) = m ? (void (*)(void (*)(void *, void *)))(void *)GetProcAddress(m, "gem_order_listen") : NULL;
            if (reg) { reg(order_event); listen = 1; logf_("orders: listening to GEM Teleport's GiveOrder hook (gem_order_listen)"); }
            else if (tries == 600) logf_("orders: no gem_order_listen export in gem_teleport.dll (0.2.3+): order poll only");
        }
        if (GetTickCount() - hbwall < 5000) continue;
        hbwall = GetTickCount();
        LONG all = st_hk_all, tk = st_ticks;
        if ((all == last && tk == lastt) || lines >= 400) continue;
        last = all; lastt = tk; lines++;
        int known = 0; for (int i = 0; i < NKNOWN; i++) if (g_known[i].used) known++;
        LONG mx = InterlockedExchange(&st_tick_us_max, 0);
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        LONG hmx = InterlockedExchange(&st_hook_qpc_max, 0), hn = st_hook_timed;
        long long hq = st_hook_qpc, oq = st_orig_qpc, pq = st_hpoll_qpc, f = qf.QuadPart ? qf.QuadPart : 1;
        logf_("hb: [%u ms] hook calls %ld (scan %ld, humans %ld, human vs enemy human %ld, fast pass %ld) | frame hook %s, ticks %ld (gated %ld, wrong thread %ld; sim %lu ui %lu) polls %ld, order events %ld, units on the poll list %d | tick cost: %lld us total, %ld us average, %ld us worst since the last line"
              " | hook cost: %lld us total over %ld timed calls (%lld us of it the order poll), %lld ns average, %lld us worst since the last line; engine MatchAmmo %lld us total",
              g_clock ? *g_clock : 0, all, (LONG)st_hk_scan, (LONG)st_hk_human, (LONG)st_hk_enemy, (LONG)st_fast,
              g_frame_state == 1 ? "ON" : g_frame_state == 2 ? "FAILED" : "not tried yet", tk, (LONG)st_tick_gated, (LONG)st_tick_wrongthread,
              (DWORD)g_sim_tid, (DWORD)g_ui_tid, (LONG)st_hk_polls, (LONG)st_order_events, known,
              (long long)st_tick_us, tk ? (LONG)(st_tick_us / tk) : 0, mx,
              hq * 1000000 / f, hn, pq * 1000000 / f, hn ? hq * 1000000000 / f / hn : 0, (long long)hmx * 1000000 / f, oq * 1000000 / f);
    }
    return 0;
}
static void hook_leave(int *x) { (void)x; g_in_hook--; }
/* 0.7.35 fast pass. Most units in a match carry no melee weapon, and for them every branch below ends in "engine's choice":
 * the probe / button already said so (nomelee_until, NoMeleeMs) and nothing is pending. Such a pick goes to the engine after
 * the checks that concern the TARGET (ignored / downed) and the known-units upkeep (he is still somebody's enemy); scan_note,
 * the order-pick path, the direct-control test and ms_decide are skipped. A melee ammo candidate is never fast: that is how a
 * unit that picked a sword up is noticed (melee_known clears nomelee_until). [MeleeStance] FastPass */
static int fast_nomelee(const uint8_t *ctx)
{
    if (!cfg_kcache || !cfg_enabled || !g_clock) return 0;
    const uint8_t *ammo = *(const uint8_t *const *)ctx, *ent = *(const uint8_t *const *)(ctx + 8);
    if (!ammo || !ent || !(ent[0x30] & 0x04)) return 0;
    unit_t *s = unit_of(ent, 0);
    uint32_t now = *g_clock;
    if (!s || !s->nomelee_until || (int32_t)(s->nomelee_until - now) <= 0) return 0;
    if (s->probe_ms || s->blind_ms) return 0;
    if (s->ad_until && (int32_t)(s->ad_until - now) > 0) return 0;
    if (s->melee_until && (int32_t)(s->melee_until - now) > 0) return 0;
    if (vec_has(ammo + cfg_ammotags + TAGS_VEC, cfg_mtags, n_mtags) != 0) return 0;
    return 1;
}

static void *ms_hook_body(uint8_t *ctx, uint8_t *ret);
__attribute__((used, ms_abi, noinline)) void *ms_hook(uint8_t *ctx)
{
    uint8_t *ret = (uint8_t *)__builtin_return_address(0);
    if (!cfg_hooktime || g_in_hook) return ms_hook_body(ctx, ret);          /* a nested call is inside the outer one's time */
    LONGLONG a = qpc_now(); LONG64 o0 = st_orig_qpc;
    void *r = ms_hook_body(ctx, ret);
    LONGLONG own = (qpc_now() - a) - (LONGLONG)(st_orig_qpc - o0);          /* the dll's part: not the engine's MatchAmmo */
    if (own < 0) own = 0;
    st_hook_qpc += own; st_hook_timed++;
    if (own > st_hook_qpc_max) st_hook_qpc_max = own > 0x7FFFFFFF ? 0x7FFFFFFF : (LONG)own;
    return r;
}
static __attribute__((noinline)) void *ms_hook_body(uint8_t *ctx, uint8_t *ret)
{
    int from_scan = g_clock && g_scan_lo && ret >= g_scan_lo && ret < g_scan_hi;
    g_in_hook++; int hook_guard __attribute__((cleanup(hook_leave), unused)) = 0;   /* 0.7.22: no tick inside the hook */
    if (from_scan && !g_sim_tid) g_sim_tid = GetCurrentThreadId();
    if (from_scan) frame_hook_try();                           /* 0.7.23 */
    if (g_clock) log_epoch(*g_clock);                          /* 0.7.35: before any per-unit state is read (was in scan_note only) */
    /* 0.7.35: with the quant hook in, ms_tick() flushes both queues and runs the order poll at the start of every quant, so the
     * hook no longer walks them on every pick (64 + 32 slots, each a weak-ref check); without it (hook not found) as before */
    int ticking = g_frame_state == 1 && cfg_tick_on;
    int fast = fast_nomelee(ctx);
    {   /* 0.7.28: a target the dll must ignore (hidden / melee_disabled / call-ins) -> the engine picks on its own */
        const uint8_t *iti = *(const uint8_t *const *)(ctx + 0x10);
        const uint8_t *itg = iti ? *(const uint8_t *const *)(iti + 0x18) : NULL;
        if (itg && (itg[0x30] & 0x02) && ignored_target(itg)) { st_hk_all++; st_tgt_ignored++; return orig_timed(ctx); }
    }
    if (from_scan && cfg_charge && !ticking) charge_flush(*(uint8_t **)(ctx + 8));
    if (from_scan && (cfg_ad_on || cfg_veh_on)) { if (!ticking) autodraw_flush(*(uint8_t **)(ctx + 8)); if (!fast) scan_note(ctx); }
    st_hk_all++; if (from_scan) st_hk_scan++;                  /* 0.7.21 heartbeat counters */
    {
        const uint8_t *he = *(const uint8_t *const *)(ctx + 8), *hti = *(const uint8_t *const *)(ctx + 0x10);
        if (he && (he[0x30] & 0x04)) {
            st_hk_human++;
            const uint8_t *ht = hti ? *(const uint8_t *const *)(hti + 0x18) : NULL;
            if (ht && (ht[0x30] & 0x04) && ht[0xDA8] != he[0xDA8]) st_hk_enemy++;
        }
    }
    if (from_scan && g_clock) {                                /* 0.7.18: remember the scanning unit; poll the ordered ones */
        uint8_t *su = *(uint8_t **)(ctx + 8);
        if (su && (su[0x30] & 0x04) && cfg_poll_on) known_note(su, *g_clock);
        const uint8_t *sti = *(const uint8_t *const *)(ctx + 0x10);              /* 0.7.19: the scan's target too - a unit on an */
        uint8_t *st = sti ? *(uint8_t *const *)(sti + 0x18) : NULL;              /* order never scans, but the enemy's scans see it */
        if (st && st != su && (st[0x30] & 0x04) && !linked_raw(st) && cfg_poll_on) known_note(st, *g_clock);
        if (su && st && st != su && (su[0x30] & 0x04) && (st[0x30] & 0x04)) hostile_set(su[0xDA8], st[0xDA8]);   /* 0.7.24: an enemy scan's target is hostile */
        if (ticking) { }                                       /* 0.7.35: the tick polls */
        else if (cfg_hooktime) { LONGLONG pa = qpc_now(); order_poll(su, *g_clock); st_hpoll_qpc += qpc_now() - pa; }
        else order_poll(su, *g_clock);
    }
    if (!from_scan && cfg_log && g_rawlines < 40) {            /* 0.7.18: what reaches the hook from outside the scan, unfiltered */
        g_rawlines++;
        const uint8_t *ti = *(const uint8_t *const *)(ctx + 0x10);
        logf_("raw: [%u ms] hook call from exe+0x%llX: unit %p ti %p target %p", g_clock ? *g_clock : 0,
              (unsigned long long)(g_exe_lo && ret > g_exe_lo ? ret - g_exe_lo : 0), *(void **)(ctx + 8), (const void *)ti,
              ti ? *(void *const *)(ti + 0x18) : NULL);
    }
    if (!from_scan && g_clock && cfg_poll_on) {                /* 0.7.19: a unit seen picking a weapon for an order is polled too */
        uint8_t *ou = *(uint8_t **)(ctx + 8);
        if (ou && (ou[0x30] & 0x04) && !is_linked(ou)) known_note(ou, *g_clock);
    }
    int ord = !fast && !from_scan && order_pick(ctx, ret);     /* 0.7.17: an order's pick in fight range -> same path as the scan */
    if (ord) scan_note(ctx);
    if (cfg_down_on) {                                        /* downed (personage) target: no weapon is usable against it */
        const uint8_t *ti = *(const uint8_t *const *)(ctx + 0x10);
        const uint8_t *tg = ti ? *(const uint8_t *const *)(ti + 0x18) : NULL;
        if (tg && (tg[0x30] & 0x04) && vec_has(tg + cfg_enttags, cfg_downtag, 1) == 1) { st_down++; return NULL; }
    }
    if (fast) { st_fast++; return orig_timed(ctx); }           /* 0.7.35 */
    uint8_t ready = ctx[0x18];
    int d = ms_decide(ctx);
    void *r;
    if (d == REJECT) {
        st_reject++;
        if (cfg_log && g_lines < 40) {
            g_lines++;
            char nm[200];
            vec_str(*(uint8_t **)ctx + cfg_namtags + TAGS_VEC, nm, sizeof nm);
            logf_("stance: unit %p refused \"%s\" (refused %u, melee picks %u)", *(void **)(ctx + 8), nm, st_reject, st_allow);
        }
        return NULL;
    }
    if (d == MELEE) {
        if (cfg_lift_ready) ctx[0x18] = 1;
        float saved[3]; int shifted = adj_shift(ctx, saved);
        r = orig_timed(ctx);
        if (shifted) { float *pt = (float *)(*(uint8_t **)(ctx + 0x10) + 0x28); pt[0] = saved[0]; pt[1] = saved[1]; pt[2] = saved[2]; }
        ctx[0x18] = ready;
        if (r) {
            st_allow++;
            if (cfg_log && g_lines < 40) {
                g_lines++;
                char nm[200];
                vec_str(*(uint8_t **)ctx + cfg_namtags + TAGS_VEC, nm, sizeof nm);
                logf_("stance: unit %p takes \"%s\" (refused %u, melee picks %u)", *(void **)(ctx + 8), nm, st_reject, st_allow);
            }
        }
    } else r = orig_timed(ctx);
    if ((from_scan || ord) && cfg_charge && r) charge_note(ctx, ready != 0, d == MELEE);
    return r;
}

/* ---------------------------------------------------------------- instant buttons on several units */
/* Vanilla runs an {instant} button only when exactly one selected unit can use it (GetActor). For actions with
 * {apply_to_all}, when GetActor finds none/several, run the vanilla ExecuteInstant once per eligible unit with
 * GetActor answering that unit (the action's vptr points at a copy of its vtable for the call), so each unit gets
 * exactly what a single-unit click gives it. */
typedef void  (*exinst_t)(void *action, void *list);
typedef void *(*getactor_t)(void *action, void *list);
typedef void *(*expand_t)(void *out, void *list);
typedef uint8_t (*canapply_t)(void *action, void *unit, void *zero);
static int cfg_multi = 1;
static exinst_t g_exinst;
static uint32_t g_off_apply, g_off_getactor, g_off_canapply;
static expand_t g_expand;
static void *g_cur_actor;
static uint32_t st_multi_clicks, st_multi_units;
#define VTCOPY 64
static struct { void **orig; void **copy; } g_vtc[16];

__attribute__((used, ms_abi)) void *ms_getactor_cur(void *action, void *list) { (void)action; (void)list; return g_cur_actor; }

static void **vt_copy(void **vt)
{
    for (int i = 0; i < 16; i++) if (g_vtc[i].orig == vt) return g_vtc[i].copy;
    for (int i = 0; i < 16; i++)
        if (!g_vtc[i].orig) {
            void **c = (void **)VirtualAlloc(NULL, VTCOPY * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!c) return NULL;
            memcpy(c, vt, VTCOPY * 8);
            c[g_off_getactor / 8] = (void *)ms_getactor_cur;
            g_vtc[i].orig = vt; g_vtc[i].copy = c;
            return c;
        }
    return NULL;
}

/* Draw the melee weapon when the stance turns ON: melee_toggle.set has {item "melee"} {item_in_box} and
 * {item "melee"}; for a unit being switched on (not tagged with the stance tags) that does not hold a melee weapon (no
 * charge tag), the dll gives it the engine's take-item order for the action's {item} tags (Order::eTakeItem, the order
 * `{take_item}` builds: the held item already matches = done, else the first matching inventory item goes TakeToRight)
 * + AddOrder(unit, order, {1, stamp}). 0.6.3 ran the action's {instructions} through CreateOrder instead, but
 * eCustomActionOrder first checks the click target (action vt+0x58: no {target} = fails) and silently drops itself.
 * Button name = std::string at action+0x60 (confirmed in game). */
typedef void *(*pickctor_t)(void *buf);
static pickctor_t g_pickctor;
static uint32_t g_off_item;
/* ---------------------------------------------------------------- 0.7.1 reload cancel */
/* Order::eTakeItem::Update (game exe+0x8EA920) keeps waiting while Human::InventoryBusy (exe+0x7CCEB0: the human's inventory
 * op list [ops+0x80..0x88] is not empty); a running reload is such an op (the magazine transfer, eInventoryTransferCommand),
 * so a melee draw given mid-reload stood until the reload ended (Alex: "they get stuck holding the weapon"). Before a draw
 * the dll calls Human::CancelInventoryOps (exe+0xB67C30: brain event 0x707, then Inventory::CancelAll exe+0x7CABB0 on
 * Human::InvOps exe+0x7CCD50; the host sends eInventoryTransferCancelCommand) unless the weapon in hand (GetCurrentWeapon
 * exe+0x806340 on the holder at human+0xDD8, params +0x98, tags +0xEC8) carries one of [ReloadCancel] KeepTags. */
typedef int   (*invbusy_t)(const void *human);
typedef void  (*invcancel_t)(void *unused, void *human);
typedef void *(*curweap_t)(void *holder);
static invbusy_t   g_invbusy;
static invcancel_t g_invcancel;
static curweap_t   g_curweap;
static int  cfg_rc_on = 1, n_keep = 12, g_rclines;
static char cfg_keep[MAXT][TLEN] = { "mgun", "hmg_shell", "stubber", "lascannon", "plasmacannon", "sm_weapon_plasmacannon",
                                     "sm_weapon_heavybolter", "rocketlauncher", "heavy_flamethrower", "grenadelauncher", "at_rifle", "ogryn_weapon" };
static uint32_t st_rc_cancel, st_rc_keep;

static int reload_cancel(uint8_t *u)
{
    if (!cfg_rc_on || !g_invbusy || !g_invcancel || !u || !(u[0x30] & 0x04)) return 0;
    if (!g_invbusy(u)) return 0;
    const uint8_t *w = g_curweap ? (const uint8_t *)g_curweap(*(void **)(u + 0xDD8)) : NULL;
    const uint8_t *prm = w ? *(const uint8_t *const *)(w + 0x98) : NULL;
    /* 0.7.5 gated the cancel on the weapon's reload record (weapon+0xB0, +0x10 start clock != 0) because InventoryBusy was true
     * for every toggled unit at once (0.7.4 log). Wrong: 0.7.5 log, a reloading unit was "left alone" four times and never drew
     * (eWeaponPreload::Update also waits on the record's +0x14 flag bits 0x14, which that gate ignored). 0.7.7: cancel whenever
     * the inventory is busy, as 0.7.4 did (swaps worked), and only LOG what the record says (flags / start clock) for the next read. */
    const uint8_t *rb = w ? *(const uint8_t *const *)(w + 0xB0) : NULL;
    uint32_t rflags = rb ? rb[0x14] : 0, rstart = rb ? *(const uint32_t *)(rb + 0x10) : 0;
    if (!g_rc_force && prm && n_keep && vec_has(prm + cfg_ammotags + TAGS_VEC, cfg_keep, n_keep) == 1) {
        st_rc_keep++;
        if (cfg_log && g_rclines < 40) { g_rclines++; logf_("reload: [%u ms] unit %p keeps its reload (held weapon has a KeepTags tag) (cancelled %u, kept %u)", now_ms(), (void *)u, st_rc_cancel, st_rc_keep); }
        return 0;
    }
    g_invcancel(NULL, u);
    st_rc_cancel++;
    if (cfg_log && g_rclines < 40) { g_rclines++; logf_("reload: [%u ms] unit %p inventory ops cancelled before the melee draw (weapon reload record: flags 0x%02X start %u) (cancelled %u, kept %u)", now_ms(), (void *)u, rflags, rstart, st_rc_cancel, st_rc_keep); }
    return 1;
}
/* 0.7.26: name words of the weapon in hand (its stuff name Tags, like an ammo's): the take-item {item} words that bring it back */
static int held_weapon_name(uint8_t *u, char *out, size_t cap)
{
    out[0] = 0;
    if (!g_curweap || !u || !*(void **)(u + 0xDD8)) return 0;
    const uint8_t *w = (const uint8_t *)g_curweap(*(void **)(u + 0xDD8));
    const uint8_t *prm = w ? *(const uint8_t *const *)(w + 0x98) : NULL;
    if (!prm || !readable(prm + cfg_namtags + TAGS_VEC, 16)) return 0;
    vec_str(prm + cfg_namtags + TAGS_VEC, out, cap);
    return out[0] != 0;
}
/* item tags for a weapon name, built once per name with the engine's TagContainer (as melee_tags) and kept for the session */
#define NGUNTC 32
static struct { char name[72]; uint8_t *tc; } g_guntc[NGUNTC];
static int n_guntc;
static void *gun_tags(const char *name)
{
    for (int i = 0; i < n_guntc; i++) if (!strcmp(g_guntc[i].name, name)) return g_guntc[i].tc;
    if (n_guntc >= NGUNTC || !g_tcctor || !g_tcfromstr || !name[0]) return NULL;
    uint8_t *tc = (uint8_t *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tc) return NULL;
    snprintf(g_guntc[n_guntc].name, sizeof g_guntc[n_guntc].name, "%s", name);
    const char *keep = g_guntc[n_guntc].name; size_t n = strlen(keep);
    g_tcctor(tc);
    __attribute__((aligned(16))) uint8_t str[0x20];                 /* MSVC std::string: inline up to 15 chars, else a pointer */
    memset(str, 0, sizeof str);
    if (n < 16) { memcpy(str, keep, n); *(uint64_t *)(str + 0x18) = 15; }
    else { *(const char **)str = keep; *(uint64_t *)(str + 0x18) = n; }
    *(uint64_t *)(str + 0x10) = n;
    g_tcfromstr(tc, str, ' ');
    char got[200]; vec_str(tc + TAGS_VEC, got, sizeof got);
    if (cfg_log) logf_("ranged: item tags built for \"%s\": \"%s\"", keep, got);
    g_guntc[n_guntc].tc = got[0] ? tc : NULL;
    return g_guntc[n_guntc++].tc;
}
/* 0.7.26 back to the gun (Alex's 0.7.25 test: after the fight he ordered an attack on enemies 8-18 m away and the unit ran at
 * them with the knife - an attack order uses whatever is in hand). A unit whose melee weapon the dll drew, not in melee
 * stance, with nobody in fight range, gets a take-item order for the gun it held before: at once when it has an attack order
 * on a far enemy (atk_tgt; that order is dropped so the take-item runs first, and given back when the knife is away), else
 * HoldMs after the last enemy in range. 1 = ordered. */
static int ranged_back(uint8_t *u, uint32_t now, uint8_t *atk_tgt)
{
    unit_t *s = unit_of(u, 0);
    if (!s || !s->gun[0] || tag_stance(u)) return 0;
    if (vec_has(u + cfg_enttags, g_ctagl, 1) != 1) {                             /* melee weapon not in hand */
        if (!auto_active(u) && (uint32_t)(now - s->ad_draw_ms) > 5000) s->gun[0] = 0;   /* and no draw running: he is on a gun again */
        return 0;
    }
    if (!atk_tgt && (uint32_t)(now - s->fr_ms) < cfg_ad_hold) return 0;         /* an enemy was in range a moment ago */
    if (s->gun_ms && (uint32_t)(now - s->gun_ms) < 2500) return 0;              /* one order per swap */
    st_gun_try++;
    void *tags = g_takeitem ? gun_tags(s->gun) : NULL;
    if (!tags) return 0;
    s->gun_ms = now ? now : 1; s->ad_until = 0; s->auto_drawn = 0;
    if (atk_tgt) { if (g_invcancel) g_invcancel(NULL, u); want_add_k(u, atk_tgt, now, 1); }
    uint32_t params[2] = { 1, *g_ordstamp };
    void *o = g_takeitem(tags), *ok = o ? g_addorder(u, o, params) : NULL;
    if (ok) st_gun_draw++;
    if (cfg_log && g_rblines < 60) { char qi[160]; g_rblines++; logf_("ranged: [%u ms] unit %p nobody in fight range%s: take-item order for \"%s\" %s | unit now has %s (re-draws %u)", now, (void *)u, atk_tgt ? " and an attack order on a far enemy" : "", s->gun, ok ? "added" : "REFUSED", queue_info(u, qi, sizeof qi), st_gun_draw); }
    return ok != NULL;
}
/* 0.7.17: the weapon in hand carries a [ReloadCancel] KeepTags tag (heavy weapon) */
static int holds_keep_weapon(uint8_t *u)
{
    if (!g_curweap || !n_keep || !u || !*(void **)(u + 0xDD8)) return 0;
    const uint8_t *w = (const uint8_t *)g_curweap(*(void **)(u + 0xDD8));
    const uint8_t *prm = w ? *(const uint8_t *const *)(w + 0x98) : NULL;
    return prm && vec_has(prm + cfg_ammotags + TAGS_VEC, cfg_keep, n_keep) == 1;
}

static char cfg_drawbtn[TLEN] = "melee_toggle";
static int cfg_draw = 1, g_names_logged, g_dlines;
/* 0.7.13: every log cap counted per game process; Alex's Nov 8 log spent all 200 charge + 200 held lines in the first mission and the
 * later ones (the "units aren't charging" test) left nothing. The caps now restart when the sim clock goes backwards (a new mission). */
static uint32_t g_epoch_last;
/* 0.7.31: every per-mission unit table is wiped when a new mission starts. Alex's Nov 9 3v3: the dedicated server crashed one
 * second after "Exit game mode" (ACCESS_VIOLATION in order_poll's nearest-enemy loop, +0x5ce3 in 0.7.30): the battle scene is
 * freed as a whole, the engine never clears our weak refs then (ref+8 stays 1), so 6 dead units were still "live" on the poll
 * list. Plain memset, no ref_free: the refs' entities and ref lists went with the old scene. */
static void mission_flush(void)
{
    memset(g_seen, 0, sizeof g_seen);
    memset(g_unit, 0, sizeof g_unit);
    memset(g_pend, 0, sizeof g_pend);
    memset(g_last, 0, sizeof g_last);
    memset(g_dpend, 0, sizeof g_dpend);
    memset(g_known, 0, sizeof g_known);
    memset(g_kcache, 0, sizeof g_kcache);
    memset(g_want, 0, sizeof g_want);
    memset(g_mine, 0, sizeof g_mine);
    memset(g_hostile, 0, sizeof g_hostile);
    g_poll_last = 0;
}
static void log_epoch(uint32_t now)
{
    if (g_epoch_last && now + 5000 < g_epoch_last) {
        mission_flush();
        g_lines = g_clines = g_hlines = g_vlines = g_vreflines = g_adjlines = g_adlines = g_rclines = g_dlines = g_mlines = g_mdiag = g_seenlines = g_addiag = g_plines = g_skiplines = g_olines = g_rawlines = g_polllines = g_ovtlines = g_rblines = g_reflines = 0;
        if (cfg_log) logf_("--- clock %u -> %u: new mission, log line caps reset, unit tables cleared", g_epoch_last, now);
    }
    g_epoch_last = now;
}
static uint8_t *g_draw_action;                 /* the melee_toggle action, remembered from its first click (actions live for the whole session) */
static uint32_t st_draws, st_draw_drop, st_draw_wait;
#define OFF_BUTTONNAME  0x60

static int is_draw_action(uint8_t *action)
{
    if (!cfg_draw || !g_takeitem || !g_ordstamp || !g_addorder) return 0;
    size_t n; const char *s = str_get(action + OFF_BUTTONNAME, &n);
    if (s && eq_ci(s, n, cfg_drawbtn)) {
        if (cfg_log && !g_names_logged) { g_names_logged = 1; logf_("draw: toggle action %p found (button \"%.*s\", {item} tags +0x%X)", (void *)action, (int)n, s, g_off_item); }
        g_draw_action = action; return 1;
    }
    return 0;
}
/* 0.7.19: 1 = the melee_toggle button applies to this unit (it carries a melee item), 0 = it does not, -1 = unknown (the action is
 * only known after its first click in the session, or the CanApply slot was not found) */
static int melee_by_button(const void *u)
{
    if (!g_draw_action || !g_off_canapply || !u) return -1;
    void **vt = *(void ***)g_draw_action;
    if (!vt) return -1;
    canapply_t can = (canapply_t)vt[g_off_canapply / 8];
    return can(g_draw_action, (void *)u, NULL) ? 1 : 0;
}
static int wants_draw(const uint8_t *u)
{
    return u && (u[0x30] & 0x04) && vec_has(u + cfg_enttags, g_tagl, n_tagl) != 1 && vec_has(u + cfg_enttags, g_ctagl, 1) != 1;
}
/* 1 = a take-item order was added, 0 = skipped (linked, cooldown) or refused. Re-adding the order restarts the take
 * (stage 0 clears the running take op), so a unit gets at most one per DrawCooldownMs. */
static int draw_melee(uint8_t *action, void **vt, uint8_t *u)
{
    (void)vt;
    if (!g_takeitem || !g_addorder || !u || is_linked(u)) return 0;
    /* 0.7.26: a unit another dll is driving (SkipTags, e.g. mid jetpack jump): no draw now. The reload cancel below drops the
     * unit's orders (CancelInventoryOps = brain vt+0x98, mask 0x707), which would kill the running jump order. */
    if (n_skiptags && vec_has(u + cfg_enttags, cfg_skiptags, n_skiptags) == 1) return 0;
    uint32_t now = g_clock ? *g_clock : 0;
    last_t *l = now ? last_of(u, 1) : NULL;
    if (l && l->draw_ms && (uint32_t)(now - l->draw_ms) < cfg_draw_cd) { st_draw_wait++; return 0; }
    reload_cancel(u);                                        /* 0.7.1: a running reload would hold the take-item order */
    void *o = g_takeitem(action + g_off_item);
    uint32_t params[2] = { 1, *g_ordstamp };
    void *ok = o ? g_addorder(u, o, params) : NULL;
    if (ok) { st_draws++; if (l) l->draw_ms = now; } else st_draw_drop++;
    if (cfg_log && g_dlines < 40) {
        g_dlines++;
        logf_("draw: [%u ms] unit %p take-item order %p %s (draws %u, refused %u, waited %u)", now, (void *)u, o, ok ? "added" : "REFUSED by AddOrder", st_draws, st_draw_drop, st_draw_wait);
    }
    return ok != NULL;
}

__attribute__((used, ms_abi)) void ms_exinst(uint8_t *action, void *list)
{
    if (!g_ui_tid) g_ui_tid = GetCurrentThreadId();           /* 0.7.22: the thread the engine runs button clicks on */
    if (!cfg_multi || !action || !list || !action[g_off_apply]) { g_exinst(action, list); return; }
    void **vt = *(void ***)action;
    int drawact = is_draw_action(action);
    uint8_t *one = (uint8_t *)((getactor_t)vt[g_off_getactor / 8])(action, list);
    if (one) {                                                 /* vanilla single-unit case */
        int d = drawact && wants_draw(one);
        g_exinst(action, list);
        if (d) draw_melee(action, vt, one);
        return;
    }
    struct { void **b, **e, **c; } units = { 0, 0, 0 };
    g_expand(&units, list);                                    /* selection, squad card expanded (vector leaks, a few bytes per click) */
    if (!units.b || units.e <= units.b || units.e - units.b > 256) { g_exinst(action, list); return; }
    void **cp = vt_copy(vt);
    if (!cp) { g_exinst(action, list); return; }
    canapply_t can = (canapply_t)vt[g_off_canapply / 8];
    int n = 0;
    for (void **u = units.b; u < units.e; u++) {
        if (!*u || !can(action, *u, NULL)) continue;
        int d = drawact && wants_draw((uint8_t *)*u);
        g_cur_actor = *u;
        *(void ***)action = cp;
        g_exinst(action, list);
        *(void ***)action = vt;
        if (d) draw_melee(action, vt, (uint8_t *)*u);
        n++;
    }
    g_cur_actor = NULL;
    st_multi_clicks++; st_multi_units += n;
    if (cfg_log && g_lines < 40) {
        g_lines++;
        logf_("instant: button on %d of %d selected units (clicks %u, units %u)", n, (int)(units.e - units.b), st_multi_clicks, st_multi_units);
    }
}

/* ---------------------------------------------------------------- melee through a linked shield */
/* Before a paired blow starts, the engine casts a ray attacker -> target (query 0x6003, the bullet volumes) into a
 * global hit list (0x48-byte records sorted by distance, entity at +0x18) and drops the blow unless the FIRST hit
 * is the target (blow picker, game exe+0x95E5C2). A forcefield is its own entity linked to the wearer, so it is hit
 * first and the wearer can never be meleed. ms_ray_ok: walk the hits in order, skip entities linked to the target
 * (its shield, its kit) or carrying <SeeThroughTag>, then the target must come next; anything else still blocks. */
static int cfg_ray = 0;          /* off by default since 0.6.1 (Alex: 0.6 behaviour); [ShieldMelee] Enabled=1 turns the 0.7 ray patch back on */
static char cfg_seetag[1][TLEN] = { "forcefield" };
static uint8_t *g_ray_hits;
static uint32_t st_ray_pass, st_ray_skip, st_ray_block;

static const uint8_t *link_holder(const uint8_t *e)
{
    if (!((*(const uint32_t *)(e + cfg_linkoff) >> cfg_linkbit) & 1)) return NULL;
    const uint8_t *lk = *(const uint8_t *const *)(e + 0x560);
    return lk ? *(const uint8_t *const *)(lk + 8) : NULL;
}

__attribute__((used, ms_abi, force_align_arg_pointer)) int ms_ray_ok(const uint8_t *target)
{
    const uint8_t *b = *(const uint8_t *const *)g_ray_hits, *e = *(const uint8_t *const *)(g_ray_hits + 8);
    if (!b || e <= b || (uintptr_t)(e - b) % 0x48 || (uintptr_t)(e - b) > 0x48 * 256) return 0;
    if (*(const uint8_t *const *)(b + 0x18) == target) { st_ray_pass++; return 1; }   /* vanilla pass */
    if (!cfg_ray || !target) return 0;
    int skipped = 0;
    for (const uint8_t *r = b; r < e; r += 0x48) {
        const uint8_t *ent = *(const uint8_t *const *)(r + 0x18);
        if (ent == target) {
            st_ray_skip++;
            if (cfg_log && g_lines < 40) { g_lines++; logf_("melee ray: target %p reached through %d attached hit(s) (passed %u, through shield %u, blocked %u)", (void *)target, skipped, st_ray_pass, st_ray_skip, st_ray_block); }
            return 1;
        }
        if (!ent) break;
        const uint8_t *h = link_holder(ent);
        int tagged = vec_has(ent + cfg_enttags, cfg_seetag, 1) == 1;
        if (h == target || tagged) { skipped++; continue; }
        if (cfg_log && g_lines < 40) { g_lines++; logf_("melee ray: target %p blocked by %p (linked to %p, tag %d)", (void *)target, (void *)ent, (void *)h, tagged); }
        break;
    }
    st_ray_block++;
    return 0;
}

/* ---------------------------------------------------------------- finders */
#define W 0x100
/* body: movzx e?x,byte[r?+394h]; test ?l,1Ch  (row flags: enable_for_ai/user/human_user) */
static const uint16_t SIG_FLAGS[] = { 0x0F, 0xB6, W, 0x94, 0x03, 0x00, 0x00, 0xF6, W, 0x1C };
/* prologue: mov [rsp+20h],rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14; push r15 */
static const uint8_t PROLOG[] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
#define PATCH 14       /* through push r14 */
/* ammo {tag} offset: add rcx,imm32 ; call */
static const uint16_t SIG_ADDTAGS[] = { 0x48, 0x81, 0xC1, W, W, 0x00, 0x00, 0xE8 };
/* linked bit: mov eax,[r?+1C0h]; shr eax,1Ch; test al,1 */
static const uint16_t SIG_LINK[] = { 0x8B, W, 0xC0, 0x01, 0x00, 0x00, 0xC1, 0xE8, 0x1C, 0xA8, 0x01 };
/* range: movaps xmm6,xmm0; call Range; comiss xmm0,xmm6 */
static const uint16_t SIG_RANGE[] = { 0x0F, 0x28, 0xF0, 0xE8, W, W, W, W, 0x0F, 0x2F, 0xC6 };
/* MakeAttack: mov byte [rbx+0C1h],dil (unique) */
static const uint16_t SIG_C1[] = { 0x40, 0x88, 0xBB, 0xC1, 0x00, 0x00, 0x00 };
/* after MakeAttack: mov rdx,rax; lea r8,[rsp+20h]; mov rcx,r64; call AddOrder */
static const uint16_t SIG_ADD[] = { 0x48, 0x8B, 0xD0, 0x4C, 0x8D, 0x44, 0x24, 0x20, 0x48, 0x8B, W, 0xE8 };
/* MakeRef: lea r8,[rsi+30h]; mov rdx,rbx; mov rcx,rdi; call SetEntity */
static const uint16_t SIG_SETENT[] = { 0x4C, 0x8D, 0x46, 0x30, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0xE8 };
/* MakeRef: mov rcx,rdi; call RefInit */
static const uint16_t SIG_REFINIT[] = { 0x48, 0x8B, 0xCF, 0xE8 };
/* MakeRef: lea rcx,[rdi+20h] */
static const uint16_t SIG_TAG20[] = { 0x48, 0x8D, 0x4F, 0x20 };
/* 0.7.1 reload cancel (function starts; W = rip-relative displacements, which differ per exe) */
/* Human::InventoryBusy: sub rsp,28h; test rcx,rcx; je; mov rax,[rcx+578h]; mov rcx,[rax+28h]; test; jne; lea rcx,[rip]; lea rdx,[rip];
 * call Find; test rax; je; mov rcx,[rax+88h]; cmp [rax+80h],rcx; je; mov al,1 */
static const uint16_t SIG_INVBUSY[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x74, 0x10, 0x48, 0x8B, 0x81, 0x78, 0x05, 0x00, 0x00,
    0x48, 0x8B, 0x48, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x07, 0x48, 0x8D, 0x0D, W, W, W, W, 0x48, 0x8D, 0x15, W, W, W, W, 0xE8, W, W, W, W,
    0x48, 0x85, 0xC0, 0x74, 0x17, 0x48, 0x8B, 0x88, 0x88, 0x00, 0x00, 0x00, 0x48, 0x39, 0x88, 0x80, 0x00, 0x00, 0x00, 0x74, 0x07, 0xB0, 0x01 };
/* Human::CancelInventoryOps(?, human): push rbx; sub rsp,20h; mov rcx,[rdx+DC8h]; mov rbx,rdx; mov edx,707h; mov rax,[rcx]; call [rax+98h];
 * mov rcx,rbx; call InvOps; mov rcx,rax; add rsp,20h; pop rbx; jmp CancelAll */
static const uint16_t SIG_INVCANCEL[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x8A, 0xC8, 0x0D, 0x00, 0x00, 0x48, 0x8B, 0xDA,
    0xBA, 0x07, 0x07, 0x00, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xE8, W, W, W, W,
    0x48, 0x8B, 0xC8, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9 };
/* GetCurrentWeapon(holder) (game exe+0x806340, also in GEM MeleeChase): movsxd rax,[rcx+4Ch]; test eax,eax; js; ... */
static const uint16_t SIG_CURWEAP[] = { 0x48, 0x63, 0x41, 0x4C, 0x85, 0xC0, 0x78, 0x1C, 0x48, 0x8B, 0xD0, 0x48, 0x8B, 0x41, 0x18, 0x48, 0x8B, 0x04, 0xD0,
    0x48, 0x85, 0xC0, 0x74, 0x0C, 0x48, 0x8B, 0x80, 0xB8, 0x02, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x75, 0x10, 0x48, 0x8B, 0x41, 0x30, 0x48, 0x3B, 0x41, 0x38,
    0x75, 0x03, 0x33, 0xC0 };
/* 0.7.11 eActor::IsManualControlled (game exe+0x7EF8A0 / editor 0x671970 / server 0x605E10): push rbx; sub rsp,20h; mov rbx,rcx;
 * mov rcx,[rcx+DE0h]; call PropNonZero; test al,al; jne; mov rax,[rbx+DD8h]; mov ecx,[rax+70h]; shr ecx,1; test cl,1 */
static const uint16_t SIG_ISMANUAL[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0xE0, 0x0D, 0x00, 0x00, 0xE8, W, W, W, W,
    0x84, 0xC0, 0x75, W, 0x48, 0x8B, 0x83, 0xD8, 0x0D, 0x00, 0x00, 0x8B, 0x48, 0x70, 0xD1, 0xE9, 0xF6, 0xC1, 0x01 };
/* 0.7.12 manual-control manager global (game exe+0x1E93B6): mov rcx,[rip+g]; call IsActive; test al,al; je; mov rcx,[rip+g];
 * call GetActor; cmp byte [rax+16h],0 - both rip loads must name the same global, GetActor must be `mov rax,[rcx+3B0h]; ret` */
static const uint16_t SIG_MGRACT[] = { 0x48, 0x8B, 0x0D, W, W, W, W, 0xE8, W, W, W, W, 0x84, 0xC0, 0x74, W, 0x48, 0x8B, 0x0D, W, W, W, W,
    0xE8, W, W, W, W, 0x80, 0x78, 0x16, 0x00 };
static const uint8_t MGR_GETACTOR[] = { 0x48, 0x8B, 0x81, 0xB0, 0x03, 0x00, 0x00, 0xC3 };
/* sim clock: brain timers `mov ebx,[rdi+X]; call GetClock; cmp eax,ebx; jb` (from GEM BoardFilter) */
static const uint16_t SIG_CLK[] = { 0x8B, 0x9F, W, W, 0x00, 0x00, 0xE8, W, W, W, W, 0x3B, 0xC3, 0x0F, 0x82 };

static int match(const uint8_t *p, const uint16_t *sig, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (sig[i] != W && p[i] != sig[i]) return 0;
    return 1;
}
#define MATCH(p, s) match(p, s, sizeof s / 2)
static uint8_t *rel32(uint8_t *at) { return at + 4 + *(int32_t *)at; }

typedef struct {
    uint8_t *fn; uint32_t ammotags; int linksites;
    /* charge */
    uint8_t *mkmove;
    uint8_t *fight, *mvchange, *uimove;      /* 0.6.9: eFight factory, eMovementChange factory, the UI click-move factory */
    uint8_t *range, *scan_lo, *scan_hi, *mkatk, *addorder, *mkref, *setent, *refinit, *tagctor, *refvt,
            *refdtor, *tagdtor, *ordstamp, *clock;
    uint32_t reflist;                        /* 0.7.33: entity offset of its back-ref list, from RefDtor (0 = not found) */
    const char *charge_err;
    /* blow reach table (near/medium/far) */
    float *blowtab; uint8_t *blowfn;
    /* instant buttons on a multi-unit selection */
    uint8_t *exinst, *getactor, *expand, *pickctor, *takeitem; uint32_t off_apply, off_getactor, off_canapply, off_item;
    uint8_t **exslots[32]; int nexslots; const char *inst_err; int64_t vt_delta;
    /* melee line-of-reach ray (blow start) */
    uint8_t *ray_site, *ray_ok, *ray_fail, *ray_hits; int ray_reg; const char *ray_err;
    /* 0.7 auto-draw: TagContainer ctor + FromString (from the {tags} action, as GEM TeamTags finds them) */
    uint8_t *tcctor, *tcfromstr; const char *tc_err;
    void **vt_attack, **vt_attackb, **vt_fight;   /* 0.7 explicit-order check: Order::eAttack / eAttackBasic / eFight */
    uint8_t *invbusy, *invcancel, *curweap; const char *rc_err;   /* 0.7.1 reload cancel */
    uint8_t *ismanual; const char *manual_err;                     /* 0.7.11 direct control */
    uint8_t **mgr; const char *mgr_err;                            /* 0.7.12 manual-control manager global */
} sites_t;

typedef struct { uint8_t *base, *lo, *hi; RUNTIME_FUNCTION *rf; size_t nrf; } img_t;

static RUNTIME_FUNCTION *func_of(img_t *im, uint8_t *p)
{
    uint32_t rva = (uint32_t)(p - im->base);
    size_t a = 0, b = im->nrf;
    while (a < b) { size_t m = (a + b) / 2; if (im->rf[m].BeginAddress <= rva) a = m + 1; else b = m; }
    if (!a) return NULL;
    RUNTIME_FUNCTION *f = &im->rf[a - 1];
    return rva < f->EndAddress ? f : NULL;
}
static int in_text(img_t *im, uint8_t *p) { return p >= im->lo && p < im->hi; }

/* next E8 call target at or after p within n bytes (NULL if none) */
static uint8_t *next_call(img_t *im, uint8_t *p, int n, uint8_t **at)
{
    for (int i = 0; i < n; i++)
        if (p[i] == 0xE8) {
            uint8_t *t = rel32(p + i + 1);
            if (in_text(im, t)) { if (at) *at = p + i; return t; }
        }
    return NULL;
}

static const char *find_charge(img_t *im, sites_t *s)
{
    uint8_t *lo = im->lo, *hi = im->hi;
    /* range call inside MatchAmmo */
    RUNTIME_FUNCTION *mf = func_of(im, s->fn);
    for (uint8_t *p = s->fn; p + 11 <= im->base + mf->EndAddress; p++)
        if (MATCH(p, SIG_RANGE)) { s->range = rel32(p + 4); break; }
    if (!s->range || !in_text(im, s->range)) return "range call not found in the ammo match";
    /* enemy scan = the only function that calls MatchAmmo twice */
    RUNTIME_FUNCTION *cand[16]; int cnt[16], nc = 0;
    for (uint8_t *p = lo; p + 5 <= hi; p++) {
        if (*p != 0xE8 || rel32(p + 1) != s->fn) continue;
        RUNTIME_FUNCTION *f = func_of(im, p);
        if (!f) continue;
        int i = 0; while (i < nc && cand[i] != f) i++;
        if (i == nc) { if (nc == 16) continue; cand[nc] = f; cnt[nc++] = 0; }
        cnt[i]++;
    }
    RUNTIME_FUNCTION *scan = NULL;
    for (int i = 0; i < nc; i++) if (cnt[i] == 2) { if (scan) return "enemy scan not unique"; scan = cand[i]; }
    if (!scan) return "enemy scan not found";
    s->scan_lo = im->base + scan->BeginAddress; s->scan_hi = im->base + scan->EndAddress;
    /* MakeAttack: function holding the unique +0xC1 flag store */
    uint8_t *c1 = NULL; int nc1 = 0;
    for (uint8_t *p = lo; p + 7 <= hi; p++) if (MATCH(p, SIG_C1)) { c1 = p; nc1++; }
    if (nc1 != 1) return "attack order factory not found";
    RUNTIME_FUNCTION *af = func_of(im, c1);
    if (!af) return "attack order factory has no function entry";
    s->mkatk = im->base + af->BeginAddress;
    /* its UI call site: ... call MakeAttack; [nop] mov rdx,rax; lea r8,[rsp+20h]; mov rcx,r; call AddOrder */
    uint8_t *site = NULL;
    for (uint8_t *p = lo; p + 24 <= hi; p++) {
        if (*p != 0xE8 || rel32(p + 1) != s->mkatk) continue;
        uint8_t *q = p + 5; while (*q == 0x90) q++;
        if (!MATCH(q, SIG_ADD)) continue;
        if (site) return "attack command site not unique";
        site = p;
        s->addorder = rel32(q + 12);
    }
    if (!site || !in_text(im, s->addorder)) return "attack command site not found";
    /* MakeRef: the last call before MakeAttack (within 0x40) whose body has the SetEntity sequence */
    for (uint8_t *p = site - 0x40; p < site; p++) {
        if (*p != 0xE8) continue;
        uint8_t *f = rel32(p + 1);
        if (!in_text(im, f)) continue;
        for (int i = 0; i < 0x70; i++)
            if (MATCH(f + i, SIG_SETENT)) { s->mkref = f; s->setent = rel32(f + i + 11); break; }
    }
    if (!s->mkref) return "target ref builder not found";
    uint8_t *mr = s->mkref, *at;
    for (int i = 0; i < 0x60 && !s->refvt; i++)          /* lea rax,[rip+vt]; mov [rdi],rax */
        if (mr[i] == 0x48 && mr[i + 1] == 0x8D && mr[i + 2] == 0x05 && mr[i + 7] == 0x48 && mr[i + 8] == 0x89 && mr[i + 9] == 0x07)
            s->refvt = rel32(mr + i + 3);
    for (int i = 0; i < 0x60 && !s->tagctor; i++)
        if (MATCH(mr + i, SIG_TAG20)) s->tagctor = next_call(im, mr + i + 4, 16, NULL);
    for (int i = 0; i < 0x70 && !s->refinit; i++)
        if (MATCH(mr + i, SIG_REFINIT) && in_text(im, rel32(mr + i + 4)) && rel32(mr + i + 4) != s->tagctor) s->refinit = rel32(mr + i + 4);
    if (!s->refvt || !s->tagctor || !s->refinit) return "target ref builder layout unexpected";
    /* after AddOrder: RefDtor then TagDtor */
    uint8_t *q = site + 5; while (*q == 0x90) q++;
    q += 17;
    s->refdtor = next_call(im, q, 0x30, &at);
    if (!s->refdtor) return "ref destructor not found";
    for (int i = 0; i < 0x60 && !s->reflist; i++) {      /* add rcx,imm32; mov rdx,rbx; call unlink */
        const uint8_t *d = s->refdtor + i;
        if (d[0] == 0x48 && d[1] == 0x81 && d[2] == 0xC1 && d[7] == 0x48 && d[8] == 0x8B && d[9] == 0xD3 && d[10] == 0xE8
            && *(const uint32_t *)(d + 3) < 0x1000) s->reflist = *(const uint32_t *)(d + 3);
    }
    s->tagdtor = next_call(im, at + 5, 0x20, NULL);
    if (!s->tagdtor) return "tag destructor not found";
    /* order stamp: mov e?x,[rip+g] within 0x40 bytes before MakeAttack */
    for (uint8_t *p = site - 0x40; p < site; p++)
        if (p[0] == 0x8B && (p[1] & 0xC7) == 0x05 && p[-1] != 0x0F) {
            uint8_t *g = rel32(p + 2);
            if (g >= hi && g < im->base + 0x4000000) s->ordstamp = g;
        }
    if (!s->ordstamp) return "order stamp global not found";
    /* AI clock */
    uint8_t *cg[8] = { 0 }; int cc[8] = { 0 }, ncg = 0;
    for (uint8_t *p = lo; p + sizeof SIG_CLK / 2 <= hi; p++) {
        if (!MATCH(p, SIG_CLK)) continue;
        uint8_t *t = rel32(p + 7);
        if (t < lo || t + 7 > hi || t[0] != 0x8B || t[1] != 0x05 || t[6] != 0xC3) continue;
        uint8_t *g = rel32(t + 2);
        int i = 0; while (i < ncg && cg[i] != g) i++;
        if (i == ncg) { if (ncg == 8) continue; cg[ncg++] = g; }
        cc[i]++;
    }
    int best = -1;
    for (int i = 0; i < ncg; i++) if (best < 0 || cc[i] > cc[best]) best = i;
    if (best < 0 || cc[best] < 5) return "AI clock not found";
    s->clock = cg[best];
    /* eFight factory: the attack_melee action's "call Fight; mov r8,[rsp+50h]; mov rdx,rax; mov rcx,rbx; call AddOrder" */
    for (uint8_t *p = lo; p + 21 <= hi; p++) {
        if (p[0] != 0xE8 || memcmp(p + 5, "\x4C\x8B\x44\x24\x50\x48\x8B\xD0\x48\x8B\xCB\xE8", 12)) continue;
        if (rel32(p + 17) != s->addorder) continue;
        uint8_t *f = rel32(p + 1);
        if (!in_text(im, f)) continue;
        int alloc = 0;
        for (int i = 0; i < 0x40; i++) if (!memcmp(f + i, "\xB9\xE0\x00\x00\x00", 5)) { alloc = 1; break; }
        if (!alloc) continue;
        if (s->fight && s->fight != f) { s->fight = NULL; break; }
        s->fight = f;
    }
    /* eMovementChange factory: every "mov edx,[rax+rdx*4]; call X" (the mode table lookups) must agree, X allocates 0x48 */
    for (uint8_t *p = lo; p + 8 <= hi; p++) {
        if (memcmp(p, "\x8B\x14\x90\xE8", 4)) continue;
        uint8_t *f = rel32(p + 4);
        if (!in_text(im, f)) continue;
        int alloc = 0;
        for (int i = 0; i < 0x60; i++) if (!memcmp(f + i, "\xB9\x48\x00\x00\x00", 5)) { alloc = 1; break; }
        if (!alloc) continue;
        if (s->mvchange && s->mvchange != f) { s->mvchange = NULL; break; }
        s->mvchange = f;
    }
    /* UI click-move factory: alloc 0xF8, then within 0x40 bytes "movsd [rax+5Ch],xmm0", "mov qword [rax+40h],0", "mov [rax+38h],bl" */
    for (uint8_t *p = lo; p + 0x50 <= hi; p++) {
        if (memcmp(p, "\xB9\xF8\x00\x00\x00\xE8", 6)) continue;
        int a = 0, b = 0, c = 0;
        for (int i = 6; i < 0x40; i++) {
            if (!memcmp(p + i, "\xF2\x0F\x11\x40\x5C", 5)) a = 1;
            if (!memcmp(p + i, "\x48\xC7\x40\x40\x00\x00\x00\x00", 8)) b = 1;
            if (!memcmp(p + i, "\x88\x58\x38", 3)) c = 1;
        }
        if (!(a && b && c)) continue;
        RUNTIME_FUNCTION *rf = func_of(im, p);
        if (!rf) continue;
        uint8_t *f = im->base + rf->BeginAddress;
        if (s->uimove && s->uimove != f) { s->uimove = NULL; break; }
        s->uimove = f;
    }
    return NULL;
}

/* Action::ExecuteInstant (action vtable slot, game exe+0xB5E260): runs the {instant} block on the ONE unit
 * GetActor returns (vtable +0xF0, game exe+0xB73E90: expands the selection, returns a unit only when exactly
 * one passes CanApply = vtable +0xD8). Body: mov rax,[rcx+0x870]; cmp [rcx+0x868],rax; ...; call [rax+0xF0]. */
static const char *find_instant(img_t *im, sites_t *s)
{
    uint8_t *f = NULL; int n = 0;
    for (uint8_t *p = im->lo; p + 0x40 <= im->hi; p++) {
        /* cmp qword [rcx+0x868],rax */
        if (!(p[0] == 0x48 && p[1] == 0x39 && p[2] == 0x81 && *(uint32_t *)(p + 3) == 0x868)) continue;
        int has_call = 0;
        for (int i = 0; i < 0x30; i++) if (p[i] == 0xFF && p[i + 1] == 0x90 && *(uint32_t *)(p + i + 2) == 0xF0) { has_call = 1; break; }
        if (!has_call) continue;
        RUNTIME_FUNCTION *rf = func_of(im, p);
        if (!rf || p - (im->base + rf->BeginAddress) > 0x40) continue;
        uint8_t *fn = im->base + rf->BeginAddress;
        if (fn == f) continue;
        f = fn; n++;
    }
    if (n != 1) return n ? "ExecuteInstant not unique" : "ExecuteInstant not found";
    s->exinst = f; s->off_getactor = 0xF0;
    for (int i = 0; i < 0x140 && !s->pickctor; i++)              /* first direct call in ExecuteInstant = pick ctor */
        if (f[i] == 0xE8 && in_text(im, rel32(f + i + 1))) s->pickctor = rel32(f + i + 1);
    /* vtable slots holding it (outside .text) */
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(im->base + ((IMAGE_DOS_HEADER *)im->base)->e_lfanew);
    uint8_t *end = im->base + nt->OptionalHeader.SizeOfImage;
    uint64_t want = (uint64_t)(uintptr_t)f;
    uint64_t want2 = (uint64_t)(f - im->base) + nt->OptionalHeader.ImageBase;   /* image mapped without relocations (offline test) */
    s->vt_delta = (int64_t)(want2 - want);
    uint8_t **apply_slot = NULL;
    for (uint8_t *p = (uint8_t *)(((uintptr_t)im->hi + 7) & ~(uintptr_t)7); p + 8 <= end && s->nexslots < 32; p += 8) {
        uint64_t v = *(uint64_t *)p;
        if (v != want && v != want2) continue;
        if (v == want) s->vt_delta = 0;
        s->exslots[s->nexslots++] = (uint8_t **)p;
        if (!apply_slot) apply_slot = (uint8_t **)(p - 8);
    }
    if (!s->nexslots) return "no vtable holds ExecuteInstant";
    /* apply_to_all byte: the previous slot (Apply, game exe+0xB740F0) opens with cmp byte [rcx+X],0 */
#define VTP(x) ((uint8_t *)((uint64_t)(uintptr_t)(x) - (uint64_t)s->vt_delta))
    uint8_t *ap = VTP(*apply_slot);
    if (!in_text(im, ap)) return "Apply slot not code";
    for (int i = 0; i < 0x30; i++)
        if (ap[i] == 0x80 && ap[i + 1] == 0xB9 && ap[i + 6] == 0x00) { s->off_apply = *(uint32_t *)(ap + i + 2); break; }
    if (s->off_apply < 0x100 || s->off_apply > 0x2000) return "apply_to_all offset not found";
    /* GetActor = vtable[+0xF0]; its first call = selection expand; its call [rax+X] = CanApply */
    uint8_t **vt = s->exslots[0] - 19;                        /* slot 19 in every build seen; checked below */
    if (VTP(vt[19]) != f) return "vtable layout unexpected";
    s->getactor = VTP(vt[s->off_getactor / 8]);
    if (!in_text(im, s->getactor)) return "GetActor slot not code";
    for (int i = 0; i < 0x60 && !s->expand; i++)
        if (s->getactor[i] == 0xE8 && in_text(im, rel32(s->getactor + i + 1))) s->expand = rel32(s->getactor + i + 1);
    for (int i = 0; i < 0x90; i++)
        if (s->getactor[i] == 0xFF && s->getactor[i + 1] == 0x90 && *(uint32_t *)(s->getactor + i + 2) < 0x400) {
            s->off_canapply = *(uint32_t *)(s->getactor + i + 2); break;
        }
    if (!s->expand || !s->off_canapply) return "GetActor layout unexpected";
    return NULL;
}

/* Order::eTakeItem factory (game exe+0x8EA160, (Tags *item) -> order): the `{take_item}` order instruction's exec
 * (instruction vtable slot 3, game exe+0xB5DC30) = lea rcx,[rdx+X] (X = the action's {item} Tags, +0xC8); call it.
 * The instruction table in .rdata pairs the name pointer "take_item" with the factory (lea rcx,[rip+vt]). */
static const char *find_takeitem(img_t *im, sites_t *s)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(im->base + ((IMAGE_DOS_HEADER *)im->base)->e_lfanew);
    uint8_t *end = im->base + nt->OptionalHeader.SizeOfImage;
    static const char nm[] = "take_item";
    uint8_t *str = NULL;
    for (uint8_t *p = im->hi; p + sizeof nm <= end; p++)
        if (p[-1] == 0 && !memcmp(p, nm, sizeof nm)) { str = p; break; }
    if (!str) return "take_item name not found";
    uint64_t w1 = (uint64_t)(uintptr_t)str, w2 = (uint64_t)(str - im->base) + nt->OptionalHeader.ImageBase;
    for (uint8_t *p = (uint8_t *)(((uintptr_t)im->hi + 7) & ~(uintptr_t)7); p + 16 <= end; p += 8) {
        uint64_t v = *(uint64_t *)p;
        if (v != w1 && v != w2) continue;
        int64_t dl = v == w1 ? 0 : (int64_t)(w2 - w1);
        uint8_t *fac = (uint8_t *)(uintptr_t)(*(uint64_t *)(p + 8) - (uint64_t)dl);
        if (!in_text(im, fac)) continue;
        for (int i = 0; i < 0x30; i++) {
            if (!(fac[i] == 0x48 && fac[i + 1] == 0x8D && fac[i + 2] == 0x0D)) continue;   /* lea rcx,[rip+vt] */
            uint8_t **vt = (uint8_t **)rel32(fac + i + 3);
            if ((uint8_t *)vt < im->hi || (uint8_t *)vt + 0x20 > end) break;
            uint8_t *ex = (uint8_t *)(uintptr_t)((uint64_t)(uintptr_t)vt[3] - (uint64_t)dl);
            if (!in_text(im, ex)) break;
            for (int j = 0; j < 0x20; j++)
                if (ex[j] == 0x48 && ex[j + 1] == 0x8D && ex[j + 2] == 0x8A) {           /* lea rcx,[rdx+X] */
                    s->off_item = *(uint32_t *)(ex + j + 3);
                    s->takeitem = next_call(im, ex + j + 7, 0x10, NULL);
                    break;
                }
            break;
        }
        if (s->takeitem) break;
    }
    if (!s->takeitem || s->off_item < 0x40 || s->off_item > 0x1000) return "take_item order factory not found";
    return NULL;
}

/* TagContainer ctor + FromString: the {tags} action's instruction-table entry pairs a pointer to "tags" with its factory;
 * the factory builds the action (lea vt; mov [rbx],rax; call TagContainer::ctor -> mov byte [rcx+0x300],0), its parse
 * (vtable slot 5) calls FromString(container, std::string*, ' ') after mov r8b,0x20 (FromString reads [rcx+0x318]) */
static const char *find_tagcont(img_t *im, sites_t *s)
{
    uint8_t *base = im->base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    uint8_t *end = base + nt->OptionalHeader.SizeOfImage, *ctor = NULL; int hits = 0;
    uint64_t pref = nt->OptionalHeader.ImageBase, size = nt->OptionalHeader.SizeOfImage;
#define FIXP(x) ((uint8_t *)(((uint64_t)(uintptr_t)(x) - pref < size) ? (uintptr_t)(base + ((uint64_t)(uintptr_t)(x) - pref)) : (uintptr_t)(x)))   /* offline test: image mapped without relocations */
    for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!memcmp(sec->Name, ".text", 5)) continue;
        uint8_t *lo = base + sec->VirtualAddress, *hi = lo + sec->Misc.VirtualSize;
        for (uint8_t *q = lo; q + 16 <= hi; q += 8) {
            uint8_t *str = FIXP(*(uint8_t **)q), *fn = FIXP(*(uint8_t **)(q + 8));
            if (str <= base || str + 5 > end || !in_text(im, fn)) continue;
            if (in_text(im, str) || memcmp(str, "tags", 5) || str[-1] != 0) continue;
            if (ctor != fn) { ctor = fn; hits++; }
        }
    }
    if (hits != 1) return hits ? "tags action entry ambiguous" : "tags action entry not found";
    uint8_t *vt = NULL, *after = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
            uint8_t *t = rel32(p + 3);
            if (t > base && t < end && !in_text(im, t)) { vt = t; after = p + 7; break; }
        }
    if (!vt) return "tags action vtable not found";
    static const uint8_t ctor_sig[] = { 0xC6, 0x81, 0x00, 0x03, 0x00, 0x00, 0x00 };   /* mov byte [rcx+0x300],0 */
    uint8_t *cc = next_call(im, after, 0x20, NULL);
    int ok = 0;
    for (uint8_t *q = cc; cc && q < cc + 0x20; q++) if (!memcmp(q, ctor_sig, sizeof ctor_sig)) ok = 1;
    if (!ok) return "TagContainer ctor not recognised";
    uint8_t *ps = FIXP(((uint8_t **)vt)[5]), *fs = NULL;
#undef FIXP
    if (!in_text(im, ps)) return "tags action parse not in .text";
    for (uint8_t *q = ps; q < ps + 0x180 && !fs; q++)
        if (q[0] == 0x41 && q[1] == 0xB0 && q[2] == 0x20) fs = next_call(im, q + 3, 0x10, NULL);
    if (!fs) return "FromString not found";
    ok = 0;
    for (uint8_t *q = fs; q < fs + 0x40; q++)
        if ((q[0] & 0xFB) == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x81 && *(uint32_t *)(q + 3) == TAGS_VEC) ok = 1;
    if (!ok) return "FromString not recognised";
    s->tcctor = cc; s->tcfromstr = fs;
    return NULL;
}

/* vtable of a class by RTTI name: type descriptor (name at +0x10) -> complete object locator (sig 1, offset 0, +0xC type
 * descriptor rva, +0x14 self rva) -> the qword pointing at it sits right before the vtable. Pointers are compared as
 * live and as preferred-base addresses (offline test maps the exe without relocations). */
static void **find_vtable_rtti(img_t *im, const char *name)
{
    uint8_t *base = im->base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage, n = strlen(name) + 1;
    uint64_t pref = nt->OptionalHeader.ImageBase;
    uint8_t *td = NULL;
    for (uint8_t *p = im->hi; p + n <= base + size; p++)
        if (*p == '.' && !memcmp(p, name, n)) { td = p - 0x10; break; }
    if (!td) return NULL;
    uint32_t tdr = (uint32_t)(td - base);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!memcmp(sec->Name, ".text", 5)) continue;
        uint8_t *lo = base + sec->VirtualAddress, *hi = lo + sec->Misc.VirtualSize;
        for (uint8_t *p = lo; p + 24 <= hi; p += 4) {
            const uint32_t *col = (const uint32_t *)p;
            if (col[0] != 1 || col[1] != 0 || col[3] != tdr || col[5] != (uint32_t)(p - base)) continue;
            uint64_t va1 = (uint64_t)(uintptr_t)p, va2 = (uint64_t)(p - base) + pref;
            IMAGE_SECTION_HEADER *s2 = IMAGE_FIRST_SECTION(nt);
            for (int j = 0; j < nt->FileHeader.NumberOfSections; j++, s2++) {
                if (!memcmp(s2->Name, ".text", 5)) continue;
                uint8_t *l2 = base + s2->VirtualAddress, *h2 = l2 + s2->Misc.VirtualSize;
                for (uint8_t *q = l2; q + 16 <= h2; q += 8)
                    if (*(uint64_t *)q == va1 || *(uint64_t *)q == va2) return (void **)(q + 8);
            }
        }
    }
    return NULL;
}

static const char *find_all(uint8_t *base, sites_t *s)
{
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    img_t im = { base, NULL, NULL, NULL, 0 };
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (!memcmp(sec->Name, ".text", 5)) { im.lo = base + sec->VirtualAddress; im.hi = im.lo + sec->Misc.VirtualSize; break; }
    if (!im.lo) return "no .text section";
    IMAGE_DATA_DIRECTORY *ex = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!ex->VirtualAddress || !ex->Size) return "no exception directory";
    im.rf = (RUNTIME_FUNCTION *)(base + ex->VirtualAddress);
    im.nrf = ex->Size / sizeof *im.rf;

    memset(s, 0, sizeof *s);
    int found = 0;
    for (uint8_t *p = im.lo; p + sizeof SIG_FLAGS / 2 <= im.hi; p++) {
        if (!MATCH(p, SIG_FLAGS)) continue;
        RUNTIME_FUNCTION *f = func_of(&im, p);
        if (!f) continue;
        uint8_t *fn = base + f->BeginAddress, *fe = base + f->EndAddress;
        if (memcmp(fn, PROLOG, sizeof PROLOG)) continue;
        if (fe - fn > 0x600) continue;
        uint32_t off = 0;
        for (uint8_t *q = fn; q + sizeof SIG_ADDTAGS / 2 <= fe; q++)
            if (MATCH(q, SIG_ADDTAGS)) { off = *(uint32_t *)(q + 3); break; }
        if (off < 0x400 || off > 0x3000) continue;
        if (s->fn == fn) continue;
        if (s->fn) return "ammo match function not unique";
        s->fn = fn; s->ammotags = off; found++;
    }
    if (!found) return "ammo match function not found (unknown exe build, or already patched)";
    for (uint8_t *p = im.lo; p + sizeof SIG_LINK / 2 <= im.hi; p++)
        if (MATCH(p, SIG_LINK)) s->linksites++;
    s->charge_err = find_charge(&im, s);
    s->inst_err = find_instant(&im, s);
    if (!s->inst_err) s->inst_err = find_takeitem(&im, s);
    s->tc_err = find_tagcont(&im, s);
    s->vt_attack  = find_vtable_rtti(&im, ".?AUeAttack@Order@@");
    s->vt_attackb = find_vtable_rtti(&im, ".?AUeAttackBasic@Order@@");
    s->vt_fight   = find_vtable_rtti(&im, ".?AUeFight@Order@@");
    /* MakeMove(xy, flag): the function holding the "Moving to zero bug!" assert (game exe+0x8F3600,
     * editor 0x779730 opens mov [rsp+10h],rbx, server 0x708330) */
    {
        IMAGE_NT_HEADERS64 *nt2 = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        uint8_t *end = base + nt2->OptionalHeader.SizeOfImage, *str = NULL;
        static const char msg[] = "Moving to zero bug!";
        for (uint8_t *q = im.hi; q + sizeof msg + 1 <= end && !str; q++)
            if (q[0] == 0 && !memcmp(q + 1, msg, sizeof msg)) str = q + 1;
        uint8_t *ref = NULL;
        for (uint8_t *q = im.lo; str && q + 7 <= im.hi && !ref; q++)
            if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0xC7) == 0x05 && rel32(q + 3) == str) ref = q;
        RUNTIME_FUNCTION *rf = ref ? func_of(&im, ref) : NULL;
        uint8_t *f = rf ? base + rf->BeginAddress : NULL;
        if (f && ref - f < 0x100 && f[0] == 0x48 && f[1] == 0x89 && f[2] == 0x5C && f[3] == 0x24 && (f[4] == 0x08 || f[4] == 0x10)) s->mkmove = f;
    }
    /* blow start ray check: mov rax,[rip+hits]; cmp rax,[rip+hits+8]; je fail; cmp [rax+18h],rTarget; jne fail */
    {
        int n = 0;
        for (uint8_t *p = im.lo; p + 30 <= im.hi; p++) {
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05 || p[7] != 0x48 || p[8] != 0x3B || p[9] != 0x05) continue;
            if (p[14] != 0x0F || p[15] != 0x84 || p[20] != 0x4C || p[21] != 0x39 || (p[22] & 0xC7) != 0x40 || p[23] != 0x18) continue;
            if (p[24] != 0x0F || p[25] != 0x85) continue;
            uint8_t *g1 = rel32(p + 3), *g2 = rel32(p + 10);
            if (g2 != g1 + 8) continue;
            uint8_t *f1 = rel32(p + 16), *f2 = rel32(p + 26);
            if (f1 != f2) continue;
            n++;
            s->ray_site = p; s->ray_hits = g1; s->ray_fail = f1; s->ray_ok = p + 30; s->ray_reg = 8 + ((p[22] >> 3) & 7);
        }
        s->ray_err = n == 1 ? NULL : (n ? "melee ray check not unique" : "melee ray check not found");
    }
    /* blow reach: GetBlowDistance = movsxd rax,ecx; lea r,[rip+table]; movss xmm0,[r+rax*4]; ret,
     * table holds the vanilla 17 / 29 / 35 (near / medium / far, world units) */
    for (uint8_t *p = im.lo; p + 17 <= im.hi; p++) {
        if (p[0] != 0x48 || p[1] != 0x63 || p[2] != 0xC1 || p[3] != 0x48 || p[4] != 0x8D || (p[5] & 0xC7) != 0x05) continue;
        int reg = (p[5] >> 3) & 7;
        if (p[10] != 0xF3 || p[11] != 0x0F || p[12] != 0x10 || p[13] != 0x04 || p[14] != (0x80 | reg) || p[15] != 0xC3) continue;
        float *t = (float *)rel32(p + 6);
        if (t[0] != 17.0f || t[1] != 29.0f || t[2] != 35.0f) continue;
        if (s->blowfn) { s->blowfn = NULL; s->blowtab = NULL; break; }   /* not unique: leave it alone */
        s->blowfn = p; s->blowtab = t;
    }
    /* 0.7.1 reload cancel: three function starts by signature, each must be unique */
    {
        int nb = 0, nc = 0, nw = 0, nm = 0, ng = 0;
        for (uint8_t *p = im.lo; p + 80 <= im.hi; p++) {
            if (MATCH(p, SIG_INVBUSY))   { s->invbusy = p; nb++; }
            if (MATCH(p, SIG_INVCANCEL)) { s->invcancel = p; nc++; }
            if (MATCH(p, SIG_CURWEAP))   { s->curweap = p; nw++; }
            if (MATCH(p, SIG_ISMANUAL))  { s->ismanual = p; nm++; }           /* 0.7.11 */
            if (MATCH(p, SIG_MGRACT)) {                                         /* 0.7.12 */
                uint8_t *g1 = p + 7 + *(const int32_t *)(p + 3), *g2 = p + 16 + 7 + *(const int32_t *)(p + 16 + 3);
                uint8_t *fn = p + 16 + 7 + 5 + *(const int32_t *)(p + 16 + 7 + 1);
                if (g1 == g2 && in_text(&im, fn) && memcmp(fn, MGR_GETACTOR, sizeof MGR_GETACTOR) == 0) { s->mgr = (uint8_t **)g1; ng++; }
            }
        }
        if (nm != 1) { s->manual_err = nm ? "IsManualControlled not unique" : "IsManualControlled not found"; s->ismanual = NULL; }
        if (ng != 1) { s->mgr_err = ng ? "manual-control manager not unique" : "manual-control manager not found"; s->mgr = NULL; }
        if (nb != 1 || nc != 1 || nw != 1) {
            s->rc_err = nb != 1 ? (nb ? "inventory busy test not unique" : "inventory busy test not found")
                      : nc != 1 ? (nc ? "inventory cancel-all not unique" : "inventory cancel-all not found")
                      : (nw ? "GetCurrentWeapon not unique" : "GetCurrentWeapon not found");
            s->invbusy = s->invcancel = s->curweap = NULL;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- ini */
static int split_list(const char *src, char out[][TLEN], int max)
{
    int n = 0; const char *p = src;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        int k = 0;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && k < TLEN - 1) out[n][k++] = lc(*p++);
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        out[n][k] = 0;
        if (k) n++;
    }
    return n;
}

static void load_ini(const char *ini)
{
    char buf[1024];
    cfg_enabled     = GetPrivateProfileIntA("MeleeStance", "Enabled", 1, ini);
    cfg_log         = GetPrivateProfileIntA("MeleeStance", "Log", 1, ini);
    cfg_skip_linked = GetPrivateProfileIntA("MeleeStance", "SkipLinked", 1, ini);
    cfg_lift_ready  = GetPrivateProfileIntA("MeleeStance", "DrawFromBelt", 1, ini);
    cfg_manual_skip = GetPrivateProfileIntA("MeleeStance", "SkipDirectControl", 1, ini);   /* 0.7.11 */
    {   char sk[256]; GetPrivateProfileStringA("MeleeStance", "SkipTags", "tp_jetpack,hidden,melee_disabled,disable_melee,valkyrie,sensor_off", sk, sizeof sk, ini);   /* 0.7.17 */
        n_skiptags = split_list(sk, cfg_skiptags, 8);
        GetPrivateProfileStringA("MeleeStance", "IgnoreTargetTags", "hidden,melee_disabled,valkyrie,sensor_off", sk, sizeof sk, ini);   /* 0.7.28 */
        n_tgttags = split_list(sk, cfg_tgttags, 8); }
    cfg_order_on = GetPrivateProfileIntA("AutoDraw", "OrderPicks", 1, ini);                  /* 0.7.17 */
    cfg_manual_diag = GetPrivateProfileIntA("MeleeStance", "DiagManual", 0, ini);          /* 0.7.14 */
    cfg_hooktime = GetPrivateProfileIntA("MeleeStance", "HookTiming", 1, ini);             /* 0.7.34 */
    cfg_kcache = GetPrivateProfileIntA("MeleeStance", "FastPass", 1, ini);                 /* 0.7.35 */
    cfg_gate = GetPrivateProfileIntA("MeleeStance", "ClientGate", 1, ini);                 /* 0.7.35 */
    GetPrivateProfileStringA("MeleeStance", "Tag", "melee_stance", cfg_tag, sizeof cfg_tag, ini);
    GetPrivateProfileStringA("MeleeStance", "MeleeAmmoTags",
        "melee_light,melee_heavy,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target", buf, sizeof buf, ini);
    n_mtags = split_list(buf, cfg_mtags, MAXT);
    cfg_charge        = GetPrivateProfileIntA("Charge", "Enabled", 1, ini);
    GetPrivateProfileStringA("Charge", "Tag", "has_melee_weapon", cfg_ctag, sizeof cfg_ctag, ini);
    cfg_charge_inhand = GetPrivateProfileIntA("Charge", "MeleeInHand", 1, ini);
    cfg_charge_player = GetPrivateProfileIntA("Charge", "PlayerUnits", 1, ini);
    cfg_zerg_on = GetPrivateProfileIntA("Zerg", "Enabled", 1, ini);                          /* 0.7.29 */
    cfg_zerg_dist = (float)GetPrivateProfileIntA("Zerg", "Distance", 1600, ini);
    cfg_zerg_reorder = GetPrivateProfileIntA("Zerg", "ReorderMs", 700, ini);
    {   char zt[TLEN]; GetPrivateProfileStringA("Zerg", "Tag", "melee_infantry", zt, sizeof zt, ini);   /* 0.7.30 */
        cfg_zerg_tag[0][0] = 0; split_list(zt, cfg_zerg_tag, 1); }
    cfg_charge_cd     = (uint32_t)GetPrivateProfileIntA("Charge", "CooldownMs", 4000, ini);
    cfg_retarget      = (uint32_t)GetPrivateProfileIntA("Charge", "RetargetMs", 1500, ini);
    cfg_refresh_ms    = (uint32_t)GetPrivateProfileIntA("Charge", "RefreshMs", 400, ini);
    {
        char b2[64];
        GetPrivateProfileStringA("Charge", "SwitchRatio", "0.6", b2, sizeof b2, ini);
        cfg_switch = (float)atof(b2); if (cfg_switch < 0.1f) cfg_switch = 0.1f; if (cfg_switch > 1.0f) cfg_switch = 1.0f;
        GetPrivateProfileStringA("Charge", "ProgressUnits", "10", b2, sizeof b2, ini);
        cfg_progress = (float)atof(b2); if (cfg_progress < 0.0f) cfg_progress = 0.0f;
        GetPrivateProfileStringA("Charge", "CloseDistance", "200", b2, sizeof b2, ini);
        cfg_close = (float)atof(b2); if (cfg_close < 0.0f) cfg_close = 0.0f;
        cfg_runmode = GetPrivateProfileIntA("Charge", "RunMode", 2, ini); if (cfg_runmode > 3) cfg_runmode = 3;
        GetPrivateProfileStringA("Charge", "MoveRefreshUnits", "40", b2, sizeof b2, ini);
        cfg_refresh = (float)atof(b2); if (cfg_refresh < 10.0f) cfg_refresh = 10.0f;
        GetPrivateProfileStringA("Charge", "MoveRefreshFraction", "0.4", b2, sizeof b2, ini);                 /* 0.7.13 */
        cfg_refresh_frac = (float)atof(b2); if (cfg_refresh_frac < 0.0f) cfg_refresh_frac = 0.0f; if (cfg_refresh_frac > 1.0f) cfg_refresh_frac = 1.0f;
    }
    GetPrivateProfileStringA("Charge", "MaxDistance", "2000", buf, sizeof buf, ini);
    cfg_charge_max = (float)atof(buf);
    GetPrivateProfileStringA("Charge", "MinDistance", "60", buf, sizeof buf, ini);
    cfg_charge_min = (float)atof(buf);
    cfg_multi = GetPrivateProfileIntA("MultiSelect", "Enabled", 1, ini);
    cfg_draw  = GetPrivateProfileIntA("MeleeStance", "DrawOnToggle", 1, ini);
    cfg_draw_cd = (uint32_t)GetPrivateProfileIntA("MeleeStance", "DrawCooldownMs", 3000, ini);
    GetPrivateProfileStringA("MeleeStance", "DrawButton", "melee_toggle", cfg_drawbtn, sizeof cfg_drawbtn, ini);
    cfg_ray = GetPrivateProfileIntA("ShieldMelee", "Enabled", 0, ini);
    GetPrivateProfileStringA("ShieldMelee", "SeeThroughTag", "forcefield", buf, sizeof buf, ini);
    split_list(buf, cfg_seetag, 1);
    cfg_down_on = GetPrivateProfileIntA("Downed", "Enabled", 1, ini);
    GetPrivateProfileStringA("Downed", "Tag", "start_personage", buf, sizeof buf, ini);
    split_list(buf, cfg_downtag, 1);
    cfg_blow_on = GetPrivateProfileIntA("BlowReach", "Enabled", 1, ini);
    static const char *bk[3] = { "Near", "Medium", "Far" }, *bd[3] = { "45", "45", "45" };
    for (int i = 0; i < 3; i++) {
        GetPrivateProfileStringA("BlowReach", bk[i], bd[i], buf, sizeof buf, ini);
        cfg_blow[i] = (float)atof(buf);
        if (cfg_blow[i] < 2.0f) cfg_blow[i] = 2.0f;           /* the engine skips targets closer than 2 units */
    }
    GetPrivateProfileStringA("MeleeStance", "AlwaysTag", "melee_always", cfg_atag, sizeof cfg_atag, ini);
    cfg_ad_on     = GetPrivateProfileIntA("AutoDraw", "Enabled", 1, ini);
    cfg_ad_player = GetPrivateProfileIntA("AutoDraw", "PlayerUnits", 1, ini);
    cfg_ad_hold   = (uint32_t)GetPrivateProfileIntA("AutoDraw", "HoldMs", 3000, ini);
    GetPrivateProfileStringA("AutoDraw", "Distance", "100", buf, sizeof buf, ini);
    cfg_ad_dist = (float)atof(buf); if (cfg_ad_dist < 0.0f) cfg_ad_dist = 0.0f;
    GetPrivateProfileStringA("AutoDraw", "Item", "melee", cfg_ad_item, sizeof cfg_ad_item, ini);
    cfg_probe_on   = GetPrivateProfileIntA("AutoDraw", "Probe", 1, ini);                       /* 0.7.16 */
    cfg_probe_ms   = (uint32_t)GetPrivateProfileIntA("AutoDraw", "ProbeMs", 300, ini);
    cfg_probe_cd   = (uint32_t)GetPrivateProfileIntA("AutoDraw", "ProbeCooldownMs", 4000, ini);
    cfg_nomelee_ms = (uint32_t)GetPrivateProfileIntA("AutoDraw", "NoMeleeMs", 20000, ini);
    cfg_probe_any  = GetPrivateProfileIntA("AutoDraw", "ProbeAnyRange", 1, ini);                /* 0.7.18 */
    cfg_known_ms   = (uint32_t)GetPrivateProfileIntA("AutoDraw", "KnownMs", 300000, ini);
    cfg_poll_on    = GetPrivateProfileIntA("AutoDraw", "OrderPoll", 1, ini);
    cfg_poll_ms    = (uint32_t)GetPrivateProfileIntA("AutoDraw", "PollMs", 250, ini);
    cfg_blind_ms   = (uint32_t)GetPrivateProfileIntA("AutoDraw", "BlindCheckMs", 8000, ini);      /* 0.7.19; 0.7.24: 8000 */
    cfg_prox_on    = GetPrivateProfileIntA("AutoDraw", "Proximity", 1, ini);                        /* 0.7.24 */
    cfg_esc_ms     = (uint32_t)GetPrivateProfileIntA("AutoDraw", "EscalateMs", 1500, ini);
    { char lf[32]; GetPrivateProfileStringA("AutoDraw", "LeaveFactor", "1.5", lf, sizeof lf, ini); cfg_leave = (float)atof(lf); if (cfg_leave < 1.0f) cfg_leave = 1.0f; }   /* 0.7.27 */
    cfg_tick_on    = GetPrivateProfileIntA("AutoDraw", "Tick", 1, ini);                             /* 0.7.22 */
    cfg_tick_ms    = (uint32_t)GetPrivateProfileIntA("AutoDraw", "TickMs", 100, ini);
    if (cfg_tick_ms < 20) cfg_tick_ms = 20;
    cfg_fight_on   = GetPrivateProfileIntA("AutoDraw", "FightOrder", 1, ini);
    cfg_fight_cd   = (uint32_t)GetPrivateProfileIntA("AutoDraw", "FightCooldownMs", 1500, ini);
    if (!cfg_probe_ms) cfg_probe_ms = 1;
    cfg_veh_on     = GetPrivateProfileIntA("Vehicles", "Enabled", 1, ini);
    cfg_inf_ms     = (uint32_t)GetPrivateProfileIntA("Vehicles", "InfantryMs", 500, ini);
    GetPrivateProfileStringA("Vehicles", "InfantryRange", "400", buf, sizeof buf, ini);
    cfg_inf_range = (float)atof(buf); if (cfg_inf_range < 0.0f) cfg_inf_range = 0.0f;
    GetPrivateProfileStringA("Vehicles", "CloseDistance", "400", buf, sizeof buf, ini);
    cfg_close_veh = (float)atof(buf); if (cfg_close_veh < 0.0f) cfg_close_veh = 0.0f;
    cfg_veh_attack = GetPrivateProfileIntA("Vehicles", "CloseAttackOrder", 1, ini);
    GetPrivateProfileStringA("Vehicles", "VehicleMeleeTags", "melee_light,melee_heavy", buf, sizeof buf, ini);
    n_vtags = split_list(buf, cfg_vtags, MAXT);
    cfg_adj_ms = (uint32_t)GetPrivateProfileIntA("Vehicles", "AdjacentMs", 1500, ini);
    GetPrivateProfileStringA("Vehicles", "ContactTag", "hull_contact", buf, sizeof buf, ini);
    n_contacttag = split_list(buf, cfg_contacttag, 1);
    GetPrivateProfileStringA("Vehicles", "AdjacentMax", "120", buf, sizeof buf, ini);
    cfg_adj_extra = (float)atof(buf); if (cfg_adj_extra < 0.0f) cfg_adj_extra = 0.0f;
    cfg_rc_on = GetPrivateProfileIntA("ReloadCancel", "Enabled", 1, ini);
    GetPrivateProfileStringA("ReloadCancel", "KeepTags",
        "mgun,hmg_shell,stubber,lascannon,plasmacannon,sm_weapon_plasmacannon,sm_weapon_heavybolter,rocketlauncher,heavy_flamethrower,grenadelauncher,at_rifle,ogryn_weapon",
        buf, sizeof buf, ini);
    n_keep = split_list(buf, cfg_keep, MAXT);
}

static void init_tag(void)
{
    char tmp[1][TLEN];
    split_list(cfg_tag, g_tagl, 1);
    n_tagl = 1;
    if (split_list(cfg_atag, tmp, 1)) { strcpy(g_tagl[1], tmp[0]); n_tagl = 2; }
    split_list(cfg_ctag, g_ctagl, 1);
}

#ifndef MS_TEST
/* ---------------------------------------------------------------- install */
static uint8_t *emit(uint8_t *p, const void *b, size_t n) { memcpy(p, b, n); return p + n; }
static uint8_t *emit_absjmp(uint8_t *p, const void *to)
{
    static const uint8_t j[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
    p = emit(p, j, 6);
    uint64_t t = (uint64_t)(uintptr_t)to;
    return emit(p, &t, 8);
}

static void install(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    sites_t s;
    init_tag();
    if (!cfg_enabled) { logf_("[GEM MeleeStance %s] Enabled=0, not installed", MS_VERSION); return; }
    const char *err = find_all(base, &s);
    if (err) { logf_("[GEM MeleeStance %s] NOT installed: %s", MS_VERSION, err); return; }
    if (s.linksites < 20) {
        cfg_skip_linked = 0;
        logf_("  linked flag test not found (%d sites), SkipLinked off: crews in stance would not fire", s.linksites);
    }
    cfg_ammotags = s.ammotags;
    if (s.clock) g_clock = (volatile uint32_t *)s.clock;     /* also used by the stance (melee carried check) */
    if (cfg_charge && s.charge_err) { cfg_charge = 0; logf_("  charge OFF: %s", s.charge_err); }
    if (s.charge_err && (cfg_ad_on || cfg_veh_on)) { cfg_ad_on = 0; logf_("  auto-draw OFF (needs the charge finders): %s", s.charge_err); }
    if (!s.charge_err) {                       /* engine pointers: charge, auto-draw and the vehicle rule share them */
        g_range = (range_t)s.range; g_scan_lo = s.scan_lo; g_scan_hi = s.scan_hi;
        g_exe_lo = base;                                                            /* 0.7.17: caller RVAs in the order: log */
        g_mkatk = (mkatk_t)s.mkatk; g_addorder = (addord_t)s.addorder; g_setent = (setent_t)s.setent;
        g_mkmove = (mkmove_t)s.mkmove;
        g_fight = (fight_t)s.fight; g_mvchange = (mvchange_t)s.mvchange; g_uimove = (uimove_t)s.uimove;
        if (!s.mkmove && !s.uimove) logf_("  charge: no move order factory found, the far part of the charge is the attack order too");
        else logf_("  charge: beyond CloseDistance=%.0f: %s (exe+0x%llX)%s, refreshed when the enemy moves %.0f or %.0f%% of the distance; inside: %s (exe+0x%llX)",
                   cfg_close, s.mkmove ? "eMoveTask, the double-click run" : "eAdvance, the click walk", (unsigned long long)((s.mkmove ? s.mkmove : s.uimove) - base),
                   s.mvchange && cfg_runmode >= 0 ? " after eMovementChange" : " (no movement-mode order)", cfg_refresh, cfg_refresh_frac * 100.0f,
                   s.fight ? "eFight melee order" : "attack order (eFight not found)", (unsigned long long)((s.fight ? s.fight : s.mkatk) - base));
        if (s.mvchange) logf_("  charge: eMovementChange exe+0x%llX, RunMode=%d", (unsigned long long)(s.mvchange - base), cfg_runmode);
        g_refinit = (vfn1_t)s.refinit; g_tagctor = (vfn1_t)s.tagctor; g_refvt = s.refvt;
        g_refdtor = (vfn1_t)s.refdtor; g_tagdtor = (vfn1_t)s.tagdtor;
        g_reflist_off = s.reflist;
        if (s.reflist) logf_("  refs: entity ref list at +0x%X, stale refs are dropped without unlink", s.reflist);
        else logf_("  refs: entity ref list offset not found in RefDtor, stale-ref check OFF");
        g_ordstamp = (volatile uint32_t *)s.ordstamp; g_clock = (volatile uint32_t *)s.clock;
    }

    uint8_t *tr = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { logf_("[GEM MeleeStance] VirtualAlloc failed (%lu)", GetLastError()); return; }
    uint8_t *q = emit(tr, s.fn, PATCH);
    emit_absjmp(q, s.fn + PATCH);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_orig = (match_t)tr;

    uint8_t patch[PATCH];
    emit_absjmp(patch, (void *)ms_hook);
    DWORD old;
    if (!VirtualProtect(s.fn, PATCH, PAGE_EXECUTE_READWRITE, &old)) { logf_("[GEM MeleeStance] VirtualProtect failed (%lu)", GetLastError()); return; }
    memcpy(s.fn, patch, PATCH);
    VirtualProtect(s.fn, PATCH, old, &old);
    FlushInstructionCache(GetCurrentProcess(), s.fn, PATCH);

    char ml[600] = "";
    for (int i = 0; i < n_mtags; i++) { strcat(ml, cfg_mtags[i]); if (i + 1 < n_mtags) strcat(ml, ","); }
    if (cfg_log) CreateThread(NULL, 0, hb_thread, NULL, 0, NULL);                  /* 0.7.21 */
    logf_("[GEM MeleeStance %s] installed: ammo match exe+0x%llX, ammo tags +0x%X, linked +0x%X bit %u (%d sites) | "
          "Tag=%s AlwaysTag=%s MeleeAmmoTags=%s SkipLinked=%d DrawFromBelt=%d Log=%d",
          MS_VERSION, (unsigned long long)(s.fn - base), cfg_ammotags, cfg_linkoff, cfg_linkbit, s.linksites,
          g_tagl[0], n_tagl > 1 ? g_tagl[1] : "-", ml, cfg_skip_linked, cfg_lift_ready, cfg_log);
    if (cfg_charge)
        logf_("  charge ON: Tag=%s MeleeInHand=%d PlayerUnits=%d CooldownMs=%u RetargetMs=%u RefreshMs=%u MinDistance=%.0f MaxDistance=%.0f | scan exe+0x%llX, "
              "MakeAttack exe+0x%llX, AddOrder exe+0x%llX, ref vt exe+0x%llX, range exe+0x%llX, clock exe+0x%llX, stamp exe+0x%llX",
              g_ctagl[0], cfg_charge_inhand, cfg_charge_player, cfg_charge_cd, cfg_retarget, cfg_refresh_ms, cfg_charge_min, cfg_charge_max,
              (unsigned long long)(s.scan_lo - base), (unsigned long long)(s.mkatk - base), (unsigned long long)(s.addorder - base),
              (unsigned long long)(s.refvt - base), (unsigned long long)(s.range - base), (unsigned long long)(s.clock - base),
              (unsigned long long)(s.ordstamp - base));
    /* melee through linked shields: replace the blow picker's "first ray hit is the target" test with ms_ray_ok */
    if (cfg_ray) {
        if (s.ray_err) logf_("  melee through shields OFF: %s", s.ray_err);
        else {
            g_ray_hits = s.ray_hits;
            uint8_t *st = tr + 512, *q2 = st;
            /* mid-function site: rsp is already 16-aligned there (game/editor/server prologues), so reserve exactly
             * the 0x20 shadow space; 0x28 (right at a function entry) misaligned the call and crashed in the CRT */
            static const uint8_t sub28[] = { 0x48, 0x83, 0xEC, 0x20 }, add28[] = { 0x48, 0x83, 0xC4, 0x20 };
            static const uint8_t calrax[] = { 0xFF, 0xD0 }, testal[] = { 0x84, 0xC0, 0x74, 0x0E };
            uint8_t movrcx[3] = { (uint8_t)(s.ray_reg >= 8 ? 0x4C : 0x48), 0x89, (uint8_t)(0xC1 | ((s.ray_reg & 7) << 3)) };
            uint8_t movrax[2] = { 0x48, 0xB8 }; uint64_t fn = (uint64_t)(uintptr_t)ms_ray_ok;
            q2 = emit(q2, sub28, 4); q2 = emit(q2, movrcx, 3); q2 = emit(q2, movrax, 2); q2 = emit(q2, &fn, 8);
            q2 = emit(q2, calrax, 2); q2 = emit(q2, add28, 4); q2 = emit(q2, testal, 4);
            q2 = emit_absjmp(q2, s.ray_ok);                          /* al != 0: carry on with the blow */
            emit_absjmp(q2, s.ray_fail);                              /* al == 0: vanilla "blocked" path */
            FlushInstructionCache(GetCurrentProcess(), st, 64);
            uint8_t rp[30]; memset(rp, 0x90, sizeof rp);
            emit_absjmp(rp, st);
            DWORD o4;
            if (VirtualProtect(s.ray_site, 30, PAGE_EXECUTE_READWRITE, &o4)) {
                memcpy(s.ray_site, rp, 30);
                VirtualProtect(s.ray_site, 30, o4, &o4);
                FlushInstructionCache(GetCurrentProcess(), s.ray_site, 30);
                logf_("  melee through shields ON: blow ray test exe+0x%llX (hits exe+0x%llX, target r%d), skips hits linked to the target or tagged %s",
                      (unsigned long long)(s.ray_site - base), (unsigned long long)(s.ray_hits - base), s.ray_reg, cfg_seetag[0]);
            } else logf_("  melee through shields OFF: VirtualProtect failed (%lu)", GetLastError());
        }
    }
    /* instant buttons on several units: point every vtable slot of ExecuteInstant at ms_exinst */
    if (cfg_multi) {
        if (s.inst_err) logf_("  multi-unit instant buttons OFF: %s", s.inst_err);
        else {
            g_exinst = (exinst_t)s.exinst; g_expand = (expand_t)s.expand;
            g_pickctor = (pickctor_t)s.pickctor; g_ordstamp = (volatile uint32_t *)s.ordstamp;
            g_takeitem = (takeitem_t)s.takeitem; g_off_item = s.off_item;
            g_off_apply = s.off_apply; g_off_getactor = s.off_getactor; g_off_canapply = s.off_canapply;
            int ok = 0;
            for (int i = 0; i < s.nexslots; i++) {
                DWORD o3;
                if (!VirtualProtect(s.exslots[i], 8, PAGE_READWRITE, &o3)) continue;
                *s.exslots[i] = (uint8_t *)ms_exinst;
                VirtualProtect(s.exslots[i], 8, o3, &o3);
                ok++;
            }
            logf_("  multi-unit instant buttons ON ({apply_to_all} actions): ExecuteInstant exe+0x%llX in %d/%d vtables, "
                  "apply_to_all +0x%X, GetActor +0x%X (exe+0x%llX), CanApply +0x%X, expand exe+0x%llX",
                  (unsigned long long)(s.exinst - base), ok, s.nexslots, s.off_apply, s.off_getactor,
                  (unsigned long long)(s.getactor - base), s.off_canapply, (unsigned long long)(s.expand - base));
        }
    }
    /* 0.7 auto-draw / vehicles */
    if (s.inst_err || !s.takeitem) { if (cfg_ad_on) logf_("  auto-draw: no take-item order (%s), the hold still refuses guns", s.inst_err ? s.inst_err : "factory not found"); }
    else { g_takeitem = (takeitem_t)s.takeitem; g_ordstamp = (volatile uint32_t *)s.ordstamp; }
    if (s.tc_err) { if (cfg_ad_on) logf_("  auto-draw: item tags cannot be built (%s), the hold still refuses guns", s.tc_err); }
    else { g_tcctor = (tcctor_t)s.tcctor; g_tcfromstr = (tcfromstr_t)s.tcfromstr; }
    if (cfg_ad_on) logf_("  auto-draw ON: Distance=%.0f HoldMs=%u PlayerUnits=%d Item=%s | TagContainer ctor exe+0x%llX FromString exe+0x%llX",
                         cfg_ad_dist, cfg_ad_hold, cfg_ad_player, cfg_ad_item,
                         (unsigned long long)(s.tcctor ? s.tcctor - base : 0), (unsigned long long)(s.tcfromstr ? s.tcfromstr - base : 0));
    if (cfg_ad_on) logf_("  fight range (0.7.16): Probe=%d ProbeMs=%u ProbeCooldownMs=%u NoMeleeMs=%u | FightOrder=%d FightCooldownMs=%u (eFight vt %s)",
                         cfg_probe_on, cfg_probe_ms, cfg_probe_cd, cfg_nomelee_ms, cfg_fight_on, cfg_fight_cd, s.vt_fight ? "found" : "NOT found: no 'already fighting' test");
    g_vt_attack = s.vt_attack; g_vt_attackb = s.vt_attackb; g_vt_fight = s.vt_fight;
    if (cfg_veh_on && !s.vt_attack && !s.vt_fight) logf_("  vehicles: order vtables not found, player attack orders on vehicles are not recognised");
    if (cfg_veh_on) logf_("  vehicles ON: stance units melee vehicles with %s ammo, infantry first (InfantryMs=%u within InfantryRange=%.0f, crews excluded), close order %s from CloseDistance=%.0f, "
                          "hull contact: a unit tagged %s (script: touching a ram volume) or stalled within AdjacentMax=%.0f of the vehicle centre has its melee range measured from where it stands for AdjacentMs=%u",
                          cfg_vtags[0], cfg_inf_ms, cfg_inf_range, cfg_veh_attack ? "attack" : "eFight", cfg_close_veh, n_contacttag ? cfg_contacttag[0] : "-", cfg_adj_extra, cfg_adj_ms);
    /* 0.7.1 reload cancel */
    if (cfg_rc_on) {
        if (s.rc_err) logf_("  reload cancel OFF: %s", s.rc_err);
        else {
            g_invbusy = (invbusy_t)s.invbusy; g_invcancel = (invcancel_t)s.invcancel; g_curweap = (curweap_t)s.curweap;
            char kl[800] = "";
            for (int i = 0; i < n_keep; i++) { strcat(kl, cfg_keep[i]); if (i + 1 < n_keep) strcat(kl, ","); }
            logf_("  reload cancel ON: a melee draw drops the unit's running inventory op (its reload) unless the held weapon carries one of KeepTags=%s | "
                  "busy exe+0x%llX, cancel exe+0x%llX, GetCurrentWeapon exe+0x%llX", kl,
                  (unsigned long long)(s.invbusy - base), (unsigned long long)(s.invcancel - base), (unsigned long long)(s.curweap - base));
        }
    }
    /* 0.7.11 direct control */
    if (cfg_manual_skip) {
        if (s.manual_err && s.mgr_err) logf_("  direct control: %s, %s - stance / charge / auto-draw also run for units under direct control", s.manual_err, s.mgr_err);
        else {
            g_ismanual = (ismanual_t)s.ismanual;
            g_mgr = s.mgr;
            char a[64], b[64];
            if (s.ismanual) snprintf(a, sizeof a, "IsManualControlled exe+0x%llX", (unsigned long long)(s.ismanual - base)); else snprintf(a, sizeof a, "%s", s.manual_err);
            if (s.mgr) snprintf(b, sizeof b, "manager exe+0x%llX (actor +0x3B0)", (unsigned long long)((uint8_t *)s.mgr - base)); else snprintf(b, sizeof b, "%s", s.mgr_err);
            logf_("  direct control: a unit under manual control is left alone (no stance ammo filter, charge, auto-draw or reload cancel) | %s | %s", a, b);
        }
    }
    /* blow reach: overwrite the engine's near/medium/far table */
    if (cfg_down_on) logf_("  downed targets ignored: no weapon is chosen against units tagged %s", cfg_downtag[0]);
    if (cfg_blow_on) {
        if (!s.blowtab) logf_("  blow reach OFF: table not found (unknown exe build, or already changed)");
        else {
            DWORD o2;
            if (VirtualProtect(s.blowtab, 12, PAGE_READWRITE, &o2)) {
                s.blowtab[0] = cfg_blow[0]; s.blowtab[1] = cfg_blow[1]; s.blowtab[2] = cfg_blow[2];
                VirtualProtect(s.blowtab, 12, o2, &o2);
                logf_("  blow reach ON: near/medium/far 17/29/35 -> %.1f/%.1f/%.1f units (table exe+0x%llX, GetBlowDistance exe+0x%llX)",
                      cfg_blow[0], cfg_blow[1], cfg_blow[2], (unsigned long long)((uint8_t *)s.blowtab - base),
                      (unsigned long long)(s.blowfn - base));
            } else logf_("  blow reach OFF: VirtualProtect failed (%lu)", GetLastError());
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH];
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.');
        if (dot) *dot = 0;
        char exe[MAX_PATH], *nm;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path,
                 strstr(nm, "_ed") ? "_editor" : (strstr(nm, "_server") ? "_server" : ""));
        load_ini(ini);
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
static void fake_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    memset(p, 0, 0x20);
    if (n <= 15) { memcpy(p, s, n); *(uint64_t *)(p + 0x18) = 15; }
    else { char *h = _strdup(s); *(char **)p = h; *(uint64_t *)(p + 0x18) = n; }
    *(uint64_t *)(p + 0x10) = n;
}
static void fake_vec(uint8_t *at, uint8_t *store, const char *a, const char *b, const char *c)
{
    const char *t[3] = { a, b, c }; int n = 0;
    for (int i = 0; i < 3; i++) if (t[i]) fake_str(store + 0x60 * n++ + 0x40, t[i]);
    *(uint8_t **)at = store; *(uint8_t **)(at + 8) = store + 0x60 * n;
}
typedef struct { uint8_t obj[0x1400]; uint8_t tags[0x60 * 4]; uint8_t names[0x60 * 4]; } fake_t;

static void *g_seen_ready;
static __attribute__((ms_abi)) void *fake_orig(uint8_t *ctx) { g_seen_ready = (void *)(uintptr_t)ctx[0x18]; return (void *)0x1234; }
/* fake engine for the charge path */
static uint32_t f_clock = 1000, f_stamp = 77;
static int f_orders; static void *f_last_unit, *f_last_tgt;
static __attribute__((ms_abi)) void f_nop(void *p) { (void)p; }
static __attribute__((ms_abi)) void f_setent(void *ref, void *ent, void *tag) { (void)tag; *(uint32_t *)((uint8_t *)ref + 8) = 1; *(void **)((uint8_t *)ref + 0x18) = ent; }
static __attribute__((ms_abi)) void *f_mkatk(void *ref, uint8_t flag) { (void)flag; f_last_tgt = *(void **)((uint8_t *)ref + 0x18); return (void *)0x99; }
static __attribute__((ms_abi)) void *f_add(void *a, void *o, void *pr) { f_orders++; f_last_unit = a; return ((uint32_t *)pr)[0] == 1 && ((uint32_t *)pr)[1] == 77 ? o : NULL; }
static __attribute__((ms_abi)) float f_range(void *s) { (void)s; return 3.5f; }
/* fake inventory for the reload cancel */
static int f_busy_v, f_cancels; static void *f_weap;
static __attribute__((ms_abi)) int   f_busy(const void *h) { (void)h; return f_busy_v; }
static __attribute__((ms_abi)) void  f_cancel(void *u, void *h) { (void)u; (void)h; f_cancels++; }
static __attribute__((ms_abi)) void *f_curweap(void *holder) { (void)holder; return f_weap; }
static int f_manual_v; static __attribute__((ms_abi)) unsigned char f_manual(const void *a) { (void)a; return (unsigned char)f_manual_v; }   /* 0.7.11 */

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        HMODULE m = LoadLibraryExA(argv[i], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[i], GetLastError()); bad = 1; continue; }
        uint8_t *base = (uint8_t *)m;
        sites_t s;
        const char *err = find_all(base, &s);
        if (err) { printf("%s: FAIL %s\n", argv[i], err); bad = 1; continue; }
        printf("%s\n  ammo match exe+0x%llX, ammo {tag} +0x%X, linked-flag sites %d\n", argv[i],
               (unsigned long long)(s.fn - base), s.ammotags, s.linksites);
        if (s.ammotags != 0xEC8 || s.linksites < 20) { printf("  FAIL unexpected offsets\n"); bad = 1; }
        if (s.ray_err) { printf("  FAIL shield melee: %s\n", s.ray_err); bad = 1; }
        else printf("  shield melee: ray test exe+0x%llX hits exe+0x%llX ok exe+0x%llX fail exe+0x%llX target r%d\n",
                    (unsigned long long)(s.ray_site - base), (unsigned long long)(s.ray_hits - base),
                    (unsigned long long)(s.ray_ok - base), (unsigned long long)(s.ray_fail - base), s.ray_reg);
        if (!s.takeitem) { printf("  FAIL take-item order factory not found\n"); bad = 1; }
        else printf("  draw: TakeItem order exe+0x%llX, action {item} +0x%X\n", (unsigned long long)(s.takeitem - base), s.off_item);
        if (s.tc_err) { printf("  FAIL auto-draw tags: %s\n", s.tc_err); bad = 1; }
        else printf("  auto-draw: TagContainer ctor exe+0x%llX FromString exe+0x%llX\n", (unsigned long long)(s.tcctor - base), (unsigned long long)(s.tcfromstr - base));
        printf("  orders: eAttack vt exe+0x%llX eAttackBasic vt exe+0x%llX eFight vt exe+0x%llX\n", (unsigned long long)(s.vt_attack ? (uint8_t *)s.vt_attack - base : 0), (unsigned long long)(s.vt_attackb ? (uint8_t *)s.vt_attackb - base : 0), (unsigned long long)(s.vt_fight ? (uint8_t *)s.vt_fight - base : 0));
        if (!s.vt_attack || !s.vt_attackb || !s.vt_fight) { printf("  FAIL order vtables\n"); bad = 1; }
        if (s.inst_err) { printf("  FAIL multi-select: %s\n", s.inst_err); bad = 1; }
        else printf("  multi-select: ExecuteInstant exe+0x%llX in %d vtables, apply_to_all +0x%X, GetActor exe+0x%llX (+0x%X), CanApply +0x%X, expand exe+0x%llX\n",
                    (unsigned long long)(s.exinst - base), s.nexslots, s.off_apply, (unsigned long long)(s.getactor - base),
                    s.off_getactor, s.off_canapply, (unsigned long long)(s.expand - base));
        if (!s.blowtab) { printf("  FAIL blow reach table not found\n"); bad = 1; }
        else printf("  blow reach: table exe+0x%llX = %.0f/%.0f/%.0f, GetBlowDistance exe+0x%llX\n",
                    (unsigned long long)((uint8_t *)s.blowtab - base), s.blowtab[0], s.blowtab[1], s.blowtab[2],
                    (unsigned long long)(s.blowfn - base));
        if (!s.mkmove) { printf("  FAIL move order (MakeMove) not found\n"); bad = 1; }
        else printf("  move: MakeMove exe+0x%llX\n", (unsigned long long)(s.mkmove - base));
        if (!s.fight) { printf("  FAIL eFight (attack_melee order) factory not found\n"); bad = 1; }
        else printf("  melee: eFight exe+0x%llX\n", (unsigned long long)(s.fight - base));
        if (!s.mvchange) { printf("  FAIL eMovementChange factory not found\n"); bad = 1; }
        else printf("  mode: eMovementChange exe+0x%llX\n", (unsigned long long)(s.mvchange - base));
        if (!s.uimove) { printf("  FAIL UI click-move factory not found\n"); bad = 1; }
        else printf("  click move: exe+0x%llX\n", (unsigned long long)(s.uimove - base));
        if (s.rc_err) { printf("  FAIL reload cancel: %s\n", s.rc_err); bad = 1; }
        else printf("  reload cancel: InventoryBusy exe+0x%llX CancelInventoryOps exe+0x%llX GetCurrentWeapon exe+0x%llX\n",
                    (unsigned long long)(s.invbusy - base), (unsigned long long)(s.invcancel - base), (unsigned long long)(s.curweap - base));
        if (s.manual_err) { printf("  FAIL direct control: %s\n", s.manual_err); bad = 1; }
        else printf("  direct control: IsManualControlled exe+0x%llX\n", (unsigned long long)(s.ismanual - base));
        if (s.mgr_err) { if (strstr(argv[i], "server")) printf("  manual-control manager: %s (server exe: expected)\n", s.mgr_err); else { printf("  FAIL manual-control manager: %s\n", s.mgr_err); bad = 1; } }
        else printf("  manual-control manager: exe+0x%llX\n", (unsigned long long)((uint8_t *)s.mgr - base));
        if (s.charge_err) { printf("  FAIL charge: %s\n", s.charge_err); bad = 1; continue; }
#define R(x) (unsigned long long)((x) - base)
        printf("  charge: scan exe+0x%llX..0x%llX range exe+0x%llX MakeAttack exe+0x%llX AddOrder exe+0x%llX\n"
               "          MakeRef exe+0x%llX SetEntity exe+0x%llX RefInit exe+0x%llX TagCtor exe+0x%llX refvt exe+0x%llX\n"
               "          RefDtor exe+0x%llX TagDtor exe+0x%llX stamp exe+0x%llX clock exe+0x%llX\n",
               R(s.scan_lo), R(s.scan_hi), R(s.range), R(s.mkatk), R(s.addorder), R(s.mkref), R(s.setent), R(s.refinit),
               R(s.tagctor), R(s.refvt), R(s.refdtor), R(s.tagdtor), R(s.ordstamp), R(s.clock));
        if (s.reflist) printf("  refs: entity ref list at +0x%X\n", s.reflist);
        else { printf("  FAIL refs: entity ref list offset not found in RefDtor\n"); bad = 1; }
    }
    cfg_log = 0; init_tag(); g_orig = (match_t)fake_orig;
    static fake_t man, sm_inst, crew, tank, sword, bolter, nade, hvy, enemy;
    memset(&man, 0, sizeof man); man.obj[0x30] = 6;
    fake_vec(man.obj + cfg_enttags, man.tags, "faction_sm", "Melee_Stance", NULL);
    memset(&sm_inst, 0, sizeof sm_inst); sm_inst.obj[0x30] = 6;
    fake_vec(sm_inst.obj + cfg_enttags, sm_inst.tags, "faction_sm", "has_melee_weapon", NULL);
    memset(&crew, 0, sizeof crew); crew.obj[0x30] = 6;
    fake_vec(crew.obj + cfg_enttags, crew.tags, "melee_stance", NULL, NULL);
    *(uint32_t *)(crew.obj + 0x1C0) = 1u << 28;
    memset(&tank, 0, sizeof tank); tank.obj[0x30] = 2;
    fake_vec(tank.obj + cfg_enttags, tank.tags, "melee_stance", NULL, NULL);
    memset(&sword, 0, sizeof sword);
    fake_vec(sword.obj + cfg_ammotags + TAGS_VEC, sword.tags, "melee_light", "pierce", NULL);
    memset(&hvy, 0, sizeof hvy);
    fake_vec(hvy.obj + cfg_ammotags + TAGS_VEC, hvy.tags, "pierce", "melee_heavy", NULL);
    memset(&bolter, 0, sizeof bolter);
    fake_vec(bolter.obj + cfg_ammotags + TAGS_VEC, bolter.tags, "bolt", "small_arms", "pierce");
    memset(&nade, 0, sizeof nade);
    fake_vec(nade.obj + cfg_ammotags + TAGS_VEC, nade.tags, "grenade", "fg", NULL);
    memset(&enemy, 0, sizeof enemy); enemy.obj[0x30] = 6; enemy.obj[0xDA8] = 1;
    struct { fake_t *u, *a; int want; const char *what; } c[] = {
        { &man, &sword, MELEE, "stance + chainsword ammo" },
        { &man, &hvy, MELEE, "stance + heavy melee ammo" },
        { &man, &bolter, REJECT, "stance + bolter ammo" },
        { &man, &nade, REJECT, "stance + grenade" },
        { &sm_inst, &bolter, PASS, "no stance + bolter" },
        { &sm_inst, &sword, PASS, "no stance + chainsword" },
        { &crew, &bolter, PASS, "stance but linked (crew)" },
        { &tank, &bolter, PASS, "non-human tagged" },
    };
    int fails = 0;
    cfg_charge = 0;
    cfg_probe_on = 0; cfg_fight_on = 0;                       /* 0.7.16 rules: own block at the end; the older cases test the old paths */
    cfg_known_ms = 0; cfg_poll_on = 0;                        /* 0.7.18: same (the old cases expect the 20 s melee memory) */
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        uint8_t ctx[0x20] = { 0 };
        *(void **)ctx = c[i].a->obj; *(void **)(ctx + 8) = c[i].u->obj;
        int got = ms_decide(ctx);
        if (got != c[i].want) { printf("  FAIL %s: got %d\n", c[i].what, got); fails++; }
    }
    {
        uint8_t ctx[0x20] = { 0 };
        *(void **)ctx = bolter.obj; *(void **)(ctx + 8) = man.obj;
        if (ms_hook(ctx) != NULL) { printf("  FAIL hook reject\n"); fails++; }
        *(void **)ctx = sword.obj; g_seen_ready = (void *)7;
        if (ms_hook(ctx) != (void *)0x1234 || g_seen_ready != (void *)1 || ctx[0x18] != 0) { printf("  FAIL hook melee/ready\n"); fails++; }
        *(void **)(ctx + 8) = sm_inst.obj; *(void **)ctx = bolter.obj; g_seen_ready = (void *)7;
        if (ms_hook(ctx) != (void *)0x1234 || g_seen_ready != (void *)0) { printf("  FAIL hook pass\n"); fails++; }
    }
    /* charge path on a fake engine (charge_note / charge_flush directly; the return-address gate is the finder's job) */
    {
        cfg_charge = 1;
        g_tagctor = g_tagdtor = g_refinit = g_refdtor = (vfn1_t)f_nop; g_setent = (setent_t)f_setent;
        g_mkatk = (mkatk_t)f_mkatk; g_addorder = (addord_t)f_add; g_range = (range_t)f_range;
        g_clock = &f_clock; g_ordstamp = &f_stamp; g_refvt = (void *)0x55;
        uint8_t ti[0x40] = { 0 };
        *(void **)(ti + 0x18) = enemy.obj;
        *(float *)(ti + 0x28) = 100.0f;                      /* enemy 100 units away on X */
        uint8_t ctx[0x20] = { 0 };
        *(void **)ctx = sword.obj; *(void **)(ctx + 8) = sm_inst.obj; *(void **)(ctx + 0x10) = ti;
        charge_note(ctx, 0, 0);                              /* not in hand -> nothing */
        int q0 = (int)st_charge_q;
        charge_note(ctx, 1, 0);                              /* in hand, has_melee_weapon -> queued */
        int q1 = (int)st_charge_q;
        charge_flush(NULL);                                  /* same tick -> nothing yet */
        int o0 = f_orders;
        f_clock += 50;
        charge_flush(sm_inst.obj);                           /* the unit itself is scanning -> wait */
        int o1 = f_orders;
        charge_flush(man.obj);                               /* another unit's scan, later tick -> order */
        int o2 = f_orders;
        charge_note(ctx, 1, 0);                              /* same target inside the cooldown -> nothing */
        int q2 = (int)st_charge_q;
        *(float *)(ti + 0x28) = 3.0f;                        /* in reach */
        f_clock += 10000;
        charge_note(ctx, 1, 0);
        int q3 = (int)st_charge_q;
        *(void **)ctx = bolter.obj; *(float *)(ti + 0x28) = 100.0f;
        charge_note(ctx, 1, 0);                              /* ranged ammo -> nothing */
        int q4 = (int)st_charge_q;
        *(void **)ctx = sword.obj; *(void **)(ctx + 8) = man.obj;
        charge_note(ctx, 0, 1);                              /* stance unit, sword on the belt -> queued */
        int q5 = (int)st_charge_q;
        /* always tag counts as stance; beyond MaxDistance and inside MinDistance nothing */
        static fake_t brute; memset(&brute, 0, sizeof brute); brute.obj[0x30] = 6;
        fake_vec(brute.obj + cfg_enttags, brute.tags, "faction_tg", "melee_always", NULL);
        uint8_t c2[0x20] = { 0 };
        *(void **)c2 = sword.obj; *(void **)(c2 + 8) = brute.obj;
        ms_decide(c2);                                       /* his chainsword shows up in a scan */
        *(void **)c2 = bolter.obj;
        int al = ms_decide(c2) == REJECT;
        *(void **)c2 = sword.obj; *(void **)(c2 + 0x10) = ti;
        *(float *)(ti + 0x28) = 2500.0f; charge_note(c2, 0, 1);
        int q6 = (int)st_charge_q;
        *(float *)(ti + 0x28) = 40.0f; charge_note(c2, 0, 1);
        int q7 = (int)st_charge_q;
        *(float *)(ti + 0x28) = 1900.0f; charge_note(c2, 0, 1);
        int q8 = (int)st_charge_q;
        /* with a clock: a stance unit never seen with melee ammo keeps its guns */
        static fake_t lasgunner; memset(&lasgunner, 0, sizeof lasgunner); lasgunner.obj[0x30] = 6;
        fake_vec(lasgunner.obj + cfg_enttags, lasgunner.tags, "melee_stance", NULL, NULL);
        uint8_t c3[0x20] = { 0 };
        *(void **)c3 = bolter.obj; *(void **)(c3 + 8) = lasgunner.obj;
        int lg = ms_decide(c3) == PASS;
        *(void **)c3 = sword.obj; ms_decide(c3);             /* now he shows a melee weapon */
        *(void **)c3 = bolter.obj;
        int lg2 = ms_decide(c3) == REJECT;
        f_clock += SEEN_MS + 1;                              /* melee weapon gone for 20 s */
        int lg3 = ms_decide(c3) == PASS;
        int ok = q0 == 0 && q1 == 1 && o0 == 0 && o1 == 0 && o2 == 1 && f_last_unit == sm_inst.obj && f_last_tgt == enemy.obj
                 && q2 == 1 && q3 == 1 && q4 == 1 && q5 == 2 && al && q6 == 2 && q7 == 2 && q8 == 3 && lg && lg2 && lg3;
        if (!(lg && lg2 && lg3)) printf("  FAIL carried-melee gate %d %d %d\n", lg, lg2, lg3);
        /* 0.7.13 refresh threshold: 40 or 40 %% of the distance */
        int rf = !goal_moved(500.0f, 100.0f, 0.0f) && goal_moved(500.0f, 250.0f, 0.0f) && goal_moved(80.0f, 45.0f, 0.0f) && !goal_moved(80.0f, 30.0f, 0.0f);
        cfg_refresh_frac = 0.0f; rf = rf && goal_moved(500.0f, 100.0f, 0.0f); cfg_refresh_frac = 0.4f;
        printf("refresh threshold: %s\n", rf ? "OK" : "FAIL"); if (!rf) fails++;
        /* 0.7.13 per-mission log caps */
        g_clines = 150; log_epoch(100000); log_epoch(100500); int ep1 = g_clines == 150; log_epoch(2000); int ep2 = g_clines == 0; g_epoch_last = 0;
        printf("log caps per mission: same %d reset %d -> %s\n", ep1, ep2, ep1 && ep2 ? "OK" : "FAIL"); if (!(ep1 && ep2)) fails++;
        printf("charge: queue %d/%d flush %d/%d/%d cooldown %d reach %d ranged %d stance %d always %d far %d close %d 95m %d -> %s\n",
               q0, q1, o0, o1, o2, q2, q3, q4, q5, al, q6, q7, q8, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    /* shield melee ray: [shield linked to target, target] passes; [wall, target] blocks; [target] passes; tagged item passes */
    {
        static fake_t tgt, shield, wall, kit; static uint8_t link[0x20];
        memset(&tgt, 0, sizeof tgt); memset(&shield, 0, sizeof shield); memset(&wall, 0, sizeof wall); memset(&kit, 0, sizeof kit);
        *(uint32_t *)(shield.obj + 0x1C0) = 1u << 28; *(uint8_t **)(shield.obj + 0x560) = link; *(uint8_t **)(link + 8) = tgt.obj;
        fake_vec(kit.obj + cfg_enttags, kit.tags, "forcefield", NULL, NULL);
        static uint8_t recs[0x48 * 3]; static uint8_t *vec[2];
        g_ray_hits = (uint8_t *)vec; cfg_ray = 1;
#define HITS(a, b2) do { memset(recs, 0, sizeof recs); *(void **)(recs + 0x18) = (a); *(void **)(recs + 0x48 + 0x18) = (b2); vec[0] = recs; vec[1] = recs + 0x48 * ((b2) ? 2 : 1); } while (0)
        HITS(shield.obj, tgt.obj); int r1 = ms_ray_ok(tgt.obj);
        HITS(wall.obj, tgt.obj);   int r2 = ms_ray_ok(tgt.obj);
        HITS(tgt.obj, NULL);       int r3 = ms_ray_ok(tgt.obj);
        HITS(kit.obj, tgt.obj);    int r4 = ms_ray_ok(tgt.obj);
        HITS(shield.obj, NULL);    int r5 = ms_ray_ok(tgt.obj);
        int ok = r1 == 1 && r2 == 0 && r3 == 1 && r4 == 1 && r5 == 0;
        printf("shield ray: shield+target %d wall+target %d target %d tagged+target %d shield-only %d -> %s\n", r1, r2, r3, r4, r5, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    /* 0.7 vehicles: only stance units melee a vehicle, infantry first, an explicit (not dll) order on it wins */
    {
        static fake_t veh, guard, foe; static uint8_t ti[0x40], brain[0x400], order[0x100]; static void *blk[2], *map[1];
        memset(&veh, 0, sizeof veh); veh.obj[0x30] = 2; veh.obj[0xDA8] = 1;
        memset(&foe, 0, sizeof foe); foe.obj[0x30] = 6; foe.obj[0xDA8] = 1;
        uint8_t cv[0x20] = { 0 };
        *(void **)(ti + 0x18) = veh.obj; *(void **)(cv + 0x10) = ti;
        *(void **)cv = hvy.obj;
        *(void **)(cv + 8) = sm_inst.obj; int v1 = ms_decide(cv) == REJECT;           /* no stance: refused */
        static fake_t st; memset(&st, 0, sizeof st); st.obj[0x30] = 6;
        fake_vec(st.obj + cfg_enttags, st.tags, "melee_stance", NULL, NULL);
        *(void **)(cv + 8) = st.obj; int v2 = ms_decide(cv) == MELEE;                  /* stance, no infantry seen */
        static uint8_t tf[0x40]; uint8_t cf[0x20] = { 0 };
        *(void **)(tf + 0x18) = foe.obj; *(float *)(tf + 0x28) = 300.0f;                /* 0.7.8: within InfantryRange 400 */
        *(void **)cf = hvy.obj; *(void **)(cf + 8) = st.obj; *(void **)(cf + 0x10) = tf;
        scan_note(cf);                                                                 /* enemy infantry at 300 */
        int v3 = ms_decide(cv) == REJECT;                                              /* infantry first */
        /* explicit order on the vehicle at the front of the brain queue */
        g_vt_attack = (void **)0x7777;
        *(void **)order = (void *)0x7777; *(uint32_t *)(order + 0x48) = 1; *(void **)(order + 0x58) = veh.obj;
        blk[0] = order; map[0] = blk;
        *(uint32_t *)(brain + 0x39C) = 0x1000; *(int32_t *)(brain + 0x88) = 1;
        *(void **)(brain + 0x70) = map; *(uint64_t *)(brain + 0x78) = 1; *(uint64_t *)(brain + 0x80) = 0;
        *(uint8_t **)(st.obj + 0xDC8) = brain;
        int v4 = ms_decide(cv) == MELEE;                                               /* ordered: melee it despite infantry */
        mine_add(order);
        int v5 = ms_decide(cv) == MELEE;                                               /* 0.7.1: the dll's own order commits too */
        f_clock += 3001;
        int v6 = ms_decide(cv) == MELEE;                                               /* infantry not seen for InfantryMs */
        /* auto-draw: a gunner with a knife and an enemy inside Distance refuses guns for HoldMs */
        memset(&guard, 0, sizeof guard); guard.obj[0x30] = 6;
        fake_vec(guard.obj + cfg_enttags, guard.tags, "faction_ig", NULL, NULL);
        uint8_t cg[0x20] = { 0 };
        *(void **)cg = sword.obj; *(void **)(cg + 8) = guard.obj; *(void **)(cg + 0x10) = tf;
        ms_decide(cg);                                                                 /* his knife shows up in a scan */
        *(void **)cg = bolter.obj;
        int a1 = ms_decide(cg) == PASS;                                                /* enemy at 500: keeps the gun */
        g_takeitem = (takeitem_t)(void *)f_nop;                                        /* only checked for non-NULL here */
        *(float *)(tf + 0x28) = 80.0f; scan_note(cg);
        int a2 = ms_decide(cg) == REJECT;                                              /* enemy at 80: gun refused */
        int a3 = 0; for (int i = 0; i < NDPEND; i++) if (g_dpend[i].used && g_dpend[i].unit == guard.obj) a3 = 1;   /* draw queued */
        f_clock += cfg_ad_hold + 1;
        int a4 = ms_decide(cg) == PASS;                                                /* hold ran out */
        int ok = v1 && v2 && v3 && v4 && v5 && v6 && a1 && a2 && a3 && a4;
        printf("vehicles: no-stance %d stance %d inf-first %d ordered %d own-order %d inf-gone %d | auto-draw: far %d close %d queued %d expired %d -> %s\n",
               v1, v2, v3, v4, v5, v6, a1, a2, a3, a4, ok ? "OK" : "FAIL");
        if (!ok) fails++;
        g_vt_attack = NULL;
    }
    /* 0.7.3 hull contact: the shift only while the unit is marked pressed against THIS vehicle, and only beyond reach x 0.8 */
    {
        static fake_t marine, tank2; static uint8_t ti2[0x40];
        memset(&marine, 0, sizeof marine); marine.obj[0x30] = 6;
        memset(&tank2, 0, sizeof tank2); tank2.obj[0x30] = 2; tank2.obj[0xDA8] = 1;
        *(void **)(ti2 + 0x18) = tank2.obj; *(float *)(ti2 + 0x28) = 88.0f;            /* unit at the origin, centre 88 units east */
        uint8_t ca[0x20] = { 0 };
        *(void **)ca = sword.obj; *(void **)(ca + 8) = marine.obj; *(void **)(ca + 0x10) = ti2;
        float sv[3];
        int h1 = adj_shift(ca, sv) == 0;                                                /* not marked */
        last_t *l = last_of(marine.obj, 1); l->adj_tgt = tank2.obj; l->adj_until = f_clock + 1500;
        int h2 = adj_shift(ca, sv) == 1 && sv[0] == 88.0f && *(float *)(ti2 + 0x28) > 9.9f && *(float *)(ti2 + 0x28) < 10.1f;   /* ADJ_AHEAD = 10 units in front */
        *(float *)(ti2 + 0x28) = 88.0f;
        f_clock += 1600; int h3 = adj_shift(ca, sv) == 0;                               /* window over */
        fake_vec(marine.obj + cfg_enttags, marine.tags, "hull_contact", NULL, NULL);     /* 0.7.4: script tag; no attack order on the fake, so the vehicle must be within AdjacentMax / 2 (0.7.6: 60) */
        int h4a = adj_shift(ca, sv) == 0;                                               /* 88 from the centre: too far without an order */
        *(float *)(ti2 + 0x28) = 50.0f;
        int h4 = h4a && adj_shift(ca, sv) == 1 && sv[0] == 50.0f;
        *(float *)(ti2 + 0x28) = 88.0f;
        int ok = h1 && h2 && h3 && h4;
        printf("hull contact: unmarked %d shifted %d expired %d tagged %d -> %s\n", h1, h2, h3, h4, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    /* 0.7.11 direct control: a stance unit under manual control gets PASS for its guns and no charge; back to normal when it ends / disabled */
    {
        static fake_t dc; memset(&dc, 0, sizeof dc); dc.obj[0x30] = 6;
        fake_vec(dc.obj + cfg_enttags, dc.tags, "melee_stance", NULL, NULL);
        static uint8_t ti3[0x40]; memset(ti3, 0, sizeof ti3);
        *(void **)(ti3 + 0x18) = enemy.obj; *(float *)(ti3 + 0x28) = 300.0f;
        uint8_t ctx[0x20] = { 0 };
        *(void **)ctx = bolter.obj; *(void **)(ctx + 8) = dc.obj; *(void **)(ctx + 0x10) = ti3;
        g_ismanual = (ismanual_t)f_manual;
        /* 0.7.32: a unit missing either component IsManualControlled reads is never asked (the engine function would fault) */
        f_manual_v = 1; int m7 = !ismanual_safe(dc.obj);
        static uint8_t fcomp[0x80]; *(void **)(dc.obj + 0xDE0) = fcomp; int m7b = !ismanual_safe(dc.obj);   /* only one of the two */
        *(void **)(dc.obj + 0xDD8) = fcomp; int m7c = ismanual_safe(dc.obj) == 1;                                /* both: the engine is asked */
        f_manual_v = 0; *(void **)ctx = sword.obj; int m0 = ms_decide(ctx) == MELEE;                             /* carries melee (the filter only refuses guns of a unit seen with melee ammo) */
        f_manual_v = 1; *(void **)ctx = bolter.obj; int m1 = m0 && ms_decide(ctx) == PASS && st_manual == 1;
        *(void **)ctx = sword.obj; uint32_t q0 = st_charge_q; charge_note(ctx, 1, 1); int m2 = st_charge_q == q0;   /* no charge while driven */
        f_manual_v = 0; charge_note(ctx, 1, 1); int m2b = st_charge_q == q0 + 1;                                   /* same unit, released -> queued */
        *(void **)ctx = bolter.obj; int m3 = ms_decide(ctx) == REJECT;
        cfg_manual_skip = 0; f_manual_v = 1; int m4 = ms_decide(ctx) == REJECT; cfg_manual_skip = 1; f_manual_v = 0;
        static uint8_t fmgr[0x400]; static uint8_t *fmgr_p = fmgr; memset(fmgr, 0, sizeof fmgr);           /* 0.7.12: property off, manager drives dc */
        *(void **)(fmgr + 0x3B0) = dc.obj; g_mgr = &fmgr_p;
        int m5 = ms_decide(ctx) == PASS; q0 = st_charge_q; charge_note(ctx, 1, 1); int m5b = st_charge_q == q0;
        *(void **)(fmgr + 0x3B0) = enemy.obj; int m6 = ms_decide(ctx) == REJECT;                              /* another unit driven -> dc back to normal */
        g_mgr = NULL;
        g_ismanual = NULL;
        int ok = m1 && m2 && m2b && m3 && m4 && m5 && m5b && m6 && m7 && m7b && m7c;
        printf("direct control: guns pass %d no charge %d charge after %d normal %d disabled %d | manager: pass %d no charge %d other unit %d | missing component: none %d one %d both %d -> %s\n", m1, m2, m2b, m3, m4, m5, m5b, m6, m7, m7b, m7c, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    /* 0.7.1 reload cancel: idle inventory -> nothing; busy + heavy weapon in hand -> kept; busy + bolter -> cancelled */
    {
        static fake_t gunner, hbparams; static uint8_t weapon[0x100], rrec[0x20];
        memset(&gunner, 0, sizeof gunner); gunner.obj[0x30] = 6; *(void **)(gunner.obj + 0xDD8) = (void *)0x1;
        *(void **)(weapon + 0xB0) = rrec; *(uint32_t *)(rrec + 0x10) = 1234;                 /* reload running */
        memset(&hbparams, 0, sizeof hbparams);
        fake_vec(hbparams.obj + cfg_ammotags + TAGS_VEC, hbparams.tags, "bolt", "SM_Weapon_HeavyBolter", NULL);
        g_invbusy = (invbusy_t)f_busy; g_invcancel = (invcancel_t)f_cancel; g_curweap = (curweap_t)f_curweap;
        *(void **)(weapon + 0x98) = hbparams.obj; f_weap = weapon;
        f_busy_v = 0; int r1 = reload_cancel(gunner.obj) == 0 && f_cancels == 0;
        f_busy_v = 1; int r2 = reload_cancel(gunner.obj) == 0 && f_cancels == 0 && st_rc_keep == 1;
        *(void **)(weapon + 0x98) = bolter.obj; int r3 = reload_cancel(gunner.obj) == 1 && f_cancels == 1;
        *(uint32_t *)(rrec + 0x10) = 0; int r3b = reload_cancel(gunner.obj) == 1 && f_cancels == 2;   /* 0.7.7: busy -> cancel whatever the record says */
        *(uint32_t *)(rrec + 0x10) = 1234;
        f_weap = NULL; int r4 = reload_cancel(gunner.obj) == 1 && f_cancels == 3;      /* nothing in hand: cancel */
        cfg_rc_on = 0; int r5 = reload_cancel(gunner.obj) == 0 && f_cancels == 3; cfg_rc_on = 1;
        int ok = r1 && r2 && r3 && r3b && r4 && r5;
        printf("reload cancel: idle %d heavy-kept %d bolter-cancelled %d busy-no-record %d empty-hand %d disabled %d -> %s\n", r1, r2, r3, r3b, r4, r5, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    /* 0.7.16 probe + fight order */
    {
        g_ismanual = NULL; g_mgr = NULL; cfg_probe_on = 1; cfg_fight_on = 1; cfg_charge = 1;
        g_takeitem = (takeitem_t)(void *)f_nop;
        static fake_t kn, gn, ft; static uint8_t tp[0x40], fbrain[0x400], forder[0x100]; static void *fblk[2], *fmap[1];
        memset(&kn, 0, sizeof kn); kn.obj[0x30] = 6; fake_vec(kn.obj + cfg_enttags, kn.tags, "faction_sm", NULL, NULL);
        memset(&gn, 0, sizeof gn); gn.obj[0x30] = 6; fake_vec(gn.obj + cfg_enttags, gn.tags, "faction_ig", NULL, NULL);
        memset(tp, 0, sizeof tp); *(void **)(tp + 0x18) = enemy.obj; *(float *)(tp + 0x28) = 80.0f;   /* enemy 80 units out: fight range */
        uint8_t ck[0x20] = { 0 }, cn[0x20] = { 0 };
        *(void **)ck = bolter.obj; *(void **)(ck + 8) = kn.obj; *(void **)(ck + 0x10) = tp;
        *(void **)cn = bolter.obj; *(void **)(cn + 8) = gn.obj; *(void **)(cn + 0x10) = tp;
        f_clock += 100000;
        uint32_t s0 = st_probe;
        int p0 = ms_decide(ck) == PASS;                                                /* no probe yet: gun passes */
        scan_note(ck); int p1 = st_probe == s0 + 1;                                     /* enemy close, no melee seen -> probe */
        int p2 = ms_decide(ck) == REJECT;                                               /* bolter refused during the probe */
        *(void **)ck = sword.obj; int p3 = ms_decide(ck) == MELEE && carries_melee(kn.obj) && st_probe_ok == 1;   /* knife offered -> found */
        *(void **)ck = bolter.obj; scan_note(ck); int p4 = st_probe == s0 + 1;         /* carries melee now: no new probe (auto-draw hold) */
        scan_note(cn); int p5 = st_probe == s0 + 2;                                     /* the lasgunner: probe */
        int p6 = ms_decide(cn) == REJECT;
        f_clock += cfg_probe_ms; scan_note(cn); int p7 = st_probe_fail == 1;           /* only guns offered -> no melee */
        int p8 = ms_decide(cn) == PASS;                                                 /* keeps his gun */
        f_clock += cfg_probe_cd + 1; scan_note(cn); int p9 = st_probe == s0 + 2;       /* NoMeleeMs: no new probe */
        f_clock += cfg_nomelee_ms; scan_note(cn); int p10 = st_probe == s0 + 3;        /* after NoMeleeMs: probes again */
        /* fight order: knife in hand, enemy at 40 (in reach, under MinDistance) -> eFight queued */
        memset(&ft, 0, sizeof ft); ft.obj[0x30] = 6; fake_vec(ft.obj + cfg_enttags, ft.tags, "faction_sm", "has_melee_weapon", NULL);
        *(float *)(tp + 0x28) = 40.0f;
        uint8_t cf2[0x20] = { 0 }; *(void **)cf2 = sword.obj; *(void **)(cf2 + 8) = ft.obj; *(void **)(cf2 + 0x10) = tp;
        uint32_t fq0 = st_fight_q, cq0 = st_charge_q;
        charge_note(cf2, 1, 0); int f1 = st_fight_q == fq0 + 1 && st_charge_q == cq0 + 1;
        int o0 = f_orders; f_clock += 50; charge_flush(man.obj); int f2 = f_orders > o0;   /* order issued */
        charge_note(cf2, 1, 0); int f3 = st_fight_q == fq0 + 1;                        /* inside FightCooldownMs: nothing */
        g_vt_fight = (void **)0x8888;                                                   /* engine runs eFight on this enemy */
        *(void **)forder = (void *)0x8888; *(uint32_t *)(forder + 0x48) = 1; *(void **)(forder + 0x58) = enemy.obj;
        fblk[0] = forder; fmap[0] = fblk;
        *(uint32_t *)(fbrain + 0x39C) = 0x1000; *(int32_t *)(fbrain + 0x88) = 1;
        *(void **)(fbrain + 0x70) = fmap; *(uint64_t *)(fbrain + 0x78) = 1; *(uint64_t *)(fbrain + 0x80) = 0;
        *(uint8_t **)(ft.obj + 0xDC8) = fbrain;
        f_clock += cfg_fight_cd + 1; charge_note(cf2, 1, 0); int f4 = st_fight_q == fq0 + 1;   /* already fighting it: nothing */
        *(void **)(forder + 0x58) = man.obj;                                            /* fighting someone else (dll order) */
        mine_add(forder);
        charge_note(cf2, 1, 0); int f5 = st_fight_q == fq0 + 2;                        /* -> eFight on this one */
        g_vt_fight = NULL; *(uint8_t **)(ft.obj + 0xDC8) = NULL;
        cfg_fight_on = 0; *(float *)(tp + 0x28) = 40.0f; f_clock += 10000;
        uint32_t cq1 = st_charge_q; charge_note(cf2, 1, 0); int f6 = st_charge_q == cq1;   /* FightOrder=0: old rule, nothing in reach */
        cfg_fight_on = 1;
        /* 0.7.17 order picks: the hook called from outside the scan (g_scan_lo unset here = every call is "outside") */
        static fake_t ou, jp; static uint8_t to[0x40];
        memset(&ou, 0, sizeof ou); ou.obj[0x30] = 6; fake_vec(ou.obj + cfg_enttags, ou.tags, "faction_sm", NULL, NULL);
        memset(&jp, 0, sizeof jp); jp.obj[0x30] = 6; fake_vec(jp.obj + cfg_enttags, jp.tags, "faction_sm", "tp_jetpack", NULL);
        memset(to, 0, sizeof to); *(void **)(to + 0x18) = enemy.obj; *(float *)(to + 0x28) = 40.0f;
        uint8_t co[0x20] = { 0 }; *(void **)co = bolter.obj; *(void **)(co + 8) = ou.obj; *(void **)(co + 0x10) = to;
        f_clock += 100000; uint32_t s1 = st_probe, fq1 = st_fight_q;
        int o1 = ms_hook(co) == NULL && st_probe == s1 + 1;                              /* order pick at 40: probe, bolter refused */
        *(void **)co = sword.obj;
        int o2 = ms_hook(co) != NULL && st_fight_q == fq1 + 1;                           /* knife offered: melee + fight order queued */
        *(void **)co = bolter.obj; *(void **)(co + 8) = jp.obj;
        int o3 = ms_hook(co) != NULL && st_probe == s1 + 1;                              /* tp_jetpack: left alone, gun passes, no probe */
        *(float *)(to + 0x28) = 300.0f; *(void **)(co + 8) = ou.obj; f_clock += 10000;
        int o4 = ms_hook(co) != NULL;                                                    /* order pick at 300: engine's choice (knife seen, no hold) */
        /* 0.7.18 order poll: units with an explicit attack order run no scan; the dll checks their front order itself */
        cfg_known_ms = 300000; cfg_poll_on = 1; cfg_prox_on = 0; cfg_esc_ms = 0;
        static fake_t pa, pb, pc; static uint8_t pbrain[3][0x400], pord[3][0x100]; static void *pblk[3][2], *pmap[3][1];
        fake_t *pu[3] = { &pa, &pb, &pc };
        memset(&pa, 0, sizeof pa); pa.obj[0x30] = 6; fake_vec(pa.obj + cfg_enttags, pa.tags, "faction_sm", "has_melee_weapon", NULL);   /* knife in hand */
        memset(&pb, 0, sizeof pb); pb.obj[0x30] = 6; fake_vec(pb.obj + cfg_enttags, pb.tags, "faction_sm", NULL, NULL);                 /* knife on the belt, known */
        memset(&pc, 0, sizeof pc); pc.obj[0x30] = 6; fake_vec(pc.obj + cfg_enttags, pc.tags, "faction_ig", NULL, NULL);                 /* unknown */
        g_vt_attack = (void **)0x7777;
        *(float *)(enemy.obj + 0x74) = 40.0f;                                            /* enemy 40 units east of the origin */
        for (int i = 0; i < 3; i++) {
            *(void **)pord[i] = (void *)0x7777; *(uint32_t *)(pord[i] + 0x48) = 1; *(void **)(pord[i] + 0x58) = enemy.obj;
            pblk[i][0] = pord[i]; pmap[i][0] = pblk[i];
            *(uint32_t *)(pbrain[i] + 0x39C) = 0x1000; *(int32_t *)(pbrain[i] + 0x88) = 1;
            *(void **)(pbrain[i] + 0x70) = pmap[i]; *(uint64_t *)(pbrain[i] + 0x78) = 1; *(uint64_t *)(pbrain[i] + 0x80) = 0;
            *(uint8_t **)(pu[i]->obj + 0xDC8) = pbrain[i];
        }
        f_clock += 100000;
        melee_known(pb.obj);
        for (int i = 0; i < 3; i++) known_note(pu[i]->obj, f_clock);
        uint32_t pf0 = st_poll_fight, pd0 = st_poll_draw, pn0 = st_poll_unknown;
        order_poll(man.obj, f_clock);
        /* 0.7.19: pc (unknown) gets the blind draw: draws = known (pb) + blind (pc) */
        int q1 = st_poll_fight == pf0 + 1, q2 = st_poll_draw == pd0 + 2, q3 = st_poll_unknown == pn0 + 1 && unit_of(pc.obj, 0)->blind_ms;
        order_poll(man.obj, f_clock + 10);                                               /* inside PollMs: nothing */
        int q4 = st_poll_fight == pf0 + 1 && st_poll_draw == pd0 + 2;
        order_poll(man.obj, f_clock + cfg_blind_ms + 1);                                 /* blind draw judged: nothing in hand -> no melee */
        unit_t *bs = unit_of(pc.obj, 0);
        int q6 = bs && !bs->blind_ms && bs->nomelee_until && !has_melee(pc.obj);
        *(float *)(enemy.obj + 0x74) = 300.0f;                                           /* enemy out of fight range */
        order_poll(man.obj, f_clock + cfg_blind_ms + 1000);
        int q5 = st_poll_draw == pd0 + 2;
        /* 0.7.22: the dll's own tick (no hook call needed) + the wanted fight: pb's knife comes into his hand -> fight order */
        *(float *)(enemy.obj + 0x74) = 40.0f;
        fake_vec(pb.obj + cfg_enttags, pb.tags, "faction_sm", "has_melee_weapon", NULL);
        f_clock += cfg_blind_ms + 4000; g_sim_tid = GetCurrentThreadId(); g_tick_wall = 0;   /* 4 s after the last poll's want refresh */
        LONG tk0 = st_ticks; uint32_t wf0 = st_want_fight, cq9 = st_charge_q;
        ms_tick();
        int t1 = st_ticks == tk0 + 1 && st_want_fight == wf0 + 1 && st_charge_q > cq9;
        g_tick_wall = 0; ms_tick();                                                      /* clock did not advance: no second tick */
        int t2 = st_ticks == tk0 + 1;
        g_sim_tid = GetCurrentThreadId() + 1; f_clock += 500; g_tick_wall = 0; ms_tick(); /* not the engine's thread: nothing */
        int t3 = st_ticks == tk0 + 1 && st_tick_wrongthread == 1;
        g_sim_tid = 0;
        printf("own tick: tick+wanted fight %d paused %d wrong thread %d\n", t1, t2, t3);
        if (!(t1 && t2 && t3)) q1 = 0;
        /* 0.7.24 fight-range stance: an idle unit with a knife on the belt and a known hostile at 40 -> draw + wanted fight; a
         * friend at 40 does nothing; knife still not in hand after EscalateMs -> fight order anyway */
        {
            static fake_t px, py; cfg_prox_on = 1; cfg_esc_ms = 1500;
            memset(&px, 0, sizeof px); px.obj[0x30] = 6; fake_vec(px.obj + cfg_enttags, px.tags, "faction_sm", NULL, NULL);
            memset(&py, 0, sizeof py); py.obj[0x30] = 6; py.obj[0xDA8] = 7; fake_vec(py.obj + cfg_enttags, py.tags, "faction_ig", NULL, NULL);
            *(float *)(py.obj + 0x78) = 2000.0f;                                         /* owner 7, far from everyone, hostile to nobody */
            *(float *)(px.obj + 0x78) = 30.0f;                                           /* px: 30 north of the origin, enemy 40 east of it = 50 away */
            f_clock += 20000; known_note(px.obj, f_clock); known_note(py.obj, f_clock); known_note(enemy.obj, f_clock); melee_known(px.obj);
            int w0 = 0, w1 = 0, wy = 0;
            for (int i = 0; i < NWANT; i++) if (g_want[i].used && g_want[i].unit == px.obj) w0 = 1;
            g_poll_last = 0; order_poll(NULL, f_clock);
            for (int i = 0; i < NWANT; i++) { if (g_want[i].used && g_want[i].unit == px.obj && g_want[i].tgt == enemy.obj) w1 = 1; if (g_want[i].used && g_want[i].unit == py.obj) wy = 1; }
            uint32_t e0 = st_want_esc; g_poll_last = 0; order_poll(NULL, f_clock + 1600);
            int x3 = 0;                                                                  /* (the enemy, idle with px in his range, escalates too) */
            for (int i = 0; i < NWANT; i++) if (g_want[i].used && g_want[i].unit == px.obj && g_want[i].esc) x3 = st_want_esc > e0;
            printf("fight-range stance: draw+want %d friend-ignored %d escalated %d\n", !w0 && w1, !wy, x3);
            if (w0 || !w1 || wy || !x3) q1 = 0;
            /* 0.7.26: knife in hand from the dll, nobody in fight range for HoldMs -> the gun is asked for; melee stance: never */
            static fake_t pg; memset(&pg, 0, sizeof pg); pg.obj[0x30] = 6; fake_vec(pg.obj + cfg_enttags, pg.tags, "faction_sm", "has_melee_weapon", NULL);
            *(float *)(pg.obj + 0x78) = -3000.0f;                                        /* far from everyone */
            known_note(pg.obj, f_clock); unit_t *gs = unit_of(pg.obj, 1); snprintf(gs->gun, sizeof gs->gun, "sm_bolter"); gs->fr_ms = f_clock;
            uint32_t g0 = st_gun_try; g_poll_last = 0; order_poll(NULL, f_clock + 100);
            int y1 = st_gun_try == g0;                                                   /* an enemy was in range 100 ms ago: not yet */
            g_poll_last = 0; order_poll(NULL, f_clock + cfg_ad_hold + 200);
            int y2 = st_gun_try == g0 + 1;
            f_clock += 90000; unit_t *gs2 = unit_of(pg.obj, 0);
            int y3 = gs2 == gs && !strcmp(gs2->gun, "sm_bolter");                        /* state survives a quiet minute and a half */
            /* 0.7.27: knife in hand from the dll, enemy at 130 (beyond Distance 100, inside x1.5): stays in melee, no gun yet */
            *(float *)(pg.obj + 0x74) = *(float *)(enemy.obj + 0x74); *(float *)(pg.obj + 0x78) = 130.0f;
            uint32_t g1 = st_gun_try; int y4 = 0; g_poll_last = 0; order_poll(NULL, f_clock);
            for (int i = 0; i < NPEND; i++) if (g_pend[i].used && g_pend[i].unit == pg.obj) y4 = st_gun_try == g1;
            *(float *)(pg.obj + 0x78) = 170.0f; f_clock += 10000; g_poll_last = 0; order_poll(NULL, f_clock);   /* at 170: out, the gun is asked for */
            int y5 = st_gun_try == g1 + 1;
            printf("back to the gun: held %d asked %d state kept %d | range edge: stays at 130 %d leaves at 170 %d\n", y1, y2, y3, y4, y5);
            if (!(y1 && y2 && y3 && y4 && y5)) q1 = 0;
            cfg_prox_on = 0; cfg_esc_ms = 0;
        }
        *(float *)(enemy.obj + 0x74) = 0.0f; g_vt_attack = NULL;
        for (int i = 0; i < 3; i++) *(uint8_t **)(pu[i]->obj + 0xDC8) = NULL;
        printf("order poll: fight %d draw %d blind %d interval %d far %d blind-judged %d\n", q1, q2, q3, q4, q5, q6);
        int ok = p0 && p1 && p2 && p3 && p4 && p5 && p6 && p7 && p8 && p9 && p10 && f1 && f2 && f3 && f4 && f5 && f6 && o1 && o2 && o3 && o4
              && q1 && q2 && q3 && q4 && q5 && q6;
        printf("order picks: probe %d melee+fight %d jetpack-skip %d far %d\n", o1, o2, o3, o4);
        printf("fight range: no-probe %d probe %d gun-refused %d melee-found %d no-reprobe %d probe2 %d refused2 %d none %d gun-kept %d no-melee-wait %d reprobe %d | fight queued %d ordered %d cooldown %d running %d other %d off %d -> %s\n",
               p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, f1, f2, f3, f4, f5, f6, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    {   /* 0.7.33 stale refs: listed -> alive and unlinked as before; not listed / broken list -> gone, zeroed before RefDtor */
        static fake_t re; memset(&re, 0, sizeof re);
        static uint8_t rlist[0x40]; __attribute__((aligned(16))) static uint8_t rr[REFSZ];
        memset(rlist, 0, sizeof rlist); memset(rr, 0, sizeof rr);
        g_reflist_off = 0x198;
        *(uint32_t *)(rr + 8) = 1; *(void **)(rr + 0x18) = re.obj;
        *(void **)(rlist + 0x18) = rr;                                       /* second entry names the ref */
        *(void **)(re.obj + 0x198 + 8) = rlist; *(void **)(re.obj + 0x198 + 0x10) = rlist + 0x20;
        uint32_t s0 = st_ref_stale;
        int r1 = ref_ent(rr) == re.obj;
        ref_free(rr); int r2 = st_ref_stale == s0 && *(void **)(rr + 0x18) == re.obj;        /* listed: left to RefDtor */
        int r1b = ref_ent(rr) == re.obj;
        *(void **)(rlist + 0x18) = NULL;
        int r3a = ref_ent(rr) == re.obj;                                     /* same clock value: the kept answer */
        f_clock++;
        int r3 = r1b && r3a && ref_ent(rr) == NULL;                          /* next clock value: entry gone */
        *(void **)(re.obj + 0x198 + 8) = (void *)GetModuleHandleA(NULL); *(void **)(re.obj + 0x198 + 0x10) = NULL;   /* the crash: begin in the image, end 0 */
        f_clock++;
        int r4 = ref_ent(rr) == NULL;
        ref_free(rr); int r5 = st_ref_stale == s0 + 1 && *(void **)(rr + 0x18) == NULL && *(uint32_t *)(rr + 8) == 0;
        *(void **)(rr + 0x18) = (void *)(uintptr_t)0x10; *(uint32_t *)(rr + 8) = 1;          /* entity memory unmapped */
        f_clock++;
        int r6 = ref_ent(rr) == NULL;
        g_reflist_off = 0; int r7 = ref_ent(rr) == (void *)(uintptr_t)0x10;                  /* offset unknown: no check, as 0.7.32 */
        printf("stale refs: listed %d kept %d unlisted %d broken-list %d zeroed %d unmapped %d check-off %d -> %s\n",
               r1, r2, r3, r4, r5, r6, r7, r1 && r2 && r3 && r4 && r5 && r6 && r7 ? "OK" : "FAIL");
        if (!(r1 && r2 && r3 && r4 && r5 && r6 && r7)) fails++;
    }
    {   /* 0.7.35 self-test: fast pass, known-units cache, client gate, queues left to the tick */
        static fake_t fu, fv; static uint8_t tf[0x40];
        memset(&fu, 0, sizeof fu); fu.obj[0x30] = 6; fake_vec(fu.obj + cfg_enttags, fu.tags, "faction_ig", NULL, NULL);
        memset(&fv, 0, sizeof fv); fv.obj[0x30] = 6; fake_vec(fv.obj + cfg_enttags, fv.tags, "faction_ig", NULL, NULL);
        memset(tf, 0, sizeof tf); *(void **)(tf + 0x18) = enemy.obj; *(float *)(tf + 0x28) = 40.0f;
        uint8_t cx[0x20] = { 0 }; *(void **)cx = bolter.obj; *(void **)(cx + 8) = fu.obj; *(void **)(cx + 0x10) = tf;
        g_clock = &f_clock; f_clock += 100000;
        unit_t *us = unit_of(fu.obj, 1); us->nomelee_until = f_clock + 20000;
        LONG fa = st_fast; uint32_t pr = st_probe;
        int q1 = ms_hook(cx) == (void *)0x1234 && st_fast == fa + 1 && st_probe == pr;      /* known "no melee": engine's choice, no probe */
        *(void **)cx = sword.obj;
        int q2 = ms_hook(cx) != NULL && st_fast == fa + 1 && unit_of(fu.obj, 1)->nomelee_until == 0;   /* melee ammo: full path, learnt */
        *(void **)cx = bolter.obj; ms_hook(cx);
        int q3 = st_fast == fa + 1;                                                          /* carries melee now: never fast */
        cfg_kcache = 0; us = unit_of(fv.obj, 1); us->nomelee_until = f_clock + 20000; *(void **)(cx + 8) = fv.obj;
        ms_hook(cx); int q4 = st_fast == fa + 1;                                             /* FastPass=0: full path */
        cfg_kcache = 1;
        /* known-units cache */
        f_clock += 1000; known_note(fu.obj, f_clock);
        int ks = -1; for (int i = 0; i < NKNOWN; i++) if (g_known[i].used && g_known[i].unit == fu.obj) ks = i;
        int k1 = ks >= 0;
        if (ks >= 0) { ref_free(g_known[ks].ref); g_known[ks].used = 0; }
        known_note(fu.obj, f_clock + 10);
        int k2 = 1; for (int i = 0; i < NKNOWN; i++) if (g_known[i].used && g_known[i].unit == fu.obj) k2 = 0;   /* inside KCACHE_MS: not looked up */
        known_note(fu.obj, f_clock + KCACHE_MS);
        int k3 = 0; for (int i = 0; i < NKNOWN; i++) if (g_known[i].used && g_known[i].unit == fu.obj) k3 = 1;   /* after it: noted again */
        /* client gate: no weapon pick seen in this process -> the tick does nothing */
        LONG all0 = st_hk_all, tk0 = st_ticks, gt0 = st_tick_gated;
        cfg_tick_on = 1; g_sim_tid = GetCurrentThreadId(); f_clock += 1000;
        st_hk_all = 0; g_tick_wall = 0; g_tick_clock = 0;
        ms_tick(); int g1 = st_ticks == tk0 && st_tick_gated == gt0 + 1;
        st_hk_all = 1; g_tick_wall = 0;
        ms_tick(); int g2 = st_ticks == tk0 + 1;
        st_hk_all = all0;
        /* with the quant hook in, a scan pick leaves the charge queue to the tick; without it the hook flushes as before */
        memset(g_pend, 0, sizeof g_pend);
        pend_push(fu.obj, enemy.obj, 40.0f, 0, 0, f_clock);
        int pi = -1; for (int i = 0; i < NPEND; i++) if (g_pend[i].used && g_pend[i].unit == fu.obj) pi = i;
        g_scan_lo = (uint8_t *)1; g_scan_hi = (uint8_t *)(~(uintptr_t)0);                    /* every call counts as "from the scan" */
        *(void **)(cx + 8) = fv.obj; f_clock += 50;
        g_frame_state = 1; ms_hook(cx);
        int t1 = pi >= 0 && g_pend[pi].used;                                                 /* ticking: still queued */
        g_frame_state = 2; f_clock += 50; ms_hook(cx);
        int t2 = pi >= 0 && !g_pend[pi].used;                                                /* no quant hook: flushed from the scan */
        g_frame_state = 0; g_scan_lo = g_scan_hi = NULL;
        int ok35 = q1 && q2 && q3 && q4 && k1 && k2 && k3 && g1 && g2 && t1 && t2;
        printf("0.7.35: fast %d learn %d not-fast %d off %d | cache add %d hit %d expire %d | gate closed %d open %d | queue kept %d flushed %d -> %s\n",
               q1, q2, q3, q4, k1, k2, k3, g1, g2, t1, t2, ok35 ? "OK" : "FAIL");
        if (!ok35) fails++;
    }
    printf("filter: %zu cases + 3 hook cases + charge, %d failed\n", sizeof c / sizeof c[0], fails);
    if (fails) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
