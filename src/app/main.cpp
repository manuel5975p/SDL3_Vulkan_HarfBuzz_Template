// Demo: a window, a cleared background, and every vkhb_ui primitive drawn once. The frame loop in
// run() is the part you copy; draw_demo_ui() is the part you delete.
// Flags: --gpu=N, --present-gpu=N, --frames=N, --screenshot=PATH (PPM), --list-gpus,
//        --present=fifo|mailbox|immediate (--no-vsync = immediate), --headless [--width/--height],
//        --music=ASSET, --no-music.

#include "assets/assets.hpp"
#ifdef VKHB_HAS_AUDIO
#include "audio/audio.hpp"
#endif
#include "render/color.hpp"
#include "render/headless.hpp"
#include "render/present.hpp"
#include "render/vk_context.hpp"
#include "ui/format.hpp"
#include "ui/panel_chrome.hpp"
#include "ui/text_wrap.hpp"
#include "ui/theme.hpp"
#include "ui/ui_input.hpp"
#include "ui/ui_renderer.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

using vkhb::render::HeadlessTarget;
using vkhb::render::PresentPreference;
using vkhb::render::Presenter;
using vkhb::render::VulkanContext;
using vkhb::ui::FontWeight;
using vkhb::ui::Rect;
using vkhb::ui::TextStyle;
using vkhb::ui::UiRenderer;
namespace theme = vkhb::ui::theme;

namespace {

struct Options {
  std::optional<uint32_t> gpu_index;
  std::optional<uint32_t> present_gpu_index;
  std::optional<uint64_t> max_frames;
  std::optional<std::string> screenshot_path;
  PresentPreference present = PresentPreference::Vsync;
  uint32_t width = 1280;
  uint32_t height = 800;
  bool headless = false;
  bool list_gpus = false;
  // The logical asset name of the looping background music; empty plays nothing. The default is the
  // Ogg Opus file this template ships in assets/ — point it elsewhere or clear it in your own app.
  // Used by run() only: a --headless or --list-gpus run opens no audio device and ignores this.
  std::string music = "embrace.ogg";
};

// The number behind a `--flag=`, for both plain and optional targets.
template <typename T>
struct ParsedValue {
  using type = T;
};
template <typename T>
struct ParsedValue<std::optional<T>> {
  using type = T;
};

Options parse_args(int argc, char** argv) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value_of = [&](std::string_view prefix) -> std::optional<std::string_view> {
      return arg.starts_with(prefix) ? std::optional(arg.substr(prefix.size())) : std::nullopt;
    };
    const auto parse = [](std::string_view v, auto& out) {
      typename ParsedValue<std::remove_reference_t<decltype(out)>>::type n = 0;
      if (std::from_chars(v.data(), v.data() + v.size(), n).ec == std::errc{}) out = n;
    };
    if (arg == "--list-gpus") {
      opts.list_gpus = true;
    } else if (arg == "--headless") {
      opts.headless = true;
    } else if (arg == "--no-music") {
      opts.music.clear();
    } else if (const auto v = value_of("--music=")) {
      opts.music = std::string(*v);
    } else if (arg == "--no-vsync") {
      // Deliberately Immediate, not Mailbox: --no-vsync has to mean "do not wait for the refresh"
      // whatever the surface offers, and Mailbox falls back to FIFO on a surface without MAILBOX.
      opts.present = PresentPreference::Immediate;
    } else if (const auto v = value_of("--present=")) {
      if (*v == "fifo" || *v == "vsync") opts.present = PresentPreference::Vsync;
      else if (*v == "mailbox") opts.present = PresentPreference::Mailbox;
      else if (*v == "immediate") opts.present = PresentPreference::Immediate;
      else std::println(stderr, "Unknown present mode '{}' (fifo|mailbox|immediate)", *v);
    } else if (const auto v = value_of("--width=")) {
      parse(*v, opts.width);
    } else if (const auto v = value_of("--height=")) {
      parse(*v, opts.height);
    } else if (const auto v = value_of("--gpu=")) {
      parse(*v, opts.gpu_index);
    } else if (const auto v = value_of("--present-gpu=")) {
      parse(*v, opts.present_gpu_index);
    } else if (const auto v = value_of("--frames=")) {
      parse(*v, opts.max_frames);
    } else if (const auto v = value_of("--screenshot=")) {
      opts.screenshot_path = std::string(*v);
    } else {
      std::println(stderr, "Unknown argument: {}", arg);
    }
  }
  return opts;
}

