/*
 * GEM Guidance 1.1  -  Call to Arms: Gates of Hell (GEM engine)  -  module of gem_weaponscript.dll (Indomitus Launcher payload)
 * Fire-and-forget lock-on for guided missiles + range-dependent drift. Split out of the all-in-one GEM Converge 0.5.1.
 *
 * Per-weapon opt-ins, stuff tags:
 *   "lock_on" on the WEAPON, vanilla "atgm" on its AMMO: the entity under the sight when the shot leaves (or the
 *            ground point if nothing is there) is locked. The engine already steers any "atgm" round toward the
 *            gunner's LIVE aim point (Bullet::Quant, 10 deg/s, the CtA TOW code); every tick this dll hands that
 *            code the locked point instead, so the gunner (or the AI, which retargets right after firing) may look
 *            away. If the engine guidance cannot run (owner dead, weapon dropped) the dll turns the velocity itself
 *            at ini LockTurn deg/s.
 *   "drift_<N>" on the weapon: rounds veer off with a constant sideways pull, N = degrees off at 100 units (the miss
 *            grows with distance). Unguided rounds: the launch origin slides sideways every tick (the engine's
 *            closed-form gravity stays intact). Locked rounds: the lock point walks sideways instead.
 *
 * Hooks (chainable, see common\gem_common.h): SpawnBullet (shared with GEM Converge / GEM Overcharge) computes the
 * lock for the bullet about to be created, Bullet::Launch binds it to that bullet, Bullet::Quant (per bullet tick)
 * swaps the locked point into the aimer's target for the duration of the engine's guidance and does the drift.
 */
#define GEM_NAME    "GEM Guidance"
#define GEM_VERSION "1.1.2"
#define GEM_SECT    L"guidance"
#include "../common/gem_common.h"

typedef void    (*BulletLaunchFn)(void *bullet, float *pos, float *vel, float gravity);
typedef char    (*BulletQuantFn)(void *bullet);
typedef void   *(*GetHeldItemFn)(void *humanPart);

static BulletLaunchFn   g_bulletLaunch;     /* Bullet::Launch(bullet, pos, vel, gravityScale) */
static BulletQuantFn    g_bulletQuant;      /* Bullet::Quant(bullet) -> keep alive */
static GetHeldItemFn    g_getHeldItem;      /* human part -> held weapon item (the aimer's owner) */
static uint8_t *g_quantMs;                  /* ms per quant (int) */

/* Bullet layout (eBullet.cpp, 1.067 game/editor/server identical): */
#define BUL_AMMO     0xA0    /* ammo stuff (its tag vector carries "atgm") */
#define BUL_TICKS    0xA8    /* int16 ticks since launch (closed-form flight time) */
#define BUL_FLAGS    0xAA    /* uint16: 0x400 = finished */
#define BUL_OWNER    0xB0    /* owner entity */
#define BUL_ORIGIN   0xC8    /* float3 launch origin */
#define BUL_VEL      0xD4    /* float3 velocity, units per tick */
#define BUL_POS      0xE0    /* float3 current position */
#define BUL_SPEED    0xEC    /* |velocity| */
#define BUL_WEAPON   0x128   /* weapon stuff */

