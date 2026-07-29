# All third-party code is fetched from source at configure time and built as part of this tree —
# no system packages, no package manager, one pinned revision per dependency (below). Nothing here
# is installed; everything is EXCLUDE_FROM_ALL + SYSTEM so dependency headers never emit warnings
# under this project's own -Wall -Wextra and unused dep targets never get built.
#
# Offline / local checkout: point any dependency at an existing source tree instead of downloading
# it with the standard FetchContent escape hatch, e.g.
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_HARFBUZZ=/path/to/harfbuzz
# and pass -DFETCHCONTENT_FULLY_DISCONNECTED=ON once every dependency has a local source dir.

include(FetchContent)

set(VKHB_SDL_TAG       "release-3.4.12"      CACHE STRING "SDL3 git tag")
set(VKHB_VOLK_TAG      "vulkan-sdk-1.4.350.1" CACHE STRING "volk git tag")
set(VKHB_VULKAN_HEADERS_TAG "vulkan-sdk-1.4.350.1" CACHE STRING "Vulkan-Headers git tag (fallback when no SDK is installed)")
set(VKHB_XZ_EMBEDDED_TAG "v2024-12-30"       CACHE STRING "xz-embedded git tag")
set(VKHB_HARFBUZZ_TAG  "14.2.1"              CACHE STRING "HarfBuzz git tag (must be >= 14.0, when hb-gpu landed)")
set(VKHB_GLM_TAG       "1.0.1"               CACHE STRING "glm git tag")
set(VKHB_INTER_VERSION "4.1"                 CACHE STRING "Inter font release version")
set(VKHB_SDL_MIXER_URL "https://github.com/libsdl-org/SDL_mixer/archive/refs/tags/release-3.2.4.tar.gz"
    CACHE STRING "SDL3_mixer source archive")
set(VKHB_SDL_MIXER_SHA256 "f2ea848ccdf2f394cd4973ee0f6c482e04511044695cccfd46bab6dcd7f780aa"
    CACHE STRING "SHA256 of VKHB_SDL_MIXER_URL")
# The three codec libraries behind the Opus decoder, pinned to the exact commits SDL_mixer
# release-3.2.4 points its external/ submodules at. They are libsdl-org forks rather than upstream
# Xiph on purpose: upstream opusfile is autotools-only, and these carry the CMake support SDL_mixer
# add_subdirectory()s. Read the pins back out of any tag with
#   curl -s https://api.github.com/repos/libsdl-org/SDL_mixer/git/trees/<tag> ...
set(VKHB_OGG_COMMIT      "936fdd822a4e4fc963197b3eda931b89b52859a0" CACHE STRING "libsdl-org/ogg commit")
set(VKHB_OGG_SHA256      "30e1012c1a1909f61e1ef27dc72fe072da51641ea826ba32e1f4ebfd3fc24419" CACHE STRING "")
set(VKHB_OPUS_COMMIT     "ac9f053e1db9cc1c7608b74e029c25ded0254e3e" CACHE STRING "libsdl-org/opus commit")
set(VKHB_OPUS_SHA256     "5e3d4e850b5dc517f0215be8f96fc345cdb66ac4ded5871d36c20ffbeba62c0e" CACHE STRING "")
set(VKHB_OPUSFILE_COMMIT "9e5322c62de32d2b2ad88324b3ad311174cec41d" CACHE STRING "libsdl-org/opusfile commit")
set(VKHB_OPUSFILE_SHA256 "80cbf6b0c83abc0ac5382d30828703047b08bc9ff7efbc48e0fc397f9839710b" CACHE STRING "")

