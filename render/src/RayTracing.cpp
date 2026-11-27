#include <lunar/render/RayTracing.hpp>

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

namespace lunar::render {

namespace {

// A name LUNAR_RT_SKIP takes, and the bit it sets.
struct Name {
    std::string_view name;
    std::uint32_t bit;
};

// Every name, in the order of RayTracingSkip's bits: the first name of a bit
// is the one rayTracingSkipName answers with, the rest are the aliases it
// takes too. "none" is not here: it means skip nothing, like being unset.
constexpr Name names[] = {
    {"sun", static_cast<std::uint32_t>(RayTracingSkip::Sun)},
    {"ao", static_cast<std::uint32_t>(RayTracingSkip::AmbientOcclusion)},
    {"ambient-occlusion", static_cast<std::uint32_t>(RayTracingSkip::AmbientOcclusion)},
    {"occlusion", static_cast<std::uint32_t>(RayTracingSkip::AmbientOcclusion)},
    {"lights", static_cast<std::uint32_t>(RayTracingSkip::LocalLights)},
    {"local-lights", static_cast<std::uint32_t>(RayTracingSkip::LocalLights)},
    {"reflect", static_cast<std::uint32_t>(RayTracingSkip::Reflections)},
    {"reflections", static_cast<std::uint32_t>(RayTracingSkip::Reflections)},
    {"reflection", static_cast<std::uint32_t>(RayTracingSkip::Reflections)},
    {"gather", static_cast<std::uint32_t>(RayTracingSkip::LightGather)},
    {"light-gather", static_cast<std::uint32_t>(RayTracingSkip::LightGather)},
    {"denoise", static_cast<std::uint32_t>(RayTracingSkip::Denoise)},
    {"sky", static_cast<std::uint32_t>(RayTracingSkip::SkyReach)},
    {"sky-reach", static_cast<std::uint32_t>(RayTracingSkip::SkyReach)},
};

// Whether a name from LUNAR_RT_SKIP, whitespace around it and all, is the
// (lower case) `name`: what a run of the benchmark says does not decide.
bool isName(std::string_view value, std::string_view name)
{
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!value.empty() && space(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && space(value.back()))
        value.remove_suffix(1);
    if (value.size() != name.size())
        return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(value[i]))) != name[i])
            return false;
    }
    return true;
}

// What LUNAR_RT_SKIP is set to, as its bits: unset, empty or "none" is zero,
// and so is a list of names none of which we know. The comma separated names
// are read in one go, and a name that isn't in the table above is ignored
// (the ones around it still count): a typo only costs the measurement of the
// feature it was meant for, and the log says what was skipped all the same.
std::uint32_t readMask()
{
    const char* value = std::getenv("LUNAR_RT_SKIP"); // NOLINT(concurrency-mt-unsafe)
    if (!value || *value == '\0')
        return 0;
    std::uint32_t mask = 0;
    for (std::string_view rest(value); !rest.empty();) {
        const std::size_t comma = rest.find(',');
        const std::string_view name = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (isName(name, "none"))
            continue;
        for (const Name& candidate : names) {
            if (isName(name, candidate.name)) {
                mask |= candidate.bit;
                break;
            }
        }
    }
    return mask;
}

// The mask, read once: everything here happens while the pipelines are made,
// so after that it costs a load and a test that is already made.
std::uint32_t skipMask()
{
    static const std::uint32_t mask = readMask();
    return mask;
}

} // namespace

std::uint32_t rayTracingSkipMask()
{
    return skipMask();
}

std::string rayTracingSkipText()
{
    std::string text;
    for (std::uint32_t bit = 1u; bit != 0; bit *= 2u) {
        if ((skipMask() & bit) == 0)
            continue;
        const std::string_view name = rayTracingSkipName(bit);
        if (name.empty())
            continue;
        if (!text.empty())
            text += ',';
        text += name;
    }
    return text.empty() ? std::string{"none"} : text;
}

std::string_view rayTracingSkipName(std::uint32_t bit)
{
    for (const Name& entry : names) {
        if (entry.bit == bit)
            return entry.name;
    }
    return {};
}

} // namespace lunar::render