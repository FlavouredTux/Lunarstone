// Sound: decoding, the mixer, roll-off, and the sound system following a
// game's Sounds.

#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/audio/Decode.hpp>
#include <lunar/audio/Mixer.hpp>
#include <lunar/audio/Music.hpp>
#include <lunar/audio/SoundSystem.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace lunar;
using namespace lunar::audio;

namespace {

bool near(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

// A WAV file of 16-bit samples.
std::string wav(std::uint32_t rate, std::uint16_t channels, const std::vector<std::int16_t>& samples)
{
    std::string out;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out += static_cast<char>(v >> (8 * i));
    };
    auto u16 = [&](std::uint16_t v) {
        out += static_cast<char>(v);
        out += static_cast<char>(v >> 8);
    };
    const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
    out += "RIFF";
    u32(36 + bytes);
    out += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(rate * channels * 2);
    u16(static_cast<std::uint16_t>(channels * 2));
    u16(16);
    out += "data";
    u32(bytes);
    for (std::int16_t s : samples)
        u16(static_cast<std::uint16_t>(s));
    return out;
}

// A mono clip at the mixer's rate, every sample the same.
std::shared_ptr<const Clip> constant(float value, std::size_t frames)
{
    auto clip = std::make_shared<Clip>();
    clip->sampleRate = Mixer::rate;
    clip->channels = 1;
    clip->samples.assign(frames, static_cast<std::int16_t>(value * 32767.0f));
    return clip;
}

std::filesystem::path contentSound(std::string_view name)
{
    return assets::AssetConfig::fromEnvironment().contentDir / "sounds" / name;
}

} // namespace

TEST(audio_decodes_wav)
{
    auto clip = decodeAudio(wav(22050, 2, {100, -100, 200, -200, 300, -300}));
    CHECK(clip.has_value());
    CHECK_EQ(clip->sampleRate, 22050u);
    CHECK_EQ(clip->channels, 2u);
    CHECK_EQ(clip->frames(), 3u);
    CHECK_EQ(clip->samples[3], -200);
    CHECK(!decodeAudio("not a sound at all").has_value());
}

TEST(audio_decodes_ogg_and_mp3)
{
    for (const char* name : {"oof.ogg", "action_jump.mp3"}) {
        const auto path = contentSound(name);
        if (!std::filesystem::exists(path))
            SKIP("no Roblox content folder with sounds");
        auto bytes = readFile(path);
        CHECK(bytes.has_value());
        auto clip = decodeAudio(*bytes);
        CHECK(clip.has_value());
        CHECK(clip->sampleRate >= 22050);
        CHECK(clip->seconds() > 0.2 && clip->seconds() < 1.0);
    }
}

TEST(audio_mixer_plays_ends_and_loops)
{
    Mixer mixer(false);
    const Mixer::Voice voice = mixer.add();
    mixer.set(voice, {constant(0.5f, 960), true, false, 1.0f, 1.0f, 0.5f});
    std::vector<float> block(960 * 2, 0.0f);
    mixer.mix(block);
    // Fading in over the block from silence, then at its gains.
    CHECK(near(block[0], 0.0f, 0.01f));
    CHECK(near(block[2 * 959], 0.5f, 0.01f));
    CHECK(near(block[2 * 959 + 1], 0.25f, 0.01f));
    // The clip is used up: the next block ends it.
    std::ranges::fill(block, 0.0f);
    mixer.mix(block);
    Mixer::State state = mixer.state(voice);
    CHECK_EQ(state.ended, 1u);
    CHECK(!state.playing);

    // Looped, at double speed: through it twice a block.
    mixer.set(voice, {constant(0.5f, 480), true, true, 2.0f, 1.0f, 1.0f});
    mixer.seek(voice, 0.0);
    mixer.mix(block);
    state = mixer.state(voice);
    CHECK(state.playing);
    CHECK(state.loops >= 3u);
    // Seeking moves it; a removed voice is gone.
    mixer.seek(voice, 0.005);
    CHECK(near(static_cast<float>(mixer.state(voice).seconds), 0.005f));
    mixer.remove(voice);
    CHECK_EQ(mixer.voices(), 0u);
}

