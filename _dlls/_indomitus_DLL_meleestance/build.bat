@echo off
rem GEM MeleeStance build - Indomitus Launcher payload dll
rem   build.bat          -> dist\gem_meleestance.dll + dist\gem_meleestance.ini
rem   build.bat payload  -> the above, then copies both into ..\..\launcher\payload\
rem   build.bat test     -> dist\ms_test.exe, run on the game / editor / server exe (offline finders + filter/charge cases)
rem                         folder: 2nd argument, else %%GOH_BIN%%, else the D:\SteamLibrary install
rem zig: ..\..\zig\zig.exe (the launcher repo's portable zig), else zig on PATH, else python -m ziglang
rem Everything is also written to build_log.txt next to this file.
setlocal EnableExtensions
cd /d "%~dp0"
set "LOG=%~dp0build_log.txt"
> "%LOG%" echo GEM MeleeStance build %DATE% %TIME% [%*]
if not exist dist mkdir dist

set "CCX="
if exist "%~dp0..\..\zig\zig.exe" set CCX="%~dp0..\..\zig\zig.exe" cc
if not defined CCX (where zig >nul 2>&1 && set CCX=zig cc)
if not defined CCX (python -c "import ziglang" >nul 2>&1 && set CCX=python -m ziglang cc)
if not defined CCX (
  >> "%LOG%" echo ERROR: no zig found ^(..\..\zig\zig.exe, PATH, python -m ziglang^)
  goto :fail
)
rem same flags as every other payload dll (no AVX/FMA, identical results on every CPU). Do not change these.
set CCX=%CCX% -target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math
>> "%LOG%" echo compiler: %CCX%

if /i "%~1"=="test" goto :test

call :cc -shared -s -o dist\gem_meleestance.dll src\gem_meleestance.c || goto :fail
copy /y src\gem_meleestance.ini dist\ >nul || goto :fail
>> "%LOG%" echo built dist\gem_meleestance.dll + dist\gem_meleestance.ini
if /i not "%~1"=="payload" goto :ok

set "PAY=%~dp0..\..\launcher\payload"
if not exist "%PAY%\" (
  >> "%LOG%" echo ERROR: launcher payload folder not found: %PAY%
  goto :fail
)
copy /y dist\gem_meleestance.dll "%PAY%\" >nul || goto :fail
copy /y dist\gem_meleestance.ini "%PAY%\" >nul || goto :fail
>> "%LOG%" echo copied into %PAY% - now rebuild the launcher ^(build_zig.bat^)
goto :ok

:test
set "BIN=%GOH_BIN%"
if not "%~2"=="" set "BIN=%~2"
if "%BIN%"=="" set "BIN=D:\SteamLibrary\steamapps\common\Call to Arms - Gates of Hell\binaries\x64"
call :cc -DMS_TEST -o dist\ms_test.exe src\gem_meleestance.c || goto :fail
>> "%LOG%" echo running on %BIN%
dist\ms_test.exe "%BIN%\call_to_arms.exe" "%BIN%\call_to_arms_ed.exe" "%BIN%\call_to_arms_server.exe" >> "%LOG%" 2>&1 || goto :fail
goto :ok

:cc
%CCX% %* >> "%LOG%" 2>&1 && exit /b 0
rem a stale zig global cache (objects deleted under %LocalAppData%\zig) fails like this; retry once
>> "%LOG%" echo compile failed, retrying with a local zig cache ^(.zig-cache^)
set "ZIG_GLOBAL_CACHE_DIR=%~dp0.zig-cache"
%CCX% %* >> "%LOG%" 2>&1
exit /b %errorlevel%

:ok
>> "%LOG%" echo OK
type "%LOG%"
if not defined NOPAUSE pause
exit /b 0

:fail
>> "%LOG%" echo FAILED
type "%LOG%"
if not defined NOPAUSE pause
exit /b 1
