@echo off
cd /d C:\Dev\fgvk
cmake -S C:\Dev\fgvk -B C:\Dev\fgvk\build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build C:\Dev\fgvk\build --config Release
