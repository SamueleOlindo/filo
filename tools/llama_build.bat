@echo off
REM Builds llama.cpp as a static library, for computing embeddings and nothing
REM else.
REM
REM No examples, no server, no tools: what we need is one function that turns
REM text into a vector. Everything else would be extra weight in the binary and
REM extra surface to maintain.
REM
REM CPU only: the model is 100-300M parameters, it runs in milliseconds even
REM without an accelerator, and demanding a GPU would shut out half the users.
REM
REM Two things have to be found before this can run, and neither of them can be
REM guessed from the repository:
REM
REM   LLAMA_ROOT    working directory for this build. Clone llama.cpp into
REM                 <LLAMA_ROOT>\llama.cpp; the libraries end up in
REM                 <LLAMA_ROOT>\out. Hand the same path to CMake as
REM                 -DLLAMA_ROOT=... to build Filo against it.
REM   VSINSTALLDIR  install root of Visual Studio or the Build Tools. Already
REM                 set inside a Developer Command Prompt; otherwise this
REM                 script asks vswhere, and only then gives up.

setlocal

if "%LLAMA_ROOT%"=="" goto :no_root
if not exist "%LLAMA_ROOT%\llama.cpp\CMakeLists.txt" goto :no_checkout

REM --- locate MSVC ------------------------------------------------------------
REM Already inside a Developer Command Prompt: nothing to do.
where cl.exe >nul 2>&1 && goto :msvc_ready

set "VSDIR=%VSINSTALLDIR%"
if not "%VSDIR%"=="" goto :got_vsdir

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_msvc
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" goto :no_msvc

:got_vsdir
if "%VSDIR:~-1%"=="\" set "VSDIR=%VSDIR:~0,-1%"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto :no_msvc
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto :no_msvc
:msvc_ready

set "SRC=%LLAMA_ROOT%\llama.cpp"
set "OUT=%LLAMA_ROOT%\out"

cmake -S "%SRC%" -B "%OUT%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DLLAMA_BUILD_TESTS=OFF ^
  -DLLAMA_BUILD_EXAMPLES=OFF ^
  -DLLAMA_BUILD_SERVER=OFF ^
  -DLLAMA_BUILD_TOOLS=OFF ^
  -DLLAMA_CURL=OFF ^
  -DGGML_OPENMP=OFF ^
  -DGGML_NATIVE=OFF ^
  -DGGML_AVX2=ON || exit /b 1

cmake --build "%OUT%" --target llama || exit /b 1

echo ==== DONE
dir /s /b "%OUT%\*.lib"
echo.
echo Now configure Filo with: -DLLAMA_ROOT=%LLAMA_ROOT%
endlocal
exit /b 0

:no_root
echo ERROR: LLAMA_ROOT is not set.
echo        It is the working directory for this build, somewhere with a few GB
echo        free and outside the Filo tree. For example:
echo.
echo            set LLAMA_ROOT=C:\src\llama-build
echo            git clone https://github.com/ggml-org/llama.cpp "%%LLAMA_ROOT%%\llama.cpp"
echo            tools\llama_build.bat
exit /b 1

:no_checkout
echo ERROR: no llama.cpp checkout in "%LLAMA_ROOT%\llama.cpp".
echo        LLAMA_ROOT is the PARENT of the checkout, not the checkout itself:
echo.
echo            git clone https://github.com/ggml-org/llama.cpp "%LLAMA_ROOT%\llama.cpp"
exit /b 1

:no_msvc
echo ERROR: the MSVC toolchain was not found.
echo        Run this from an "x64 Native Tools Command Prompt", or set
echo        VSINSTALLDIR to the Visual Studio / Build Tools install root:
echo.
echo            set VSINSTALLDIR=C:\Program Files\Microsoft Visual Studio\2022\Community
exit /b 1
