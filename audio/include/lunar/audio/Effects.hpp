#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lunar::audio {

// Roblox's SoundEffects as the mixer runs them on a voice, at Mixer::rate.
// Parameters, in each kind's order (the properties' own units):
//   Equalizer:  LowGain, MidGain, HighGain (dB)
//   Distortion: Level (0-1)
//   Tremolo:    Depth (0-1), Duty (0-1), Frequency (Hz)
//   Flange:     Depth (0-1), Mix (0-1), Rate (Hz)
//   Chorus:     Depth (0-1), Mix (0-1), Rate (Hz)
//   Echo:       Delay (s), DryLevel (dB), Feedback (0-1), WetLevel (dB)
//   Reverb:     DecayTime (s), Density (0-1), Diffusion (0-1), DryLevel (dB), WetLevel (dB)
//   Compressor: Attack (s), GainMakeup (dB), Ratio, Release (s), Threshold (dB)
//   PitchShift: Octave (pitch times this, 0.5-2)
enum class EffectKind : std::uint8_t { Equalizer, Distortion, Tremolo, Flange, Chorus, Echo, Reverb, Compressor, PitchShift };

struct Effect {
    EffectKind kind = EffectKind::Equalizer;
    std::array<float, 5> p{};
    friend bool operator==(const Effect&, const Effect&) = default;
};

// One voice's effects, in order, with what each keeps from block to block
// (filter memories, delay lines); kept while the kinds stay the same.
class EffectChain {
public:
    EffectChain();
    ~EffectChain();
    EffectChain(EffectChain&&) noexcept;
    EffectChain& operator=(EffectChain&&) noexcept;

    // In place over interleaved stereo.
    void process(std::span<const Effect> effects, std::span<float> stereo);

    struct Stage;

private:
    std::vector<std::unique_ptr<Stage>> stages_;
    std::vector<EffectKind> kinds_;
};

} // namespace lunar::audio
