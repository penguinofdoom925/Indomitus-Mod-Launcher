/* GEM Teleport - the {teleport} instruction, its sub-order, the lock and the hooks
 * Part of the unity build: #included by gem_teleport.c, not compiled on its own.
 *
 * {teleport {max_radius M} {pre_use_delay S} {post_use_delay S}}   (metres, seconds, seconds)
 *
 * execute(): the soldier joins the teleporting set, {on "teleport_begin"} fires and a TpOrder sub-order is installed in
 * the custom action order (engine SetSubOrder). The engine ticks it: after pre_use_delay (eSingleOrder's own ms
 * countdown, simulation time, so every multiplayer peer agrees) the soldier is moved with Entity::SetPos,
 * {on "teleport_jump"} fires, and after post_use_delay the order ends; its Finish takes the soldier out of the set and
 * fires {on "teleport_end"}.
 * While in the set: Entity::GiveOrder refuses new orders (hook, lock_orders) and the health damage actions skip the
 * unit (vtable hooks, invulnerable). The two are independent.
 * The eCustom IsApplicable hook refuses a ground click farther than max_radius from the soldier (forbidden cursor,
 * and the running order fails the same way); an entity click is already refused by {target ground}.
 */

/* ------------------------------------------------------------------------------------ */
/* instruction object                                                                   */
/* ------------------------------------------------------------------------------------ */
typedef struct TpInstr {
    void **vt;
    uint8_t nocheck, pad[7];                 /* +8: written by the engine's parser ("nocheck") */
    float maxRadius, preS, postS;            /* metres, seconds, seconds */
    float spread;                            /* metres: landing point scattered within this radius of the click (0 = exact) */
    float flightS;                           /* seconds of visible flight between pre and post (0 = 0.1-style instant move) */
    float arcRatio, arcMax;                  /* arc peak height = distance * arcRatio, capped at arcMax metres */
    float faceOffset;                        /* degrees, 0/90/180/270: turn toward the landing point (+ this); < 0 = don't turn */
    float bandMid, bandLong;                 /* metres: distance band events teleport_short / _mid / _long */
    float minRadius;                         /* metres: a click nearer than this is refused like one beyond max_radius (0 = none) */
    float spreadNear;                        /* metres: spread for a click at min_radius; grows linearly to `spread` at max_radius (< 0 = same as spread) */
    uint32_t magic;
} TpInstr;
#define TP_MAGIC 0x54504C54u                 /* 'TLPT' */
static void *g_instrVtStorage[5]; static void **const g_instrVt = g_instrVtStorage + 1;   /* [-1] = RTTI locator */
static struct { const char *name; void *(*ctor)(void); } g_instrEntry;
static void **g_instrNode;
static float g_defRadius = 30.0f, g_defPre = 0.5f, g_defPost = 0.5f, g_defSpread = 0.0f;
static float g_defFlight = 0.0f, g_defArcRatio = 0.2f, g_defArcMax = 8.0f, g_defFace = 0.0f, g_defBandMid = 15.0f, g_defBandLong = 28.0f;
static int g_fireEvents = 1, g_wantLock = 1, g_wantInvuln = 1, g_okEvents, g_okLock, g_okInvuln;
static volatile int g_instrInstalled;

/* ------------------------------------------------------------------------------------ */
/* teleporting set: units between the click and the release (read by the order and damage hooks) */
/* ------------------------------------------------------------------------------------ */
#define TP_MAXLOCK 64
static void *volatile g_locked[TP_MAXLOCK];
static int tp_is_locked(const void *e)
{
    if (!e) return 0;
    for (int i = 0; i < TP_MAXLOCK; i++) if (g_locked[i] == e) return 1;
    return 0;
}
static void tp_lock(void *e)
{
    if (tp_is_locked(e)) return;
    for (int i = 0; i < TP_MAXLOCK; i++) if (!g_locked[i]) { g_locked[i] = e; return; }
    logf_("teleport: more than %d units teleporting at once - %p is not locked nor invulnerable", TP_MAXLOCK, e);
}
static void tp_unlock(const void *e) { for (int i = 0; i < TP_MAXLOCK; i++) if (g_locked[i] == e) g_locked[i] = NULL; }

/* ------------------------------------------------------------------------------------ */
/* named events                                                                         */
/* ------------------------------------------------------------------------------------ */
#ifndef FX_TEST
static int g_evLogged;
static void fire_named(void *entity, const char *name)
{
    uint8_t *e = (uint8_t *)entity;
    if (!g_okEvents || !g_fireEvents || !e || !(e[ENT_FLAGS] & 2)) return;
    uint8_t *def = *(uint8_t **)(e + g_entDefOff);
    if (!def || !*(void **)(def + g_defSetOff)) return;      /* no compiled interaction set -> FireNamed would fault */
    __attribute__((aligned(16))) uint8_t ctx[0x400];
    __attribute__((aligned(16))) uint8_t tag[0x80];
    memset(ctx, 0, sizeof ctx);
    g_ctxCtor(ctx, entity);
    g_tagCtorCStr(tag, name);
    g_fireNamed(ctx, tag);
    g_tagDtor(tag);
    g_ctxDtor(ctx);
    if (g_verbose && g_evLogged < 100) { logf_("event \"%s\" -> entity %p", name, entity); if (++g_evLogged == 100) logf_("(further events not logged)"); }
}

