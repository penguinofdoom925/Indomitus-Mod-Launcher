@echo off
rem GEM CostRed build (zig cc: pip install ziglang, or set ZIG_EXE to a zig.exe, e.g. the launcher repo's zig\zig.exe)
rem   build.bat        -> out\gem_costred.dll (+ gem_inject.exe copied from gem_meleestance\out)
rem   build.bat test   -> out\cr_test.exe, the finder on the three game exes (game: 2 functions, editor / server: none)
setlocal
cd /d "%~dp0"
if not exist out mkdir out
set ZIG=python -m ziglang
if defined ZIG_EXE set ZIG="%ZIG_EXE%"
set CC=%ZIG% cc -target x86_64-windows-gnu -mcpu=baseline -O2
if /i "%1"=="test" (
  %CC% -DCR_TEST -o out\cr_test.exe gem_costred.c || exit /b 1
  out\cr_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms.exe" 2 || exit /b 1
  out\cr_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms_ed.exe" 0 || exit /b 1
  out\cr_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms_server.exe" 0 || exit /b 1
  exit /b 0
)
%CC% -shared -s -o out\gem_costred.dll gem_costred.c || exit /b 1
if exist ..\gem_meleestance\out\gem_inject.exe copy /y ..\gem_meleestance\out\gem_inject.exe out\ >nul
copy /y gem_costred.ini out\ >nul
copy /y inject.bat out\ >nul
echo built out\gem_costred.dll
