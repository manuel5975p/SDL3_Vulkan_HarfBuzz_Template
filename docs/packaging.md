# Packaging: size, self-containment, Windows

The default build is for development: fast to link, debuggable, assets as loose files next to the
executable. Everything below is opt-in and lives in `cmake/BuildOptions.cmake`.

## The short version

```sh
# Linux, one self-contained binary
cmake -B build-rel -G Ninja -DVKHB_RELEASE=ON -DBUILD_TESTING=OFF
cmake --build build-rel
./build-rel/src/vkhb_demo            # ~6.9 MB, copies anywhere on its own

# Windows, cross-compiled with the mingw-w64 GCC
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
      -DVKHB_RELEASE=ON -DBUILD_TESTING=OFF
cmake --build build-win
# build-win/src/vkhb_demo.exe — ~6.9 MB, imports nothing but OS DLLs
```

3.2 MB of those 6.9 is one file: the demo's `assets/embrace.ogg`, embedded uncompressed because
Opus is already compressed. An empty `assets/` gives 3.6 MB. The default (loose-files) build keeps
that music beside the executable instead, in `shaders/` (48 KB), `fonts/` (1.3 MB) and
`embrace.ogg` (3.2 MB). The numbers here are for the template's own demo; they will move as soon as
you put a real app in it, but the shape of the difference will not.

## Options

`-DVKHB_RELEASE=ON` is an umbrella that flips the whole set on; each one is still individually
settable on the same command line (`-DVKHB_RELEASE=ON -DVKHB_STRIP=OFF` does what it looks like).

| Option | Default | Effect |
| --- | --- | --- |
| `VKHB_RELEASE` | `OFF` | Turns on everything below |
| `VKHB_LTO` | `VKHB_RELEASE` | `-flto=full` on Clang, `-flto -flto-partition=one` on GCC |
| `VKHB_OPTIMIZE_SIZE` | `VKHB_RELEASE` | `-Os -g0 -fno-ident`, section GC |
| `VKHB_STRIP` | `VKHB_RELEASE` | Links with `-s` |
| `VKHB_STATIC_RUNTIME` | `VKHB_RELEASE`, always on mingw | `-static-libstdc++ -static-libgcc` |
| `VKHB_STATIC_LINK` | on mingw | `-static` — no libc either |
| `VKHB_EMBED_ASSETS` | `VKHB_RELEASE` | Shaders and fonts go inside the binary |
| `VKHB_SDL_AUDIO` | `ON` | Keep SDL's audio subsystem |
| `VKHB_AUDIO` | `ON` | Build `vkhb_audio` on SDL3_mixer; needs `VKHB_SDL_AUDIO` |
| `VKHB_AUDIO_VENDORED` | `ON` | Build libopus/opusfile/libogg from source; `OFF` links the system's and costs three `.so` dependencies |
| `VKHB_FETCH_VULKAN_HEADERS` | on unless an SDK was found | Fetch Vulkan-Headers instead of using a system SDK |

`cmake/BuildOptions.cmake` is included *before* `cmake/Dependencies.cmake` on purpose: LTO and `-Os`
are directory-scoped options, which only reach subdirectories added afterwards, and they have to
reach SDL, HarfBuzz and volk too. Moving that include breaks the size numbers without breaking the
build, which is the worst kind of regression — don't.

LTO also swaps in the toolchain's own archiver. GCC emits slim LTO objects whose symbol tables only
binutils' LTO plugin can read, so `gcc-ar`/`gcc-nm`/`gcc-ranlib` are selected instead of the plain
ones (plain `ar` silently produces archives the linker then reports as "has no symbols"). On Clang
the equivalent trap is the linker: LLVM bitcode is only readable by a plugin of the same LLVM
version, so falling through to GNU `ld` picks up whatever `LLVMgold.so` the system happens to have
and fails with "Unknown attribute kind" whenever that is older than the compiler. `ld.lld` ships
with the compiler and therefore always matches — hence `-fuse-ld=lld` plus `llvm-ar`/`ranlib`/`nm`.
Both blocks live in `cmake/BuildOptions.cmake`; they are not optional polish.

## Embedded assets

With `VKHB_EMBED_ASSETS=ON` the build still stages `shaders/`, `fonts/` and whatever sits under
`assets/` into the build tree, then packs those exact files into one xz stream and links it into the
executable, so a dev build and a release build ship byte-identical assets and differ only in how
they are carried. Currently 1.23 MB of SPIR-V and fonts compress to 307 KB.

Watch the archive when `assets/` holds media: already-compressed formats (Opus, Vorbis, PNG) do not
shrink under xz, so they land in the executable at close to full size — the 3.2 MB `embrace.ogg` the
demo plays takes the packed archive from 307 KB to 3.4 MB. Ship large media as loose files if that
matters more than being a single file.

