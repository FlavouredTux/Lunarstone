#include <lunar/audio/Effects.hpp>

#include <lunar/audio/Mixer.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lunar::audio {

namespace {

constexpr double fs = Mixer::rate;
constexpr double twoPi = 2.0 * std::numbers::pi;

double fromDb(float db)
{
    return db <= -80.0f ? 0.0 : std::pow(10.0, static_cast<double>(db) / 20.0);
}

// A second-order section (RBJ's cookbook), one memory per channel.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2]{}, z2[2]{};

    void lowpass(double f, double q = std::numbers::sqrt2 / 2) { design(f, q, false); }
    void highpass(double f, double q = std::numbers::sqrt2 / 2) { design(f, q, true); }
    void design(double f, double q, bool high)
    {
        const double w = twoPi * std::min(f, fs * 0.45) / fs;
        const double c = std::cos(w), alpha = std::sin(w) / (2 * q);
        const double a0 = 1 + alpha;
        b0 = (high ? (1 + c) : (1 - c)) / 2 / a0;
        b1 = (high ? -(1 + c) : (1 - c)) / a0;
        b2 = b0;
        a1 = -2 * c / a0;
        a2 = (1 - alpha) / a0;
    }
    double run(double x, int ch)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
};

// A delay line per channel, read between samples.
struct Delay {
    std::vector<float> line[2];
    std::size_t at = 0;
    explicit Delay(std::size_t length = 1)
    {
        line[0].assign(length, 0.0f);
        line[1].assign(length, 0.0f);
    }
    std::size_t size() const { return line[0].size(); }
    float read(int ch, double back) const
    {
        const std::size_t n = size();
        back = std::clamp(back, 1.0, static_cast<double>(n - 1));
        double pos = static_cast<double>(at) - back;
        if (pos < 0)
            pos += static_cast<double>(n);
        const auto i = static_cast<std::size_t>(pos);
        const auto j = (i + 1) % n;
        const auto t = static_cast<float>(pos - static_cast<double>(i));
        return line[ch][i] + (line[ch][j] - line[ch][i]) * t;
    }
    void write(float l, float r)
    {
        line[0][at] = l;
        line[1][at] = r;
        at = (at + 1) % size();
    }
};

} // namespace

struct EffectChain::Stage {
    virtual ~Stage() = default;
    virtual void run(const Effect& e, std::span<float> stereo) = 0;
};

namespace {

// Three bands split at 400 Hz and 4 kHz, each at its gain. GUESS: the
// client's crossover frequencies.
struct Equalizer final : EffectChain::Stage {
    Biquad low, high;
    Equalizer()
    {
        low.lowpass(400.0);
        high.highpass(4000.0);
    }
    void run(const Effect& e, std::span<float> s) override
    {
        const double gl = fromDb(e.p[0]), gm = fromDb(e.p[1]), gh = fromDb(e.p[2]);
        for (std::size_t i = 0; i + 1 < s.size(); i += 2)
            for (int ch = 0; ch < 2; ++ch) {
                const double x = s[i + static_cast<std::size_t>(ch)];
                const double l = low.run(x, ch), h = high.run(x, ch);
                s[i + static_cast<std::size_t>(ch)] = static_cast<float>(gl * l + gm * (x - l - h) + gh * h);
            }
    }
};

// Soft clipping, harder with Level. GUESS: the drive and the make-up.
struct Distortion final : EffectChain::Stage {
    void run(const Effect& e, std::span<float> s) override
    {
        const float drive = 1.0f + 20.0f * std::clamp(e.p[0], 0.0f, 1.0f);
        const float makeup = 1.0f / std::sqrt(drive);
        for (float& x : s)
            x = std::tanh(drive * x) * makeup;
    }
};

// On for Duty of each cycle, down by Depth for the rest, edges smoothed.
struct Tremolo final : EffectChain::Stage {
    double phase = 0.0;
    float gain = 1.0f;
    void run(const Effect& e, std::span<float> s) override
    {
        const double step = static_cast<double>(std::max(e.p[2], 0.0f)) / fs;
        const float smooth = 1.0f - std::exp(-1.0f / (0.002f * static_cast<float>(fs)));
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
            const float target = phase < static_cast<double>(e.p[1]) ? 1.0f : 1.0f - std::clamp(e.p[0], 0.0f, 1.0f);
            gain += (target - gain) * smooth;
            s[i] *= gain;
            s[i + 1] *= gain;
            phase = std::fmod(phase + step, 1.0);
        }
    }
};