// BGRA8 (as produced by the UNORM offscreen target) -> binary PPM. Dependency-free frame capture.
bool write_ppm_from_bgra(const std::filesystem::path& path, const uint8_t* bgra, uint32_t width, uint32_t height) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << "P6\n" << width << ' ' << height << "\n255\n";
  std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = bgra + static_cast<size_t>(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 3 + 0] = src[x * 4 + 2];
      row[x * 3 + 1] = src[x * 4 + 1];
      row[x * 3 + 2] = src[x * 4 + 0];
    }
    out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
  }
  return static_cast<bool>(out);
}

// Everything draw_demo_ui() mutates. A real app puts its own state here.
struct DemoState {
  bool panel_open = false;
  bool running = true;
  uint64_t counter = 1234567;
};

void draw_demo_ui(UiRenderer& ui, VkExtent2D extent, DemoState& state, float fps, uint64_t frame_count,
                  const char* present_mode) {
  const float w = static_cast<float>(extent.width);
  const TextStyle faint_label{.size = 12.0f, .color = theme::text_faint()};

  // --- top bar ---
  constexpr float kBarH = 44.0f;
  ui.rect({0, 0, w, kBarH}, theme::glass());
  ui.line(0, kBarH, w, kBarH, 1.0f, theme::edge());
  const TextStyle bar_title{.size = 16.0f, .weight = FontWeight::SemiBold};
  const float bar_baseline = kBarH * 0.5f + ui.ascent(bar_title) * 0.5f;
  ui.text(20.0f, bar_baseline, "SDL3 \xc2\xb7 Vulkan \xc2\xb7 HarfBuzz", bar_title);

  if (ui.button({w - 300.0f, 7.0f, 90.0f, 30.0f}, state.panel_open ? "Close" : "Panel"))
    state.panel_open = !state.panel_open;

  const TextStyle readout_style{.size = 13.0f, .color = theme::text_dim()};
  const std::string readout =
      std::format("{}   {:.0f} fps   frame {}", present_mode, fps, vkhb::ui::format_money(frame_count));
  ui.text(w - 20.0f - ui.measure_text(readout, readout_style), bar_baseline, readout, readout_style);

  // --- three weights: outlines are evaluated per fragment, so any size stays sharp ---
  float y = kBarH + 60.0f;
  for (const auto& [weight, name] : std::array<std::pair<FontWeight, const char*>, 3>{
           {{FontWeight::Regular, "Regular"}, {FontWeight::SemiBold, "SemiBold"}, {FontWeight::Bold, "Bold"}}}) {
    ui.text(40.0f, y, name, faint_label);
    ui.text(140.0f, y, "Grumpy wizards make toxic brew 0123456789", {.size = 26.0f, .weight = weight});
    y += 46.0f;
  }

  // --- primitives ---
  y += 24.0f;
  ui.text(40.0f, y, "PRIMITIVES", faint_label);
  y += 20.0f;
  ui.rect({40.0f, y, 90.0f, 56.0f}, theme::ink600());
  ui.rect({146.0f, y, 90.0f, 56.0f}, theme::accent_dim(), 12.0f);
  ui.rect({252.0f, y, 110.0f, 56.0f}, theme::success(), 28.0f);
  ui.circle(406.0f, y + 28.0f, 28.0f, theme::warning());
  ui.line(456.0f, y + 52.0f, 560.0f, y + 4.0f, 4.0f, theme::danger());

  // --- buttons: default, accent, danger ---
  y += 96.0f;
  ui.text(40.0f, y, "BUTTONS", faint_label);
  y += 20.0f;
  if (ui.button({40.0f, y, 120.0f, 36.0f}, "Add 1000")) state.counter += 1000;
  if (ui.button({172.0f, y, 120.0f, 36.0f}, "Reset",
                {.fill = theme::accent_dim(), .hover_fill = theme::accent()}))
    state.counter = 0;
  if (ui.button({304.0f, y, 120.0f, 36.0f}, "Quit",
                {.fill = theme::ink600(), .hover_fill = theme::danger()}))
    state.running = false;
  ui.text(440.0f, y + 24.0f, vkhb::ui::format_money(state.counter),
          {.size = 18.0f, .color = theme::accent(), .weight = FontWeight::SemiBold});

  // --- word wrap ---
  y += 76.0f;
  ui.text(40.0f, y, "WORD WRAP", faint_label);
  y += 22.0f;
  const TextStyle body{.size = 15.0f, .color = theme::text_dim()};
  for (const std::string& line : vkhb::ui::wrap_text(
           ui,
           "Each unique glyph is shaped once by HarfBuzz, encoded into a curve-band blob by libharfbuzz-gpu, "
           "and evaluated per fragment: one quad per glyph, sharp at any scale.",
           460.0f, body)) {
    ui.text(40.0f, y, line, body);
    y += 22.0f;
  }

  ui.text(40.0f, static_cast<float>(extent.height) - 28.0f,
          state.panel_open ? "Click the backdrop or \xc3\x97 to close"
                           : "TAB: modal panel \xc2\xb7 P: present mode \xc2\xb7 ESC: quit",
          faint_label);

  if (!state.panel_open) return;

  // --- modal panel ---
  const auto chrome = vkhb::ui::draw_panel_chrome(ui, extent, "Panel", "modal chrome from vkhb_ui");
  if (chrome.wants_close) state.panel_open = false;

  float row_y = chrome.body.y + 28.0f;
  for (int i = 0; i < 6; ++i) {
    const Rect row{chrome.body.x, row_y - 20.0f, chrome.body.w, 32.0f};
    ui.rect(row, ui.input().hovers(row) ? theme::glass_hi() : theme::ink700(), 8.0f);
    ui.text(row.x + 14.0f, row_y, std::format("Row {}", i + 1), {.size = 14.0f});
    const TextStyle value_style{.size = 14.0f, .color = theme::accent(), .weight = FontWeight::SemiBold};
    const std::string value = vkhb::ui::format_money(state.counter * static_cast<uint64_t>(i + 1));
    ui.text(row.x + row.w - 14.0f - ui.measure_text(value, value_style), row_y, value, value_style);
    row_y += 40.0f;
  }
}