# --- SDL3: window + input + Vulkan surface creation. Static only; the shared lib and the test
# library would just be dead weight next to a single self-contained executable.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)
# This template talks to Vulkan itself and only asks SDL for a window, an event queue and a
# VkSurfaceKHR. Everything below is therefore dead weight — most of all SDL_RENDER, which is what
# pulls in the Direct3D 9/11/12 and Vulkan render drivers on Windows (SDL_DIRECTX=OFF removes the
# D3D headers' reach entirely). SDL_VULKAN must stay ON: it is SDL_Vulkan_CreateSurface, not a
# render backend, and nothing starts without it. Turn any of these back on if your app needs them.
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_DIRECTX OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_KMSDRM OFF CACHE BOOL "" FORCE)  # KMSDRM is an EGL-only backend; it will not build without GLES
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_OFFSCREEN OFF CACHE BOOL "" FORCE)  # headless rendering here is surfaceless Vulkan, not SDL
# Audio is the one unused subsystem left switchable rather than simply off: it is the subsystem an
# app built on this template is most likely to want back.
option(VKHB_SDL_AUDIO "Keep SDL's audio subsystem (unused by the demo)" ON)
set(SDL_AUDIO ${VKHB_SDL_AUDIO} CACHE BOOL "" FORCE)

# SDL's optional X11 extension checks are hard errors, not soft feature probes: a Linux box without
# libXtst installed fails the *configure* step outright rather than building without XTEST. XTEST
# only drives SDL's synthetic-input path, which nothing here uses, so refuse to let it gate the
# build. Flip these ON if you actually need them (and install the matching -devel packages).
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG ${VKHB_SDL_TAG}
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL SYSTEM
)

# --- volk: the Vulkan meta-loader. It dlopen()s/LoadLibrary()s the ICD at runtime, so all it needs
# at build time is the Vulkan headers — no loader library, which is what makes cross-compiling to
# Windows possible without a Windows SDK. VOLK_PULL_IN_VULKAN does find_package(Vulkan) and puts the
# system SDK's headers on volk's interface; when there is no SDK (the usual case when
# cross-compiling) the headers get fetched instead and wired on by hand below.
find_package(Vulkan QUIET)
set(VKHB_FETCH_VULKAN_HEADERS_DEFAULT ON)
if(Vulkan_FOUND AND NOT CMAKE_CROSSCOMPILING)
  set(VKHB_FETCH_VULKAN_HEADERS_DEFAULT OFF)
endif()
option(VKHB_FETCH_VULKAN_HEADERS "Fetch Vulkan-Headers instead of using an installed Vulkan SDK"
       ${VKHB_FETCH_VULKAN_HEADERS_DEFAULT})

if(VKHB_FETCH_VULKAN_HEADERS)
  set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
  set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG ${VKHB_VULKAN_HEADERS_TAG}
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL SYSTEM
  )
else()
  set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG ${VKHB_VOLK_TAG}
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL SYSTEM
)

