#include "audio.hpp"

// MIX_Init/MIX_Quit are reference counted, so every entry point that touches SDL_mixer pairs one
// with the other and the library stays alive exactly as long as something needs it.
//
// The MIX_Audio cache is keyed by logical name and lives as long as the Engine. Caching is not just
// a speed trick: MIX_LoadAudioNoCopy borrows the asset bytes rather than copying them, which is
// only sound because vkhb::assets promises process-lifetime spans (see assets.hpp).

#include "assets/assets.hpp"

#include <SDL3/SDL_audio.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <cassert>
#include <map>
#include <utility>

namespace vkhb::audio {

namespace {

std::string sdl_error(std::string_view what) {
  const char* detail = SDL_GetError();
  return std::string(what) + ": " + (detail && *detail ? detail : "unknown error");
}

}  // namespace

struct Engine::Impl {
  MIX_Mixer* mixer = nullptr;
  MIX_Track* music = nullptr;
  std::map<std::string, MIX_Audio*, std::less<>> cache;

  // The MIX_Audio for a logical name, decoded straight out of the asset span.
  std::expected<MIX_Audio*, std::string> audio_for(std::string_view name) {
    if (const auto it = cache.find(name); it != cache.end()) return it->second;

    const auto bytes = assets::load(name);
    if (!bytes) return std::unexpected(bytes.error());

    MIX_Audio* audio = MIX_LoadAudioNoCopy(mixer, bytes->data(), bytes->size(), false);
    if (!audio) return std::unexpected(sdl_error(std::string("could not decode '") + std::string(name) + "'"));

    return cache.emplace(std::string(name), audio).first->second;
  }
};

std::expected<Engine, std::string> Engine::open() { return open_mixer(true); }

std::expected<Engine, std::string> Engine::open_silent() { return open_mixer(false); }

std::expected<Engine, std::string> Engine::open_mixer(bool with_device) {
  if (!MIX_Init()) return std::unexpected(sdl_error("MIX_Init failed"));

  // Float samples at 48 kHz stereo for the deviceless case: the format every decoder here produces
  // natively, so a silent mixer does no resampling work it would not do in front of a real device.
  const SDL_AudioSpec spec{.format = SDL_AUDIO_F32, .channels = 2, .freq = 48000};
  MIX_Mixer* mixer = with_device ? MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr)
                                 : MIX_CreateMixer(&spec);
  if (!mixer) {
    // Read the error out before unwinding: MIX_Quit tears down decoders and the SDL audio
    // subsystem, and anything in there that succeeds may leave a cleared or unrelated SDL error.
    const std::string why = sdl_error(with_device ? "could not open an audio device"
                                                  : "could not create a deviceless mixer");
    MIX_Quit();
    return std::unexpected(why);
  }

  MIX_Track* music = MIX_CreateTrack(mixer);
  if (!music) {
    const std::string why = sdl_error("could not create the music track");
    MIX_DestroyMixer(mixer);
    MIX_Quit();
    return std::unexpected(why);
  }

  Engine engine;
  engine.impl_ = new Impl{.mixer = mixer, .music = music, .cache = {}};
  return engine;
}

Engine::Engine(Engine&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}

// Swap rather than destroy-then-steal: `other`'s destructor disposes of whatever this held, and a
// self-move stays a no-op without a guard.
Engine& Engine::operator=(Engine&& other) noexcept {
  std::swap(impl_, other.impl_);
  return *this;
}

Engine::~Engine() {
  if (!impl_) return;
  // Order matters: tracks and MIX_Audio belong to the mixer, and MIX_Quit would free them anyway,
  // but doing it explicitly keeps a moved-from-then-reopened Engine from tripping over stale ones.
  for (const auto& [name, audio] : impl_->cache) MIX_DestroyAudio(audio);
  MIX_DestroyTrack(impl_->music);
  MIX_DestroyMixer(impl_->mixer);
  MIX_Quit();
  delete impl_;
  impl_ = nullptr;
}

// Every method below dereferences impl_, which is null only on a moved-from Engine: using one is a
// caller error, so it is an assert rather than a branch on a state the type says cannot happen.
std::expected<void, std::string> Engine::play_music(std::string_view asset_name, int loops) {
  assert(impl_ && "play_music on a moved-from Engine");
  assert(loops >= -1 && "loops must be -1 (forever) or a repeat count");

  const auto audio = impl_->audio_for(asset_name);
  if (!audio) return std::unexpected(audio.error());

  if (!MIX_SetTrackAudio(impl_->music, *audio))
    return std::unexpected(sdl_error("could not assign the music track"));

  // The loop count has to ride in on the play properties: MIX_SetTrackLoops only adjusts a track
  // that is already playing, and starting a stopped track resets it.
  const SDL_PropertiesID options = SDL_CreateProperties();
  if (options == 0) return std::unexpected(sdl_error("SDL_CreateProperties failed"));

  SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, loops);
  const bool played = MIX_PlayTrack(impl_->music, options);
  SDL_DestroyProperties(options);

  if (!played) return std::unexpected(sdl_error(std::string("could not play '") + std::string(asset_name) + "'"));
  return {};
}

// A track with nothing assigned has no sample rate to convert against, so MIX_TrackMSToFrames
// answers -1; MIX_StopTrack reads any non-positive fade as "stop now", which is what a stop on an
// idle track should do anyway.
void Engine::stop_music(int fade_out_ms) {
  assert(impl_ && "stop_music on a moved-from Engine");
  MIX_StopTrack(impl_->music, MIX_TrackMSToFrames(impl_->music, fade_out_ms));
}

void Engine::pause_music() {
  assert(impl_ && "pause_music on a moved-from Engine");
  MIX_PauseTrack(impl_->music);
}

void Engine::resume_music() {
  assert(impl_ && "resume_music on a moved-from Engine");
  MIX_ResumeTrack(impl_->music);
}

bool Engine::music_playing() const {
  assert(impl_ && "music_playing on a moved-from Engine");
  return MIX_TrackPlaying(impl_->music);
}

std::expected<void, std::string> Engine::play_sound(std::string_view asset_name) {
  assert(impl_ && "play_sound on a moved-from Engine");

  const auto audio = impl_->audio_for(asset_name);
  if (!audio) return std::unexpected(audio.error());

  if (!MIX_PlayAudio(impl_->mixer, *audio))
    return std::unexpected(sdl_error(std::string("could not play '") + std::string(asset_name) + "'"));
  return {};
}

void Engine::set_gain(float gain) {
  assert(impl_ && "set_gain on a moved-from Engine");
  assert(gain >= 0.0f && "gain must not be negative");
  MIX_SetMixerGain(impl_->mixer, gain);
}

float Engine::gain() const {
  assert(impl_ && "gain on a moved-from Engine");
  return MIX_GetMixerGain(impl_->mixer);
}

std::vector<std::string> decoders() {
  if (!MIX_Init()) return {};

  std::vector<std::string> names;
  const int count = MIX_GetNumAudioDecoders();
  names.reserve(static_cast<size_t>(count > 0 ? count : 0));
  for (int i = 0; i < count; ++i) {
    if (const char* name = MIX_GetAudioDecoder(i)) names.emplace_back(name);
  }

  MIX_Quit();
  return names;
}

}  // namespace vkhb::audio
