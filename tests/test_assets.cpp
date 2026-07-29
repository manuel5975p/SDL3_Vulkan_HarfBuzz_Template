// The asset layer from the outside, so the same assertions cover both backends: the loose-files one
// reading the staged build tree, and the embedded one unpacking the xz archive out of its own
// .rodata. A failure here after a packer change means cmake/PackAssets.cmake and
// src/assets/assets_embedded.cpp have drifted apart.

#include "test_harness.hpp"

#include "assets/assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace {

std::span<const std::byte> load_or_report(std::string_view name) {
  auto bytes = vkhb::assets::load(name);
  if (!bytes) {
    std::println(stderr, "CHECK FAILED: load('{}'): {}", name, bytes.error());
    ++vkhb::test::g_failures;
    return {};
  }
  return *bytes;
}

uint32_t read_be32(std::span<const std::byte> data) {
  if (data.size() < 4) return 0;
  return uint32_t(data[0]) << 24 | uint32_t(data[1]) << 16 | uint32_t(data[2]) << 8 | uint32_t(data[3]);
}

// Every compiled shader must be a plausible SPIR-V module: the magic word in either endianness, and
// a size that is a whole number of 32-bit words, which is what load_shader_module insists on.
void test_shaders() {
  constexpr std::array kShaders{"shaders/ui_rect.vert.spv", "shaders/ui_rect.frag.spv", "shaders/ui_text.vert.spv",
                                "shaders/ui_text.frag.spv"};
  for (const char* name : kShaders) {
    const std::span<const std::byte> spv = load_or_report(name);
    CHECK(spv.size() >= 20);
    CHECK(spv.size() % 4 == 0);
    uint32_t magic = 0;
    if (spv.size() >= 4) std::memcpy(&magic, spv.data(), 4);
    CHECK(magic == 0x07230203u || magic == 0x03022307u);
  }
}

// The three faces UiRenderer loads, identified by their sfnt tag: 0x00010000 for TrueType outlines.
void test_fonts() {
  constexpr std::array kTrueType{"fonts/Inter-Regular.ttf", "fonts/Inter-SemiBold.ttf", "fonts/Inter-Bold.ttf"};
  for (const char* name : kTrueType) {
    const std::span<const std::byte> font = load_or_report(name);
    CHECK(font.size() > 4096);
    CHECK(read_be32(font) == 0x00010000u);
  }
}

// A second load() must hand back the very same bytes. ui_font.cpp wraps this memory in a READONLY
// hb_blob_t and never copies it, so a backend returning a fresh buffer per call would leave every
// Font pointing at freed memory.
void test_spans_are_stable() {
  const std::span<const std::byte> first = load_or_report("fonts/Inter-Regular.ttf");
  const std::span<const std::byte> second = load_or_report("fonts/Inter-Regular.ttf");
  CHECK(first.data() == second.data());
  CHECK(first.size() == second.size());
}

void test_missing_asset_is_an_error() {
  CHECK(!vkhb::assets::load("shaders/does_not_exist.spv").has_value());
  CHECK(!vkhb::assets::load("").has_value());
  CHECK(!vkhb::assets::load("fonts").has_value());  // a directory is not an asset
}

}  // namespace

int main(int argc, char** argv) {
  // The loose-files backend has to be told where the staged assets are; tests/CMakeLists.txt passes
  // the build directory in. The embedded backend ignores it.
  if (argc > 1) vkhb::assets::set_base_dir(argv[1]);

  test_shaders();
  test_fonts();
  test_spans_are_stable();
  test_missing_asset_is_an_error();

  std::println("test_assets: {} backend", vkhb::assets::embedded() ? "embedded" : "loose-files");
  return TEST_MAIN_RESULT();
}