/* ---- finders ------------------------------------------------------------------------- */
/* Bullet::Launch owns the "Invalid bullet velocity - %g" assert */
static int find_bullet_launch(void)
{
    uint8_t *s = find_cstr("Invalid bullet velocity - %g");
    if (!s) { logf_("Bullet::Launch: assert string not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, 0x15, 0);
    if (!lea) { logf_("Bullet::Launch: string xref not found"); return 0; }
    uint8_t *f = func_start_of(lea, 0x400);          /* unwind table: a sibling's entry patch does not matter */
    if (!f) { logf_("Bullet::Launch: function start not found"); return 0; }
    g_bulletLaunch = (BulletLaunchFn)f;
    logf_("Bullet::Launch = exe+0x%llX", (unsigned long long)(f - g_base));
    return 1;
}

/* Bullet::Quant = the function that tests the ammo for the global "atgm" Tag and also calls Bullet::Launch
   (the other user of that Tag is the hit handler). Inside it: mov rcx,[owner+disp32] ; call GetHeldItem. */
static int find_quant(void)
{
    g_bulletQuant = NULL; g_getHeldItem = NULL; g_quantMs = NULL;
    uint8_t *str = find_cstr("atgm");
    if (!str) { logf_("Quant: 'atgm' not found"); return 0; }
    uint8_t *init = find_lea_to(g_text.p, g_text.n, str, 0x15, 0);
    if (!init) { logf_("Quant: 'atgm' static init not found"); return 0; }
    uint8_t *tag = NULL;
    for (uint8_t *q = init + 7; q < init + 0x20; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D) { tag = q + 7 + *(int32_t *)(q + 3); break; }
    if (!tag) { logf_("Quant: global atgm tag not found"); return 0; }
    for (int k = 0; k < 8 && !g_bulletQuant; k++) {
        uint8_t *use = find_lea_to(g_text.p, g_text.n, tag, 0x15, k);
        if (!use) break;
        uint8_t *f = func_start_of(use, 0x1000);
        if (!f) continue;
        for (int c = 0; c < 64; c++) {
            uint8_t *t = call_target(f, 0x400, c);
            if (!t) break;
            if (t == (uint8_t *)g_bulletLaunch) { g_bulletQuant = (BulletQuantFn)f; break; }
        }
    }
    if (!g_bulletQuant) { logf_("Quant: function not found"); return 0; }
    uint8_t *f = (uint8_t *)g_bulletQuant;
    /* ms per quant: first  mov r14d,[rip+g]  (44 8B 35) */
    for (uint8_t *p = f; p < f + 0x400; p++)
        if (p[0] == 0x44 && p[1] == 0x8B && p[2] == 0x35) { g_quantMs = p + 7 + *(int32_t *)(p + 3); break; }
    /* owner -> human part: GetAimPoint reads the same field (mov rax,[r8+disp32], 49 8B 80) */
    uint8_t *ap = (uint8_t *)g_getAimPoint;
    for (uint8_t *p = ap; p < ap + 0x60; p++)
        if (p[0] == 0x49 && p[1] == 0x8B && p[2] == 0x80) { g_ownerItemOff = *(uint32_t *)(p + 3); break; }
    if (!g_ownerItemOff) { logf_("Quant: human part offset not found in GetAimPoint"); return 0; }
    /* in Quant:  mov rcx,[rcx+part] ; call GetHeldItem   (48 8B 89 disp32 E8) */
    for (uint8_t *p = f; p < f + 0x1000 && !g_getHeldItem; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x89 && *(uint32_t *)(p + 3) == g_ownerItemOff && p[7] == 0xE8)
            g_getHeldItem = (GetHeldItemFn)(p + 12 + *(int32_t *)(p + 8));
    if (!g_getHeldItem || !in_text((void *)g_getHeldItem)) { logf_("Quant: GetHeldItem not found"); return 0; }
    logf_("Bullet::Quant = exe+0x%llX, GetHeldItem = exe+0x%llX, owner->human part +0x%X, quant ms %s",
          (unsigned long long)(f - g_base), (unsigned long long)((uint8_t *)g_getHeldItem - g_base), g_ownerItemOff, g_quantMs ? "found" : "NOT found");
    return 1;
}

/* ---- config --------------------------------------------------------------------------- */
static char g_lockTag[64] = "lock_on";
static size_t g_lockTagLen;
static double g_lockTurn = 10.0;      /* deg/s, fallback steering when the engine's own guidance can't run */
static int g_lockLog = 10;
static int g_allowNet = 0;
static int g_driftMax = 45;

static int has_lock_tag(const uint8_t *stuff)
{
    const uint8_t *b = *(const uint8_t *const *)(stuff + g_stuffTagsBeg), *e = *(const uint8_t *const *)(stuff + g_stuffTagsEnd);
    if (!b || e < b || (size_t)(e - b) > TAG_SIZE * 256) return 0;
    for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
        size_t len; const char *s = tag_str(t, &len);
        if (len == g_lockTagLen && !_strnicmp(s, g_lockTag, len)) return 1;     /* engine lowercases tags; ini value may not be */
    }
    return 0;
}

/* ---- lock-on ------------------------------------------------------------------------- */
/* A lock = the entity (or ground point) under the sight when the shot left. Every tick we hand the
   engine's own "atgm" guidance that point instead of the live aim point, so the gunner may look away. */
typedef struct {
    void *bullet;        /* key */
    float origin[3];     /* bullet+ORIGIN at lock time: a reused bullet slot has a different one */
    void *ent;           /* locked entity, NULL = fixed point */
    const void *entVt;   /* its vtable at lock time (cheap liveness check) */
    float off[3];        /* hit point relative to the entity position */
    float point[3];      /* last target point */
    int fallback;        /* steered by us, not by the engine */
    int ticks;           /* quant calls seen (log) */
    int driftN;          /* weapon drift_<N> (0 = none), resolved on the first tick */
    float dperp[3];      /* drift direction (unit, perpendicular to the launch velocity) */
    double dtan;         /* drift slope: target offset = dtan * distance flown */
} Lock;
#define MAX_LOCKS 64
static Lock g_locks[MAX_LOCKS];
static int g_pendingLock;            /* set by SpawnBullet for the Bullet::Launch calls inside it */
static Lock g_pending;

static Lock *lock_find(void *bullet)
{
    for (int i = 0; i < MAX_LOCKS; i++) if (g_locks[i].bullet == bullet) return &g_locks[i];
    return NULL;
}
/* 1.1.2: how many lock slots are in use, so Bullet::Quant can leave at once when there is none */
static int g_lockCount;
static void lock_drop(Lock *l) { if (l->bullet) { l->bullet = NULL; g_lockCount--; } }

static void lock_target(const uint8_t *weapon, const double *aim)
{
    memset(&g_pending, 0, sizeof g_pending);
    g_pending.point[0] = (float)aim[0]; g_pending.point[1] = (float)aim[1]; g_pending.point[2] = (float)aim[2];
    const uint8_t *owner = *(const uint8_t *const *)(weapon + g_ownerOff);
    uint8_t *beg = NULL, *end = NULL;
    rd_mem(g_hitVec, &beg, 8); rd_mem(g_hitVec + 8, &end, 8);
    char name[96] = "";
    /* hit record (0x48 bytes, engine readers: cmp [rec],1 / cmp [rec],4 / movss [rec+4] / entity flags via
       [[rec+0x18]+0x30]): +0 int type (1 = terrain: material looked up at the point; 4 = entity volume),
       +4 float distance, +8 float3 point, +0x18 entity* */
    if (beg && end > beg) {
        float hp[3]; void *cand = NULL; int type = 0;
        if (rd_mem(beg, &type, 4) && type != 1 && rd_mem(beg + 8, hp, 12) && rd_mem(beg + 0x18, &cand, 8) && cand && cand != owner &&
            fabs(hp[0] - aim[0]) < 1.0 && fabs(hp[1] - aim[1]) < 1.0 && fabs(hp[2] - aim[2]) < 1.0 &&
            rtti_name(cand, name, sizeof name)) {
            float ep[3];
            if (rd_mem((uint8_t *)cand + ENT_POS, ep, 12)) {
                g_pending.ent = cand;
                rd_mem(cand, &g_pending.entVt, 8);
                for (int i = 0; i < 3; i++) g_pending.off[i] = g_pending.point[i] - ep[i];
            }
        }
    }
    g_pendingLock = 1;
    if (g_lockLog > 0) {
        g_lockLog--;
        int type = -1; if (beg && end > beg) rd_mem(beg, &type, 4);
        logf_("lock: %s at %.1f %.1f %.1f (%d hit records, first type %d)", g_pending.ent ? name : "ground point", aim[0], aim[1], aim[2],
              (beg && end > beg) ? (int)((end - beg) / 0x48) : 0, type);
    }
}

/* aim point with the direct-control flag forced on, so the raycast runs for AI shots as well */
static void aim_point_raycast(const uint8_t *weapon, void *aimer, double *aim)
{
    const uint8_t *owner = *(const uint8_t *const *)(weapon + g_ownerOff);
    uint8_t *part = NULL; uint32_t flags = 0, *fp = NULL;
    if (owner && rd_mem(owner + g_ownerItemOff, &part, 8) && part && rd_mem(part + g_itemFlagOff, &flags, 4)) fp = (uint32_t *)(part + g_itemFlagOff);
    if (fp && !(flags & 2)) *fp = flags | 2;
    g_getAimPoint(aimer, aim);
    if (fp && !(flags & 2)) *fp = flags;
}

/* SpawnBullet: for a lock_on weapon compute the lock, keep it pending while the engine creates the bullet(s)
   (GEM Converge multi-shot may spawn several inside an outer hook: each gets the lock) */
static SpawnBulletFn g_spawnBulletOrig;
static int g_on;                     /* all three hooks installed; until then every hook is a pass-through */
static char hook_spawn_bullet(void *shooter, int syncIdx, float *zero, float *dir)
{
    const uint8_t *weapon = *(const uint8_t *const *)((const uint8_t *)shooter + g_weaponOff);
    const uint8_t *stuff = weapon ? *(const uint8_t *const *)(weapon + g_stuffOff) : NULL;
    void *aimer = weapon ? *(void **)(weapon + g_aimerOff) : NULL;
    if (!g_on || !stuff || !aimer || !has_lock_tag(stuff) || (!g_allowNet && g_isNetGame && g_isNetGame()))
        return g_spawnBulletOrig(shooter, syncIdx, zero, dir);
    double aim[3];
    aim_point_raycast(weapon, aimer, aim);
    lock_target(weapon, aim);
    char r = g_spawnBulletOrig(shooter, syncIdx, zero, dir);
    g_pendingLock = 0;
    return r;
}

/* Bullet::Launch: the bullet SpawnBullet just created comes through here -> bind the pending lock to it.
   1.1.2: every bullet, however it was created, also comes through here: the drift question is asked now (dreg_launch) */
static void dreg_launch(void *bullet);
static BulletLaunchFn g_bulletLaunchOrig;
static void hook_bullet_launch(void *bullet, float *pos, float *vel, float gravity)
{
    g_bulletLaunchOrig(bullet, pos, vel, gravity);
    if (!g_on) return;
    dreg_launch(bullet);
    if (!g_pendingLock) return;
    Lock *l = lock_find(bullet);
    int fresh = 0;
    if (!l) { for (int i = 0; i < MAX_LOCKS; i++) if (!g_locks[i].bullet) { l = &g_locks[i]; fresh = 1; break; } }
    if (!l) return;
    *l = g_pending;
    l->bullet = bullet;
    g_lockCount += fresh;
    memcpy(l->origin, (uint8_t *)bullet + BUL_ORIGIN, 12);
}

static int lock_target_pos(Lock *l, float *out)
{
    if (l->ent) {
        const void *vt = NULL; float ep[3];
        if (rd_mem(l->ent, &vt, 8) && vt == l->entVt && rd_mem((uint8_t *)l->ent + ENT_POS, ep, 12) &&
            fabs(ep[0]) < 1e7 && fabs(ep[1]) < 1e7 && fabs(ep[2]) < 1e7) {
            for (int i = 0; i < 3; i++) l->point[i] = ep[i] + l->off[i];
        } else l->ent = NULL;          /* gone: keep flying to its last position */
    }
    memcpy(out, l->point, 12);
    return 1;
}

/* ---- drift ---------------------------------------------------------------------------- */
/* weapon tag drift_<N>: the round veers off its line with a constant sideways pull, so the miss grows with
   distance (N = degrees off at 100 units, twice that at 200...). Each round draws its own strength (0..N)
   and direction from the launch geometry, so lockstep machines agree. Implemented by sliding the launch
   origin sideways every tick: the engine's closed-form flight (origin + v*t - gravity) keeps its gravity. */
/* 1.1.2: a cached answer is trusted for DRIFT_CACHE_MS only. The cache is keyed by the weapon's eStuff address, and
   every eStuff is recreated on a map load: an old address can come back as a different weapon (gem_overcharge.c keeps
   names beside its pointers for the same reason). Before, an entry lived until 32 newer weapon types pushed it out, so
   after a map change a drift_<N> launcher could fly straight, or a plain gun drift, on this machine only (heap
   addresses differ per machine: in a network game that is a desync). This module has no "new mission" signal and only
   runs while bullets fly, so entries simply expire: a map load takes far longer than the window, and reading one
   weapon's tags again twice a second costs about a microsecond. The answer itself only depends on the weapon's tags,
   so when it is read does not change the simulation. */
#define DRIFT_CACHE_MS 500
#ifdef GEM_TEST
static uint32_t g_driftTestTick;        /* the test's clock */
#define drift_now() g_driftTestTick
#else
#define drift_now() ((uint32_t)GetTickCount())
#endif
typedef struct { const void *stuff; int n; uint32_t at; } DriftTagCache;
static DriftTagCache g_driftCache[32];

static int stuff_drift(const uint8_t *stuff)
{
    uint32_t now = drift_now();
    int at = -1;
    for (int i = 0; i < 32; i++) if (g_driftCache[i].stuff == stuff) {
        if ((uint32_t)(now - g_driftCache[i].at) < DRIFT_CACHE_MS) return g_driftCache[i].n;
        at = i; break;                                  /* expired: read the tags again, into the same slot */
    }
    int n = 0;
    const uint8_t *b = NULL, *e = NULL;
    if (rd_mem(stuff + g_stuffTagsBeg, &b, 8) && rd_mem(stuff + g_stuffTagsEnd, &e, 8) && b && e > b && (size_t)(e - b) <= TAG_SIZE * 256)
        for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
            size_t len; const char *s = tag_str(t, &len);
            if (len > 6 && len < 64 && !_strnicmp(s, "drift_", 6)) { int v = atoi(s + 6); if (v >= 1 && v <= g_driftMax) n = v; }
        }
    static int slot;
    if (at < 0) { at = slot; slot = (slot + 1) % 32; }
    g_driftCache[at] = (DriftTagCache){ stuff, n, now };
    return n;
}

