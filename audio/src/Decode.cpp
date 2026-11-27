#include <lunar/audio/Decode.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <span>

#if LUNAR_WITH_VORBIS
#include <vorbis/vorbisfile.h>
#endif
#if LUNAR_WITH_MPG123
#include <mpg123.h>
#endif

namespace lunar::audio {

namespace {

// Keeps the first two channels of interleaved samples.
void append(Clip& clip, std::span<const std::int16_t> samples, std::uint32_t channels)
{
    if (channels <= 2) {
        clip.samples.insert(clip.samples.end(), samples.begin(), samples.end());
        return;
    }
    for (std::size_t i = 0; i + channels <= samples.size(); i += channels) {
        clip.samples.push_back(samples[i]);
        clip.samples.push_back(samples[i + 1]);
    }
}

std::uint32_t le32(std::string_view b, std::size_t at)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at]))
           | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8
           | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16
           | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24;
}

std::uint16_t le16(std::string_view b, std::size_t at)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[at])
                                      | static_cast<unsigned char>(b[at + 1]) << 8);
}

std::expected<Clip, std::string> decodeWav(std::string_view bytes)
{
    Clip clip;
    std::uint16_t format = 0, bits = 0;
    std::uint32_t channels = 0;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const std::string_view id = bytes.substr(at, 4);
        const std::uint32_t size = le32(bytes, at + 4);
        const std::size_t body = at + 8;
        if (size > bytes.size() - body)
            return std::unexpected("WAV chunk runs past the end");
        if (id == "fmt " && size >= 16) {
            format = le16(bytes, body);
            channels = le16(bytes, body + 2);
            clip.sampleRate = le32(bytes, body + 4);
            bits = le16(bytes, body + 14);
        } else if (id == "data") {
            if (format != 1 || bits != 16 || channels == 0 || clip.sampleRate == 0)
                return std::unexpected(std::format("WAV format {} with {} bits isn't 16-bit PCM", format, bits));
            std::vector<std::int16_t> samples(size / 2);
            std::memcpy(samples.data(), bytes.data() + body, samples.size() * 2);
            append(clip, samples, channels);
            clip.channels = std::min<std::uint32_t>(channels, 2);
            return clip;
        }
        at = body + size + (size & 1);
    }
    return std::unexpected("WAV without data");
}

#if LUNAR_WITH_VORBIS
struct Reader {
    std::string_view bytes;
    std::size_t at = 0;
};

std::expected<Clip, std::string> decodeOgg(std::string_view bytes)
{
    Reader reader{bytes};
    ov_callbacks callbacks{
        .read_func = [](void* out, std::size_t size, std::size_t count, void* source) -> std::size_t {
            auto& r = *static_cast<Reader*>(source);
            const std::size_t n = std::min(size * count, r.bytes.size() - r.at);
            std::memcpy(out, r.bytes.data() + r.at, n);
            r.at += n;
            return size == 0 ? 0 : n / size;
        },
        .seek_func = [](void* source, ogg_int64_t offset, int whence) -> int {
            auto& r = *static_cast<Reader*>(source);
            const auto base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<ogg_int64_t>(r.at)
                                                                           : static_cast<ogg_int64_t>(r.bytes.size());
            const ogg_int64_t to = base + offset;
            if (to < 0 || to > static_cast<ogg_int64_t>(r.bytes.size()))
                return -1;
            r.at = static_cast<std::size_t>(to);
            return 0;
        },
        .close_func = nullptr,
        .tell_func = [](void* source) -> long { return static_cast<long>(static_cast<Reader*>(source)->at); },
    };
    OggVorbis_File file;
    if (ov_open_callbacks(&reader, &file, nullptr, 0, callbacks) != 0)
        return std::unexpected("not an Ogg Vorbis stream");
    const vorbis_info* info = ov_info(&file, -1);
    Clip clip;
    clip.sampleRate = static_cast<std::uint32_t>(info->rate);
    const auto channels = static_cast<std::uint32_t>(info->channels);
    clip.channels = std::min<std::uint32_t>(channels, 2);
    std::vector<std::int16_t> buffer(4096 * channels);
    int section = 0;
    for (;;) {
        const long got = ov_read(&file, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size() * 2),
                                 0, 2, 1, &section);
        if (got == OV_HOLE)
            continue;
        if (got <= 0)
            break;
        append(clip, std::span(buffer).first(static_cast<std::size_t>(got) / 2), channels);
    }
    ov_clear(&file);
    if (clip.channels == 0 || clip.samples.empty())
        return std::unexpected("Ogg Vorbis stream without samples");
    return clip;
}
#endif

