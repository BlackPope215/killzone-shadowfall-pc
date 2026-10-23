@echo off
rem Opens the classic launcher (the previous window). Use it if the launcher
rem does not open or does not look right on this PC. Same options, saves and cache.
cd /d "%~dp0"
set KZSF_LAUNCHER_UI=tk
start "" "Killzone Shadow Fall.exe"
