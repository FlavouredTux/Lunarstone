#pragma once

#include <lunar/audio/Decode.hpp>
#include <lunar/audio/Effects.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lunar::audio {

// Plays clips: voices mixed into stereo at `rate`, each resampled to its
// speed (linearly), at a gain per ear, looping or not. With an output, a
// thread of its own writes the mix to PulseAudio (which PipeWire serves
// too) a few hundredths of a second ahead; without one, mix() is called by hand.
//
// What's asked of a voice waits for the next block: a mix only holds the
// lock to pick the changes up and to say where its voices got to, so the
// game's thread never waits for one. Starting, stopping, jumping and being
// removed fade over fadeFrames instead of clicking.
class Mixer {
public:
    static constexpr std::uint32_t rate = 48000;
    static constexpr std::uint32_t fadeFrames = rate / 500; // 2 ms

    // output: open the sound device (failing quietly: outputError says why).
    explicit Mixer(bool output = true);
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    bool hasOutput() const { return output_ != nullptr; }
    const std::string& outputError() const { return outputError_; }

    using Voice = std::uint32_t;
    struct Params {
        std::shared_ptr<const Clip> clip;
        // A voice that played to its end stays stopped while this stays
        // set: it starts again once it's stopped or seeked.
        bool playing = false;
        bool looped = false;
        float speed = 1.0f;
        float left = 1.0f; // gain per ear, ramped to over a block so changes don't click
        float right = 1.0f;
        std::vector<Effect> effects; // run in order on the voice, before its gains
        friend bool operator==(const Params&, const Params&) = default;
    };
    // What it did since: where it is, and how often it ended or looped.
    struct State {
        double seconds = 0.0;
        std::uint64_t ended = 0;
        std::uint64_t loops = 0;
        bool playing = false;
        // The seek (seek()'s number) its last end came after: an end from
        // before the latest seek is the old play's, which that seek replaced.
        std::uint64_t endedAfter = 0;
    };

    Voice add();
    void remove(Voice voice);
    void set(Voice voice, const Params& params);
    // From there on; a voice that's playing fades out where it was and in
    // there. Returns the seek's number for the voice (1, 2, ...; 0 for none).
    std::uint64_t seek(Voice voice, double seconds);
    // As of the last block, but where it was seeked to if that's still to come.
    State state(Voice voice) const;
    std::size_t voices() const;

    // Adds every playing voice into `stereo` (left, right, ...); the
    // output thread's work, for tests and offline use.
    void mix(std::span<float> stereo);

private:
    // A voice as asked for, and as last heard of (under mutex_).
    struct Control {
        Params params;
        std::optional<double> seek;
        std::uint64_t seeks = 0; // asked for
        bool queued = false;     // in changed_
        State state;
    };
    // A voice as the mix plays it: mix()'s own.
    struct Entry {
        Params params;
        std::shared_ptr<const Clip> sounding; // the clip it's playing
        std::optional<double> seek;
        double frame = 0.0; // into `sounding`
        bool on = false;
        bool finished = false;             // played to its end and not asked again since
        bool removed = false;              // fading out on its way out
        std::uint32_t fadeIn = fadeFrames; // frames into fading in
        float left = 0.0f;                 // the gains reached so far
        float right = 0.0f;
        std::uint64_t ended = 0;
        std::uint64_t loops = 0;
        std::uint64_t seeks = 0;      // the last seek's number, as picked up
        std::uint64_t endedAfter = 0; // `seeks` when it last ended
        EffectChain chain;
    };
    void play(Entry& e, std::span<float> stereo);

    void run(std::stop_token stop);

    mutable std::mutex mutex_;
    std::unordered_map<Voice, Control> controls_;
    std::vector<Voice> changed_;
    std::vector<Voice> removed_;
    Voice next_ = 1;

    std::unordered_map<Voice, Entry> entries_;
    std::vector<float> scratch_; // one voice alone, for its effects
    struct Output;
    std::unique_ptr<Output> output_;
    std::string outputError_;
    std::jthread thread_;
};

} // namespace lunar::audio
