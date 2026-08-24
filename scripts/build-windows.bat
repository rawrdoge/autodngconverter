@echo off
rem Build rawimport-pipeline.exe natively for Windows (MSVC + vcpkg static deps).
rem Usage: scripts\build-windows.bat [extra cmake args]
rem
rem CI-friendly behavior:
rem  - Skips the hardcoded vcvars64.bat call when an MSVC environment is already
rem    active (VCINSTALLDIR set, e.g. by ilammy/msvc-dev-cmd in GitHub Actions).
rem  - Honors VCPKG_ROOT from the environment (GitHub Actions sets it); local
rem    machines fall back to E:\dev\vcpkg.
rem  - Only prepends the local cmake dir to PATH when it exists.
setlocal
if "%VCINSTALLDIR%"=="" (
  call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
  if errorlevel 1 exit /b 1
)
if exist "E:\dev\cmake-3.31.6-windows-x86_64\bin\cmake.exe" set "PATH=E:\dev\cmake-3.31.6-windows-x86_64\bin;%PATH%"
rem VCPKG_ROOT resolution: RI_VCPKG_ROOT (ours, collision-proof vs the runner's
rem Visual Studio bundled vcpkg) wins; else inherit env; else local default.
if "%RI_VCPKG_ROOT%"=="" (
  if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=E:\dev\vcpkg"
) else (
  set "VCPKG_ROOT=%RI_VCPKG_ROOT%"
)
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo error: vcpkg not found at %VCPKG_ROOT%
  exit /b 1
)
if not exist build-win mkdir build-win
rem Generator: honor CMAKE_GENERATOR from the environment (CI runners may ship
rem a newer Visual Studio than the local default below).
if "%CMAKE_GENERATOR%"=="" set "CMAKE_GENERATOR=Visual Studio 17 2022"
cmake -S . -B build-win -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static %*
if errorlevel 1 exit /b 1
cmake --build build-win --config Release -- /m:%NUMBER_OF_PROCESSORS%
