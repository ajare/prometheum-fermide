@echo off
setlocal

set "CONFIG=Release"
set "BUILD_DIR=build-windows"

:parse_arguments
if "%~1"=="" goto validate_arguments

if /I "%~1"=="/config" (
    if "%~2"=="" (
        echo Error: /config requires a value. 1>&2
        goto usage_error
    )
    set "CONFIG=%~2"
    shift
    shift
    goto parse_arguments
)

if /I "%~1"=="/build-dir" (
    if "%~2"=="" (
        echo Error: /build-dir requires a value. 1>&2
        goto usage_error
    )
    set "BUILD_DIR=%~2"
    shift
    shift
    goto parse_arguments
)

if /I "%~1"=="/?" goto usage
if /I "%~1"=="/help" goto usage

echo Error: unknown parameter "%~1". 1>&2
goto usage_error

:validate_arguments
if /I "%CONFIG%"=="Debug" goto configure
if /I "%CONFIG%"=="Release" goto configure

echo Error: /config must be Debug or Release. 1>&2
goto usage_error

:configure
pushd "%~dp0" >nul || (
    echo Error: unable to enter the repository directory. 1>&2
    exit /b 1
)

where cmake >nul 2>&1 || (
    echo Error: cmake was not found on PATH. 1>&2
    popd
    exit /b 1
)

echo Configuring a fresh %CONFIG% build in "%BUILD_DIR%"...
cmake --fresh -S . -B "%BUILD_DIR%" -A x64
if errorlevel 1 goto build_failed

echo Building all targets...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --parallel --clean-first
if errorlevel 1 goto build_failed

echo Build completed successfully.
popd
exit /b 0

:build_failed
set "RESULT=%ERRORLEVEL%"
echo Build failed with exit code %RESULT%. 1>&2
popd
exit /b %RESULT%

:usage_error
call :print_usage
exit /b 2

:usage
call :print_usage
exit /b 0

:print_usage
echo Usage: %~nx0 [/config Debug^|Release] [/build-dir path]
echo.
echo   /config     Build configuration. Defaults to Release.
echo   /build-dir  CMake build directory. Defaults to build-windows.
exit /b 0