/* ------------------------------------------------------------------------------------ */
/* the sub-order                                                                        */
/* ------------------------------------------------------------------------------------ */
typedef struct TpOrder {
    uint8_t base[ORD_SIZE];                  /* eSingleOrder: vptr, actor, link, flags, result, wait, retry, sub */
    uint8_t *entity;
    float pos[3];                            /* landing point */
    float start[3];                          /* take-off point (set when the flight starts) */
    int32_t preMs, postMs, flightMs;
    float arcH;                              /* world units */
    float faceOffset;                        /* degrees (0/90/180/270), < 0 = no turn */
    int32_t state;                           /* 0 new, 1 waiting to take off, 2 flying, 3 landed */
    int32_t active;                          /* in the teleporting set; cleared by the release */
    int32_t diag;                            /* flight ticks still to log (lift: lines) */
    float rot[9]; int32_t hasRot;            /* facing rows, re-applied after every SetPos */
    uint32_t magic;
} TpOrder;
static void *g_orderVtStorage[21]; static void **const g_orderVt = g_orderVtStorage + 1;   /* [-1] = RTTI locator, 0..19 */
typedef void *(*OrderDtorFn)(void *o, unsigned flags);
typedef uint64_t (*OrderTickFn)(void *o);
static OrderDtorFn g_singleOrderDtor;
static OrderTickFn g_singleOrderTick;
static int g_tpLogged, g_mtxLogged;

/* Entity matrix at +0x50: three axis rows (x, y, z; 3 floats each) then the position at +0x74. Entity::SetPos copies
   the whole 0x30 bytes, lets the class (vt 25) put the new position in, and writes it back, so rows written before
   SetPos are kept. Facing: the forward axis is taken to be row y (unconfirmed for GEM humans; {face_offset 90/180/270}
   in the action file turns it if a test shows another axis). Only exact 90-degree steps, no trig: same result on
   every peer. Row lengths are kept (any model scale survives). */
static void tp_face(uint8_t *entity, const float *from, const float *to, float offsetDeg)
{
    if (offsetDeg < 0.0f) return;
    float fx = to[0] - from[0], fy = to[1] - from[1], l = sqrtf(fx * fx + fy * fy);
    if (!(l > 1.0f)) return;                                 /* under 1 unit: keep the old facing */
    fx /= l; fy /= l;
    int q = ((int)(offsetDeg / 90.0f + 0.5f)) & 3;           /* 0, 90, 180, 270 counter-clockwise */
    for (int k = 0; k < q; k++) { float t = fx; fx = -fy; fy = t; }
    float *m = (float *)(entity + ENT_POS - 0x24);           /* +0x50 */
    float sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float sy = sqrtf(m[3] * m[3] + m[4] * m[4] + m[5] * m[5]);
    float sz = sqrtf(m[6] * m[6] + m[7] * m[7] + m[8] * m[8]);
    if (!(sx > 0.0f)) sx = 1.0f; if (!(sy > 0.0f)) sy = 1.0f; if (!(sz > 0.0f)) sz = 1.0f;
    if (g_verbose && g_mtxLogged < 20) {
        logf_("face: unit %p matrix before x(%.3f %.3f %.3f) y(%.3f %.3f %.3f) z(%.3f %.3f %.3f), target dir (%.3f %.3f) offset %d",
              entity, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], fx, fy, q * 90);
        g_mtxLogged++;
    }
    /* 0.2.1: forward = row x (0.2.0 log: fresh units are identity, walked units have row x = their heading) */
    m[0] = fx * sx;  m[1] = fy * sx;  m[2] = 0.0f;           /* x = forward    */
    m[3] = -fy * sy; m[4] = fx * sy;  m[5] = 0.0f;           /* y = z cross x  */
    m[6] = 0.0f;     m[7] = 0.0f;     m[8] = sz;             /* z = up         */
}

/* Move + lift. Entity::SetPos grounds a human (0.2.0 log: z read back 0.0 mid-arc), so with lift on the wanted z is
   written straight into the matrix afterwards and the world is notified again the way SetPos does (vt 33, reason 3).
   Whether the human's own tick grounds it again before the frame is drawn is what the lift: log lines show. */
static int g_lift = 1, g_liftLogged;
/* 0.2.2: rot (9 floats, the rows tp_face made) is written back the same way. In 0.2.1 the rows written before SetPos
   did not show (units jumping backwards kept their old facing): SetPos or the human's tick rebuilds them. */
static void tp_place(uint8_t *entity, const float *pt, const float *rot, int diag)
{
    float *p = (float *)(entity + ENT_POS), *m = (float *)(entity + ENT_POS - 0x24);
    float zBefore = p[2], rxBefore[2] = { m[0], m[1] };
    g_setPos(entity, pt, 0);
    float zSet = p[2], rxSet[2] = { m[0], m[1] };
    int changed = 0;
    if (g_lift && p[2] != pt[2]) { p[2] = pt[2]; changed = 1; }
    if (rot && memcmp(m, rot, 9 * sizeof(float))) { memcpy(m, rot, 9 * sizeof(float)); changed = 1; }
    if (changed) ((void (*)(void *, int, int))(*(void ***)entity)[33])(entity, 3, 0);
    if (diag && g_verbose && g_liftLogged < 60) {
        logf_("lift: unit %p want z %.1f | z at tick start %.1f, after SetPos %.1f, after write %.1f | row x at tick start (%.2f %.2f), after SetPos (%.2f %.2f), want (%.2f %.2f)",
              entity, pt[2], zBefore, zSet, p[2], rxBefore[0], rxBefore[1], rxSet[0], rxSet[1], rot ? rot[0] : 0.0f, rot ? rot[1] : 0.0f);
        g_liftLogged++;
    }
}

