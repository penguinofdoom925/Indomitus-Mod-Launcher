/* GEM MeleeChase - melee chase logic
 * Part of the unity build: #included by gem_meleechase.c, not compiled on its own.
 *
 * HUMANS: nothing (0.5.0). The fight-distance multiplier (two Human::eOutdoorsBattle vtable slots + the eAttack const
 *   read) is removed: infantry melee belongs to GEM MeleeStance (stance, draw, charge, blow reach), and two DLLs moving
 *   the same hand-fight distance worked against each other. The "fight/distance" readers are still LOCATED, read-only:
 *   they are how find_weapons() reaches GetCurrentWeapon, the weapon list and Tag(const char*), which the vehicle part
 *   needs. Every hook below skips human entities.
 *
 * VEHICLES / FAUNA (approach)
 *   The vehicle brain (Actor::eVS_Root) has an "approach" behaviour: when its target is out of weapon range it
 *   gives itself ONE move order (Order::eMoveTask) into a circle of 0.95 x weapon range around the target's
 *   position at that moment. While that order runs the state does not run again, so a moving target is chased
 *   to where it WAS, and only then re-approached. For a unit with a melee-tagged weapon GEM MeleeChase:
 *   - re-plans the running approach when the point it drives to is off by a fraction of the remaining distance;
 *   - for a melee-reach approach, drives to contact_factor x reach (not the edge) and aims ahead of the target;
 *   - keeps pursuing a target that backs off inside weapon range (vanilla waits until it leaves range);
 *   - stops only when the target is in reach and not getting away. smooth_repath: re-plans don't brake.
 *   ai_prioritize_melee / ai_prioritize_ranged (def props) replace the approach range the brain computed.
 *
 * Simulation: it changes AI decisions. Every multiplayer peer needs the DLL (like GEM Fauna's squads).
 */

/* ------------------------------------------------------------------------------------ */
/* engine layout (found at startup)                                                     */
/* ------------------------------------------------------------------------------------ */
#define ENT_FLAGS    0x30        /* byte: bit1 alive / in scene, bit2 human, bit3 vehicle class */
#define ENT_POS      0x74        /* float x, y, z (world units) */
#define WORLD_SCALE  20.0f       /* world units per metre */
#define TAG_SIZE     0x60
#define TAG_NAME     0x40        /* MSVC std::string inside a Tag */

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

/* weapons: entity +o_weaponry -> weaponry; GetCurrentWeapon(weaponry) -> weapon in hands (or first weapon);
   weaponry +o_wlistBeg/End = vector<weapon*>; weapon +o_weapItem -> item entity; item +o_itemTagBeg/End = vector<Tag> */
typedef void *(*GetWeaponFn)(void *weaponry);
static GetWeaponFn g_getWeapon;
static uint32_t o_weaponry, o_wlistBeg, o_wlistEnd, o_weapItem, o_itemTagBeg, o_itemTagEnd;
static int g_okWeapons;

/* weapon finder (find_weapons): walks the human battle state's "fight/distance" readers to reach the weapon getter.
   Read-only since 0.5.0: nothing here is hooked any more. */
typedef void  (*TagCtorCStrFn)(void *tag, const char *s);
typedef float (*ConstGetFn)(void *brain, void *tag, float def);         /* brain const (metres) x world scale */
typedef uint64_t (*SlotFn)(void *self, uint64_t a, uint64_t b, uint64_t c);
static TagCtorCStrFn g_tagCtorCStr;
static ConstGetFn g_constGet;
static uint8_t *g_fdStr, *g_fdSiteState, *g_fdSiteAttack;               /* "fight/distance"; its two readers' getter calls */
static uint32_t o_fightOff;                                             /* eOutdoorsBattle: cached fight distance */
static uint32_t o_hStateBrain, o_hBrainEntity;                          /* state +x -> brain, brain +x -> entity */
static void **g_vtBattle;                                               /* Human::eOutdoorsBattle */
#define MAX_HSLOTS 4
static int g_hSlot[MAX_HSLOTS], g_hSlotN;

/* vehicles */
typedef void *(*MoveCircleFn)(void *circle, uint64_t flags);
typedef uint64_t (*UpdateFn)(void *order, uint64_t phase, uint64_t a, uint64_t b);
typedef void (*FinishSubFn)(void *order);
static void **g_vtVsRoot, **g_vtMoveTask;
static SlotFn g_vsTickOrig;
static MoveCircleFn g_moveCircle, g_moveCircleOrig;
static UpdateFn g_mtUpdateOrig;
typedef void *(*DtorFn)(void *self, uint64_t flags);
static DtorFn g_mtDtorOrig;
static FinishSubFn g_finishSub;                                          /* eMoveTask phase 2 minus the chassis stop */
static uint8_t *g_apSite;                                               /* "approach": call eMoveTask(circle) */
static uint32_t o_vStateBrain, o_vBrainEntity, o_vMode, o_vRecBeg, o_vRecEnd, o_recX, o_recY, o_aiOrder;
static volatile uint32_t *g_simTime;                                    /* simulation time, ms */
static int g_okVehicle;

/* weapon priority props (vehicles / fauna): override the approach range the vehicle brain computed */
typedef float (*WeaponRangeFn)(void *weapon);                           /* world units */
static WeaponRangeFn g_weaponRange;
static uint32_t o_approach;                                             /* brain: approach range (world units), -1 = none */
static uint32_t o_recEnt;                                               /* target record -> target entity (compared only) */
static uint32_t o_entDef, o_defPropBeg, o_defPropEnd;                   /* entity -> def; def {props} = vector<Tag> */
static char g_propMelee[48] = "ai_prioritize_melee", g_propRanged[48] = "ai_prioritize_ranged";
static int g_okPriority, g_wantPriority = 1;

/* hold ranged fire in melee: the vehicle target evaluation hands each weapon its target with Weapon::SetTarget(weapon,
   eTarget*, a, mode, b); an eTarget of type 0 clears it (SetTarget then calls Clear(weapon, 1)) */
typedef uint64_t (*SetTargetFn)(void *weapon, void *desc, uint64_t a, uint64_t mode, uint64_t b);
typedef uint64_t (*ClearTargetFn)(void *weapon, uint64_t how);
typedef uint64_t (*EvalFn)(void *brain, uint64_t a, uint64_t b, uint64_t c);
static SetTargetFn g_setTarget;
static ClearTargetFn g_clearTarget;
static uint8_t *g_evalFn, *g_evalJump;                                  /* target evaluation; the one jump/call into it */
#define MAX_ST_SITES 8
static uint8_t *g_stSites[MAX_ST_SITES]; static int g_stSiteN;
static uint32_t o_weapOwner;                                            /* weapon -> owner entity */
static int g_okHold, g_wantHold = 1;
static float g_holdFactor = 1.0f;

/* settings */
static char  g_meleeTag[48] = "melee"; static size_t g_meleeTagLen = 5;
/* re-plan threshold = clamp(distance x fraction, min, max); checks every repathMs, or repathMsClose within closeRange */
static float g_repathFrac = 0.25f, g_repathMin = 1.0f * WORLD_SCALE, g_repathMax = 10.0f * WORLD_SCALE;
static uint32_t g_repathMs = 500, g_repathMsClose = 200;
static float g_closeRange = 10.0f * WORLD_SCALE;
/* close combat: goal radius = melee reach x contact; aim lead_seconds ahead (at most lead_max); pursue a target backing
   off faster than g_recede even inside weapon range */
static float g_contact = 0.5f, g_leadSec = 0.6f, g_leadMax = 6.0f * WORLD_SCALE, g_recede = 0.3f * WORLD_SCALE;
static int   g_pursue = 1, g_okClose;
/* chase_always: a melee unit (melee-only, or ai_prioritize_melee) pursues whenever its target is outside 90% of its
   reach, not only when the target backs off; chase_run_mode: movement mode set on the chassis for a chase (-1 = keep) */
