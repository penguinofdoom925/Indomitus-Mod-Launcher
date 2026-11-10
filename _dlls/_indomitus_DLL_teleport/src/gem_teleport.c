/*
 * GEM Teleport  -  Call to Arms: Gates of Hell (GEM engine, 1.067)
 *
 * Adds the {teleport ...} instruction to the custom interface actions (the /interface/action .set files, the
 * buttons of the control panel). A soldier given such an action is locked (orders refused, damage
 * ignored), waits pre_use_delay, is moved to the clicked point, waits post_use_delay and is released.
 * The click is refused (forbidden cursor) outside max_radius; {target ground} already refuses a click
 * on an entity (house, wall, vehicle, tree...), so the landing point is never inside an object.
 *
 *   {action
 *       {target ground}
 *       {button "jetpack"} {item "jetpack_ability"} {item_in_box} {NotifySelect}
 *       {instructions
 *           {teleport {max_radius 40} {pre_use_delay 0.8} {post_use_delay 0.6}}
 *       }
 *   }
 *
 * Visuals are mod data: the DLL fires the interaction events {on "teleport_begin"} (lock),
 * {on "teleport_jump"} (just after the move) and {on "teleport_end"} (release) on the soldier, and the
 * action file can hold {fsm_request}/{animations} instructions before and after {teleport}.
 *
 * Every address is re-derived at startup from strings, RTTI and byte patterns (nothing is hard-coded
 * per exe); if a finder fails the feature stays off and gem_teleport.log says why. The same DLL works in
 * the game, the editor and the dedicated server. Injected by the Indomitus launcher (no dbghelp proxy).
 * It changes the simulation: in multiplayer every peer must run it.
 *
 * Layout (unity build: this file is the only translation unit; the order below is the dependency order):
 *   tp/core.c        logging, ini, PE / .pdata / pattern / RTTI helpers        (same as GEM Fauna)
 *   tp/patch.c       thread-safe code patching, inline hooks, vtable hooks     (same as GEM Fauna)
 *   tp/engine.c      finders: instruction registry, reader API, orders, actions, SetPos, GiveOrder, damage
 *   tp/teleport.c    the {teleport} instruction, the teleport sub-order, the lock, the hooks
 *   tp/crashlog.c    crash / process-exit logging                              (same as GEM Fauna)
 *   tp/main.c        init sequence, registry watch thread, DllMain
 *   tp/test_main.c   TP_TEST only: offline finder check against exe files (tests/run_offline_check.sh)
 *
 * Build: build.bat (zig cc).  Offline check: sh tests/run_offline_check.sh <game> <editor> <server>
 */
#ifndef TP_TEST
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <stdlib.h>
#else
#include <stdlib.h>
#include <sys/mman.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define FX_VERSION "0.2.12"
#define FX_NAME    "GEM Teleport"
#define FX_INI     L"\\gem_teleport.ini"
#define FX_LOG     L"\\gem_teleport.log"
#define FX_SECT    L"teleport"
#ifdef TP_TEST
#define FX_TEST
#endif
#include "tp/core.c"
#include "tp/patch.c"
#include "tp/engine.c"
#include "tp/teleport.c"
#include "tp/crashlog.c"
#include "tp/main.c"
#include "tp/test_main.c"
