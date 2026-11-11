/* GEM SensorTags 0.2 - switch a unit's sensor off with an entity tag (Call to Arms: Gates of Hell, GEM 2, 1.067)
 *
 * While an actor (human, vehicle, gun) carries the tag "sensor_off" (ini Tag=), its sensor is off:
 * it sees nothing, spots nothing, its seen list is cleared. Remove the tag and the sensor comes back
 * in whatever state the engine wants for it at that moment. Plain entity tags: {tags add "sensor_off"},
 * {tags remove "sensor_off"}, breed tags through TeamTags/BreedTags-style dlls, triggers, anything.
 *
 * Engine (all found at runtime, nothing hard-coded; game 1.067 addresses for reference):
 *   eSensor (RTTI .?AVeSensor@Actor@@), one per actor at actor+0xDD0, owner actor at sensor+0x260
 *   (actors without a {sensor} block share a dummy sensor with owner 0, never touched here).
 *   sensor+0x18 = "off" byte. Every reader tests it against 0 (quant exe+0x873B00, IsActive
 *   exe+0x873C70, the seen-units pass exe+0x879FAC). Every writer does
 *     cmp [s+0x18],v ; je ; mov [s+0x18],v ; call [vtbl+0x58]   (slot 11 = clear seen lists)
 *   (place enter/exit with {turnoff {sensor}} exe+0x77C140/0x77B950, SetSenseless exe+0x9259B0,
 *   control-mode changes exe+0x7A21C0/0xA13A90/0xD00850).
 *   Quant = vtable slot 13 (+0x68), called every actor tick from eActor::Quant (exe+0x7EFE5B)
 *   whenever the sensor has an owner, off or not.
 * How the dll keeps it off without losing the engine's own value:
 *   while tagged the byte holds 2 + <engine value> (2 or 3). Readers see non-zero = off. Writers
 *   compare 2/3 against 0/1, so they always write and always call clear; the clear hook sees the
 *   value the engine just wrote and turns it back into 2 + value. Tag gone: the quant hook writes
 *   the engine value back (and clears, like the engine does on every change).
 * MP: sensing runs on the host only (clients use eSensorClient, whose quant is a no-op, and get the
 * seen lists from the host), so the host (game or call_to_arms_server.exe) must run the dll.
 * Only the eSensor vtable is patched; eSensorClient is left alone.
 * 0.2 (Alex, Nov 11 2026): the quant hook looks for the tag once per CheckMs per sensor (ini, default 1000;
 *   0 = every quant, as 0.1 did), always on a sensor's first quant. So a unit that spawns tagged is blind from its
 *   first tick, and a tag added or removed later acts within CheckMs. An engine write of the off byte still goes
 *   through the clear hook at once, tag test included, so a held sensor is never switched on in between.
 *
 * Build: build.bat   Offline finder + logic test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define ST_VERSION "0.2"
#define ST_SECT    "SensorTags"

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

/* ---------------------------------------------------------------- config */
static int  cfg_enabled = 1, cfg_log = 0;
static char cfg_tag[48] = "sensor_off";
static size_t cfg_taglen = 10;
static uint32_t cfg_check_ms = 1000;        /* 0.2 [SensorTags] CheckMs: ms between two looks at one unit's tags; 0 = every quant */

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines;
static void logf_(const char *fmt, ...)
{
    if (g_lines > 5000) return;
#ifndef ST_TEST
    if (!g_log && g_logpath[0]) g_log = fopen(g_logpath, "w");
    if (!g_log) return;
    FILE *out = g_log;
#else
    FILE *out = stdout;
#endif
    va_list ap; va_start(ap, fmt);
    vfprintf(out, fmt, ap); fputc('\n', out); fflush(out);
    va_end(ap);
    g_lines++;
}

/* ---------------------------------------------------------------- PE helpers */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;

