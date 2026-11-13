@echo off
rem GEM TeamSound build (zig cc via pip: pip install ziglang)
rem   build.bat        -> out\gem_teamsound.dll + out\gem_inject.exe
rem   build.bat test   -> out\ts_test.exe, run on the three game exes
setlocal
cd /d "%~dp0"
if not exist out mkdir out
set CC=python -m ziglang cc -target x86_64-windows-gnu -mcpu=baseline -O2
if /i "%1"=="test" (
  %CC% -DTS_TEST -o out\ts_test.exe gem_teamsound.c || exit /b 1
  rem one exe per process: a second exe mapped into the same process is not relocated
  out\ts_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms.exe" || exit /b 1
  out\ts_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms_ed.exe" || exit /b 1
  out\ts_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms_server.exe" || exit /b 1
  exit /b 0
)
%CC% -shared -s -o out\gem_teamsound.dll gem_teamsound.c || exit /b 1
%CC% -s -o out\gem_inject.exe -municode ..\gem_squadfollow\gem_inject.c || exit /b 1
copy /y gem_teamsound.ini out\ >nul
echo built out\gem_teamsound.dll out\gem_inject.exe
