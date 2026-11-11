/*
 * GEM WeaponScript 1.1  -  Call to Arms: Gates of Hell (GEM engine)  -  Indomitus Launcher payload dll (one dll, three modules)
 *
 * One dll, three weapon modules (each its own translation unit, see common\gem_common.h):
 *   gem_converge.c     barrel convergence ("converge") + multi-shot ("bullets_per_shot_N")      [converge]
 *   gem_overcharge.c   ammo swap by unit tag (plasma overcharge)                               [overcharge]
 *   gem_guidance.c     fire-and-forget lock-on ("lock_on" + "atgm") + drift ("drift_N")          [guidance]
 *
 * This file is the process-level part: DllMain, the exe filter, the one-copy mutex, the log file
 * (gem_weaponscript.log) and the ini (gem_weaponscript.ini, one section per module + [weaponscript]),
 * then each module's <x>_init() in turn. The modules hook the same engine functions as the three separate
 * dlls did and chain on each other in the same way; the only change is the packaging.
 */
#define GEM_NAME    "GEM WeaponScript"
#define GEM_VERSION "1.1.1"
#define GEM_FILE    L"gem_weaponscript"
#define GEM_SECT    L"weaponscript"
#include "../common/gem_common.h"

int converge_init(void);
int overcharge_init(void);
int guidance_init(void);

/* ---- dll-wide log + ini (the modules' logf_ / ini_* wrappers end up here) ------------- */
static wchar_t g_dir[MAX_PATH];           /* folder of this dll = the launcher's payload folder */
static FILE *g_log;
static int g_console;
static CRITICAL_SECTION g_logLock;

void ws_vlog(const char *name, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d [%s] %s\n", st.wHour, st.wMinute, st.wSecond, name, buf);
    EnterCriticalSection(&g_logLock);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(line);
}

/* <payload folder>\gem_weaponscript.<ext>; 0 = does not fit (g_dir is checked at start-up, so this cannot happen) */
static int ws_path(wchar_t *out, const wchar_t *ext)
{
    int k = _snwprintf(out, MAX_PATH, L"%ls\\" GEM_FILE L"%ls", g_dir, ext);
    if (k <= 0 || k >= MAX_PATH) { out[0] = 0; return 0; }
    return 1;
}
static void ini_path(wchar_t *ini) { ws_path(ini, L".ini"); }

void ws_ini_str(const wchar_t *sect, const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; ini_path(ini);
    wchar_t wkey[64], wdef[256], wout[256];
    MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_ACP, 0, def, -1, wdef, 256);
    GetPrivateProfileStringW(sect, wkey, wdef, wout, 256, ini);
    if (!WideCharToMultiByte(CP_ACP, 0, wout, -1, out, (int)n, NULL, NULL)) {   /* does not fit: keep the default */
        snprintf(out, n, "%s", def);
        logf_("[%ls] %s= is longer than %u characters - using the default \"%s\"", sect, key, (unsigned)(n - 1), def);
    }
    out[n - 1] = 0;
}

int ws_ini_int(const wchar_t *sect, const char *key, int def)
{
    wchar_t ini[MAX_PATH]; ini_path(ini);
    wchar_t wkey[64]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(sect, wkey, def, ini);
}

/* ---- process-level guards ------------------------------------------------------------- */
/* The old all-in-one GEM Converge (<= 0.5.1, a VCOMP140.dll proxy in binaries\x64) carries every feature of this
   dll; running both would apply each effect twice. Its log prefix "[GEM Converge]" is in its image. */