Three pieces:

- `cmake/PackAssets.cmake` builds the archive — an ASCII table of contents followed by the payloads,
  compressed with the **host** `xz` (`-9e --check=crc32`). The layout is documented in
  `src/assets/assets_embedded.cpp`; change the two together.
- `src/assets/assets_embedded.cpp` pulls the archive in with `#embed` and unpacks it on first use
  with [xz-embedded](https://github.com/tukaani-project/xz-embedded) (0BSD, decoder only, ~40 KB of
  code, fetched only when this option is on). `XZ_SINGLE` mode makes the output buffer double as the
  LZMA2 dictionary, so the decoder allocates nothing but the result.
- `src/assets/assets_dir.cpp` is the other backend: the same three functions, reading from a
  directory. Exactly one of the two is compiled into `vkhb_assets`.

Nothing outside `src/assets/` knows which is in play. Every caller asks for a logical name —
`load_shader_module(dev, "shaders/ui_rect.vert.spv")`, `Font::create("fonts/Inter-Regular.ttf", …)` —
and `main()` calls `assets::set_base_dir(SDL_GetBasePath())` once, which the embedded backend
ignores. `tests/test_assets.cpp` runs against whichever backend the build selected.

`#embed` needs GCC 15+ or Clang 19+, and resolves `<...>` against `--embed-dir`, which is a search
path of its own that `-I` does not feed. No compiler reports `#embed` dependencies to the build
system, hence the explicit `OBJECT_DEPENDS` in `src/CMakeLists.txt`.

## Windows

`cmake/toolchains/mingw-w64-x86_64.cmake` cross-compiles with the mingw-w64 GCC. No Windows SDK and
no Vulkan SDK are needed: volk `LoadLibrary`s `vulkan-1.dll` at runtime, so only Vulkan *headers* are
required at build time and `cmake/Dependencies.cmake` fetches them automatically when
cross-compiling. `glslc` runs on the host.

The result is a console `.exe` — `--headless` and `--list-gpus` print to the terminal that started
it — importing nothing but `KERNEL32`/`USER32`/`GDI32`/`SHELL32`/`OLE32`/`IMM32`/`SETUPAPI`/
`VERSION`/`WINMM`/`ADVAPI32` and the UCRT. It is the only file that has to be copied to the target
machine. (Cross-compiled and inspected with `objdump -p`; not executed on Windows.)

Two mingw-specific things `BuildOptions.cmake` handles, both of which look like unrelated
compiler bugs when they bite:

- **`-Wa,-mbig-obj` everywhere.** COFF caps an object at 32k sections, and HarfBuzz already asks for
  this flag on its own sources. GCC's `lto-wrapper` keeps assembler options only when *every* input
  object agrees on them, and otherwise drops them all — after which the LTO link dies on "COFF
  object format mismatch". Applying it project-wide keeps them in agreement.
- **`-lstdc++exp`.** `std::print`/`std::println` reach the Windows console through
  `std::__write_to_terminal`, which libstdc++ keeps in a separate archive that nothing pulls in
  automatically. There is no such split on Linux.

## SDL

This template asks SDL for a window, an event queue and a `VkSurfaceKHR`, and talks to Vulkan itself,
so `cmake/Dependencies.cmake` switches off everything else: `SDL_RENDER` (which is what pulls in the
Direct3D 9/11/12 and Vulkan *render drivers* on Windows), `SDL_GPU`, `SDL_DIRECTX`, `SDL_OPENGL`,
`SDL_OPENGLES`, `SDL_KMSDRM` (an EGL-only backend that will not compile without GLES), camera,
haptic, hidapi, joystick, sensor, power, dialog, tray and offscreen. `SDL_VULKAN` stays **on** — it
is `SDL_Vulkan_CreateSurface`, not a render backend, and the app does not start without it. Audio is
left switchable rather than simply off, behind `VKHB_SDL_AUDIO`, because it is the subsystem an app
built on this template is most likely to want back. If yours needs joysticks or the clipboard
dialogs, turn the corresponding line back on — the list is a starting point, not a rule.

A note on SDL and LTO, since it is the obvious thing to worry about: SDL routes every public entry
point through the dynamic-API jump table in `src/dynapi`, and `SDL_dynapi.h` deliberately `#error`s
if you try to define `SDL_DYNAMIC_API=0` from the command line — that switch is source-edit only. It
turns out not to matter: the table, its initializer and the implementations all sit in one
translation unit that LTO sees whole. This build links SDL with LTO like everything else.

## Fonts and licensing

The demo carries Inter (OFL-1.1). The loose-files build stages `fonts/LICENSE.txt` next to the
executable; an embedded build has nowhere to put it, so ship that license text alongside whatever
you distribute — and do the same for any face you add.