// Enumerates devices against a real surface, so present capability is reported accurately. With no
// display (a bare TTY, ssh) it falls back to surfaceless probing: every device then reports present=no.
int run_list_gpus() {
  const bool video = SDL_Init(SDL_INIT_VIDEO);
  SDL_Window* window =
      video ? SDL_CreateWindow("vkhb probe", 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN) : nullptr;
  if (!window) {
    std::println(stderr, "no display ({}); present capability unknown", SDL_GetError());
  }
  const auto gpus = VulkanContext::probe_gpus(window);
  if (gpus) {
    std::println("{} Vulkan physical device(s):", gpus->size());
    for (size_t i = 0; i < gpus->size(); ++i) {
      const auto& g = (*gpus)[i];
      std::println("  [{}] {} ({}) render={} present={}", i, g.name, vkhb::render::gpu_type_name(g.type),
                   g.can_render() ? "yes" : "no", !window ? "?" : g.can_present() ? "yes" : "no");
    }
  } else {
    std::println(stderr, "could not enumerate GPUs: {}", gpus.error());
  }

  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
  return gpus ? 0 : 1;
}

int run(const Options& opts) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("vkhb demo", static_cast<int>(opts.width), static_cast<int>(opts.height),
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    std::println(stderr, "SDL_CreateWindow failed: {}", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  int exit_code = 1;
  {  // Scoped so every Vulkan RAII member is destroyed before SDL_DestroyWindow below.
    const auto fail = [&](std::string_view what, const std::string& why) {
      std::println(stderr, "{}: {}", what, why);
      return 1;
    };

    auto ctx_result = VulkanContext::create(window, opts.gpu_index, opts.present_gpu_index);
    if (!ctx_result) {
      SDL_DestroyWindow(window);
      SDL_Quit();
      return fail("Vulkan init failed", ctx_result.error());
    }
    VulkanContext ctx = std::move(*ctx_result);

    if (ctx.cross_gpu())
      std::println("Rendering on {} ({}), presenting via {} — frames cross through host memory",
                   ctx.render().gpu().name, vkhb::render::gpu_type_name(ctx.render().gpu().type),
                   ctx.present().gpu().name);
    else
      std::println("Using GPU: {} ({})", ctx.render().gpu().name, vkhb::render::gpu_type_name(ctx.render().gpu().type));

    int pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
    auto presenter_result = Presenter::create(ctx, static_cast<uint32_t>(pixel_w), static_cast<uint32_t>(pixel_h));
    if (!presenter_result) {
      SDL_DestroyWindow(window);
      SDL_Quit();
      return fail("Presenter init failed", presenter_result.error());
    }
    Presenter presenter = std::move(*presenter_result);

    // Asking for a mode is not getting one: each preference falls back down to something the
    // surface offers, and MAILBOX only does anything with three images — so print what was actually
    // obtained rather than what was requested (see docs/presentation.md).
    if (opts.present != PresentPreference::Vsync) {
      if (auto r = presenter.set_present_preference(opts.present); !r)
        std::println(stderr, "Could not switch present mode: {}", r.error());
    }
    std::println("Present mode: {} (asked for {}), {} swapchain images", presenter.present_mode_name(),
                 vkhb::render::present_preference_name(opts.present), presenter.swapchain_image_count());

    auto ui_result = UiRenderer::create(ctx.render(), Presenter::kColorFormat);
    if (!ui_result) {
      SDL_DestroyWindow(window);
      SDL_Quit();
      return fail("UI renderer init failed", ui_result.error());
    }
    UiRenderer ui = std::move(*ui_result);

#ifdef VKHB_HAS_AUDIO
    // Audio is a nice-to-have here: a machine with no sound server, or an assets/ without the music
    // file in it, still gets its window — every failure below is a warning, not a return. The
    // Engine outlives the frame loop and stops the music when this scope ends.
    std::optional<vkhb::audio::Engine> audio;
    if (!opts.music.empty()) {
      if (auto engine = vkhb::audio::Engine::open(); !engine) {
        std::println(stderr, "Audio disabled: {}", engine.error());
      } else {
        audio.emplace(std::move(*engine));
        if (auto played = audio->play_music(opts.music); played) {
          std::string decoders;
          for (const std::string& name : vkhb::audio::decoders())
            decoders += (decoders.empty() ? "" : ", ") + name;
          std::println("Music: {} (decoders: {})", opts.music, decoders);
        } else {
          std::println(stderr, "Music disabled: {}", played.error());
        }
      }
    }
#endif

    // The offscreen target is UNORM, so the clear value is raw sRGB — nothing encodes it on write.
    const glm::vec3 clear = vkhb::render::hex_to_srgb(0x0b0f14);

    DemoState state;
    bool ok = true, resize_pending = false, was_down = false;
    uint64_t frames_rendered = 0, last_ticks = SDL_GetTicksNS();
    float fps = 0.0f;
    vkhb::ui::UiInput input;

    while (state.running) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) state.running = false;
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) resize_pending = true;
        if (event.type == SDL_EVENT_KEY_DOWN) {
          if (event.key.key == SDLK_ESCAPE) state.running = false;
          if (event.key.key == SDLK_TAB) state.panel_open = !state.panel_open;
          if (event.key.key == SDLK_P) {  // cycle vsync -> mailbox -> immediate; rebuilds the swapchain
            const auto next = [](PresentPreference p) {
              switch (p) {
                case PresentPreference::Vsync: return PresentPreference::Mailbox;
                case PresentPreference::Mailbox: return PresentPreference::Immediate;
                case PresentPreference::Immediate: return PresentPreference::Vsync;
              }
              return PresentPreference::Vsync;
            };
            if (auto r = presenter.set_present_preference(next(presenter.present_preference())); !r)
              std::println(stderr, "Present mode switch failed: {}", r.error());
            else
              std::println("Present mode: {}, {} swapchain images", presenter.present_mode_name(),
                           presenter.swapchain_image_count());
          }
        }
      }
      if (!state.running) break;

      SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
      if (pixel_w <= 0 || pixel_h <= 0) {
        SDL_Delay(10);  // minimized: nothing to render into
        continue;
      }

      // Pointer state in physical pixels (UI space), with press/release edges for hit-testing.
      int win_w = 1, win_h = 1;
      SDL_GetWindowSize(window, &win_w, &win_h);
      float mx = 0.0f, my = 0.0f;
      const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
      const bool down = (buttons & SDL_BUTTON_LMASK) != 0;
      input = {
          .mouse_x = mx * static_cast<float>(pixel_w) / static_cast<float>(std::max(win_w, 1)),
          .mouse_y = my * static_cast<float>(pixel_h) / static_cast<float>(std::max(win_h, 1)),
          .down = down,
          .pressed = down && !was_down,
          .released = !down && was_down,
      };
      was_down = down;

      if (resize_pending) {
        if (auto r = presenter.resize(static_cast<uint32_t>(pixel_w), static_cast<uint32_t>(pixel_h)); !r) {
          ok = fail("resize failed", r.error()) == 0;
          break;
        }
        resize_pending = false;
      }

      const uint64_t now_ticks = SDL_GetTicksNS();
      const float dt = std::min(0.1f, static_cast<float>(now_ticks - last_ticks) / 1e9f);
      last_ticks = now_ticks;
      if (dt > 0.0f) fps = fps == 0.0f ? 1.0f / dt : fps * 0.92f + (1.0f / dt) * 0.08f;

      auto target = presenter.begin_frame(clear);
      if (!target) {
        ok = false;
        std::println(stderr, "begin_frame failed: {}", target.error());
        break;
      }
      if (!*target) {  // swapchain out of date
        resize_pending = true;
        continue;
      }

      ui.begin_frame((*target)->extent, input);
      draw_demo_ui(ui, (*target)->extent, state, fps, frames_rendered, presenter.present_mode_name());
      ui.render((*target)->cmd, (*target)->frame_index, (*target)->color_view, (*target)->extent);

      if (auto r = presenter.end_frame(); !r) {
        ok = false;
        std::println(stderr, "end_frame failed: {}", r.error());
        break;
      }

      // The last frame of the run, so a --frames=N screenshot shows the state at frame N-1.
      const uint64_t shot_frame = opts.max_frames ? *opts.max_frames - 1 : 0;
      if (opts.screenshot_path && frames_rendered == shot_frame) {
        if (auto pixels = presenter.read_last_frame()) {
          const VkExtent2D e = presenter.extent();
          if (write_ppm_from_bgra(*opts.screenshot_path, pixels->data(), e.width, e.height))
            std::println("Wrote screenshot to {}", *opts.screenshot_path);
          else
            std::println(stderr, "Failed to write screenshot to {}", *opts.screenshot_path);
        } else {
          std::println(stderr, "Screenshot readback failed: {}", pixels.error());
        }
      }

      ++frames_rendered;
      if (opts.max_frames && frames_rendered >= *opts.max_frames) state.running = false;
    }

    exit_code = ok ? 0 : 1;
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  return exit_code;
}

