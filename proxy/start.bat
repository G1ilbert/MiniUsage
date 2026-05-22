@echo off
cd /d %~dp0
cargo build --release 2>nul
rem CLAUDE_CREDENTIALS_PATH defaults to %USERPROFILE%\.claude\.credentials.json
rem ตั้งให้ override ถ้าต้องการชี้ไปที่อื่น เช่น:
rem set CLAUDE_CREDENTIALS_PATH=C:\path\to\.credentials.json
start /min "" target\release\claude-proxy.exe
