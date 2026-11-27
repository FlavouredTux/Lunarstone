#include <lunar/avatar/UserAvatar.hpp>

#include <lunar/assets/Json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>

namespace lunar::avatar {

namespace json = assets::json;

namespace {

std::expected<json::Value, std::string> parseReply(const std::expected<assets::HttpResponse, std::string>& reply,
                                                   std::string_view what)
{
    if (!reply)
        return std::unexpected(std::format("{}: {}", what, reply.error()));
    if (reply->status != 200)
        return std::unexpected(std::format("{}: {}", what, assets::describeHttpFailure(*reply)));
    auto document = json::parse(reply->body);
    if (!document || !document->object())
        return std::unexpected(std::format("{}: the reply isn't a JSON object", what));
    return std::move(*document);
}

std::optional<std::int64_t> integerOf(const json::Value* value)
{
    return value ? value->integer() : std::nullopt;
}

std::optional<double> numberOf(const json::Value* value)
{
    return value ? value->number() : std::nullopt;
}

std::expected<RobloxUser, std::string> userFrom(const json::Value& value, std::string_view what)
{
    RobloxUser user;
    const std::optional<std::int64_t> id = integerOf(value["id"]);
    const json::Value* name = value["name"];
    const std::string* text = name ? name->string() : nullptr;
    if (!id || !text)
        return std::unexpected(std::format("{}: no user in the reply", what));
    user.id = *id;
    user.name = *text;
    const json::Value* display = value["displayName"];
    user.displayName = display && display->string() ? *display->string() : user.name;
    return user;
}

// Roblox usernames: 3 to 20 letters, digits and underscores.
bool validUsername(std::string_view name)
{
    return name.size() >= 3 && name.size() <= 20 && std::ranges::all_of(name, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
           });
}

std::optional<Color3> hexColor(const json::Value* value)
{
    if (!value || !value->string())
        return std::nullopt;
    const std::string& text = *value->string();
    const std::size_t start = text.starts_with('#') ? 1 : 0;
    std::uint32_t rgb = 0;
    const char* end = text.data() + text.size();
    if (text.size() != start + 6 || std::from_chars(text.data() + start, end, rgb, 16).ptr != end)
        return std::nullopt;
    return Color3::fromRGB(static_cast<int>(rgb >> 16), static_cast<int>((rgb >> 8) & 0xff),
                           static_cast<int>(rgb & 0xff));
}

// Enum.AssetType numbers the avatar endpoints name.
enum AssetType : std::uint8_t {
    TShirt = 2,
    Hat = 8,
    Shirt = 11,
    Pants = 12,
    Head = 17,
    Face = 18,
    Gear = 19,
    Torso = 27,
    RightArm = 28,
    LeftArm = 29,
    LeftLeg = 30,
    RightLeg = 31,
    HairAccessory = 41,
    FaceAccessory = 42,
    NeckAccessory = 43,
    ShoulderAccessory = 44,
    FrontAccessory = 45,
    BackAccessory = 46,
    WaistAccessory = 47,
    ClimbAnimation = 48,
    FallAnimation = 50,
    IdleAnimation = 51,
    JumpAnimation = 52,
    RunAnimation = 53,
    SwimAnimation = 54,
    WalkAnimation = 55,
    EmoteAnimation = 61,
    TShirtAccessory = 64,
    ShirtAccessory = 65,
    PantsAccessory = 66,
    JacketAccessory = 67,
    SweaterAccessory = 68,
    ShortsAccessory = 69,
    LeftShoeAccessory = 70,
    RightShoeAccessory = 71,
    DressSkirtAccessory = 72,
    EyebrowAccessory = 76,
    EyelashAccessory = 77,
    MoodAnimation = 78,
    DynamicHead = 79,
};

// Layered clothing and the other accessories that become
// AccessoryDescriptions rather than going into a slot's id list.
std::optional<std::pair<Enum::AccessoryType, bool>> describedAccessory(std::int64_t type)
{
    using A = Enum::AccessoryType;
    switch (type) {
    case TShirtAccessory:
        return std::pair{A::TShirt, true};
    case ShirtAccessory:
        return std::pair{A::Shirt, true};
    case PantsAccessory:
        return std::pair{A::Pants, true};
    case JacketAccessory:
        return std::pair{A::Jacket, true};
    case SweaterAccessory:
        return std::pair{A::Sweater, true};
    case ShortsAccessory:
        return std::pair{A::Shorts, true};
    case LeftShoeAccessory:
        return std::pair{A::LeftShoe, true};
    case RightShoeAccessory:
        return std::pair{A::RightShoe, true};
    case DressSkirtAccessory:
        return std::pair{A::DressSkirt, true};
    // GUESS: eyebrows and eyelashes are rigid, though they only fit a dynamic head.
    case EyebrowAccessory:
        return std::pair{A::Eyebrow, false};
    case EyelashAccessory:
        return std::pair{A::Eyelash, false};
    default:
        return std::nullopt;
    }
}

void appendId(std::string& list, std::int64_t id)
{
    if (!list.empty())
        list += ',';
    list += std::to_string(id);
}

void applyAsset(HumanoidDescription& description, UserAvatar& avatar, std::int64_t id, std::int64_t type,
                const json::Value* meta)
{
    auto slot
        = [&](const std::string& (HumanoidDescription::*get)() const, void (HumanoidDescription::*set)(std::string)) {
              std::string list = (description.*get)();
              appendId(list, id);
              (description.*set)(std::move(list));
          };
    switch (type) {
    case TShirt:
        description.setGraphicTShirt(id);
        return;
    case Shirt:
        description.setShirt(id);
        return;
    case Pants:
        description.setPants(id);
        return;
    case Face:
        description.setFace(id);
        return;
    case Head:
    case DynamicHead:
        description.setHead(id);
        return;
    case Torso:
        description.setTorso(id);
        return;
    case RightArm:
        description.setRightArm(id);
        return;
    case LeftArm:
        description.setLeftArm(id);
        return;
    case LeftLeg:
        description.setLeftLeg(id);
        return;
    case RightLeg:
        description.setRightLeg(id);
        return;
    case Hat:
        slot(&HumanoidDescription::hatAccessory, &HumanoidDescription::setHatAccessory);
        return;
    case HairAccessory:
        slot(&HumanoidDescription::hairAccessory, &HumanoidDescription::setHairAccessory);
        return;
    case FaceAccessory:
        slot(&HumanoidDescription::faceAccessory, &HumanoidDescription::setFaceAccessory);
        return;
    case NeckAccessory:
        slot(&HumanoidDescription::neckAccessory, &HumanoidDescription::setNeckAccessory);
        return;
    case ShoulderAccessory:
        slot(&HumanoidDescription::shouldersAccessory, &HumanoidDescription::setShouldersAccessory);
        return;
    case FrontAccessory:
        slot(&HumanoidDescription::frontAccessory, &HumanoidDescription::setFrontAccessory);
        return;
    case BackAccessory:
        slot(&HumanoidDescription::backAccessory, &HumanoidDescription::setBackAccessory);
        return;
    case WaistAccessory:
        slot(&HumanoidDescription::waistAccessory, &HumanoidDescription::setWaistAccessory);
        return;
    case ClimbAnimation:
        description.setClimbAnimation(id);
        return;
    case FallAnimation:
        description.setFallAnimation(id);
        return;
    case IdleAnimation:
        description.setIdleAnimation(id);
        return;
    case JumpAnimation:
        description.setJumpAnimation(id);
        return;
    case RunAnimation:
        description.setRunAnimation(id);
        return;
    case SwimAnimation:
        description.setSwimAnimation(id);
        return;
    case WalkAnimation:
        description.setWalkAnimation(id);
        return;
    case MoodAnimation:
        description.setMoodAnimation(id);
        return;
    case Gear:
        avatar.gear.push_back(id);
        return;
    default:
        break;
    }
    const auto accessory = describedAccessory(type);
    if (!accessory)
        return; // emotes and anything else the description has no place for
    auto item = std::make_shared<AccessoryDescription>();
    item->setName("AccessoryDescription");
    item->setAssetId(id);
    item->setAccessoryType(accessory->first);
    item->setIsLayered(accessory->second);
    // GUESS: layered items carry their stacking order and puffiness in "meta".
    if (meta) {
        if (const auto order = integerOf((*meta)["order"]))
            item->setOrder(static_cast<int>(*order));
        if (const auto puffiness = numberOf((*meta)["puffiness"]))
            item->setPuffiness(static_cast<float>(*puffiness));
    }
    item->setParent(&description);
}

} // namespace