/* per-round drift draw from the launch geometry: unit direction perpendicular to v, strength tan(0..N deg) */
static int drift_params(const float *origin, const float *v, int n, float *perp, double *tanv)
{
    double vd[3] = { v[0], v[1], v[2] }, speed = vnorm(vd);
    if (speed < 1e-9) return 0;
    uint64_t h = mix64(bits_d(origin[0]) ^ mix64(bits_d(origin[1]) + bits_d(origin[2])) ^ mix64(bits_d(v[0]) ^ bits_d(v[2])));
    double u1 = (double)(h >> 11) * (1.0 / 9007199254740992.0), u2 = (double)(mix64(h) >> 11) * (1.0 / 9007199254740992.0);
    double up[3] = { 0, 0, 1 }; if (fabs(vd[2]) > 0.9) { up[0] = 1; up[2] = 0; }
    double e1[3] = { vd[1]*up[2] - vd[2]*up[1], vd[2]*up[0] - vd[0]*up[2], vd[0]*up[1] - vd[1]*up[0] }; vnorm(e1);
    double e2[3] = { vd[1]*e1[2] - vd[2]*e1[1], vd[2]*e1[0] - vd[0]*e1[2], vd[0]*e1[1] - vd[1]*e1[0] };
    double phi = 6.283185307 * u2;
    for (int i = 0; i < 3; i++) perp[i] = (float)(cos(phi) * e1[i] + sin(phi) * e2[i]);
    *tanv = tan(n * u1 * 0.01745329);
    return 1;
}

