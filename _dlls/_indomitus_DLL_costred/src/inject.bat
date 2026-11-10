@echo off
rem In-house test: inject gem_costred.dll into the running game (main menu, before a match).
rem Result line in gem_costred.log next to the dll.
cd /d "%~dp0"
if not exist gem_inject.exe cd out
gem_inject.exe gem_costred.dll
pause