std::expected<RobloxUser, std::string> findUser(const assets::RobloxWeb& web, std::string_view idOrName)
{
    std::int64_t id = 0;
    const std::string text(idOrName);
    const char* end = text.data() + text.size();
    if (!text.empty() && std::from_chars(text.data(), end, id).ptr == end) {
        if (id <= 0)
            return std::unexpected(std::format("{} isn't a user id", id));
        const std::string what = std::format("user {}", id);
        auto document
            = parseReply(web.anonymous({.url = std::format("https://users.roblox.com/v1/users/{}", id)}), what);
        if (!document)
            return std::unexpected(document.error());
        return userFrom(*document, what);
    }
    if (!validUsername(idOrName))
        return std::unexpected(std::format("\"{}\" isn't a user id or a username", idOrName));
    const std::string what = std::format("user {}", idOrName);
    auto document = parseReply(
        web.anonymous({.method = "POST",
                       .url = "https://users.roblox.com/v1/usernames/users",
                       .headers = {"Content-Type: application/json", "Accept: application/json"},
                       .body = std::format(R"({{"usernames":["{}"],"excludeBannedUsers":true}})", idOrName)}),
        what);
    if (!document)
        return std::unexpected(document.error());
    const json::Value* data = (*document)["data"];
    if (!data || !data->array() || data->array()->empty())
        return std::unexpected(std::format("there's no user called {}", idOrName));
    return userFrom(data->array()->front(), what);
}