# --- HarfBuzz: text shaping plus the experimental libharfbuzz-gpu ("Slug"-style GPU glyph
# encoding) this template's text renderer is built on — HB_BUILD_GPU is the one non-default knob
# that matters. Subsetting/rasterization/CLI utils are all unused here.
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU ON CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
# CoreText is HarfBuzz's macOS system-font backend; this template only ever shapes with the OpenType
# backend on font files it loads itself, so it is dead weight — and it does not build here. Its
# headers pull in ApplicationServices, whose CF_ENUM macro expands to `enum E : type E;`, a
# redeclaration form no standard C++ mode accepts (-Welaborated-enum-base, an error by default).
# Apple Clang has an exemption for its own SDK; the Homebrew LLVM this project builds with does not.
set(HB_HAVE_CORETEXT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(harfbuzz
  GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
  GIT_TAG ${VKHB_HARFBUZZ_TAG}
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL SYSTEM
)

# --- SDL3_mixer: the codecs SDL itself does not have. Core SDL decodes WAV and nothing else, so
# every compressed format the app plays comes from here.
option(VKHB_AUDIO "Build vkhb_audio (SDL3_mixer: Ogg Opus, Ogg Vorbis, WAV)" ON)
if(VKHB_AUDIO AND NOT VKHB_SDL_AUDIO)
  message(FATAL_ERROR "VKHB_AUDIO needs VKHB_SDL_AUDIO=ON: SDL_mixer has no audio device to mix to "
                      "when SDL's audio subsystem is compiled out. Set -DVKHB_AUDIO=OFF instead.")
endif()

if(VKHB_AUDIO)
  # SDL_mixer's BUILD_SHARED_LIBS defaults to ON, and it is a plain cache option — left alone it
  # would flip the whole build to shared libraries from this point on, including the static-archive
  # assumption the harfbuzz-gpu fixup below depends on. Everything here is static.
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  # Opus is the one decoder here that needs a real library behind it (libopus + opusfile + libogg).
  # Vendored, those are fetched and compiled into the binary like everything else; unvendored, they
  # come from the system, which means three more .so files next to a build that is otherwise one
  # self-contained executable. Vendoring costs ~1.2 MB of source and about twenty seconds of build.
  option(VKHB_AUDIO_VENDORED "Build libopus/opusfile/libogg from source instead of using the system's" ON)
  set(SDLMIXER_VENDORED ${VKHB_AUDIO_VENDORED} CACHE BOOL "" FORCE)
  # Link the codecs at build time instead of dlopen()ing them at runtime: a missing library then
  # fails the link rather than silently turning into "no Opus decoder" at startup.
  set(SDLMIXER_DEPS_SHARED OFF CACHE BOOL "" FORCE)
  # And with STRICT, a codec that was asked for but cannot be found fails *configure* instead of
  # quietly dropping out of the build.
  set(SDLMIXER_STRICT ON CACHE BOOL "" FORCE)
  set(SDLMIXER_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)

  # Codecs. Opus is what the demo plays; stb_vorbis costs nothing extra (it is a single in-tree
  # source file, no external library) and covers the other half of what "an .ogg" tends to mean;
  # WAVE is SDL_mixer's own uncompressed path. Everything else is off — turn one back on here if you
  # need it, and check whether it drags in a system library first (FLAC/MP3 have dependency-free
  # dr_flac/dr_mp3 backends; MOD, GME, MIDI-via-FluidSynth and WavPack do not).
  set(SDLMIXER_OPUS ON CACHE BOOL "" FORCE)
  set(SDLMIXER_VORBIS_STB ON CACHE BOOL "" FORCE)
  set(SDLMIXER_WAVE ON CACHE BOOL "" FORCE)
  set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_VORBIS_TREMOR OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_AIFF OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_AU OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_VOC OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_FLAC OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_GME OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_MOD OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_MP3 OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_MIDI OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)

  # SDL_mixer expects its vendored codecs as git submodules under external/, which a GitHub-generated
  # source tarball leaves as empty directories. They are filled in below instead — so this declare
  # must *populate without configuring*, or SDL_mixer would try to add_subdirectory(external/opus)
  # while it is still empty. SOURCE_SUBDIR naming a directory with no CMakeLists.txt in it is how
  # FetchContent_MakeAvailable is told to download and extract only; the real add_subdirectory
  # happens by hand further down.
  FetchContent_Declare(SDL3_mixer
    URL ${VKHB_SDL_MIXER_URL}
    URL_HASH SHA256=${VKHB_SDL_MIXER_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR .populate-only
    EXCLUDE_FROM_ALL SYSTEM
  )
endif()

# --- glm: vector/matrix math. Header-only in practice; its tests/install rules are off.
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)  # glm has no C++23 knob; 20 mode is a strict subset
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG ${VKHB_GLM_TAG}
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL SYSTEM
)

# --- Inter (OFL-1.1): the static TTFs the demo loads at runtime. This archive has no CMakeLists,
# so FetchContent just unpacks it and VKHB_FONT_DIR below points at the extracted TTFs.
FetchContent_Declare(inter
  URL https://github.com/rsms/inter/releases/download/v${VKHB_INTER_VERSION}/Inter-${VKHB_INTER_VERSION}.zip
  URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL SYSTEM
)

