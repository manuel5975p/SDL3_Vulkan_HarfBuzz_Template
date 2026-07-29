# Windowless unit tests only: nothing here creates a device or a window, so `ctest` runs headless.
# Each file is its own executable with its own main(); CTest gives each a timeout so a hang fails
# fast instead of blocking the suite.

function(vkhb_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE vkhb_ui vkhb_compile_flags)
  set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

vkhb_add_test(test_format)
vkhb_add_test(test_text_wrap)

# test_assets depends on the staged shaders/fonts, and in the loose-files build it needs to be told
# where they landed — hence the extra argument and the build dependency. In an embedded build both
# are inert but harmless, which is exactly what makes one test cover both backends.
add_executable(test_assets test_assets.cpp)
target_link_libraries(test_assets PRIVATE vkhb_assets vkhb_compile_flags)
set_target_properties(test_assets PROPERTIES CXX_EXTENSIONS OFF)
add_dependencies(test_assets vkhb_shaders vkhb_fonts)
add_test(NAME test_assets COMMAND test_assets "${CMAKE_BINARY_DIR}/src")
set_tests_properties(test_assets PROPERTIES TIMEOUT 30)

# test_audio mixes without an audio device (Engine::open_silent), so it runs on a machine with no
# sound server. It is handed the staged-asset directory like test_assets, plus — only when there is
# actually a music file in assets/ — the name to play, so an empty assets/ still leaves a passing
# suite.
if(VKHB_AUDIO)
  add_executable(test_audio test_audio.cpp)
  target_link_libraries(test_audio PRIVATE vkhb_audio vkhb_compile_flags)
  set_target_properties(test_audio PROPERTIES CXX_EXTENSIONS OFF)
  add_dependencies(test_audio vkhb_fonts vkhb_loose_assets)

  set(test_audio_args "${CMAKE_BINARY_DIR}/src")
  if(EXISTS "${CMAKE_SOURCE_DIR}/assets/embrace.ogg")
    list(APPEND test_audio_args "embrace.ogg")
  endif()
  add_test(NAME test_audio COMMAND test_audio ${test_audio_args})
  set_tests_properties(test_audio PROPERTIES TIMEOUT 60)
endif()