TEST(audio_roll_off_curves)
{
    using Mode = Enum::RollOffMode;
    CHECK(near(SoundSystem::rollOff(Mode::Inverse, 5.0f, 10.0f, 100.0f), 1.0f));
    CHECK(near(SoundSystem::rollOff(Mode::Inverse, 20.0f, 10.0f, 100.0f), 0.5f));
    CHECK(near(SoundSystem::rollOff(Mode::Inverse, 200.0f, 10.0f, 100.0f), 0.0f));
    CHECK(near(SoundSystem::rollOff(Mode::Linear, 55.0f, 10.0f, 100.0f), 0.5f));
    CHECK(near(SoundSystem::rollOff(Mode::LinearSquare, 55.0f, 10.0f, 100.0f), 0.25f));
    CHECK(SoundSystem::rollOff(Mode::InverseTapered, 90.0f, 10.0f, 100.0f) < 0.02f);
}

TEST(audio_sound_system_plays_a_sound_to_its_end)
{
    if (!std::filesystem::exists(contentSound("oof.ogg")))
        SKIP("no Roblox content folder with sounds");
    auto game = DataModel::create();
    auto part = std::make_shared<Part>();
    part->setParent(game->workspace());
    auto sound = std::make_shared<Sound>();
    sound->setSoundId("rbxasset://sounds/oof.ogg");
    sound->setParent(part.get());
    std::vector<std::string> heard;
    sound->eventFired.connect([&](std::string_view event, std::span<const Value>) { heard.emplace_back(event); });
    SoundSystem sounds(*game, std::make_shared<assets::AssetCache>(), false);

    // Loads in the background.
    const CFrame listener(0, 0, 5);
    for (int i = 0; i < 200 && !sound->isLoaded(); ++i) {
        sounds.update(listener);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(sound->isLoaded());
    CHECK(sound->timeLength() > 0.2 && sound->timeLength() < 1.0);
    CHECK(std::ranges::contains(heard, std::string("Loaded")));

    // Playing: heard (5 studs away, inside its roll-off), time moving on, then Ended.
    sound->setPlaying(true);
    sounds.update(listener);
    std::vector<float> block(960, 0.0f);
    float loudest = 0.0f;
    for (int i = 0; i < 100 && sound->playing(); ++i) {
        std::ranges::fill(block, 0.0f);
        sounds.mixer().mix(block);
        for (float s : block)
            loudest = std::max(loudest, std::abs(s));
        sounds.update(listener);
    }
    CHECK(loudest > 0.05f);
    CHECK(!sound->playing());
    CHECK(std::ranges::contains(heard, std::string("Ended")));
    CHECK_EQ(sounds.stats().clips, 1u);
}

namespace {

// A stereo sine at the mixer's rate.
std::vector<float> sine(double hz, std::size_t frames, float amplitude = 0.5f)
{
    std::vector<float> out(frames * 2);
    for (std::size_t i = 0; i < frames; ++i)
        out[2 * i] = out[2 * i + 1]
            = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / Mixer::rate));
    return out;
}

float peakFrom(const std::vector<float>& s, std::size_t fromFrame)
{
    float peak = 0.0f;
    for (std::size_t i = fromFrame * 2; i < s.size(); ++i)
        peak = std::max(peak, std::abs(s[i]));
    return peak;
}

std::vector<float> through(const Effect& effect, std::vector<float> s)
{
    EffectChain chain;
    const std::array effects{effect};
    // In blocks, as the mixer runs it.
    for (std::size_t at = 0; at < s.size(); at += 960)
        chain.process(effects, std::span(s).subspan(at, std::min<std::size_t>(960, s.size() - at)));
    return s;
}

} // namespace

TEST(audio_equalizer_cuts_its_bands)
{
    const Effect flat{EffectKind::Equalizer, {0.0f, 0.0f, 0.0f}};
    CHECK(near(peakFrom(through(flat, sine(1000, 9600)), 4800), 0.5f, 0.02f));
    // The muffle: highs gone, lows kept.
    const Effect muffle{EffectKind::Equalizer, {0.0f, -30.0f, -80.0f}};
    CHECK(peakFrom(through(muffle, sine(12000, 9600)), 4800) < 0.03f);
    CHECK(peakFrom(through(muffle, sine(60, 9600)), 4800) > 0.4f);
}