# --- xz-embedded (0BSD): Lasse Collin's decoder-only XZ implementation, ~40 KiB of object code, so
# a VKHB_EMBED_ASSETS build can carry its assets compressed and unpack them at startup. Only fetched
# when that is on. It ships no CMakeLists — the three translation units the single-shot decoder
# needs are compiled directly below (no BCJ filters, no CRC64: see the --check=crc32 in
# cmake/PackAssets.cmake).
if(VKHB_EMBED_ASSETS)
  FetchContent_Declare(xz_embedded
    GIT_REPOSITORY https://github.com/tukaani-project/xz-embedded.git
    GIT_TAG ${VKHB_XZ_EMBEDDED_TAG}
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL SYSTEM
  )
  FetchContent_MakeAvailable(xz_embedded)

  add_library(vkhb_xz STATIC
    ${xz_embedded_SOURCE_DIR}/linux/lib/xz/xz_crc32.c
    ${xz_embedded_SOURCE_DIR}/linux/lib/xz/xz_dec_stream.c
    ${xz_embedded_SOURCE_DIR}/linux/lib/xz/xz_dec_lzma2.c
  )
  target_include_directories(vkhb_xz SYSTEM PUBLIC
    ${xz_embedded_SOURCE_DIR}/linux/include/linux
    ${xz_embedded_SOURCE_DIR}/userspace
  )
  # Vendored third-party C: keep it out of this project's warning budget.
  target_compile_options(vkhb_xz PRIVATE -w)
endif()

if(VKHB_FETCH_VULKAN_HEADERS)
  FetchContent_MakeAvailable(VulkanHeaders)
endif()

FetchContent_MakeAvailable(SDL3 volk harfbuzz glm inter)

# Separate from the list above so SDL3's targets already exist when SDL_mixer configures: it looks
# for SDL3::SDL3 in the current build first and only falls back to find_package() if there is none.
if(VKHB_AUDIO)
  # Step 1: unpack SDL_mixer (populate-only, see the declare above).
  FetchContent_MakeAvailable(SDL3_mixer)

  # Step 2: fill in the codec submodule directories the tarball left empty. Each is declared with
  # SOURCE_DIR pointing straight into SDL_mixer's external/, so the archive lands exactly where
  # SDL_mixer's vendored path looks for it, and with the same populate-only SOURCE_SUBDIR — these
  # get add_subdirectory()d by SDL_mixer itself, in the order it needs (ogg, then opus, then
  # opusfile), not by us.
  if(VKHB_AUDIO_VENDORED)
    foreach(codec ogg opus opusfile)
      string(TOUPPER ${codec} codec_upper)
      FetchContent_Declare(vkhb_${codec}
        URL "https://github.com/libsdl-org/${codec}/archive/${VKHB_${codec_upper}_COMMIT}.tar.gz"
        URL_HASH SHA256=${VKHB_${codec_upper}_SHA256}
        SOURCE_DIR "${sdl3_mixer_SOURCE_DIR}/external/${codec}"
        SOURCE_SUBDIR .populate-only
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL SYSTEM
      )
      FetchContent_MakeAvailable(vkhb_${codec})
    endforeach()

    # libopus builds its demo/repacketizer executables and a test suite by default; none of that is
    # wanted inside another project. (SDL_mixer already sets BUILD_PROGRAMS for its own sake.)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)  # libogg
  endif()

  # Step 3: now that external/ is whole, configure SDL_mixer for real.
  add_subdirectory("${sdl3_mixer_SOURCE_DIR}" "${sdl3_mixer_BINARY_DIR}" EXCLUDE_FROM_ALL SYSTEM)

  # Step 4: give libopus's SSE4.1 sources their target feature back under clang-cl. libopus attaches
  # -msse4.1 per source file, but guards that on `NOT MSVC` because cl.exe accepts any intrinsic
  # regardless of /arch. CMake also sets MSVC for clang-cl, and clang does *not*: an intrinsic whose
  # target feature is off is a hard error ("always_inline function '_mm_shuffle_epi8' requires
  # target feature 'ssse3'"), so every *_sse4_1.c fails to compile. Re-add the flag exactly where
  # upstream puts it — per file, not per target — so the runtime dispatch libopus is built with
  # (OPUS_HAVE_RTCD) keeps choosing these paths only on CPUs that have SSE4.1, and the generic code
  # around them stays baseline. /clang: is how clang-cl is told to forward a driver option it does
  # not itself list; SSE4.1 has no /arch: spelling, unlike AVX.
  if(TARGET opus AND MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(vkhb_opus_dir "${sdl3_mixer_SOURCE_DIR}/external/opus")
    get_target_property(vkhb_opus_sources opus SOURCES)
    set(vkhb_opus_sse4_1_sources "")
    foreach(src IN LISTS vkhb_opus_sources)
      if(src MATCHES "_sse4_1\\.c$")
        cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY "${vkhb_opus_dir}" NORMALIZE
                   OUTPUT_VARIABLE vkhb_opus_src_abs)
        list(APPEND vkhb_opus_sse4_1_sources "${vkhb_opus_src_abs}")
      endif()
    endforeach()
    if(vkhb_opus_sse4_1_sources)
      # DIRECTORY scope: source file properties are only visible to targets in the directory that
      # set them, and `opus` lives in its own.
      set_source_files_properties(${vkhb_opus_sse4_1_sources}
        DIRECTORY "${vkhb_opus_dir}"
        PROPERTIES COMPILE_OPTIONS "/clang:-msse4.1")
    elseif(vkhb_opus_sources MATCHES "/x86/")
      # x86 intrinsics are in the build but the files this fixup keys on are not — upstream renamed
      # something. Say so here rather than letting it surface as a wall of clang errors.
      message(WARNING "libopus was configured with x86 intrinsics but no *_sse4_1.c sources were "
                      "found to flag for clang-cl; the opus build is likely to fail.")
    endif()
  endif()