static int get_sections(uint8_t *base)
{
    g_base = base; g_text.p = g_rdata.p = g_data.p = NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh[i].Name, ".data", 8)) g_data = s;
    }
    return g_text.p && g_rdata.p && g_data.p;
}
static int in_text(const void *a)  { return (uint8_t *)a >= g_text.p  && (uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }
#define OFF(a) ((unsigned long long)((uint8_t *)(a) - g_base))

static uint8_t *find_bytes(uint8_t *p, size_t n, const void *pat, size_t m)
{
    const uint8_t *q = pat;
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == q[0] && !memcmp(p + i, q, m)) return p + i;
    return NULL;
}
static uint8_t *rel32(uint8_t *at, int len) { return at + len + *(int32_t *)(at + len - 4); }

static uint8_t *next_call(uint8_t *p, size_t window, uint8_t **site)
{
    for (size_t i = 0; i < window; i++)
        if (p[i] == 0xE8) {
            uint8_t *t = rel32(p + i, 5);
            if (in_text(t)) { if (site) *site = p + i; return t; }
        }
    return NULL;
}

/* vtable of a class by its RTTI name (complete object locator, offset 0) */
static void **find_vtable(const char *rtti)
{
    size_t n = strlen(rtti) + 1;
    uint8_t *td = NULL;
    for (uint8_t *p = g_data.p; p + n <= g_data.p + g_data.n; p++)
        if (*p == '.' && !memcmp(p, rtti, n)) { td = p - 0x10; break; }
    if (!td) return NULL;
    uint32_t tdr = (uint32_t)(td - g_base);
    for (uint8_t *p = g_rdata.p; p + 24 <= g_rdata.p + g_rdata.n; p += 4) {
        uint32_t *col = (uint32_t *)p;
        if (col[0] != 1 || col[1] != 0 || col[3] != tdr || col[5] != (uint32_t)(p - g_base)) continue;
        uint64_t va = (uint64_t)(uintptr_t)p;
        for (uint8_t *q = g_rdata.p; q + 16 <= g_rdata.p + g_rdata.n; q += 8)
            if (*(uint64_t *)q == va) return (void **)(q + 8);
    }
    return NULL;
}

/* ---------------------------------------------------------------- engine bindings */
typedef uint64_t (*QuantFn)(void *s);
typedef void     (*ClearFn)(void *s);

#define TC_SELF_A  0x308
#define TC_SELF_B  0x310
#define TC_VEC_BEG 0x318
#define TC_VEC_END 0x320
#define TAG_SIZE   0x60
#define TAG_STR    0x40
#define SLOT_CLEAR 11                       /* +0x58, the call after every engine write of the off byte */

static void   **vt_sensor;
static uint32_t o_off, o_owner, o_sensor, o_tags, s_quant;
static QuantFn  f_quant;
static ClearFn  f_clear;

/* quant: the slot that calls [rax+0x70] (re-enable timer) and tests cmp byte [rdi+OFF],0,
   and reads the owner as mov rax,[rdi+OWN] (game/server) or cmp qword [rcx+OWN],0 (editor assert) */
