#include <lunar/audio/Mixer.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

#if LUNAR_WITH_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

namespace lunar::audio {

struct Mixer::Output {
#if LUNAR_WITH_PULSE
    pa_simple* stream = nullptr;
    ~Output()
    {
        if (stream)
            pa_simple_free(stream);
    }
#endif
};

namespace {

// Ten milliseconds a block; the device holds about four of them.
constexpr std::uint32_t blockFrames = Mixer::rate / 100;

} // namespace

Mixer::Mixer(bool output)
{
    if (!output)
        return;
#if LUNAR_WITH_PULSE
    const pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, rate, 2};
    const auto block = static_cast<std::uint32_t>(blockFrames * 2 * sizeof(float));
    const pa_buffer_attr buffering{
        .maxlength = static_cast<std::uint32_t>(-1),
        .tlength = block * 4,
        .prebuf = static_cast<std::uint32_t>(-1),
        .minreq = block,
        .fragsize = static_cast<std::uint32_t>(-1),
    };
    int error = 0;
    pa_simple* stream = pa_simple_new(nullptr, "Lunarstone", PA_STREAM_PLAYBACK, nullptr, "Game", &spec, nullptr,
                                      &buffering, &error);
    if (!stream) {
        outputError_ = std::format("PulseAudio: {}", pa_strerror(error));
        return;
    }
    output_ = std::make_unique<Output>();
    output_->stream = stream;
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
#else
    outputError_ = "this build has no sound output (it was built without libpulse-simple)";
#endif
}

