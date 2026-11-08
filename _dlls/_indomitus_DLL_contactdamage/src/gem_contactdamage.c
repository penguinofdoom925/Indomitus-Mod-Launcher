/* GEM ContactDamage 0.1.0 - vehicle component hp damage from contact handlers (melee contactors)
 * Call to Arms: Gates of Hell (GEM 2, 1.067: game, editor, server)
 *
 * Script use (a vehicle's {on contact} handler, effector = the melee contactor):
 *     {tags add "melee_body_25"} {vehicle_health_damage_pierce} {tags remove "melee_body_25"}
 * takes 25 hp off the vehicle's "body" component exactly as a piercing round does: the engine's own subtract
 * (hp, then hp_broken), break / destroy state changes with their script events, and in MP the host's damage command.
 * The number comes from a tag <TagPrefix><N> on the vehicle (self) or, failing that, on the effector.
 *
 * Engine (game exe 1.067; found at runtime):
 *   Interaction::eVehicleHealthDamagePierce (RTTI), vtable slot 1 Execute (shared by every HealthDamage action) tail-calls
 *   slot 9 Apply(this, ctx, actor) (game exe+0x745960): needs ctx flag 0x2000000 (volume), returns on MP clients,
 *   comp = [[ctx+0x10]+0x38], dmg = slot 10 GetDamage(this, ctx, actor) (game exe+0x745DF0, reads the bullet at ctx+0x120
 *   with no check: a contact ctx has none, so vanilla crashes here), then actor vt+0x1E8, Health_Subtract(comp+0x14, dmg),
 *   SetState(comp, 1 / 2, ctx) and on the MP host an eVehicleHealthDamageCommand.
 *   ctx (0x190): +0 self, +8 effector, +0x10 volume, +0x120 bullet, +0x188 flags (0x400000 bullet, 0x2000000 volume).
 *   SetVolume(ctx, volume) game exe+0x76DDF0 (stores +0x10, copies the volume name, sets 0x2000000).
 *   actor components: inline vector +0xB90/+0xB98, stride 0x28, +0 desc ([desc+0x328] = name Tag, text at Tag+0x40);
 *   actor volumes: vector<volume*> +0x510/+0x518, volume+0x38 = its component.
 * Hook: the Pierce vtable's slots 9 and 10 only (Blast and every other action untouched). With a bullet in the ctx the
 * original runs unchanged. Without one: no damage tag = nothing happens (no crash); with one the ctx volume is pointed at a
 * volume of the body component for the call, slot 10 answers N, and the ctx volume is put back afterwards.
 * MP: Apply returns before slot 10 on clients, so only the host (call_to_arms_server.exe for hosted games) needs the dll;
 * clients get the result through the engine's damage command.
 *
 * Build: build.bat      Offline finder + logic test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define CD_VERSION "0.1.0"
#define TLEN 48

/* ---------------------------------------------------------------- config */
static int   cfg_enabled = 1, cfg_log = 1, cfg_effector = 1;
static char  cfg_prefix[TLEN] = "melee_body_", cfg_comp[TLEN] = "body";
static float cfg_max = 100000.0f;
static uint32_t cfg_enttags = 0x4E8, cfg_comps = 0xB90, cfg_vols = 0x510;
#define CTX_SELF     0x00
#define CTX_EFFECTOR 0x08
#define CTX_VOLUME   0x10
#define CTX_FLAGS    0x188
#define F_BULLET     0x400000u
#define F_EFFECTOR   0x1000000u
#define F_VOLUME     0x2000000u
#define COMP_STRIDE  0x28
#define DESC_NAME    0x328
#define VOL_COMP     0x38
#define TAG_STR      0x40

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines;
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

