#pragma once

// Audio playback on top of SDL3_mixer. Everything is addressed by the same logical asset names the
// renderers use — a file dropped at assets/embrace.ogg plays as "embrace.ogg" — so a loose-files
// build and an embedded build play the same bytes; Engine hands the asset span straight to
// SDL_mixer without copying it.
//
// Decoding happens on SDL's audio thread, so nothing here needs a per-frame update() call. The
// codecs actually compiled in are chosen in cmake/Dependencies.cmake (Ogg Opus, Ogg Vorbis, WAV by
// default); decoders() reports what this build ended up with.

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace vkhb::audio {

// One mixer device plus the MIX_Audio cache behind it. Move-only: it owns the device.
// Exactly one instance should exist at a time; a second open() opens a second device.
// A moved-from Engine may only be destroyed or assigned to — every other member asserts.
class Engine {
 public:
  // Opens the default playback device. Fails when there is no audio device or SDL_INIT_AUDIO was
  // never initialized — a caller that only wants pictures can treat that as non-fatal.
  static std::expected<Engine, std::string> open();

  // A mixer with no output device: audio is decoded and mixed on demand but never heard. For tests
  // and headless runs, where opening a device would fail or block on a sound server.
  static std::expected<Engine, std::string> open_silent();

  Engine(Engine&& other) noexcept;
  Engine& operator=(Engine&& other) noexcept;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine();

  // Starts asset_name on the music track, replacing whatever was playing there.
  // pre: loops >= -1, where -1 repeats forever and 0 plays once.
  std::expected<void, std::string> play_music(std::string_view asset_name, int loops = -1);

  // Fades the music track out over fade_out_ms and stops it. Silent no-op when nothing is playing.
  void stop_music(int fade_out_ms = 0);
  void pause_music();
  void resume_music();
  bool music_playing() const;

  // Fire-and-forget one-shot on a track the mixer picks and reclaims. Unrelated to the music track,
  // so it neither stops nor is stopped by play_music().
  std::expected<void, std::string> play_sound(std::string_view asset_name);

  // Master gain for everything this engine plays. 1.0 is unattenuated; above that clips.
  // pre: gain >= 0.
  void set_gain(float gain);
  float gain() const;

 private:
  struct Impl;
  Engine() = default;
  // The shared body of both factories; with_device picks MIX_CreateMixerDevice over MIX_CreateMixer.
  static std::expected<Engine, std::string> open_mixer(bool with_device);

  Impl* impl_ = nullptr;
};

// The decoder names SDL_mixer registered in this build, e.g. "OGG", "OPUS", "WAV". Callable before
// any Engine exists; empty if SDL_mixer failed to initialize.
std::vector<std::string> decoders();

}  // namespace vkhb::audio