static int   g_chaseAlways = 1, g_chaseMode = 2;
static uint32_t o_chassis, o_modeSlot;                                   /* entity -> chassis; chassis vtable byte offset of SetMovementMode */
static int   g_smooth = 1, g_logChase = 1;

/* ------------------------------------------------------------------------------------ */
/* helpers                                                                              */
/* ------------------------------------------------------------------------------------ */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

static int rd_u32(const uint8_t *p) { return *(const int32_t *)p; }
static uint8_t *rel32_target(uint8_t *insn, int len) { return insn + len + rd_u32(insn + len - 4); }

/* Tags are interned: two Tags are equal when their first qword (the id) is equal (the engine's Tag== at exe+0x556810).
   The ids of the melee tag and the two priority props are built once, lazily on the first engine call that needs them
   (engine thread, after CRT init), with the engine's Tag(const char*) ctor. Until then / without the ctor: name compare. */
static uint64_t g_idMelee, g_idPropMelee, g_idPropRanged;
static volatile LONG g_idState;                                         /* 0 none, 1 building, 2 ready, 3 unavailable */
static TagCtorCStrFn g_tagCtorForIds;                                   /* set by find_weapons (Tag::Tag(const char*)) */
static int ids_ready(void)
{
#ifdef FX_TEST
    return 0;
#else
    if (g_idState == 2) return 1;
    if (g_idState != 0) return 0;
    if (InterlockedCompareExchange(&g_idState, 1, 0) != 0) return 0;
    if (!g_tagCtorForIds) { g_idState = 3; return 0; }
    static uint8_t buf[3][TAG_SIZE * 2] __attribute__((aligned(16)));   /* never freed */
    const char *names[3] = { g_meleeTag, g_propMelee, g_propRanged };
    uint64_t *ids[3] = { &g_idMelee, &g_idPropMelee, &g_idPropRanged };
    for (int i = 0; i < 3; i++) {
        char low[48]; size_t k = 0;
        for (; names[i][k] && k < sizeof low - 1; k++) low[k] = (char)((names[i][k] >= 'A' && names[i][k] <= 'Z') ? names[i][k] + 32 : names[i][k]);
        low[k] = 0;
        g_tagCtorForIds(buf[i], low);
        *ids[i] = *(uint64_t *)buf[i];
    }
    MemoryBarrier();
    g_idState = 2;
    return 1;
#endif
}

/* safety: for the first checks, the id result is compared with the name compare; any disagreement turns ids off */
static volatile LONG g_idChecks, g_idPos;                               /* checks done / of which positive (both agreed on a hit) */
#define IDS_TRUSTED (g_idChecks > 400 && g_idPos > 5)
static int id_verify(int byId, int byName, const char *what)
{
    if (byId != byName) {
        g_idState = 3;
#ifndef FX_TEST
        logf_("tag ids disagree with names for \"%s\" - using name compares", what);
#endif
        return byName;
    }
#ifndef FX_TEST
    if (byId) InterlockedIncrement(&g_idPos);
#endif
    return byId;
}

/* does the tag vector [beg,end) contain the melee tag */
static int tags_have_melee(uint8_t *beg, uint8_t *end)
{
    if (!beg || end < beg || (size_t)(end - beg) > TAG_SIZE * 256) return 0;
    int byId = -1;
    if (ids_ready()) {
        byId = 0;
        for (uint8_t *t = beg; t + TAG_SIZE <= end; t += TAG_SIZE) if (*(uint64_t *)t == g_idMelee) { byId = 1; break; }
        if (IDS_TRUSTED) return byId;
    }
    int byName = 0;
    for (uint8_t *t = beg; t + TAG_SIZE <= end && !byName; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_NAME);
        if (s->size == g_meleeTagLen && !_strnicmp(ms_cstr(s), g_meleeTag, g_meleeTagLen)) byName = 1;
    }
    if (byId < 0) return byName;
#ifndef FX_TEST
    InterlockedIncrement(&g_idChecks);
#endif
    return id_verify(byId, byName, g_meleeTag);
}
static int weapon_is_melee(uint8_t *w)
{
    if (!w) return 0;
    uint8_t *item = *(uint8_t **)(w + o_weapItem);
    if (!item) return 0;
    return tags_have_melee(*(uint8_t **)(item + o_itemTagBeg), *(uint8_t **)(item + o_itemTagEnd));
}
/* def {props} contains name (case-insensitive) */
static int def_has_prop(uint8_t *e, const char *name, const uint64_t *id)
{
    size_t n = strlen(name);
    uint8_t *def = *(uint8_t **)(e + o_entDef);
    if (!def || !n) return 0;
    uint8_t *b = *(uint8_t **)(def + o_defPropBeg), *en = *(uint8_t **)(def + o_defPropEnd);
    if (!b || en < b || (size_t)(en - b) > TAG_SIZE * 256) return 0;
    int byId = -1;
    if (ids_ready()) {
        byId = 0;
        for (uint8_t *t = b; t + TAG_SIZE <= en; t += TAG_SIZE) if (*(uint64_t *)t == *id) { byId = 1; break; }
        if (IDS_TRUSTED) return byId;
    }
    int byName = 0;
    for (uint8_t *t = b; t + TAG_SIZE <= en && !byName; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_NAME);
        if (s->size == n && !_strnicmp(ms_cstr(s), name, n)) byName = 1;
    }
    if (byId < 0) return byName;
#ifndef FX_TEST
    InterlockedIncrement(&g_idChecks);
#endif
    return id_verify(byId, byName, name);
}

/* ------------------------------------------------------------------------------------ */
/* finders                                                                              */
/* ------------------------------------------------------------------------------------ */

/* weapon -> item -> item tags, as read by the AI's "can take this pose" test (held item tagged cannot_lie):
   mov rax,[rax+ITEM] ; mov rdi,[rax+TAGS_END] ; mov rbx,[rax+TAGS_BEG]  (TAGS_END = TAGS_BEG + 8) */
static int find_weapon_layout(void)
{
    static const uint8_t pat[] = { 0x48, 0x8B, 0x80 };
    int hits = 0;
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p - 24, pat, 3)); p++) {
        if (p[5] || p[6] || p[7] != 0x48 || p[8] != 0x8B || p[9] != 0xB8 || p[14] != 0x48 || p[15] != 0x8B || p[16] != 0x98) continue;
        uint32_t item = *(uint32_t *)(p + 3), end = *(uint32_t *)(p + 10), beg = *(uint32_t *)(p + 17);
        if (item > 0x200 || end != beg + 8 || beg < 0x400 || beg > 0x4000) continue;
        if (!hits) { o_weapItem = item; o_itemTagBeg = beg; o_itemTagEnd = end; }
        else if (item != o_weapItem || beg != o_itemTagBeg) { logf_("weapons: held-item tag reads disagree"); return 0; }
        hits++;
    }
    if (!hits) { logf_("weapons: held-item tag read not found"); return 0; }
    logf_("weapons: weapon +0x%X -> item, item tags +0x%X..+0x%X (%d site(s))", o_weapItem, o_itemTagBeg, o_itemTagEnd, hits);
    return 1;
}

/* the two "fight/distance" readers: lea rdx,str ; ... call Tag::Tag(cstr) ; ... call ConstGet(brain, tag, def) */
static int fd_site(uint8_t *lea, uint8_t **ctor, uint8_t **getSite, uint8_t **getFn)
{
    uint8_t *s1, *s2;
    *ctor = next_call(lea + 7, 0x20, &s1);
    if (!*ctor) return 0;
    *getFn = next_call(s1 + 5, 0x40, &s2);
    if (!*getFn) return 0;
    *getSite = s2;
    return 1;
}