static const char *find_sensor(void)
{
    vt_sensor = find_vtable(".?AVeSensor@Actor@@");
    if (!vt_sensor) return "eSensor vtable not found";
    int hits = 0;
    for (uint32_t k = 8; k < 24; k++) {
        uint8_t *f = vt_sensor[k];
        if (!in_text(f)) break;
        uint32_t off = 0, own = 0; int calls70 = 0;
        for (uint8_t *p = f; p < f + 0xA0; p++) {
            if (p[0] == 0xFF && p[1] == 0x50 && p[2] == 0x70) calls70 = 1;
            if (!off && p[0] == 0x80 && p[1] == 0x7F && p[2] >= 0x10 && p[2] < 0x40 && p[3] == 0x00 &&
                (p[4] == 0x75 || (p[4] == 0x0F && p[5] == 0x85))) off = p[2];
            if (!own && p[0] == 0x48 && ((p[1] == 0x8B && p[2] == 0x87) || (p[1] == 0x83 && (p[2] == 0xB9 || p[2] == 0xBF)))) {
                uint32_t d = *(uint32_t *)(p + 3);
                if (d >= 0x100 && d < 0x800 && !(d & 7) && (p[1] == 0x8B || p[7] == 0x00)) own = d;
            }
        }
        if (calls70 && off && own) { hits++; s_quant = k; o_off = off; o_owner = own; f_quant = (QuantFn)f; }
    }
    if (hits != 1) return hits ? "sensor quant slot ambiguous" : "sensor quant slot not found";
    f_clear = (ClearFn)vt_sensor[SLOT_CLEAR];
    if (!in_text(f_clear)) return "sensor clear slot not in .text";

    /* every engine write of the off byte is followed by call [rax+0x58]: mov [rcx+OFF],r8 / imm then the call */
    int writers = 0;
    for (uint8_t *p = g_text.p + 10; p + 3 <= g_text.p + g_text.n; p++) {
        if (p[0] != 0xFF || p[1] != 0x50 || p[2] != SLOT_CLEAR * 8) continue;
        for (uint8_t *w = p - 10; w < p - 2; w++)
            if ((w[0] == 0x88 && (w[1] == 0x41 || w[1] == 0x51) && w[2] == o_off) ||
                (w[0] == 0xC6 && w[1] == 0x41 && w[2] == o_off && w[3] <= 1)) { writers++; break; }
    }
    if (writers < 3) return "off-byte writers not recognised";

    /* actor -> sensor: mov rax,[rbx+SENSOR] ; cmp qword [rax+OWN],0 (eActor::Quant and friends) */
    uint32_t cand = 0; int agree = 0, disagree = 0;
    for (uint8_t *p = g_text.p; p + 15 <= g_text.p + g_text.n; p++) {
        if (p[0] != 0x48 || p[1] != 0x8B || (p[2] & 0xC0) != 0x80 || (p[2] & 7) == 4) continue;      /* mov r64,[r64+d32] */
        if (p[7] != 0x48 || p[8] != 0x83 || (p[9] & 0xF8) != 0xB8 || (p[9] & 7) == 4) continue;      /* cmp qword [r64+d32],imm8 */
        if (*(uint32_t *)(p + 10) != o_owner || p[14] != 0) continue;
        if (((p[2] >> 3) & 7) != (p[9] & 7)) continue;                    /* the loaded reg is the cmp base */
        uint32_t d = *(uint32_t *)(p + 3);
        if (d < 0x400 || d > 0x4000) continue;
        if (!cand) cand = d;
        if (d == cand) agree++; else disagree++;
    }
    if (!cand || disagree >= agree) return "actor sensor offset not found";
    o_sensor = cand;
    return NULL;
}

/* entity tag container offset, from the {tags add|remove} action (same derivation as GEM TeamTags) */
static const char *find_tags(void)
{
    uint8_t *ctor = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "tags", 5) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) return hits ? "tags action entry ambiguous" : "tags action entry not found";
    uint8_t *vt = NULL, *after = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && in_rdata(rel32(p, 7))) { vt = rel32(p, 7); after = p + 7; break; }
    if (!vt) return "tags action vtable not found";
    static const uint8_t ctor_sig[] = { 0xC6, 0x81, 0x00, 0x03, 0x00, 0x00, 0x00 };            /* mov byte [rcx+0x300],0 */
    uint8_t *cc = next_call(after, 0x20, NULL);
    if (!cc || !find_bytes(cc, 0x20, ctor_sig, sizeof ctor_sig)) return "TagContainer ctor not recognised";
    uint8_t *ev = ((uint8_t **)vt)[1], *p = NULL;
    if (!in_text(ev)) return "tags action execute not in .text";
    for (uint8_t *q = ev; q < ev + 0x100; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) { o_tags = *(uint32_t *)(q + 3); p = q + 7; break; }
    if (!p || o_tags < 0x100 || o_tags > 0x400) return "entity tag container offset not found";
    uint8_t *cp = next_call(p, 0x20, NULL);
    if (!cp || !find_bytes(cp, 0x40, ctor_sig, sizeof ctor_sig)) return "TagContainer copy not recognised";
    return NULL;
}

