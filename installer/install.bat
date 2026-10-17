@echo off
rem RE4 Modern Controls installer: double-click to install, or pass -Uninstall / -Defaults.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