/* ---------------------------------------------------------------- strings / tags */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
/* MSVC std::string: buf/ptr +0, size +0x10, cap +0x18 */
static const char *str_get(const uint8_t *p, size_t *n)
{
    uint64_t size = *(const uint64_t *)(p + 0x10), cap = *(const uint64_t *)(p + 0x18);
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
/* N of the first tag "<prefix><N>" in an entity tag vector (0x60-byte Tags, text at +0x40), else 0 */
static float tag_amount(const uint8_t *ent)
{
    if (!ent) return 0;
    const uint8_t *v = ent + cfg_enttags;
    const uint8_t *b = *(const uint8_t *const *)v, *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e || e < b || (uintptr_t)(e - b) % 0x60 || (uintptr_t)(e - b) > 0x60 * 1024) return 0;
    size_t pl = strlen(cfg_prefix);
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + TAG_STR, &k);
        if (!s || k <= pl || k > 40) continue;
        size_t i = 0;
        for (; i < pl; i++) if (lc(s[i]) != lc(cfg_prefix[i])) break;
        if (i < pl) continue;
        char num[48]; memcpy(num, s + pl, k - pl); num[k - pl] = 0;
        char *end; float f = (float)strtod(num, &end);
        if (end == num || *end) continue;
        if (f > cfg_max) f = cfg_max;
        return f > 0 ? f : 0;
    }
    return 0;
}

/* a volume of the actor's component named cfg_comp, NULL if none */
static uint8_t *body_volume(const uint8_t *actor, const uint8_t **comp_out)
{
    const uint8_t *cb = *(const uint8_t *const *)(actor + cfg_comps), *ce = *(const uint8_t *const *)(actor + cfg_comps + 8);
    if (!cb || !ce || ce <= cb || (uintptr_t)(ce - cb) % COMP_STRIDE || (uintptr_t)(ce - cb) > COMP_STRIDE * 256) return NULL;
    const uint8_t *comp = NULL;
    for (const uint8_t *c = cb; c < ce && !comp; c += COMP_STRIDE) {
        const uint8_t *desc = *(const uint8_t *const *)c;
        const uint8_t *nm = desc ? *(const uint8_t *const *)(desc + DESC_NAME) : NULL;
        size_t k; const char *s = nm ? str_get(nm + TAG_STR, &k) : NULL;
        if (s && eq_ci(s, k, cfg_comp)) comp = c;
    }
    if (!comp) return NULL;
    uint8_t *const *vb = *(uint8_t *const *const *)(actor + cfg_vols), *const *ve = *(uint8_t *const *const *)(actor + cfg_vols + 8);
    if (!vb || !ve || ve <= vb || ve - vb > 1024) return NULL;
    for (uint8_t *const *v = vb; v < ve; v++)
        if (*v && *(const uint8_t *const *)(*v + VOL_COMP) == comp) { if (comp_out) *comp_out = comp; return *v; }
    return NULL;
}

/* ---------------------------------------------------------------- hooks */
typedef void  (*apply_t)(void *self, uint8_t *ctx, uint8_t *actor);
typedef float (*getdmg_t)(void *self, uint8_t *ctx, uint8_t *actor);
typedef void *(*setvol_t)(uint8_t *ctx, uint8_t *vol);
static apply_t  g_apply;
static getdmg_t g_getdmg;
static setvol_t g_setvol;
static float g_n;                           /* damage for the running Apply (game logic is single-threaded) */
static uint32_t st_hits, st_skip, st_nobody;

__attribute__((used, ms_abi)) float cd_getdmg(void *self, uint8_t *ctx, uint8_t *actor)
{
    if (g_n > 0) return g_n;
    if (!(*(const uint32_t *)(ctx + CTX_FLAGS) & F_BULLET)) return 0;   /* no bullet: vanilla would read a null bullet */
    return g_getdmg(self, ctx, actor);
}