typedef struct { void *bullet; float origin[3]; float perp[3]; double kk; double dPrev; int16_t lastT; } Drift;
#define MAX_DRIFT 128
static Drift g_drift[MAX_DRIFT];

static void drift_tick(uint8_t *b, int n)
{
    float *origin = (float *)(b + BUL_ORIGIN), *v = (float *)(b + BUL_VEL);
    int16_t t = *(int16_t *)(b + BUL_TICKS);
    Drift *d = NULL, *freeSlot = NULL;
    for (int i = 0; i < MAX_DRIFT; i++) { if (g_drift[i].bullet == b) { d = &g_drift[i]; break; } if (!freeSlot && !g_drift[i].bullet) freeSlot = &g_drift[i]; }
    if (d && (memcmp(d->origin, origin, 12) || t < d->lastT)) d->bullet = NULL, d = NULL;   /* slot reused / relaunched */
    if (!d) {
        if (!freeSlot) return;
        d = freeSlot; memset(d, 0, sizeof *d); d->bullet = b;
        double vd[3] = { v[0], v[1], v[2] }, speed = vnorm(vd), tanv;
        if (!drift_params(origin, v, n, d->perp, &tanv)) { d->bullet = NULL; return; }
        d->kk = tanv / 100.0 * speed * speed;    /* D(t) = kk * t^2 (t in ticks, speed units/tick) */
        d->dPrev = d->kk * (double)t * t;
        d->lastT = t;
    }
    int ms = 0; if (g_quantMs) rd_mem(g_quantMs, &ms, 4);
    if (ms <= 0 || ms > 1000) ms = 50;
    double tn = (double)t + ms, dn = d->kk * tn * tn, step = dn - d->dPrev;
    for (int i = 0; i < 3; i++) origin[i] += (float)(d->perp[i] * step);
    memcpy(d->origin, origin, 12);
    d->dPrev = dn; d->lastT = (int16_t)(t + ms);
}