/* point of the flight at t (0..1): straight line in x/y, parabola on top of the straight z line (peak arcH at t 0.5) */
static void tp_arc(const TpOrder *o, float t, float *out)
{
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    out[0] = o->start[0] + (o->pos[0] - o->start[0]) * t;
    out[1] = o->start[1] + (o->pos[1] - o->start[1]) * t;
    out[2] = o->start[2] + (o->pos[2] - o->start[2]) * t + 4.0f * o->arcH * t * (1.0f - t);
}

static void tp_order_release(TpOrder *o, int fire)
{
    if (!o->active) return;
    o->active = 0;
    tp_unlock(o->entity);
    if (fire) fire_named(o->entity, "teleport_end");
}
/* eSingleOrder::Tick counts +0x24 down by the simulation quant while bit 20 is set, then calls Update(this, 1) */
static void tp_order_wait(TpOrder *o, int32_t ms)
{
    *(int32_t *)(o->base + ORD_WAIT) = ms;
    *(uint32_t *)(o->base + ORD_FLAGS) |= ORD_WAITING;
}
static int tp_entity_ok(const uint8_t *e) { return e && (e[ENT_FLAGS] & 2) && (e[ENT_FLAGS] & 4); }

/* slot 19: Update(this, phase) -> 0 running, 1 done, 2 failed */
static int tp_order_update(TpOrder *o, int phase)
{
    if (phase == 2) { tp_order_release(o, 1); return 0; }
    if (!tp_entity_ok(o->entity)) { if (g_verbose) logf_("teleport: unit %p gone before the jump - cancelled", o->entity); return 2; }
    if (phase == 0) {                                        /* first tick: arm the pre-delay */
        o->state = 1;
        if (o->preMs > 0) { tp_order_wait(o, o->preMs); return 0; }
    }
    if (o->state == 1) {                                     /* pre-delay over: take off (or the 0.1-style instant move) */
        float *p = (float *)(o->entity + ENT_POS);
        o->start[0] = p[0]; o->start[1] = p[1]; o->start[2] = p[2];
        tp_face(o->entity, o->start, o->pos, o->faceOffset);
        if (o->faceOffset >= 0.0f) { memcpy(o->rot, o->entity + ENT_POS - 0x24, sizeof o->rot); o->hasRot = 1; }
        if (o->flightMs > 0) {
            if (g_verbose && g_tpLogged < 50) logf_("teleport: unit %p takes off (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), %d ms, arc %.1f units",
                                                     o->entity, p[0], p[1], p[2], o->pos[0], o->pos[1], o->pos[2], o->flightMs, o->arcH);
            tp_place(o->entity, o->start, o->hasRot ? o->rot : NULL, 0);     /* applies the new facing at once */
            o->diag = 6;
            fire_named(o->entity, "teleport_takeoff");
            o->state = 2;
            tp_order_wait(o, o->flightMs);                   /* tp_order_tick moves him along the arc meanwhile */
            return 0;
        }
        o->state = 2;                                        /* no flight: fall through to the landing */
    }
    if (o->state == 2) {                                     /* flight over (or instant): land */
        float *p = (float *)(o->entity + ENT_POS);
        if (g_verbose && g_tpLogged < 50) logf_("teleport: unit %p (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)", o->entity, p[0], p[1], p[2], o->pos[0], o->pos[1], o->pos[2]);
        tp_place(o->entity, o->pos, o->hasRot ? o->rot : NULL, 0);          /* lands facing the way he flew */
        fire_named(o->entity, "teleport_jump");
        o->state = 3;
        if (g_tpLogged < 50) g_tpLogged++;
        if (o->postMs > 0) { tp_order_wait(o, o->postMs); return 0; }
    }
    return 1;
}
/* slot 3: Tick(this). The engine's eSingleOrder::Tick subtracts the simulation quant from the wait at +0x24 and
   returns 0 without calling Update while it is still > 0. During the flight that remaining time IS the progress:
   t = 1 - remaining / flightMs (simulation milliseconds, identical on every peer). */
static uint64_t tp_order_tick(TpOrder *o)
{
    uint64_t r = g_singleOrderTick(o);
    if (o->state == 2 && o->flightMs > 0 && (*(uint32_t *)(o->base + ORD_FLAGS) & ORD_WAITING) && tp_entity_ok(o->entity)) {
        int32_t rem = *(int32_t *)(o->base + ORD_WAIT);
        float pt[3];
        tp_arc(o, 1.0f - (float)rem / (float)o->flightMs, pt);
        tp_place(o->entity, pt, o->hasRot ? o->rot : NULL, o->diag-- > 0);
    }
    return r;
}
/* slot 4: Finish(this). The engine's Finish only calls Update(this, 2) when flag bit 18 is set (decoded 0.2.1:
   bt 0x10 ; bt 0x12 ; call vt[19](this, 2)), which our order never has, so teleport_end never fired in 0.1.x / 0.2.0
   (the lock was only dropped by the destructor). Release here: Tick calls Finish when Update returns done / failed,
   and the engine calls it for a cancelled order. */
