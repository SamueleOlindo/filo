cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)   # needed to pick the CRT through a property
project(filo LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# STATIC CRT (/MT). With the dynamic one the executable depends on MSVCP140.dll
# and VCRUNTIME140.dll, which are part of the Visual C++ Redistributable and on
# a freshly installed PC can be missing: the user would get an error at startup.
# It costs a few hundred KB and keeps the promise of a single executable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (MSVC)
    # EXCEPTIONS FROM extern "C" FUNCTIONS MUST BE CATCHABLE.
    #
    # CMake hands MSVC /EHsc by default. The trailing "c" is not decoration: it
    # tells the compiler to assume extern "C" functions never throw, so it drops
    # the unwinding it would otherwise emit around them. llama.cpp exposes a C
    # API implemented in C++ that throws — its tokenizer raises std::regex_error
    # on input MSVC's std::regex cannot handle — and under /EHsc that throw
    # walks straight past every catch block into std::terminate.
    #
    # It cost a five-hour indexing run to find. /EHs is the same model without
    # the false promise.
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    if (NOT CMAKE_CXX_FLAGS MATCHES "/EHs")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHs")
    endif()

    # /utf-8       : the sources are UTF-8 (otherwise MSVC assumes the local codepage)
    # /permissive- : turn off Microsoft's non-standard extensions
    # Warnings are set PER TARGET: applying them globally and then overriding
    # them on third-party code makes cl complain with D9025.
    add_compile_options(/utf-8 /permissive-)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# SQLite as a static library: 9MB of C compiled once, then only linked.
# No DLL to ship, it ends up inside our own .exe.
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5              # full-text search -> layer [4] of the architecture
    # THREADSAFE=2 drops the per-connection mutexes. It is a PROMISE the code
    # has to keep: a connection must never leave its own thread.
    # There is a single writer, and the readers get one connection each.
    SQLITE_THREADSAFE=2
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DQS=0                    # no double-quoted strings: those are identifiers
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_USE_ALLOCA
    SQLITE_OMIT_DEPRECATED
)

# Snowball: only the runtime plus Italian and English, not the other 28
# languages. BSD-3, like everything else here: nothing owed beyond credit.
# libdeflate: ONLY the decompression path (MIT). No compression, no checksums:
# DOCX, XLSX and PPTX only ever get read.
add_library(libdeflate STATIC
    third_party/libdeflate/lib/deflate_decompress.c
    third_party/libdeflate/lib/utils.c
    third_party/libdeflate/lib/x86/cpu_features.c
)
target_include_directories(libdeflate PUBLIC third_party/libdeflate)
target_compile_definitions(libdeflate PUBLIC LIBDEFLATE_STATIC)

add_library(snowball STATIC
    third_party/snowball/runtime/api.c
    third_party/snowball/runtime/utilities.c
    third_party/snowball/src_c/stem_UTF_8_italian.c
    third_party/snowball/src_c/stem_UTF_8_english.c
)
target_include_directories(snowball PUBLIC third_party)

# PDFium, if it has been built. The project stays buildable without it: PDFs
# simply do not get extracted, instead of stopping anyone who has not done a
# multi-GB Chromium checkout from compiling at all.
#
# Empty by default, and empty means "no PDF support", not an error. To turn it
# on, point this at the pdfium checkout, either with -DPDFIUM_ROOT=<path> or by
# exporting PDFIUM_ROOT before configuring. tools/pdfium_build.bat builds the
# tree this expects and prints the path to use.
set(PDFIUM_ROOT "$ENV{PDFIUM_ROOT}" CACHE PATH "root of the PDFium checkout; empty disables PDF extraction")
set(PDFIUM_LIB "${PDFIUM_ROOT}/out/Release/obj/pdfium.lib")