/* ---- 1.1.2: which bullets in flight belong to a drift_<N> weapon ---------------------- */
/* Before, Bullet::Quant asked this of every bullet on every tick: a system call to read the bullet's weapon, then the
   cache above (Alex's Nov 11 2026 audit: about 0.4 us per bullet per tick, for the two launchers that carry
   drift_<N>). Now it is asked once, when the bullet is launched, and the answer is kept for the few rounds that do
   drift; with no such round and no locked round in flight Bullet::Quant goes straight to the engine.
   It is asked in the Bullet::Launch hook, not in SpawnBullet, because the exe builds bullets in three places:
   SpawnBullet, the second branch of eWeaponBulletShootingCommand (the network path of SyncedProjectiles weapons, which
   the HKM launchers are; it copies a bullet instead of calling SpawnBullet) and the HEAT jet of
   Damage::eEffectDescCumulative. All three write the weapon into bullet+0x128 and then call Bullet::Launch (1.067
   game exe: written at exe+0x81AB83 / 0xABEE8 / 0xA741E5, called at exe+0x81ADD2 / 0xABF51 / 0xA742B0). A re-launch of
   the same bullet (guided rounds, every tick) asks again and gets the same answer.
   A registration ends with the bullet's last tick. One whose bullet went without a last tick (its scene was freed
   mid-flight) is told by its age in Bullet::Quant calls, not in time, so a paused game cannot age a live round: every
   live bullet is ticked once per quant, so one not seen for DREG_STALE calls is gone. Until it is swept (at the next
   launch) it costs only the fast exit, and a new bullet at its address is asked afresh at its own launch. */
