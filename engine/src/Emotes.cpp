#include <lunar/Classes.hpp>

#include <charconv>
#include <limits>

namespace lunar {
namespace {

// RobloxPlayer 0.742 vfn_98/vfn_101 emit caret-delimited fields and a
// backslash after each entry; vfn_99/vfn_102 read the same format.
std::string_view take(std::string_view& input, char delimiter)
{
    const auto end = input.find(delimiter);
    const auto field = input.substr(0, end);
    input = end == input.npos ? std::string_view{} : input.substr(end + 1);
    return field;
}

template <class T>
std::optional<T> integer(std::string_view input)
{
    const auto start = input.find_first_not_of(" \t\r\n");
    if (start == input.npos)
        return {};
    input.remove_prefix(start);
    if (input.front() == '+')
        input.remove_prefix(1);
    T value{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size())
        return {};
    return value;
}

} // namespace

void HumanoidDescription::setEmotes(Emotes emotes)
{
    std::erase_if(emotes, [](const auto& entry) { return entry.second.empty(); });
    emoteState_.available = std::move(emotes);
    changed.fire("EmotesDataInternal");
    emoteState_.availableChanged.fire(emoteState_.available);
}

void HumanoidDescription::addEmote(std::string name, std::int64_t id)
{
    emoteState_.available[std::move(name)].insert(id);
    changed.fire("EmotesDataInternal");
    emoteState_.availableChanged.fire(emoteState_.available);
}

void HumanoidDescription::removeEmote(std::string_view name)
{
    if (auto it = emoteState_.available.find(name); it != emoteState_.available.end())
        emoteState_.available.erase(it);
    changed.fire("EmotesDataInternal");
    emoteState_.availableChanged.fire(emoteState_.available);
}

void HumanoidDescription::setEquippedEmotes(EquippedEmotes emotes)
{
    emoteState_.equipped = std::move(emotes);
    changed.fire("EquippedEmotesDataInternal");
    emoteState_.equippedChanged.fire(emoteState_.equipped);
}

std::string HumanoidDescription::emotesDataInternal() const
{
    std::string data;
    for (const auto& [name, ids] : emoteState_.available) {
        data += name;
        data += '^';
        for (const auto id : ids) {
            data += std::to_string(id);
            data += '^';
        }
        data += '\\';
    }
    return data;
}

void HumanoidDescription::setEmotesDataInternal(std::string data)
{
    Emotes emotes;
    std::string_view input(data);
    while (!input.empty()) {
        auto entry = take(input, '\\');
        if (entry.find('^') == entry.npos)
            break;
        const std::string name(take(entry, '^'));
        std::set<std::int64_t> ids;
        while (!entry.empty()) {
            if (const auto id = integer<std::int64_t>(take(entry, '^')); id && *id != 0)
                ids.insert(*id);
        }
        if (!ids.empty())
            emotes.insert_or_assign(name, std::move(ids));
    }
    setEmotes(std::move(emotes));
}

std::string HumanoidDescription::equippedEmotesDataInternal() const
{
    std::string data;
    for (const auto& [slot, name] : emoteState_.equipped) {
        data += std::to_string(slot);
        data += '^';
        data += name;
        data += '\\';
    }
    return data;
}

void HumanoidDescription::setEquippedEmotesDataInternal(std::string data)
{
    EquippedEmotes emotes;
    std::string_view input(data);
    while (!input.empty()) {
        auto entry = take(input, '\\');
        if (entry.find('^') == entry.npos)
            break;
        const auto slot = integer<int>(take(entry, '^'));
        if (slot)
            emotes.insert_or_assign(*slot, std::string(entry));
    }
    setEquippedEmotes(std::move(emotes));
}

} // namespace lunar
