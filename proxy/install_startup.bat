@echo off
setlocal
set STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
set TARGET=%STARTUP%\claude-proxy.bat
set PROXY_DIR=%~dp0

rem Write a Startup bat that explicitly cd's to the proxy folder.
rem (Copying start.bat as-is would break because its %~dp0 would resolve
rem  to the Startup folder, not the proxy folder.)
(
    echo @echo off
    echo cd /d "%PROXY_DIR:~0,-1%"
    echo if not exist target\release\claude-proxy.exe ^(
    echo     cargo build --release 2^>nul
    echo ^)
    echo start /min "" target\release\claude-proxy.exe
) > "%TARGET%"

echo Installed: %TARGET%
echo Pointing at proxy folder: %PROXY_DIR%
pause