/* 0.5.0: was find_human. Locates (never patches) the human battle state's "fight/distance" readers, because one of those
   slots is the shortest known road to GetCurrentWeapon, the entity's weaponry offset and the weapon list; also the
   Tag(const char*) ctor used for the interned tag ids. The log lines keep their "human:" prefix. */
static int find_weapons(void)
{
    g_fdStr = find_cstr("fight/distance");
    if (!g_fdStr) { logf_("human: \"fight/distance\" not found"); return 0; }
    uint8_t *sites[4]; int n = 0;
    for (int k = 0; k < 4 && n < 4; k++) { uint8_t *l = find_lea_to(g_fdStr, k); if (!l) break; if (l[0] == 0x48 && l[1] == 0x8D && l[2] == 0x15) sites[n++] = l; }
    if (n != 2) { logf_("human: expected 2 readers of \"fight/distance\", found %d", n); return 0; }
    uint8_t *ctor[2], *gs[2], *gf[2];
    for (int i = 0; i < 2; i++) if (!fd_site(sites[i], &ctor[i], &gs[i], &gf[i])) { logf_("human: reader %d not recognised", i); return 0; }
    if (gf[0] != gf[1] || ctor[0] != ctor[1]) { logf_("human: the two readers call different functions"); return 0; }
    g_constGet = (ConstGetFn)gf[0]; g_tagCtorCStr = (TagCtorCStrFn)ctor[0]; g_tagCtorForIds = g_tagCtorCStr;
    /* the state's reader stores the result: movss [reg+disp32],xmm0 within 0x20 bytes after the call */
    int stateIdx = -1;
    for (int i = 0; i < 2 && stateIdx < 0; i++)
        for (uint8_t *q = gs[i] + 5; q < gs[i] + 0x20; q++)
            if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x11 && (q[3] & 0xC0) == 0x80 && (q[3] & 7) != 4) {
                o_fightOff = *(uint32_t *)(q + 4); stateIdx = i; break;
            }
    if (stateIdx < 0 || o_fightOff < 0x100 || o_fightOff > 0x2000) { logf_("human: cached fight distance field not found"); return 0; }
    g_fdSiteState = gs[stateIdx]; g_fdSiteAttack = gs[1 - stateIdx];

    g_vtBattle = find_vtable_rtti(".?AUeOutdoorsBattle@Human@@");
    if (!g_vtBattle) { logf_("human: Human::eOutdoorsBattle vtable not found"); return 0; }
    /* slots whose function reads the cached field: movss xmm,[reg+off] (optional REX 41..47), one pass over .text */
    uint32_t fns[12]; int nfn = 0;
    while (nfn < 12 && in_text(g_vtBattle[nfn])) { fns[nfn] = (uint32_t)((uint8_t *)g_vtBattle[nfn] - g_base); nfn++; }
    int hit[12] = { 0 };
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p - 8, (const uint8_t *)"\xF3", 1)); p++) {
        uint8_t *q = p + 1; if ((q[0] & 0xF0) == 0x40) q++;
        if (q[0] != 0x0F || q[1] != 0x10 || (q[2] & 0xC0) != 0x80 || (q[2] & 7) == 4) continue;
        if (*(uint32_t *)(q + 3) != o_fightOff) continue;
        uint32_t f = func_of(p);
        for (int s = 0; s < nfn; s++) if (fns[s] == f) hit[s] = 1;
    }
    g_hSlotN = 0;
    for (int s = 0; s < nfn; s++)
        if (hit[s]) { if (g_hSlotN >= MAX_HSLOTS) { logf_("human: too many slots read the field"); return 0; } g_hSlot[g_hSlotN++] = s; }
    if (g_hSlotN < 1) { logf_("human: no eOutdoorsBattle slot reads the field"); return 0; }

    /* in one of those slots: mov rcx,[rcx+B] (state->brain) ... mov rcx,[rcx+E] (brain->entity) ... mov rcx,[rcx+W] ; call GetCurrentWeapon */
    for (int i = 0; i < g_hSlotN && !g_getWeapon; i++) {
        uint8_t *f = (uint8_t *)g_vtBattle[g_hSlot[i]];
        uint32_t b = 0, e = 0;
        for (uint8_t *q = f; q < f + 0x80; q++) {
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x49) { if (!b) b = q[3]; else if (!e) e = q[3]; }
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x89 && q[7] == 0xE8 && b && e) {
                o_hStateBrain = b; o_hBrainEntity = e; o_weaponry = *(uint32_t *)(q + 3);
                g_getWeapon = (GetWeaponFn)rel32_target(q + 7, 5); break;
            }
        }
    }
    if (!g_getWeapon || !in_text((void *)g_getWeapon) || o_weaponry < 0x400 || o_weaponry > 0x4000) { logf_("human: weapon getter not found"); return 0; }
    /* GetCurrentWeapon falls back to the first weapon of the list: mov rax,[rcx+L] ; cmp rax,[rcx+L+8] */
    for (uint8_t *q = (uint8_t *)g_getWeapon; q < (uint8_t *)g_getWeapon + 0x40; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x41 && q[4] == 0x48 && q[5] == 0x3B && q[6] == 0x41 && q[7] == q[3] + 8) { o_wlistBeg = q[3]; o_wlistEnd = q[7]; break; }
    if (!o_wlistBeg) { logf_("human: weapon list not found in the weapon getter"); return 0; }
    logf_("human: fight distance field +0x%X, slots", o_fightOff);
    for (int i = 0; i < g_hSlotN; i++) logf_("  eOutdoorsBattle slot %d = exe+0x%llX", g_hSlot[i], EXE_OFF(g_vtBattle[g_hSlot[i]]));
    logf_("human: const getter exe+0x%llX, eAttack reader call exe+0x%llX, state->brain +0x%X, brain->entity +0x%X",
          EXE_OFF(g_constGet), EXE_OFF(g_fdSiteAttack), o_hStateBrain, o_hBrainEntity);
    logf_("weapons: entity +0x%X -> weaponry, current weapon exe+0x%llX, list +0x%X..+0x%X", o_weaponry, EXE_OFF(g_getWeapon), o_wlistBeg, o_wlistEnd);
    return 1;
}

