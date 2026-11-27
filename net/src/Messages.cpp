#include <lunar/net/Messages.hpp>

namespace lunar::net {

const char* messageIdName(std::uint8_t id)
{
    switch (id) {
    // RakNet 4.
    case 0x00:
        return "ID_CONNECTED_PING";
    case 0x01:
        return "ID_UNCONNECTED_PING";
    case 0x02:
        return "ID_UNCONNECTED_PING_OPEN_CONNECTIONS";
    case 0x03:
        return "ID_CONNECTED_PONG";
    case 0x04:
        return "ID_DETECT_LOST_CONNECTIONS";
    case 0x05:
        return "ID_OPEN_CONNECTION_REQUEST_1";
    case 0x06:
        return "ID_OPEN_CONNECTION_REPLY_1";
    case 0x07:
        return "ID_OPEN_CONNECTION_REQUEST_2";
    case 0x08:
        return "ID_OPEN_CONNECTION_REPLY_2";
    case 0x09:
        return "ID_CONNECTION_REQUEST";
    case 0x0a:
        return "ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY";
    case 0x0b:
        return "ID_OUR_SYSTEM_REQUIRES_SECURITY";
    case 0x0c:
        return "ID_PUBLIC_KEY_MISMATCH";
    case 0x0d:
        return "ID_OUT_OF_BAND_INTERNAL";
    case 0x0e:
        return "ID_SND_RECEIPT_ACKED";
    case 0x0f:
        return "ID_SND_RECEIPT_LOSS";
    case 0x10:
        return "ID_CONNECTION_REQUEST_ACCEPTED";
    case 0x11:
        return "ID_CONNECTION_ATTEMPT_FAILED";
    case 0x12:
        return "ID_ALREADY_CONNECTED";
    case 0x13:
        return "ID_NEW_INCOMING_CONNECTION";
    case 0x14:
        return "ID_NO_FREE_INCOMING_CONNECTIONS";
    case 0x15:
        return "ID_DISCONNECTION_NOTIFICATION";
    case 0x16:
        return "ID_CONNECTION_LOST";
    case 0x17:
        return "ID_CONNECTION_BANNED";
    case 0x18:
        return "ID_INVALID_PASSWORD";
    case 0x19:
        return "ID_INCOMPATIBLE_PROTOCOL_VERSION";
    case 0x1a:
        return "ID_IP_RECENTLY_CONNECTED";
    case 0x1b:
        return "ID_TIMESTAMP";
    // Roblox's handshake ids (offline).
    case 0x78:
        return "ID_RBX_OPEN_REQUEST_2";
    case 0x7b:
        return "ID_RBX_OPEN_REQUEST_1";
    case 0x7d:
        return "ID_RBX_OPEN_REPLY_2";
    case 0x7e:
        return "ID_RBX_OPEN_REPLY_1";
    // Roblox. GUESS for the names the binary doesn't spell out (they follow
    // the community's long-standing table); see ROBLOX-LAYER.md.
    case 0x81:
        return "ID_SET_GLOBALS";
    case 0x82:
        return "ID_TEACH_DESCRIPTOR_DICTIONARIES";
    case 0x83:
        return "ID_DATA";
    case 0x84:
        return "ID_MARKER";
    case 0x85:
        return "ID_PHYSICS";
    case 0x86:
        return "ID_TOUCHES";
    case 0x87:
        return "ID_CHAT_ALL";
    case 0x88:
        return "ID_CHAT_TEAM";
    case 0x89:
        return "ID_REPORT_ABUSE";
    case 0x8a:
        return "ID_SUBMIT_TICKET";
    case 0x8b:
        return "ID_CHAT_GAME";
    case 0x8c:
        return "ID_CHAT_PLAYER";
    case 0x8d:
        return "ID_CLUSTER";
    case 0x8e:
        return "ID_PROTOCOL_MISMATCH";
    case 0x8f:
        return "ID_PREFERRED_SPAWN_NAME";
    case 0x90:
        return "ID_JOIN_REQUEST";
    case 0x91:
        return "ID_SCHEMA_SYNC";
    case 0x92:
        return "ID_PLACEID_VERIFICATION";
    case 0x93:
        return "ID_DICTIONARY_FORMAT";
    case 0x94:
        return "ID_HASH_MISMATCH";
    case 0x95:
        return "ID_SECURITYKEY_MISMATCH";
    case 0x96:
        return "ID_REQUEST_STATS";
    case 0x97:
        return "ID_NEW_SCHEMA";
    case 0x98:
        return "ID_KICK";
    case 0x9a:
        return "ID_REMOTE_MICROPROFILER";
    case 0x9b:
        return "ID_BYTECODE_CHALLENGE"; // GUESS for the name: "Bytecode Auth Check"
    case 0x9c:
        return "ID_SCRIPT_BLOB"; // GUESS: a blob handed to the script runtime (0x1054c1060)
    case 0xa1:
        return "ID_ISR";
    case 0xa2:
        return "ID_SECURITY_A2"; // the security channel (SecurityChannelJob, 0x1057a6606), GUESS names
    case 0xa3:
        return "ID_SECURITY_A3";
    case 0xa4:
        return "ID_SECURITY_A4";
    case 0xa5:
        return "ID_LARGE_REPLICATOR"; // 0x104b7222c: "LargeReplicatorSender received non-ack type packet!"
    case 0xa6:
        return "ID_ISR_RECEIVER"; // 0x1055003a4, read only with ISR on (GUESS name)
    case 0xa7:
        return "ID_SHARED_STRING_HASHES";
    case 0xaa:
        return "ID_HASHED"; // the FNV-1a wrapper (ROBLOX-LAYER.md 1.3)
    default:
        return nullptr;
    }
}

} // namespace lunar::net
