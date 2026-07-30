@echo off
REM Builds PDFium as a self-contained static library.
REM The checkout is already there: this script picks up from gn gen onwards.
REM Run tools\pdfium_spike.bat first if it is not.
REM
REM What has to be found first, none of it guessable from the repository:
REM
REM   PDFIUM_BUILD_ROOT  the workspace holding depot_tools and the pdfium
REM                      checkout. Careful, this is NOT the PDFIUM_ROOT that
REM                      CMake wants: that one is <PDFIUM_BUILD_ROOT>\pdfium.
REM   VSINSTALLDIR       install root of Visual Studio or the Build Tools.
REM                      Already set inside a Developer Command Prompt;
REM                      otherwise this script asks vswhere.

setlocal

if "%PDFIUM_BUILD_ROOT%"=="" goto :no_root
set "ROOT=%PDFIUM_BUILD_ROOT%"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
if not exist "%ROOT%\pdfium\BUILD.gn" goto :no_checkout

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
set DEPOT_TOOLS_UPDATE=0
set "GYP_MSVS_OVERRIDE_PATH=%VSDIR%"
set "vs2022_install=%VSDIR%"
set "PATH=%ROOT%\depot_tools;%PATH%"

cd /d "%ROOT%\pdfium" || exit /b 1

echo ==== gn gen
REM  pdf_is_complete_lib=true   -> an archive that really is self-contained
REM  is_official_build=false    -> mandatory: with ThinLTO the .obj files come
REM                               out as LLVM bitcode, which link.exe cannot
REM                               read
REM  pdf_enable_v8/xfa=false    -> no JavaScript, no XFA modules
REM
REM  use_custom_libcxx=false    -> THE ONE THAT MATTERS. By default Chromium
REM    uses its own standard library (libc++). The result does link against
REM    MSVC, but the same binary then holds TWO standard libraries, and the
REM    types that have to be ABI-compatible -- std::exception_ptr first among
REM    them -- end up defined twice. With this option PDFium uses Microsoft's
REM    library like the rest of the project, and the problem goes away instead
REM    of being worked around.
call gn gen out/Release --args="is_debug=false is_component_build=false is_official_build=false pdf_is_complete_lib=true pdf_enable_v8=false pdf_enable_xfa=false pdf_use_skia=false symbol_level=0 treat_warnings_as_errors=false use_custom_libcxx=false" || exit /b 1

echo ==== ninja (long)
call ninja -C out/Release pdfium || exit /b 1

echo ==== DONE
dir out\Release\obj\pdfium.lib
echo.
echo Now configure Filo with: -DPDFIUM_ROOT=%ROOT%\pdfium
endlocal
exit /b 0

:no_root
echo ERROR: PDFIUM_BUILD_ROOT is not set.
echo        It is the workspace with depot_tools and the pdfium checkout in it,
echo        several GB, outside the Filo tree. For example:
echo.
echo            set PDFIUM_BUILD_ROOT=C:\src\pdfium-build
echo.
echo        Run tools\pdfium_spike.bat to create it from scratch.
exit /b 1

:no_checkout
echo ERROR: no pdfium checkout in "%ROOT%\pdfium".
echo        PDFIUM_BUILD_ROOT is the PARENT of the checkout, not the checkout
echo        itself. Run tools\pdfium_spike.bat to fetch it.
exit /b 1

:no_msvc
echo ERROR: the MSVC toolchain was not found; depot_tools needs its install
echo        path. Run this from an "x64 Native Tools Command Prompt", or set
echo        VSINSTALLDIR to the Visual Studio / Build Tools install root:
echo.
echo            set VSINSTALLDIR=C:\Program Files\Microsoft Visual Studio\2022\Community
exit /b 1