static int find_vehicle(void)
{
    g_vtVsRoot = find_vtable_rtti(".?AUeVS_Root@Actor@@");
    g_vtMoveTask = find_vtable_rtti(".?AUeMoveTask@Order@@");
    void **vtSingle = find_vtable_rtti(".?AVeSingleOrder@@");
    if (!g_vtVsRoot || !g_vtMoveTask || !vtSingle) { logf_("vehicle: vtables not found (eVS_Root %p, eMoveTask %p, eSingleOrder %p)", (void *)g_vtVsRoot, (void *)g_vtMoveTask, (void *)vtSingle); return 0; }
    uint8_t *tick = (uint8_t *)g_vtVsRoot[3];
    if (!in_text(tick)) return 0;
    uint32_t tickRva = (uint32_t)(tick - g_base);

    /* tick head: mov rcx,[rcx+B] ; cmp dword [rcx+M],1 ; mov rax,[rcx+E] ; mov r12,[rcx+R] ; cmp r12,[rcx+R+8] */
    /* (the order of the entity load and the mode compare differs between the exes) */
    for (uint8_t *q = tick; q < tick + 0xC0; q++) {
        if (!o_vStateBrain && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x49) o_vStateBrain = q[3];
        if (!o_vMode && o_vStateBrain && q[0] == 0x83 && q[1] == 0xB9 && q[6] == 0x01) o_vMode = *(uint32_t *)(q + 2);
        if (!o_vBrainEntity && o_vStateBrain && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x41) o_vBrainEntity = q[3];
        if (!o_vRecBeg && q[0] == 0x4C && q[1] == 0x8B && q[2] == 0xA1 && q[7] == 0x4C && q[8] == 0x3B && q[9] == 0xA1 &&
            *(uint32_t *)(q + 10) == *(uint32_t *)(q + 3) + 8) { o_vRecBeg = *(uint32_t *)(q + 3); o_vRecEnd = o_vRecBeg + 8; }
    }
    if (!o_vStateBrain || !o_vMode || !o_vBrainEntity || !o_vRecBeg) { logf_("vehicle: tick head not recognised"); return 0; }

    /* the approach: movabs rax,"approach" in the tick, then the call that builds an eMoveTask from the circle */
    static const uint8_t ap[] = { 0x48, 0xB8, 'a', 'p', 'p', 'r', 'o', 'a', 'c', 'h' };
    uint8_t *mov = NULL;
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p, ap, sizeof ap)); p++)
        if (func_of(p) == tickRva) { mov = p; break; }
    if (!mov) { logf_("vehicle: \"approach\" not found in the eVS_Root tick"); return 0; }
    for (uint8_t *q = mov + 10; q < mov + 0xA0 && !g_apSite; q++) {
        if (q[0] != 0xE8) continue;
        uint8_t *t = rel32_target(q, 5);
        if (!in_text(t)) continue;
        for (uint8_t *r = t; r < t + 0x40; r++)
            if (r[0] == 0x48 && r[1] == 0x8D && r[2] == 0x05 && r + 7 + rd_u32(r + 3) == (uint8_t *)g_vtMoveTask) { g_apSite = q; g_moveCircle = (MoveCircleFn)t; break; }
    }
    if (!g_apSite) { logf_("vehicle: approach move order call not found"); return 0; }
    /* right after: mov rcx,[rsi+B] ; cmp rax,[rcx+AI] (is it already the brain's AI order) */
    for (uint8_t *q = g_apSite + 5; q < g_apSite + 0x20; q++)
        if (q[0] == 0x48 && q[1] == 0x3B && q[2] == 0x81) { o_aiOrder = *(uint32_t *)(q + 3); break; }
    if (o_aiOrder < 0x40 || o_aiOrder > 0x400) { logf_("vehicle: AI order slot not found"); return 0; }
    /* target record position: movss xmm0,[r12+X] ; movss xmm1,[r12+Y] in the tick */
    for (uint8_t *p = tick; p < tick + 0x800 && !o_recX; p++) {
        if (p[0] != 0xF3 || p[1] != 0x41 || p[2] != 0x0F || p[3] != 0x10 || p[4] != 0x44 || p[5] != 0x24) continue;
        for (uint8_t *q = p + 7; q < p + 0x18; q++)
            if (q[0] == 0xF3 && q[1] == 0x41 && q[2] == 0x0F && q[3] == 0x10 && q[4] == 0x4C && q[5] == 0x24 && q[6] == p[6] + 4) { o_recX = p[6]; o_recY = q[6]; break; }
    }
    if (!o_recX) { logf_("vehicle: target record position not found"); return 0; }

    /* eMoveTask slot 0 = deleting dtor(this, flags): test bl,1 ; ... (hooked to forget deleted orders) */
    if (!in_text(g_vtMoveTask[0]) || !find_bytes((uint8_t *)g_vtMoveTask[0], 0x20, (const uint8_t *)"\xF6\xC3\x01", 3)) { logf_("vehicle: eMoveTask dtor not recognised"); return 0; }
    /* eMoveTask Update (slot 19): switch on phase 0/1/2; phase 2 = chassis stop, then FinishSub(order) */
    uint8_t *upd = (uint8_t *)g_vtMoveTask[19];
    static const uint8_t ph[] = { 0x85, 0xD2, 0x74 };
    uint8_t *sw = find_bytes(upd, 0x40, ph, 3);
    if (!sw || sw[4] != 0x83 || sw[5] != 0xEA || sw[6] != 0x01) { logf_("vehicle: eMoveTask Update not recognised"); return 0; }
    static const uint8_t fs[] = { 0x33, 0xD2, 0x48, 0x8B, 0x09, 0xE8 };
    uint8_t *f2 = find_bytes(upd, 0xA0, fs, sizeof fs);
    if (f2 && f2[10] == 0x48 && f2[11] == 0x8B && f2[12] == 0xCB && f2[13] == 0xE8) g_finishSub = (FinishSubFn)rel32_target(f2 + 13, 5);
    if (!g_finishSub) logf_("vehicle: eMoveTask phase-2 helper not found - smooth_repath off");

    /* simulation clock: eSingleOrder::Tick does mov eax,[rip+QUANT] ; sub [rbx+0x24],eax ; the clock is the dword before */
    uint8_t *st = (uint8_t *)vtSingle[3];
    for (uint8_t *q = st; q < st + 0x40; q++)
        if (q[0] == 0x8B && q[1] == 0x05 && q[6] == 0x29 && q[7] == 0x43) { g_simTime = (volatile uint32_t *)(rel32_target(q, 6) - 4); break; }
    if (!g_simTime || !in_data((void *)g_simTime)) { logf_("vehicle: simulation clock not found"); return 0; }
    /* cross-check: a getter 'mov eax,[rip+CLOCK] ; ret' exists */
    int getter = 0;
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p - 8, (const uint8_t *)"\x8B\x05", 2)); p++)
        if (p[6] == 0xC3 && rel32_target(p, 6) == (uint8_t *)g_simTime) { getter = 1; break; }
    if (!getter) { logf_("vehicle: simulation clock not confirmed"); return 0; }

    logf_("vehicle: eVS_Root tick exe+0x%llX (state->brain +0x%X, mode +0x%X, brain->entity +0x%X, targets +0x%X, AI order +0x%X)",
          EXE_OFF(tick), o_vStateBrain, o_vMode, o_vBrainEntity, o_vRecBeg, o_aiOrder);
    logf_("vehicle: approach call exe+0x%llX -> eMoveTask(circle) exe+0x%llX, eMoveTask Update exe+0x%llX, record pos +0x%X/+0x%X, clock exe+0x%llX",
          EXE_OFF(g_apSite), EXE_OFF(g_moveCircle), EXE_OFF(upd), o_recX, o_recY, EXE_OFF(g_simTime));
    return 1;
}

/* weapon priority: the tick reads the approach range (movss xmm0,[rcx+A] ; comiss xmm0,xmm14 ; jb) then checks the target
   with mov rdx,r12 ; call T, and T reads the target's def props (mov rax,[rsi+DEF] ; mov rdi,[rax+END] ; mov rbx,[rax+BEG]).
   The target evaluation resets the range (mov dword [rcx+A],-1.0) and gets each weapon's range with
   mov rcx,rdi ; call RANGE ; mov rcx,[rsi+x] ; movaps xmm6,xmm0. */
