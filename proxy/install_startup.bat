@echo off
set STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
copy "%~dp0start.bat" "%STARTUP%\claude-proxy.bat"
echo Installed to startup folder
pause
