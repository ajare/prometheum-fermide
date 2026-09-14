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
if /I "%CONFIG%"=="Debug" goto find_solution
if /I "%CONFIG%"=="Release" goto find_solution

echo Error: /config must be Debug or Release. 1>&2
goto usage_error

:find_solution
pushd "%~dp0" >nul || (
    echo Error: unable to enter the repository directory. 1>&2
    exit /b 1
)

if exist "%BUILD_DIR%\*.sln" goto incremental_build
if exist "%BUILD_DIR%\*.slnx" goto incremental_build

echo No Visual Studio solution found in "%BUILD_DIR%"; creating a fresh build.
call "%~dp0build_from_scratch.bat" /config "%CONFIG%" /build-dir "%BUILD_DIR%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

:incremental_build
where cmake >nul 2>&1 || (
    echo Error: cmake was not found on PATH. 1>&2
    popd
    exit /b 1
)

echo Checking CMake build files...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target ZERO_CHECK --parallel
if errorlevel 1 goto build_failed

echo Building changed targets...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --parallel
if errorlevel 1 goto build_failed

echo Incremental build completed successfully.
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