static int find_priority(void)
{
    uint8_t *tick = (uint8_t *)g_vtVsRoot[3], *tagFn = NULL;
    for (uint8_t *q = tick; q < tick + 0x800 && !o_approach; q++)
        if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x10 && q[3] == 0x81 && q[8] == 0x41 && q[9] == 0x0F && q[10] == 0x2F)
            for (uint8_t *r = q + 11; r < q + 0x20; r++)
                if (r[0] == 0x49 && r[1] == 0x8B && r[2] == 0xD4 && r[3] == 0xE8) { o_approach = *(uint32_t *)(q + 4); tagFn = rel32_target(r + 3, 5); break; }
    if (!o_approach || !in_text(tagFn)) { logf_("priority: approach range read not found"); return 0; }
    /* T starts with mov rsi,[rdx+E] : the record's entity */
    for (uint8_t *q = tagFn; q < tagFn + 0x20 && !o_recEnt; q++) if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x72) o_recEnt = q[3];
    if (!o_recEnt) { logf_("priority: target record entity offset not found"); return 0; }
    for (uint8_t *q = tagFn; q < tagFn + 0x40 && !o_entDef; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x86 && q[7] == 0x48 && q[8] == 0x8B && q[9] == 0xB8 && q[14] == 0x48 && q[15] == 0x8B && q[16] == 0x98 &&
            *(uint32_t *)(q + 10) == *(uint32_t *)(q + 17) + 8) { o_entDef = *(uint32_t *)(q + 3); o_defPropBeg = *(uint32_t *)(q + 17); o_defPropEnd = *(uint32_t *)(q + 10); }
    if (!o_entDef) { logf_("priority: def props read not found"); return 0; }
    uint8_t reset[10] = { 0xC7, 0x81 }; memcpy(reset + 2, &o_approach, 4); reset[6] = 0; reset[7] = 0; reset[8] = 0x80; reset[9] = 0xBF;
    uint8_t *ev = find_bytes(g_text.p, g_text.n, reset, sizeof reset);
    if (!ev) { logf_("priority: target evaluation not found"); return 0; }
    /* (the order of 'mov rcx,[rsi+x]' and 'movaps xmm6,xmm0' differs between the exes); the range is then stored with
       movss [rsi+A],xmm6 within 0x80 bytes */
    for (uint8_t *q = ev; q < ev + 0x800 && !g_weaponRange; q++) {
        if (q[0] != 0x48 || q[1] != 0x8B || q[2] != 0xCF || q[3] != 0xE8) continue;
        int a = q[8] == 0x48 && q[9] == 0x8B && q[10] == 0x4E && q[12] == 0x0F && q[13] == 0x28 && q[14] == 0xF0;
        int b = q[8] == 0x0F && q[9] == 0x28 && q[10] == 0xF0 && q[11] == 0x48 && q[12] == 0x8B && q[13] == 0x4E;
        if (!a && !b) continue;
        uint8_t st[8] = { 0xF3, 0x0F, 0x11, 0xB6 }; memcpy(st + 4, &o_approach, 4);
        if (find_bytes(q, 0x80, st, 8)) g_weaponRange = (WeaponRangeFn)rel32_target(q + 3, 5);
    }
    if (!g_weaponRange || !in_text((void *)g_weaponRange)) { logf_("priority: weapon range getter not found"); return 0; }
    logf_("priority: approach range brain +0x%X, entity +0x%X -> def, props +0x%X..+0x%X, weapon range exe+0x%llX, record entity +0x%X",
          o_approach, o_entDef, o_defPropBeg, o_defPropEnd, EXE_OFF(g_weaponRange), o_recEnt);
    return 1;
}

/* the chassis' movement-mode setter, as Order::eMovementChange uses it (slot 19): mov edi,CHASSIS ... mov edx,[rbx+0x3c] ;
   ... call [rax+MODE] */
static int find_run_mode(void)
{
    void **vt = find_vtable_rtti(".?AUeMovementChange@Order@@");
    if (!vt || !in_text(vt[19])) { logf_("run mode: eMovementChange not found"); return 0; }
    uint8_t *f = (uint8_t *)vt[19];
    for (uint8_t *q = f; q < f + 0x30 && !o_chassis; q++)
        if (q[0] == 0xBF && *(uint32_t *)(q + 1) >= 0x800 && *(uint32_t *)(q + 1) < 0x2000) o_chassis = *(uint32_t *)(q + 1);
    uint8_t *m = find_bytes(f, 0x80, (const uint8_t *)"\x8B\x53\x3C", 3);
    if (m) for (uint8_t *q = m; q < m + 0x30 && !o_modeSlot; q++)
        if (q[0] == 0xFF && q[1] == 0x90 && *(uint32_t *)(q + 2) < 0x800 && !(*(uint32_t *)(q + 2) & 7)) o_modeSlot = *(uint32_t *)(q + 2);
    if (!o_chassis || !o_modeSlot) { logf_("run mode: chassis / mode setter not recognised"); return 0; }
    logf_("run mode: entity +0x%X -> chassis, SetMovementMode = chassis vtable +0x%X", o_chassis, o_modeSlot);
    return 1;
}

/* the target evaluation (function of the 'mov dword [rcx+A],-1.0' reset), its single caller (a tail jmp), and the
   Weapon::SetTarget calls inside it. SetTarget: cmp qword [rcx+OWNERITEM],0 near the start; cmp dword [rsi+8],0 ; jne ;
   mov edx,1 ; mov rcx,rbx ; call Clear. The owner entity: the weapon's 'mov r,[rcx+OWNER]' followed by
   'mov r,[r+WEAPONRY]' (the same weaponry offset the human finder found). */
static int find_hold(void)
{
    if (!o_approach || !o_weaponry) { logf_("hold: needs the priority and weapon finders"); return 0; }
    uint8_t reset[10] = { 0xC7, 0x81 }; memcpy(reset + 2, &o_approach, 4); reset[6] = 0; reset[7] = 0; reset[8] = 0x80; reset[9] = 0xBF;
    uint8_t *ev = find_bytes(g_text.p, g_text.n, reset, sizeof reset);
    uint32_t evRva = ev ? func_of(ev) : 0;
    if (!evRva) { logf_("hold: target evaluation not found"); return 0; }
    g_evalFn = g_base + evRva;
    int callers = 0;
    for (uint8_t *p = g_text.p; p + 5 < g_text.p + g_text.n; p++)
        if ((p[0] == 0xE8 || p[0] == 0xE9) && rel32_target(p, 5) == g_evalFn) { g_evalJump = p; callers++; }
    if (callers != 1) { logf_("hold: target evaluation has %d callers (expected 1)", callers); return 0; }
    /* SetTarget = the call target inside the evaluation that starts like Weapon::SetTarget */
    g_stSiteN = 0;
    for (uint8_t *p = g_evalFn; p < g_evalFn + 0x1400; p++) {
        if (p[0] != 0xE8 || func_of(p) != evRva) continue;
        uint8_t *t = rel32_target(p, 5);
        if (!in_text(t)) continue;
        if (!g_setTarget) {
            uint8_t *c = find_bytes(t, 0x30, (const uint8_t *)"\x48\x83\xB9", 3);
            uint8_t *z = NULL;                                          /* cmp dword [rDESC+8],0 ; jne (rsi or rbp) */
            for (uint8_t *q = t; q < t + 0x90 && !z; q++)
                if (q[0] == 0x83 && (q[1] & 0xF8) == 0x78 && q[2] == 0x08 && q[3] == 0x00 && q[4] == 0x75 && q[6] == 0xBA &&
                    *(uint32_t *)(q + 7) == 1 && q[11] == 0x48 && q[12] == 0x8B && q[13] == 0xCB && q[14] == 0xE8) z = q;
            if (!c || c[7] != 0x00 || !z) continue;
            g_setTarget = (SetTargetFn)t; g_clearTarget = (ClearTargetFn)rel32_target(z + 14, 5);
        }
        if (t == (uint8_t *)g_setTarget && g_stSiteN < MAX_ST_SITES) g_stSites[g_stSiteN++] = p;
    }
    if (!g_setTarget || !g_stSiteN) { logf_("hold: Weapon::SetTarget not found in the target evaluation"); return 0; }
    for (uint8_t *q = (uint8_t *)g_setTarget; q < (uint8_t *)g_setTarget + 0x60 && !o_weapOwner; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x81 && *(uint32_t *)(q + 3) >= 0x40 && *(uint32_t *)(q + 3) < 0x200 &&
            q[7] == 0x48 && q[8] == 0x8B && (q[9] & 0xC0) == 0x80 && *(uint32_t *)(q + 10) == 0x568) o_weapOwner = *(uint32_t *)(q + 3);
    if (!o_weapOwner) {
        /* fallback: any 'mov rax,[r+OWN] ; mov r,[rax+WEAPONRY]' in the exe (weapon code reading its owner's weaponry) */
        for (uint8_t *p = g_text.p; p + 14 < g_text.p + g_text.n && !o_weapOwner; p++)
            if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC7) == 0x81 && *(uint32_t *)(p + 3) >= 0x40 && *(uint32_t *)(p + 3) < 0x200 &&
                p[7] == 0x48 && p[8] == 0x8B && (p[9] & 0xC7) == 0x80 && *(uint32_t *)(p + 10) == o_weaponry) o_weapOwner = *(uint32_t *)(p + 3);
    }
    if (!o_weapOwner) { logf_("hold: weapon owner offset not found"); return 0; }
    logf_("hold: target evaluation exe+0x%llX (entered from exe+0x%llX), Weapon::SetTarget exe+0x%llX (%d site(s)), clear exe+0x%llX, weapon +0x%X -> owner",
          EXE_OFF(g_evalFn), EXE_OFF(g_evalJump), EXE_OFF(g_setTarget), g_stSiteN, EXE_OFF(g_clearTarget), o_weapOwner);
    return 1;
}