typedef struct { void *bullet; int n; uint32_t seq; } DriftReg;
#define DREG_STALE 0x10000
static DriftReg g_dreg[MAX_DRIFT];
static int g_dregCount;
static uint32_t g_quantSeq;                 /* Bullet::Quant calls seen while a registration existed */

static void drift_forget(const void *bullet) { for (int i = 0; i < MAX_DRIFT; i++) if (g_drift[i].bullet == bullet) g_drift[i].bullet = NULL; }
static DriftReg *dreg_find(const void *bullet) { for (int i = 0; i < MAX_DRIFT; i++) if (g_dreg[i].bullet == bullet) return &g_dreg[i]; return NULL; }
static void dreg_release(DriftReg *r) { drift_forget(r->bullet); r->bullet = NULL; g_dregCount--; }

static void dreg_launch(void *bullet)
{
    const uint8_t *wstuff = *(const uint8_t *const *)((const uint8_t *)bullet + BUL_WEAPON);
    int n = wstuff ? stuff_drift(wstuff) : 0;
    if (n <= 0 && !g_dregCount) return;                 /* the usual case: not a drift weapon, and none in flight */
    DriftReg *mine = NULL, *spare = NULL;
    for (int i = 0; i < MAX_DRIFT; i++) {
        DriftReg *r = &g_dreg[i];
        if (r->bullet == bullet) { mine = r; continue; }
        if (r->bullet && (uint32_t)(g_quantSeq - r->seq) > DREG_STALE) dreg_release(r);
        if (!r->bullet && !spare) spare = r;
    }
    if (n > 0) {
        if (!mine) { if (!spare) return; mine = spare; mine->bullet = bullet; g_dregCount++; }
        mine->n = n; mine->seq = g_quantSeq;
    } else if (mine) dreg_release(mine);                /* this address held a drift round that went without a last tick */
}

/* Bullet::Quant: for a locked bullet the engine's atgm branch reads aimer+target; swap our point in for the
   duration of the call. Without an aimer (owner dead, item dropped) steer the velocity ourselves. */
