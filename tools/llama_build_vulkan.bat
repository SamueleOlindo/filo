@echo off
REM Builds llama.cpp with the Vulkan backend.
REM
REM Why Vulkan and not CUDA or ROCm: Vulkan runs on AMD, Intel and NVIDIA from
REM the same code, and the end user has to install NOTHING -- the runtime ships
REM with the graphics driver. CUDA would tie the project to one brand and put a
REM download of several hundred MB in front of everybody.
REM
REM The Vulkan SDK is needed ONLY here, to compile the shaders with glslc.
REM
REM What has to be found first, none of it guessable from the repository:
REM
REM   LLAMA_ROOT    working directory for this build. Clone llama.cpp into
REM                 <LLAMA_ROOT>\llama.cpp; the libraries end up in
REM                 <LLAMA_ROOT>\out-vulkan. Hand the same path to CMake as
REM                 -DLLAMA_ROOT=... to build Filo against it.
REM   VULKAN_SDK    root of the Vulkan SDK. The SDK installer sets it; if it is
REM                 missing this script looks in C:\VulkanSDK.
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

REM --- locate the Vulkan SDK --------------------------------------------------
REM The installer sets VULKAN_SDK. If it did not, fall back to the default
REM install location and take the newest version found there.
if not "%VULKAN_SDK%"=="" goto :got_sdk
for /d %%d in (C:\VulkanSDK\*) do set "VULKAN_SDK=%%d"
if "%VULKAN_SDK%"=="" goto :no_sdk
:got_sdk
if not exist "%VULKAN_SDK%\Bin\glslc.exe" goto :no_sdk
set "PATH=%VULKAN_SDK%\Bin;%PATH%"
echo SDK: %VULKAN_SDK%

set "SRC=%LLAMA_ROOT%\llama.cpp"
set "OUT=%LLAMA_ROOT%\out-vulkan"

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
  -DGGML_AVX2=ON ^
  -DGGML_VULKAN=ON || exit /b 1

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
echo            tools\llama_build_vulkan.bat
exit /b 1

:no_checkout
echo ERROR: no llama.cpp checkout in "%LLAMA_ROOT%\llama.cpp".
echo        LLAMA_ROOT is the PARENT of the checkout, not the checkout itself:
echo.
echo            git clone https://github.com/ggml-org/llama.cpp "%LLAMA_ROOT%\llama.cpp"
exit /b 1

:no_sdk
echo ERROR: the Vulkan SDK was not found; glslc is needed to compile the
echo        shaders. Install it from https://vulkan.lunarg.com/ and reopen the
echo        prompt, or point VULKAN_SDK at it:
echo.
echo            set VULKAN_SDK=C:\VulkanSDK\1.3.296.0
echo.
echo        For a CPU-only build use tools\llama_build.bat instead.
exit /b 1

:no_msvc
echo ERROR: the MSVC toolchain was not found.
echo        Run this from an "x64 Native Tools Command Prompt", or set
echo        VSINSTALLDIR to the Visual Studio / Build Tools install root:
echo.
echo            set VSINSTALLDIR=C:\Program Files\Microsoft Visual Studio\2022\Community
exit /b 1