typedef void (*OrderFinishFn)(void *o);
static OrderFinishFn g_singleOrderFinish;
static void tp_order_finish(TpOrder *o)
{
    tp_order_release(o, 1);
    g_singleOrderFinish(o);
}
/* slot 0: deleting dtor(this, flags) */
static void *tp_order_dtor(TpOrder *o, unsigned flags)
{
    tp_order_release(o, 0);                                  /* safety: a destroyed order never leaves a lock behind (no event: the unit may be gone) */
    g_singleOrderDtor(o, 0);                                 /* engine base: drops a sub-order, resets the vptr */
    if (flags & 1) g_engineDelete(o, sizeof *o);
    return o;
}

/* ------------------------------------------------------------------------------------ */
/* instruction vtable                                                                   */
/* ------------------------------------------------------------------------------------ */
static void *tp_instr_dtor(TpInstr *i, unsigned flags) { if (flags & 1) g_engineDelete(i, sizeof *i); return i; }
static uint8_t tp_instr_ready(TpInstr *i) { (void)i; return 1; }

static void tp_instr_parse(TpInstr *i, void *reader)
{
    int n = 0;
    while (g_rdPeek(reader, 4, 0) && n++ < 16) {
        g_rdEnter(reader);
        const MsString *w = (const MsString *)g_rdWord(reader, 1);
        const char *s = w ? ms_cstr(w) : "";
        float *dst = NULL;
        if (!_stricmp(s, "max_radius")) dst = &i->maxRadius;
        else if (!_stricmp(s, "pre_use_delay")) dst = &i->preS;
        else if (!_stricmp(s, "post_use_delay")) dst = &i->postS;
        else if (!_stricmp(s, "spread")) dst = &i->spread;
        else if (!_stricmp(s, "flight_time")) dst = &i->flightS;
        else if (!_stricmp(s, "arc_ratio")) dst = &i->arcRatio;
        else if (!_stricmp(s, "arc_max")) dst = &i->arcMax;
        else if (!_stricmp(s, "face_offset")) dst = &i->faceOffset;
        else if (!_stricmp(s, "band_mid")) dst = &i->bandMid;
        else if (!_stricmp(s, "band_long")) dst = &i->bandLong;
        else if (!_stricmp(s, "min_radius")) dst = &i->minRadius;
        else if (!_stricmp(s, "spread_near")) dst = &i->spreadNear;
        else logf_("{teleport}: unknown parameter \"%s\" (max_radius, min_radius, pre_use_delay, post_use_delay, spread, spread_near, flight_time, arc_ratio, arc_max, face_offset, band_mid, band_long)", s);
        if (g_rdPeek(reader, 9, 0)) { float v = g_rdFloat(reader); if (dst) *dst = v; }
        else if (dst) logf_("{teleport}: \"%s\" needs a number", s);
        g_rdLeave(reader);
    }
    if (!(i->maxRadius > 0.0f) || i->maxRadius > 100000.0f) i->maxRadius = g_defRadius;
    if (!(i->preS >= 0.0f) || i->preS > 600.0f) i->preS = g_defPre;
    if (!(i->postS >= 0.0f) || i->postS > 600.0f) i->postS = g_defPost;
    if (!(i->spread >= 0.0f) || i->spread > 1000.0f) i->spread = g_defSpread;
    if (!(i->flightS >= 0.0f) || i->flightS > 60.0f) i->flightS = g_defFlight;
    if (!(i->arcRatio >= 0.0f) || i->arcRatio > 10.0f) i->arcRatio = g_defArcRatio;
    if (!(i->arcMax >= 0.0f) || i->arcMax > 1000.0f) i->arcMax = g_defArcMax;
    if (!(i->faceOffset >= -1.0f) || i->faceOffset > 360.0f) i->faceOffset = g_defFace;
    if (!(i->minRadius >= 0.0f) || i->minRadius >= i->maxRadius) { if (i->minRadius != 0.0f) logf_("{teleport}: min_radius %g ignored (must be 0 .. max_radius)", i->minRadius); i->minRadius = 0.0f; }
    if (!(i->spreadNear >= 0.0f) || i->spreadNear > 1000.0f) i->spreadNear = -1.0f;
    logf_("{teleport} parsed: min_radius %g m, spread_near %g m (-1 = same as spread)", i->minRadius, i->spreadNear);
    if (!(i->bandMid >= 0.0f)) i->bandMid = g_defBandMid;
    if (!(i->bandLong >= i->bandMid)) i->bandLong = i->bandMid > g_defBandLong ? i->bandMid : g_defBandLong;
    logf_("{teleport} parsed: max_radius %g m, pre_use_delay %g s, post_use_delay %g s, spread %g m, flight_time %g s, arc %g x dist (max %g m), face_offset %g, bands <%g / <%g m",
          i->maxRadius, i->preS, i->postS, i->spread, i->flightS, i->arcRatio, i->arcMax, i->faceOffset, i->bandMid, i->bandLong);
}

/* Landing scatter. Must give the same point on every multiplayer peer, so no RNG state and no libm trig (sin/cos may
   differ in the last bit between UCRT builds): the seed is a hash of values the lockstep simulation already agrees on
   (the unit's start position, the clicked point), and the point is drawn by rejection sampling in the unit square
   (only +, -, *, compare: IEEE-exact everywhere). Units of one squad stand at different spots -> different points. */
