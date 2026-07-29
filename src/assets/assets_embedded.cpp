#include "assets.hpp"

// Embedded backend: the whole asset tree is one xz stream sitting in .rodata, decompressed once on
// the first load() and served out of that buffer afterwards.
//
// Archive layout (uncompressed), all ASCII up to the payload:
//
//   VKHBA1 00000123\n            16-byte header: magic, then the TOC length in bytes, zero-padded,
//                                counting this line. Fixed width so it can be written before the
//                                rest of the TOC is measured.
//   shaders/ui_rect.vert.spv 0 1234\n     one line per entry: name, payload offset, byte size,
//   fonts/Inter-Regular.ttf 1234 98765\n  offsets relative to the end of the TOC.
//   <payload>                    entry bytes, concatenated in TOC order.
//
// cmake/PackAssets.cmake writes it; keep the two in step.

#include <vkhb_assets_size.hpp>  // generated: detail::kArchiveBytes

extern "C" {
#include <xz.h>
}

#include <charconv>
#include <cstdint>
#include <map>
#include <vector>

namespace vkhb::assets {

namespace {

// The compressed archive itself. #embed keeps it a plain byte array with no per-byte source text,
// which matters at a couple of megabytes; C23/C++26 spelling, supported by GCC 15+ and Clang 19+.
alignas(8) constexpr unsigned char kBlob[] = {
#embed <vkhb_assets.xz>
};

struct Archive {
  std::vector<std::byte> bytes;
  std::map<std::string, std::span<const std::byte>, std::less<>> entries;
  std::string error;  // non-empty if unpacking failed; reported on every load()
};

// Decompresses the blob into `out`. XZ_SINGLE mode makes the output buffer double as the LZMA2
// dictionary, so the decoder allocates nothing beyond its own state regardless of the dictionary
// size the compressor chose. pre: out.size() == the archive's exact uncompressed length.
std::string decompress(std::span<std::byte> out) {
  xz_crc32_init();
  xz_dec* dec = xz_dec_init(XZ_SINGLE, 0);
  if (!dec) return "xz_dec_init failed (out of memory)";

  xz_buf buf{
      .in = kBlob,
      .in_pos = 0,
      .in_size = sizeof kBlob,
      .out = reinterpret_cast<uint8_t*>(out.data()),
      .out_pos = 0,
      .out_size = out.size(),
  };
  const xz_ret ret = xz_dec_run(dec, &buf);
  xz_dec_end(dec);

  if (ret != XZ_STREAM_END)
    return "embedded asset archive is corrupt (xz_dec_run = " + std::to_string(static_cast<int>(ret)) + ")";
  if (buf.out_pos != out.size()) return "embedded asset archive has an unexpected size";
  return {};
}

// Reads a decimal field starting at `pos`, stopping at `delim`. Advances pos past the delimiter.
bool parse_field(std::string_view toc, size_t& pos, char delim, uint64_t& value) {
  const size_t end = toc.find(delim, pos);
  if (end == std::string_view::npos) return false;
  const auto [ptr, ec] = std::from_chars(toc.data() + pos, toc.data() + end, value);
  if (ec != std::errc{} || ptr != toc.data() + end) return false;
  pos = end + 1;
  return true;
}

// Splits the decompressed buffer into named spans. Returns an error string, empty on success.
std::string parse_toc(Archive& archive) {
  constexpr std::string_view kMagic = "VKHBA1 ";
  constexpr size_t kHeaderBytes = 16;  // "VKHBA1 " + 8 digits + '\n'

  const std::string_view all(reinterpret_cast<const char*>(archive.bytes.data()), archive.bytes.size());
  if (all.size() < kHeaderBytes || !all.starts_with(kMagic)) return "embedded asset archive has a bad header";

  uint64_t toc_bytes = 0;
  size_t pos = kMagic.size();
  if (!parse_field(all, pos, '\n', toc_bytes) || toc_bytes < kHeaderBytes || toc_bytes > all.size())
    return "embedded asset archive has a bad table of contents length";

  const std::string_view toc = all.substr(kHeaderBytes, toc_bytes - kHeaderBytes);
  const std::span<const std::byte> payload(archive.bytes.data() + toc_bytes, archive.bytes.size() - toc_bytes);

  for (size_t line = 0; line < toc.size();) {
    const size_t name_end = toc.find(' ', line);
    if (name_end == std::string_view::npos) return "embedded asset archive has a malformed entry";
    const std::string_view name = toc.substr(line, name_end - line);

    size_t field = name_end + 1;
    uint64_t offset = 0, size = 0;
    if (!parse_field(toc, field, ' ', offset) || !parse_field(toc, field, '\n', size))
      return "embedded asset archive has a malformed entry for '" + std::string(name) + "'";
    if (offset > payload.size() || size > payload.size() - offset)
      return "embedded asset archive entry '" + std::string(name) + "' runs past the end";

    archive.entries.emplace(std::string(name), payload.subspan(offset, size));
    line = field;
  }
  return {};
}

// Unpacked on first use rather than at load time: no static constructor, and a build that never
// draws (--list-gpus) never pays for it.
const Archive& archive() {
  static const Archive unpacked = [] {
    Archive a;
    a.bytes.resize(detail::kArchiveBytes);
    a.error = decompress(a.bytes);
    if (a.error.empty()) a.error = parse_toc(a);
    return a;
  }();
  return unpacked;
}

}  // namespace

bool embedded() { return true; }

void set_base_dir(std::filesystem::path) {}  // assets travel with the binary; nothing to point at

std::expected<std::span<const std::byte>, std::string> load(std::string_view name) {
  const Archive& a = archive();
  if (!a.error.empty()) return std::unexpected(a.error);
  const auto it = a.entries.find(name);
  if (it == a.entries.end()) return std::unexpected("no embedded asset named '" + std::string(name) + "'");
  return it->second;
}

}  // namespace vkhb::assets