# PDFium is built with clang-cl and LLVM's standard library (libc++), not with
# Microsoft's: its symbols live in the std::__Cr namespace.
# `pdf_is_complete_lib` collects PDFium's own objects but NOT that runtime,
# which in the build graph is a "source set" and produces no archive at all.
# So it has to be built on its own and packed by hand:
#
#   ninja -C out/Release buildtools/third_party/libc++:libc++
#   lib.exe /OUT:obj/libcxx.lib obj/buildtools/third_party/libc++/libc++/*.obj
#
# The two standard libraries live in the same executable because no C++ type
# crosses the boundary: fpdfview.h is plain C.
set(PDFIUM_CXX_LIB "${PDFIUM_ROOT}/out/Release/obj/libcxx.lib")

option(FILO_DISABLE_PDFIUM "leave PDFium out even when it is present" OFF)

# Say what is wrong HERE, naming the variable, rather than letting the compiler
# fail later on a missing fpdfview.h that names nothing at all.
if (FILO_DISABLE_PDFIUM)
    set(FILO_HAVE_PDFIUM OFF)
    message(STATUS "PDFium off by FILO_DISABLE_PDFIUM: PDFs will not be extracted")
elseif (PDFIUM_ROOT STREQUAL "")
    set(FILO_HAVE_PDFIUM OFF)
    message(STATUS "PDFIUM_ROOT is not set: PDFs will not be extracted. "
                   "Set -DPDFIUM_ROOT=<pdfium checkout> to turn extraction on.")
elseif (NOT EXISTS "${PDFIUM_LIB}")
    message(FATAL_ERROR
        "PDFIUM_ROOT is set to '${PDFIUM_ROOT}', but there is no library at "
        "${PDFIUM_LIB}. Build PDFium with tools/pdfium_build.bat, or pass "
        "-DPDFIUM_ROOT= (empty) to build Filo without PDF extraction.")
elseif (NOT EXISTS "${PDFIUM_ROOT}/public/fpdfview.h")
    message(FATAL_ERROR
        "PDFIUM_ROOT is set to '${PDFIUM_ROOT}', which holds the library but "
        "not ${PDFIUM_ROOT}/public/fpdfview.h. PDFIUM_ROOT has to be the "
        "pdfium checkout itself, not the directory above it.")
else()
    set(FILO_HAVE_PDFIUM ON)
    message(STATUS "PDFium found: ${PDFIUM_LIB}")
endif()

# llama.cpp: the inference engine behind the embeddings. Same deal as PDFium:
# without it the project still compiles and semantic search stays off.
#
# Empty by default, and empty means "no semantic search", not an error. This is
# the working directory of tools/llama_build.bat, not the llama.cpp checkout:
# it holds llama.cpp/ next to out/ or out-vulkan/. Set it with
# -DLLAMA_ROOT=<path> or by exporting LLAMA_ROOT before configuring.
set(LLAMA_ROOT "$ENV{LLAMA_ROOT}" CACHE PATH "root of the llama.cpp build tree; empty disables semantic search")

# The Vulkan build wins when both are there: it accelerates on AMD, Intel and
# NVIDIA from the same code, and the user has nothing to install because the
# runtime comes with the graphics driver. With no card llama.cpp falls back to
# the CPU on its own: it is an addition, not a requirement.
if (LLAMA_ROOT STREQUAL "")
    set(FILO_HAVE_LLAMA OFF)
    message(STATUS "LLAMA_ROOT is not set: semantic search disabled. "
                   "Set -DLLAMA_ROOT=<llama.cpp build root> to turn it on.")
elseif (EXISTS "${LLAMA_ROOT}/out-vulkan/src/llama.lib")
    set(FILO_HAVE_LLAMA ON)
    set(LLAMA_OUT "${LLAMA_ROOT}/out-vulkan")
    set(LLAMA_VULKAN ON)
    message(STATUS "llama.cpp with Vulkan: GPU acceleration on")
