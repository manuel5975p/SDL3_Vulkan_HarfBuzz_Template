# SDL3 + Vulkan + HarfBuzz Template

[![CI](https://github.com/manuel5975p/SDL3_Vulkan_HarfBuzz_Template/actions/workflows/ci.yml/badge.svg)](https://github.com/manuel5975p/SDL3_Vulkan_HarfBuzz_Template/actions/workflows/ci.yml)

A cross-platform and minimal template to (vibe)code off of. Starting point for a Vulkan app with
GPU-rendered text: SDL3 window/input, Vulkan 1.3 via volk with dynamic rendering, HarfBuzz shaping
with `libharfbuzz-gpu` glyph encoding and rendering.

Supports:

- Window + Vulkan context, device functions called directly via `volk`
- Headless mode + builtin screenshots for visual verification in terminal sessions
- Asset bundling and static linking into one fat executable with `-DVKHB_RELEASE=ON`
- Cross compilation from Linux to Windows with mingw-w64
- Basic 2D and 3D rendering
- Music and sound effects via SDL3_mixer (Ogg Opus, Ogg Vorbis, WAV), addressed by the same logical
  asset names as everything else — core SDL3 decodes WAV and nothing else
- Split-GPU rendering: **presentation and rendering do not need to happen on the same GPU** (but
  they can, and most of the time will). Monitor on the MoBo port, rendering on the discrete GPU.

```sh
# Unbundled build: assets NOT packed into the executable
cmake -B build -G Ninja && cmake --build build && ./build/src/vkhb_demo

# Bundled build: the executable is the only file you need
cmake -B build -G Ninja -DVKHB_RELEASE=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

## Modules

The first configure fetches SDL3, SDL3_mixer (plus libopus/opusfile/libogg), volk, HarfBuzz, glm
and Inter (pinned in `cmake/Dependencies.cmake`).

- **`vkhb_render`** - instance/device/surface selection, swapchain, the `Presenter` frame loop
  (offscreen target, late acquire, present modes, cross-GPU handover), a surfaceless headless
  target with the same frame shape, dedicated-allocation buffers and images, SPIR-V loading.
- **`vkhb_ui`** - `UiRenderer`: `rect`, `line`, `circle`, `text`, `measure_text`,
  `ascent`/`descent` in pixel space, rebuilt each frame; plus fonts, a dark theme, word wrap,
  number formatting and modal panel chrome.
- **`vkhb_assets`** - logical-name lookup, backed by files next to the executable or by an xz
  archive embedded in the binary.
- **`vkhb_audio`** - `Engine`: looping music on a dedicated track plus fire-and-forget one-shots,
  decoded straight out of an asset span by SDL3_mixer. `Engine::open_silent()` mixes without an
  audio device, which is what makes the audio tests runnable in CI. Turn the whole library off with
  `-DVKHB_AUDIO=OFF`; see [Audio](#audio).
- Plus a demo exercising all of it, GPU-free `ctest` unit tests, and `--headless --screenshot` for
  visual checks with no display.

## Build instructions

### What every platform needs

| Requirement | Why |
| --- | --- |
| CMake >= 3.28 | `FetchContent_Declare(EXCLUDE_FROM_ALL)` |
| Ninja (recommended) | every command below uses `-G Ninja`; Make and MSBuild work too |
| C++23 compiler: GCC >= 15, Clang >= 19, or MSVC 17.6+ | `std::ranges::contains`, `std::expected`, `std::print`, and `#embed` for the embedded asset archive |
| `glslc` (shaderc, ships with the Vulkan SDK) | `find_program(GLSLC ... REQUIRED)` in `src/CMakeLists.txt` |
| `xz` on the host | only when `VKHB_EMBED_ASSETS` / `VKHB_RELEASE` is on |
| `git` + network on the first configure | SDL3, SDL3_mixer + its Opus codecs, volk, HarfBuzz, glm, Inter and (optionally) Vulkan-Headers are fetched |
| A Vulkan 1.3 driver at runtime | `dynamicRendering` + `synchronization2` are required, not optional |

Not needed: a Vulkan *loader* library, and in most cases the Vulkan SDK at all.

- volk loads the ICD itself at runtime, so only headers are needed to build.
- Headers come from an installed SDK, or are fetched when there is none
  (`VKHB_FETCH_VULKAN_HEADERS`, on by default when `find_package(Vulkan)` fails or when cross
  compiling).

### The two build shapes

```sh
# dev: loose assets next to the executable, fast link, debuggable
cmake -B build -G Ninja
cmake --build build

# shipping: one self-contained file (LTO + -Os + strip + static runtime + embedded assets)
cmake -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DVKHB_RELEASE=ON -DBUILD_TESTING=OFF
cmake --build build-rel
```

- `CMAKE_BUILD_TYPE` defaults to `RelWithDebInfo`.
- `VKHB_OPTIMIZE_SIZE` appends `-Os -g0` *after* the build type's flags and the last `-O`/`-g`
  wins, so `VKHB_RELEASE=ON` is size-optimized whatever the build type says.
- Pass `-DCMAKE_BUILD_TYPE=Release` anyway, so the dependencies pick up `NDEBUG`.
- Every umbrella switch is individually overridable on the same command line; the full option table
  is in `docs/packaging.md`.

### Linux (tested here)

Toolchain packages:

```sh
# Arch / Manjaro
sudo pacman -S cmake ninja gcc shaderc xz git
# Debian / Ubuntu (needs a distro new enough for g++-15)
sudo apt install cmake ninja-build g++-15 glslc xz-utils git
# Fedora
sudo dnf install cmake ninja-build gcc-c++ glslc xz git
```

(Nothing extra for audio: the Opus codecs are fetched and built from source. Add `opusfile` /
`libopusfile-dev` / `opusfile-devel` only if you want `-DVKHB_AUDIO_VENDORED=OFF`.)

Plus:

- The Vulkan ICD for your GPU - `vulkan-radeon` / `vulkan-intel` / `nvidia-utils` on Arch,
  `mesa-vulkan-drivers` on Debian.
- SDL's build-time headers for your windowing system: `libwayland-dev` / `libxkbcommon-dev` /
  `libx11-dev` and friends on Debian, `wayland` / `libx11` on Arch. SDL `dlopen`s those backends at
  runtime, so they are a build-time *include* dependency only.

```sh
cmake -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DVKHB_RELEASE=ON -DBUILD_TESTING=OFF
cmake --build build-rel
./build-rel/src/vkhb_demo
./build-rel/src/vkhb_demo --headless --frames=3 --screenshot=out.ppm   # no display needed
```

- Verified on this machine with GCC 16.1, CMake 4.3, Ninja: 6.9 MB executable, runs headless and
  writes a screenshot with no other file beside it. Over half of that is the demo's music —
  `assets/embrace.ogg` is 3.2 MB and does not compress, so an `assets/` with nothing in it gives
  3.6 MB. See [Audio](#audio) if that matters to you.
- `ctest --test-dir build --output-on-failure` runs the windowless unit tests (drop
  `-DBUILD_TESTING=OFF` for that).
- **Do not reach for `-DVKHB_STATIC_LINK=ON` on Linux.** SDL reaches X11/Wayland through `dlopen()`,
  and a statically linked glibc cannot do that portably; CMake prints a warning saying so.
  `VKHB_STATIC_RUNTIME` (on by default in a release build) is the one you want, and it already
  removes the two libraries most likely to be missing on someone else's machine.

### Linux -> Windows cross compile (tested here)

This is the supported way to produce a Windows binary. It needs no Windows SDK and no Vulkan SDK:
volk `LoadLibrary`s `vulkan-1.dll` at runtime, `cmake/Dependencies.cmake` fetches the headers when
`CMAKE_CROSSCOMPILING` is set, and `glslc`/`xz` run on the host.

```sh
# Arch / Manjaro
sudo pacman -S mingw-w64-gcc
# Debian / Ubuntu (needs a mingw-w64 GCC >= 15; Ubuntu 24.04's is 13 and too old)
sudo apt install mingw-w64

cmake -B build-win -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DVKHB_RELEASE=ON -DBUILD_TESTING=OFF
cmake --build build-win
# -> build-win/src/vkhb_demo.exe
```

- Verified on this machine: a 6.9 MB PE32+ *console* executable (3.2 MB of that the embedded
  `assets/embrace.ogg`, as above), stripped, importing nothing but
  `KERNEL32`/`USER32`/`GDI32`/`SHELL32`/`OLE32`/`IMM32`/`SETUPAPI`/`VERSION`/`WINMM`/`ADVAPI32` and
  the UCRT `api-ms-win-crt-*` stubs - i.e. the only file that has to be copied to the target
  machine. Check your own build with
  `x86_64-w64-mingw32-objdump -p build-win/src/vkhb_demo.exe | grep 'DLL Name'`.
- Being a console app, `--headless` and `--list-gpus` print into the terminal that started it. It
  was cross-compiled and inspected here, not executed on Windows.
- The toolchain file assumes the sysroot at `/usr/x86_64-w64-mingw32` and the `x86_64-w64-mingw32-*`
  tool names; override with `-DVKHB_MINGW_TRIPLE=...` if your distro spells them differently.
- `BuildOptions.cmake` forces two mingw-only things that look like unrelated compiler bugs when they
  bite:
  - `-Wa,-mbig-obj` project-wide - COFF's 32k-section cap, and GCC's `lto-wrapper` drops assembler
    options unless every input object agrees on them.
  - `-lstdc++exp` - `std::print` reaches the Windows console through `std::__write_to_terminal`,
    which libstdc++ keeps in a separate archive.
- Windows builds are always fully `-static`: nobody wants to ship `libstdc++-6.dll` and
  `libwinpthread-1.dll` next to an `.exe`, and `-static-libstdc++`/`-static-libgcc` alone leave
  `libwinpthread` behind.
- Testing the result under Wine is optional and best-effort:
  `wine build-win/src/vkhb_demo.exe --headless --frames=3 --screenshot=out.ppm` only works if Wine
  can reach a Vulkan ICD.

### Windows, natively (built in CI, not hand-tested)

**MSYS2 UCRT64** is the path of least resistance: it is the same toolchain the cross build uses,
just hosted on Windows, and the `MINGW` branches in `cmake/BuildOptions.cmake` apply identically.

```sh
# in an MSYS2 UCRT64 shell
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,shaderc} xz git
cmake -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DVKHB_RELEASE=ON -DBUILD_TESTING=OFF
cmake --build build-rel
```

**MSVC and clang-cl** build the default configuration:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
:: clang-cl instead of cl.exe: add -T ClangCL to the configure line
```

- Warning flags are per-target (`cmake/CompileFlags.cmake`) and have an MSVC arm (`/W4 /utf-8`), so
  nothing GCC-shaped is forced onto `cl.exe`.
- **`VKHB_RELEASE` and its individual switches stay GNU/Clang-only.** `-Os`, `-flto`, `-s`,
  `--gc-sections` and `-static-libstdc++` are all GCC spellings, and `VKHB_LTO` raises a
  `FATAL_ERROR` on any compiler that is not GNU or Clang. Porting them means giving each an MSVC arm
  (`/O1 /Gy /Gw`, `/GL` + `/LTCG`, `/OPT:REF,ICF`, `/MT`, and `#embed` support in your toolset).
- Only `glslc` has to be found on `PATH`; the CI job installs it from MSYS2 rather than pulling in
  the full Vulkan SDK, which works because it is a host tool the build merely shells out to.
- Install a Vulkan-capable GPU driver to *run* the demo (every desktop driver ships `vulkan-1.dll`);
  the LunarG Vulkan SDK is optional and only buys validation layers plus its own `glslc.exe`.

### macOS (built in CI, otherwise untested)

There is no native Vulkan on macOS: you need MoltenVK, and the build below is only known to compile.

```sh
brew install cmake ninja llvm shaderc xz
brew install --cask vulkan-sdk   # runtime only; it carries MoltenVK
cmake -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
      -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ \
      -DVKHB_LTO=ON -DVKHB_OPTIMIZE_SIZE=ON -DVKHB_EMBED_ASSETS=ON \
      -DVKHB_STRIP=OFF -DVKHB_STATIC_RUNTIME=OFF -DVKHB_STATIC_LINK=OFF \
      -DBUILD_TESTING=OFF
cmake --build build-rel
```

- Note what is deliberately *not* used: `-DVKHB_RELEASE=ON`. Its individual switches assume a
  GNU-style toolchain Apple's does not provide - `-static-libstdc++`/`-static-libgcc` are rejected
  by Apple Clang (macOS has no static libc; the only supported interface is the shared `libSystem`),
  `ld64` has no `--gc-sections` (it wants `-dead_strip`) and takes `-Wl,-x`/`strip(1)` rather
  than `-s`.
- Homebrew LLVM rather than Apple Clang gets you a recent libc++ and `#embed`, which Apple's shipped
  Clang may lag on: check with `echo '#embed <stdio.h>' | clang -E -` before assuming. To link
  against the bundled libc++ add
  `-L$(brew --prefix llvm)/lib/c++ -L$(brew --prefix llvm)/lib/unwind -lunwind` to the linker flags,
  as the CI job does.
- Two things need real work beyond the build:
  - **Instance creation.** MoltenVK is a portability driver, so an instance has to request
    `VK_KHR_portability_enumeration` and set `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, and
    a device has to enable `VK_KHR_portability_subset`. `src/render/vk_context.cpp` does neither, so
    `--list-gpus` will most likely come back empty until you add them.
  - **Vulkan 1.3.** `vk_context.cpp` asks for `apiVersion = VK_API_VERSION_1_3` and refuses any
    device without `dynamicRendering` and `synchronization2`. MoltenVK only reached that recently
    and the portability subset restricts things around it: expect to meet the limits, not just the
    API version.
- `.app` bundling is out of scope here. A `VKHB_EMBED_ASSETS` build is a single executable, so
  dropping it into `Contents/MacOS/` alongside `libMoltenVK.dylib` and an `Info.plist` is the shape
  of it.

## Audio

Core SDL3 decodes WAV and nothing else, so music comes from **SDL3_mixer**, fetched as a release
tarball alongside everything else. `vkhb_audio` wraps it in the same logical-asset addressing the
renderers use:

```cpp
auto engine = vkhb::audio::Engine::open();          // std::expected; no device is not fatal
engine->play_music("embrace.ogg");                  // loops forever by default
engine->play_sound("click.wav");                    // fire-and-forget, own track
engine->set_gain(0.5f);
```

Drop a file anywhere under `assets/` and it is staged next to the executable keeping its path
relative to that directory — `assets/embrace.ogg` loads as `"embrace.ogg"`, `assets/music/foo.ogg`
as `"music/foo.ogg"`. The glob is `CONFIGURE_DEPENDS`, so adding a file and rebuilding is enough.
An embedded build (`VKHB_EMBED_ASSETS`) puts them in the archive with the shaders and fonts; note
that already-compressed media does not shrink under xz, so a 3 MB Opus file is 3 MB of executable.

The demo plays `assets/embrace.ogg` if it is there: `--music=NAME` picks a different asset,
`--no-music` skips it. `--headless` and `--list-gpus` never open a device — a screenshot run stays
silent, and both flags are accepted and ignored there.

- **Codecs compiled in: Ogg Opus, Ogg Vorbis (stb_vorbis), WAV.** Everything else SDL_mixer can do —
  FLAC, MP3, MOD, MIDI, GME, WavPack — is switched off in `cmake/Dependencies.cmake`; turn one back
  on there, and check first whether it needs a library of its own (FLAC and MP3 have
  dependency-free `dr_flac`/`dr_mp3` backends; the rest do not).
- **`libopus`, `opusfile` and `libogg` are fetched and built statically**, like every other
  dependency here, so an audio build still links nothing but libc and libm and the mingw cross build
  imports no extra DLL. `-DVKHB_AUDIO_VENDORED=OFF` uses the system's instead (`libopusfile-dev` /
  `opusfile`), which builds ~20 s faster and costs you three `.so`s in `ldd`.
- `-DVKHB_AUDIO=OFF` compiles the library and the demo's audio out entirely; `SDLMIXER_STRICT` is
  on, so a codec that cannot be satisfied fails configure instead of silently disappearing.
- `Engine::open_silent()` mixes into no device at all, which is how `test_audio` decodes and plays
  the real file on a machine with no sound server.

### How the vendoring works

Worth knowing before you bump `VKHB_SDL_MIXER_URL`. SDL_mixer expects its codecs as git submodules
under `external/`, and a GitHub-generated release tarball leaves those directories **empty** — so
`SDLMIXER_VENDORED=ON` alone would fail on the first `add_subdirectory(external/opus)`.
`cmake/Dependencies.cmake` therefore fetches SDL_mixer in three steps:

1. Declare it with `SOURCE_SUBDIR .populate-only` — a path with no `CMakeLists.txt` in it, which
   makes `FetchContent_MakeAvailable` download and extract *without* configuring.
2. Fetch `ogg`, `opus` and `opusfile` with `SOURCE_DIR` pointing straight into
   `${sdl3_mixer_SOURCE_DIR}/external/`, filling the empty submodule directories.
3. `add_subdirectory()` SDL_mixer by hand, which now finds a complete tree.

Those three are the **libsdl-org forks**, not upstream Xiph, pinned by commit to exactly what
release-3.2.4's submodules point at (`VKHB_OGG_COMMIT` and friends). That matters: upstream
`opusfile` is autotools-only, and the forks are what carry the CMake support SDL_mixer expects. When
you move to a newer SDL_mixer, re-read its pins:

```sh
curl -s https://api.github.com/repos/libsdl-org/SDL_mixer/git/trees/release-3.2.4 # -> external tree
curl -s https://api.github.com/repos/libsdl-org/SDL_mixer/git/trees/<external-tree-sha>
```

## CI

`.github/workflows/ci.yml` configures, builds and runs `ctest` on every push and pull request:

| Job | Runner | Toolchain |
| --- | --- | --- |
| `linux-gcc` | ubuntu-24.04 | GCC 15 (`ppa:ubuntu-toolchain-r/test`) |
| `linux-clang` | ubuntu-24.04 | Clang 20 (`apt.llvm.org`) + libstdc++ 15 |
| `macos-clang` | macos-14 | Homebrew LLVM + its libc++ |
| `windows-mingw` | windows-2022 | MSYS2 UCRT64 GCC + Ninja |
| `windows-msvc` | windows-2022 | `cl.exe`, Visual Studio 17 2022 generator |
| `windows-clang` | windows-2022 | `clang-cl`, same generator with `-T ClangCL` |

- **Build-only.** No runner has a GPU or a display, so no job launches the demo or renders a frame.
  `ctest` is safe everywhere because every test in `tests/` is windowless by construction.
- **Default configuration only** - loose assets, no LTO, no size flags. The embedded-asset and
  release paths need `#embed` plus `xz` and are GNU-only; verify those locally with the recipes in
  `docs/packaging.md`.
- The C++23 library floor (`std::ranges::contains` needs libstdc++ 15 / libc++ 19 / MSVC 17.6) is
  why the Linux jobs pull toolchains from upstream apt repos instead of using the runner image's
  GCC 13/14 and Clang 16-18 - and why Ubuntu's own mingw-w64 (GCC 13) cannot host a cross-compile
  job even though cross compiling is the documented release path.

## LTO

`VKHB_LTO` (on with `VKHB_RELEASE`, off otherwise) turns on link-time optimization across the app
*and every fetched dependency*: SDL, HarfBuzz, volk, the lot.

- That reach is the whole point, and it is why `cmake/BuildOptions.cmake` is included **before**
  `cmake/Dependencies.cmake` in the top-level `CMakeLists.txt`: `add_compile_options`/
  `add_link_options` are directory-scoped and only affect subdirectories added afterwards. Swapping
  those two `include()` lines still builds - it just silently loses the size win, which is the worst
  kind of regression.
- It deliberately does not use CMake's `INTERPROCEDURAL_OPTIMIZATION`: that property is per-target,
  so it would miss the fetched dependencies, and it picks its own flags rather than the two this
  build wants.

What it actually does, per compiler:

| | GCC | Clang |
| --- | --- | --- |
| compile + link flags | `-flto -flto-partition=one` | `-flto=full` |
| archiver | `gcc-ar` / `gcc-nm` / `gcc-ranlib` | `llvm-ar` / `llvm-nm` / `llvm-ranlib` |
| linker | default `ld`/`ld.bfd` | `ld.lld` from the compiler's own directory |

- **`-flto-partition=one`** puts the whole program into a single partition. It links slower, and it
  is what lets the size-oriented inliner and identical-code folding see everything at once.
- **The archiver swap is not polish.** GCC emits slim LTO objects whose symbol tables only binutils'
  LTO plugin can read; the `gcc-*` wrappers load that plugin, while plain `ar` silently writes
  archives the linker later reports as "has no symbols".
- **The linker swap is not polish either.** LLVM bitcode is only readable by a plugin of the same
  LLVM version. Falling through to GNU `ld` picks up whatever `LLVMgold.so` the system happens to
  have, which fails with "Unknown attribute kind" as soon as the system LLVM is older than the
  compiler. `ld.lld` ships with the compiler, so it always matches. Both the linker and the tool
  probes use `HINTS` on the compiler's own directory.
- **`-Os` is passed at link time too** when `VKHB_OPTIMIZE_SIZE` is on. The LTO link re-runs the
  optimizers and needs the same `-O` level the objects were built at: without it GCC links the
  bytecode at `-O0` and the entire size win evaporates.
- **On mingw it interacts with `-Wa,-mbig-obj`**, which is therefore applied project-wide rather
  than just where a translation unit overflows COFF's 32k-section cap. `lto-wrapper` keeps assembler
  options only when every input object agrees on them and otherwise drops them, after which the link
  dies on "COFF object format mismatch" the moment HarfBuzz is in it.
- Any compiler that is not GNU or Clang gets a `FATAL_ERROR` rather than a build that quietly
  ignores the flags.

Costs and controls:

- **Link time.** One partition over SDL + HarfBuzz + the app is a slow, memory-hungry
  single-threaded link at the end of every build. That is why the dev default is off.
- **Debuggability.** Combined with `-Os -g0` there is nothing left to debug against, and inlining
  across translation units makes the stack traces that remain misleading.
- Turn it off with `-DVKHB_RELEASE=ON -DVKHB_LTO=OFF` when bisecting a miscompile or when the link
  step is what is hurting.
- **Measured here** (GCC 16.1, `-DCMAKE_BUILD_TYPE=Release -DVKHB_RELEASE=ON`, x86-64 Linux):
  `vkhb_demo` is 3,551,880 bytes with `VKHB_LTO=OFF` and 3,428,872 bytes with it on - about 120 KB
  (3.5%) on top of what `-Os` and `--gc-sections` already do. Modest, because a large part of that
  binary is the embedded asset archive and the shipped code is mostly SDL and HarfBuzz, which
  section GC already trims well. Your own app's share of the binary is where it pays off.

Other size knobs and the full option table live in `docs/packaging.md`.
