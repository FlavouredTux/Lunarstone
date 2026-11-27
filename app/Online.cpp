#include "Online.hpp"

#include <lunar/assets/Inflate.hpp>
#include <lunar/assets/Web.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <system_error>

namespace {

std::string describeColor(lunar::Color3 color)
{
    return std::format("#{:02X}{:02X}{:02X}", std::lround(color.r * 255.0f), std::lround(color.g * 255.0f),
                       std::lround(color.b * 255.0f));
}

} // namespace

bool parseUserFlag(std::span<const std::string_view> args, std::size_t& i, UserChoice& choice)
{
    if (args[i] == "--me") {
        choice.me = true;
        return true;
    }
    if (args[i] == "--user" && i + 1 < args.size()) {
        choice.user = args[++i];
        return true;
    }
    return false;
}

std::expected<ChosenUser, std::string> resolveUser(const UserChoice& choice)
{
    const lunar::assets::RobloxWeb web;
    auto user = choice.me ? lunar::avatar::authenticatedUser(web) : lunar::avatar::findUser(web, choice.user);
    if (!user)
        return std::unexpected(user.error());
    auto avatar = lunar::avatar::fetchUserAvatar(web, user->id);
    if (!avatar)
        return std::unexpected(avatar.error());
    return ChosenUser{.user = std::move(*user),
                      .avatar = std::make_shared<const lunar::avatar::UserAvatar>(std::move(*avatar))};
}

int avatarInfo(const UserChoice& choice)
{
    auto chosen = resolveUser(choice);
    if (!chosen) {
        std::println(stderr, "{}", chosen.error());
        return 1;
    }
    const lunar::avatar::RobloxUser& user = chosen->user;
    const lunar::avatar::UserAvatar& avatar = *chosen->avatar;
    const lunar::HumanoidDescription& d = *avatar.description;
    std::println("{} (@{}, user {})", user.displayName, user.name, user.id);
    std::println("rig: {}", avatar.rigType == lunar::Enum::HumanoidRigType::R6 ? "R6" : "R15");
    std::println("colors: head {} torso {} arms {} {} legs {} {}", describeColor(d.headColor()),
                 describeColor(d.torsoColor()), describeColor(d.leftArmColor()), describeColor(d.rightArmColor()),
                 describeColor(d.leftLegColor()), describeColor(d.rightLegColor()));
    std::println("scales: height {} width {} depth {} head {} proportion {} body type {}", d.heightScale(),
                 d.widthScale(), d.depthScale(), d.headScale(), d.proportionScale(), d.bodyTypeScale());
    std::println("body: head {} torso {} arms {} {} legs {} {}", d.head(), d.torso(), d.leftArm(), d.rightArm(),
                 d.leftLeg(), d.rightLeg());
    std::println("clothing: shirt {} pants {} t-shirt {} face {}", d.shirt(), d.pants(), d.graphicTShirt(), d.face());
    const std::pair<std::string_view, const std::string*> slots[] = {
        {"hat", &d.hatAccessory()},   {"hair", &d.hairAccessory()},           {"face", &d.faceAccessory()},
        {"neck", &d.neckAccessory()}, {"shoulders", &d.shouldersAccessory()}, {"front", &d.frontAccessory()},
        {"back", &d.backAccessory()}, {"waist", &d.waistAccessory()},
    };
    for (const auto& [slot, list] : slots)
        if (!list->empty())
            std::println("accessory {}: {}", slot, *list);
    for (const lunar::InstancePtr& child : d.children())
        if (const auto* item = dynamic_cast<const lunar::AccessoryDescription*>(child.get()))
            std::println("accessory {} (type {}{}, order {})", item->assetId(), static_cast<int>(item->accessoryType()),
                         item->isLayered() ? ", layered" : "", item->order());
    std::println("animations: idle {} walk {} run {} jump {} fall {} climb {} swim {} mood {}", d.idleAnimation(),
                 d.walkAnimation(), d.runAnimation(), d.jumpAnimation(), d.fallAnimation(), d.climbAnimation(),
                 d.swimAnimation(), d.moodAnimation());
    for (const lunar::avatar::AvatarEmote& emote : avatar.emotes)
        std::println("emote {}: {} ({})", emote.position, emote.assetId, emote.name);
    return 0;
}

int downloadPlace(std::string_view placeId, const std::filesystem::path& outPath)
{
    std::uint64_t id = 0;
    const std::string text(placeId);
    const char* end = text.data() + text.size();
    if (text.empty() || std::from_chars(text.data(), end, id).ptr != end || id == 0) {
        std::println(stderr, "{} isn't a place id", placeId);
        return 2;
    }
    const lunar::assets::RobloxWeb web;
    auto bytes = web.downloadAsset(id);
    if (!bytes) {
        std::println(stderr, "place {}: {}", id, bytes.error());
        return 1;
    }
    if (bytes->starts_with("\x1f\x8b")) {
        auto unpacked
            = lunar::assets::gunzip(std::span(reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()));
        if (!unpacked) {
            std::println(stderr, "place {}: {}", id, unpacked.error());
            return 1;
        }
        bytes->assign(unpacked->begin(), unpacked->end());
    }
    const bool binary = bytes->starts_with("<roblox!");
    if (!binary && !bytes->starts_with("<roblox")) {
        std::println(stderr, "place {}: what came back isn't a place file", id);
        return 1;
    }

    // Through a temporary name, so a failed write leaves nothing half done.
    std::filesystem::path temporary = outPath;
    temporary += ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            std::println(stderr, "{}: can't write", outPath.string());
            return 1;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, outPath, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        std::println(stderr, "{}: can't write", outPath.string());
        return 1;
    }
    std::println("place {}: {} bytes ({}) -> {}", id, bytes->size(), binary ? "binary" : "XML", outPath.string());
    return 0;
}
