// vkhb_audio without a sound card: Engine::open_silent() mixes into nothing, so decoding, the
// MIX_Audio cache and every error path are testable in CI. What this cannot cover is whether a real
// device produces sound — that is what `vkhb_demo --music=...` is for.
//
// The music-file half only runs when tests/CMakeLists.txt found one in assets/ and passed its name
// in; the template has to stay buildable and testable with an empty assets/ directory.

#include "test_harness.hpp"

#include "assets/assets.hpp"
#include "audio/audio.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace {

// Opus is the one decoder in this build with an external dependency (opusfile), so it is also the
// one that silently disappears if cmake/Dependencies.cmake stops finding it.
void test_expected_decoders_are_present() {
  const std::vector<std::string> names = vkhb::audio::decoders();
  CHECK(!names.empty());
  for (const char* wanted : {"OPUS", "STBVORBIS", "WAV"}) {
    const bool found = std::ranges::find(names, wanted) != names.end();
    if (!found) std::println(stderr, "missing decoder: {}", wanted);
    CHECK(found);
  }
}

void test_missing_asset_is_an_error() {
  auto engine = vkhb::audio::Engine::open_silent();
  CHECK(engine.has_value());
  if (!engine) return;

  CHECK(!engine->play_music("does_not_exist.ogg").has_value());
  CHECK(!engine->play_sound("").has_value());
  CHECK(!engine->music_playing());
}

// Not a decodable stream: a font is bytes SDL_mixer must reject rather than misread.
void test_undecodable_asset_is_an_error() {
  auto engine = vkhb::audio::Engine::open_silent();
  CHECK(engine.has_value());
  if (!engine) return;

  CHECK(!engine->play_music("fonts/Inter-Regular.ttf").has_value());
}

void test_gain_roundtrips() {
  auto engine = vkhb::audio::Engine::open_silent();
  CHECK(engine.has_value());
  if (!engine) return;

  CHECK_NEAR(engine->gain(), 1.0, 1e-6);
  engine->set_gain(0.25f);
  CHECK_NEAR(engine->gain(), 0.25, 1e-6);
}

void test_music_playback(std::string_view asset_name) {
  auto engine = vkhb::audio::Engine::open_silent();
  CHECK(engine.has_value());
  if (!engine) return;

  CHECK(engine->play_music(asset_name, 0).has_value());
  CHECK(engine->music_playing());

  engine->pause_music();
  CHECK(engine->play_music(asset_name, -1).has_value());  // replaces whatever state the track was in
  CHECK(engine->music_playing());

  engine->stop_music();
  CHECK(!engine->music_playing());

  // Second play of the same name comes out of the MIX_Audio cache; it must behave identically.
  CHECK(engine->play_music(asset_name, 0).has_value());
  CHECK(engine->music_playing());
}

// The move operators hand the device over rather than duplicating or dropping it: the destination
// keeps working, and the two destructors between them run exactly one teardown.
void test_move_transfers_the_device() {
  auto engine = vkhb::audio::Engine::open_silent();
  CHECK(engine.has_value());
  if (!engine) return;

  engine->set_gain(0.75f);
  vkhb::audio::Engine moved = std::move(*engine);
  CHECK_NEAR(moved.gain(), 0.75, 1e-6);

  auto other = vkhb::audio::Engine::open_silent();
  CHECK(other.has_value());
  if (!other) return;

  // Assignment swaps, so `moved` ends up holding what `other` had and is still usable.
  *other = std::move(moved);
  CHECK_NEAR(other->gain(), 0.75, 1e-6);
}

// A one-shot goes to a track the mixer owns, so it must neither need nor disturb the music track.
void test_sound_is_independent_of_music(std::string_view asset_name) {
  auto engine = vkhb::audio::Engine::open_silent();
  CHECK(engine.has_value());
  if (!engine) return;

  CHECK(engine->play_sound(asset_name).has_value());
  CHECK(!engine->music_playing());

  CHECK(engine->play_music(asset_name, 0).has_value());
  CHECK(engine->play_sound(asset_name).has_value());
  CHECK(engine->music_playing());
}

// Two engines at once: each holds its own MIX_Init reference, so destroying the first must not tear
// the library out from under the second.
void test_engines_are_independent() {
  auto first = vkhb::audio::Engine::open_silent();
  CHECK(first.has_value());
  {
    auto second = vkhb::audio::Engine::open_silent();
    CHECK(second.has_value());
  }
  if (first) {
    first->set_gain(0.5f);
    CHECK_NEAR(first->gain(), 0.5, 1e-6);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) vkhb::assets::set_base_dir(argv[1]);

  test_expected_decoders_are_present();
  test_missing_asset_is_an_error();
  test_undecodable_asset_is_an_error();
  test_gain_roundtrips();
  test_move_transfers_the_device();
  test_engines_are_independent();

  if (argc > 2) {
    test_music_playback(argv[2]);
    test_sound_is_independent_of_music(argv[2]);
    std::println("test_audio: played '{}'", argv[2]);
  } else {
    std::println("test_audio: no music asset configured, playback test skipped");
  }

  return TEST_MAIN_RESULT();
}
