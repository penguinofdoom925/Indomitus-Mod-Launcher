# GEM SessionDump 0.1

Writes a minidump every time the game, editor or dedicated server ends, crash or not. One file per
exe, overwritten each session. No engine hooks: works on any game version.

| File | When |
|---|---|
| `session.mdmp` + `session.txt` | process end (quit, crash, kill from inside the game) |
| `snapshot.mdmp` + `snapshot.txt` | Ctrl+Alt+F12 with the game window in front (beeps when written) |

Editor files get `_editor`, the server `_server`. Default folder = the engine's own
`%LocalAppData%\digitalmindsoft\gates of hell\log\minidumps` (server `log_ds\minidumps`).
The `.txt` holds start/end time, uptime, how the process ended and the exit code.

**The exit dump is taken after the game has shut its own systems down** (map unloaded, menus gone),
so it shows how the process ended, not the battle. For battle state press Ctrl+Alt+F12 during the
match. A crash dump from the engine's handler is still written as before; the session dump is
written after it (the crash handler ends the process through the same call).

Not caught: Task Manager "End task" / killing the process from outside, power loss.

## ini (`gem_sessiondump.ini`)
`Enabled`, `Level` (0 small ~3 MB like the engine crash dump, 1 default = + globals and memory the
stacks point at, 2 = full memory, several GB), `Type` (raw MINIDUMP_TYPE hex, overrides Level),
`Hotkey`, `TimeoutMs` (max wait at exit, default 30000), `Dir`, `Log` (`gem_sessiondump.log` next to the dll).

## Install
- Launcher: `gem_sessiondump.dll` + `.ini` into `launcher\payload\` (and the `server\` payload for
  a hosted MP server).
- In-house test: `inject.bat`-style, `gem_inject.exe gem_sessiondump.dll` while the game runs.
- Offline test: `build.bat test` (child process loads the dll, ends via ExitProcess and via
  TerminateProcess, both dumps checked).

## How it works
- `ntdll!RtlExitUserProcess` (what ExitProcess calls) gets a 12-byte jump to the dll at its entry,
  before it takes the loader + PEB locks and ends the other threads, so every thread is still
  alive in the dump. Exit is one-way: the hook puts the original bytes back and calls it.
  (Dumping later, at its `NtTerminateProcess(0)`, deadlocks: dbghelp waits for those locks.)
- `ntdll!NtTerminateProcess` gets the same jump (original called through a copy of the syscall
  stub) for TerminateProcess on our own process (-1 / own pid), which skips RtlExitUserProcess.
- Either one wakes a worker thread made at startup, which writes the dump with the System32
  `dbghelp.dll` (the game folder's `dbghelp.dll` is the BreedTags proxy). The exiting thread waits
  at most `TimeoutMs`. If the ExitProcess hook could not be set, the dump is written in
  `DLL_PROCESS_DETACH` instead (other threads already gone).

Offline test passes on Windows 11 (Nov 8 2026): ExitProcess and TerminateProcess both dump.
Not yet run inside the game.
