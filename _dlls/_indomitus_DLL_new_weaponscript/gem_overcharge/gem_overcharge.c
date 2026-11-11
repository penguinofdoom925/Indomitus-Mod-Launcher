/*
 * GEM Overcharge 1.1  -  Call to Arms: Gates of Hell (GEM engine)  -  module of gem_weaponscript.dll (Indomitus Launcher payload)
 * Ammo swap by unit script tag (plasma overcharge). Split out of the all-in-one GEM Converge 0.5.1.
 *
 * While the firing unit (or, for a vehicle gun, the vehicle its gunner is linked to) carries the script tag
 * "overcharge" (ini OverchargeTag), its weapon's loaded magazine is the alternate ammo whose set-file NAME tags are
 * the loaded ammo's plus "overcharge" (ini OverchargeSuffix): plasmagun_mag.ammo -> plasmagun_mag.overcharge.ammo.
 * The swap is PERSISTENT while tagged (the impact code and the aiming display ask the weapon for its current ammo,
 * so a swap that only lived inside the shot gave overcharge speed/tracer but standard penetration/damage); the first
 * shot after the tag is gone puts the standard magazine back. Everything keyed by the ammo follows: weapon
 * {parameters "<x>"} blocks (picked per shot by the loaded ammo's name/{tag}s), bullet stuff, tracer, hit fx.
 * An overcharge shot costs one magazine round, like a standard shot (the engine's own consume step takes nothing
 * while the sibling is loaded, so the dll takes it).
 *
 * Hooks (chainable, see common\gem_common.h): eBulletShooter::Launch (the swap + round cost), SpawnBullet (shared
 * with GEM Converge / GEM Guidance; makes sure the right magazine is in when a bullet is created).
 */
#define GEM_NAME    "GEM Overcharge"
#define GEM_VERSION "1.1.1"
#define GEM_SECT    L"overcharge"
#include "../common/gem_common.h"

/* ---- bindings ------------------------------------------------------------------------- */
/* eStuff registry (eStuffs.cpp): a global {eStuff* begin, end} vector walked by the tag lookup that logs
   "No Stuff found for tags"; each eStuff keeps its set-file NAME tags ("ammo plasmagun_mag overcharge") in
   a Tags object whose vector sits at stuff+0xB80/0xB88 (the {tag} tags are the other Tags object, vector at
   +0x11E0). The weapon's loaded magazine is the weapon ITEM's filling: item+0x50 = ammo eStuff*, +0x58 =
   rounds (eInventoryItem::Fill); eAmmo (weapon+0xB0) reaches the item through weapon vtable slot 3. */
static uint8_t *g_stuffReg;           /* {begin, end} of eStuff* */
static uint32_t g_nameTagsOff;        /* eStuff: name-tag vector begin (end = +8) */
static uint32_t g_weaponItemSlot;     /* weapon vtable byte offset of GetItem() */
static uint32_t g_fillStuffOff;       /* item: filling stuff pointer */
static uint32_t g_weaponAmmoOff;      /* weapon: eAmmo* (logged only) */
static uint32_t g_entTagsOff;         /* entity: script tag container */

#define TC_SELF_A   0x308   /* TagContainer: owner back-pointers (== container itself) */
#define TC_SELF_B   0x310
#define TC_VEC_BEG  0x318
#define TC_VEC_END  0x320