static const char *find_all(uint8_t *base)
{
    o_off = o_owner = o_sensor = o_tags = s_quant = 0;
    if (!get_sections(base)) return "sections not found";
    const char *e;
    if ((e = find_sensor())) return e;
    if ((e = find_tags())) return e;
    return NULL;
}

/* ---------------------------------------------------------------- tag test */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

static int has_tag(const uint8_t *e)
{
    const uint8_t *c = e + o_tags;
    if (*(const uint8_t *const *)(c + TC_SELF_A) != c || *(const uint8_t *const *)(c + TC_SELF_B) != c) return 0;
    const uint8_t *b = *(uint8_t *const *)(c + TC_VEC_BEG), *end = *(uint8_t *const *)(c + TC_VEC_END);
    if (!b || end < b || (size_t)(end - b) > 4096 * TAG_SIZE) return 0;
    for (const uint8_t *t = b; t < end; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_STR);
        if (s->size == cfg_taglen && !memcmp(ms_cstr(s), cfg_tag, cfg_taglen)) return 1;
    }
    return 0;
}

/* the sensor's owner, only if it really is the owner's sensor (actor+SENSOR == s) */
static uint8_t *owner_of(uint8_t *s)
{
    uint8_t *e = *(uint8_t **)(s + o_owner);
    if (!e || *(uint8_t **)(e + o_sensor) != s) return NULL;
    return e;
}

/* ---------------------------------------------------------------- hooks */
static QuantFn orig_quant;
static ClearFn orig_clear;
static volatile LONG g_forced;                                          /* sensors held off right now (log only) */

/* off byte: 0/1 = engine value, untouched; 2/3 = held off by the tag, engine value = byte - 2 */
static void sync(uint8_t *s, uint8_t *e)
{
    uint8_t b = s[o_off];
    if (has_tag(e)) {
        if (b >= 2) return;
        s[o_off] = (uint8_t)(2 + b);
        if (b == 0) orig_clear(s);                                       /* on -> off: drop the seen lists, as the engine does */
        InterlockedIncrement(&g_forced);
        if (cfg_log) logf_("off: entity %p sensor %p (engine value %d, %ld held)", (void *)e, (void *)s, b, g_forced);
    } else if (b >= 2) {
        uint8_t v = (uint8_t)((b - 2) & 1);
        s[o_off] = v;
        if (v == 0) orig_clear(s);                                       /* back on */
        InterlockedDecrement(&g_forced);
        if (cfg_log) logf_("on:  entity %p sensor %p (engine value %d, %ld held)", (void *)e, (void *)s, v, g_forced);
    }
}

/* 0.2: is this sensor's tag check due? One slot per sensor address, direct-mapped: the sensor last checked through
   that slot and when. A sensor not in its slot is always due (its first quant, or another sensor shares the slot:
   then both are simply checked on every quant, as 0.1 did for everyone), so a check can come early but never more
   than CheckMs late. The address is only compared, never read through. A new sensor that lands on the address of
   one checked less than CheckMs ago waits out the rest of that window; that is the one case where the first check is
   not at once. Wall-clock time: sensing runs on the host only, so nothing here has to match another machine. */
