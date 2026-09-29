@echo off
cd /d "%~dp0"

echo Compiling server_viewer.cpp...
g++ server_viewer.cpp -o server_viewer.exe -O2 -lws2_32 -lgdi32 -luser32

if %ERRORLEVEL% EQU 0 (
    echo Build successful! Launching Server Viewer...
    server_viewer.exe
) else (
    echo [ERROR] Compilation failed. Check the errors above.
    pause
)