// A short delay swept by a sine, mixed back in: flange (1 to 5 ms) or
// chorus (15 to 35 ms). GUESS: the delay ranges.
struct Sweep final : EffectChain::Stage {
    Delay delay{static_cast<std::size_t>(0.05 * fs)};
    double phase = 0.0;
    double base, sweep;
    Sweep(double baseSeconds, double sweepSeconds) : base(baseSeconds * fs), sweep(sweepSeconds * fs) {}
    void run(const Effect& e, std::span<float> s) override
    {
        const double depth = std::clamp(e.p[0], 0.0f, 1.0f);
        const float mix = std::clamp(e.p[1], 0.0f, 1.0f);
        const double step = static_cast<double>(std::max(e.p[2], 0.0f)) / fs;
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
            // The right channel's sweep a quarter turn behind: wider.
            const double l = base + sweep * depth * (0.5 + 0.5 * std::sin(twoPi * phase));
            const double r = base + sweep * depth * (0.5 + 0.5 * std::sin(twoPi * (phase + 0.25)));
            const float wl = delay.read(0, l), wr = delay.read(1, r);
            delay.write(s[i], s[i + 1]);
            s[i] = s[i] * (1.0f - mix * 0.5f) + wl * mix * 0.5f;
            s[i + 1] = s[i + 1] * (1.0f - mix * 0.5f) + wr * mix * 0.5f;
            phase = std::fmod(phase + step, 1.0);
        }
    }
};

struct Echo final : EffectChain::Stage {
    Delay delay{static_cast<std::size_t>(5.0 * fs) + 2};
    void run(const Effect& e, std::span<float> s) override
    {
        const double back = std::clamp(static_cast<double>(e.p[0]), 0.01, 5.0) * fs;
        const auto dry = static_cast<float>(fromDb(e.p[1]));
        const float feedback = std::clamp(e.p[2], 0.0f, 0.99f);
        const auto wet = static_cast<float>(fromDb(e.p[3]));
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
            const float dl = delay.read(0, back), dr = delay.read(1, back);
            delay.write(s[i] + dl * feedback, s[i + 1] + dr * feedback);
            s[i] = s[i] * dry + dl * wet;
            s[i + 1] = s[i + 1] * dry + dr * wet;
        }
    }
};

// Freeverb's shape: eight damped combs into four all-passes a channel,
// the combs' feedback set for DecayTime. GUESS: how Density and Diffusion map.
struct Reverb final : EffectChain::Stage {
    static constexpr std::array<int, 8> combLengths{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr std::array<int, 4> allpassLengths{556, 441, 341, 225};
    static constexpr int spread = 23;
    struct Comb {
        std::vector<float> line;
        std::size_t at = 0;
        float store = 0.0f;
    };
    std::array<Comb, 16> combs;
    std::array<Comb, 8> allpasses;
    Reverb()
    {
        const double scale = fs / 44100.0;
        for (std::size_t i = 0; i < 16; ++i)
            combs[i].line.assign(static_cast<std::size_t>((combLengths[i % 8] + (i >= 8 ? spread : 0)) * scale), 0.0f);
        for (std::size_t i = 0; i < 8; ++i)
            allpasses[i].line.assign(static_cast<std::size_t>((allpassLengths[i % 4] + (i >= 4 ? spread : 0)) * scale),
                                     0.0f);
    }
    void run(const Effect& e, std::span<float> s) override
    {
        const double decay = std::clamp(static_cast<double>(e.p[0]), 0.1, 20.0);
        const float damp = 0.2f + 0.4f * (1.0f - std::clamp(e.p[1], 0.0f, 1.0f));
        const float diffusion = 0.3f + 0.4f * std::clamp(e.p[2], 0.0f, 1.0f);
        const auto dry = static_cast<float>(fromDb(e.p[3]));
        const auto wet = static_cast<float>(fromDb(e.p[4])) * 0.2f;
        std::array<float, 16> feedback{};
        for (std::size_t i = 0; i < 16; ++i)
            feedback[i] = static_cast<float>(std::pow(10.0, -3.0 * static_cast<double>(combs[i].line.size()) / (decay * fs)));
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
            const float in = (s[i] + s[i + 1]) * 0.5f;
            float out[2]{};
            for (std::size_t c = 0; c < 16; ++c) {
                Comb& comb = combs[c];
                const float y = comb.line[comb.at];
                comb.store = y * (1.0f - damp) + comb.store * damp;
                comb.line[comb.at] = in + comb.store * feedback[c];
                comb.at = (comb.at + 1) % comb.line.size();
                out[c / 8] += y;
            }
            for (std::size_t a = 0; a < 8; ++a) {
                Comb& pass = allpasses[a];
                float& o = out[a / 4];
                const float buffered = pass.line[pass.at];
                pass.line[pass.at] = o + buffered * diffusion;
                pass.at = (pass.at + 1) % pass.line.size();
                o = buffered - o;
            }
            s[i] = s[i] * dry + out[0] * wet;
            s[i + 1] = s[i + 1] * dry + out[1] * wet;
        }
    }
};