static BulletQuantFn g_bulletQuantOrig;
static char hook_bullet_quant(void *bullet)
{
    if (!g_on) return g_bulletQuantOrig(bullet);
    if (!g_lockCount && !g_dregCount) return g_bulletQuantOrig(bullet);   /* 1.1.2: no locked and no drift_<N> round in flight */
    DriftReg *r = NULL;
    if (g_dregCount) { g_quantSeq++; r = dreg_find(bullet); if (r) r->seq = g_quantSeq; }
    Lock *l = g_lockCount ? lock_find(bullet) : NULL;
    if (!l) {
        if (r) drift_tick((uint8_t *)bullet, r->n);
        char alive = g_bulletQuantOrig(bullet);
        /* looked up again: the engine can re-launch the bullet inside its own quant, which passes dreg_launch */
        if (!alive && g_dregCount && (r = dreg_find(bullet)) != NULL) dreg_release(r);
        return alive;
    }
    uint8_t *b = (uint8_t *)bullet;
    if (memcmp(l->origin, b + BUL_ORIGIN, 12) && !l->fallback) { lock_drop(l); return g_bulletQuantOrig(bullet); }   /* slot reused */
    float tp[3]; lock_target_pos(l, tp);
    /* drift on a guided round: the engine flies it from its current position (no closed form to bend),
       so the lock point itself walks sideways with the distance flown -> the miss grows with range */
    if (l->ticks == 0) {
        const uint8_t *wstuff = NULL; int n = 0;
        if (rd_mem(b + BUL_WEAPON, &wstuff, 8) && wstuff) n = stuff_drift(wstuff);
        if (n > 0 && drift_params(l->origin, (const float *)(b + BUL_VEL), n, l->dperp, &l->dtan)) l->driftN = n;
    }
    if (l->driftN > 0) {
        /* distance flown from our own launch record (the bullet's +0xFC field gave offsets far above tan(N)*d) */
        const float *pos = (const float *)(b + BUL_POS);
        double dx = pos[0] - l->origin[0], dy = pos[1] - l->origin[1], dz = pos[2] - l->origin[2];
        double flown = sqrt(dx*dx + dy*dy + dz*dz);
        if (flown > 0 && flown < 1e6) for (int i = 0; i < 3; i++) tp[i] += (float)(l->dperp[i] * l->dtan * flown);
    }

    double *aimTarget = NULL; double saved[3];
    const uint8_t *owner = *(const uint8_t *const *)(b + BUL_OWNER);
    uint8_t *part = NULL; void *item = NULL; uint8_t *aimer = NULL;
    if (owner && rd_mem(owner + g_ownerItemOff, &part, 8) && part && (item = g_getHeldItem(part)) != NULL &&
        rd_mem((uint8_t *)item + g_aimerOff, &aimer, 8) && aimer) {
        aimTarget = (double *)(aimer + g_aimTargetOff);
        memcpy(saved, aimTarget, 24);
        aimTarget[0] = tp[0]; aimTarget[1] = tp[1]; aimTarget[2] = tp[2];
    }
    char alive = g_bulletQuantOrig(bullet);
    if (l->ticks++ == 0 && g_lockLog > 0)
        logf_("lock tick: %s, target %.1f %.1f %.1f, bullet at %.1f %.1f %.1f", aimTarget ? "engine guidance (aimer target swapped)" : "no aimer -> dll steering",
              tp[0], tp[1], tp[2], ((float *)(b + BUL_POS))[0], ((float *)(b + BUL_POS))[1], ((float *)(b + BUL_POS))[2]);
    if (aimTarget) memcpy(aimTarget, saved, 24);
    else if (alive) {
        /* no engine guidance this tick: rotate the velocity toward the target (<= LockTurn deg/s) and
           restart the closed-form flight from the current position */
        float *v = (float *)(b + BUL_VEL), *pos = (float *)(b + BUL_POS);
        double vd[3] = { v[0], v[1], v[2] }, speed = vnorm(vd);
        double want[3] = { tp[0] - pos[0], tp[1] - pos[1], tp[2] - pos[2] };
        if (speed > 1e-6 && vnorm(want) > 1.0) {
            int ms = 0; if (g_quantMs) rd_mem(g_quantMs, &ms, 4);
            if (ms <= 0 || ms > 1000) ms = 50;
            double maxTurn = g_lockTurn * 0.01745329 * ms / 1000.0;
            double dot = vd[0]*want[0] + vd[1]*want[1] + vd[2]*want[2];
            if (dot > 1) dot = 1;
            if (dot < -1) dot = -1;
            double ang = acos(dot);
            if (ang > 1e-4) {
                double t = ang <= maxTurn ? 1.0 : maxTurn / ang;
                double nd[3] = { vd[0] + (want[0] - vd[0]) * t, vd[1] + (want[1] - vd[1]) * t, vd[2] + (want[2] - vd[2]) * t };
                if (vnorm(nd) > 1e-6) {
                    for (int i = 0; i < 3; i++) v[i] = (float)(nd[i] * speed);
                    memcpy(b + BUL_ORIGIN, pos, 12);
                    *(int16_t *)(b + BUL_TICKS) = 0;
                    memcpy(l->origin, pos, 12);
                    l->fallback = 1;
                }
            }
        }
    }
    if (!alive || (*(uint16_t *)(b + BUL_FLAGS) & 0x400)) {
        if (g_lockLog > 0) {
            const float *pos = (const float *)(b + BUL_POS);
            double dx = pos[0] - tp[0], dy = pos[1] - tp[1], dz = pos[2] - tp[2];
            double ox = pos[0] - l->origin[0], oy = pos[1] - l->origin[1], oz = pos[2] - l->origin[2];
            double flown = sqrt(ox*ox + oy*oy + oz*oz);
            logf_("lock end: %d ticks, %.1f units from the target, flew %.0f units, drift offset %.1f%s", l->ticks, sqrt(dx*dx + dy*dy + dz*dz),
                  flown, l->driftN > 0 ? l->dtan * flown : 0.0, l->fallback ? " (dll steering)" : "");
        }
        lock_drop(l);
        /* a round that is only flagged finished keeps its registration until its last tick, as it kept drifting before */
        if (!alive && g_dregCount && (r = dreg_find(bullet)) != NULL) dreg_release(r);
    }
    return alive;
}