TEST(audio_time_effects)
{
    // An impulse: Echo repeats it a Delay later, Reverb leaves a tail.
    std::vector<float> impulse(48000 * 2, 0.0f);
    impulse[0] = impulse[1] = 1.0f;
    const auto echoed = through({EffectKind::Echo, {0.25f, 0.0f, 0.0f, 0.0f}}, impulse);
    CHECK(near(echoed[2 * 12000], 1.0f, 0.05f));
    const auto reverb = through({EffectKind::Reverb, {1.5f, 1.0f, 1.0f, -80.0f, 0.0f}}, impulse);
    CHECK(peakFrom(reverb, 4800) > 0.001f);

    // Tremolo at full depth: silent for the rest of each cycle.
    const auto gated = through({EffectKind::Tremolo, {1.0f, 0.5f, 10.0f}}, sine(1000, 9600));
    float quiet = 0.0f;
    for (std::size_t i = 2 * 3600; i < 2 * 4700; ++i) // the second half of the cycle from 0.075 s
        quiet = std::max(quiet, std::abs(gated[i]));
    CHECK(quiet < 0.02f);

    // An octave up: twice the zero crossings.
    const auto up = through({EffectKind::PitchShift, {2.0f}}, sine(500, 48000));
    int crossings = 0;
    for (std::size_t i = 2 * 4800; i + 2 < up.size(); i += 2)
        crossings += (up[i] < 0.0f) != (up[i + 2] < 0.0f) ? 1 : 0;
    const int expected = 2 * 1000 * 43200 / 48000;
    CHECK(crossings > expected * 8 / 10 && crossings < expected * 13 / 10);
}

TEST(audio_sound_group_effects_reach_their_sounds)
{
    if (!std::filesystem::exists(contentSound("oof.ogg")))
        SKIP("no Roblox content folder with sounds");
    auto game = DataModel::create();
    auto* service = game->getService("SoundService");
    auto group = std::make_shared<SoundGroup>();
    group->setParent(service);
    auto sound = std::make_shared<Sound>();
    sound->setSoundId("rbxasset://sounds/oof.ogg");
    sound->setSoundGroup(group.get());
    sound->setParent(service);
    SoundSystem sounds(*game, std::make_shared<assets::AssetCache>(), false);
    for (int i = 0; i < 200 && !sound->isLoaded(); ++i) {
        sounds.update(CFrame());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto loudness = [&] {
        sound->setTimePosition(0.0);
        sound->setPlaying(true);
        float loudest = 0.0f;
        for (int i = 0; i < 20; ++i) {
            sounds.update(CFrame());
            std::vector<float> block(960, 0.0f);
            sounds.mixer().mix(block);
            for (float s : block)
                loudest = std::max(loudest, std::abs(s));
        }
        sound->setPlaying(false);
        sounds.update(CFrame());
        return loudest;
    };
    CHECK(loudness() > 0.05f);
    // Everything cut on the group: silence. Disabled again: heard.
    auto eq = std::make_shared<EqualizerSoundEffect>();
    eq->setLowGain(-80.0f);
    eq->setMidGain(-80.0f);
    eq->setHighGain(-80.0f);
    eq->setParent(group.get());
    CHECK(loudness() < 0.001f);
    eq->setEnabled(false);
    CHECK(loudness() > 0.05f);
}

// Played to its end, a voice the game still asks to play (it hasn't heard
// yet) stays quiet rather than starting over for a frame; stopped or
// seeked, it plays again.
TEST(audio_mixer_a_voice_that_ended_stays_ended)
{
    Mixer mixer(false);
    const Mixer::Voice voice = mixer.add();
    Mixer::Params params{constant(0.5f, 480), true, false, 1.0f, 1.0f, 1.0f};
    mixer.set(voice, params);
    std::vector<float> block(std::size_t{960} * 2, 0.0f);
    mixer.mix(block);
    std::ranges::fill(block, 0.0f);
    mixer.mix(block);
    CHECK_EQ(mixer.state(voice).ended, 1u);
    for (int i = 0; i < 3; ++i) {
        params.left = params.right = 0.9f - 0.1f * static_cast<float>(i);
        mixer.set(voice, params);
        std::ranges::fill(block, 0.0f);
        mixer.mix(block);
        CHECK(peakFrom(block, 0) == 0.0f);
    }
    mixer.seek(voice, 0.0);
    std::ranges::fill(block, 0.0f);
    mixer.mix(block);
    CHECK(peakFrom(block, 0) > 0.3f);
    std::ranges::fill(block, 0.0f);
    mixer.mix(block);
    CHECK_EQ(mixer.state(voice).ended, 2u);
}

// Starting, jumping, stopping and being removed fade: the waveform never
// steps further than a fade's slope, where cutting would step by the whole
// signal (a click).
TEST(audio_mixer_fades_instead_of_clicking)
{
    // Half a second at +0.5, then half a second at -0.5.
    auto clip = std::make_shared<Clip>();
    clip->sampleRate = Mixer::rate;
    clip->channels = 1;
    clip->samples.assign(Mixer::rate, static_cast<std::int16_t>(16384));
    std::fill(clip->samples.begin() + Mixer::rate / 2, clip->samples.end(), static_cast<std::int16_t>(-16384));
    Mixer mixer(false);
    const Mixer::Voice voice = mixer.add();
    std::vector<float> heard;
    auto run = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) {
            std::vector<float> block(std::size_t{960} * 2, 0.0f);
            mixer.mix(block);
            heard.insert(heard.end(), block.begin(), block.end());
        }
    };
    mixer.set(voice, {clip, true, true, 1.0f, 1.0f, 1.0f});
    run(3);
    mixer.seek(voice, 0.75); // into the other half
    run(2);
    mixer.set(voice, {clip, false, true, 1.0f, 1.0f, 1.0f});
    run(2);
    mixer.set(voice, {clip, true, true, 1.0f, 1.0f, 1.0f});
    mixer.seek(voice, 0.1);
    run(2);
    mixer.remove(voice);
    run(2);
    float steepest = 0.0f;
    for (std::size_t i = 2; i < heard.size(); i += 2)
        steepest = std::max(steepest, std::abs(heard[i] - heard[i - 2]));
    CHECK(steepest < 0.6f / Mixer::fadeFrames);
    // Heard on both sides of the jump, and nothing once it's gone.
    CHECK(near(heard[std::size_t{2} * 2879], 0.5f, 0.01f));
    CHECK(near(heard[std::size_t{2} * 4700], -0.5f, 0.01f));
    CHECK(peakFrom(heard, heard.size() / 2 - 960) == 0.0f);
    CHECK_EQ(mixer.voices(), 0u);
}

