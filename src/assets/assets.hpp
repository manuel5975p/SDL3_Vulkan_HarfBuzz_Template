#pragma once

// Runtime asset lookup by logical name. Every asset the app reads after startup — compiled SPIR-V
// and font files — is addressed as a slash-separated name like "shaders/ui_rect.vert.spv", never as
// a path. Two interchangeable backends answer those names (src/assets/assets_dir.cpp and
// assets_embedded.cpp, one of which is linked in): a directory next to the executable, or an xz
// archive compiled into the binary. Callers cannot tell the difference, which is the point — the
// renderers stay free of any notion of where a build put its files.

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace vkhb::assets {

// True when this binary carries its own assets, i.e. it was built with VKHB_EMBED_ASSETS=ON.
bool embedded();

// Sets the directory logical names resolve against; ignored by the embedded backend.
// pre: called before the first load(), typically once at startup with SDL_GetBasePath().
void set_base_dir(std::filesystem::path dir);

// The bytes of one asset. The span stays valid until the process exits — both backends keep the
// data alive — so HarfBuzz can hold a font blob without copying it, and a caller may cache the span.
// pre: name is a logical name, not a path (no "./", no absolute paths, '/' as separator).
std::expected<std::span<const std::byte>, std::string> load(std::string_view name);

}  // namespace vkhb::assets
