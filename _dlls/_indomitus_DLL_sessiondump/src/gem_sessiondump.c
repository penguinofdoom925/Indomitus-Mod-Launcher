/* GEM SessionDump 0.1 - write a minidump every time the game / editor / server process ends
 * (Call to Arms: Gates of Hell). No engine hooks, works on any game version.
 *
 * - Exit dump: ntdll!NtTerminateProcess is hooked. The first call that ends this process (handle 0,
 *   -1 or our own pid; ExitProcess, TerminateProcess, the engine's crash handler, all go through it)
 *   writes <Dir>\session[_editor|_server].mdmp + .txt before anything is torn down by the kernel.
 *   ExitProcess calls it first with handle 0 ("kill the other threads"), so every thread is still
 *   alive in the dump. Overwritten every session (written to .tmp, then renamed).
 * - Snapshot: Ctrl+Alt+F12 while the game window is in front writes snapshot[_editor].mdmp
 *   (mid-match state; the exit dump comes after the game has already left the map). Beeps when done.
 * - The dump is written by a worker thread made at startup (MiniDumpWriteDump should not dump its
 *   own calling thread); the exiting thread waits for it at most TimeoutMs, then lets the exit go on.
 * - dbghelp: the game folder's dbghelp.dll is the BreedTags proxy, so the real one is loaded from
 *   System32 by full path.
 * - Fallback when the hook could not be set: dump in DLL_PROCESS_DETACH (other threads already gone).
 *
 * Build: build.bat
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define SD_VERSION "0.1.2"
#define SD_SECT    "SessionDump"

/* ---------------------------------------------------------------- config */
static int   cfg_enabled = 1, cfg_level = 1, cfg_hotkey = 1, cfg_timeout = 30000, cfg_log = 1;
static DWORD cfg_type = 0;                    /* raw MINIDUMP_TYPE override, 0 = from Level */
static char  cfg_dir[MAX_PATH];

static char  g_suffix[16];                    /* "", "_editor", "_server" */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static SYSTEMTIME g_start;
static DWORD g_start_tick;

static void logf_(const char *fmt, ...)
{
    if (!cfg_log) return;
    if (!g_log && g_logpath[0]) g_log = fopen(g_logpath, "w");
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

/* ---------------------------------------------------------------- dump */
typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
static MiniDumpWriteDump_t pMiniDumpWriteDump;

static DWORD dump_type(void)
{
    if (cfg_type) return cfg_type;
    DWORD t = MiniDumpNormal | MiniDumpWithHandleData | MiniDumpWithUnloadedModules
            | MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo;
    if (cfg_level >= 1) t |= MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory;
    if (cfg_level >= 2) t |= MiniDumpWithFullMemory;
    return t;
}

/* one request at a time: kind 0 = exit, 1 = snapshot */
static HANDLE g_req, g_done;
static volatile LONG g_kind, g_exit_code, g_exit_taken;
static const char *g_reason = "";

static void write_dump(int kind)
{
    if (!pMiniDumpWriteDump) { logf_("no MiniDumpWriteDump, skipped"); return; }
    char base[MAX_PATH], tmp[MAX_PATH], fin[MAX_PATH], txt[MAX_PATH];
    snprintf(base, sizeof base, "%s\\%s%s", cfg_dir, kind ? "snapshot" : "session", g_suffix);
    snprintf(tmp, sizeof tmp, "%s.mdmp.tmp", base);
    snprintf(fin, sizeof fin, "%s.mdmp", base);
    snprintf(txt, sizeof txt, "%s.txt", base);

    DWORD t0 = GetTickCount();
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { logf_("cannot create %s (%lu)", tmp, GetLastError()); return; }
    BOOL ok = pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                 (MINIDUMP_TYPE)dump_type(), NULL, NULL, NULL);
    DWORD err = ok ? 0 : GetLastError();
    LARGE_INTEGER sz = {0}; GetFileSizeEx(f, &sz);
    CloseHandle(f);
    if (!ok) { logf_("MiniDumpWriteDump failed 0x%08lX", err); DeleteFileA(tmp); return; }
    MoveFileExA(tmp, fin, MOVEFILE_REPLACE_EXISTING);

    SYSTEMTIME now; GetLocalTime(&now);
    DWORD up = (GetTickCount() - g_start_tick) / 1000;
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    FILE *t = fopen(txt, "w");
    if (t) {
        fprintf(t, "GEM SessionDump %s\nexe      %s\npid      %lu\nstarted  %04d-%02d-%02d %02d:%02d:%02d\n"
                   "dumped   %04d-%02d-%02d %02d:%02d:%02d\nuptime   %lu:%02lu:%02lu\nreason   %s\n",
                SD_VERSION, exe, GetCurrentProcessId(),
                g_start.wYear, g_start.wMonth, g_start.wDay, g_start.wHour, g_start.wMinute, g_start.wSecond,
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                up / 3600, up / 60 % 60, up % 60, kind ? "snapshot (Ctrl+Alt+F12)" : g_reason);
        if (!kind) fprintf(t, "exitcode 0x%08lX\n", (DWORD)g_exit_code);
        fprintf(t, "type     0x%08lX\nsize     %lld bytes\n", dump_type(), (long long)sz.QuadPart);
        fclose(t);
    }
    logf_("%s written, %lld MB in %lu ms", fin, (long long)(sz.QuadPart >> 20), GetTickCount() - t0);
}

