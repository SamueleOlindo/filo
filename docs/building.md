# Building Filo

Filo builds with MSVC and CMake into a single executable. The two heavy
dependencies — PDFium and llama.cpp — are optional in the build system, and
leaving them out is a supported configuration rather than a broken one. That is
deliberate: nobody should have to do a multi-gigabyte Chromium checkout before
they can compile a file search program.

## What you need

- Windows 10 or 11, x64.
- **MSVC** — Visual Studio 2022 or the standalone Build Tools, with the C++
  workload. That also brings the Windows SDK, which is needed for the resource
  compiler as well as the headers.
- **CMake 3.21 or newer.** The project itself asks for 3.20; the presets file
  asks for 3.21.
- **Ninja.** Both presets use it. Visual Studio ships one, but not on `PATH`;
  see the user presets section below if `cmake` cannot find it.

Everything else is vendored under `third_party/`: SQLite with FTS5, the
Snowball stemmers for Italian and English, libdeflate's decompression path,
xxHash, and the WebView2 headers with the static loader. The default build
downloads nothing.

At *run* time the machine also needs the Microsoft Edge WebView2 runtime, which
is part of Windows 11 and an install away on Windows 10. Without it the window
refuses to open and says so.

## The short version

```
cmake --preset default
cmake --build --preset default
build\Filo.exe --selftest
```

That gives you `build\Filo.exe`: Release, static CRT, no DLLs to ship, PDF
extraction off and both models off.

For a debug build, add `-DCMAKE_BUILD_TYPE=Debug` to the configure step; the
CRT setting follows it, so you get the debug static runtime rather than a
mismatch.

Note that every preset writes into `build/`. Two builds started at the same
time in the same clone will fight over it.

## What the default build leaves out

Two features switch themselves off when their dependency is absent, and CMake
says so while configuring:

```
-- PDFIUM_ROOT is not set: PDFs will not be extracted. ...
-- LLAMA_ROOT is not set: semantic search disabled. ...
```

Both are `CACHE PATH` variables that default to the environment variable of the
same name, so either of these works:

```
set PDFIUM_ROOT=C:\src\pdfium-build\pdfium
cmake --preset default

cmake --preset default -DLLAMA_ROOT=C:\src\llama-build
```

Empty means "off". **Set but wrong is a hard error**, on purpose: CMake stops
and names the variable and the file it looked for, rather than letting the
compile fail two minutes later on a missing `fpdfview.h` that explains nothing.
To turn a feature back off, pass the variable empty — `-DPDFIUM_ROOT=` —
rather than deleting it and wondering why the cache still remembers it.

There is also `-DFILO_DISABLE_PDFIUM=ON`, which ignores a PDFium that is
present. It exists for comparing the two builds without moving directories
around.

**Without PDFium**, `src/extract/pdf.cpp` is not compiled and PDFs are counted
as "waiting for extractor" during indexing. Everything else indexes normally.

**Without llama.cpp**, `src/vector/embedder.cpp` and
`src/search/query_model.cpp` are not compiled, and `src/vector/no_llama.cpp` is
compiled in their place. It is a set of stubs that answer "not available",
because the window calls both classes unconditionally — they are already
allowed to be missing at run time. Without those stubs the dependency-free
build compiled cleanly and then died with eight unresolved externals, which is
what everyone cloning the repository hit first. If you add a method to
`Embedder` or `QueryModel` and call it from outside, that file needs a stub for
it, and only a build of this configuration will tell you.

## Presets

`CMakePresets.json` has exactly two, and neither contains a path from anyone's
machine:

- **`default`** — Release, Ninja, MSVC, `build/`. Configures on a machine that
  has neither optional dependency.
- **`full`** — the same, plus `PDFIUM_ROOT` and `LLAMA_ROOT` taken from the
  environment. Use it when both are already exported.

Machine-specific paths belong in **`CMakeUserPresets.json`**, which is in
`.gitignore` and is the one place for them. A typical one:

```json
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 21, "patch": 0 },
  "configurePresets": [
    {
      "name": "local",
      "inherits": "default",
      "displayName": "This machine",
      "cacheVariables": {
        "PDFIUM_ROOT": "C:/src/pdfium-build/pdfium",
        "LLAMA_ROOT": "C:/src/llama-build",
        "CMAKE_MAKE_PROGRAM": "C:/.../CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
      }
    }
  ],
  "buildPresets": [
    { "name": "local", "configurePreset": "local" }
  ]
}
```

`CMAKE_MAKE_PROGRAM` is how you point at the Ninja that came with Visual Studio
without putting it on `PATH`. Drop that line if `ninja` is already findable.

## Building llama.cpp

`LLAMA_ROOT` is **not** the llama.cpp checkout. It is a working directory that
holds the checkout in `llama.cpp/` next to the build output in `out/` or
`out-vulkan/`. That is the layout the scripts produce and the layout CMake
looks for.

CPU only:

```
set LLAMA_ROOT=C:\src\llama-build
git clone https://github.com/ggml-org/llama.cpp %LLAMA_ROOT%\llama.cpp
tools\llama_build.bat
```

With the Vulkan backend:

```
tools\llama_build_vulkan.bat
```

Both scripts build a static library and nothing else — no examples, no server,
no tools — and both find MSVC themselves through `vswhere` if you are not
already in a Developer Command Prompt.

