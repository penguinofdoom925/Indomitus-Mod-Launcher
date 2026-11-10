/* GEM CostRed 0.1 - MP buy menu: the cost is red whenever you can't afford the unit
 * (Call to Arms: Gates of Hell, GEM 2, 1.067; game exe only, the editor / server have no buy menu)
 *
 * Engine, per buy button update (two functions, game 1.067 exe+0x22FB20 and exe+0xCD03B0):
 *     text_mp = format("actionbar/count_fx", cost_mp) ; text_cp = format("actionbar/count_fx", cost_cp)
 *     if (mouse is on this button) {                              <- GATE  cmp ... ; je/jne far
 *         if (cost_mp > have_mp) text_mp = format("actionbar/count_fx_red", cost_mp - have_mp)
 *         if (cost_cp > have_cp) text_cp = format("actionbar/count_fx_red", cost_cp - have_cp)
 *     }
 *   each red block:  comiss xmmC,xmmH ; jbe far ; subss xmmC,xmmH ; ... lea rcx,"actionbar/count_fx_red"
 * So the red text only shows under the mouse, and it is the amount you are short, not the cost.
 *
 * Patch (all found by signature from the "actionbar/count_fx_red" string, nothing hard-coded):
 *   GATE jump (6 bytes)            -> nops: the red blocks run for every button, every update
 *   subss xmmC,xmmH (5 bytes)      -> call stub (5 bytes), stub = <the gate's own cmp> ; j<gate cc> +5 ; subss ; ret
 * Result: not under the mouse = the cost in red; under the mouse = the shortfall in red, as before
 * (ini HoverShortfall=0: the cost stays the cost under the mouse too, subss -> nops, no stub).
 * The gate's cmp only reads rbp-relative locals / a callee-saved register, so it works from the stub.
 * UI only, nothing synced: each player decides for themself.
 *
 * Build: build.bat   Offline finder test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define CR_VERSION "0.1"
#define CR_SECT    "CostRed"

static int cfg_enabled = 1, cfg_hover_shortfall = 1;

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static void logf_(const char *fmt, ...)
{
#ifndef CR_TEST
    if (!g_log && g_logpath[0]) g_log = fopen(g_logpath, "w");
    if (!g_log) return;
    FILE *out = g_log;
#else
    FILE *out = stdout;
#endif
    va_list ap; va_start(ap, fmt);
    vfprintf(out, fmt, ap); fputc('\n', out); fflush(out);
    va_end(ap);
}

/* ---------------------------------------------------------------- PE helpers */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata;
static uint8_t *g_base;
#define OFF(a) ((unsigned long long)((uint8_t *)(a) - g_base))

static int get_sections(uint8_t *base)
{
    g_base = base; g_text.p = g_rdata.p = NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
    }
    return g_text.p && g_rdata.p;
}

/* ---------------------------------------------------------------- finder */
#define MAX_FN    4                 /* buy button update functions (1.067: 2) */
#define MAX_BLOCK 2                 /* red blocks per function (MP, CP) */
typedef struct {
    uint8_t *gate;                  /* the far je / jne */
    uint8_t *cmp; int cmp_len;      /* the compare in front of it */
    uint8_t *sub[MAX_BLOCK]; int nsub;
} Site;
static Site g_site[MAX_FN];
static int  g_nsite;

/* comiss xmmC,xmmH ; jbe rel32 ; subss xmmC,xmmH  (both with a REX prefix: 41 0F 2F rm / F3 41 0F 5C rm) */
static int is_red_block(const uint8_t *sub)
{
    const uint8_t *c = sub - 10;
    return sub[0] == 0xF3 && (sub[1] & 0xF0) == 0x40 && sub[2] == 0x0F && sub[3] == 0x5C && sub[4] >= 0xC0 &&
           c[0] == sub[1] && c[1] == 0x0F && c[2] == 0x2F && c[3] == sub[4] &&
           c[4] == 0x0F && c[5] == 0x86;
}