elseif (EXISTS "${LLAMA_ROOT}/out/src/llama.lib")
    set(FILO_HAVE_LLAMA ON)
    set(LLAMA_OUT "${LLAMA_ROOT}/out")
    set(LLAMA_VULKAN OFF)
    message(STATUS "llama.cpp CPU only")
else()
    # Set but wrong: fail now, naming the variable and what was looked for.
    message(FATAL_ERROR
        "LLAMA_ROOT is set to '${LLAMA_ROOT}', but neither "
        "${LLAMA_ROOT}/out-vulkan/src/llama.lib nor "
        "${LLAMA_ROOT}/out/src/llama.lib is there. Build llama.cpp with "
        "tools/llama_build.bat (or tools/llama_build_vulkan.bat), or pass "
        "-DLLAMA_ROOT= (empty) to build Filo without semantic search.")
endif()

add_executable(filo
    src/main.cpp
    src/content/chunker.cpp
    src/content/gate.cpp
    src/content/normalize.cpp
    src/content/reader.cpp
    src/content/selector.cpp
    src/extract/api.cpp
    src/extract/epub.cpp
    src/extract/html.cpp
    src/extract/ooxml.cpp
    src/extract/rtf.cpp
    src/extract/xmlscan.cpp
    src/extract/zip.cpp
    src/worker/seh_guard.c
    src/worker/supervisor.cpp
    src/worker/worker_main.cpp
    src/index/file_index.cpp
    src/index/watcher.cpp
    src/ntfs/mft_enumerator.cpp
    src/ntfs/usn_reader.cpp
    src/ntfs/volume.cpp
    src/search/searcher.cpp
    src/search/query_plan.cpp
    src/reclaim/scan.cpp
    src/reclaim/duplicates.cpp
    src/reclaim/classify.cpp
    src/reclaim/recycle.cpp
    src/store/content_db.cpp
    src/store/query.cpp
    src/store/snippet.cpp
    src/store/tokenizer.cpp
    src/ui/webview_window.cpp
    src/vector/vector_store.cpp
    res/filo.rc        # application icon + version information
)
# The file name with a capital letter: it is what shows up in the title bar, in
# Task Manager and in the file properties.
set_target_properties(filo PROPERTIES OUTPUT_NAME "Filo")

if (MSVC)
    target_compile_options(filo PRIVATE /W4)
endif()

