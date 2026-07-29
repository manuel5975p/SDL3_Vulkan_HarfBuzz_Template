# vkhb_template

SDL3 + Vulkan 1.3 (volk, dynamic rendering) + HarfBuzz-GPU text. C++23.

## Build

```
cmake -B build -G Ninja && cmake --build build   # first run fetches deps (network)
ctest --test-dir build --output-on-failure
./build/src/vkhb_demo [--gpu=N --present-gpu=N --frames=N --screenshot=out.ppm --list-gpus]
./build/src/vkhb_demo --headless --frames=30 --width=640 --height=400 --screenshot=out.ppm
./build/src/vkhb_demo --present=fifo|mailbox|immediate      # --no-vsync = immediate; P cycles
./build/src/vkhb_demo --music=embrace.ogg | --no-music      # windowed only; default assets/embrace.ogg
```

`--headless`: no window, no surface. `--list-gpus`: `render=`/`present=` against a real surface.
Windowed `--screenshot` grabs frame `--frames-1` (0 without `--frames`).

**Read `docs/presentation.md` before touching the frame loop, present modes or headless mode** -
failures there are silent (a misordered but correct pipeline just halves vsync fps) and it covers
checking your work with the sync validation layer.

Release (`docs/packaging.md`): one ~6.9 MB binary (3.2 MB of it embedded `assets/` music), no runtime
deps past OS + Vulkan driver.

```
cmake -B build-rel -G Ninja -DVKHB_RELEASE=ON
cmake -B build-win -G Ninja -DVKHB_RELEASE=ON -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
```

- Knobs overriding that umbrella (`cmake/BuildOptions.cmake`): `VKHB_LTO`, `VKHB_OPTIMIZE_SIZE`,
  `VKHB_STRIP`, `VKHB_STATIC_RUNTIME`, `VKHB_STATIC_LINK`, `VKHB_EMBED_ASSETS`.
- Needs `glslc`, CMake >= 3.28, GCC >= 15/Clang >= 19 (`#embed`). Vulkan headers: installed SDK or
  fetched. volk loads the ICD at runtime - no loader to link.
- `BUILD_TESTING=OFF` drops all tests. Deps pinned in `cmake/Dependencies.cmake`; override with
  `-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ=/path`.
- Warning flags hang off the `vkhb_compile_flags` INTERFACE target (`cmake/CompileFlags.cmake`,
  GCC/Clang + MSVC arms), **not** `add_compile_options`, so deps never see them and no flag spelling
  is forced on a toolchain. Link it `PRIVATE` into every new target here.
- `CMAKE_CXX_SCAN_FOR_MODULES OFF` (top-level `CMakeLists.txt`) is **the one deliberate global
  `CMAKE_*` setting** - keep new settings on targets or INTERFACE targets. CMake scans every
  `cxx_std_20`+ target; nothing here is a module, so it only buys an extra pass per source plus
  clangd-hostile `-fmodules-ts -fmodule-mapper=… -fdeps-format=…` in `compile_commands.json`. Global
  beats repeating it in nine `set_target_properties` calls and every future one; deps never request
  C++20 anyway.
- CI (`.github/workflows/ci.yml`): default config + `ctest` on Linux GCC 15/Clang 20, macOS Homebrew
  LLVM, MSYS2 UCRT64, MSVC, clang-cl - build-only, no GPU.

## Layout

- `src/assets/` - `vkhb_assets`: `load("shaders/ui_rect.vert.spv")` by logical name. Exactly one
  backend compiles in: `assets_dir.cpp` (files under `SDL_GetBasePath()`) or `assets_embedded.cpp`
  (xz archive `#embed`ded in the binary).
- `src/render/` - `vkhb_render`: `vk_context` (instance + one or two `Device`s), `swapchain`,
  `Presenter` (frame loop), `headless` (offscreen target, same frame shape), buffers/images, SPIR-V
  loading, sRGB helpers. No UI or text knowledge.
- `src/ui/` - `vkhb_ui`: `ui_font` (HarfBuzz shaping + hb-gpu glyphs in a texel-buffer atlas),
  `ui_pipeline`, `ui_renderer` (immediate-mode API), `theme`, `text_wrap`, `format`, `panel_chrome`.
- `src/audio/` - `vkhb_audio`: `Engine` over SDL3_mixer (Ogg Opus/Vorbis/WAV), music track plus
  one-shots from asset spans; `open_silent()` mixes with no device, for tests. Gated on `VKHB_AUDIO`;
  libopus/opusfile/libogg fetched and static-linked, so no runtime dep. Read README.md's Audio section
  (three-step vendoring dance) before touching the SDL_mixer block in `cmake/Dependencies.cmake`.
- `assets/` - hand-placed runtime files, staged keeping their path relative to that directory
  (`assets/embrace.ogg` loads as `"embrace.ogg"`). Globbed `CONFIGURE_DEPENDS`.
- `src/app/main.cpp` - demo: the frame loop to copy, `draw_demo_ui()` to delete.
- `tests/` - windowless only; anything needing a device does not belong here.

