#include "assets.hpp"

// Loose-files backend: logical names resolve under a base directory, which for a dev build is the
// build tree the shader/font staging rules write into. Contents are cached because assets.hpp
// promises process-lifetime spans, and because a font blob handed to HarfBuzz must outlive the call.

#include <fstream>
#include <map>
#include <utility>
#include <vector>

namespace vkhb::assets {

namespace {

std::filesystem::path& base_dir() {
  static std::filesystem::path dir{"."};
  return dir;
}

// std::less<> so a string_view can be looked up without allocating a std::string first.
std::map<std::string, std::vector<std::byte>, std::less<>>& cache() {
  static std::map<std::string, std::vector<std::byte>, std::less<>> c;
  return c;
}

}  // namespace

bool embedded() { return false; }

void set_base_dir(std::filesystem::path dir) { base_dir() = std::move(dir); }

std::expected<std::span<const std::byte>, std::string> load(std::string_view name) {
  if (const auto it = cache().find(name); it != cache().end()) return std::span<const std::byte>(it->second);

  const std::filesystem::path path = base_dir() / name;
  // Checked before opening: a directory opens fine on POSIX and then reports a nonsense size, which
  // would turn a mistyped asset name into a multi-exabyte allocation rather than an error.
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return std::unexpected("asset '" + path.string() + "' is not a readable file");

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return std::unexpected("could not open asset '" + path.string() + "'");

  const std::streamsize size = file.tellg();
  if (size < 0) return std::unexpected("could not size asset '" + path.string() + "'");
  file.seekg(0);

  std::vector<std::byte> data(static_cast<size_t>(size));
  if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size))
    return std::unexpected("failed reading asset '" + path.string() + "'");

  const auto [it, _] = cache().emplace(std::string(name), std::move(data));
  return std::span<const std::byte>(it->second);
}

}  // namespace vkhb::assets