static int own_window_in_front(void)
{
    HWND w = GetForegroundWindow(); DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static void load_dbghelp(void);
static DWORD WINAPI worker(LPVOID p)
{
    (void)p;
    load_dbghelp();
    int held = 0, reg = 0;
    /* 0.1.1: a registered hotkey (WM_HOTKEY in this thread's queue). The 0.1 GetAsyncKeyState poll never saw Ctrl+Alt+F12 in the
     * editor (Alex, Nov 8 2026: no snapshot, no log line; inferred: the engine's raw keyboard input leaves the async key state
     * stale). The poll stays as a fallback when the combination is already taken by another program. */
    if (cfg_hotkey) {
        MSG m; PeekMessageA(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);           /* make sure this thread has a message queue */
        reg = RegisterHotKey(NULL, 0x5D01, MOD_CONTROL | MOD_ALT | 0x4000 /* MOD_NOREPEAT */, VK_F12);
        if (reg) logf_("hotkey Ctrl+Alt+F12 registered");
        else logf_("hotkey Ctrl+Alt+F12 NOT registered (error %lu, taken by another program?) - falling back to key polling", GetLastError());
    }
    for (;;) {
        DWORD r = cfg_hotkey ? MsgWaitForMultipleObjects(1, &g_req, FALSE, 100, QS_HOTKEY | QS_POSTMESSAGE)
                             : WaitForSingleObject(g_req, INFINITE);
        if (r == WAIT_OBJECT_0) { write_dump(g_kind); SetEvent(g_done); continue; }
        if (!cfg_hotkey) continue;
        MSG m; int hit = 0;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE))
            if (m.message == WM_HOTKEY && m.wParam == 0x5D01) hit = 1;
        if (hit) {
            if (own_window_in_front()) { write_dump(1); MessageBeep(MB_OK); }
            else logf_("hotkey pressed with another program in front, no snapshot");
        }
        if (!reg) {                                                             /* fallback: poll */
            int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000)
                    && (GetAsyncKeyState(VK_F12) & 0x8000);
            if (down && !held && own_window_in_front()) { write_dump(1); MessageBeep(MB_OK); }
            held = down;
        }
    }
}

/* exit path: hand the dump to the worker, wait for it (bounded). A second thread ending the process
   meanwhile waits for the same dump (g_finished) instead of killing it half-written. */
static HANDLE g_finished;
static void exit_dump(const char *reason, LONG code)
{
    if (InterlockedExchange(&g_exit_taken, 1)) { WaitForSingleObject(g_finished, cfg_timeout); return; }
    g_reason = reason; g_exit_code = code;
    logf_("process ending (%s, code 0x%08lX)", reason, (DWORD)code);
    g_kind = 0;
    SetEvent(g_req);
    if (WaitForSingleObject(g_done, cfg_timeout) != WAIT_OBJECT_0) logf_("dump timed out after %d ms", cfg_timeout);
    SetEvent(g_finished);
}

