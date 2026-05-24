@echo off
setlocal

set STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
set TARGET=%STARTUP%\claude-proxy.vbs
set OLD_TARGET=%STARTUP%\claude-proxy.bat
set PROXY_DIR=%~dp0

rem Strip trailing backslash from PROXY_DIR
set PROXY_DIR=%PROXY_DIR:~0,-1%
set EXE=%PROXY_DIR%\target\release\claude-proxy.exe

rem === Build the proxy first if release binary does not exist ===
if not exist "%EXE%" (
    echo Release binary not found. Building...
    pushd "%PROXY_DIR%"
    cargo build --release
    popd
    if not exist "%EXE%" (
        echo ERROR: build failed.
        pause
        exit /b 1
    )
)

rem === Remove any old .bat launcher from previous install ===
if exist "%OLD_TARGET%" del "%OLD_TARGET%"

rem === Write a VBS that launches the exe completely hidden (window style 0) ===
> "%TARGET%" echo Set objShell = CreateObject("WScript.Shell")
>>"%TARGET%" echo objShell.Run """%EXE%""", 0, False
>>"%TARGET%" echo Set objShell = Nothing

echo.
echo Installed VBS launcher: %TARGET%
echo Proxy exe:              %EXE%
echo.
echo The proxy will start COMPLETELY HIDDEN (no window) on next login.
echo Test now without rebooting:
echo   wscript "%TARGET%"
echo   curl http://localhost:8765/usage
echo.
pause