__attribute__((used, ms_abi)) void cd_apply(void *self, uint8_t *ctx, uint8_t *actor)
{
    uint32_t fl = ctx ? *(const uint32_t *)(ctx + CTX_FLAGS) : 0;
    if (!ctx || !actor || (fl & F_BULLET)) { g_apply(self, ctx, actor); return; }   /* a real pierce: vanilla */
    float n = tag_amount(actor);
    if (n <= 0 && cfg_effector && (fl & F_EFFECTOR)) n = tag_amount(*(const uint8_t *const *)(ctx + CTX_EFFECTOR));
    if (n <= 0) { st_skip++; return; }                                    /* no amount: nothing (vanilla would crash) */
    const uint8_t *comp = NULL;
    uint8_t *vol = body_volume(actor, &comp);
    if (!vol) {
        st_nobody++;
        if (cfg_log && g_lines < 40) { g_lines++; logf_("hit: actor %p has no \"%s\" component with a volume, %.1f hp dropped", (void *)actor, cfg_comp, n); }
        return;
    }
    uint8_t *saved = (fl & F_VOLUME) ? *(uint8_t **)(ctx + CTX_VOLUME) : NULL;
    float hp0 = *(const float *)(comp + 0x14), br0 = *(const float *)(comp + 0x18);
    g_setvol(ctx, vol);
    g_n = n;
    g_apply(self, ctx, actor);
    g_n = 0;
    g_setvol(ctx, saved);                                                 /* NULL clears the volume flag again */
    if (!(fl & F_VOLUME)) *(uint32_t *)(ctx + CTX_FLAGS) &= ~F_VOLUME;
    st_hits++;
    if (cfg_log && g_lines < 40) {
        g_lines++;
        logf_("hit: actor %p %s %.1f hp: hp %.1f -> %.1f, broken %.1f -> %.1f, state %d (hits %u, no amount %u, no body %u)",
              (void *)actor, cfg_comp, n, hp0, *(const float *)(comp + 0x14), br0, *(const float *)(comp + 0x18), (int)(int8_t)comp[0x24],
              st_hits, st_skip, st_nobody);
    }
}

/* ---------------------------------------------------------------- finders */
typedef struct { uint8_t *base, *lo, *hi; size_t size; uint64_t pref; IMAGE_NT_HEADERS64 *nt; } img_t;
typedef struct { void **vt; uint8_t *apply, *getdmg, *setvol; } sites_t;

static int in_text(img_t *im, const uint8_t *p) { return p >= im->lo && p < im->hi; }
static uint8_t *fixp(img_t *im, uint64_t x) { return (x - im->pref < im->size) ? im->base + (x - im->pref) : (uint8_t *)(uintptr_t)x; }

/* vtable by RTTI name: type descriptor -> complete object locator (1, 0, ..., td rva, ..., self rva) -> vtable */
static void **find_vtable_rtti(img_t *im, const char *name)
{
    size_t n = strlen(name) + 1;
    uint8_t *td = NULL;
    for (uint8_t *p = im->hi; p + n <= im->base + im->size; p++)
        if (*p == '.' && !memcmp(p, name, n)) { td = p - 0x10; break; }
    if (!td) return NULL;
    uint32_t tdr = (uint32_t)(td - im->base);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(im->nt);
    for (int k = 0; k < im->nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!memcmp(sec->Name, ".text", 5)) continue;
        uint8_t *lo = im->base + sec->VirtualAddress, *hi = lo + sec->Misc.VirtualSize;
        for (uint8_t *p = lo; p + 24 <= hi; p += 4) {
            const uint32_t *col = (const uint32_t *)p;
            if (col[0] != 1 || col[1] != 0 || col[3] != tdr || col[5] != (uint32_t)(p - im->base)) continue;
            uint64_t va1 = (uint64_t)(uintptr_t)p, va2 = (uint64_t)(p - im->base) + im->pref;
            IMAGE_SECTION_HEADER *s2 = IMAGE_FIRST_SECTION(im->nt);
            for (int j = 0; j < im->nt->FileHeader.NumberOfSections; j++, s2++) {
                if (!memcmp(s2->Name, ".text", 5)) continue;
                uint8_t *l2 = im->base + s2->VirtualAddress, *h2 = l2 + s2->Misc.VirtualSize;
                for (uint8_t *q = l2; q + 16 <= h2; q += 8)
                    if (*(uint64_t *)q == va1 || *(uint64_t *)q == va2) return (void **)(q + 8);
            }
        }
    }
    return NULL;
}