#ifdef ST_TEST
static uint32_t g_test_now;                 /* the test's clock */
static int g_syncs;                         /* tag checks done from the quant hook */
#define st_now() g_test_now
#else
#define st_now() ((uint32_t)GetTickCount())
#endif
#define NCHK 4096
static struct { uint8_t *s; uint32_t at; } g_chk[NCHK];
static uint32_t chk_slot(const uint8_t *s) { return ((uint32_t)((uintptr_t)s >> 4) * 2654435761u) >> 20; }   /* top 12 bits */
static int check_due(uint8_t *s)
{
    if (!cfg_check_ms) return 1;
    uint32_t now = st_now(), h = chk_slot(s);
    if (g_chk[h].s == s && (uint32_t)(now - g_chk[h].at) < cfg_check_ms) return 0;
    g_chk[h].s = s; g_chk[h].at = now;
    return 1;
}

static uint64_t hk_quant(void *sv)
{
    uint8_t *s = sv, *e = owner_of(s);
    if (e && check_due(s)) {
#ifdef ST_TEST
        g_syncs++;
#endif
        sync(s, e);
    }
    return orig_quant(sv);
}

/* the engine wrote 0/1 into a held sensor (it always does: 2/3 never equals its value) and calls
   clear: keep that value and hold the sensor off again */
static void hk_clear(void *sv)
{
    uint8_t *s = sv, b = s[o_off];
    if (b < 2) {
        uint8_t *e = owner_of(s);
        if (e && has_tag(e)) {
            s[o_off] = (uint8_t)(2 + b);
            if (cfg_log) logf_("held: entity %p sensor %p, engine wrote %d", (void *)e, (void *)s, b);
        }
    }
    orig_clear(sv);
}

/* ---------------------------------------------------------------- config */
static void load_ini(const char *ini)
{
    char buf[64];
    cfg_enabled = GetPrivateProfileIntA(ST_SECT, "Enabled", 1, ini);
    cfg_log     = GetPrivateProfileIntA(ST_SECT, "Log", 0, ini);
    int ms      = (int)GetPrivateProfileIntA(ST_SECT, "CheckMs", 1000, ini);                /* 0.2 */
    cfg_check_ms = ms < 0 ? 0 : ms > 60000 ? 60000 : (uint32_t)ms;
    GetPrivateProfileStringA(ST_SECT, "Tag", cfg_tag, buf, sizeof buf, ini);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    if (n && n < sizeof cfg_tag) {
        for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)buf[i]);   /* engine tags are lowercase */
        strcpy(cfg_tag, buf);
    }
    cfg_taglen = strlen(cfg_tag);
}

#ifndef ST_TEST
static int patch_slot(void **slot, void *hook)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

static void install(void)
{
    const char *err = find_all((uint8_t *)GetModuleHandleW(NULL));
    if (err) { logf_("[GEM SensorTags %s] NOT installed: %s", ST_VERSION, err); return; }
    if (!cfg_enabled) { logf_("[GEM SensorTags %s] Enabled=0: not installed", ST_VERSION); return; }
    orig_quant = f_quant; orig_clear = f_clear;
    if (!patch_slot(&vt_sensor[SLOT_CLEAR], (void *)hk_clear) || !patch_slot(&vt_sensor[s_quant], (void *)hk_quant)) {
        logf_("[GEM SensorTags] VirtualProtect failed (%lu)", GetLastError()); return;
    }
    logf_("[GEM SensorTags %s] installed | eSensor vtable exe+0x%llX quant slot %u exe+0x%llX clear slot %u exe+0x%llX | "
          "off +0x%X owner +0x%X actor sensor +0x%X tags +0x%X | Tag=%s Log=%d CheckMs=%u%s",
          ST_VERSION, OFF(vt_sensor), s_quant, OFF(f_quant), SLOT_CLEAR, OFF(f_clear),
          o_off, o_owner, o_sensor, o_tags, cfg_tag, cfg_log, cfg_check_ms, cfg_check_ms ? "" : " (every quant)");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH], exe[MAX_PATH], *nm;
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.');
        if (dot) *dot = 0;
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
/* build.bat test -> st_test.exe <exe>: the finders on that mapped exe, then the hold/restore logic
   on a fake sensor + actor (engine functions are not called offline). */