if (FILO_HAVE_LLAMA)
    # The library was found, so the headers have to be there too. Check before
    # compiling: a missing llama.h halfway through the build says nothing about
    # which variable is wrong.
    if (NOT EXISTS "${LLAMA_ROOT}/llama.cpp/include/llama.h")
        message(FATAL_ERROR
            "found ${LLAMA_OUT}/src/llama.lib, but no headers at "
            "${LLAMA_ROOT}/llama.cpp/include. LLAMA_ROOT has to hold the "
            "llama.cpp checkout in llama.cpp/ next to the build directory, "
            "which is the layout tools/llama_build.bat produces.")
    endif()
    target_sources(filo PRIVATE src/vector/embedder.cpp src/search/query_model.cpp)
    target_include_directories(filo PRIVATE
        "${LLAMA_ROOT}/llama.cpp/include"
        "${LLAMA_ROOT}/llama.cpp/ggml/include"
    )
    target_compile_definitions(filo PRIVATE FILO_HAVE_LLAMA=1)
    # Order matters: llama needs ggml, ggml needs ggml-cpu and ggml-base.
    target_link_libraries(filo PRIVATE
        "${LLAMA_OUT}/src/llama.lib"
        "${LLAMA_OUT}/ggml/src/ggml.lib"
        "${LLAMA_OUT}/ggml/src/ggml-cpu.lib"
        "${LLAMA_OUT}/ggml/src/ggml-base.lib"
    )
    if (LLAMA_VULKAN)
        target_link_libraries(filo PRIVATE
            "${LLAMA_OUT}/ggml/src/ggml-vulkan/ggml-vulkan.lib"
            vulkan-1.lib
            delayimp.lib
        )
        # vulkan-1.lib lives in the SDK, which is needed ONLY at build time.
        # The SDK installer sets VULKAN_SDK; if it is not in the environment,
        # look in the default install location.
        set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}")
        if (VULKAN_SDK_DIR STREQUAL "")
            file(GLOB VULKAN_SDK_DIRS "C:/VulkanSDK/*")
            if (VULKAN_SDK_DIRS)
                list(GET VULKAN_SDK_DIRS 0 VULKAN_SDK_DIR)
            endif()
        endif()
        if (NOT EXISTS "${VULKAN_SDK_DIR}/Lib/vulkan-1.lib")
            message(FATAL_ERROR
                "the Vulkan build of llama.cpp was found, but the Vulkan SDK "
                "was not: vulkan-1.lib is needed to link. Install the SDK from "
                "https://vulkan.lunarg.com/ and set the VULKAN_SDK environment "
                "variable, or point LLAMA_ROOT at a CPU-only build.")
        endif()
        target_link_directories(filo PRIVATE "${VULKAN_SDK_DIR}/Lib")
        target_compile_definitions(filo PRIVATE FILO_HAVE_VULKAN=1)

        # DELAY LOAD, OR THE EXE DOES NOT START AT ALL WITHOUT A GPU DRIVER.
        #
        # A normal import of vulkan-1.dll is resolved by the Windows loader
        # BEFORE any of our code runs. On a machine with no Vulkan loader — an
        # office PC, a VM, an old laptop — the process is refused outright, with
        # a system dialog and no way for FILO_FORCE_CPU to help: it is read by
        # code that never executes.
        #
        # Delayed, the DLL is only resolved on first use, which gives the
        # program a chance to notice it is missing and stay on the CPU. The
        # fallback stops being a promise and starts being reachable.
        target_link_options(filo PRIVATE "/DELAYLOAD:vulkan-1.dll")
        target_sources(filo PRIVATE src/platform/vulkan_probe.cpp)
    endif()
else()
    # The window calls Embedder and QueryModel whether or not llama.cpp is here,
    # because both are allowed to be absent at runtime and it already handles
    # that. Without this the dependency-free build compiles and then fails to
    # link with eight unresolved externals — which is what anyone cloning the
    # repository would hit first. See src/vector/no_llama.cpp.
    target_sources(filo PRIVATE src/vector/no_llama.cpp)
endif()

if (FILO_HAVE_PDFIUM)
    target_sources(filo PRIVATE src/extract/pdf.cpp)
    target_include_directories(filo PRIVATE "${PDFIUM_ROOT}/public")
    target_compile_definitions(filo PRIVATE FILO_HAVE_PDFIUM=1)
    # Winmm: PDFium calls timeGetTime in its allocator. Not linked by default.
    target_link_libraries(filo PRIVATE "${PDFIUM_LIB}" Winmm.lib)
    # The libc++ runtime is needed ONLY if PDFium was built with its own
    # standard library. With use_custom_libcxx=false it does not exist and is
    # not needed.
    if (EXISTS "${PDFIUM_CXX_LIB}")
        target_link_libraries(filo PRIVATE "${PDFIUM_CXX_LIB}")
    endif()
endif()

target_include_directories(filo PRIVATE
    src
    third_party
    third_party/fts5
    third_party/webview2/include
    third_party/xxhash
)

# WebView2LoaderStatic: the STATIC variant of the loader. The dynamic one would
# force us to ship WebView2Loader.dll next to the executable, breaking the
# single-binary constraint.
target_link_libraries(filo PRIVATE
    sqlite3
    snowball
    libdeflate
    ${CMAKE_SOURCE_DIR}/third_party/webview2/lib/WebView2LoaderStatic.lib
    Shell32.lib ShLwApi.lib Ole32.lib OleAut32.lib Version.lib Advapi32.lib
)
