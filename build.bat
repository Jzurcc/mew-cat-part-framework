@echo off
REM Build MewItemFramework.dll
set "DEPLOY_DIR="

setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Is Visual Studio ^(or Build Tools^) installed?
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"

if not defined VSDIR (
    echo ERROR: Could not find Visual Studio C++ Build Tools.
    echo Install "Desktop development with C++" from the Visual Studio Installer.
    pause
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

if errorlevel 1 (
    echo ERROR: Could not initialize the x64 MSVC environment.
    pause
    exit /b 1
)

set "CL_EXE=%VCToolsInstallDir%bin\Hostx64\x64\cl.exe"
set "LINK_EXE=%VCToolsInstallDir%bin\Hostx64\x64\link.exe"

if not exist "%CL_EXE%" (
    echo ERROR: cl.exe not found at "%CL_EXE%"
    pause
    exit /b 1
)

if not exist "%LINK_EXE%" (
    echo ERROR: link.exe not found at "%LINK_EXE%"
    pause
    exit /b 1
)

if not exist "src\MewItemFramework.c" (
    echo ERROR: src\MewItemFramework.c not found.
    echo Make sure build.bat is in the MewItemFramework project root ^(beside the src folder^).
    pause
    exit /b 1
)

if not exist "MewItemFramework.def" (
    echo ERROR: MewItemFramework.def is missing.
    pause
    exit /b 1
)

if not exist build mkdir build

REM Never let an old DLL make a failed build look successful.
if exist "MewItemFramework.dll" del /q "MewItemFramework.dll"
if exist "build\MewItemFramework.exp" del /q "build\MewItemFramework.exp"
if exist "build\MewItemFramework.lib" del /q "build\MewItemFramework.lib"

echo Building MewItemFramework.dll...

"%CL_EXE%" /nologo /c /O2 /W3 /MD /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo"build\MewItemFramework.obj" /Tc"%~dp0src\MewItemFramework.c"
if errorlevel 1 goto :failed

"%LINK_EXE%" /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO /OUT:"MewItemFramework.dll" /IMPLIB:"build\MewItemFramework.lib" /DEF:"MewItemFramework.def" "build\MewItemFramework.obj" kernel32.lib
if errorlevel 1 goto :failed

if not exist "MewItemFramework.dll" (
    echo ERROR: Linker returned success but MewItemFramework.dll was not created.
    goto :failed
)

echo.
echo Build succeeded: "%CD%\MewItemFramework.dll"

if not defined DEPLOY_DIR goto :done

if not exist "%DEPLOY_DIR%" mkdir "%DEPLOY_DIR%"
if errorlevel 1 goto :deploy_failed

copy /Y "MewItemFramework.dll" "%DEPLOY_DIR%\MewItemFramework.dll" >nul
if errorlevel 1 goto :deploy_failed

if exist "description.json" (
    copy /Y "description.json" "%DEPLOY_DIR%\description.json" >nul
)

if exist "preview.png" (
    copy /Y "preview.png" "%DEPLOY_DIR%\preview.png" >nul
)

echo Deployed to "%DEPLOY_DIR%"

:done
pause
exit /b 0

:deploy_failed
echo.
echo Build succeeded, but deployment FAILED.
echo DLL remains at "%CD%\MewItemFramework.dll"
pause
exit /b 1

:failed
echo.
echo Build FAILED.
pause
exit /b 1
