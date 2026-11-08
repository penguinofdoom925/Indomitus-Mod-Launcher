@echo off
rem GEM ContactDamage 0.1.0 build - Indomitus Launcher payload dll
rem   build.bat          -> dist\gem_contactdamage.dll + dist\gem_contactdamage.ini
rem   build.bat payload  -> the above, then copies both into ..\..\launcher\payload\
rem   build.bat test     -> dist\cd_test.exe, finders + logic cases on the game / editor / server exe
rem                         (binaries\x64 folder: 2nd argument, else GOH_BIN)
rem zig: ..\..\zig\zig.exe, else zig on PATH, else python -m ziglang
rem Flags = the mod-side build that was tested (same as MeleeStance).
setlocal EnableExtensions
cd /d "%~dp0"
if not exist dist mkdir dist
set "CCX="
if exist "%~dp0..\..\zig\zig.exe" set CCX="%~dp0..\..\zig\zig.exe" cc
if not defined CCX (where zig >nul 2>&1 && set CCX=zig cc)
if not defined CCX (python -c "import ziglang" >nul 2>&1 && set CCX=python -m ziglang cc)
if not defined CCX (echo ERROR: no zig found & exit /b 1)
set CCX=%CCX% -target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math

if /i "%~1"=="test" goto :test
%CCX% -shared -s -o dist\gem_contactdamage.dll src\gem_contactdamage.c || exit /b 1
copy /y src\gem_contactdamage.ini dist\ >nul
del dist\*.lib dist\*.pdb >nul 2>&1
echo built dist\gem_contactdamage.dll
if /i not "%~1"=="payload" exit /b 0
copy /y dist\gem_contactdamage.dll ..\..\launcher\payload\ >nul || exit /b 1
copy /y dist\gem_contactdamage.ini ..\..\launcher\payload\ >nul || exit /b 1
echo copied into launcher\payload - now rebuild the launcher
exit /b 0

:test
set "BIN=%GOH_BIN%"
if not "%~2"=="" set "BIN=%~2"
if "%BIN%"=="" (echo pass the binaries\x64 folder or set GOH_BIN & exit /b 1)
%CCX% -DCD_TEST -o dist\cd_test.exe src\gem_contactdamage.c || exit /b 1
dist\cd_test.exe "%BIN%\call_to_arms.exe" "%BIN%\call_to_arms_ed.exe" "%BIN%\call_to_arms_server.exe"
exit /b %errorlevel%