## Conventions

- Namespaces `vkhb::render`, `vkhb::ui`. Errors are `std::expected<T, std::string>` via
  `vk_error("vkCreateFoo", r)`; asserts are for internal invariants only.
- Vulkan objects are move-only RAII, hand-written move/destructor, each caching a
  `const VolkDeviceTable*` so its destructor needs no context in scope.
- UI space is physical pixels, origin top-left, +y down. `text()` anchors the *baseline*.
- Rebuild UI draw data every frame - no retained scene graph.
- `Presenter::begin_frame()`'s clear colour is *optional*: `std::nullopt` (plus `LOAD_OP_DONT_CARE`)
  when the first pass writes every pixel. The demo passes one.
- Position plus a designated-initializer struct: `ui.text(x, y, "Hi", {.size = 20})`,
  `ui.rect({x,y,w,h}, color)`, `ui.button({x,y,w,h}, "Save")`. `begin_frame(extent, input)` latches
  pointer state, so widgets aren't handed it individually.
- `button()` is identified by its rect: unique, and stable frame to frame.

## Gotchas

- **Nothing renders into a swapchain image.** Draw into the offscreen `Presenter::kColorFormat`
  (B8G8R8A8_UNORM) target on the *render* device; `Presenter` copies it to the swapchain - build every
  pipeline with that format. UNORM blends alpha on gamma-encoded bytes (CSS-style) everywhere without
  `VK_KHR_swapchain_mutable_format`; cost is one full-screen copy per frame.
- **Render and present GPU may differ.** Rendering always uses the requested GPU; if it can't present
  it's paired with one that can and frames cross host memory - a CPU sync per frame, since no
  semaphore spans devices. `ctx.cross_gpu()` says which; `--present-gpu=N` forces it.
- **Instance-level calls only in `vk_context.cpp`.** Physical-device and surface entry points have no
  volk table, so they're the only globals: `VulkanContext` calls them once and caches `gpus()`,
  `surface_formats()`, `present_modes()`; `Device` caches memory props behind `find_memory_type()`.
  Deliberate exception: `surface_caps()` is live, since `currentExtent` tracks the window.
- **No global *device* calls.** `volkLoadInstanceOnly` leaves volk's device globals null, so a stray
  one crashes loudly instead of dispatching to whichever device loaded last. Use `dev.vk().vkFoo(...)`.
- **Null window means headless.** `VulkanContext::create(nullptr, gpu)` is surfaceless: `headless()`
  true, `present() == render()`, `surface_formats()` empty.
- **Acquire in `end_frame()`, not `begin_frame()`.** `vkAcquireNextImageKHR` blocks until the
  presentation engine frees an image (FIFO: next refresh). Up front that wait stacks on CPU recording
  and FIFO halves to 30 fps; it need only precede the swapchain copy, the last command recorded. So
  the submit waits the acquire semaphore at `TRANSFER`, and the swapchain barrier must be *sourced* at
  `TRANSFER` to land after that wait - `TOP_OF_PIPE` names no stage the wait covers.
- **Cross-GPU presents one frame behind.** `end_frame()` submits N, then hands over N-1: the CPU wait
  is on a frame-old submit and the render GPU overlaps the transfer. Inline handover would serialise
  render + copy + present into one frame time and make present mode stop mattering. Hence acquire
  lives in `flush_pending()` there, and an out-of-date acquire drops the pending frame and arms
  `needs_resize_`.
- **`VK_SUBOPTIMAL_KHR` arms a rebuild, once per extent.** Ignored, the window scales forever;
  honoured naively, it rebuilds every frame on compositors reporting it permanently.
  `Presenter::suboptimal_settled_` is that guard - don't drop it.
- **MAILBOX forces >= 3 swapchain images.** Two block in acquire exactly like FIFO while looking
  enabled. The startup line prints the count obtained.
- **Glyph atlas never evicts.** Fine for a finite Latin UI; replace it for unbounded text.
- **harfbuzz-gpu duplicates `hb-static.cc`** - `cmake/Dependencies.cmake` strips it for static builds;
  drop that block and `multiple definition of _hb_NullPool` returns.
- **Shaders/fonts are runtime assets**, by logical name via `assets::load()`, never by path. New
  shader -> `VKHB_SHADER_SOURCES`; it lands in the staged `shaders/` dir and, under
  `VKHB_EMBED_ASSETS`, in the embedded archive. `main()` calls `assets::set_base_dir()` once; the
  embedded backend ignores it.
- **Embedded archive format lives twice**: `cmake/PackAssets.cmake` writes it,
  `src/assets/assets_embedded.cpp` parses it - change together. `tests/test_assets` runs whichever
  backend the build selected and catches drift.
- `ui_text.vert/frag` textually `#include` hb-gpu's `.glsl` from the fetched harfbuzz tree
  (`VKHB_HARFBUZZ_SHADER_INCLUDE_DIR`).
- Frame sync: one fence/semaphore per frame in flight, one `render_finished` semaphore per *swapchain
  image*.
