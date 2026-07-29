# Size/packaging knobs for shipping a single self-contained binary.
#
# Everything here is off by default so a plain `cmake -B build` stays a fast, debuggable dev build.
# `-DVKHB_RELEASE=ON` flips the whole set on at once (LTO + -Os + strip + static runtime + embedded
# assets); each switch is still individually overridable on the same command line.
#
# This file must be included BEFORE cmake/Dependencies.cmake: directory-scoped compile/link options
# only reach subdirectories added afterwards, and LTO is worthless unless SDL, HarfBuzz and volk are
# compiled with it too.

option(VKHB_RELEASE "Umbrella switch: size-optimized, LTO'd, stripped, asset-embedding build" OFF)

# COFF caps an object file at 32k sections, which a C++ translation unit with -ffunction-sections
# reaches easily; HarfBuzz already asks for -Wa,-mbig-obj on its own sources for that reason.
# Applying it everywhere is not just belt-and-braces: GCC's lto-wrapper keeps assembler options only
# when every input object agrees on them, and otherwise drops them with a warning — which then makes
# the LTO link fail on "COFF object format mismatch" as soon as HarfBuzz is in the link.
if(MINGW)
  add_compile_options(-Wa,-mbig-obj)
  # std::print/std::println reach the Windows console through std::__write_to_terminal, which
  # libstdc++ keeps in the separate libstdc++exp archive. Nothing pulls it in automatically, and
  # this project prints from main.cpp on every path. (No such split on Linux — hence mingw-only.)
  link_libraries(stdc++exp)
endif()

set(_vkhb_rel ${VKHB_RELEASE})
# Full static linking is the norm on Windows and a trap on Linux (see the -static block below), so
# only the mingw side of a release build opts into it by default.
# Nobody wants to ship libstdc++-6.dll and libwinpthread-1.dll next to an .exe, and a mingw binary that
# forgets them will not start at all — so on Windows even a dev build links everything in. It has to
# be the full -static: -static-libstdc++/-static-libgcc leave libwinpthread behind.
set(_vkhb_static_full ${MINGW})
set(_vkhb_static_runtime ${_vkhb_rel})
if(MINGW)
  set(_vkhb_static_runtime ON)
endif()

option(VKHB_LTO           "Link-time optimization across the app and every fetched dependency" ${_vkhb_rel})
option(VKHB_OPTIMIZE_SIZE "Optimize for size (-Os, no debug info, --gc-sections)"              ${_vkhb_rel})
option(VKHB_STRIP         "Strip the symbol table from linked binaries"                        ${_vkhb_rel})
option(VKHB_STATIC_RUNTIME "Link libstdc++/libgcc into the binary"                    ${_vkhb_static_runtime})
option(VKHB_STATIC_LINK   "Fully static link (-static): no libc either"                        ${_vkhb_static_full})
option(VKHB_EMBED_ASSETS  "Bundle shaders and fonts into the executable as one xz archive"     ${_vkhb_rel})

