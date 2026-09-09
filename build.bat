@echo off
rem minimon one-click build (requires CMake + MinGW g++ on PATH)
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: cmake not found on PATH.
  goto :fail
)

cmake -B build -G "MinGW Makefiles"
if errorlevel 1 goto :fail

rem Cannot relink while the app runs (Windows file lock).
tasklist /FI "IMAGENAME eq minimon.exe" 2>nul | find /I "minimon.exe" >nul
if not errorlevel 1 (
  echo ERROR: minimon.exe is running. Close it with the q key, then retry.
  goto :fail
)

cmake --build build
if errorlevel 1 goto :fail

echo.
echo BUILD OK: build\minimon.exe
goto :end

:fail
echo.
echo BUILD FAILED

:end
pause