struct Compressor final : EffectChain::Stage {
    double envelope = -120.0; // dB
    void run(const Effect& e, std::span<float> s) override
    {
        const double attack = std::exp(-1.0 / (static_cast<double>(std::max(e.p[0], 0.0001f)) * fs));
        const double makeup = fromDb(e.p[1]);
        const double ratio = std::max(e.p[2], 1.0f);
        const double release = std::exp(-1.0 / (static_cast<double>(std::max(e.p[3], 0.0001f)) * fs));
        const double threshold = e.p[4];
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
            const double peak = std::max(std::abs(s[i]), std::abs(s[i + 1]));
            const double level = 20.0 * std::log10(std::max(peak, 1e-6));
            const double k = level > envelope ? attack : release;
            envelope = level + (envelope - level) * k;
            const double over = envelope - threshold;
            const double reduce = over > 0.0 ? over - over / ratio : 0.0;
            const auto gain = static_cast<float>(std::pow(10.0, -reduce / 20.0) * makeup);
            s[i] *= gain;
            s[i + 1] *= gain;
        }
    }
};

// Two read heads sliding through a short window at the pitch's rate,
// crossfaded so each fades out before it wraps.
struct PitchShift final : EffectChain::Stage {
    static constexpr double window = 2048.0;
    Delay delay{4096};
    double offset = 0.0;
    void run(const Effect& e, std::span<float> s) override
    {
        const double pitch = std::clamp(static_cast<double>(e.p[0]), 0.5, 2.0);
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
            delay.write(s[i], s[i + 1]);
            offset = std::fmod(offset + (1.0 - pitch) + window, window);
            const double second = std::fmod(offset + window / 2, window);
            const auto fade = static_cast<float>(std::abs(offset / window * 2.0 - 1.0)); // 1 at the window's ends
            for (int ch = 0; ch < 2; ++ch) {
                const float a = delay.read(ch, offset + 1.0);
                const float b = delay.read(ch, second + 1.0);
                s[i + static_cast<std::size_t>(ch)] = a * (1.0f - fade) + b * fade;
            }
        }
    }
};

std::unique_ptr<EffectChain::Stage> makeStage(EffectKind kind)
{
    switch (kind) {
    case EffectKind::Equalizer:
        return std::make_unique<Equalizer>();
    case EffectKind::Distortion:
        return std::make_unique<Distortion>();
    case EffectKind::Tremolo:
        return std::make_unique<Tremolo>();
    case EffectKind::Flange:
        return std::make_unique<Sweep>(0.001, 0.004);
    case EffectKind::Chorus:
        return std::make_unique<Sweep>(0.015, 0.020);
    case EffectKind::Echo:
        return std::make_unique<Echo>();
    case EffectKind::Reverb:
        return std::make_unique<Reverb>();
    case EffectKind::Compressor:
        return std::make_unique<Compressor>();
    case EffectKind::PitchShift:
        return std::make_unique<PitchShift>();
    }
    return nullptr;
}

} // namespace

EffectChain::EffectChain() = default;
EffectChain::~EffectChain() = default;
EffectChain::EffectChain(EffectChain&&) noexcept = default;
EffectChain& EffectChain::operator=(EffectChain&&) noexcept = default;

void EffectChain::process(std::span<const Effect> effects, std::span<float> stereo)
{
    // A different line-up starts over: no memories carried between unlike stages.
    bool same = effects.size() == kinds_.size();
    for (std::size_t i = 0; same && i < effects.size(); ++i)
        same = effects[i].kind == kinds_[i];
    if (!same) {
        stages_.clear();
        kinds_.clear();
        for (const Effect& e : effects) {
            stages_.push_back(makeStage(e.kind));
            kinds_.push_back(e.kind);
        }
    }
    for (std::size_t i = 0; i < effects.size(); ++i)
        stages_[i]->run(effects[i], stereo);
}

} // namespace lunar::audio
