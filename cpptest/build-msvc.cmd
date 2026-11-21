@echo off
setlocal
cd /d "%~dp0"

set "BGI_CMAKE=%~1"
if not defined BGI_CMAKE set "BGI_CMAKE=cmake"
set "BGI_OPENCV=%~2"
if not defined BGI_OPENCV set "BGI_OPENCV=%~dp0_deps\opencv\build"

if not defined VCToolsInstallDir (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "BGI_VS=%%i"
)
if defined BGI_VS call "%BGI_VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

"%BGI_CMAKE%" -S . -B build-native -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release "-DOpenCV_DIR=%BGI_OPENCV%" || exit /b 1
"%BGI_CMAKE%" --build build-native || exit /b 1
endlocal
