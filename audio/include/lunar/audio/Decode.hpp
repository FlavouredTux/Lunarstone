#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::audio {

// Decoded audio: 16-bit samples, interleaved, at its own rate.
struct Clip {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0; // 1 or 2
    std::vector<std::int16_t> samples;

    std::size_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }
    double seconds() const { return sampleRate == 0 ? 0.0 : static_cast<double>(frames()) / sampleRate; }
};

// What Roblox serves sounds as: Ogg Vorbis, MP3 or WAV (16-bit PCM),
// told apart by their first bytes. More than two channels keep the first
// two. Ogg and MP3 need the system's vorbisfile and mpg123 (see
// audio/CMakeLists.txt); without them those fail with a reason.
std::expected<Clip, std::string> decodeAudio(std::string_view bytes);

} // namespace lunar::audio
