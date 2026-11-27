#include <lunar/audio/Music.hpp>

#if LUNAR_WITH_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <numbers>
#include <utility>

namespace lunar::audio {

namespace {

constexpr float lowest = 40.0f, highest = 16000.0f;
// Automatic gain: a band is full at the loudest band lately (`reference`,
// which sinks this many dB a second once the music gets quieter, never
// under quietestDb so silence stays dark) and silent rangeDb under it.
constexpr float rangeDb = 45.0f;
constexpr float sinkDbPerSecond = 6.0f;
constexpr float quietestDb = -60.0f;
// How fast a band falls back, of its height a second.
constexpr float release = 2.5f;
// A beat: the bass this much over its average of the last second or so,
// at most every eighth of a second.
constexpr float beatRatio = 1.35f;
constexpr float beatGap = 0.125f;

void fft(std::vector<std::complex<float>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

} // namespace

Spectrum::Spectrum(int sampleRate, std::size_t bandCount)
    : sampleRate_(sampleRate), history_(fftSize, 0.0f), window_(fftSize), bands_(bandCount, 0.0f)
{
    for (std::size_t i = 0; i < fftSize; ++i)
        window_[i] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i)
                                            / static_cast<float>(fftSize - 1));
    // Log-spaced edges, each band at least one bin wide.
    const float binHz = static_cast<float>(sampleRate) / static_cast<float>(fftSize);
    std::size_t last = 0;
    for (std::size_t b = 0; b <= bandCount; ++b) {
        const float hz = lowest * std::pow(highest / lowest, static_cast<float>(b) / static_cast<float>(bandCount));
        std::size_t bin = std::max<std::size_t>(static_cast<std::size_t>(hz / binHz), 1);
        if (b > 0)
            bin = std::max(bin, last + 1);
        bin = std::min(bin, fftSize / 2);
        edges_.push_back(bin);
        last = bin;
    }
}

void Spectrum::push(std::span<const float> samples)
{
    for (float s : samples.last(std::min(samples.size(), fftSize))) {
        history_[next_] = s;
        next_ = (next_ + 1) % fftSize;
    }
}

void Spectrum::update(float dt)
{
    std::vector<std::complex<float>> a(fftSize);
    for (std::size_t i = 0; i < fftSize; ++i)
        a[i] = history_[(next_ + i) % fftSize] * window_[i];
    fft(a);
    // A full-scale sine through the Hann window peaks at fftSize / 4.
    const float full = static_cast<float>(fftSize) / 4.0f;
    std::vector<float> db(bands_.size());
    float loudest = quietestDb;
    for (std::size_t b = 0; b < bands_.size(); ++b) {
        float peak = 0.0f;
        for (std::size_t k = edges_[b]; k < edges_[b + 1] && k < fftSize / 2; ++k)
            peak = std::max(peak, std::abs(a[k]));
        db[b] = 20.0f * std::log10(std::max(peak / full, 1e-9f));
        loudest = std::max(loudest, db[b]);
    }
    referenceDb_ = std::max(loudest, referenceDb_ - sinkDbPerSecond * dt);
    float sum = 0.0f;
    for (std::size_t b = 0; b < bands_.size(); ++b) {
        const float target = std::clamp((db[b] - (referenceDb_ - rangeDb)) / rangeDb, 0.0f, 1.0f);
        bands_[b] = target > bands_[b] ? target : std::max(target, bands_[b] - release * bands_[b] * dt);
        sum += bands_[b];
    }
    level_ = bands_.empty() ? 0.0f : sum / static_cast<float>(bands_.size());

    // The beat, from the bass (up to 150 Hz).
    const float binHz = static_cast<float>(sampleRate_) / static_cast<float>(fftSize);
    float bass = 0.0f;
    for (std::size_t k = 1; static_cast<float>(k) * binHz < 150.0f; ++k)
        bass += std::norm(a[k]);
    bass = std::sqrt(bass) / full;
    sinceBeat_ += dt;
    // Relative to how loud it's been, so quiet music beats too; never on noise.
    if (bass > 0.002f && bass > bassAverage_ * beatRatio && sinceBeat_ >= beatGap) {
        beat_ = 1.0f;
        sinceBeat_ = 0.0f;
    } else {
        beat_ *= std::exp(-6.0f * dt);
    }
    bassAverage_ += (bass - bassAverage_) * std::min(dt * 1.5f, 1.0f);
}

// --- MonitorCapture -----------------------------------------------------------------

struct MonitorCapture::Stream {
#if LUNAR_WITH_PULSE
    pa_simple* simple = nullptr;
    ~Stream()
    {
        if (simple)
            pa_simple_free(simple);
    }
#endif
};

MonitorCapture::MonitorCapture()
{
#if LUNAR_WITH_PULSE
    const pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, rate, 2};
    // Ten milliseconds a read.
    const pa_buffer_attr buffering{
        .maxlength = static_cast<std::uint32_t>(-1),
        .tlength = static_cast<std::uint32_t>(-1),
        .prebuf = static_cast<std::uint32_t>(-1),
        .minreq = static_cast<std::uint32_t>(-1),
        .fragsize = static_cast<std::uint32_t>(rate / 100 * 2 * sizeof(float)),
    };
    int error = 0;
    pa_simple* simple = pa_simple_new(nullptr, "Lunarstone", PA_STREAM_RECORD, "@DEFAULT_MONITOR@", "Music",
                                      &spec, nullptr, &buffering, &error);
    if (!simple) {
        error_ = std::format("PulseAudio: {}", pa_strerror(error));
        return;
    }
    stream_ = std::make_unique<Stream>();
    stream_->simple = simple;
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
#else
    error_ = "this build can't record sound (it was built without libpulse-simple)";
#endif
}

MonitorCapture::~MonitorCapture()
{
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

std::vector<float> MonitorCapture::take()
{
    const std::lock_guard lock(mutex_);
    return std::exchange(pending_, {});
}

std::string MonitorCapture::error() const
{
    const std::lock_guard lock(mutex_);
    return error_;
}

void MonitorCapture::run(std::stop_token stop)
{
#if LUNAR_WITH_PULSE
    std::vector<float> block(rate / 100 * 2);
    while (!stop.stop_requested()) {
        int error = 0;
        if (pa_simple_read(stream_->simple, block.data(), block.size() * sizeof(float), &error) < 0) {
            const std::lock_guard lock(mutex_);
            error_ = std::format("PulseAudio: {}", pa_strerror(error));
            return;
        }
        const std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i + 1 < block.size(); i += 2)
            pending_.push_back((block[i] + block[i + 1]) * 0.5f);
        // Nobody took them for a second: only the newest matter.
        if (pending_.size() > static_cast<std::size_t>(rate))
            pending_.erase(pending_.begin(), pending_.end() - rate / 2);
    }
#else
    (void)stop;
#endif
}

} // namespace lunar::audio