Vulkan rather than CUDA or ROCm, because it runs on AMD, Intel and NVIDIA from
one build and the end user installs nothing: the runtime arrives with the
graphics driver. The **Vulkan SDK is needed only at build time** — `glslc` to
compile the shaders in the script, and `vulkan-1.lib` to link Filo. CMake finds
the SDK through `VULKAN_SDK` or, failing that, under `C:\VulkanSDK\`.

When both `out-vulkan/` and `out/` exist, CMake picks the Vulkan one. In the
Filo binary `vulkan-1.dll` is delay loaded, so the executable still starts on a
machine with no Vulkan loader at all and falls back to the CPU instead of being
refused by the Windows loader before any of its own code runs.

## Building PDFium

This one is long — a Chromium-style checkout of several gigabytes and a build
to match. `PDFIUM_BUILD_ROOT` is the workspace holding `depot_tools` and the
checkout; the `PDFIUM_ROOT` that CMake wants is `%PDFIUM_BUILD_ROOT%\pdfium`,
one level down. Mixing the two up is the mistake the CMake error messages are
written to catch.

From scratch:

```
set PDFIUM_BUILD_ROOT=C:\src\pdfium-build
tools\pdfium_spike.bat
```

To rebuild once the checkout exists, `tools\pdfium_build.bat` picks up from
`gn gen` onwards. The build arguments matter more than they look:
`pdf_is_complete_lib=true` for an archive that is really self-contained,
`is_official_build=false` because ThinLTO emits bitcode that `link.exe` cannot
read, no V8 and no XFA, and `use_custom_libcxx=false` so PDFium uses
Microsoft's standard library like the rest of the program instead of putting a
second one in the same binary.

If you do build PDFium against its own libc++, CMake will also link
`out/Release/obj/libcxx.lib` when that file exists — it has to be packed by
hand, and the two-line recipe is in a comment in `CMakeLists.txt`.

## Checking a build

```
build\Filo.exe --selftest
```

**This needs nothing installed**: no model, no content index, no elevation, and
no index of the disk. It builds a small index in memory and checks the
invariants whose failure produces confident wrong answers rather than a
crash — a child following a renamed folder, an MFT record reused under a new
sequence number, paths surviving compaction, vectors staying attached to their
own chunk after removal, the query planner, and the ownership gates on the
space screen.
The last group asks Windows about the Recycle Bin on `C:`, read-only, to check
that a file too large for it is refused; nothing is put in one. It prints a
line per check and exits non-zero if any of them failed.

Other checks, in rough order of how much they touch:

| | |
|---|---|
| `--selftest <model.gguf>` | adds the tokenizer checks. llama.cpp builds only |
| `--dbtest` | writes 20,000 synthetic documents and proves selective delete |
| `--workertest` | crashes workers on purpose and shows the pool surviving |
| `--extract <path>` | prints what an extractor actually saw in one file |
| `--bintest` | really recycles a file it created, and leaves it in the bin |

There are also a handful of environment variables for pinning down a build that
misbehaves:

| | |
|---|---|
| `FILO_LLAMA_LOG` | bring back everything llama.cpp and ggml print |
| `FILO_FORCE_CPU` | ignore the GPU, whatever the build supports |
| `FILO_PRETEND_NO_VULKAN` | act as though the Vulkan loader were missing |
| `FILO_UI_PROBE` | capture the page to this path once it has loaded |
| `FILO_UI_VIEW` | which screen that capture should photograph |
| `FILO_ANON_HOME` | show your profile folder as this instead, for screenshots |

`FILO_ANON_HOME=C:\Users\you` displays `C:\Users\alice\Documents\x.pdf` as
`C:\Users\you\Documents\x.pdf`. It changes only what the page is SHOWN, never
what Filo acts on, and that is safe for a structural reason rather than a
hopeful one: the page names files to delete by id into a snapshot the host
built, never by path, so a displayed path cannot reach the deletion code at all.
The two messages that do carry a path back — open and reveal — are put through
the inverse, so a file opened from an anonymised list still opens.

It hides the profile folder and nothing else. File names, project folders and
anything else on screen are still real, which matters if you are about to
publish the picture.

## Compiler settings worth knowing about

Four choices in `CMakeLists.txt` are not defaults and are not arbitrary. Each
has a comment there explaining what went wrong without it; the short version:

- **`/EHs`, not `/EHsc`.** The trailing `c` promises that `extern "C"` functions
  never throw, and MSVC then drops the unwinding around them. llama.cpp's C
  API is implemented in C++ and does throw — its tokenizer raises
  `std::regex_error` on input MSVC's `std::regex` cannot handle — and under
  `/EHsc` that throw walks past every catch block into `std::terminate`.
- **Static CRT** (`/MT`). A few hundred KB against needing the Visual C++
  redistributable on a freshly installed PC.
- **Warnings per target.** `/W4` is set on the `filo` target only. Setting it
  globally and then relaxing it on vendored C makes `cl` complain with D9025.
- **SQLite with `SQLITE_THREADSAFE=2`.** The per-connection mutexes are gone,
  which makes it a promise the code has to keep: a connection never leaves the
  thread that opened it. One writer, and one connection per reader.