#ifndef FX_TEST
/* ------------------------------------------------------------------------------------ */
/* vehicles: approach orders follow a moving target                                     */
/* ------------------------------------------------------------------------------------ */
typedef struct { void *order; uint8_t *ent, *brain; float cx, cy, r, reach; uint32_t next; uint8_t used, started, repath; } Chase;
#define MAX_CHASE 512
static Chase g_ch[MAX_CHASE];
static int g_chHigh;                                                   /* slots [0, g_chHigh) may be used */
static volatile LONG g_chLive;                                         /* number of used slots (fast path when 0) */
static CRITICAL_SECTION g_cs;                                          /* g_ch and g_tr */
static volatile LONG g_regLogged, g_repLogged, g_stopLogged, g_pursueLogged;
static _Thread_local void *t_apOrder;
static _Thread_local float t_apX, t_apY, t_apR;
/* set by vs_tick for the unit whose tick is running, read by ap_move */
static _Thread_local int t_chase, t_haveV;
static _Thread_local float t_mel, t_vx, t_vy;

static int ch_find(void *o) { for (int i = 0; i < g_chHigh; i++) if (g_ch[i].used && g_ch[i].order == o) return i; return -1; }
static void ch_remove(int i) { g_ch[i].used = 0; InterlockedDecrement(&g_chLive); while (g_chHigh && !g_ch[g_chHigh - 1].used) g_chHigh--; }

/* ---- target velocity: per chasing unit, the last sample of its target record (game time) ---- */
typedef struct { uint8_t *ent; void *tgt; float x, y, vx, vy; uint32_t t; } Track;
#define MAX_TRACK 1024
static Track g_tr[MAX_TRACK];

/* caller holds g_cs. Velocity in world units per second, smoothed; 0 for a new target. Only floats are kept: the target
   pointer is compared, never dereferenced. */
static void track(uint8_t *e, uint8_t *rec, float *vx, float *vy)
{
    void *tgt = *(void **)(rec + o_recEnt);
    float x = *(float *)(rec + o_recX), y = *(float *)(rec + o_recY);
    uint32_t now = *g_simTime;
    size_t h = (size_t)(((uintptr_t)e >> 4) * 2654435761u) % MAX_TRACK;
    Track *t = NULL, *oldest = NULL;
    for (int k = 0; k < 16; k++) {
        Track *c = &g_tr[(h + k) % MAX_TRACK];
        if (c->ent == e || !c->ent) { t = c; break; }
        if (!oldest || (int32_t)(c->t - oldest->t) < 0) oldest = c;
    }
    if (!t) t = oldest;
    if (t->ent != e || t->tgt != tgt) { t->ent = e; t->tgt = tgt; t->x = x; t->y = y; t->vx = t->vy = 0; t->t = now; }
    else {
        uint32_t dt = now - t->t;
        if (dt >= 100) {
            if (dt <= 3000) {
                float ix = (x - t->x) * 1000.0f / (float)dt, iy = (y - t->y) * 1000.0f / (float)dt;
                t->vx = 0.5f * t->vx + 0.5f * ix; t->vy = 0.5f * t->vy + 0.5f * iy;
            } else t->vx = t->vy = 0;
            t->x = x; t->y = y; t->t = now;
        }
    }
    *vx = t->vx; *vy = t->vy;
}

static float dist2d(float ax, float ay, float bx, float by) { float dx = ax - bx, dy = ay - by; return __builtin_sqrtf(dx * dx + dy * dy); }

/* where to aim: the target's position plus lead_seconds of its velocity (at most lead_max_m) */
static void lead_point(float tx, float ty, float vx, float vy, float *px, float *py)
{
    float lx = vx * g_leadSec, ly = vy * g_leadSec, l = __builtin_sqrtf(lx * lx + ly * ly);
    if (l > g_leadMax && l > 0) { lx *= g_leadMax / l; ly *= g_leadMax / l; }
    *px = tx + lx; *py = ty + ly;
}

/* speed (world units/s) at which the target moves away from (x,y); negative = coming closer */
static float receding(float x, float y, float tx, float ty, float vx, float vy)
{
    float dx = tx - x, dy = ty - y, d = __builtin_sqrtf(dx * dx + dy * dy);
    return d > 0 ? (dx * vx + dy * vy) / d : 0;
}

/* melee reach (largest range among melee-tagged weapons) and ranged reach of a unit; -1 = has melee, range unknown */
static float unit_reach(uint8_t *e, float *ranged)
{
    *ranged = 0;
    uint8_t *wy = *(uint8_t **)(e + o_weaponry);
    if (!wy) return 0;
    uint8_t **wb = *(uint8_t ***)(wy + o_wlistBeg), **we = *(uint8_t ***)(wy + o_wlistEnd);
    if (!wb || we < wb || we - wb > 64) return 0;
    float mel = 0;
    for (; wb < we; wb++) {
        if (!*wb) continue;
        int m = weapon_is_melee(*wb);
        float r = g_weaponRange ? g_weaponRange(*wb) : -1.0f;
        if (r > 5000.0f * WORLD_SCALE) r = 0;
        if (m) { if (r > mel || (r < 0 && mel == 0)) mel = r; }
        else if (r > *ranged) *ranged = r;
    }
    return mel;
}

static void *ap_move(void *circle, uint64_t flags)
{
    float *c = (float *)((uint8_t *)circle + 8);                       /* x, y, radius */
    if (t_chase && t_mel > 0 && c[2] <= t_mel + WORLD_SCALE) {         /* an approach to melee reach */
        float r = t_mel * g_contact;
        if (r < 0.5f * WORLD_SCALE) r = 0.5f * WORLD_SCALE;
        c[2] = r;                                                       /* drive into contact, not to the edge of reach */
        if (t_haveV) lead_point(c[0], c[1], t_vx, t_vy, &c[0], &c[1]);  /* and to where the target is going */
    }
    void *o = g_moveCircleOrig(circle, flags);
    t_apOrder = o; t_apX = c[0]; t_apY = c[1]; t_apR = c[2];
    return o;
}

/* ai_prioritize_melee / ai_prioritize_ranged: the approach range the tick is about to use becomes the melee reach
   (charge in) or the ranged reach (stand off and shoot); inside that reach -1 = no approach.
   Returns 0 = no prop, 1 = ranged priority, 2 = melee priority. */