/* ---- setup ---------------------------------------------------------------------------- */
/* finders, then the hooks; 1 = feature ON */
static int setup(int install)
{
    int ok = find_launch() && find_spawn_bullet_parts() && find_aim_point() && find_stuff_tags()
          && find_bullet_launch() && find_quant() && find_aim_extras();
    /* Bullet::Launch + Bullet::Quant first: once SpawnBullet is hooked the lock path is live */
    if (!ok || (install && !(install_hook_at("Bullet::Launch", (void *)g_bulletLaunch, (void *)hook_bullet_launch, (void **)&g_bulletLaunchOrig) &&
                             install_hook_at("Bullet::Quant", (void *)g_bulletQuant, (void *)hook_bullet_quant, (void **)&g_bulletQuantOrig) &&
                             install_hook_at("SpawnBullet", (void *)g_spawnBullet, (void *)hook_spawn_bullet, (void **)&g_spawnBulletOrig)))) {
        logf_("lock-on guidance / drift       OFF (engine code not recognised%s)", g_bulletLaunchOrig || g_bulletQuantOrig ? "; the hooks already in place pass straight through" : "");
        return 0;
    }
    g_on = 1;
    logf_("lock-on guidance / drift       ON (stuff tag \"%s\" on the weapon + \"atgm\" on the ammo; fallback turn %g deg/s; drift_N up to %d deg per 100 units; multiplayer %s)",
          g_lockTag, g_lockTurn, g_driftMax, g_allowNet ? "allowed" : "off");
    log_mp_note(1);                                   /* drift always applies, so the dll always changes the simulation */
    if (!g_allowNet) logf_("lock-on stands down in network games (AllowMultiplayer=0); drift_N still applies there");
    return 1;
}

static void read_config(void)
{
    ini_str("LockTag", "lock_on", g_lockTag, sizeof g_lockTag);
    g_lockTagLen = strlen(g_lockTag);
    g_lockTurn = ini_int("LockTurn", 10);
    g_lockLog = g_verbose ? ini_int("LockLog", 10) : 0;
    g_driftMax = ini_int("DriftMax", 45);
    g_allowNet = ini_int("AllowMultiplayer", 0);
}

#ifndef GEM_TEST
/* called from gem_weaponscript.c DllMain; 1 = feature ON */
int guidance_init(void)
{
    int on = 0;
    if (gem_module_open()) {
        read_config();
        if (!g_lockTagLen) logf_("LockTag= is empty - nothing to do");
        else on = setup(1);
    }
    gem_module_close();
    return on;
}
#else
int guidance_selftest(uint8_t *base, const char *exePath, void ***hooked, int *nhooked, GemFound *found)
{
    gem_test_reset();
    g_testExe = exePath;
    g_bulletLaunch = NULL; g_bulletQuant = NULL; g_getHeldItem = NULL; g_quantMs = NULL;
    get_sections(base);
    read_config();
    int r = setup(1);
    *hooked = g_hooked; *nhooked = g_nhooked;
    gem_found(found);
    found->bulletLaunch = (uint8_t *)g_bulletLaunch; found->bulletQuant = (uint8_t *)g_bulletQuant;
    return r;
}
#endif
