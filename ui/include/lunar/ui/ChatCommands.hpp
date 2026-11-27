#pragma once

#include <lunar/Classes.hpp>

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::ui {

enum class ChatCommandKind { Custom, Help, Clear, Console, Mute, Unmute, Emote, Version, Team, Whisper, Channel };

struct ChatCommandInfo {
    ChatCommandKind kind;
    std::vector<std::string> aliases;
    bool enabled = true;
    bool visible = true;
    TextChatCommand* instance = nullptr;
};

inline std::string chatAlias(std::string_view value)
{
    std::string result(value);
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!result.empty() && result.front() != '/' && result != "%%")
        result.insert(result.begin(), '/');
    return result;
}

// The built-in objects can be renamed, disabled, or given different aliases by
// the experience. Dispatch and completion must read the same current values.
inline std::vector<ChatCommandInfo> chatCommands(const TextChatService* service)
{
    struct Default {
        std::string_view name, primary, secondary;
        ChatCommandKind kind;
    };
    using K = ChatCommandKind;
    constexpr std::array defaults{
        Default{"RBXHelpCommand", "/help", "/?", K::Help},
        Default{"RBXClearCommand", "/clear", "/cls", K::Clear},
        Default{"RBXConsoleCommand", "/console", "", K::Console},
        Default{"RBXMuteCommand", "/mute", "/m", K::Mute},
        Default{"RBXUnmuteCommand", "/unmute", "/um", K::Unmute},
        Default{"RBXEmoteCommand", "/emote", "/e", K::Emote},
        Default{"RBXVersionCommand", "/version", "/v", K::Version},
        Default{"RBXTeamCommand", "/team", "/t", K::Team},
        Default{"RBXWhisperCommand", "/whisper", "/w", K::Whisper},
    };
    std::vector<ChatCommandInfo> result;
    std::array<bool, defaults.size()> present{};
    if (service)
        for (const auto& child : service->descendants())
            if (auto* command = instanceCast<TextChatCommand>(child.get())) {
                K kind = K::Custom;
                for (std::size_t i = 0; i < defaults.size(); ++i)
                    if (command->name() == defaults[i].name) {
                        present[i] = true;
                        kind = defaults[i].kind;
                        break;
                    }
                ChatCommandInfo info{kind, {}, command->enabled(), command->autocompleteVisible(), command};
                for (const auto& alias : {command->primaryAlias(), command->secondaryAlias()})
                    if (!alias.empty())
                        info.aliases.push_back(chatAlias(alias));
                result.push_back(std::move(info));
            }
    for (std::size_t i = 0; i < defaults.size(); ++i) {
        if (present[i])
            continue;
        const auto& command = defaults[i];
        ChatCommandInfo info{
            command.kind, {std::string(command.primary)}, !service || service->createDefaultCommands()};
        if (!command.secondary.empty())
            info.aliases.emplace_back(command.secondary);
        if (command.kind == K::Team)
            info.aliases.emplace_back("%%");
        result.push_back(std::move(info));
    }
    result.push_back({K::Channel, {"/c", "/channel"}, !service || service->createDefaultCommands()});
    return result;
}

} // namespace lunar::ui
