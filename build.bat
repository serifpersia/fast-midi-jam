@echo off
setlocal enabledelayedexpansion

:: Check for CMake
where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo CMake not found! Install with: winget install Kitware.CMake
    exit /b 1
)

:: Check for compiler
set COMPILER_FOUND=0
set COMPILER_TYPE=unknown

where g++ >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set COMPILER_FOUND=1
    set COMPILER_TYPE=mingw
    goto COMPILER_FOUND
)

where cl >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set COMPILER_FOUND=1
    set COMPILER_TYPE=msvc
    goto COMPILER_FOUND
)

echo No C++ compiler found! Install MinGW (winget install mingw) or Visual Studio.
exit /b 1

:COMPILER_FOUND

:: Download RtMidi if not exists
if not exist rtmidi (
    echo Downloading RtMidi...
    powershell -Command "Invoke-WebRequest -Uri 'https://github.com/thestk/rtmidi/archive/refs/heads/master.zip' -OutFile 'rtmidi.zip'"
    powershell -Command "Expand-Archive -Path 'rtmidi.zip' -DestinationPath '.' -Force"
    if not exist rtmidi mkdir rtmidi
    copy /Y "rtmidi-master\*.h" "rtmidi\"
    copy /Y "rtmidi-master\*.cpp" "rtmidi\"
    del rtmidi.zip
    rd /s /q rtmidi-master
)

:: Remove existing build directory
if exist build rd /s /q build

:: Create and build
if not exist build mkdir build
cd build

if "%COMPILER_TYPE%"=="mingw" (
    echo Using MinGW...
    cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
    if !ERRORLEVEL! neq 0 exit /b 1
    :: Use all available CPU cores for parallel building
    for /f "tokens=*" %%i in ('powershell -Command "(Get-CimInstance -ClassName Win32_ComputerSystem).NumberOfLogicalProcessors"') do set JOBS=%%i
    echo Building with !JOBS! jobs...
    mingw32-make -j!JOBS!
    if !ERRORLEVEL! neq 0 exit /b 1
) else (
    echo Using Visual Studio...
    cmake -G "Visual Studio 17 2022" -A x64 ..
    if !ERRORLEVEL! neq 0 exit /b 1
    :: MSBuild already uses multiple cores by default, but we can make it explicit
    cmake --build . --config Release -- /maxcpucount
    if !ERRORLEVEL! neq 0 exit /b 1
)

echo Build complete! Executables: build\MidiJamServer.exe and build\MidiJamClient.exe
cd ..

endlocal