#if LUNAR_WITH_MPG123
std::expected<Clip, std::string> decodeMp3(std::string_view bytes)
{
    static const bool initialized = [] {
#if MPG123_API_VERSION < 46
        mpg123_init();
#endif
        return true;
    }();
    (void)initialized;
    int error = 0;
    mpg123_handle* handle = mpg123_new(nullptr, &error);
    if (!handle)
        return std::unexpected(std::format("mpg123: {}", mpg123_plain_strerror(error)));
    struct Close {
        mpg123_handle* h;
        ~Close() { mpg123_delete(h); }
    } close{handle};
    mpg123_param(handle, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);
    // Any rate, as 16-bit samples.
    mpg123_format_none(handle);
    const long* rates = nullptr;
    std::size_t rateCount = 0;
    mpg123_rates(&rates, &rateCount);
    for (std::size_t i = 0; i < rateCount; ++i)
        mpg123_format(handle, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    if (mpg123_open_feed(handle) != MPG123_OK)
        return std::unexpected("mpg123 won't open a feed");
    if (mpg123_feed(handle, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) != MPG123_OK)
        return std::unexpected("mpg123 won't take the data");
    Clip clip;
    std::uint32_t channels = 0;
    std::vector<std::int16_t> buffer(8192);
    for (;;) {
        std::size_t done = 0;
        const int result = mpg123_read(handle, reinterpret_cast<unsigned char*>(buffer.data()), buffer.size() * 2, &done);
        if (result == MPG123_NEW_FORMAT) {
            long rate = 0;
            int ch = 0, encoding = 0;
            mpg123_getformat(handle, &rate, &ch, &encoding);
            if (clip.sampleRate != 0 && (clip.sampleRate != static_cast<std::uint32_t>(rate) || channels != static_cast<std::uint32_t>(ch)))
                break; // a format change midway: keep what came before
            clip.sampleRate = static_cast<std::uint32_t>(rate);
            channels = static_cast<std::uint32_t>(ch);
            clip.channels = std::min<std::uint32_t>(channels, 2);
        }
        if (done > 0 && channels > 0)
            append(clip, std::span(buffer).first(done / 2), channels);
        if (result == MPG123_DONE || result == MPG123_NEED_MORE)
            break;
        if (result != MPG123_OK && result != MPG123_NEW_FORMAT)
            return std::unexpected(std::format("mpg123: {}", mpg123_plain_strerror(result)));
    }
    if (clip.channels == 0 || clip.samples.empty())
        return std::unexpected("MP3 without samples");
    return clip;
}
#endif

} // namespace

std::expected<Clip, std::string> decodeAudio(std::string_view bytes)
{
    if (bytes.size() < 12)
        return std::unexpected("too short to be audio");
    if (bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WAVE")
        return decodeWav(bytes);
    if (bytes.starts_with("OggS")) {
#if LUNAR_WITH_VORBIS
        return decodeOgg(bytes);
#else
        return std::unexpected("Ogg Vorbis needs vorbisfile, which this build is without");
#endif
    }
    if (bytes.starts_with("fLaC"))
        return std::unexpected("FLAC isn't supported");
    // ID3 tags or a bare MPEG frame sync: MP3.
    const auto b0 = static_cast<unsigned char>(bytes[0]);
    const auto b1 = static_cast<unsigned char>(bytes[1]);
    if (bytes.starts_with("ID3") || (b0 == 0xFF && (b1 & 0xE0) == 0xE0)) {
#if LUNAR_WITH_MPG123
        return decodeMp3(bytes);
#else
        return std::unexpected("MP3 needs mpg123, which this build is without");
#endif
    }
    return std::unexpected("not a sound format this knows (Ogg, MP3, WAV)");
}

} // namespace lunar::audio
