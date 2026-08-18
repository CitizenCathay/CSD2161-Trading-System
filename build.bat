@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SOURCE_DIR=%~dp0"
if "%SOURCE_DIR:~-1%"=="\" set "SOURCE_DIR=%SOURCE_DIR:~0,-1%"

set "CONFIG=Debug"
set "ARCH=x64"
if not "%~1"=="" set "CONFIG=%~1"
if not "%~2"=="" set "ARCH=%~2"
set "GEN=sln"
if not "%~3"=="" set "GEN=%~3"
set "RUN=run"
if not "%~4"=="" set "RUN=%~4"
set "DEPS=install"
if not "%~5"=="" set "DEPS=%~5"

call :setup_vs_env "%ARCH%" || exit /b 1

call :ensure_vcpkg "%DEPS%" || exit /b 1

set "TOOLCHAIN_FILE=%SOURCE_DIR%\vcpkg\scripts\buildsystems\vcpkg.cmake"
if not exist "%TOOLCHAIN_FILE%" (
  echo vcpkg toolchain not found: "%TOOLCHAIN_FILE%"
  exit /b 1
)

if /i "%GEN%"=="ninja" (
  call :build_with_preset || exit /b 1
) else (
  call :build_with_sln || exit /b 1
)

if /i "%RUN%"=="norun" exit /b 0

call :find_exe "Backend" "%BUILD_DIR%" "%CONFIG%" BACKEND_EXE || exit /b 1
call :find_exe "Frontend" "%BUILD_DIR%" "%CONFIG%" FRONTEND_EXE || exit /b 1

echo.
echo Starting Backend:  "%BACKEND_EXE%"
start "Backend" cmd /k ""%BACKEND_EXE%""

timeout /t 1 /nobreak >nul

echo Starting Frontend: "%FRONTEND_EXE%"
start "Frontend" cmd /k ""%FRONTEND_EXE%""

exit /b 0

:setup_vs_env
set "REQ_ARCH=%~1"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere not found: "%VSWHERE%"
  echo Install Visual Studio 2026 ^(or Build Tools^) and retry.
  exit /b 1
)

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if "%VS_INSTALL%"=="" (
  echo Visual Studio with C++ tools not found via vswhere.
  exit /b 1
)

set "VSDEV=%VS_INSTALL%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEV%" (
  echo VsDevCmd.bat not found: "%VSDEV%"
  exit /b 1
)

echo.
echo Using Visual Studio at: "%VS_INSTALL%"
call "%VSDEV%" -no_logo -arch=%REQ_ARCH% -host_arch=%REQ_ARCH%
set "VCPKG_ROOT=%SOURCE_DIR%\vcpkg"
exit /b %errorlevel%

:resolve_preset
set "REQ_CONFIG=%~1"
set "REQ_ARCH=%~2"
set "OUT_VAR=%~3"

if /i "%REQ_ARCH%"=="x64" (
  if /i "%REQ_CONFIG%"=="Debug" set "%OUT_VAR%=x64-debug" & exit /b 0
  if /i "%REQ_CONFIG%"=="Release" set "%OUT_VAR%=x64-release" & exit /b 0
)

if /i "%REQ_ARCH%"=="x86" (
  if /i "%REQ_CONFIG%"=="Debug" set "%OUT_VAR%=x86-debug" & exit /b 0
  if /i "%REQ_CONFIG%"=="Release" set "%OUT_VAR%=x86-release" & exit /b 0
)

echo Unsupported arguments. Usage:
echo   build.bat [Debug^|Release] [x64^|x86] [sln^|ninja] [run^|norun] [install^|noinstall]
exit /b 1

:find_exe
set "TARGET_NAME=%~1"
set "SEARCH_ROOT=%~2"
set "TARGET_CONFIG=%~3"
set "OUT_VAR=%~4"

set "CANDIDATE1=%SEARCH_ROOT%\%TARGET_NAME%\%TARGET_CONFIG%\%TARGET_NAME%.exe"
set "CANDIDATE2=%SEARCH_ROOT%\%TARGET_CONFIG%\%TARGET_NAME%.exe"
set "CANDIDATE3=%SEARCH_ROOT%\%TARGET_NAME%.exe"

if exist "%CANDIDATE1%" ( set "%OUT_VAR%=%CANDIDATE1%" & exit /b 0 )
if exist "%CANDIDATE2%" ( set "%OUT_VAR%=%CANDIDATE2%" & exit /b 0 )
if exist "%CANDIDATE3%" ( set "%OUT_VAR%=%CANDIDATE3%" & exit /b 0 )

set "FOUND="
for /r "%SEARCH_ROOT%" %%F in (%TARGET_NAME%.exe) do (
  echo %%~fF | findstr /i /c:"\%TARGET_CONFIG%\" >nul && (
    set "FOUND=%%~fF"
    goto :find_exe_done
  )
)

:find_exe_done
if defined FOUND (
  set "%OUT_VAR%=%FOUND%"
  exit /b 0
)

echo Failed to locate %TARGET_NAME%.exe in "%SEARCH_ROOT%" for config "%TARGET_CONFIG%".
exit /b 1

:build_with_preset
call :resolve_preset "%CONFIG%" "%ARCH%" PRESET || exit /b 1
set "BUILD_DIR=%SOURCE_DIR%\build\%PRESET%"

echo.
echo Configuring (Preset=%PRESET%)...
cmake --preset "%PRESET%" || exit /b 1

echo.
echo Building targets (Configuration=%CONFIG% Architecture=%ARCH%)...
cmake --build "%BUILD_DIR%" --target Backend Frontend || exit /b 1
exit /b 0

:build_with_sln
set "VS_GENERATOR=Visual Studio 17 2022"
call :resolve_preset "%CONFIG%" "%ARCH%" PRESET || exit /b 1
set "BUILD_DIR=%SOURCE_DIR%\build\vs\%PRESET%"

echo.
echo Configuring solution - Generator=%VS_GENERATOR% Arch=%ARCH%...
cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G "%VS_GENERATOR%" -A "%ARCH%" -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN_FILE%" || exit /b 1

echo.
echo Building solution - Configuration=%CONFIG% Architecture=%ARCH%...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target Backend Frontend -- /m || exit /b 1

if exist "%BUILD_DIR%\TradingSystem.sln" (
  echo.
  echo Solution generated: "%BUILD_DIR%\TradingSystem.sln"
)
exit /b 0

:ensure_vcpkg
set "REQ_DEPS=%~1"

if not exist "%SOURCE_DIR%\vcpkg" (
  echo.
  echo Cloning vcpkg...
  git clone https://github.com/microsoft/vcpkg.git "%SOURCE_DIR%\vcpkg" || exit /b 1
)

if not exist "%SOURCE_DIR%\vcpkg\vcpkg.exe" (
  echo.
  echo Bootstrapping vcpkg...
  pushd "%SOURCE_DIR%\vcpkg"
  call bootstrap-vcpkg.bat || ( popd & exit /b 1 )
  popd
)

if /i "%REQ_DEPS%"=="noinstall" exit /b 0

if not exist "%SOURCE_DIR%\vcpkg.json" (
  echo vcpkg.json not found at: "%SOURCE_DIR%\vcpkg.json"
  exit /b 1
)

echo.
echo Installing vcpkg dependencies...
pushd "%SOURCE_DIR%\vcpkg"
.\vcpkg.exe install --x-manifest-root="%SOURCE_DIR%" || ( popd & exit /b 1 )
popd
exit /b 0
