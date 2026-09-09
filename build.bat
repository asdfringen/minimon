@echo off
rem minimon one-click build (auto-detects CMake + MinGW g++ on common paths)
setlocal
cd /d "%~dp0"

call :ensure_cmake
where.exe cmake >nul 2>&1
if errorlevel 1 (
  echo ERROR: cmake not found on PATH.
  echo HINT: install CMake, e.g.  winget install -e --id Kitware.CMake
  echo        or download from https://cmake.org/download/ and check
  echo        "Add CMake to the system PATH".
  echo        You can also set CMAKE_EXE to the full path of cmake.exe, e.g.
  echo          set CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe
  goto :fail
)

call :ensure_mingw
where.exe g++ >nul 2>&1
if errorlevel 1 (
  echo ERROR: g++ not found on PATH. Install MinGW-w64, e.g.
  echo        winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
  goto :fail
)

tasklist /FI "IMAGENAME eq minimon.exe" /V 2>nul | findstr /I "%~dp0build\minimon.exe" >nul
if not errorlevel 1 (
  echo ERROR: build\minimon.exe is running. Close it, then retry.
  goto :fail
)

if not exist "%~dp0build" goto :builddir_ready
rmdir /s /q "%~dp0build" >nul 2>&1
if not exist "%~dp0build" goto :builddir_ready
rem rmdir sometimes reports failure from transient locks (AV/indexer);
rem wait a moment and retry once before giving up.
ping -n 3 127.0.0.1 >nul 2>&1
rmdir /s /q "%~dp0build" >nul 2>&1
if not exist "%~dp0build" goto :builddir_ready
echo ERROR: failed to clear build directory.
echo        (build\minimon.exe or another file may be locked.
echo         Close it, wait a few seconds, then retry.)
goto :fail
:builddir_ready

cmake -S . -B build -G "MinGW Makefiles"
if errorlevel 1 goto :fail

cmake --build build
if errorlevel 1 goto :fail

echo.
echo BUILD OK: build\minimon.exe
goto :end

rem ---- auto-detect cmake in well-known locations (session PATH only) ----
:ensure_cmake
where.exe cmake >nul 2>&1
if not errorlevel 1 exit /b 0
rem Explicit override: set CMAKE_EXE=C:\path\to\cmake.exe
if not defined CMAKE_EXE goto :no_cmake_exe
if not exist "%CMAKE_EXE%" goto :no_cmake_exe
for %%D in ("%CMAKE_EXE%") do set "PATH=%%~dpD;%PATH%"
echo Using cmake from CMAKE_EXE: %CMAKE_EXE%
where.exe cmake >nul 2>&1
if not errorlevel 1 exit /b 0
:no_cmake_exe
rem Common install locations (each dir checked for cmake.exe).
rem Ordered newest/most-compatible first; VS-bundled CMake is often outdated
rem (e.g. 3.23, which cannot drive new GCC), so it is deliberately last.
call :try_cmake_dir "%ProgramFiles%\CMake\bin"
call :try_cmake_dir "%ProgramW6432%\CMake\bin"
call :try_cmake_dir "%ProgramFiles(x86)%\CMake\bin"
call :try_cmake_dir "%LOCALAPPDATA%\Microsoft\WinGet\Links"
call :try_cmake_dir "%ProgramFiles%\WinGet\Links"
call :try_cmake_dir "%ProgramData%\chocolatey\bin"
call :try_cmake_dir "C:\tools\cmake\bin"
call :try_cmake_dir "%USERPROFILE%\scoop\shims"
call :try_cmake_dir "%SCOOP%\shims"
call :try_cmake_dir "C:\msys64\mingw64\bin"
call :try_cmake_dir "C:\msys64\ucrt64\bin"
call :try_cmake_dir "C:\msys64\usr\bin"
rem retcomm toolchain (agent shells have it on PATH; Explorer double-click misses it)
call :try_cmake_dir "%USERPROFILE%\.local\share\retcomm\toolchains\cmake-clang-v1\latest\bin"
call :try_cmake_dir "%LOCALAPPDATA%\retcomm\toolchains\cmake-clang-v1\latest\bin"
call :try_cmake_dir "%USERPROFILE%\.local\bin"
rem Versioned retcomm toolchain dirs (version number may change): pick any that has cmake.exe
for /d %%D in ("%LOCALAPPDATA%\retcomm\toolchains\cmake-clang-v1\*") do call :try_cmake_dir "%%D\bin"
for /d %%D in ("%USERPROFILE%\.local\share\retcomm\toolchains\cmake-clang-v1\*") do call :try_cmake_dir "%%D\bin"
for /d %%D in ("%LOCALAPPDATA%\retcomm\toolchains\cmake-clang-v1\*") do call :try_cmake_dir "%%D"
for /d %%D in ("%USERPROFILE%\.local\share\retcomm\toolchains\cmake-clang-v1\*") do call :try_cmake_dir "%%D"
rem WinGet-managed CMake package dirs
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Kitware.CMake_*") do call :try_cmake_dir "%%D\bin"
rem Next to g++ / mingw32-make if those are on PATH but cmake is not
for %%E in (g++.exe mingw32-make.exe gcc.exe) do (
  for /f "delims=" %%P in ('where.exe %%E 2^>nul') do call :try_cmake_dir "%%~dpP"
)
rem Visual Studio bundled CMake - LAST resort: often outdated (e.g. 3.23)
rem and unable to drive a new MinGW GCC.
call :try_cmake_dir "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
call :try_cmake_dir "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
call :try_cmake_dir "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
call :try_cmake_dir "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
call :try_cmake_dir "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
call :try_cmake_dir "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where.exe cmake >nul 2>&1
if not errorlevel 1 exit /b 0
exit /b 1