// A voice too far away to hear isn't mixed, but its time goes on: it ends
// when it would have.
TEST(audio_mixer_unheard_voices_keep_time)
{
    Mixer mixer(false);
    const Mixer::Voice voice = mixer.add();
    mixer.set(voice, {constant(0.5f, 4800), true, false, 1.0f, 0.0f, 0.0f});
    std::vector<float> block(std::size_t{960} * 2, 0.0f);
    for (int i = 0; i < 4; ++i)
        mixer.mix(block);
    CHECK(peakFrom(block, 0) == 0.0f);
    CHECK(near(static_cast<float>(mixer.state(voice).seconds), 0.08f));
    CHECK(mixer.state(voice).playing);
    mixer.mix(block);
    mixer.mix(block);
    CHECK_EQ(mixer.state(voice).ended, 1u);
}

// Sounds play from ReplicatedStorage (the game keeps its music there), not from
// the Starter containers' templates.
TEST(audio_sound_system_plays_stored_sounds_but_not_templates)
{
    if (!std::filesystem::exists(contentSound("oof.ogg")))
        SKIP("no Roblox content folder with sounds");
    auto game = DataModel::create();
    auto make = [&](std::string_view service) {
        auto sound = std::make_shared<Sound>();
        sound->setSoundId("rbxasset://sounds/oof.ogg");
        sound->setLooped(true);
        sound->setPlaying(true);
        sound->setParent(game->getService(service));
        return sound;
    };
    auto stored = make("ReplicatedStorage");
    auto starter = make("StarterGui");
    SoundSystem sounds(*game, std::make_shared<assets::AssetCache>(), false);
    for (int i = 0; i < 200 && !stored->isLoaded(); ++i) {
        sounds.update(CFrame());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(stored->isLoaded());
    CHECK(!starter->isLoaded());
    sounds.update(CFrame());
    std::vector<float> block(960, 0.0f);
    sounds.mixer().mix(block);
    CHECK(peakFrom(block, 0) > 0.05f);
    CHECK_EQ(sounds.stats().playing, 1u);
}

// The music visualizer's analysis: a tone lights its own band, not the
// others, and a bass hit after a quiet stretch is a beat that fades.
TEST(audio_spectrum_bands_and_beat)
{
    constexpr int rate = 48000;
    lunar::audio::Spectrum spectrum(rate, 64);
    auto tone = [&](float hz, float amplitude, int count, int& at) {
        std::vector<float> out(static_cast<std::size_t>(count));
        for (float& s : out)
            s = amplitude * std::sin(2.0f * 3.14159265f * hz * static_cast<float>(at++) / rate);
        return out;
    };
    int at = 0;
    spectrum.push(tone(1000.0f, 0.5f, 4096, at));
    spectrum.update(1.0f / 60.0f);
    const auto bands = spectrum.bands();
    const auto loudest = std::ranges::max_element(bands) - bands.begin();
    // 40 Hz to 16 kHz in 64 log steps: 1 kHz is about band 34.
    CHECK(loudest >= 32 && loudest <= 36);
    CHECK(bands[static_cast<std::size_t>(loudest)] > 0.8f);
    CHECK(bands[2] < 0.3f);
    CHECK(bands[60] < 0.3f);
    // Quiet music dances as well (the automatic gain): a tone at -40 dB.
    lunar::audio::Spectrum quiet(rate, 64);
    at = 0;
    for (int frame = 0; frame < 30; ++frame) {
        quiet.push(tone(1000.0f, 0.01f, 800, at));
        quiet.update(1.0f / 60.0f);
    }
    CHECK(*std::ranges::max_element(quiet.bands()) > 0.8f);

    lunar::audio::Spectrum beats(rate, 64);
    at = 0;
    for (int frame = 0; frame < 120; ++frame) { // two quiet seconds
        beats.push(tone(60.0f, 0.001f, 800, at));
        beats.update(1.0f / 60.0f);
    }
    CHECK(beats.beat() < 0.1f);
    beats.push(tone(60.0f, 0.8f, 2048, at)); // the kick
    beats.update(1.0f / 60.0f);
    CHECK(beats.beat() > 0.9f);
    for (int frame = 0; frame < 30; ++frame) {
        beats.push(tone(60.0f, 0.001f, 800, at));
        beats.update(1.0f / 60.0f);
    }
    CHECK(beats.beat() < 0.1f);
}

namespace {

// Sounds downloaded from a stand-in for Roblox: every asset is `body`,
// answered after `latency` with the status `status` gives its id.
std::shared_ptr<assets::AssetCache> fakeRoblox(std::function<long(std::uint64_t)> status,
                                               std::chrono::milliseconds latency, std::string body)
{
    assets::AssetConfig config;
    config.download = true;
    config.web = std::make_shared<const assets::RobloxWeb>(
        [status = std::move(status), latency, body = std::move(body)](
            const assets::HttpRequest& request) -> std::expected<assets::HttpResponse, std::string> {
            std::this_thread::sleep_for(latency);
            const std::uint64_t id = std::stoull(request.url.substr(request.url.rfind('=') + 1));
            const long code = status(id);
            return assets::HttpResponse{.status = code, .body = code == 200 ? body : std::string("{}")};
        });
    return std::make_shared<assets::AssetCache>(std::move(config));
}

// A mono WAV at the mixer's rate, `seconds` long, every sample `value`.
std::string steadyWav(double seconds, std::int16_t value = 16384)
{
    return wav(Mixer::rate, 1, std::vector<std::int16_t>(static_cast<std::size_t>(seconds * Mixer::rate), value));
}

std::shared_ptr<Sound> soundIn(Instance& parent, std::uint64_t id, bool playing = false)
{
    auto sound = std::make_shared<Sound>();
    sound->setSoundId(std::format("rbxassetid://{}", id));
    sound->setPlaying(playing);
    sound->setParent(&parent);
    return sound;
}

} // namespace

// A sound the game plays loads ahead of those only being got ready, and a
// download Roblox rate-limits waits for its retry without holding the others
// up [live: the game, one 429 kept every sound from loading for 20 s, while the
// server destroyed the sounds it played on time: cut short or never heard].
TEST(audio_a_sound_played_now_loads_first_and_a_rate_limit_holds_nothing_up)
{
    auto cache = fakeRoblox([](std::uint64_t id) { return id == 1 ? 429L : 200L; }, std::chrono::milliseconds(20),
                            steadyWav(0.1));
    auto game = DataModel::create();
    Instance& storage = *game->getService("ReplicatedStorage");
    auto limited = soundIn(storage, 1);
    std::vector<std::shared_ptr<Sound>> templates;
    for (std::uint64_t id = 2; id < 42; ++id)
        templates.push_back(soundIn(storage, id));
    SoundSystem sounds(*game, cache, false);
    sounds.update(CFrame()); // all asked for, the rate-limited one first
    auto played = soundIn(storage, 99, true);
    const auto start = std::chrono::steady_clock::now();
    while (!played->isLoaded() && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        sounds.update(CFrame());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(played->isLoaded());
    // Long before the 40 templates one after another (800 ms), let alone
    // asset 1's retries (1, 2, 4... s).
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(400));
    CHECK(!limited->isLoaded());
    // The rest load meanwhile, side by side.
    while (!std::ranges::all_of(templates, [](const auto& s) { return s->isLoaded(); })
           && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        sounds.update(CFrame());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(std::ranges::all_of(templates, [](const auto& s) { return s->isLoaded(); }));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(800));
    CHECK_EQ(sounds.stats().failed, 0u); // asset 1 isn't given up on
}

// Moved within the game (the game throws a character's trash can into
// Workspace.Thrown, and puts far trees in ReplicatedStorage), a sound plays
// on with its voice: no restart, no second Loaded, no PlayOnRemove. Taken out
// of the game, it stops and PlayOnRemove plays it out.
TEST(audio_a_sound_moved_within_the_game_plays_on)
{
    auto cache = fakeRoblox([](std::uint64_t) { return 200L; }, std::chrono::milliseconds(0), steadyWav(1.0));
    auto game = DataModel::create();
    auto from = std::make_shared<Part>();
    from->setParent(game->workspace());
    auto to = std::make_shared<Part>();
    to->setParent(game->workspace());
    auto sound = soundIn(*from, 7);
    sound->setPlayOnRemove(true);
    int loadedEvents = 0;
    sound->eventFired.connect([&](std::string_view event, std::span<const Value>) { loadedEvents += event == "Loaded"; });
    SoundSystem sounds(*game, cache, false);
    for (int i = 0; i < 500 && !sound->isLoaded(); ++i) {
        sounds.update(CFrame());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(sound->isLoaded());
    sound->setPlaying(true);
    std::vector<float> block(std::size_t{960} * 2, 0.0f);
    auto run = [&](int blocks) {
        float quietest = 1.0f;
        for (int i = 0; i < blocks; ++i) {
            sounds.update(CFrame());
            std::ranges::fill(block, 0.0f);
            sounds.mixer().mix(block);
            for (float s : block)
                quietest = std::min(quietest, std::abs(s));
        }
        return quietest;
    };
    run(10);
    const double before = sound->timePosition();
    CHECK(before > 0.05);
    sound->setParent(to.get());
    CHECK(run(5) > 0.1f); // heard throughout
    CHECK(sound->playing());
    CHECK(sound->timePosition() > before);
    CHECK_EQ(loadedEvents, 1);
    CHECK_EQ(sounds.mixer().voices(), 1u); // its own; no PlayOnRemove

    sound->setParent(nullptr);
    sounds.update(CFrame());
    CHECK_EQ(sounds.stats().sounds, 0u);
    CHECK_EQ(sounds.mixer().voices(), 1u); // PlayOnRemove's
    std::ranges::fill(block, 0.0f);
    sounds.mixer().mix(block);
    CHECK(peakFrom(block, 480) > 0.1f);
}

// Play() again just as the mixer ended the sound, before the sound system
// heard it ended: it plays again, as Play() does, rather than the old end
// stopping the new play.
TEST(audio_play_again_as_it_ends_plays_again)
{
    auto cache = fakeRoblox([](std::uint64_t) { return 200L; }, std::chrono::milliseconds(0), steadyWav(0.2));
    auto game = DataModel::create();
    auto sound = soundIn(*game->getService("ReplicatedStorage"), 8);
    int ended = 0;
    sound->eventFired.connect([&](std::string_view event, std::span<const Value>) { ended += event == "Ended"; });
    SoundSystem sounds(*game, cache, false);
    for (int i = 0; i < 500 && !sound->isLoaded(); ++i) {
        sounds.update(CFrame());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(sound->isLoaded());
    sound->setPlaying(true);
    std::vector<float> block(std::size_t{480} * 2, 0.0f); // 10 ms
    auto mix = [&] {
        std::ranges::fill(block, 0.0f);
        sounds.mixer().mix(block);
        return peakFrom(block, 0);
    };
    for (int i = 0; i < 15; ++i) { // 0.15 s in
        sounds.update(CFrame());
        mix();
    }
    for (int i = 0; i < 8; ++i) // the mixer plays it to its end; the game hasn't looked yet
        mix();
    sound->setTimePosition(0.0); // Sound:Play()
    sound->setPlaying(true);
    sounds.update(CFrame());
    CHECK(sound->playing());
    CHECK_EQ(ended, 0);
    CHECK(mix() > 0.1f);
    for (int i = 0; i < 40 && sound->playing(); ++i) {
        sounds.update(CFrame());
        mix();
    }
    CHECK(!sound->playing());
    CHECK_EQ(ended, 1);
}