static int old_combined_loaded(void)
{
    HMODULE m = GetModuleHandleW(L"vcomp140.dll");
    if (!m) return 0;
    wchar_t p[MAX_PATH], sys[MAX_PATH];
    if (!GetModuleFileNameW(m, p, MAX_PATH)) return 0;
    UINT ns = GetSystemDirectoryW(sys, MAX_PATH);
    if (ns && !_wcsnicmp(p, sys, ns)) return 0;                          /* the real OpenMP runtime */
    uint8_t *b = (uint8_t *)m;
    uint32_t pe = *(uint32_t *)(b + 0x3C);
    uint8_t *nt = b + pe;
    uint16_t nsec = *(uint16_t *)(nt + 6), optSize = *(uint16_t *)(nt + 20);
    PeSec *sh = (PeSec *)(nt + 24 + optSize);
    static const char k[] = "[GEM Converge]";
    for (int i = 0; i < nsec; i++)
        if ((sh[i].flags & PE_SCN_MEM_READ) && find_bytes(b + sh[i].va, sh[i].vsize, (const uint8_t *)k, sizeof k - 1))
            return 1;
    return 0;
}

/* the 1.0 split dlls (one per module) must not sit in the payload next to this one */
static const wchar_t *split_dll_loaded(void)
{
    static const wchar_t *k[] = { L"gem_converge.dll", L"gem_overcharge.dll", L"gem_guidance.dll" };
    for (int i = 0; i < 3; i++) if (GetModuleHandleW(k[i])) return k[i];
    return NULL;
}

/* one copy per process, even if the launcher injects the dll twice from different paths */
static int first_instance(void)
{
    wchar_t name[96];
    swprintf(name, 96, L"Local\\" GEM_FILE L"_%lu", GetCurrentProcessId());
    HANDLE h = CreateMutexW(NULL, FALSE, name);
    return h && GetLastError() != ERROR_ALREADY_EXISTS;
}

/* folder, exe filter, [weaponscript] Enabled, mutex, log file, console. 0 = do nothing in this process. */
static int ws_open(HMODULE self)
{
    DWORD dl = GetModuleFileNameW(self, g_dir, MAX_PATH);
    if (!dl || dl >= MAX_PATH) return 0;                        /* path truncated: no ini / log possible, stay out */
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;
    wchar_t probe[MAX_PATH];
    if (!ws_path(probe, L".ini")) return 0;
    static wchar_t exe[MAX_PATH];
    DWORD el = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!el || el >= MAX_PATH) return 0;
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return 0;          /* only the game / editor / server exes */
    if (!ini_int("Enabled", 1)) return 0;
    if (!first_instance()) return 0;
    g_console = ini_int("Console", 0);
    if (ini_int("LogFile", 1)) {
        wchar_t lp[MAX_PATH];
        if (ws_path(lp, L".log")) g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(GEM_NAME); freopen("CONOUT$", "w", stdout); }
    if (!get_sections((uint8_t *)GetModuleHandleW(NULL))) { logf_("not a PE I understand"); return 0; }
    logf_(GEM_NAME " " GEM_VERSION " loaded into %ls (timestamp %08X, exe at %p, %s unwind entries) from %ls", en, g_timestamp, (void *)g_base, g_pdata ? "with" : "WITHOUT", g_dir);
    if (old_combined_loaded()) {
        logf_("the old all-in-one GEM Converge VCOMP140.dll is loaded in this process - doing nothing (remove it from binaries\\x64: it also loads for Steam-started games)");
        return 0;
    }
    const wchar_t *split = split_dll_loaded();
    if (split) {
        logf_("the 1.0 split dll %ls is loaded in this process - doing nothing (remove the three split dlls + inis from launcher\\payload\\)", split);
        return 0;
    }
    return 1;
}

__declspec(dllexport) const char *GemHookVersion(void) { return GEM_NAME " " GEM_VERSION; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        if (ws_open(h)) {
            int a = converge_init();
            int b = overcharge_init();
            int c = guidance_init();
            logf_("modules: convergence / multi-shot %s, overcharge %s, lock-on / drift %s", a ? "ON" : "OFF", b ? "ON" : "OFF", c ? "ON" : "OFF");
            if (a || b || c) {
                /* the engine now jumps into this dll: it must never be unloaded */
                HMODULE pin;
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void *)&DllMain, &pin))
                    logf_("could not pin the dll in the process (%lu)", GetLastError());
            }
        }
    }
    return TRUE;
}
