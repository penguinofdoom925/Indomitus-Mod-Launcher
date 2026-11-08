@echo off
rem GEM SessionDump 0.1 build - Indomitus Launcher payload dll
rem   build.bat          -> dist\gem_sessiondump.dll + dist\gem_sessiondump.ini
rem   build.bat payload  -> the above, then copies both into ..\..\launcher\payload\
rem   build.bat test     -> dist\sd_test.exe: loads the dll into a child process, exits / terminates it,
rem                         checks a dump was written (runs in %TEMP%\sd_test, needs no game)
rem zig: ..\..\zig\zig.exe, else zig on PATH, else python -m ziglang
rem The payload ini ships Enabled=0 (off until tested in game).
setlocal EnableExtensions
cd /d "%~dp0"
if not exist dist mkdir dist
set "CCX="
if exist "%~dp0..\..\zig\zig.exe" set CCX="%~dp0..\..\zig\zig.exe" cc
if not defined CCX (where zig >nul 2>&1 && set CCX=zig cc)
if not defined CCX (python -c "import ziglang" >nul 2>&1 && set CCX=python -m ziglang cc)
if not defined CCX (echo ERROR: no zig found & exit /b 1)
set CCX=%CCX% -target x86_64-windows-gnu -mcpu=baseline -O2

%CCX% -shared -s -o dist\gem_sessiondump.dll src\gem_sessiondump.c || exit /b 1
copy /y src\gem_sessiondump.ini dist\ >nul
del dist\*.lib dist\*.pdb >nul 2>&1
echo built dist\gem_sessiondump.dll
if /i "%~1"=="test" goto :test
if /i not "%~1"=="payload" exit /b 0
copy /y dist\gem_sessiondump.dll ..\..\launcher\payload\ >nul || exit /b 1
copy /y dist\gem_sessiondump.ini ..\..\launcher\payload\ >nul || exit /b 1
echo copied into launcher\payload - now rebuild the launcher
exit /b 0

:test
%CCX% -o dist\sd_test.exe test\sd_test.c || exit /b 1
dist\sd_test.exe
exit /b %errorlevel%