std::expected<RobloxUser, std::string> authenticatedUser(const assets::RobloxWeb& web)
{
    if (!web.hasCookie())
        return std::unexpected(web.noLoginHint());
    auto document
        = parseReply(web.withCookie({.url = "https://users.roblox.com/v1/users/authenticated"}), "the signed-in user");
    if (!document)
        return std::unexpected(document.error());
    return userFrom(*document, "the signed-in user");
}

std::expected<UserAvatar, std::string> parseAvatarFetch(std::string_view text)
{
    auto document = json::parse(text);
    if (!document || !document->object())
        return std::unexpected("the avatar isn't a JSON object");
    UserAvatar avatar;
    avatar.description = std::make_shared<HumanoidDescription>();
    HumanoidDescription& description = *avatar.description;

    // parseUseR15 (0x1036ca51b): R6 or R15, anything else is an error.
    if (const json::Value* type = (*document)["resolvedAvatarType"]; type && type->string()) {
        if (*type->string() == "R6")
            avatar.rigType = Enum::HumanoidRigType::R6;
        else if (*type->string() != "R15")
            return std::unexpected(std::format("unknown resolvedAvatarType {}", *type->string()));
    } else {
        return std::unexpected("no resolvedAvatarType in the avatar");
    }

    if (const json::Value* colors = (*document)["bodyColor3s"]) {
        if (auto c = hexColor((*colors)["headColor3"]))
            description.setHeadColor(*c);
        if (auto c = hexColor((*colors)["torsoColor3"]))
            description.setTorsoColor(*c);
        if (auto c = hexColor((*colors)["leftArmColor3"]))
            description.setLeftArmColor(*c);
        if (auto c = hexColor((*colors)["rightArmColor3"]))
            description.setRightArmColor(*c);
        if (auto c = hexColor((*colors)["leftLegColor3"]))
            description.setLeftLegColor(*c);
        if (auto c = hexColor((*colors)["rightLegColor3"]))
            description.setRightLegColor(*c);
    } else if (const json::Value* ids = (*document)["bodyColors"]) {
        // The older form: BrickColor numbers (headColorId...).
        auto brick = [&](std::string_view key) -> std::optional<Color3> {
            const auto number = integerOf((*ids)[key]);
            if (!number)
                return std::nullopt;
            return BrickColor::fromNumber(static_cast<int>(*number)).color();
        };
        if (auto c = brick("headColorId"))
            description.setHeadColor(*c);
        if (auto c = brick("torsoColorId"))
            description.setTorsoColor(*c);
        if (auto c = brick("leftArmColorId"))
            description.setLeftArmColor(*c);
        if (auto c = brick("rightArmColorId"))
            description.setRightArmColor(*c);
        if (auto c = brick("leftLegColorId"))
            description.setLeftLegColor(*c);
        if (auto c = brick("rightLegColorId"))
            description.setRightLegColor(*c);
    }

    if (const json::Value* scales = (*document)["scales"]) {
        auto scale = [&](std::string_view key, void (HumanoidDescription::*set)(float)) {
            if (const auto value = numberOf((*scales)[key]))
                (description.*set)(static_cast<float>(*value));
        };
        scale("height", &HumanoidDescription::setHeightScale);
        scale("width", &HumanoidDescription::setWidthScale);
        scale("depth", &HumanoidDescription::setDepthScale);
        scale("head", &HumanoidDescription::setHeadScale);
        scale("proportion", &HumanoidDescription::setProportionScale);
        scale("bodyType", &HumanoidDescription::setBodyTypeScale);
    }

    if (const json::Value* assets = (*document)["assetAndAssetTypeIds"]; assets && assets->array()) {
        for (const json::Value& asset : *assets->array()) {
            const auto id = integerOf(asset["assetId"]);
            const auto type = integerOf(asset["assetTypeId"]);
            if (id && type && *id > 0)
                applyAsset(description, avatar, *id, *type, asset["meta"]);
        }
    }
    // animationAssetIds names the pack again, keyed by state (and "mood").
    if (const json::Value* animations = (*document)["animationAssetIds"]; animations && animations->object()) {
        const std::pair<std::string_view, void (HumanoidDescription::*)(std::int64_t)> states[] = {
            {"climb", &HumanoidDescription::setClimbAnimation}, {"fall", &HumanoidDescription::setFallAnimation},
            {"idle", &HumanoidDescription::setIdleAnimation},   {"jump", &HumanoidDescription::setJumpAnimation},
            {"run", &HumanoidDescription::setRunAnimation},     {"swim", &HumanoidDescription::setSwimAnimation},
            {"walk", &HumanoidDescription::setWalkAnimation},   {"mood", &HumanoidDescription::setMoodAnimation},
        };
        for (const auto& [key, set] : states)
            if (const auto id = integerOf((*animations)[key]); id && *id > 0)
                (description.*set)(*id);
    }

    if (const json::Value* emotes = (*document)["emotes"]; emotes && emotes->array()) {
        for (const json::Value& emote : *emotes->array()) {
            const auto id = integerOf(emote["assetId"]);
            if (!id)
                continue;
            AvatarEmote entry{.assetId = *id};
            if (const json::Value* name = emote["assetName"]; name && name->string())
                entry.name = *name->string();
            if (const auto position = integerOf(emote["position"]))
                entry.position = static_cast<int>(*position);
            if (!entry.name.empty()) {
                description.addEmote(entry.name, entry.assetId);
                if (entry.position > 0) {
                    auto equipped = description.equippedEmotes();
                    equipped.insert_or_assign(entry.position, entry.name);
                    description.setEquippedEmotes(std::move(equipped));
                }
            }
            avatar.emotes.push_back(std::move(entry));
        }
    }
    return avatar;
}

