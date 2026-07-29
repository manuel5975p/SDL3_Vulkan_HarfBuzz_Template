# SDL3 + Vulkan + HarfBuzz-GPU application template.
# Layout: cmake/BuildOptions.cmake holds the size/packaging switches; cmake/Dependencies.cmake
# fetches every third-party library; cmake/CompileFlags.cmake carries this project's warning flags.
# src/ builds four static libraries (vkhb_assets: runtime asset lookup; vkhb_render: Vulkan
# plumbing; vkhb_ui: HarfBuzz text + immediate-mode 2D UI; vkhb_audio: SDL3_mixer playback, only
# when VKHB_AUDIO is on) plus a demo executable; tests/ holds windowless unit tests, gated on
# BUILD_TESTING.
cmake_minimum_required(VERSION 3.28)  # FetchContent_Declare(EXCLUDE_FROM_ALL) needs 3.28
project(vkhb_template LANGUAGES C CXX)

# No target here is a C++20 module. Without this, every target that asks for cxx_std_20 or newer
# gets a module-dependency scan: an extra pass per source plus -fmodules-ts/-fmodule-mapper in the
# compile line, which slows the build and confuses clangd reading compile_commands.json. Set as a
# directory-wide variable rather than per target on purpose — see AGENTS.md.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

include(CTest)  # defines BUILD_TESTING (default ON), the single switch for every test below

# Before Dependencies.cmake on purpose: LTO and -Os are directory-scoped options, which only reach
# subdirectories added *afterwards*, and they have to reach SDL, HarfBuzz and volk too. Moving this
# include below the next one breaks the size numbers without breaking the build — see
# docs/packaging.md.
include(cmake/BuildOptions.cmake)
include(cmake/Dependencies.cmake)

# Warning flags are carried by an INTERFACE target that only this project's own targets link
# against, so fetched dependency sources stay quiet and no flag spelling is forced on a toolchain
# that does not understand it.
include(cmake/CompileFlags.cmake)

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
