#pragma once

// TextChatService as a client's own chat window sees it: what each message
// looks like once the game's OnIncomingMessage callbacks had their say, and
// how a typed message goes out. ScriptContext::setChatSink and sendChat are
// the entry points (TextChat.cpp has the protocol).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lunar {

class Instance;

struct ChatLine {
    enum class Kind : std::uint8_t {
        Message, // a player's message (ServerToClientMessageReplicateSignal), or our own being sent
        System,  // TextChannel:DisplaySystemMessage
        Hidden,  // UserMessageIntentSentRemote: someone said something this account may not read
        Bubble,  // TextChatService:DisplayBubble: a bubble over `adornee`, not a chat window line
    };
    Kind kind = Kind::Message;
    std::string channel;     // the TextChannel's name: RBXGeneral, RBXSystem, RBXTeam..., RBXWhisper:...
    std::string prefix;      // PrefixText, rich text
    std::string text;        // Text, rich text with &lt; &gt; &amp; as the server escapes them
    std::string translation; // Translation, when the server or a callback gave one
    std::string senderName;  // the sender's Player.Name; empty for system lines
    std::string senderDisplayName;
    std::int64_t userId = 0; // the sender's (TextSource.UserId), 0 for system lines
    std::string messageId;   // "<userId>-{guid}"
    std::string status;      // the Enum.TextChatMessageStatus item: Success, Sending, Floodchecked ...
    std::int64_t timestamp = 0; // Unix milliseconds, from the server; 0 when it gave none
    bool own = false;           // the local player's
    std::weak_ptr<Instance> adornee; // Bubble: the part or character it hangs over
};

// Preset chat: the catalog's fixed phrases, which an account that may not
// chat freely (no age check) can still send where the experience allows it.
// What the server said about it: ServerToClientPresetChatConfigChangedSignal(
// enabled, config) and ServerToClientPresetChatUserAccessChangedSignal(accessLevel).
struct PresetChatAccess {
    bool enabled = false; // the experience offers it
    std::string config;   // JSON: {"surface":"in-experience-chat","defaultVersion":"default-draft-v4","customVersion":""}
    std::string access;   // ours: "Enabled" [live]; empty until the server says
};

// One phrase of the catalog (apis.roblox.com/preset-chat/v1/preset-chat-options).
struct ChatPreset {
    std::string id;      // presetId, a UUID
    std::string text;    // the phrase in our locale ("Good luck")
    std::string version; // the catalog's version ("default-draft-v4")
};

// Rich text as plain text: tags dropped, &lt; &gt; &quot; &apos; &amp; turned back into characters.
std::string plainChatText(std::string_view richText);

// What TextChannel:SendAsync does to a message before it goes out (the 0.742
// client with TextChatEscapePostFilterClient off): surrounding whitespace
// trimmed, & < > escaped, a tab becomes three spaces, and runs of more than
// three spaces or newlines are cut to three.
std::string prepareChatText(std::string_view text);

} // namespace lunar