static const char *find_all(uint8_t *base)
{
    static const char key[] = "actionbar/count_fx_red";
    g_nsite = 0;
    if (!get_sections(base)) return "sections not found";
    uint8_t *str = NULL;
    for (uint8_t *p = g_rdata.p; p + sizeof key <= g_rdata.p + g_rdata.n; p++)
        if (*p == 'a' && !memcmp(p, key, sizeof key)) { str = p; break; }
    if (!str) return "no buy menu in this exe (string not found)";

    /* every lea rcx,[rip+key], in address order; the subss sits at most 0x20 bytes in front of it */
    uint8_t *prev = NULL;
    for (uint8_t *p = g_text.p + 0x40; p + 7 <= g_text.p + g_text.n; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x0D || p + 7 + *(int32_t *)(p + 3) != str) continue;
        uint8_t *sub = NULL;
        for (uint8_t *q = p - 5; q >= p - 0x20; q--)
            if (is_red_block(q)) { sub = q; break; }
        if (!sub) return "red block not recognised";
        if (prev && sub - prev < 0x200) {                      /* second block of the same function */
            Site *s = &g_site[g_nsite - 1];
            if (s->nsub >= MAX_BLOCK) return "too many red blocks in one function";
            s->sub[s->nsub++] = sub;
        } else {                                               /* first block: the gate is right in front of its comiss */
            if (g_nsite >= MAX_FN) return "too many buy button functions";
            Site *s = &g_site[g_nsite];
            uint8_t *gate = sub - 10 - 6;
            if (gate[0] != 0x0F || (gate[1] != 0x84 && gate[1] != 0x85)) return "hover gate jump not recognised";
            if (gate[-7] == 0x80 && gate[-6] == 0xBD && gate[-1] == 0x00)         { s->cmp = gate - 7; s->cmp_len = 7; }  /* cmp byte [rbp+d32],0 */
            else if (gate[-4] == 0x48 && gate[-3] == 0x39 && gate[-2] == 0x5D)    { s->cmp = gate - 4; s->cmp_len = 4; }  /* cmp [rbp+d8],rbx */
            else return "hover gate compare not recognised";
            /* the gate must jump past this function's red blocks */
            if (gate + 6 + *(int32_t *)(gate + 2) <= sub) return "hover gate target unexpected";
            s->gate = gate; s->sub[0] = sub; s->nsub = 1;
            g_nsite++;
        }
        prev = sub;
    }
    if (!g_nsite) return "no red blocks found";
    return NULL;
}

/* stub: <cmp> ; j<cc> +5 ; subss ; ret   (cc = the gate's: taken = not under the mouse = keep the cost) */
static size_t make_stub(uint8_t *out, const Site *s, const uint8_t *sub)
{
    size_t n = 0;
    memcpy(out, s->cmp, s->cmp_len); n += s->cmp_len;
    out[n++] = (uint8_t)(s->gate[1] - 0x10); out[n++] = 5;     /* 0F 84 -> 74, 0F 85 -> 75 */
    memcpy(out + n, sub, 5); n += 5;
    out[n++] = 0xC3;
    return n;
}