Mixer::~Mixer()
{
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Mixer::run(std::stop_token stop)
{
#if LUNAR_WITH_PULSE
    std::vector<float> block(std::size_t{blockFrames} * 2);
    while (!stop.stop_requested()) {
        std::ranges::fill(block, 0.0f);
        mix(block);
        // Past full scale it bends rather than clips.
        for (float& s : block)
            if (std::abs(s) > 0.8f)
                s = std::copysign(0.8f + 0.2f * std::tanh((std::abs(s) - 0.8f) / 0.2f), s);
        int error = 0;
        if (pa_simple_write(output_->stream, block.data(), block.size() * sizeof(float), &error) < 0)
            break;
    }
#else
    (void)stop;
#endif
}

Mixer::Voice Mixer::add()
{
    const std::scoped_lock lock(mutex_);
    const Voice voice = next_++;
    controls_.emplace(voice, Control{});
    return voice;
}

void Mixer::remove(Voice voice)
{
    const std::scoped_lock lock(mutex_);
    if (controls_.erase(voice))
        removed_.push_back(voice);
}

void Mixer::set(Voice voice, const Params& params)
{
    const std::scoped_lock lock(mutex_);
    const auto it = controls_.find(voice);
    if (it == controls_.end() || it->second.params == params)
        return;
    Control& c = it->second;
    c.params = params;
    if (!std::exchange(c.queued, true))
        changed_.push_back(voice);
}

std::uint64_t Mixer::seek(Voice voice, double seconds)
{
    const std::scoped_lock lock(mutex_);
    const auto it = controls_.find(voice);
    if (it == controls_.end())
        return 0;
    Control& c = it->second;
    c.seek = std::max(seconds, 0.0);
    if (!std::exchange(c.queued, true))
        changed_.push_back(voice);
    return ++c.seeks;
}

Mixer::State Mixer::state(Voice voice) const
{
    const std::scoped_lock lock(mutex_);
    const auto it = controls_.find(voice);
    if (it == controls_.end())
        return {};
    State state = it->second.state;
    if (it->second.seek)
        state.seconds = *it->second.seek;
    return state;
}

std::size_t Mixer::voices() const
{
    const std::scoped_lock lock(mutex_);
    return controls_.size();
}

void Mixer::mix(std::span<float> stereo)
{
    if (stereo.size() < 2)
        return;
    {
        const std::scoped_lock lock(mutex_);
        for (const Voice voice : changed_) {
            const auto it = controls_.find(voice);
            if (it == controls_.end())
                continue;
            Control& c = it->second;
            c.queued = false;
            Entry& e = entries_[voice];
            e.params = c.params;
            if (c.seek) {
                e.seek = std::exchange(c.seek, std::nullopt);
                e.seeks = c.seeks;
            }
        }
        changed_.clear();
        for (const Voice voice : removed_)
            if (const auto it = entries_.find(voice); it != entries_.end())
                it->second.removed = true;
        removed_.clear();
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        play(it->second, stereo);
        if (it->second.removed && !it->second.on)
            it = entries_.erase(it);
        else
            ++it;
    }
    const std::scoped_lock lock(mutex_);
    for (const auto& [voice, e] : entries_) {
        const auto it = controls_.find(voice);
        if (it == controls_.end())
            continue;
        const Clip* clip = e.sounding.get();
        it->second.state
            = {clip && clip->sampleRate ? e.frame / clip->sampleRate : 0.0, e.ended, e.loops, e.on, e.endedAfter};
    }
}

void Mixer::play(Entry& e, std::span<float> stereo)
{
    const std::size_t frames = stereo.size() / 2;
    const Params& p = e.params;
    const std::optional<double> seek = std::exchange(e.seek, std::nullopt);
    // Played to its end, it's asked again by being stopped or moved.
    if (seek || !p.playing)
        e.finished = false;
    const Clip* asked = p.clip.get();
    const bool wanted
        = p.playing && !e.finished && !e.removed && asked && asked->frames() >= 2 && asked->sampleRate > 0;
    // Moved to: frames into the clip it's going to play.
    auto jump = [&] {
        e.sounding = p.clip;
        e.frame = asked && asked->sampleRate ? *seek * asked->sampleRate : 0.0;
    };
    if (!e.on) {
        // Silent, the gains just follow: a start fades in by itself.
        e.left = p.left;
        e.right = p.right;
        if (!wanted) {
            if (seek)
                jump();
            return;
        }
    }
    // Where it is fades out: stopping, moving, or another clip.
    const bool cut = e.on && (!wanted || seek || p.clip != e.sounding);
    const bool silent = p.left == 0.0f && p.right == 0.0f && e.left == 0.0f && e.right == 0.0f;
    if (!silent)
        scratch_.assign(frames * 2, 0.0f);

    // `count` frames from `at` of the clip it's playing, each times its
    // fade: the start's ramp up while that lasts, or down to nothing when
    // `out`. Played to its end, it stops there (unless it's on its way out anyway).
    auto render = [&](std::size_t at, std::size_t count, bool out) {
        const Clip& clip = *e.sounding;
        const double step = static_cast<double>(clip.sampleRate) / rate * static_cast<double>(std::max(p.speed, 0.0f));
        const auto length = static_cast<double>(clip.frames());
        auto end = [&] {
            e.on = false;
            if (out)
                return;
            e.finished = true;
            e.frame = 0.0;
            ++e.ended;
            e.endedAfter = e.seeks;
        };
        if (silent) {
            // Unheard: only time passes.
            e.frame += step * static_cast<double>(count);
            e.fadeIn = std::min<std::uint32_t>(fadeFrames, e.fadeIn + static_cast<std::uint32_t>(count));
            if (e.frame >= length) {
                if (!p.looped)
                    end();
                else if (!out) {
                    e.loops += static_cast<std::uint64_t>(e.frame / length);
                    e.frame = std::fmod(e.frame, length);
                }
            }
            return;
        }
        const float from = static_cast<float>(e.fadeIn) / fadeFrames;
        const std::uint32_t channels = clip.channels;
        const std::int16_t* samples = clip.samples.data();
        constexpr float scale = 1.0f / 32768.0f;
        for (std::size_t i = 0; i < count; ++i) {
            if (e.frame >= length) {
                if (!p.looped) {
                    end();
                    return;
                }
                e.frame = std::fmod(e.frame, length);
                e.loops += out ? 0 : 1;
            }
            float fade = 1.0f;
            if (out)
                fade = from * (1.0f - static_cast<float>(i + 1) / static_cast<float>(count));
            else if (e.fadeIn < fadeFrames)
                fade = static_cast<float>(++e.fadeIn) / fadeFrames;
            const auto a = static_cast<std::size_t>(e.frame);
            const auto b = a + 1 < clip.frames() ? a + 1 : (p.looped ? 0 : a);
            const auto t = static_cast<float>(e.frame - static_cast<double>(a));
            float* to = &scratch_[2 * (at + i)];
            if (channels == 1) {
                to[0] = to[1] = (samples[a] + (samples[b] - samples[a]) * t) * scale * fade;
            } else {
                to[0] = (samples[2 * a] + (samples[2 * b] - samples[2 * a]) * t) * scale * fade;
                to[1] = (samples[2 * a + 1] + (samples[2 * b + 1] - samples[2 * a + 1]) * t) * scale * fade;
            }
            e.frame += step;
        }
    };

    std::size_t at = 0;
    if (cut) {
        at = std::min<std::size_t>(fadeFrames, frames);
        render(0, at, true);
        e.on = false;
    }
    if (seek)
        jump();
    if (!e.on && wanted) {
        e.on = true;
        e.sounding = p.clip;
        e.fadeIn = 0;
    }
    if (e.on)
        render(at, frames - at, false);

    if (silent)
        return;
    e.chain.process(p.effects, scratch_);
    // Gains ramp from where they were to where they're asked to be.
    const float dl = (p.left - e.left) / static_cast<float>(frames);
    const float dr = (p.right - e.right) / static_cast<float>(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        e.left += dl;
        e.right += dr;
        stereo[2 * i] += scratch_[2 * i] * e.left;
        stereo[2 * i + 1] += scratch_[2 * i + 1] * e.right;
    }
    e.left = p.left;
    e.right = p.right;
}

} // namespace lunar::audio