static uint64_t tp_mix(uint64_t x)
{
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31; return x;
}
static void tp_scatter(const uint8_t *entity, const float *click, float radiusUnits, float *out)
{
    out[0] = click[0]; out[1] = click[1]; out[2] = click[2];
    if (!(radiusUnits > 0.0f)) return;
    uint32_t w[5];
    memcpy(w, entity + ENT_POS, 3 * sizeof(float));
    memcpy(w + 3, click, 2 * sizeof(float));
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int k = 0; k < 5; k++) h = tp_mix(h ^ w[k]);
    for (int tries = 0; tries < 32; tries++) {
        h = tp_mix(h + 0x9E3779B97F4A7C15ull);
        float x = (float)(int32_t)((h >> 8) & 0xFFFFFF) * (2.0f / 16777216.0f) - 1.0f;    /* [-1, 1) */
        float y = (float)(int32_t)((h >> 40) & 0xFFFFFF) * (2.0f / 16777216.0f) - 1.0f;
        if (x * x + y * y <= 1.0f) { out[0] = click[0] + x * radiusUnits; out[1] = click[1] + y * radiusUnits; return; }
    }
    /* 32 misses in a row (odds ~1e-21): land on the click */
}

static float tp_dist2d(const uint8_t *entity, const float *tgt)
{
    const float *p = (const float *)(entity + ENT_POS);
    float dx = p[0] - tgt[0], dy = p[1] - tgt[1];
    return sqrtf(dx * dx + dy * dy);
}

/* 0.2.10 cooldown: a unit carrying the script tag g_cdTag (ini cooldown_tag, default "tp_cooldown", empty = off) is refused:
   {on "teleport_cooldown"} fires on it and the action fails (no lock, no move). The timer itself is script (tag + delay), so it
   runs on the simulation clock on every peer. Entity script tags = vector at entity + g_entTagsOff (ini ent_tags, 0x4E8 in
   1.067, same layout GEM MeleeStance reads): begin/end of 0x60-byte Tags, MSVC std::string at +0x40. */
static char g_cdTag[64] = "tp_cooldown";
static uint32_t g_entTagsOff = 0x4E8;
static int tp_has_tag(const uint8_t *entity, const char *name)
{
    size_t m = strlen(name);
    if (!m || !entity) return 0;
    const uint8_t *b = *(const uint8_t *const *)(entity + g_entTagsOff);
    const uint8_t *e = *(const uint8_t *const *)(entity + g_entTagsOff + 8);
    if (!b || !e || e < b) return 0;
    uintptr_t len = (uintptr_t)(e - b);
    if (len % 0x60 || len > 0x60 * 1024) return 0;                      /* not a tag vector: never refuse on garbage */
    for (const uint8_t *t = b; t < e; t += 0x60) {
        const uint8_t *s = t + 0x40;
        uint64_t size = *(const uint64_t *)(s + 0x10), cap = *(const uint64_t *)(s + 0x18);
        if (cap < size || cap > 0x10000 || size != m) continue;
        const char *z = cap > 15 ? *(const char *const *)s : (const char *)s;
        size_t k = 0;
        for (; k < m; k++) { char a = z[k], c = name[k]; if (a >= 'A' && a <= 'Z') a += 32; if (c >= 'A' && c <= 'Z') c += 32; if (a != c) break; }
        if (k == m) return 1;
    }
    return 0;
}

/* slot 3: execute(this, action, order, entity, target, flagF0) -> bool */
static uint8_t tp_instr_execute(TpInstr *i, uint8_t *action, uint8_t *order, uint8_t *entity, uint8_t *target, uint8_t *f0)
{
    (void)action; (void)f0;
    if (!order || !target || !tp_entity_ok(entity)) { logf_("{teleport}: not a living human - action failed"); return 0; }
    if (*(int32_t *)(target + TGT_TYPE) != TGT_GROUND) { logf_("{teleport}: target is not a ground point (add {target ground} to the action) - action failed"); return 0; }
    if (g_cdTag[0] && tp_has_tag(entity, g_cdTag)) {         /* 0.2.10: on cooldown - tell the script, do nothing */
        if (g_verbose) logf_("{teleport}: unit %p has tag \"%s\" - on cooldown, action refused", entity, g_cdTag);
        fire_named(entity, "teleport_cooldown");
        return 0;
    }
    const float *tp = (const float *)(target + TGT_POS);
    float d = tp_dist2d(entity, tp), lim = i->maxRadius * *g_worldScale;
    if (d > lim) { if (g_verbose) logf_("{teleport}: target %.1f units away, limit %.1f - action failed", d, lim); return 0; }
    if (d < i->minRadius * *g_worldScale) { if (g_verbose) logf_("{teleport}: target %.1f units away, minimum %.1f - action failed", d, i->minRadius * *g_worldScale); return 0; }
    TpOrder *o = (TpOrder *)g_engineNew(sizeof *o);
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    *(void ***)o->base = g_orderVt;                          /* eSingleOrder ctor state = all zero; SetSubOrder sets +8 */
    o->entity = entity;
    float spreadM = i->spread;                               /* 0.2.9: spread_near at min_radius -> spread at max_radius, linear */
    if (i->spreadNear >= 0.0f && i->maxRadius > i->minRadius) {
        float f = (d / *g_worldScale - i->minRadius) / (i->maxRadius - i->minRadius);
        if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
        spreadM = i->spreadNear + (i->spread - i->spreadNear) * f;
    }
    tp_scatter(entity, tp, spreadM * *g_worldScale, o->pos);      /* z stays the clicked ground height */
    o->preMs = (int32_t)(i->preS * 1000.0f + 0.5f); o->postMs = (int32_t)(i->postS * 1000.0f + 0.5f);
    o->flightMs = (int32_t)(i->flightS * 1000.0f + 0.5f);
    o->faceOffset = i->faceOffset;
    float jumpM = tp_dist2d(entity, o->pos) / *g_worldScale;                 /* the real jump: start -> scattered landing */
    float arcM = jumpM * i->arcRatio; if (arcM > i->arcMax) arcM = i->arcMax;
    o->arcH = arcM * *g_worldScale;
    o->magic = TP_MAGIC; o->active = 1;
    tp_lock(entity);
    /* distance band first, so the script can pick the animation set before teleport_begin */
    fire_named(entity, jumpM < i->bandMid ? "teleport_short" : jumpM < i->bandLong ? "teleport_mid" : "teleport_long");
    if (g_verbose) logf_("{teleport}: unit %p click (%.1f %.1f %.1f) -> land (%.1f %.1f), %.1f units (limit %.1f), spread %.1f m, wait %d ms / %d ms",
                         entity, tp[0], tp[1], tp[2], o->pos[0], o->pos[1], d, lim, spreadM, o->preMs, o->postMs);
    g_setSubOrder(order, o);                                 /* finishes and deletes a previous sub-order, links ours */
    fire_named(entity, "teleport_begin");
    return 1;
}

