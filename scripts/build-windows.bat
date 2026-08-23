@echo off
rem Build rawimport-pipeline.exe natively for Windows (MSVC + vcpkg static deps).
rem Usage: scripts\build-windows.bat [extra cmake args]
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=C:\dev\cmake-3.31.6-windows-x86_64\bin;%PATH%"
set "VCPKG_ROOT=C:\dev\vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo error: vcpkg not found at %VCPKG_ROOT%
  exit /b 1
)
if not exist build-win mkdir build-win
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static %*
if errorlevel 1 exit /b 1
cmake --build build-win --config Release -- /m:%NUMBER_OF_PROCESSORS%