// No window, no surface, no swapchain: the same UI is drawn into an offscreen target on a
// surfaceless device and read back. This is what makes the render path testable without a display —
// given the same inputs the output is byte-exact, so a screenshot is a regression test.
int run_headless(const Options& opts) {
  auto ctx_result = VulkanContext::create(nullptr, opts.gpu_index);
  if (!ctx_result) {
    std::println(stderr, "Headless Vulkan init failed: {}", ctx_result.error());
    return 1;
  }
  const VulkanContext ctx = std::move(*ctx_result);
  std::println("Headless on GPU: {} ({})", ctx.render().gpu().name,
               vkhb::render::gpu_type_name(ctx.render().gpu().type));

  auto target_result = HeadlessTarget::create(ctx.render(), opts.width, opts.height);
  if (!target_result) {
    std::println(stderr, "Headless target init failed: {}", target_result.error());
    return 1;
  }
  HeadlessTarget target = std::move(*target_result);

  auto ui_result = UiRenderer::create(ctx.render(), HeadlessTarget::kColorFormat);
  if (!ui_result) {
    std::println(stderr, "UI renderer init failed: {}", ui_result.error());
    return 1;
  }
  UiRenderer ui = std::move(*ui_result);

  const glm::vec3 clear = vkhb::render::hex_to_srgb(0x0b0f14);
  const uint64_t frames = opts.max_frames.value_or(1);
  DemoState state;
  // No pointer device: a zeroed input leaves every widget in its resting state.
  const vkhb::ui::UiInput input;

  for (uint64_t i = 0; i < frames; ++i) {
    auto frame = target.begin_frame(clear);
    if (!frame) {
      std::println(stderr, "begin_frame failed: {}", frame.error());
      return 1;
    }
    ui.begin_frame(frame->extent, input);
    draw_demo_ui(ui, frame->extent, state, 0.0f, i, "headless");
    ui.render(frame->cmd, frame->frame_index, frame->color_view, frame->extent);
    if (auto r = target.end_frame(); !r) {
      std::println(stderr, "end_frame failed: {}", r.error());
      return 1;
    }
  }

  if (opts.screenshot_path) {  // the last frame drawn, as in the windowed path
    auto pixels = target.read_frame();
    if (!pixels) {
      std::println(stderr, "Screenshot readback failed: {}", pixels.error());
      return 1;
    }
    const VkExtent2D e = target.extent();
    if (!write_ppm_from_bgra(*opts.screenshot_path, pixels->data(), e.width, e.height)) {
      std::println(stderr, "Failed to write screenshot to {}", *opts.screenshot_path);
      return 1;
    }
    std::println("Wrote screenshot to {}", *opts.screenshot_path);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opts = parse_args(argc, argv);

  // Loose-files builds read shaders and fonts from next to the executable; embedded builds ignore
  // this and serve them out of the binary. Set before anything touches an asset either way.
  const char* base_path = SDL_GetBasePath();
  vkhb::assets::set_base_dir(base_path ? base_path : "./");

  if (opts.list_gpus) return run_list_gpus();
  if (opts.headless) return run_headless(opts);
  return run(opts);
}