static void *tp_instr_ctor(void)
{
    TpInstr *i = (TpInstr *)g_engineNew(sizeof *i);
    if (!i) return NULL;
    memset(i, 0, sizeof *i);
    i->vt = g_instrVt; i->maxRadius = g_defRadius; i->preS = g_defPre; i->postS = g_defPost; i->spread = g_defSpread;
    i->flightS = g_defFlight; i->arcRatio = g_defArcRatio; i->arcMax = g_defArcMax; i->faceOffset = g_defFace;
    i->bandMid = g_defBandMid; i->bandLong = g_defBandLong; i->spreadNear = -1.0f; i->magic = TP_MAGIC;
    return i;
}

/* ------------------------------------------------------------------------------------ */
/* hooks                                                                                */
/* ------------------------------------------------------------------------------------ */
static IsApplicableFn g_isApplicableOrig;
static TpInstr *tp_find_instr(const uint8_t *action)
{
    if (!action) return NULL;
    TpInstr **b = *(TpInstr ***)(action + g_instrBegOff), **e = *(TpInstr ***)(action + g_instrEndOff);
    if (!b || e < b || e - b > 256) return NULL;
    for (; b < e; b++) if (*b && (*b)->vt == g_instrVt && (*b)->magic == TP_MAGIC) return *b;
    return NULL;
}
/* 0.2.5: {on "teleport_aim"} while the player has a {teleport} action's cursor up. The engine asks IsApplicable for
   each selected unit against the point under the cursor; that call is the only sign of "aiming" (no instruction has
   run yet). The event is re-fired at most every AIM_MS per unit, so the script shows its range fx and lets a short
   named delay hide it again once the calls stop. Local player only by nature (UI state): the script must touch views
   only, no tags / damage / orders. Fired only on the thread the GiveOrder hook runs on (the engine thread). */
#define TP_AIM_MS 200
#define TP_MAXAIM 32
static int g_aimEvents = 1, g_aimLogged;
static DWORD g_engineTid;
/* 0.2.6 (from the 0.2.5 log): the engine asks IsApplicable on TWO threads. The window thread asks every frame for
   every selected unit; the engine thread only about every 0.4 s and not for every unit (events came ~0.8 s apart per
   unit, for 2 of the selected units: the rings flickered). So every call, from either thread, only stamps the unit as
   "aimed at now"; an engine-thread call then fires the event for EVERY unit stamped in the last TP_AIM_FRESH ms. */
#define TP_AIM_FRESH 400
/* 0.2.7 (Alex: no event spam): two events per aim, not one every 200 ms. {on "teleport_aim_begin"} once when a unit
   starts being aimed with, {on "teleport_aim_end"} once when it stops (cursor cancelled, unit deselected, jump given).
   Every IsApplicable call, on either thread, only stamps the unit. tp_aim_scan() turns stamps into the two events and
   must run on the engine thread; noticing the END needs a call after the aiming stopped, so it is driven by a
   per-quant callback from GEM MeleeStance (export gem_quant_listen, which owns the Scene::eQuantExecutor slot).
   Without that export the scan only runs from this DLL's own engine-thread entries (IsApplicable, GiveOrder), and
   the end event can then lag until the next order given to any unit. */
