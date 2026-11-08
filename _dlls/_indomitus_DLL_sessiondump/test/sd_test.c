/* sd_test: copies the dll into %TEMP%\sd_test with an ini pointing Dir there, then runs itself as a
   child that loads the dll and ends via ExitProcess(7) and via TerminateProcess(self, 9);
   checks session.mdmp + session.txt after each. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static char dir[MAX_PATH], dll[MAX_PATH];

static int run_child(const char *mode, unsigned want)
{
    char me[MAX_PATH], cmd[MAX_PATH * 2], f[MAX_PATH];
    GetModuleFileNameA(NULL, me, MAX_PATH);
    snprintf(f, sizeof f, "%s\\session.mdmp", dir); DeleteFileA(f);
    snprintf(f, sizeof f, "%s\\session.txt", dir); DeleteFileA(f);
    snprintf(cmd, sizeof cmd, "\"%s\" %s \"%s\"", me, mode, dll);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("spawn failed\n"); return 1; }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    snprintf(f, sizeof f, "%s\\session.mdmp", dir);
    WIN32_FILE_ATTRIBUTE_DATA a; int have = GetFileAttributesExA(f, GetFileExInfoStandard, &a);
    char want_s[32]; snprintf(want_s, sizeof want_s, "exitcode 0x%08X", want);
    char txt[2048] = ""; snprintf(f, sizeof f, "%s\\session.txt", dir);
    FILE *t = fopen(f, "r"); if (t) { txt[fread(txt, 1, sizeof txt - 1, t)] = 0; fclose(t); }
    int ok = have && a.nFileSizeLow > 1000 && code == want && strstr(txt, want_s);
    printf("%-10s exit %lu, dump %s %lu bytes, txt %s -> %s\n", mode, code, have ? "yes" : "NO",
           have ? a.nFileSizeLow : 0, strstr(txt, want_s) ? "ok" : "BAD", ok ? "OK" : "FAIL");
    if (!ok) printf("%s\n", txt);
    return !ok;
}

int main(int argc, char **argv)
{
    if (argc == 3) {                            /* child */
        if (!LoadLibraryA(argv[2])) { printf("child: load failed %lu\n", GetLastError()); return 99; }
        Sleep(500);
        if (!strcmp(argv[1], "exit")) ExitProcess(7);
        TerminateProcess(GetCurrentProcess(), 9);
        return 98;
    }
    char tmp[MAX_PATH], src[MAX_PATH], ini[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    snprintf(dir, sizeof dir, "%ssd_test", tmp); CreateDirectoryA(dir, NULL);
    GetModuleFileNameA(NULL, src, MAX_PATH); *strrchr(src, '\\') = 0;
    strcat(src, "\\gem_sessiondump.dll");
    snprintf(dll, sizeof dll, "%s\\gem_sessiondump.dll", dir);
    if (!CopyFileA(src, dll, FALSE)) { printf("copy failed\n"); return 1; }
    snprintf(ini, sizeof ini, "%s\\gem_sessiondump.ini", dir);
    FILE *f = fopen(ini, "w");
    fprintf(f, "[SessionDump]\nEnabled=1\nLevel=1\nHotkey=0\nDir=%s\nLog=1\n", dir); fclose(f);
    int bad = run_child("exit", 7) | run_child("terminate", 9);
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
