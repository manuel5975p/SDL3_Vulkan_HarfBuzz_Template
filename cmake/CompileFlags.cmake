# The warning/encoding flags this project's own targets are built with, as one INTERFACE target.
#
# Deliberately not add_compile_options(): that is directory-scoped, so it would also land on every
# dependency added from this directory downwards, and it forces one compiler's flag spelling on
# everyone. Link vkhb_compile_flags PRIVATE into each target defined by this project instead —
# nothing else picks it up, and each toolchain gets its own spelling below.

add_library(vkhb_compile_flags INTERFACE)

if(MSVC)
  # /W4 is MSVC's -Wall -Wextra; MSVC's own /Wall is unusably noisy about system headers.
  target_compile_options(vkhb_compile_flags INTERFACE /W4)
  # Sources are UTF-8 and contain non-ASCII in comments and in a few literals ("×"). Without this
  # cl.exe decodes them in the machine's ANSI codepage: C4819 everywhere and mojibake in the output.
  target_compile_options(vkhb_compile_flags INTERFACE /utf-8)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl maps /W4 onto -Wall -Wextra, so it needs the exemption below like any other clang.
    target_compile_options(vkhb_compile_flags INTERFACE -Wno-missing-field-initializers)
  endif()
else()
  # Designated initializers deliberately leave trailing Vulkan struct fields at their zero default;
  # that's idiomatic here, so -Wmissing-field-initializers would be noise on every Vk*Info.
  target_compile_options(vkhb_compile_flags INTERFACE -Wall -Wextra -Wno-missing-field-initializers)
endif()
