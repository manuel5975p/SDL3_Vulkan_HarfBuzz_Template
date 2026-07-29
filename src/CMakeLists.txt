# --- vkhb_assets: the one place that knows where runtime assets live. Exactly one backend is
# compiled in — a directory next to the executable, or an xz archive linked into the binary — so
# every other library can ask for "shaders/foo.spv" and stay ignorant of how this build ships.
# The embedded blob and its size header are generated below, once the assets have been staged.
if(VKHB_EMBED_ASSETS)
  add_library(vkhb_assets STATIC assets/assets_embedded.cpp)
  target_link_libraries(vkhb_assets PRIVATE vkhb_xz)
  target_include_directories(vkhb_assets PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  # #embed resolves <...> against --embed-dir, a search path of its own that -I does not feed.
  target_compile_options(vkhb_assets PRIVATE "--embed-dir=${CMAKE_CURRENT_BINARY_DIR}/generated")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(vkhb_assets PRIVATE -Wno-c23-extensions)  # #embed is C++26, used early
  endif()
else()
  add_library(vkhb_assets STATIC assets/assets_dir.cpp)
endif()
target_link_libraries(vkhb_assets PRIVATE vkhb_compile_flags)
target_include_directories(vkhb_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vkhb_assets PUBLIC cxx_std_23)
set_target_properties(vkhb_assets PROPERTIES CXX_EXTENSIONS OFF)

# --- vkhb_render: Vulkan plumbing with no UI/text knowledge — instance/device selection (render
# and present devices may differ), swapchain, the Presenter that gets an offscreen frame onto the
# screen, the surfaceless headless target with the same frame shape, dedicated-allocation
# buffers/images, SPIR-V module loading.
add_library(vkhb_render STATIC
  render/vk_context.cpp
  render/swapchain.cpp
  render/present.cpp
  render/headless.cpp
  render/vk_buffer.cpp
  render/vk_image.cpp
  render/shader.cpp
)
target_link_libraries(vkhb_render PUBLIC volk::volk SDL3::SDL3-static glm::glm vkhb_assets)
target_link_libraries(vkhb_render PRIVATE vkhb_compile_flags)
target_include_directories(vkhb_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vkhb_render PUBLIC cxx_std_23)
set_target_properties(vkhb_render PROPERTIES CXX_EXTENSIONS OFF)

# --- vkhb_ui: the reusable part of this template — HarfBuzz shaping + hb-gpu glyph encoding
# (ui_font), the two screen-space pipelines (ui_pipeline), and the immediate-mode
# rect/line/circle/text API on top (ui_renderer), plus small helpers built only from that API
# (theme colors, word wrap, number formatting, modal panel chrome).
add_library(vkhb_ui STATIC
  ui/ui_font.cpp
  ui/ui_pipeline.cpp
  ui/ui_renderer.cpp
  ui/text_wrap.cpp
  ui/format.cpp
  ui/panel_chrome.cpp
)
target_link_libraries(vkhb_ui PUBLIC vkhb_render harfbuzz harfbuzz-gpu glm::glm vkhb_assets)
target_link_libraries(vkhb_ui PRIVATE vkhb_compile_flags)
target_compile_features(vkhb_ui PUBLIC cxx_std_23)
set_target_properties(vkhb_ui PROPERTIES CXX_EXTENSIONS OFF)

# --- vkhb_audio: SDL3_mixer behind the same logical-asset addressing everything else uses, so a
# sound plays from a loose file or from the embedded archive without the caller knowing which.
if(VKHB_AUDIO)
  add_library(vkhb_audio STATIC audio/audio.cpp)
  target_link_libraries(vkhb_audio PUBLIC SDL3_mixer::SDL3_mixer SDL3::SDL3-static vkhb_assets)
  target_link_libraries(vkhb_audio PRIVATE vkhb_compile_flags)
  target_include_directories(vkhb_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_features(vkhb_audio PUBLIC cxx_std_23)
  set_target_properties(vkhb_audio PROPERTIES CXX_EXTENSIONS OFF)
endif()

# --- Shaders: compiled to SPIR-V at build time and staged into the build tree, where the
# loose-files asset backend finds them next to the executable (no install step needed for local
# dev) and the asset packer picks them up for an embedded build.
# The second -I covers hb-gpu's own .glsl library sources, which ui_text.vert/frag #include
# directly (they are meant to be textually concatenated into a caller's shader, not compiled
# standalone).
find_program(GLSLC glslc REQUIRED)
set(VKHB_SHADER_SOURCES
  render/shaders/ui_rect.vert
  render/shaders/ui_rect.frag
  render/shaders/ui_text.vert
  render/shaders/ui_text.frag
)
set(VKHB_SHADER_OUTPUTS "")
set(VKHB_ASSET_MANIFEST_LINES "")
foreach(shader ${VKHB_SHADER_SOURCES})
  get_filename_component(shader_name ${shader} NAME)
  set(shader_out "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader_name}.spv")
  add_custom_command(
    OUTPUT ${shader_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND ${GLSLC} -I${CMAKE_CURRENT_SOURCE_DIR}/render/shaders -I${VKHB_HARFBUZZ_SHADER_INCLUDE_DIR}
            -fauto-bind-uniforms -fauto-map-locations --target-env=vulkan1.3
            -o ${shader_out} ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader} ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/ui_common.glsl
    COMMENT "Compiling shader ${shader_name}"
    VERBATIM
  )
  list(APPEND VKHB_SHADER_OUTPUTS ${shader_out})
  list(APPEND VKHB_ASSET_MANIFEST_LINES "shaders/${shader_name}.spv=${shader_out}")
endforeach()
add_custom_target(vkhb_shaders DEPENDS ${VKHB_SHADER_OUTPUTS})

# --- Fonts: the three Inter weights UiRenderer loads, staged next to the executable and addressed
# the same way as shaders ("fonts/Inter-Regular.ttf"). Inter is OFL-1.1; its license is staged
# alongside so a copied build directory stays compliant. An embedded build has nowhere to put that
# license file — ship it with whatever you distribute (see docs/packaging.md).
set(VKHB_FONT_FILES Inter-Regular.ttf Inter-SemiBold.ttf Inter-Bold.ttf)
set(VKHB_FONT_OUTPUTS "")
foreach(font_name ${VKHB_FONT_FILES})
  set(font_out "${CMAKE_CURRENT_BINARY_DIR}/fonts/${font_name}")
  add_custom_command(
    OUTPUT ${font_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy "${VKHB_FONT_DIR}/${font_name}" ${font_out}
    DEPENDS "${VKHB_FONT_DIR}/${font_name}"
    COMMENT "Staging font ${font_name}"
    VERBATIM
  )
  list(APPEND VKHB_FONT_OUTPUTS ${font_out})
  list(APPEND VKHB_ASSET_MANIFEST_LINES "fonts/${font_name}=${font_out}")
endforeach()
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fonts/LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${VKHB_FONT_LICENSE}" "${CMAKE_CURRENT_BINARY_DIR}/fonts/LICENSE.txt"
  DEPENDS "${VKHB_FONT_LICENSE}"
  COMMENT "Staging font license"
  VERBATIM
)
list(APPEND VKHB_FONT_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/fonts/LICENSE.txt")
add_custom_target(vkhb_fonts DEPENDS ${VKHB_FONT_OUTPUTS})

# --- Hand-placed assets: everything under <project>/assets, staged next to the executable keeping
# its path relative to that directory — assets/embrace.ogg is loaded as "embrace.ogg",
# assets/music/foo.ogg as "music/foo.ogg". CONFIGURE_DEPENDS re-runs the glob when files appear or
# vanish, so dropping a file in assets/ and rebuilding is enough; no edit here.
#
# Note for VKHB_EMBED_ASSETS builds: these go into the embedded archive like everything else, and
# already-compressed media (Opus, Vorbis, PNG) does not shrink further under xz — a 3 MB music file
# is 3 MB of executable. Ship large media as loose files if that matters.
file(GLOB_RECURSE VKHB_LOOSE_ASSETS CONFIGURE_DEPENDS
     RELATIVE "${CMAKE_SOURCE_DIR}/assets" "${CMAKE_SOURCE_DIR}/assets/*")
#
# Names are checked here rather than left to fail later: the embedded archive's table of contents is
# whitespace-separated ASCII (`name offset size` per line), so a file with a space in its name would
# configure and build fine, then make *every* load() fail at runtime in a VKHB_EMBED_ASSETS build
# only. `=` would break the manifest's own name=path lines the same way. Fail at configure time, in
# every build config, where the message can say what to do about it.
set(VKHB_LOOSE_ASSET_OUTPUTS "")
foreach(asset ${VKHB_LOOSE_ASSETS})
  if(asset MATCHES "[ \t=]")
    message(FATAL_ERROR
      "Asset name 'assets/${asset}' contains a space, tab or '='. The embedded asset archive cannot "
      "carry those in a name — rename the file (hyphens and underscores are fine).")
  endif()
  set(asset_out "${CMAKE_CURRENT_BINARY_DIR}/${asset}")
  get_filename_component(asset_out_dir "${asset_out}" DIRECTORY)
  add_custom_command(
    OUTPUT ${asset_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${asset_out_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/${asset}" ${asset_out}
    DEPENDS "${CMAKE_SOURCE_DIR}/assets/${asset}"
    COMMENT "Staging asset ${asset}"
    VERBATIM
  )
  list(APPEND VKHB_LOOSE_ASSET_OUTPUTS ${asset_out})
  list(APPEND VKHB_ASSET_MANIFEST_LINES "${asset}=${asset_out}")
endforeach()
add_custom_target(vkhb_loose_assets DEPENDS ${VKHB_LOOSE_ASSET_OUTPUTS})

# --- Asset embedding: pack every staged shader and font into one xz stream that assets_embedded.cpp
# pulls in with #embed. The staged loose files above stay the packer's input, so a dev build and a
# release build ship byte-identical assets and only differ in how they are carried.
#
# The manifest is written at configure time rather than passed on the command line — see
# cmake/PackAssets.cmake. assets_embedded.cpp gets an explicit OBJECT_DEPENDS on the archive because
# no compiler reports #embed dependencies to the build system.
if(VKHB_EMBED_ASSETS)
  find_program(VKHB_XZ NAMES xz REQUIRED DOC "Host xz used to compress the embedded asset archive")
  set(VKHB_ASSET_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(VKHB_ASSET_MANIFEST "${VKHB_ASSET_GEN_DIR}/assets.manifest")
  set(VKHB_ASSET_ARCHIVE "${VKHB_ASSET_GEN_DIR}/vkhb_assets.xz")
  set(VKHB_ASSET_SIZE_HEADER "${VKHB_ASSET_GEN_DIR}/vkhb_assets_size.hpp")

  string(REPLACE ";" "\n" VKHB_ASSET_MANIFEST_TEXT "${VKHB_ASSET_MANIFEST_LINES}")
  file(WRITE "${VKHB_ASSET_MANIFEST}" "${VKHB_ASSET_MANIFEST_TEXT}\n")

  add_custom_command(
    OUTPUT ${VKHB_ASSET_ARCHIVE} ${VKHB_ASSET_SIZE_HEADER}
    COMMAND ${CMAKE_COMMAND}
            -DMANIFEST=${VKHB_ASSET_MANIFEST}
            -DARCHIVE=${VKHB_ASSET_ARCHIVE}
            -DSIZE_HEADER=${VKHB_ASSET_SIZE_HEADER}
            -DXZ=${VKHB_XZ}
            -DSTAGE_DIR=${VKHB_ASSET_GEN_DIR}/stage
            -P ${CMAKE_SOURCE_DIR}/cmake/PackAssets.cmake
    DEPENDS ${VKHB_SHADER_OUTPUTS} ${VKHB_FONT_OUTPUTS} ${VKHB_LOOSE_ASSET_OUTPUTS} ${VKHB_ASSET_MANIFEST}
            ${CMAKE_SOURCE_DIR}/cmake/PackAssets.cmake
    COMMENT "Packing assets into ${VKHB_ASSET_ARCHIVE}"
    VERBATIM
  )
  add_custom_target(vkhb_asset_blob DEPENDS ${VKHB_ASSET_ARCHIVE} ${VKHB_ASSET_SIZE_HEADER})
  add_dependencies(vkhb_assets vkhb_asset_blob)
  set_source_files_properties(assets/assets_embedded.cpp
    PROPERTIES OBJECT_DEPENDS "${VKHB_ASSET_ARCHIVE};${VKHB_ASSET_SIZE_HEADER}")
endif()

# --- Demo: a window, a cleared background, and every UI primitive exercised once, plus a
# --headless screenshot mode. Replace app/main.cpp with your own; keep the add_dependencies so
# shaders/fonts stay in step with it.
add_executable(vkhb_demo app/main.cpp)
add_dependencies(vkhb_demo vkhb_shaders vkhb_fonts vkhb_loose_assets)
target_link_libraries(vkhb_demo PRIVATE vkhb_ui vkhb_render SDL3::SDL3-static vkhb_compile_flags)
set_target_properties(vkhb_demo PROPERTIES CXX_EXTENSIONS OFF)

# The demo's audio is compiled out entirely with -DVKHB_AUDIO=OFF rather than stubbed, so a build
# without SDL_mixer has no dead code and no silent no-ops in it.
if(VKHB_AUDIO)
  target_link_libraries(vkhb_demo PRIVATE vkhb_audio)
  target_compile_definitions(vkhb_demo PRIVATE VKHB_HAS_AUDIO)
endif()