endif()

# volk was built with VOLK_PULL_IN_VULKAN off in the fetched-headers case, so it has no Vulkan
# headers on its interface yet. Both of volk's targets need them: volk::volk for the compiled
# loader, volk_headers for header-only consumers.
if(VKHB_FETCH_VULKAN_HEADERS)
  foreach(volk_target volk volk_headers)
    if(TARGET ${volk_target})
      # volk_headers is an INTERFACE library, which rejects the PUBLIC keyword.
      get_target_property(volk_kind ${volk_target} TYPE)
      if(volk_kind STREQUAL "INTERFACE_LIBRARY")
        target_link_libraries(${volk_target} INTERFACE Vulkan::Headers)
      else()
        target_link_libraries(${volk_target} PUBLIC Vulkan::Headers)
      endif()
    endif()
  endforeach()
endif()

# A note on SDL and LTO, since it is the obvious thing to worry about here: SDL routes every public
# entry point through the dynamic-API jump table in src/dynapi, and SDL_dynapi.h #errors out if you
# try to define SDL_DYNAMIC_API=0 from the command line — that switch is deliberately source-edit
# only. It is not actually a problem: the table, its initializer and the real implementations all
# sit in one translation unit that LTO sees whole, and taking a function's address there keeps it
# alive rather than confusing anything. So this build leaves the shim alone and links SDL with LTO
# like everything else. (SDL_STATIC_LIB is defined by SDL3::SDL3-static's interface already.)

# harfbuzz-gpu compiles hb-static.cc, which libharfbuzz already contains: harmless when harfbuzz is
# a shared library, a wall of "multiple definition of _hb_NullPool" when (as here) both are static
# archives linked into one binary. Drop that one source; the target keeps its own compile options.
if(NOT BUILD_SHARED_LIBS AND TARGET harfbuzz-gpu)
  get_target_property(_hb_gpu_sources harfbuzz-gpu SOURCES)
  list(REMOVE_ITEM _hb_gpu_sources "${harfbuzz_SOURCE_DIR}/src/hb-static.cc")
  set_target_properties(harfbuzz-gpu PROPERTIES SOURCES "${_hb_gpu_sources}")
endif()

# Consumed by src/CMakeLists.txt: the -I root for hb-gpu's .glsl library sources (ui_text.vert/frag
# #include them textually) and the directory the demo's fonts are staged from.
set(VKHB_HARFBUZZ_SHADER_INCLUDE_DIR "${harfbuzz_SOURCE_DIR}/src")
set(VKHB_FONT_DIR "${inter_SOURCE_DIR}/extras/ttf")
set(VKHB_FONT_LICENSE "${inter_SOURCE_DIR}/LICENSE.txt")