static int g_clears;
static void fake_clear(void *s) { (void)s; g_clears++; }
static uint64_t fake_quant(void *s) { (void)s; return 0; }

static __attribute__((aligned(16))) uint8_t g_sensor[0x400], g_actor[0x4000], g_tag[2][TAG_SIZE];
static void set_tags(int n, const char **names)
{
    uint8_t *c = g_actor + o_tags;
    *(uint8_t **)(c + TC_SELF_A) = c; *(uint8_t **)(c + TC_SELF_B) = c;
    for (int i = 0; i < n; i++) {
        MsString *s = (MsString *)(g_tag[i] + TAG_STR); memset(s, 0, sizeof *s);
        s->size = strlen(names[i]); memcpy(s->u.buf, names[i], s->size + 1); s->cap = 15;
    }
    *(uint8_t **)(c + TC_VEC_BEG) = g_tag[0]; *(uint8_t **)(c + TC_VEC_END) = g_tag[n];
}
/* an engine writer: cmp ; je ; mov ; call clear */
static void engine_write(uint8_t v) { if (g_sensor[o_off] != v) { g_sensor[o_off] = v; hk_clear(g_sensor); } }

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc > 1) {
        HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[1], GetLastError()); return 1; }
        const char *err = find_all((uint8_t *)m);
        if (err) { printf("%s: FAIL %s\n", argv[1], err); return 1; }
        printf("%s:\n  eSensor vtable exe+0x%llX quant slot %u exe+0x%llX clear slot %u exe+0x%llX\n"
               "  off +0x%X owner +0x%X actor sensor +0x%X tags +0x%X\n",
               argv[1], OFF(vt_sensor), s_quant, OFF(f_quant), SLOT_CLEAR, OFF(f_clear), o_off, o_owner, o_sensor, o_tags);
        if (o_off != 0x18 || o_owner != 0x260 || o_sensor != 0xDD0 || s_quant != 13) { printf("  FAIL offsets differ from 1.067\n"); bad = 1; }
    } else { o_off = 0x18; o_owner = 0x260; o_sensor = 0xDD0; o_tags = 0x1D0; }

    orig_clear = fake_clear; orig_quant = fake_quant;
    cfg_check_ms = 0;                                                    /* the 0.1 table below: a check on every quant */
    *(uint8_t **)(g_sensor + o_owner) = g_actor; *(uint8_t **)(g_actor + o_sensor) = g_sensor;
    const char *on[] = { "soldier", "sensor_off" }, *off[] = { "soldier" };
    struct { const char *what; int tag; int write; uint8_t want; int clears; } st[] = {
        { "untagged tick",                 0, -1, 0, 0 },
        { "tag added -> held (2)",         1, -1, 2, 1 },
        { "tick while held",               1, -1, 2, 0 },
        { "engine wants off (place) -> 3", 1,  1, 3, 1 },
        { "engine wants on -> 2",          1,  0, 2, 1 },
        { "engine wants off again -> 3",   1,  1, 3, 1 },
        { "tag removed -> engine off (1)", 0, -1, 1, 0 },
        { "engine wants on (untagged)",    0,  0, 0, 1 },
        { "tag added while on -> 2",       1, -1, 2, 1 },
        { "tag removed -> on (0)",         0, -1, 0, 1 },
    };
    g_sensor[o_off] = 0;
    for (size_t i = 0; i < sizeof st / sizeof *st; i++) {
        if (st[i].tag) set_tags(2, on); else set_tags(1, off);
        g_clears = 0;
        if (st[i].write >= 0) engine_write((uint8_t)st[i].write);
        hk_quant(g_sensor);
        int ok = g_sensor[o_off] == st[i].want && g_clears == st[i].clears;
        printf("  %-32s byte %d clears %d %s\n", st[i].what, g_sensor[o_off], g_clears, ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    *(uint8_t **)(g_actor + o_sensor) = NULL;                            /* not the actor's sensor: ignored */
    set_tags(2, on); g_sensor[o_off] = 0; hk_quant(g_sensor);
    printf("  %-32s byte %d %s\n", "foreign owner ignored", g_sensor[o_off], g_sensor[o_off] == 0 ? "OK" : "FAIL");
    if (g_sensor[o_off]) bad = 1;

    /* 0.2 CheckMs: looked at on the first quant, then once per CheckMs; engine writes still act at once */
    {
        #define CK(what, cond) do { int ok_ = (cond); printf("  %-48s byte %d %s\n", what, g_sensor[o_off], ok_ ? "OK" : "FAIL"); if (!ok_) bad = 1; } while (0)
        *(uint8_t **)(g_actor + o_sensor) = g_sensor;
        cfg_check_ms = 1000; memset(g_chk, 0, sizeof g_chk); g_test_now = 50000;
        set_tags(2, on); g_sensor[o_off] = 0; hk_quant(g_sensor);
        CK("CheckMs: spawned tagged, first quant -> 2", g_sensor[o_off] == 2);
        set_tags(1, off); g_test_now += 400; hk_quant(g_sensor);
        CK("CheckMs: tag removed, 400 ms later still 2", g_sensor[o_off] == 2);
        g_test_now += 599; hk_quant(g_sensor);
        CK("CheckMs: 999 ms later still 2", g_sensor[o_off] == 2);
        g_test_now += 1; hk_quant(g_sensor);
        CK("CheckMs: 1000 ms later back on (0)", g_sensor[o_off] == 0);
        set_tags(2, on); g_test_now += 999; hk_quant(g_sensor);
        CK("CheckMs: tag added, 999 ms later still 0", g_sensor[o_off] == 0);
        g_test_now += 1; hk_quant(g_sensor);
        CK("CheckMs: 1000 ms later held (2)", g_sensor[o_off] == 2);
        g_test_now += 10; engine_write(1);
        CK("CheckMs: engine write while held -> 3 at once", g_sensor[o_off] == 3);
        set_tags(1, off); g_test_now += 1000; hk_quant(g_sensor);
        CK("CheckMs: tag gone -> engine value (1)", g_sensor[o_off] == 1);
        set_tags(2, on); g_test_now += 10; engine_write(0);
        CK("CheckMs: tag added + engine write -> 2 at once", g_sensor[o_off] == 2);
        set_tags(1, off); g_test_now += 2000; hk_quant(g_sensor); g_syncs = 0;
        for (int i = 0; i < 1000; i++) { g_test_now += 10; hk_quant(g_sensor); }                /* 10 s of 10 ms quants */
        CK("CheckMs: 1000 quants in 10 s = 10 tag checks", g_syncs == 10 && g_sensor[o_off] == 0);
        uint8_t *other = g_sensor + 16;                                                        /* another sensor in the same slot */
        while (chk_slot(other) != chk_slot(g_sensor)) other += 16;
        int a1 = check_due(g_sensor), b1 = check_due(other), a2 = check_due(g_sensor), b2 = check_due(other);
        CK("CheckMs: two sensors in one slot, both always due", a1 == 0 && b1 == 1 && a2 == 1 && b2 == 1);
        memset(g_chk, 0, sizeof g_chk); g_test_now = 0xFFFFFE00u; int w1 = check_due(g_sensor);
        g_test_now += 0x300; int w2 = check_due(g_sensor); g_test_now += 300; int w3 = check_due(g_sensor);
        CK("CheckMs: tick counter wrap", w1 == 1 && w2 == 0 && w3 == 1);
        cfg_check_ms = 0; g_syncs = 0; for (int i = 0; i < 5; i++) hk_quant(g_sensor);
        CK("CheckMs=0: every quant", g_syncs == 5);
        #undef CK
    }
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
