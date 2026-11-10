@echo off
rem GEM UserControl build (zig cc via pip: pip install ziglang; or set ZIG_EXE=<path>\zig.exe)
rem   build.bat        -> out\gem_usercontrol.dll (+ gem_inject.exe copied from gem_meleestance\out)
rem   build.bat test   -> out\uc_test.exe, finders on the three game exes (one exe per process) + logic tests
setlocal
cd /d "%~dp0"
if not exist out mkdir out
set ZIG=python -m ziglang
if defined ZIG_EXE set ZIG="%ZIG_EXE%"
set CC=%ZIG% cc -target x86_64-windows-gnu -mcpu=baseline -O2
if /i "%1"=="test" (
  %CC% -DUC_TEST -o out\uc_test.exe gem_usercontrol.c || exit /b 1
  out\uc_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms.exe" || exit /b 1
  out\uc_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms_ed.exe" || exit /b 1
  out\uc_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms_server.exe" || exit /b 1
  exit /b 0
)
%CC% -shared -s -o out\gem_usercontrol.dll gem_usercontrol.c || exit /b 1
if exist ..\gem_meleestance\out\gem_inject.exe copy /y ..\gem_meleestance\out\gem_inject.exe out\ >nul
copy /y gem_usercontrol.ini out\ >nul
copy /y inject.bat out\ >nul
echo built out\gem_usercontrol.dll
