# Cross-compile to 64-bit Windows with the mingw-w64 GCC installed on this machine.
#
#   cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
#         -DVKHB_RELEASE=ON
#   cmake --build build-win
#
# No Windows SDK and no Vulkan SDK are needed: volk LoadLibrary()s vulkan-1.dll at runtime, so the
# build only wants Vulkan headers, and cmake/Dependencies.cmake fetches those automatically when
# cross-compiling. glslc still runs on the host — it is a build tool, not a target dependency.
#
# The resulting vkhb_demo.exe is a console application, so --headless / --list-gpus output lands in
# the terminal it was started from. With VKHB_RELEASE=ON it is fully static and carries its assets,
# i.e. it is the only file that has to be copied to the target machine.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(VKHB_MINGW_TRIPLE x86_64-w64-mingw32 CACHE STRING "mingw-w64 target triple")

set(CMAKE_C_COMPILER   ${VKHB_MINGW_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${VKHB_MINGW_TRIPLE}-g++)
set(CMAKE_RC_COMPILER  ${VKHB_MINGW_TRIPLE}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${VKHB_MINGW_TRIPLE})
# Headers and libraries come from the mingw sysroot; programs (glslc, xz, git) from the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