static const char *find_all(uint8_t *base, sites_t *s)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    img_t im = { base, NULL, NULL, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.ImageBase, nt };
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (!memcmp(sec->Name, ".text", 5)) { im.lo = base + sec->VirtualAddress; im.hi = im.lo + sec->Misc.VirtualSize; }
    if (!im.lo) return "no .text section";
    memset(s, 0, sizeof *s);
    s->vt = find_vtable_rtti(&im, ".?AUeVehicleHealthDamagePierce@Interaction@@");
    if (!s->vt) return "eVehicleHealthDamagePierce vtable not found";
    s->apply = fixp(&im, (uint64_t)(uintptr_t)s->vt[9]);
    s->getdmg = fixp(&im, (uint64_t)(uintptr_t)s->vt[10]);
    if (!in_text(&im, s->apply) || !in_text(&im, s->getdmg)) return "vtable slots 9/10 not in .text (already patched?)";
    /* Apply must test the volume flag: test/bt on [reg+0x188] with 0x2000000 near the start */
    int ok = 0;
    for (uint8_t *q = s->apply; q < s->apply + 0x80 && !ok; q++)
        if (q[0] == 0x88 && q[1] == 0x01 && q[2] == 0 && q[3] == 0 && (q[-1] & 0xC0) == 0x80 &&
            ((q[4] == 0 && q[5] == 0 && q[6] == 0 && q[7] == 2) || q[4] == 0x19 || q[4] == 0x03)) ok = 1;
    if (!ok) return "Apply (slot 9) not recognised: no ctx+0x188 volume flag test";
    /* GetDamage must read the bullet at ctx+0x120 */
    ok = 0;
    for (uint8_t *q = s->getdmg; q < s->getdmg + 0x80 && !ok; q++)
        if (q[0] == 0x8B && (q[1] & 0xC0) == 0x80 && q[2] == 0x20 && q[3] == 0x01 && q[4] == 0 && q[5] == 0) ok = 1;
    if (!ok) return "GetDamage (slot 10) not recognised: no ctx+0x120 bullet read";
    /* SetVolume: push rbx; sub rsp,20h; mov rbx,rcx; ... mov [rcx|rbx+10h],rdx ... [reg+398h] (volume name) ...
     * or dword [rbx+188h],2000000h  (editor has an assert between them) */
    static const uint8_t head[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
    static const uint8_t orf[]  = { 0x81, 0x8B, 0x88, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 };
    int hits = 0;
    for (uint8_t *p = im.lo; p + 0x80 <= im.hi; p++) {
        if (memcmp(p, head, sizeof head)) continue;
        int st = 0, nm = 0, fl = 0;
        for (uint8_t *q = p + sizeof head; q < p + 0x80 && !fl; q++) {
            if (q[0] == 0xC3) break;
            if (q[0] == 0x48 && q[1] == 0x89 && (q[2] == 0x51 || q[2] == 0x53) && q[3] == 0x10) st = 1;
            if (st && q[0] == 0x98 && q[1] == 0x03 && q[2] == 0 && q[3] == 0) nm = 1;
            if (nm && !memcmp(q, orf, sizeof orf)) fl = 1;
        }
        if (!fl) continue;
        if (s->setvol != p) { s->setvol = p; hits++; }
    }
    if (hits != 1) return hits ? "ctx SetVolume not unique" : "ctx SetVolume not found";
    return NULL;
}

/* ---------------------------------------------------------------- ini */
__attribute__((unused)) static void load_ini(const char *ini)
{
    char buf[128];
    cfg_enabled  = GetPrivateProfileIntA("ContactDamage", "Enabled", 1, ini);
    cfg_log      = GetPrivateProfileIntA("ContactDamage", "Log", 1, ini);
    cfg_effector = GetPrivateProfileIntA("ContactDamage", "ReadEffector", 1, ini);
    GetPrivateProfileStringA("ContactDamage", "TagPrefix", "melee_body_", cfg_prefix, sizeof cfg_prefix, ini);
    GetPrivateProfileStringA("ContactDamage", "Component", "body", cfg_comp, sizeof cfg_comp, ini);
    GetPrivateProfileStringA("ContactDamage", "MaxDamage", "100000", buf, sizeof buf, ini);
    cfg_max = (float)atof(buf); if (cfg_max <= 0) cfg_max = 100000.0f;
}