std::expected<UserAvatar, std::string> fetchUserAvatar(const assets::RobloxWeb& web, std::int64_t userId,
                                                       std::int64_t placeId)
{
    if (userId <= 0)
        return std::unexpected(std::format("{} isn't a user id", userId));
    auto reply = web.anonymous(
        {.url = std::format("https://avatar.roblox.com/v2/avatar/avatar-fetch/?placeId={}&userId={}", placeId, userId),
         .headers = {"Accept: application/json"}});
    const std::string what = std::format("avatar of user {}", userId);
    if (!reply)
        return std::unexpected(std::format("{}: {}", what, reply.error()));
    if (reply->status != 200)
        return std::unexpected(std::format("{}: {}", what, assets::describeHttpFailure(*reply)));
    auto avatar = parseAvatarFetch(reply->body);
    if (!avatar)
        return std::unexpected(std::format("{}: {}", what, avatar.error()));
    return avatar;
}

std::expected<std::string, std::string> fetchCharacterAppearanceInfo(const assets::RobloxWeb& web, std::int64_t userId)
{
    if (userId <= 0)
        return std::unexpected(std::format("{} isn't a user id", userId));
    auto reply = web.anonymous({.url = std::format("https://avatar.roblox.com/v2/avatar/users/{}/avatar", userId),
                                .headers = {"Accept: application/json"}});
    if (!reply)
        return std::unexpected(reply.error());
    if (reply->status != 200)
        return std::unexpected(assets::describeHttpFailure(*reply));
    return std::move(reply->body);
}

} // namespace lunar::avatar