:try_cmake_dir
rem %1 = candidate dir (quoted). If it contains cmake.exe, prepend to PATH.
rem NOTE: single-line IFs only - a path like C:\Program Files (x86)\... breaks IF (...) blocks.
if "%~1"=="" exit /b 1
where.exe cmake >nul 2>&1
if not errorlevel 1 exit /b 0
if not exist "%~1\cmake.exe" exit /b 1
set "PATH=%~1;%PATH%"
echo Using cmake from: %~1\cmake.exe
exit /b 0

rem ---- ensure MinGW g++ / mingw32-make are visible (session PATH only) ----
:ensure_mingw
where.exe g++ >nul 2>&1
if not errorlevel 1 exit /b 0
call :try_mingw_dir "C:\Windows\mingw64\bin"
call :try_mingw_dir "C:\mingw64\bin"
call :try_mingw_dir "%ProgramFiles%\mingw64\bin"
call :try_mingw_dir "%ProgramFiles(x86)%\mingw64\bin"
call :try_mingw_dir "C:\msys64\mingw64\bin"
call :try_mingw_dir "C:\msys64\ucrt64\bin"
call :try_mingw_dir "%USERPROFILE%\.local\share\retcomm\toolchains\cmake-clang-v1\latest\bin"
call :try_mingw_dir "%LOCALAPPDATA%\retcomm\toolchains\cmake-clang-v1\latest\bin"
for /d %%D in ("%LOCALAPPDATA%\retcomm\toolchains\cmake-clang-v1\*") do call :try_mingw_dir "%%D\bin"
for /d %%D in ("%USERPROFILE%\.local\share\retcomm\toolchains\cmake-clang-v1\*") do call :try_mingw_dir "%%D\bin"
exit /b 0

:try_mingw_dir
rem NOTE: single-line IFs only - see :try_cmake_dir.
if "%~1"=="" exit /b 1
where.exe g++ >nul 2>&1
if not errorlevel 1 exit /b 0
if not exist "%~1\g++.exe" exit /b 1
set "PATH=%~1;%PATH%"
echo Using g++ from: %~1\g++.exe
exit /b 0

:fail
echo.
echo BUILD FAILED
goto :end

:end
pause
