@echo off
rem GEM SensorTags 0.3 build - Indomitus Launcher payload dll
rem   build.bat          -> dist\gem_sensortags.dll + dist\gem_sensortags.ini
rem   build.bat payload  -> the above, then copies both into ..\..\launcher\payload\
rem   build.bat test     -> dist\st_test.exe, finders on the game / editor / server exe (one exe per run)
rem                         (binaries\x64 folder: 2nd argument, else GOH_BIN)
rem zig: ..\..\zig\zig.exe, else zig on PATH, else python -m ziglang
rem Flags = the mod-side build that was tested in game (this dll does no float math).
setlocal EnableExtensions
cd /d "%~dp0"
if not exist dist mkdir dist
set "CCX="
if exist "%~dp0..\..\zig\zig.exe" set CCX="%~dp0..\..\zig\zig.exe" cc
if not defined CCX (where zig >nul 2>&1 && set CCX=zig cc)
if not defined CCX (python -c "import ziglang" >nul 2>&1 && set CCX=python -m ziglang cc)
if not defined CCX (echo ERROR: no zig found & exit /b 1)
set CCX=%CCX% -target x86_64-windows-gnu -mcpu=baseline -O2 -fno-fast-math

if /i "%~1"=="test" goto :test
%CCX% -shared -s -o dist\gem_sensortags.dll src\gem_sensortags.c || exit /b 1
copy /y src\gem_sensortags.ini dist\ >nul
del dist\*.lib dist\*.pdb >nul 2>&1
echo built dist\gem_sensortags.dll
if /i not "%~1"=="payload" exit /b 0
copy /y dist\gem_sensortags.dll ..\..\launcher\payload\ >nul || exit /b 1
copy /y dist\gem_sensortags.ini ..\..\launcher\payload\ >nul || exit /b 1
echo copied into launcher\payload - now rebuild the launcher
exit /b 0

:test
set "BIN=%GOH_BIN%"
if not "%~2"=="" set "BIN=%~2"
if "%BIN%"=="" (echo pass the binaries\x64 folder or set GOH_BIN & exit /b 1)
%CCX% -DST_TEST -o dist\st_test.exe src\gem_sensortags.c || exit /b 1
dist\st_test.exe "%BIN%\call_to_arms.exe" || exit /b 1
dist\st_test.exe "%BIN%\call_to_arms_ed.exe" || exit /b 1
dist\st_test.exe "%BIN%\call_to_arms_server.exe" || exit /b 1
exit /b 0
