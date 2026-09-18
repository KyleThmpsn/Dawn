@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-Dawn.ps1" %*
set "result=%errorlevel%"
if not "%result%"=="0" echo Uninstall did not complete. Read the message above.
pause
exit /b %result%