static struct { void *volatile entity; volatile ULONGLONG seen; int active; } g_aim[TP_MAXAIM];
static int g_quantReg; static ULONGLONG g_quantTry;
static void tp_aim_scan(void)
{
    ULONGLONG now = GetTickCount64();
    for (int k = 0; k < TP_MAXAIM; k++) {
        uint8_t *e = (uint8_t *)g_aim[k].entity;
        if (!e) continue;
        int fresh = now - g_aim[k].seen <= TP_AIM_FRESH;
        if (fresh && !g_aim[k].active) {
            g_aim[k].active = 1;
            if (g_aimLogged < 12) { logf_("aim: teleport_aim_begin -> unit %p", e); g_aimLogged++; }
            fire_named(e, "teleport_aim_begin");
        } else if (!fresh && g_aim[k].active) {
            g_aim[k].active = 0; g_aim[k].entity = NULL;
            if (g_aimLogged < 12) { logf_("aim: teleport_aim_end -> unit %p", e); g_aimLogged++; }
            fire_named(e, "teleport_aim_end");
        }
    }
}
static void tp_quant(void)                                   /* once per simulation quant, engine thread */
{
    g_engineTid = GetCurrentThreadId();
    if (g_aimEvents) tp_aim_scan();
}
static void tp_quant_register(void)
{
    if (g_quantReg) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_quantTry < 1000) return;
    g_quantTry = now;
    HMODULE h = GetModuleHandleA("gem_meleestance.dll");
    void (*reg)(void (*)(void)) = h ? (void (*)(void (*)(void)))(void *)GetProcAddress(h, "gem_quant_listen") : NULL;
    if (!reg) { if (g_aimLogged < 2) { logf_("aim: gem_meleestance.dll %s - no per-quant tick, teleport_aim_end may lag until the next order", h ? "has no gem_quant_listen export" : "not loaded"); g_aimLogged++; } return; }
    reg(tp_quant);
    g_quantReg = 1;
    logf_("aim: per-quant tick from GEM MeleeStance (gem_quant_listen) registered");
}
static void tp_aim(uint8_t *entity)
{
    if (!g_aimEvents || !entity) return;
    tp_quant_register();
    /* 0.2.8: only the WINDOW thread's calls count as aiming (the cursor test, every frame). The engine thread's calls
       include the running jump order re-checking its own action each tick: the one that arrives right after the lock
       is dropped looked like a new aim and flashed the rings on for ~0.5 s at every landing (Alex's clip, frame scan:
       on at click + 3.97 s = teleport_end, off 0.47 s later). */
    if (g_engineTid && GetCurrentThreadId() == g_engineTid) return;
    ULONGLONG now = GetTickCount64(); int slot = -1, spare = -1;
    for (int k = 0; k < TP_MAXAIM; k++) {
        if (g_aim[k].entity == entity) { slot = k; break; }
        if (spare < 0 && !g_aim[k].entity) spare = k;
    }
    if (slot < 0) { if (spare < 0) return; slot = spare; g_aim[slot].active = 0; g_aim[slot].seen = now; g_aim[slot].entity = entity; }
    g_aim[slot].seen = now;                                  /* stamp only; tp_aim_scan (engine thread) fires the events */
}

static uint8_t hook_is_applicable(uint8_t *action, uint8_t *flag, uint8_t *entity, uint8_t *target)
{
    uint8_t r = g_isApplicableOrig(action, flag, entity, target);
    if (!r) return 0;
    TpInstr *i = tp_find_instr(action);
    if (!i) return r;
    if (tp_is_locked(entity)) return r;                      /* the running order re-checks every tick: keep it going */
    tp_aim(entity);                                          /* before the range test: the rings show for an out-of-range cursor too */
    if (!entity || !target || *(int32_t *)(target + TGT_TYPE) != TGT_GROUND) return 0;
    float dd = tp_dist2d(entity, (const float *)(target + TGT_POS));
    return dd <= i->maxRadius * *g_worldScale && dd >= i->minRadius * *g_worldScale;
}

/* 0.2.3: order listeners for other payload DLLs (GEM MeleeStance). Exported so they need no second inline hook on
   Entity::GiveOrder. Each callback gets every (entity, order) at the entry of the hook, before the teleport lock
   decides, on the engine's thread, inside the order path: a callback must not give orders itself. Registering before
   the hook is installed is fine (the list is just filled early); the callbacks only run when the hook is live
   (lock_orders=1 and the GiveOrder finder passed). */
#define TP_MAXLISTEN 4
typedef void (*OrderListenFn)(void *entity, void *order);
static void *volatile g_listeners[TP_MAXLISTEN];
__declspec(dllexport) void gem_order_listen(OrderListenFn cb)
{
    if (!cb) return;
    for (int i = 0; i < TP_MAXLISTEN; i++) if (g_listeners[i] == (void *)cb) return;
    for (int i = 0; i < TP_MAXLISTEN; i++)
        if (!InterlockedCompareExchangePointer(&g_listeners[i], (void *)cb, NULL)) {
            logf_("order listeners: callback %p registered (slot %d of %d); live %s", (void *)cb, i + 1, TP_MAXLISTEN,
                  g_okLock ? "now" : "once the GiveOrder hook is installed");
            return;
        }
    logf_("order listeners: list full (%d), callback %p not registered", TP_MAXLISTEN, (void *)cb);
}

static GiveOrderFn g_giveOrderOrig; static int g_rejectLogged;
static uint64_t hook_give_order(uint8_t *entity, void **order, void *ctx)
{
    g_engineTid = GetCurrentThreadId();
    if (g_aimEvents && !g_quantReg) tp_aim_scan();           /* no per-quant tick: an order is the next chance to end an aim */
    if (order)
        for (int i = 0; i < TP_MAXLISTEN; i++) { OrderListenFn cb = (OrderListenFn)g_listeners[i]; if (cb) cb(entity, order); }
    if (order && tp_is_locked(entity)) {
        if (g_verbose && g_rejectLogged < 50) { logf_("lock: order refused for teleporting unit %p", entity); g_rejectLogged++; }
        ((void (*)(void *))((void **)order[0])[4])(order);               /* Finish */
        ((void (*)(void *, unsigned))((void **)order[0])[0])(order, 1);  /* delete */
        return 0;
    }
    return g_giveOrderOrig(entity, order, ctx);
}

