@echo off
REM Build spike: PDFium as a self-contained static library.
REM The question it answers: does pdfium.lib link with MSVC's link.exe, BEFORE
REM an architecture gets built on top of it. The answer can change the
REM toolchain.
REM
REM What has to be found first, none of it guessable from the repository:
REM
REM   PDFIUM_BUILD_ROOT  where depot_tools and the pdfium checkout go. Several
REM                      GB, so put it outside the Filo tree. Careful, this is
REM                      NOT the PDFIUM_ROOT that CMake wants: that one is
REM                      <PDFIUM_BUILD_ROOT>\pdfium.
REM   VSINSTALLDIR       install root of Visual Studio or the Build Tools.
REM                      Already set inside a Developer Command Prompt;
REM                      otherwise this script asks vswhere.

setlocal

if "%PDFIUM_BUILD_ROOT%"=="" goto :no_root
set "ROOT=%PDFIUM_BUILD_ROOT%"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

REM --- locate Visual Studio ---------------------------------------------------
REM depot_tools wants the install directory, not a configured shell, so
REM vcvars is of no use here.
set "VSDIR=%VSINSTALLDIR%"
if not "%VSDIR%"=="" goto :got_vsdir

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_msvc
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" goto :no_msvc

:got_vsdir
if "%VSDIR:~-1%"=="\" set "VSDIR=%VSDIR:~0,-1%"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto :no_msvc

set DEPOT_TOOLS_WIN_TOOLCHAIN=0
set DEPOT_TOOLS_METRICS=0
set "GYP_MSVS_OVERRIDE_PATH=%VSDIR%"
set "vs2022_install=%VSDIR%"

echo ==== [1/5] preparing %ROOT%
if not exist "%ROOT%" mkdir "%ROOT%"
cd /d "%ROOT%" || exit /b 1

echo ==== [2/5] depot_tools
if not exist "%ROOT%\depot_tools" (
  git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "%ROOT%\depot_tools" || exit /b 1
) else (
  echo already there, skipping
)
set "PATH=%ROOT%\depot_tools;%PATH%"

echo ==== [3/5] gclient config + sync (several GB, takes a while)
call gclient config --unmanaged https://pdfium.googlesource.com/pdfium.git || exit /b 1
call gclient sync --no-history --shallow --nohooks || exit /b 1
call gclient runhooks || exit /b 1

echo ==== [4/5] gn gen
cd /d "%ROOT%\pdfium" || exit /b 1
call gn gen out/Release --args="is_debug=false is_component_build=false is_official_build=false pdf_is_complete_lib=true pdf_enable_v8=false pdf_enable_xfa=false pdf_use_skia=false symbol_level=0 treat_warnings_as_errors=false" || exit /b 1

echo ==== [5/5] ninja
call ninja -C out/Release pdfium || exit /b 1

echo ==== DONE
dir out\Release\obj\pdfium.lib
endlocal
exit /b 0

:no_root
echo ERROR: PDFIUM_BUILD_ROOT is not set.
echo        It is where depot_tools and the pdfium checkout will go: several
echo        GB, outside the Filo tree. For example:
echo.
echo            set PDFIUM_BUILD_ROOT=C:\src\pdfium-build
echo            tools\pdfium_spike.bat
exit /b 1

:no_msvc
echo ERROR: the MSVC toolchain was not found; depot_tools needs its install
echo        path. Run this from an "x64 Native Tools Command Prompt", or set
echo        VSINSTALLDIR to the Visual Studio / Build Tools install root:
echo.
echo            set VSINSTALLDIR=C:\Program Files\Microsoft Visual Studio\2022\Community
exit /b 1