static volatile LONG g_prioLogged;
static int apply_priority(uint8_t *brain, uint8_t *e, uint8_t *rec, float mel, float rng)
{
    int pm = def_has_prop(e, g_propMelee, &g_idPropMelee), pr = !pm && def_has_prop(e, g_propRanged, &g_idPropRanged);
    int ret = pm ? 2 : pr ? 1 : 0;
    if (!ret || !rec) return ret;
    float reach = pm ? mel : rng;
    if (!(reach > 0.0f)) return ret;                                    /* no weapon of that kind: vanilla */
    float *p = (float *)(e + ENT_POS), *t = (float *)(rec + o_recX);
    float dx = p[0] - t[0], dy = p[1] - t[1], dz = p[2] - t[2];
    float dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    float want = dist >= reach ? reach : -1.0f, *cur = (float *)(brain + o_approach);
    if (*cur != want) {
        if (g_logChase && InterlockedIncrement(&g_prioLogged) <= 20)
            logf_("%s unit %p: target %.1f m, approach range %.1f -> %s", pm ? "melee-priority" : "ranged-priority", (void *)e,
                  dist / WORLD_SCALE, *cur < 0 ? -1.0f : *cur / WORLD_SCALE, want < 0 ? "hold (in reach)" : pm ? "melee reach" : "ranged reach");
        *cur = want;
    }
    return ret;
}

/* diagnostic: a vehicle-brain unit approached but none of its weapons has the melee tag - list what tags they do have */
static volatile LONG g_noMeleeLogged;
static void log_no_melee(uint8_t *e)
{
    if (!g_logChase || !e || InterlockedIncrement(&g_noMeleeLogged) > 15) return;
    char buf[700]; size_t n = 0; buf[0] = 0;
    uint8_t *wy = *(uint8_t **)(e + o_weaponry);
    uint8_t **wb = wy ? *(uint8_t ***)(wy + o_wlistBeg) : NULL, **we = wy ? *(uint8_t ***)(wy + o_wlistEnd) : NULL;
    int nw = 0;
    for (; wb && wb < we && we - wb <= 64 && n < sizeof buf - 80; wb++, nw++) {
        uint8_t *item = *wb ? *(uint8_t **)(*wb + o_weapItem) : NULL;
        n += snprintf(buf + n, sizeof buf - n, " [w%d:", nw);
        if (item) {
            uint8_t *tb = *(uint8_t **)(item + o_itemTagBeg), *te = *(uint8_t **)(item + o_itemTagEnd);
            for (uint8_t *t = tb; t && t + TAG_SIZE <= te && te - tb <= TAG_SIZE * 64 && n < sizeof buf - 40; t += TAG_SIZE)
                n += snprintf(buf + n, sizeof buf - n, " %.24s", ms_cstr((const MsString *)(t + TAG_NAME)));
        }
        n += snprintf(buf + n, sizeof buf - n, "]");
    }
    logf_("unit %p approaches but has no weapon tagged \"%s\" - not chased. Weapon tags:%s", (void *)e, g_meleeTag, nw ? buf : " (no weapons)");
}

static uint64_t vs_tick(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    t_chase = 0; t_haveV = 0; t_mel = 0;
    uint8_t *brain = *(uint8_t **)((uint8_t *)s + o_vStateBrain);
    uint8_t *e = brain ? *(uint8_t **)(brain + o_vBrainEntity) : NULL;
    if (e && !(e[ENT_FLAGS] & 4) && (e[ENT_FLAGS] & 2)) {
        float rng, mel = unit_reach(e, &rng);
        uint8_t *rec = NULL;
        if (*(int32_t *)(brain + o_vMode) == 1) {
            uint8_t **rb = *(uint8_t ***)(brain + o_vRecBeg), **re = *(uint8_t ***)(brain + o_vRecEnd);
            if (rb && rb < re) rec = *rb;
        }
        int prio = g_okPriority ? apply_priority(brain, e, rec, mel, rng) : 0;
        if (mel != 0) {
            t_chase = 1; t_mel = mel;
            if (rec && g_okClose) {
                float vx, vy;
                EnterCriticalSection(&g_cs); track(e, rec, &vx, &vy); LeaveCriticalSection(&g_cs);
                t_vx = vx; t_vy = vy; t_haveV = 1;
                /* the vanilla brain approaches only when the target is outside weapon range, and stands still inside it.
                   A melee unit (melee-only, or melee priority) instead chases whenever the target is outside 90% of its
                   reach (chase_always), and always when the target backs off (pursue_retreating). */
                float *cur = (float *)(brain + o_approach), *p = (float *)(e + ENT_POS);
                float tx = *(float *)(rec + o_recX), ty = *(float *)(rec + o_recY);
                int meleeUnit = prio == 2 || (prio == 0 && !(rng > 0));
                if (prio != 1 && mel > 0 && *cur < 0) {
                    float d = dist2d(p[0], p[1], tx, ty), back = receding(p[0], p[1], tx, ty, vx, vy);
                    int always = g_chaseAlways && meleeUnit && d > mel * 0.9f;
                    int retreat = g_pursue && d > mel * g_contact && back > g_recede;
                    if (always || retreat) {
                        *cur = mel;
                        if (g_logChase && InterlockedIncrement(&g_pursueLogged) <= 20)
                            logf_("melee unit %p: target %.1f m (%s %.1f m/s) inside weapon range - chasing", (void *)e, d / WORLD_SCALE,
                                  back >= 0 ? "moving away at" : "closing at", (back >= 0 ? back : -back) / WORLD_SCALE);
                    }
                }
            }
        }
    }

    t_apOrder = NULL;
    uint64_t r = g_vsTickOrig(s, a, b, c);
    void *o = t_apOrder; t_apOrder = NULL;
    if (!o || !brain || *(void **)(brain + o_aiOrder) != o) return r;  /* no approach, or not (any more) the AI order */
    if (!t_chase) { log_no_melee(e); return r; }
    if (!(t_apR > 0.0f) || t_apR > 2000.0f * WORLD_SCALE) return r;
    float *p = (float *)(e + ENT_POS);
    float self = dist2d(p[0], p[1], t_apX, t_apY);
    EnterCriticalSection(&g_cs);
    int i = ch_find(o);
    if (i < 0) {
        i = 0;
        while (i < MAX_CHASE && g_ch[i].used) i++;
        if (i < MAX_CHASE) { if (i >= g_chHigh) g_chHigh = i + 1; InterlockedIncrement(&g_chLive); }
    }
    if (i >= 0 && i < MAX_CHASE) {
        Chase ch = { o, e, brain, t_apX, t_apY, t_apR, t_mel > 0 ? t_mel : t_apR,
                     *g_simTime + (self < g_closeRange ? g_repathMsClose : g_repathMs), 1, 0, 0 };
        g_ch[i] = ch;
    }
    LeaveCriticalSection(&g_cs);
    /* chase at the chosen pace (the vanilla vehicle AI moves at whatever mode the chassis has) */
    if (o_modeSlot && g_chaseMode >= 0) {
        uint8_t *chs = *(uint8_t **)(e + o_chassis);
        if (chs && *(void ***)chs) {
            typedef void (*SetModeFn)(void *chassis, int mode, int flag);
            ((SetModeFn)(*(void ***)chs)[o_modeSlot / 8])(chs, g_chaseMode, 0);
        }
    }
    if (g_logChase && InterlockedIncrement(&g_regLogged) <= 20)
        logf_("melee unit %p approaches (%.1f m away, goal radius %.1f m) - following the target", (void *)e, self / WORLD_SCALE, t_apR / WORLD_SCALE);
    return r;
}