static int g_dmgLogged;
static int tp_damage_skip(void *entity)
{
    if (!entity || !tp_is_locked(entity)) return 0;
    if (g_verbose && g_dmgLogged < 50) { logf_("invulnerable: damage ignored for teleporting unit %p", entity); g_dmgLogged++; }
    return 1;
}
#define DMG_HOOK(k) static void hook_damage_##k(void *a, void *ctx, void *e) { if (!tp_damage_skip(e)) g_damageExec[k](a, ctx, e); }
DMG_HOOK(0) DMG_HOOK(1) DMG_HOOK(2) DMG_HOOK(3) DMG_HOOK(4) DMG_HOOK(5)
static void *const g_dmgHooks[TP_NDMG] = { (void *)hook_damage_0, (void *)hook_damage_1, (void *)hook_damage_2,
                                           (void *)hook_damage_3, (void *)hook_damage_4, (void *)hook_damage_5 };

static void install_hooks(void)
{
    /* vtables for our objects: the instruction (4 slots) and the sub-order (eSingleOrder clone with 0 and 19 replaced;
       eSingleOrder's Start, slot 16, is an empty 'ret' and is kept) */
    g_instrVt[0] = (void *)tp_instr_dtor; g_instrVt[1] = (void *)tp_instr_parse; g_instrVt[2] = (void *)tp_instr_ready; g_instrVt[3] = (void *)tp_instr_execute;
    /* RTTI locators so a typeid / dynamic_cast on our objects reads a real one (an engine instruction's, eSingleOrder's) */
    void **vtSimple = find_vtable_rtti(".?AUeInstruction_disable_action_restart@Action@Interface@@");
    g_instrVt[-1] = vtSimple ? vtSimple[-1] : NULL;
    memcpy(g_orderVt, g_vtSingleOrder, 20 * sizeof(void *));
    g_orderVt[-1] = g_vtSingleOrder[-1];
    g_singleOrderDtor = (OrderDtorFn)g_vtSingleOrder[0];
    g_singleOrderTick = (OrderTickFn)g_vtSingleOrder[3];
    g_singleOrderFinish = (OrderFinishFn)g_vtSingleOrder[4];
    g_orderVt[0] = (void *)tp_order_dtor; g_orderVt[3] = (void *)tp_order_tick; g_orderVt[4] = (void *)tp_order_finish;
    g_orderVt[19] = (void *)tp_order_update;
    g_instrEntry.name = "teleport"; g_instrEntry.ctor = tp_instr_ctor;
    /* radius / ground check on the cursor and on the running order */
    int okApp = vt_hook(g_vtCustomAction, 15, (void *)hook_is_applicable, (void **)&g_isApplicableOrig);
    logf_("radius / ground check          %s", okApp ? "ON (eCustom IsApplicable)" : "OFF (vtable not writable) - the instruction still checks the radius");
    if (g_wantLock) {
        g_okLock = install_jmp5((uint8_t *)g_giveOrder, (void *)hook_give_order, (void **)&g_giveOrderOrig, "Entity::GiveOrder");
        logf_("orders refused while teleporting %s", g_okLock ? "ON" : "OFF (hook failed)");
    } else logf_("orders refused while teleporting off (ini)");
    if (g_wantInvuln) {
        int n = 0;
        for (int i = 0; i < g_nDamage; i++) { void *orig; if (vt_hook(g_vtDamage[i], 9, g_dmgHooks[g_dmgExecIdx[i]], &orig)) n++; }
        g_okInvuln = n == g_nDamage;
        logf_("invulnerable while teleporting  %s (%d/%d damage classes)", g_okInvuln ? "ON" : "PARTIAL", n, g_nDamage);
    } else logf_("invulnerable while teleporting  off (ini)");
}

/* ------------------------------------------------------------------------------------ */
/* registry insertion (same rules as ScriptExt's condition registry)                    */
/* ------------------------------------------------------------------------------------ */
static int rd_mem(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}
static int instr_registry_built(void) { int32_t g = *g_instrGuard; return g != 0 && g != -1; }
static int instr_insert(void **node)
{
    void *old;
    do {
        old = *(void *volatile *)g_instrHead;
        if (!old) return 0;
        node[0] = old; MemoryBarrier();
    } while (InterlockedCompareExchangePointer((void *volatile *)g_instrHead, node, old) != old);
    return 1;
}
static int instr_listed(void **node)
{
    void **n = *(void **volatile *)g_instrHead;
    if (!n) return -1;
    for (int i = 0; n && i < 4096; i++) {
        if (n == node) return 1;
        if (!rd_mem(n, &n, sizeof n)) return -1;
    }
    return 0;
}
static void install_instruction(void)
{
    void **node = (void **)g_engineNew(0x10);                /* the engine frees every node at exit with its own allocator */
    if (!node) { logf_("{teleport}: engine allocation failed"); g_instrInstalled = 1; return; }
    node[1] = &g_instrEntry;
    if (!instr_insert(node)) { logf_("{teleport}: registry is empty - not registered"); g_instrInstalled = 1; return; }
    g_instrNode = node;
    logf_("{teleport} instruction         ON (registered)");
    g_instrInstalled = 1;
}
static int check_instruction(void)
{
    if (!g_instrNode) return 0;
    int st = instr_listed(g_instrNode);
    if (st < 0) return 0;
    if (st == 1) return 1;
    if (!instr_insert(g_instrNode)) return 0;
    logf_("{teleport}: lost to a concurrent engine registration - registered again");
    return 1;
}
#endif /* FX_TEST */