static void *make_jump(uint8_t j[12], void *to)
{
    uint64_t a = (uint64_t)(uintptr_t)to;        /* mov rax,imm64 ; jmp rax */
    j[0] = 0x48; j[1] = 0xB8; memcpy(j + 2, &a, 8); j[10] = 0xFF; j[11] = 0xE0;
    return j;
}
static int patch(uint8_t *at, const uint8_t *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

/* ---------------------------------------------------------------- RtlExitUserProcess hook
   ExitProcess -> ntdll!RtlExitUserProcess, which takes the loader + PEB locks and then calls
   NtTerminateProcess(0) to end the other threads. dbghelp needs those locks, so the dump has to be
   written before: at the entry of RtlExitUserProcess. Exit is one-way, so the hook just puts the
   original bytes back and calls it (no trampoline). */
typedef void (NTAPI *RtlExitUserProcess_t)(LONG);
static uint8_t *g_rtlexit, g_rtlexit_orig[12];
static CRITICAL_SECTION g_cs;
static volatile LONG g_rtlexit_restored;

static void NTAPI hk_exit(LONG code)
{
    exit_dump("exit (ExitProcess)", code);
    EnterCriticalSection(&g_cs);
    if (!g_rtlexit_restored) { patch(g_rtlexit, g_rtlexit_orig, 12); g_rtlexit_restored = 1; }
    LeaveCriticalSection(&g_cs);
    ((RtlExitUserProcess_t)(void *)g_rtlexit)(code);
}

/* ---------------------------------------------------------------- NtTerminateProcess hook
   TerminateProcess on our own process (crash handlers) never passes RtlExitUserProcess. */
typedef LONG (NTAPI *NtTerminateProcess_t)(HANDLE, LONG);
static NtTerminateProcess_t orig_term;        /* trampoline: mov r10,rcx ; mov eax,N ; syscall ; ret */

static LONG NTAPI hk_term(HANDLE h, LONG code)
{
    /* handle 0 = RtlExitUserProcess ending the other threads, locks held: never dump there
       (that path is covered by hk_exit, or by the DLL_PROCESS_DETACH fallback) */
    if (!g_exit_taken && h && (h == (HANDLE)(LONG_PTR)-1 || GetProcessId(h) == GetCurrentProcessId()))
        exit_dump("terminate (TerminateProcess on self)", code);
    return orig_term(h, code);
}

static int install_hook(void)
{
    uint8_t j[12];
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    InitializeCriticalSection(&g_cs);
    g_rtlexit = nt ? (uint8_t *)GetProcAddress(nt, "RtlExitUserProcess") : NULL;
    if (g_rtlexit) {
        memcpy(g_rtlexit_orig, g_rtlexit, 12);
        if (patch(g_rtlexit, make_jump(j, hk_exit), 12)) logf_("RtlExitUserProcess hooked");
        else g_rtlexit = NULL;
    }
    if (!g_rtlexit) logf_("RtlExitUserProcess not hooked, exit dump falls back to process detach");

    uint8_t *s = nt ? (uint8_t *)GetProcAddress(nt, "NtTerminateProcess") : NULL;
    if (!s) return 0;
    /* 4C 8B D1 = mov r10,rcx ; B8 imm32 = mov eax,<syscall number> */
    if (s[0] != 0x4C || s[1] != 0x8B || s[2] != 0xD1 || s[3] != 0xB8) {
        logf_("NtTerminateProcess stub unknown (%02X %02X %02X %02X), not hooked", s[0], s[1], s[2], s[3]);
        return 0;
    }
    uint32_t num = *(uint32_t *)(s + 4);
    uint8_t *tr = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    uint8_t t[] = { 0x4C, 0x8B, 0xD1, 0xB8, 0, 0, 0, 0, 0x0F, 0x05, 0xC3 };
    memcpy(t + 4, &num, 4); memcpy(tr, t, sizeof t);
    orig_term = (NtTerminateProcess_t)(void *)tr;
    if (!patch(s, make_jump(j, hk_term), 12)) return 0;
    logf_("NtTerminateProcess hooked (syscall 0x%X)", num);
    return 1;
}

/* ---------------------------------------------------------------- setup */
/* The game's config root is Documents\\my games\\gates of hell on some PCs and
   %LocalAppData%\\digitalmindsoft\\gates of hell on others; the unused one can hold a stale copy.
   Take the root whose log\\game.log was written last (the server writes log_ds under the same root).
   Runs inside DllMain: registry + env only (no shell32). */
static ULONGLONG file_mtime(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

static void game_root(char *out, size_t cap)
{
    char la[MAX_PATH] = "", docs[MAX_PATH] = "", raw[MAX_PATH], f[MAX_PATH + 32];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la)) la[0] = 0;
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        DWORD sz = sizeof raw - 1, type = 0;
        if (RegQueryValueExA(k, "Personal", NULL, &type, (BYTE *)raw, &sz) == ERROR_SUCCESS) {
            raw[sz < sizeof raw ? sz : sizeof raw - 1] = 0;
            if (!ExpandEnvironmentStringsA(raw, docs, sizeof docs)) docs[0] = 0;
        }
        RegCloseKey(k);
    }
    char a[MAX_PATH] = "", b[MAX_PATH] = "";
    if (docs[0]) snprintf(a, sizeof a, "%s\\my games\\gates of hell", docs);
    snprintf(b, sizeof b, "%s\\digitalmindsoft\\gates of hell", la);
    ULONGLONG ta = 0, tb;
    if (a[0]) { snprintf(f, sizeof f, "%s\\log\\game.log", a); ta = file_mtime(f); }
    snprintf(f, sizeof f, "%s\\log\\game.log", b); tb = file_mtime(f);
    snprintf(out, cap, "%s", (a[0] && ta >= tb && (ta || !tb)) ? a : b);
}