static uint64_t mt_update(void *o, uint64_t phase, uint64_t a, uint64_t b)
{
    if (!g_chLive) return g_mtUpdateOrig(o, phase, a, b);
    EnterCriticalSection(&g_cs);
    int i = ch_find(o);
    if (i < 0) { LeaveCriticalSection(&g_cs); return g_mtUpdateOrig(o, phase, a, b); }
    if ((uint32_t)phase == 0) {                                           /* start (a reused pointer that started before is stale) */
        if (g_ch[i].started) ch_remove(i); else g_ch[i].started = 1;
        LeaveCriticalSection(&g_cs);
        return g_mtUpdateOrig(o, phase, a, b);
    }
    if ((uint32_t)phase != 1) {                                           /* stop */
        int smooth = g_ch[i].repath && g_smooth && g_finishSub;
        /* the move reached its goal but the target is backing off: the next approach follows at once, so don't brake */
        if (!smooth && g_smooth && g_finishSub && g_okClose && *(int32_t *)(g_ch[i].brain + o_vMode) == 1) {
            uint8_t **rb = *(uint8_t ***)(g_ch[i].brain + o_vRecBeg), **re = *(uint8_t ***)(g_ch[i].brain + o_vRecEnd);
            if (rb && rb < re && *rb) {
                float vx, vy, *p = (float *)(g_ch[i].ent + ENT_POS);
                track(g_ch[i].ent, *rb, &vx, &vy);
                smooth = g_pursue && receding(p[0], p[1], *(float *)(*rb + o_recX), *(float *)(*rb + o_recY), vx, vy) > g_recede;
            }
        }
        ch_remove(i);
        LeaveCriticalSection(&g_cs);
        if (smooth) { g_finishSub(o); return 1; }                         /* drop the path, keep the chassis rolling */
        return g_mtUpdateOrig(o, phase, a, b);
    }
    uint32_t now = *g_simTime;
    if ((int32_t)(now - g_ch[i].next) < 0) { LeaveCriticalSection(&g_cs); return g_mtUpdateOrig(o, phase, a, b); }
    Chase ch = g_ch[i];
    uint8_t *brain = ch.brain, *rec = NULL;
    if (*(int32_t *)(brain + o_vMode) == 1) {
        uint8_t **rb = *(uint8_t ***)(brain + o_vRecBeg), **re = *(uint8_t ***)(brain + o_vRecEnd);
        if (rb && rb < re) rec = *rb;
    }
    float vx = 0, vy = 0;
    if (rec && g_okClose) track(ch.ent, rec, &vx, &vy);
    float *pos = (float *)(ch.ent + ENT_POS);
    float tx = rec ? *(float *)(rec + o_recX) : 0, ty = rec ? *(float *)(rec + o_recY) : 0;
    float self = rec ? dist2d(pos[0], pos[1], tx, ty) : 0;
    g_ch[i].next = now + (self < g_closeRange ? g_repathMsClose : g_repathMs);
    LeaveCriticalSection(&g_cs);
    if (!rec) return g_mtUpdateOrig(o, phase, a, b);

    float px, py; lead_point(tx, ty, vx, vy, &px, &py);
    float moved = dist2d(px, py, ch.cx, ch.cy), back = receding(pos[0], pos[1], tx, ty, vx, vy);
    /* in reach and the target is not getting away: stop and fight */
    if (self <= ch.reach * 0.9f && back <= g_recede) {
        if (g_logChase && InterlockedIncrement(&g_stopLogged) <= 20)
            logf_("melee unit %p: target in reach (%.1f m) - stopping", (void *)ch.ent, self / WORLD_SCALE);
        return 1;
    }
    /* the goal moved: re-plan. The threshold shrinks near the target (fraction of the distance, min..max) */
    float thr = self * g_repathFrac;
    if (thr < g_repathMin) thr = g_repathMin; if (thr > g_repathMax) thr = g_repathMax;
    if (moved > thr) {
        EnterCriticalSection(&g_cs);
        int j = ch_find(o); if (j >= 0) g_ch[j].repath = 1;
        LeaveCriticalSection(&g_cs);
        if (g_logChase && InterlockedIncrement(&g_repLogged) <= 30)
            logf_("melee unit %p: target goal moved %.1f m - new path (%.1f m away, %s %.1f m/s)", (void *)ch.ent, moved / WORLD_SCALE,
                  self / WORLD_SCALE, back >= 0 ? "moving away at" : "closing at", (back >= 0 ? back : -back) / WORLD_SCALE);
        return 1;
    }
    return g_mtUpdateOrig(o, phase, a, b);
}

/* an approach order can be deleted without ever running its stop step (replaced before it started): forget it here,
   so a later order allocated at the same address is never mistaken for it */
static void *mt_dtor(void *o, uint64_t flags)
{
    if (g_chLive) {
        EnterCriticalSection(&g_cs);
        int i = ch_find(o); if (i >= 0) ch_remove(i);
        LeaveCriticalSection(&g_cs);
    }
    return g_mtDtorOrig(o, flags);
}

/* ---- hold ranged fire while the target is inside melee reach ---- */
static _Thread_local uint8_t *t_evalBrain;
static EvalFn g_evalOrig;
static SetTargetFn g_setTargetOrig;
static volatile LONG g_holdLogged;

static uint64_t eval_wrap(void *brain, uint64_t a, uint64_t b, uint64_t c)
{
    uint8_t *prev = t_evalBrain;
    t_evalBrain = (uint8_t *)brain;
    uint64_t r = g_evalOrig(brain, a, b, c);
    t_evalBrain = prev;
    return r;
}

static uint64_t st_wrap(void *w, void *desc, uint64_t a, uint64_t mode, uint64_t b)
{
    uint8_t *brain = t_evalBrain;
    if (brain && w && desc && *(int32_t *)((uint8_t *)desc + 8) != 0 && !weapon_is_melee((uint8_t *)w)) {
        uint8_t *e = *(uint8_t **)(brain + o_vBrainEntity);
        if (e && e == *(uint8_t **)((uint8_t *)w + o_weapOwner) && !(e[ENT_FLAGS] & 4) && (e[ENT_FLAGS] & 2) &&
            *(int32_t *)(brain + o_vMode) == 1) {
            uint8_t **rb = *(uint8_t ***)(brain + o_vRecBeg), **re = *(uint8_t ***)(brain + o_vRecEnd);
            float rng, mel = (rb && rb < re && *rb) ? unit_reach(e, &rng) : 0;
            if (mel > 0 && !(g_okPriority && def_has_prop(e, g_propRanged, &g_idPropRanged) && !def_has_prop(e, g_propMelee, &g_idPropMelee))) {
                float *p = (float *)(e + ENT_POS), *t = (float *)(*rb + o_recX);
                if (dist2d(p[0], p[1], t[0], t[1]) <= mel * g_holdFactor) {
                    if (g_logChase && InterlockedIncrement(&g_holdLogged) <= 10)
                        logf_("melee unit %p: target in melee reach - ranged weapon holds fire", (void *)e);
                    return g_clearTarget(w, 1);
                }
            }
        }
    }
    return g_setTargetOrig(w, desc, a, mode, b);
}

static int install_hold(void)
{
    if (!redirect_call(g_evalJump, (void *)eval_wrap, (void **)&g_evalOrig, "target evaluation")) return 0;
    for (int i = 0; i < g_stSiteN; i++)
        if (!redirect_call(g_stSites[i], (void *)st_wrap, (void **)&g_setTargetOrig, "weapon target")) return i > 0;
    return 1;
}

static int install_vehicle(void)
{
    InitializeCriticalSection(&g_cs);
    if (!vt_hook(g_vtMoveTask, 0, (void *)mt_dtor, (void **)&g_mtDtorOrig)) { logf_("vehicle: eMoveTask dtor not hooked"); return 0; }
    if (!vt_hook(g_vtMoveTask, 19, (void *)mt_update, (void **)&g_mtUpdateOrig)) { logf_("vehicle: eMoveTask Update not hooked"); return 0; }
    if (!redirect_call(g_apSite, (void *)ap_move, (void **)&g_moveCircleOrig, "approach move order")) return 0;
    if (!vt_hook(g_vtVsRoot, 3, (void *)vs_tick, (void **)&g_vsTickOrig)) { logf_("vehicle: eVS_Root tick not hooked"); return 0; }
    return 1;
}
#endif /* FX_TEST */