# --- LTO -----------------------------------------------------------------------------------------
# Not CMake's INTERPROCEDURAL_OPTIMIZATION: that property is per-target, so it would miss the
# fetched dependencies, and it picks its own flags rather than the two the size budget wants.
# GCC's -flto-partition=one puts the whole program in one partition — slower to link, but it is what
# lets the size-oriented inliner and identical-code folding see everything at once.
if(VKHB_LTO)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_vkhb_lto_flags -flto=full)
    # Clang's bitcode is only readable by a plugin of the same LLVM version. Letting the link fall
    # through to GNU ld picks up whatever LLVMgold.so the system has, which fails with "Unknown
    # attribute kind" whenever the system LLVM is older than the compiler. lld ships with the
    # compiler, so it always matches; same reason llvm-ar/ranlib/nm rather than the binutils ones.
    get_filename_component(_vkhb_cc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(_vkhb_lld NAMES ld.lld HINTS "${_vkhb_cc_dir}")
    if(_vkhb_lld)
      add_link_options(-fuse-ld=lld)
    endif()
    foreach(tool AR RANLIB NM)
      string(TOLOWER ${tool} _lc)
      find_program(_vkhb_llvm_${_lc} NAMES "llvm-${_lc}" HINTS "${_vkhb_cc_dir}")
      if(_vkhb_llvm_${_lc})
        set(CMAKE_${tool} "${_vkhb_llvm_${_lc}}" CACHE FILEPATH "LTO-plugin-aware ${_lc}" FORCE)
      endif()
    endforeach()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_vkhb_lto_flags -flto -flto-partition=one)
    # GCC emits slim LTO objects, whose symbol tables only binutils' LTO plugin can read. The
    # gcc-ar/nm/ranlib wrappers load that plugin; plain ar silently produces archives the linker
    # then reports as "has no symbols".
    get_filename_component(_vkhb_cc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_vkhb_cc_name "${CMAKE_CXX_COMPILER}" NAME)
    string(REGEX REPLACE "g\\+\\+(-[0-9.]+)?$" "gcc" _vkhb_tool_prefix "${_vkhb_cc_name}")
    foreach(tool AR RANLIB NM)
      string(TOLOWER ${tool} _lc)
      find_program(_vkhb_${_lc} NAMES "${_vkhb_tool_prefix}-${_lc}" HINTS "${_vkhb_cc_dir}")
      if(_vkhb_${_lc})
        set(CMAKE_${tool} "${_vkhb_${_lc}}" CACHE FILEPATH "LTO-plugin-aware ${_lc}" FORCE)
      endif()
    endforeach()
  else()
    message(FATAL_ERROR "VKHB_LTO only knows GCC and Clang flags, not ${CMAKE_CXX_COMPILER_ID}")
  endif()
  add_compile_options(${_vkhb_lto_flags})
  add_link_options(${_vkhb_lto_flags})
  # The link step re-runs the optimizers, so it needs the same -O level the objects were built at.
  # Without this GCC links LTO bytecode at -O0 and the size win evaporates.
  if(VKHB_OPTIMIZE_SIZE)
    add_link_options(-Os)
  endif()
endif()

# --- Size ----------------------------------------------------------------------------------------
# -Os is appended after CMAKE_<LANG>_FLAGS_<CONFIG>, and the last -O wins, so this overrides the
# build type's -O2/-O3 without having to fight CMAKE_BUILD_TYPE. -g0 does the same for -g.
if(VKHB_OPTIMIZE_SIZE)
  add_compile_options(-Os -g0 -fno-ident -ffunction-sections -fdata-sections)
  add_link_options(LINKER:--gc-sections)
endif()

# --- Strip ---------------------------------------------------------------------------------------
# The linker's -s rather than a post-build strip(1): one less tool to locate, and it is the same
# spelling for the native and the mingw toolchain.
if(VKHB_STRIP)
  add_link_options(-s)
endif()

# --- Static linking ------------------------------------------------------------------------------
# -static-libstdc++/-static-libgcc are always safe and remove the two libraries most likely to be
# missing or too old on someone else's machine.
#
# -static (no libc either) is right on mingw, where it also absorbs libwinpthread. On Linux it is a
# footgun: SDL reaches X11/Wayland/PipeWire through dlopen(), and dlopen() in a statically linked
# glibc binary needs the exact glibc build it was linked against present at runtime — which defeats
# the point. It stays available for people who know they want it, with a warning.
if(VKHB_STATIC_RUNTIME OR VKHB_STATIC_LINK)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()
if(VKHB_STATIC_LINK)
  if(UNIX AND NOT APPLE)
    message(WARNING "VKHB_STATIC_LINK on Linux: SDL dlopen()s its X11/Wayland backends, which a "
                    "statically linked glibc cannot do portably. Prefer VKHB_STATIC_RUNTIME.")
  endif()
  add_link_options(-static)
endif()