#ifndef CR_TEST
/* executable page within rel32 reach of the exe's code */
static uint8_t *alloc_near(uint8_t *lo, uint8_t *hi)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t g = si.dwAllocationGranularity, reach = 0x70000000;
    uintptr_t from = (uintptr_t)hi > reach ? ((uintptr_t)hi - reach + g) & ~(g - 1) : g;
    uintptr_t to = (uintptr_t)lo + reach;
    for (uintptr_t a = (uintptr_t)lo & ~(g - 1); a >= from && a >= g; a -= g) {
        void *p = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    for (uintptr_t a = ((uintptr_t)hi + g) & ~(g - 1); a < to; a += g) {
        void *p = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

static int poke(uint8_t *at, const uint8_t *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

static void install(void)
{
    static const uint8_t nop6[6] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 }, nop5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
    const char *err = find_all((uint8_t *)GetModuleHandleW(NULL));
    if (err) { logf_("[GEM CostRed %s] NOT installed: %s", CR_VERSION, err); return; }
    if (!cfg_enabled) { logf_("[GEM CostRed %s] Enabled=0: not installed", CR_VERSION); return; }

    uint8_t *page = NULL, *w = NULL;
    if (cfg_hover_shortfall) {
        page = w = alloc_near(g_text.p, g_text.p + g_text.n);
        if (!page) { logf_("[GEM CostRed %s] NOT installed: no memory near the exe", CR_VERSION); return; }
    }
    /* stubs first, then the subss sites, the gates last: until a gate goes the red blocks only run under the mouse */
    for (int i = 0; i < g_nsite; i++)
        for (int k = 0; k < g_site[i].nsub; k++) {
            uint8_t *sub = g_site[i].sub[k];
            if (!cfg_hover_shortfall) { if (!poke(sub, nop5, 5)) goto fail; continue; }
            uint8_t call[5] = { 0xE8 };
            size_t n = make_stub(w, &g_site[i], sub);
            *(int32_t *)(call + 1) = (int32_t)(w - (sub + 5));
            w += (n + 15) & ~(size_t)15;
            if (!poke(sub, call, 5)) goto fail;
        }
    if (page) { DWORD old; VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old); FlushInstructionCache(GetCurrentProcess(), page, 0x1000); }
    for (int i = 0; i < g_nsite; i++)
        if (!poke(g_site[i].gate, nop6, 6)) goto fail;
    for (int i = 0; i < g_nsite; i++)
        logf_("  function %d: gate exe+0x%llX, red blocks %d (exe+0x%llX%s)", i + 1, OFF(g_site[i].gate), g_site[i].nsub,
              OFF(g_site[i].sub[0]), g_site[i].nsub > 1 ? ", ..." : "");
    logf_("[GEM CostRed %s] installed | %d function(s) | HoverShortfall=%d", CR_VERSION, g_nsite, cfg_hover_shortfall);
    return;
fail:
    logf_("[GEM CostRed %s] VirtualProtect failed (%lu): partly installed", CR_VERSION, GetLastError());
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
        if (strstr(nm, "_ed") || strstr(nm, "_server")) return TRUE;         /* no buy menu there: stay silent */
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s.log", path);
        cfg_enabled         = GetPrivateProfileIntA(CR_SECT, "Enabled", 1, ini);
        cfg_hover_shortfall = GetPrivateProfileIntA(CR_SECT, "HoverShortfall", 1, ini);
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
/* build.bat test -> cr_test.exe <exe> [expected functions]: the finder on that mapped exe + the stub bytes */
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: cr_test <exe> [expected functions]\n"); return 1; }
    int want = argc > 2 ? atoi(argv[2]) : -1;
    HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) { printf("%s: load failed %lu\n", argv[1], GetLastError()); return 1; }
    const char *err = find_all((uint8_t *)m);
    printf("%s:\n", argv[1]);
    if (err) {
        printf("  %s\n  %s\n", err, want == 0 ? "OK (none expected)" : "FAIL");
        return want != 0;
    }
    int bad = want >= 0 && g_nsite != want;
    for (int i = 0; i < g_nsite; i++) {
        Site *s = &g_site[i];
        printf("  function %d: cmp exe+0x%llX (%d bytes), gate exe+0x%llX j%s -> exe+0x%llX\n", i + 1, OFF(s->cmp), s->cmp_len,
               OFF(s->gate), s->gate[1] == 0x84 ? "e" : "ne", OFF(s->gate + 6 + *(int32_t *)(s->gate + 2)));
        if (s->nsub != 2) bad = 1;
        for (int k = 0; k < s->nsub; k++) {
            uint8_t stub[32]; size_t n = make_stub(stub, s, s->sub[k]);
            printf("    red block exe+0x%llX, stub:", OFF(s->sub[k]));
            for (size_t j = 0; j < n; j++) printf(" %02X", stub[j]);
            printf("\n");
            /* the block's jbe must land at or before the gate's target, and after the subss */
            uint8_t *jbe = s->sub[k] - 6, *t = jbe + 6 + *(int32_t *)(jbe + 2);
            if (t <= s->sub[k] || t > s->gate + 6 + *(int32_t *)(s->gate + 2)) { printf("    FAIL jbe target exe+0x%llX\n", OFF(t)); bad = 1; }
        }
    }
    printf(bad ? "  FAIL\n" : "  OK\n");
    return bad;
}
#endif