static int find_stuff_registry(void)
{
    uint8_t *s = find_cstr("No Stuff found for tags '%1%' (may lead to crash)");
    if (!s) { logf_("stuff registry: error string not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, 0x15, 0);
    if (!lea) { logf_("stuff registry: string xref not found"); return 0; }
    uint8_t *fn = NULL;
    g_nameTagsOff = 0;
    /* the lookup is a call shortly before the error: its body does  lea rax,[rip+registry] ; mov [rsp+20h],rax */
    for (uint8_t *p = lea; p > lea - 0x100 && !fn; p--) {
        if (p[0] != 0xE8) continue;
        uint8_t *t = p + 5 + *(int32_t *)(p + 1);
        if (!in_text(t)) continue;
        for (uint8_t *q = t; q < t + 0x120; q++) {
            if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x05) continue;
            uint8_t *g = q + 7 + *(int32_t *)(q + 3);
            if (!in_data(g)) continue;
            if (find_bytes(q + 7, 0x40, (const uint8_t *)"\x48\x89\x44\x24\x20", 5)) { fn = t; g_stuffReg = g; break; }
        }
    }
    if (!fn) { logf_("stuff registry: lookup function not recognised"); return 0; }
    /* name tags: the loop reads  mov r,[stuff+d] ; mov r,[stuff+d+8]  (vector begin/end), d > 0x400 */
    uint32_t disps[96]; int nd = 0;
    for (uint8_t *q = fn; q < fn + 0x300 && nd < 96; q++) {
        if ((q[0] != 0x48 && q[0] != 0x49 && q[0] != 0x4C && q[0] != 0x4D) || q[1] != 0x8B || (q[2] & 0xC0) != 0x80 || (q[2] & 7) == 4) continue;
        uint32_t d = *(uint32_t *)(q + 3);
        if (d >= 0x400 && d <= 0x1000) disps[nd++] = d;
    }
    for (int i = 0; i < nd && !g_nameTagsOff; i++)
        for (int j = 0; j < nd; j++)
            if (disps[j] == disps[i] + 8) { g_nameTagsOff = disps[i]; break; }
    if (!g_nameTagsOff) { logf_("stuff registry: name tag vector offset not found"); return 0; }
    logf_("stuff registry @ exe+0x%llX (lookup exe+0x%llX), name tags @ stuff+0x%X", (unsigned long long)(g_stuffReg - g_base), (unsigned long long)(fn - g_base), g_nameTagsOff);
    return 1;
}

/* eAmmo vtable slot 1 = GetItem thunk:  mov rcx,[rcx+8] (weapon) ; mov rax,[rcx] ; jmp [rax+slot] */
static int find_weapon_item_slot(void)
{
    uint8_t *vt = find_vtable(".?AVeAmmo@Weapon@Actor@@");
    if (!vt) { logf_("eAmmo: vftable not found"); return 0; }
    uint8_t *f = vt_slot_orig(vt, 1, "eAmmo");
    if (!in_text(f)) { logf_("eAmmo: slot 1 not in .text"); return 0; }
    uint8_t *ld = find_bytes(f, 0x10, (const uint8_t *)"\x48\x8B\x49\x08", 4);
    uint8_t *jm = find_bytes(f, 0x14, (const uint8_t *)"\xFF\x60", 2);
    if (!ld || !jm || jm < ld || jm[2] < 0x08 || jm[2] > 0x80) { logf_("eAmmo: GetItem thunk not recognised"); return 0; }
    g_weaponItemSlot = jm[2];
    logf_("eAmmo::GetItem -> weapon vtable +0x%X", g_weaponItemSlot);
    return 1;
}

/* SpawnBullet:  mov rcx,[weapon+ammo] ; call eAmmo::GetStuff ;  GetStuff = two  call [rax+8]  then  mov rax,[item+d8] */
static int find_filling_offs(void)
{
    uint8_t *f = (uint8_t *)g_spawnBullet;
    for (uint8_t *p = hk_body(f); p < f + 0x400; p++) {
        if ((p[0] != 0x48 && p[0] != 0x49) || p[1] != 0x8B || (p[2] & 0xF8) != 0x88 || (p[2] & 7) == 4 || p[7] != 0xE8) continue;
        uint32_t d = *(uint32_t *)(p + 3);
        if (d < 0x40 || d > 0x200) continue;
        uint8_t *t = p + 12 + *(int32_t *)(p + 8);
        if (!in_text(t)) continue;
        uint8_t *c1 = find_bytes(t, 0x30, (const uint8_t *)"\xFF\x50\x08", 3);
        if (!c1) continue;
        uint8_t *c2 = find_bytes(c1 + 3, 0x20, (const uint8_t *)"\xFF\x50\x08", 3);
        if (!c2) continue;
        uint8_t *ld = find_bytes(c2 + 3, 0x10, (const uint8_t *)"\x48\x8B\x40", 3);
        if (!ld || ld[3] < 0x20 || ld[3] > 0xF0) continue;
        g_weaponAmmoOff = d; g_fillStuffOff = ld[3];
        logf_("weapon->ammo +0x%X, item filling stuff @ item+0x%X (rounds +0x%X)", d, g_fillStuffOff, g_fillStuffOff + 8);
        return 1;
    }
    logf_("filling: eAmmo::GetStuff not found in SpawnBullet");
    return 0;
}

/* entity script tags (same finder as GEM NoProne): action table { "tags", ctor } -> eval: add rdx,imm32 */
static int find_entity_tags_off(void)
{
    uint8_t *ctor = NULL; int hits = 0;
    g_entTagsOff = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "tags", 5) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) { logf_("entity tags: action table entry %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *vt = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) { vt = p + 7 + *(int32_t *)(p + 3); break; }
    if (!vt || !in_rdata(vt)) { logf_("entity tags: vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8);
    if (!in_text(ev)) { logf_("entity tags: eval not in .text"); return 0; }
    for (uint8_t *q = ev; q < ev + 0x100; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) { g_entTagsOff = *(uint32_t *)(q + 3); break; }
    if (g_entTagsOff < 0x100 || g_entTagsOff > 0x400) { logf_("entity tags: container offset not found"); g_entTagsOff = 0; return 0; }
    logf_("entity {tags} @ entity+0x%X", g_entTagsOff);
    return 1;
}

/* {if linked} condition eval (eCondition_Linked vtable slot 1): bit 28 of [entity+0x1C0] = linked, [entity+0x560] -> link,
   [link+8] = the entity it is linked to (a crewman's vehicle). Vehicle guns are owned by their gunner, so an overcharge
   tag on the vehicle is found through this link. */
static uint32_t g_entLinkFlagOff, g_entLinkOff, g_linkEntOff;
static int find_linked(void)
{
    uint8_t *vt = find_vtable(".?AUeCondition_Linked@Interaction@@");
    if (!vt) { logf_("linked: vftable not found"); return 0; }
    uint8_t *f = vt_slot_orig(vt, 1, "linked");
    if (!in_text(f)) { logf_("linked: eval not in .text"); return 0; }
    /* mov eax,[ent+d32] ([41] 8B modrm mod=10 reg=eax) ; shr eax,1Ch (C1 E8 1C) ; ... mov rax,[ent+d32] (48/49 8B modrm) ; mov rdx,[rax+d8] (48 8B 50 xx)
       (game: ent in rcx; editor: ent in r9, after an assert) */
    uint8_t *p = NULL, *q = NULL;
    for (uint8_t *s = f; s < f + 0x80 && !p; s++) {
        int rex = s[0] == 0x41;
        const uint8_t *t = s + rex;
        if (t[0] != 0x8B || (t[1] & 0xF8) != 0x80 || (t[1] & 7) == 4) continue;
        if (!memcmp(t + 6, "\xC1\xE8\x1C", 3)) { p = s; g_entLinkFlagOff = *(uint32_t *)(t + 2); }
    }
    if (!p) { logf_("linked: flag test not recognised"); return 0; }
    for (uint8_t *s = p + 9; s < p + 0x80 && !q; s++) {
        if ((s[0] != 0x48 && s[0] != 0x49) || s[1] != 0x8B || (s[2] & 0xC0) != 0x80 || (s[2] & 7) == 4) continue;   /* mov r64,[ent+d32] (game rax<-rcx, editor rcx<-rax) */
        if (s[7] == 0x48 && s[8] == 0x8B && (s[9] & 0xC0) == 0x40 && (s[9] & 7) != 4 && s[10] <= 0x20) { q = s; g_entLinkOff = *(uint32_t *)(s + 3); g_linkEntOff = s[10]; }   /* then mov r64,[link+d8] */
    }
    if (!q) { logf_("linked: link load not recognised"); return 0; }
    if (g_entLinkFlagOff < 0x100 || g_entLinkFlagOff > 0x400 || g_entLinkOff < 0x400 || g_entLinkOff > 0x1000) { logf_("linked: offsets out of range"); return 0; }
    logf_("linked: flag [entity+0x%X] bit 28, link [entity+0x%X] -> entity +0x%X", g_entLinkFlagOff, g_entLinkOff, g_linkEntOff);
    return 1;
}

/* ---- overcharge ----------------------------------------------------------------------- */
typedef char  (*LaunchFn)(void *shooter);
typedef void *(*WeaponItemFn)(void *weapon);
static LaunchFn g_launchOrig;
static SpawnBulletFn g_spawnBulletOrig;
static char g_ocTag[64] = "overcharge", g_ocSuffix[64] = "overcharge";
static size_t g_ocTagLen, g_ocSuffixLen;
static int g_ocLog = 10;
static int g_ocSpawnLog = 8;
static int g_ocSpawnCalls;            /* SpawnBullet calls seen (hook_launch reports how many happened inside Launch) */

#define OC_MAXTAGS 16
#define OC_TAGLEN  48

/* entity script tag container (NoProne layout): self back-pointers at +0x308/+0x310, Tag vector +0x318/+0x320 */
static int ent_has_tag(const uint8_t *c, const char *tag, size_t len)
{
    const uint8_t *a = NULL, *b = NULL, *vb = NULL, *ve = NULL;
    if (!rd_mem(c + TC_SELF_A, &a, 8) || !rd_mem(c + TC_SELF_B, &b, 8) || a != c || b != c) return 0;
    if (!rd_mem(c + TC_VEC_BEG, &vb, 8) || !rd_mem(c + TC_VEC_END, &ve, 8) || !vb || ve < vb || (size_t)(ve - vb) > TAG_SIZE * 256) return 0;
    char s[OC_TAGLEN];
    for (const uint8_t *t = vb; t < ve; t += TAG_SIZE)
        if (rd_msstr(t + TAG_STR, s, sizeof s) && strlen(s) == len && !_strnicmp(s, tag, len)) return 1;
    return 0;
}

/* set-file name tags of an eStuff, lowercase strings; -1 when unreadable */
static int read_name_tags(const uint8_t *stuff, char out[][OC_TAGLEN], int max)
{
    const uint8_t *vb = NULL, *ve = NULL;
    if (!rd_mem(stuff + g_nameTagsOff, &vb, 8) || !rd_mem(stuff + g_nameTagsOff + 8, &ve, 8)) return -1;
    if (!vb || ve < vb || (size_t)(ve - vb) > TAG_SIZE * 64 || (ve - vb) % TAG_SIZE) return -1;
    int n = 0;
    for (const uint8_t *t = vb; t < ve && n < max; t += TAG_SIZE, n++)
        if (!rd_msstr(t + TAG_STR, out[n], OC_TAGLEN)) return -1;
    return n;
}

static void join_tags(char out[][OC_TAGLEN], int n, char *dst, size_t cap)
{
    dst[0] = 0;
    for (int i = 0; i < n; i++) { if (i) strncat(dst, " ", cap - strlen(dst) - 1); strncat(dst, out[i], cap - strlen(dst) - 1); }
}

/* base stuff s <-> overcharge sibling t (NULL = none); the name strings guard against a stale pointer after a map reload
   (every eStuff is recreated, an old address can come back as a different stuff) */
typedef struct { const void *s, *t; char sname[96], tname[96]; } OcCache;
#define OC_CACHE 64
static OcCache g_ocCache[OC_CACHE];
static int g_ocCacheN, g_ocCacheNext;

static int oc_name(const uint8_t *X, char *out, size_t cap)
{
    char xt[OC_MAXTAGS][OC_TAGLEN];
    int m = read_name_tags(X, xt, OC_MAXTAGS);
    if (m <= 0) { out[0] = 0; return 0; }
    join_tags(xt, m, out, cap);
    return m;
}

static void oc_cache_put(const void *s, const void *t, const char *sname, const char *tname)
{
    int i = g_ocCacheN < OC_CACHE ? g_ocCacheN++ : (g_ocCacheNext++ % OC_CACHE);
    g_ocCache[i].s = s; g_ocCache[i].t = t;
    strncpy(g_ocCache[i].sname, sname ? sname : "", sizeof g_ocCache[i].sname - 1); g_ocCache[i].sname[sizeof g_ocCache[i].sname - 1] = 0;
    strncpy(g_ocCache[i].tname, tname ? tname : "", sizeof g_ocCache[i].tname - 1); g_ocCache[i].tname[sizeof g_ocCache[i].tname - 1] = 0;
}

static int oc_tags_have_suffix(char xt[][OC_TAGLEN], int m)
{
    for (int j = 0; j < m; j++) if (strlen(xt[j]) == g_ocSuffixLen && !_strnicmp(xt[j], g_ocSuffix, g_ocSuffixLen)) return 1;
    return 0;
}

/* loaded ammo stuff -> its overcharge sibling (name tags = S's + suffix), cached; NULL = none */
static const uint8_t *oc_lookup(const uint8_t *S)
{
    char now[96];
    for (int i = 0; i < g_ocCacheN; i++) if (g_ocCache[i].s == S) {
        if (oc_name(S, now, sizeof now) && !strcmp(now, g_ocCache[i].sname)) return (const uint8_t *)g_ocCache[i].t;
        g_ocCache[i] = g_ocCache[--g_ocCacheN]; break;      /* stale: forget it and look again */
    }
    const uint8_t *T = NULL;
    char st[OC_MAXTAGS][OC_TAGLEN], xt[OC_MAXTAGS][OC_TAGLEN], sname[256] = "?", tname[256] = "";
    char cls[96];
    int n = read_name_tags(S, st, OC_MAXTAGS);
    const void *svt = NULL;
    if (n > 0 && n < OC_MAXTAGS - 1 && rd_mem(S, &svt, 8) && rtti_name(S, cls, sizeof cls) && strstr(cls, "eStuff")) {
        join_tags(st, n, sname, sizeof sname);
        const uint8_t *beg = NULL, *end = NULL;
        if (rd_mem(g_stuffReg, &beg, 8) && rd_mem(g_stuffReg + 8, &end, 8) && beg && end > beg && (size_t)(end - beg) < 8 * 200000) {
            const uint8_t *Tother = NULL; char oname[256] = "", ocls[96] = "?";
            for (const uint8_t *e = beg; e < end && !T; e += 8) {
                const uint8_t *X = NULL; const void *xvt = NULL;
                if (!rd_mem(e, &X, 8) || !X || X == S || !rd_mem(X, &xvt, 8)) continue;
                int m = read_name_tags(X, xt, OC_MAXTAGS);
                if (m != n + 1) continue;
                if (!oc_tags_have_suffix(xt, m)) continue;
                int ok = 1;
                for (int i = 0; i < n && ok; i++) {
                    int found = 0;
                    for (int j = 0; j < m; j++) if (!_stricmp(xt[j], st[i])) { found = 1; break; }
                    ok = found;
                }
                if (!ok) continue;
                if (xvt == svt) { T = X; join_tags(xt, m, tname, sizeof tname); }
                else if (!Tother) { Tother = X; join_tags(xt, m, oname, sizeof oname); rtti_name(X, ocls, sizeof ocls); }
            }
            if (!T && Tother) { T = Tother; strncpy(tname, oname, sizeof tname - 1); logf_("sibling \"%s\" is class %s, loaded ammo is %s (accepted)", oname, ocls, cls); }
        }
    }
    oc_cache_put(S, T, sname, tname);
    if (T) logf_("\"%s\" -> \"%s\"", sname, tname);
    else   logf_("no \"%s\" sibling for \"%s\" (class %s)", g_ocSuffix, sname, n > 0 ? cls : "?");
    return T;
}

/* overcharge sibling -> its base (name tags = T's minus the suffix), cached in the same table; NULL = T is not a sibling */
static const uint8_t *oc_base(const uint8_t *T)
{
    char now[96];
    for (int i = 0; i < g_ocCacheN; i++) if (g_ocCache[i].t == T) {
        if (oc_name(T, now, sizeof now) && !strcmp(now, g_ocCache[i].tname)) return (const uint8_t *)g_ocCache[i].s;
        g_ocCache[i] = g_ocCache[--g_ocCacheN]; break;
    }
    char tt[OC_MAXTAGS][OC_TAGLEN], xt[OC_MAXTAGS][OC_TAGLEN], tname[256] = "?", sname[256] = "";
    int n = read_name_tags(T, tt, OC_MAXTAGS);
    if (n < 2 || !oc_tags_have_suffix(tt, n)) return NULL;
    join_tags(tt, n, tname, sizeof tname);
    const uint8_t *B = NULL, *beg = NULL, *end = NULL;
    if (rd_mem(g_stuffReg, &beg, 8) && rd_mem(g_stuffReg + 8, &end, 8) && beg && end > beg && (size_t)(end - beg) < 8 * 200000) {
        for (const uint8_t *e = beg; e < end && !B; e += 8) {
            const uint8_t *X = NULL;
            if (!rd_mem(e, &X, 8) || !X || X == T) continue;
            int m = read_name_tags(X, xt, OC_MAXTAGS);
            if (m != n - 1 || oc_tags_have_suffix(xt, m)) continue;
            int ok = 1;
            for (int j = 0; j < m && ok; j++) {
                int found = 0;
                for (int i = 0; i < n; i++) if (!_stricmp(tt[i], xt[j])) { found = 1; break; }
                ok = found;
            }
            if (ok) { B = X; join_tags(xt, m, sname, sizeof sname); }
        }
    }
    if (B) { oc_cache_put(B, T, sname, tname); logf_("\"%s\" is the overcharge magazine of \"%s\"", tname, sname); }
    else logf_("no base magazine for \"%s\"", tname);
    return B;
}

/* what the firing weapon has loaded and what it should have loaded */
typedef struct {
    int tagged;                 /* the firing unit (or its vehicle) carries the overcharge tag */
    const uint8_t **slot;       /* item filling stuff pointer */
    const uint8_t *cur;         /* loaded stuff */
    const uint8_t *base;        /* the standard magazine (cur itself, or the base of a loaded sibling) */
    const uint8_t *sib;         /* the overcharge magazine (cur itself when a sibling is loaded), NULL = none */
} OcCtx;

static int oc_resolve(void *shooter, OcCtx *c)
{
    memset(c, 0, sizeof *c);
    const uint8_t *weapon = *(const uint8_t *const *)((const uint8_t *)shooter + g_weaponOff);
    if (!weapon) return 0;
    const uint8_t *owner = *(const uint8_t *const *)(weapon + g_ownerOff);
    if (!owner) return 0;
    int tagged = ent_has_tag(owner + g_entTagsOff, g_ocTag, g_ocTagLen);
    if (!tagged && g_entLinkOff) {      /* vehicle gun: the owner is the gunner, the button tags the vehicle he is linked to */
        uint32_t fl = 0; const uint8_t *link = NULL, *veh = NULL; char cls[64];
        if (rd_mem(owner + g_entLinkFlagOff, &fl, 4) && ((fl >> 28) & 1) && rd_mem(owner + g_entLinkOff, &link, 8) && link &&
            rd_mem(link + g_linkEntOff, &veh, 8) && veh && veh != owner && rtti_name(veh, cls, sizeof cls))
            tagged = ent_has_tag(veh + g_entTagsOff, g_ocTag, g_ocTagLen);
    }
    uint8_t *item = (uint8_t *)((WeaponItemFn)(*(void ***)weapon)[g_weaponItemSlot / 8])((void *)weapon);
    if (!item) return 0;
    const uint8_t **slot = (const uint8_t **)(item + g_fillStuffOff);
    const uint8_t *cur = NULL;
    if (!rd_mem(slot, &cur, 8) || !cur) return 0;
    c->tagged = tagged; c->slot = slot; c->cur = cur;
    const uint8_t *b = oc_base(cur);
    if (b) { c->base = b; c->sib = cur; }
    else   { c->base = cur; c->sib = oc_lookup(cur); }
    return 1;
}

/* Null-ammo guard (1.1.1): the engine's SpawnBullet reads [weapon+ammo] (eAmmo*) with no null check, and Launch only
   tests weapon->stuff. A weapon whose eAmmo is gone (holder killed / gun unlinked or deleted with a direct-control fire
   input still queued: client crash Nov 10 2026, exe+0x80BF96) must not fire: the shot is dropped, 0 = "nothing launched",
   which both callers already handle. */
static int g_ocNoAmmoLog = 10;
static int oc_no_ammo(void *shooter, const char *where)
{
    const uint8_t *weapon = NULL, *ammo = NULL;
    if (!g_weaponAmmoOff || !shooter) return 0;
    if (!rd_mem((const uint8_t *)shooter + g_weaponOff, &weapon, 8) || !weapon) return 0;
    if (!rd_mem(weapon + g_weaponAmmoOff, &ammo, 8) || ammo) return 0;
    if (g_ocNoAmmoLog > 0) { g_ocNoAmmoLog--; logf_("guard: %s on a weapon with no eAmmo (shooter %p, weapon %p): shot dropped", where, shooter, (const void *)weapon); }
    return 1;
}

/* SpawnBullet: make sure the right magazine is in when a bullet is created (a SyncedProjectiles spawn could run
   outside Launch); never swaps back while tagged */
static char hook_spawn_bullet(void *shooter, int syncIdx, float *zero, float *dir)
{
    if (oc_no_ammo(shooter, "SpawnBullet")) return 0;
    g_ocSpawnCalls++;
    OcCtx c;
    if (oc_resolve(shooter, &c)) {
        if (c.tagged && c.sib && c.cur != c.sib) {
            *c.slot = c.sib;
            if (g_ocSpawnLog > 0) { g_ocSpawnLog--; logf_("magazine swapped at SpawnBullet%s", syncIdx ? " [synced]" : ""); }
        } else if (!c.tagged && c.cur == c.sib && c.base && c.cur != c.base) *c.slot = c.base;
    }
    return g_spawnBulletOrig(shooter, syncIdx, zero, dir);
}

/* eBulletShooter::Launch: the persistent swap and the round cost */
static char hook_launch(void *shooter)
{
    OcCtx c;
    if (oc_no_ammo(shooter, "Launch")) return 0;
    if (!oc_resolve(shooter, &c)) return g_launchOrig(shooter);
    if (!c.tagged) {
        if (c.cur == c.sib && c.base && c.cur != c.base) {
            *c.slot = c.base;
            if (g_ocLog > 0) { g_ocLog--; logf_("toggle off, standard magazine back in"); }
        }
        return g_launchOrig(shooter);
    }
    if (!c.sib) return g_launchOrig(shooter);
    /* item+0x58 (right after the filling pointer) IS the magazine count, and with the sibling loaded the engine's own
       consume step takes nothing at all, so the dll takes the round: one per shot, like a standard shot */
    int *rounds = (int *)((uint8_t *)c.slot + 8);
    int before = 0;
    if (!rd_mem(rounds, &before, 4) || before < 1) return g_launchOrig(shooter);
    if (c.cur != c.sib) *c.slot = c.sib;        /* stays loaded */
    int spawnsBefore = g_ocSpawnCalls;
    char r = g_launchOrig(shooter);
    int spawnsIn = g_ocSpawnCalls - spawnsBefore;
    int after = before;
    rd_mem(rounds, &after, 4);
    int taken = (after < before) ? before - after : 0;
    if (after > before - 1) *rounds = before - 1;
    if (g_ocLog > 0) { g_ocLog--; int now = 0; rd_mem(rounds, &now, 4); logf_("shot: magazine %d -> %d (engine took %d in Launch, dll %d; %d SpawnBullet call(s) inside Launch)", before, now, taken, after - now, spawnsIn); }
    return r;
}

/* ---- setup ---------------------------------------------------------------------------- */
/* finders, then the hooks; 1 = ammo swap ON */
static int setup(int install)
{
    int ok = g_ocTagLen && g_ocSuffixLen && find_launch() && find_spawn_bullet_parts() && find_aim_point() && find_owner_off()
          && find_stuff_registry() && find_weapon_item_slot() && find_filling_offs() && find_entity_tags_off();
    if (ok && !find_linked()) { g_entLinkOff = 0; logf_("vehicle (gunner -> vehicle) tag lookup OFF"); }
    if (!ok || (install && !install_hook_at("eBulletShooter::Launch", (void *)g_launch, (void *)hook_launch, (void **)&g_launchOrig))) {
        logf_("overcharge ammo swap           OFF (engine code not recognised)");
        return 0;
    }
    if (install && !install_hook_at("SpawnBullet", (void *)g_spawnBullet, (void *)hook_spawn_bullet, (void **)&g_spawnBulletOrig))
        logf_("SpawnBullet hook failed: a synced bullet spawned outside Launch may fire the standard round");
    logf_("overcharge ammo swap           ON (unit tag \"%s\" -> ammo name tag \"%s\", 1 round per shot)", g_ocTag, g_ocSuffix);
    log_mp_note(1);
    return 1;
}

static void read_config(void)
{
    ini_str("OverchargeTag", "overcharge", g_ocTag, sizeof g_ocTag);
    ini_str("OverchargeSuffix", "overcharge", g_ocSuffix, sizeof g_ocSuffix);
    g_ocTagLen = strlen(g_ocTag); g_ocSuffixLen = strlen(g_ocSuffix);
    g_ocLog = g_verbose ? ini_int("OverchargeLog", 10) : 0;
    g_ocSpawnLog = g_verbose ? 8 : 0;
}

#ifndef GEM_TEST
/* called from gem_weaponscript.c DllMain; 1 = feature ON */
int overcharge_init(void)
{
    int on = 0;
    if (gem_module_open()) {
        read_config();
        on = setup(1);
    }
    gem_module_close();
    return on;
}
#else
int overcharge_selftest(uint8_t *base, const char *exePath, void ***hooked, int *nhooked, GemFound *found)
{
    gem_test_reset();
    g_testExe = exePath;
    get_sections(base);
    read_config();
    int r = setup(1);
    *hooked = g_hooked; *nhooked = g_nhooked;
    gem_found(found);
    return r;
}
#endif