#ifndef CD_TEST
static void install(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    if (!cfg_enabled) { logf_("[GEM ContactDamage %s] Enabled=0, not installed", CD_VERSION); return; }
    sites_t s;
    const char *err = find_all(base, &s);
    if (err) { logf_("[GEM ContactDamage %s] NOT installed: %s", CD_VERSION, err); return; }
    g_apply = (apply_t)s.apply; g_getdmg = (getdmg_t)s.getdmg; g_setvol = (setvol_t)s.setvol;
    DWORD old;
    if (!VirtualProtect(&s.vt[9], 16, PAGE_READWRITE, &old)) { logf_("[GEM ContactDamage] VirtualProtect failed (%lu)", GetLastError()); return; }
    s.vt[9] = (void *)cd_apply; s.vt[10] = (void *)cd_getdmg;
    VirtualProtect(&s.vt[9], 16, old, &old);
    logf_("[GEM ContactDamage %s] installed: Pierce vtable exe+0x%llX, Apply exe+0x%llX, GetDamage exe+0x%llX, SetVolume exe+0x%llX | "
          "TagPrefix=%s Component=%s ReadEffector=%d MaxDamage=%.0f Log=%d",
          CD_VERSION, (unsigned long long)((uint8_t *)s.vt - base), (unsigned long long)(s.apply - base),
          (unsigned long long)(s.getdmg - base), (unsigned long long)(s.setvol - base), cfg_prefix, cfg_comp, cfg_effector, cfg_max, cfg_log);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH], exe[MAX_PATH], *nm;
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.'); if (dot) *dot = 0;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path, strstr(nm, "_ed") ? "_editor" : (strstr(nm, "_server") ? "_server" : ""));
        load_ini(ini);
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
static void fake_str(uint8_t *p, const char *s) { memset(p, 0, 0x20); size_t n = strlen(s); memcpy(p, s, n); *(uint64_t *)(p + 0x10) = n; *(uint64_t *)(p + 0x18) = 15; }
static uint8_t *f_seen_vol; static int f_applies; static float f_dmg;
static __attribute__((ms_abi)) void f_apply(void *self, uint8_t *ctx, uint8_t *actor)
{
    f_applies++; f_seen_vol = (*(uint32_t *)(ctx + CTX_FLAGS) & F_VOLUME) ? *(uint8_t **)(ctx + CTX_VOLUME) : NULL;
    f_dmg = cd_getdmg(self, ctx, actor);
}
static __attribute__((ms_abi)) float f_getdmg(void *self, uint8_t *ctx, uint8_t *actor) { (void)self; (void)ctx; (void)actor; return 7.0f; }
static __attribute__((ms_abi)) void *f_setvol(uint8_t *ctx, uint8_t *vol)
{
    if (vol) { *(uint8_t **)(ctx + CTX_VOLUME) = vol; *(uint32_t *)(ctx + CTX_FLAGS) |= F_VOLUME; }
    else *(uint32_t *)(ctx + CTX_FLAGS) &= ~F_VOLUME;
    return ctx;
}

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        HMODULE m = LoadLibraryExA(argv[i], NULL, DONT_RESOLVE_DLL_REFERENCES);
        printf("%s\n", argv[i]);
        if (!m) { printf("  FAIL load (%lu)\n", GetLastError()); bad = 1; continue; }
        sites_t s; const char *err = find_all((uint8_t *)m, &s);
        uint8_t *b = (uint8_t *)m;
        if (err) { printf("  FAIL %s\n", err); bad = 1; }
        else printf("  Pierce vtable exe+0x%llX Apply exe+0x%llX GetDamage exe+0x%llX SetVolume exe+0x%llX\n",
                    (unsigned long long)((uint8_t *)s.vt - b), (unsigned long long)(s.apply - b),
                    (unsigned long long)(s.getdmg - b), (unsigned long long)(s.setvol - b));
        FreeLibrary(m);
    }
    /* logic: fake vehicle with components "turret" + "body", volumes for both */
    cfg_log = 0; g_apply = f_apply; g_getdmg = f_getdmg; g_setvol = f_setvol;
    static uint8_t actor[0x1000], comps[COMP_STRIDE * 2], d1[0x400], d2[0x400], n1[0x60], n2[0x60], v1[0x60], v2[0x60], *vols[2];
    static uint8_t tags[0x60 * 2], etags[0x60], eff[0x1000], ctx[0x190], other[0x60];
    fake_str(n1 + TAG_STR, "turret"); fake_str(n2 + TAG_STR, "Body");
    *(uint8_t **)(d1 + DESC_NAME) = n1; *(uint8_t **)(d2 + DESC_NAME) = n2;
    *(uint8_t **)comps = d1; *(uint8_t **)(comps + COMP_STRIDE) = d2;
    *(float *)(comps + COMP_STRIDE + 0x14) = 175.0f;
    *(uint8_t **)(actor + cfg_comps) = comps; *(uint8_t **)(actor + cfg_comps + 8) = comps + COMP_STRIDE * 2;
    *(uint8_t **)(v1 + VOL_COMP) = comps; *(uint8_t **)(v2 + VOL_COMP) = comps + COMP_STRIDE;
    vols[0] = v1; vols[1] = v2;
    *(uint8_t **)(actor + cfg_vols) = (uint8_t *)vols; *(uint8_t **)(actor + cfg_vols + 8) = (uint8_t *)(vols + 2);
    fake_str(tags + TAG_STR, "team1"); fake_str(tags + 0x60 + TAG_STR, "melee_body_25");
    *(uint8_t **)(actor + cfg_enttags) = tags; *(uint8_t **)(actor + cfg_enttags + 8) = tags + 0x60 * 2;
    int fails = 0;
    /* 1: self tag, touched the turret volume -> body volume during the call, 25 hp, turret volume restored */
    memset(ctx, 0, sizeof ctx); *(uint8_t **)(ctx + CTX_VOLUME) = v1; *(uint32_t *)(ctx + CTX_FLAGS) = F_VOLUME;
    f_applies = 0; cd_apply(NULL, ctx, actor);
    int t1 = f_applies == 1 && f_seen_vol == v2 && f_dmg == 25.0f && *(uint8_t **)(ctx + CTX_VOLUME) == v1 && (*(uint32_t *)(ctx + CTX_FLAGS) & F_VOLUME);
    /* 2: bullet present -> vanilla untouched (volume stays, GetDamage = original) */
    *(uint32_t *)(ctx + CTX_FLAGS) = F_VOLUME | F_BULLET; f_applies = 0; cd_apply(NULL, ctx, actor);
    int t2 = f_applies == 1 && f_seen_vol == v1 && f_dmg == 7.0f;
    /* 3: no tag anywhere -> nothing, no crash */
    *(uint8_t **)(actor + cfg_enttags + 8) = tags + 0x60;                /* only team1 */
    *(uint32_t *)(ctx + CTX_FLAGS) = F_VOLUME; f_applies = 0; cd_apply(NULL, ctx, actor);
    int t3 = f_applies == 0;
    /* 4: amount on the effector, ctx had no volume -> volume flag cleared again afterwards */
    fake_str(etags + TAG_STR, "melee_body_12.5");
    *(uint8_t **)(eff + cfg_enttags) = etags; *(uint8_t **)(eff + cfg_enttags + 8) = etags + 0x60;
    memset(ctx, 0, sizeof ctx); *(uint8_t **)(ctx + CTX_EFFECTOR) = eff; *(uint32_t *)(ctx + CTX_FLAGS) = F_EFFECTOR;
    f_applies = 0; cd_apply(NULL, ctx, actor);
    int t4 = f_applies == 1 && f_seen_vol == v2 && f_dmg == 12.5f && !(*(uint32_t *)(ctx + CTX_FLAGS) & F_VOLUME);
    /* 5: no body component -> dropped */
    fake_str(n2 + TAG_STR, "hull"); f_applies = 0; cd_apply(NULL, ctx, actor);
    int t5 = f_applies == 0;
    /* 6: bad tag text ignored */
    fake_str(other + TAG_STR, "melee_body_x"); *(uint8_t **)(eff + cfg_enttags) = other; *(uint8_t **)(eff + cfg_enttags + 8) = other + 0x60;
    fake_str(n2 + TAG_STR, "body"); f_applies = 0; cd_apply(NULL, ctx, actor);
    int t6 = f_applies == 0;
    /* 7: GetDamage without a bullet and no amount returns 0 instead of reading a null bullet */
    memset(ctx, 0, sizeof ctx); int t7 = cd_getdmg(NULL, ctx, actor) == 0.0f;
    int ok = t1 && t2 && t3 && t4 && t5 && t6 && t7;
    printf("logic: self-tag %d bullet %d no-tag %d effector %d no-body %d bad-tag %d getdmg %d -> %s\n", t1, t2, t3, t4, t5, t6, t7, ok ? "OK" : "FAIL");
    if (!ok) fails++;
    if (fails) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
