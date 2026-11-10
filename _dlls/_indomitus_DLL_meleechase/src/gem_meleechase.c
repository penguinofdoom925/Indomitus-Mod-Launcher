/*
 * GEM MeleeChase  -  Call to Arms: Gates of Hell (GEM engine, 1.067)
 *
 * Better chasing for melee fauna / vehicles (vehicle-brain units only):
 *   humans    not touched since 0.5.0. The fight-distance multiplier is removed; infantry melee (stance, draw,
 *             charge, blow reach) is GEM MeleeStance's job, and two DLLs changing the hand-fight distance
 *             worked against each other.
 *   vehicles  fauna / vehicles with a melee-tagged weapon keep re-planning their approach while the target
 *             moves, instead of driving to where the target was when the approach started.
 *   diag      crash log
 *
 * Every address is re-derived at startup from strings, RTTI and byte patterns (nothing is hard-coded per exe);
 * if a finder fails, that feature stays off and gem_meleechase.log says why. Same DLL for the game, the editor
 * and the dedicated server. Injected by the Indomitus launcher. Simulation: every multiplayer peer needs it.
 *
 * Layout (unity build: this file is the only translation unit):
 *   mc/core.c        logging, ini, PE / .pdata / pattern / RTTI helpers    (from GEM Fauna 0.5.1)
 *   mc/patch.c       thread-safe patching: call redirection, vtable hooks  (from GEM Fauna 0.5.1)
 *   mc/melee.c       finders + hooks
 *   mc/crashlog.c    crash / process-exit logging                          (from GEM Fauna 0.5.1)
 *   mc/main.c        init sequence, DllMain
 *   mc/test_main.c   FX_TEST only: offline finder check against exe files (tests/run_offline_check.sh)
 *
 * Build: build.bat (zig cc).  Offline check: sh tests/run_offline_check.sh <game> <editor> <server>
 */
#ifndef FX_TEST
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <stdlib.h>
#else
#include <stdlib.h>
#include <sys/mman.h>
#define _strnicmp strncasecmp
typedef int LONG;
#include <strings.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#define FX_VERSION "0.5.0"
#define FX_NAME    "GEM MeleeChase"
#define FX_INI     L"\\gem_meleechase.ini"
#define FX_LOG     L"\\gem_meleechase.log"
#define FX_SECT    L"meleechase"
#include "mc/core.c"
#include "mc/patch.c"
#include "mc/melee.c"
#include "mc/crashlog.c"
#include "mc/main.c"
#include "mc/test_main.c"