static void load_ini(const char *ini)
{
    cfg_enabled = GetPrivateProfileIntA(SD_SECT, "Enabled", 1, ini);
    cfg_level   = GetPrivateProfileIntA(SD_SECT, "Level", 1, ini);
    cfg_hotkey  = GetPrivateProfileIntA(SD_SECT, "Hotkey", 1, ini);
    cfg_timeout = GetPrivateProfileIntA(SD_SECT, "TimeoutMs", 30000, ini);
    cfg_log     = GetPrivateProfileIntA(SD_SECT, "Log", 1, ini);
    char buf[MAX_PATH];
    GetPrivateProfileStringA(SD_SECT, "Type", "0", buf, sizeof buf, ini);
    cfg_type = strtoul(buf, NULL, 0);
    GetPrivateProfileStringA(SD_SECT, "Dir", "", cfg_dir, sizeof cfg_dir, ini);
    if (!cfg_dir[0]) {
        char root[MAX_PATH];
        game_root(root, sizeof root);
        snprintf(cfg_dir, sizeof cfg_dir, "%s\\%s\\minidumps", root, strcmp(g_suffix, "_server") ? "log" : "log_ds");
    }
}

static void make_dirs(const char *path)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 3; *c; c++)
        if (*c == '\\' || *c == '/') { char k = *c; *c = 0; CreateDirectoryA(p, NULL); *c = k; }
    CreateDirectoryA(p, NULL);
}

static void load_dbghelp(void)
{
    /* real dbghelp from System32 (the game folder one is the BreedTags proxy) */
    char sys[MAX_PATH], dll[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof sys);
    snprintf(dll, sizeof dll, "%s\\dbghelp.dll", sys);
    HMODULE dh = LoadLibraryExA(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dh) dh = GetModuleHandleA("dbghelp.dll");
    if (dh) pMiniDumpWriteDump = (MiniDumpWriteDump_t)(void *)GetProcAddress(dh, "MiniDumpWriteDump");
    logf_("dbghelp %s, MiniDumpWriteDump %s", dll, pMiniDumpWriteDump ? "ok" : "MISSING");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetLocalTime(&g_start); g_start_tick = GetTickCount();
        char path[MAX_PATH], ini[MAX_PATH], exe[MAX_PATH], *nm;
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.'); if (dot) *dot = 0;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        snprintf(g_suffix, sizeof g_suffix, "%s", strstr(nm, "_ed") ? "_editor" : (strstr(nm, "_server") ? "_server" : ""));
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path, g_suffix);
        load_ini(ini);
        if (!cfg_enabled) return TRUE;
        logf_("GEM SessionDump %s in %s, dir %s, type 0x%08lX", SD_VERSION, nm, cfg_dir, dump_type());
        make_dirs(cfg_dir);
        g_req = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_done = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_finished = CreateEventA(NULL, TRUE, FALSE, NULL);
        /* the worker starts running after the loader lock is released, then loads dbghelp */
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL); if (h) CloseHandle(h);
        install_hook();
    } else if (reason == DLL_PROCESS_DETACH && res && cfg_enabled && !g_exit_taken) {
        /* hook missing or bypassed: the worker is gone by now, dump from here */
        g_exit_taken = 1; g_reason = "process detach (fallback, other threads already ended)";
        logf_("process ending (%s)", g_reason);
        write_dump(0);
    }
    return TRUE;
}
