#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace lunar::audio {

// What the music sounds like right now, for something that dances to it:
// mono samples in, a log-spaced spectrum and a beat out. Lunarstone's own,
// nothing of Roblox's.
class Spectrum {
public:
    static constexpr std::size_t fftSize = 2048;

    explicit Spectrum(int sampleRate = 48000, std::size_t bandCount = 64);

    // Samples, newest last; only the last fftSize are kept.
    void push(std::span<const float> samples);
    // The bands and the beat, moved on by `dt` seconds from the latest
    // samples: bands jump up and fall back slowly, the beat is 1 on a beat
    // and fades out.
    void update(float dt);

    // 0..1 each, low frequencies first (40 Hz to 16 kHz).
    std::span<const float> bands() const { return bands_; }
    float beat() const { return beat_; }
    // How busy the spectrum is overall, 0..1 (after the automatic gain).
    float level() const { return level_; }

private:
    int sampleRate_;
    std::vector<float> history_; // the last fftSize samples, a ring
    std::size_t next_ = 0;
    std::vector<float> window_;
    std::vector<std::size_t> edges_; // FFT bins where each band starts, one more at the end
    std::vector<float> bands_;
    float level_ = 0.0f;
    float referenceDb_ = -60.0f; // the automatic gain's full scale
    float beat_ = 0.0f;
    float bassAverage_ = 0.0f;
    float sinceBeat_ = 1.0f;
};

// What the speakers play, recorded from the default output's monitor
// (PulseAudio, or PipeWire through its PulseAudio server) on a thread of
// its own: whatever music is on (Spotify, a browser, the game itself).
class MonitorCapture {
public:
    static constexpr int rate = 48000;

    MonitorCapture();
    ~MonitorCapture();
    MonitorCapture(const MonitorCapture&) = delete;
    MonitorCapture& operator=(const MonitorCapture&) = delete;

    // Mono samples recorded since the last call.
    std::vector<float> take();
    // Why nothing is recorded; empty while it is.
    std::string error() const;

private:
    void run(std::stop_token stop);

    struct Stream;
    std::unique_ptr<Stream> stream_;
    mutable std::mutex mutex_;
    std::vector<float> pending_;
    std::string error_;
    std::jthread thread_; // last, so it stops before the rest goes away
};

} // namespace lunar::audio
