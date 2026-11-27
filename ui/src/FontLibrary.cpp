#include <lunar/ui/FontLibrary.hpp>

#include "Json.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

#if __cpp_impl_reflection
#include <meta>
#endif

namespace lunar::ui {

namespace {

using W = Enum::FontWeight;
using S = Enum::FontStyle;
using F = Enum::Font;

constexpr std::string_view families = "rbxasset://fonts/families/";

// Enum.Font -> Font.fromEnum, copied from the client's table (see
// EnumFontInfo). Only Legacy has a size scale other than 1.
constexpr auto enumFonts = std::to_array<EnumFontInfo>({
    {F::Legacy, "Legacy", "rbxasset://fonts/families/LegacyArial.json", W::Regular, S::Normal, 1.5f},
    {F::Arial, "Arial", "rbxasset://fonts/families/Arial.json", W::Regular, S::Normal, 1.0f},
    {F::ArialBold, "ArialBold", "rbxasset://fonts/families/Arial.json", W::Bold, S::Normal, 1.0f},
    {F::SourceSans, "SourceSans", "rbxasset://fonts/families/SourceSansPro.json", W::Regular, S::Normal, 1.0f},
    {F::SourceSansBold, "SourceSansBold", "rbxasset://fonts/families/SourceSansPro.json", W::Bold, S::Normal, 1.0f},
    {F::SourceSansLight, "SourceSansLight", "rbxasset://fonts/families/SourceSansPro.json", W::Light, S::Normal, 1.0f},
    {F::SourceSansItalic, "SourceSansItalic", "rbxasset://fonts/families/SourceSansPro.json", W::Regular, S::Italic,
     1.0f},
    {F::Bodoni, "Bodoni", "rbxasset://fonts/families/AccanthisADFStd.json", W::Regular, S::Normal, 1.0f},
    {F::Garamond, "Garamond", "rbxasset://fonts/families/Guru.json", W::Regular, S::Normal, 1.0f},
    {F::Cartoon, "Cartoon", "rbxasset://fonts/families/ComicNeueAngular.json", W::Regular, S::Normal, 1.0f},
    {F::Code, "Code", "rbxasset://fonts/families/Inconsolata.json", W::Regular, S::Normal, 1.0f},
    {F::Highway, "Highway", "rbxasset://fonts/families/HighwayGothic.json", W::Regular, S::Normal, 1.0f},
    {F::SciFi, "SciFi", "rbxasset://fonts/families/Zekton.json", W::Regular, S::Normal, 1.0f},
    {F::Arcade, "Arcade", "rbxasset://fonts/families/PressStart2P.json", W::Regular, S::Normal, 1.0f},
    {F::Fantasy, "Fantasy", "rbxasset://fonts/families/Balthazar.json", W::Regular, S::Normal, 1.0f},
    {F::Antique, "Antique", "rbxasset://fonts/families/RomanAntique.json", W::Regular, S::Normal, 1.0f},
    {F::SourceSansSemibold, "SourceSansSemibold", "rbxasset://fonts/families/SourceSansPro.json", W::SemiBold,
     S::Normal, 1.0f},
    {F::Gotham, "Gotham", "rbxasset://fonts/families/GothamSSm.json", W::Regular, S::Normal, 1.0f},
    {F::GothamMedium, "GothamMedium", "rbxasset://fonts/families/GothamSSm.json", W::Medium, S::Normal, 1.0f},
    {F::GothamBold, "GothamBold", "rbxasset://fonts/families/GothamSSm.json", W::Bold, S::Normal, 1.0f},
    {F::GothamBlack, "GothamBlack", "rbxasset://fonts/families/GothamSSm.json", W::Heavy, S::Normal, 1.0f},
    {F::AmaticSC, "AmaticSC", "rbxasset://fonts/families/AmaticSC.json", W::Regular, S::Normal, 1.0f},
    {F::Bangers, "Bangers", "rbxasset://fonts/families/Bangers.json", W::Regular, S::Normal, 1.0f},
    {F::Creepster, "Creepster", "rbxasset://fonts/families/Creepster.json", W::Regular, S::Normal, 1.0f},
    {F::DenkOne, "DenkOne", "rbxasset://fonts/families/DenkOne.json", W::Regular, S::Normal, 1.0f},
    {F::Fondamento, "Fondamento", "rbxasset://fonts/families/Fondamento.json", W::Regular, S::Normal, 1.0f},
    {F::FredokaOne, "FredokaOne", "rbxasset://fonts/families/FredokaOne.json", W::Regular, S::Normal, 1.0f},
    {F::GrenzeGotisch, "GrenzeGotisch", "rbxasset://fonts/families/GrenzeGotisch.json", W::Regular, S::Normal, 1.0f},
    {F::IndieFlower, "IndieFlower", "rbxasset://fonts/families/IndieFlower.json", W::Regular, S::Normal, 1.0f},
    {F::JosefinSans, "JosefinSans", "rbxasset://fonts/families/JosefinSans.json", W::Regular, S::Normal, 1.0f},
    {F::Jura, "Jura", "rbxasset://fonts/families/Jura.json", W::Regular, S::Normal, 1.0f},
    {F::Kalam, "Kalam", "rbxasset://fonts/families/Kalam.json", W::Regular, S::Normal, 1.0f},
    {F::LuckiestGuy, "LuckiestGuy", "rbxasset://fonts/families/LuckiestGuy.json", W::Regular, S::Normal, 1.0f},
    {F::Merriweather, "Merriweather", "rbxasset://fonts/families/Merriweather.json", W::Regular, S::Normal, 1.0f},
    {F::Michroma, "Michroma", "rbxasset://fonts/families/Michroma.json", W::Regular, S::Normal, 1.0f},
    {F::Nunito, "Nunito", "rbxasset://fonts/families/Nunito.json", W::Regular, S::Normal, 1.0f},
    {F::Oswald, "Oswald", "rbxasset://fonts/families/Oswald.json", W::Regular, S::Normal, 1.0f},
    {F::PatrickHand, "PatrickHand", "rbxasset://fonts/families/PatrickHand.json", W::Regular, S::Normal, 1.0f},
    {F::PermanentMarker, "PermanentMarker", "rbxasset://fonts/families/PermanentMarker.json", W::Regular, S::Normal,
     1.0f},
    {F::Roboto, "Roboto", "rbxasset://fonts/families/Roboto.json", W::Regular, S::Normal, 1.0f},
    {F::RobotoCondensed, "RobotoCondensed", "rbxasset://fonts/families/RobotoCondensed.json", W::Regular, S::Normal,
     1.0f},
    {F::RobotoMono, "RobotoMono", "rbxasset://fonts/families/RobotoMono.json", W::Regular, S::Normal, 1.0f},
    {F::Sarpanch, "Sarpanch", "rbxasset://fonts/families/Sarpanch.json", W::Regular, S::Normal, 1.0f},
    {F::SpecialElite, "SpecialElite", "rbxasset://fonts/families/SpecialElite.json", W::Regular, S::Normal, 1.0f},
    {F::TitilliumWeb, "TitilliumWeb", "rbxasset://fonts/families/TitilliumWeb.json", W::Regular, S::Normal, 1.0f},
    {F::Ubuntu, "Ubuntu", "rbxasset://fonts/families/Ubuntu.json", W::Regular, S::Normal, 1.0f},
    {F::BuilderSans, "BuilderSans", "rbxasset://fonts/families/BuilderSans.json", W::Regular, S::Normal, 1.0f},
    {F::BuilderSansMedium, "BuilderSansMedium", "rbxasset://fonts/families/BuilderSans.json", W::Medium, S::Normal,
     1.0f},
    {F::BuilderSansBold, "BuilderSansBold", "rbxasset://fonts/families/BuilderSans.json", W::Bold, S::Normal, 1.0f},
    {F::BuilderSansExtraBold, "BuilderSansExtraBold", "rbxasset://fonts/families/BuilderSans.json", W::ExtraBold,
     S::Normal, 1.0f},
    {F::Arimo, "Arimo", "rbxasset://fonts/families/Arimo.json", W::Regular, S::Normal, 1.0f},
    {F::ArimoBold, "ArimoBold", "rbxasset://fonts/families/Arimo.json", W::Bold, S::Normal, 1.0f},
});

// The table is indexed by the enum value, and its names are the enum's.
consteval bool tableMatchesEnum()
{
    for (std::size_t i = 0; i < enumFonts.size(); ++i)
        if (static_cast<std::size_t>(enumFonts[i].font) != i)
            return false;
#if __cpp_impl_reflection
    for (std::meta::info enumerator : std::meta::enumerators_of(^^Enum::Font)) {
        const auto value = static_cast<std::size_t>(std::meta::extract<Enum::Font>(enumerator));
        if (value < enumFonts.size() && enumFonts[value].name != std::meta::identifier_of(enumerator))
            return false;
    }
#endif
    return true;
}
static_assert(tableMatchesEnum());

std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::stringstream bytes;
    bytes << in.rdbuf();
    return std::move(bytes).str();
}

// An rbxasset:// path under the client's content folder, else where the
// client also looks: PlatformContent/pc and ExtraContent beside it (the Lua
// app's BuilderIcons font is in ExtraContent/LuaPackages).
std::optional<std::string> readContent(const std::filesystem::path& contentDir, std::string_view relative)
{
    if (auto bytes = readFile(contentDir / relative))
        return bytes;
    for (const char* overlay : {"PlatformContent/pc", "ExtraContent"})
        if (auto bytes = readFile(contentDir.parent_path() / overlay / relative))
            return bytes;
    return std::nullopt;
}

std::string lower(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    return out;
}

} // namespace

std::optional<EnumFontInfo> enumFontInfo(Enum::Font font)
{
    const auto index = static_cast<std::size_t>(font);
    if (index >= enumFonts.size())
        return std::nullopt;
    return enumFonts[index];
}

lunar::Font fontFromEnum(Enum::Font font)
{
    if (auto info = enumFontInfo(font))
        return {std::string(info->family), info->weight, info->style};
    return {};
}

float familySizeScale(std::string_view family)
{
    const std::string name = lower(family);
    return name.ends_with("/legacyarial.json") || name.ends_with("/legacyarimo.json") ? 1.5f : 1.0f;
}

std::string_view redirectFamily(std::string_view family)
{
    static constexpr std::pair<std::string_view, std::string_view> redirects[] = {
        {"rbxasset://fonts/families/GothamSSm.json", "rbxasset://fonts/families/Montserrat.json"},
        {"rbxassetid://11702779517", "rbxasset://fonts/families/Montserrat.json"},
        {"rbxasset://fonts/families/Arial.json", "rbxasset://fonts/families/Arimo.json"},
        {"rbxasset://fonts/families/LegacyArial.json", "rbxasset://fonts/families/LegacyArimo.json"},
        {"rbxassetid://2229763439", "rbxasset://fonts/families/Arimo.json"},
    };
    for (const auto& [from, to] : redirects)
        if (family == from)
            return to;
    return family;
}

std::optional<FontFamily> parseFontFamily(std::string_view text)
{
    auto doc = json::parse(text);
    if (!doc)
        return std::nullopt;
    FontFamily family;
    if (const auto* name = (*doc)["name"]; name && name->string())
        family.name = *name->string();
    const auto* faces = (*doc)["faces"];
    if (!faces || !faces->array())
        return std::nullopt;
    for (const json::Value& entry : *faces->array()) {
        FontFamily::Face face;
        const auto* asset = entry["assetId"];
        if (!asset || !asset->string())
            continue;
        face.assetId = *asset->string();
        if (const auto* name = entry["name"]; name && name->string())
            face.name = *name->string();
        if (const auto* weight = entry["weight"])
            if (const auto number = weight->number())
                face.weight = static_cast<int>(*number);
        if (const auto* style = entry["style"]; style && style->string())
            face.italic = *style->string() == "italic";
        family.faces.push_back(std::move(face));
    }
    return family;
}

FontLibrary::FontLibrary(std::filesystem::path contentDir) : contentDir_(std::move(contentDir)) {}

const FontFace* FontLibrary::loadFile(const std::string& assetId)
{
    if (auto it = files_.find(assetId); it != files_.end())
        return it->second.get();
    std::shared_ptr<FontFace> face;
    constexpr std::string_view builtin = "rbxasset://";
    if (assetId.starts_with(builtin)) {
        if (auto bytes = readContent(contentDir_, assetId.substr(builtin.size())))
            if (auto loaded = FontFace::load(std::move(*bytes)))
                face = std::move(*loaded);
    }
    return files_.emplace(assetId, std::move(face)).first->second.get();
}

const FontFamily* FontLibrary::family(std::string_view uri)
{
    const std::string key(redirectFamily(uri));
    if (auto it = families_.find(key); it != families_.end())
        return it->second ? &*it->second : nullptr;
    std::optional<FontFamily> parsed;
    if (key.starts_with("rbxasset://"))
        if (auto bytes = readContent(contentDir_, std::string_view(key).substr(std::string_view("rbxasset://").size())))
            parsed = parseFontFamily(*bytes);
    auto& slot = families_[key];
    slot = std::move(parsed);
    return slot ? &*slot : nullptr;
}

const FontFace* FontLibrary::face(const lunar::Font& font)
{
    std::scoped_lock lock(mutex_);
    const std::string key
        = std::format("{}|{}|{}", font.family, static_cast<int>(font.weight), static_cast<int>(font.style));
    if (auto it = resolved_.find(key); it != resolved_.end())
        return it->second;

    const FontFace* result = nullptr;
    if (const FontFamily* fam = family(font.family)) {
        const int weight = static_cast<int>(font.weight);
        const bool italic = font.style == Enum::FontStyle::Italic;
        // Loadable faces, best first: same style, then nearest weight, heavier on ties.
        std::vector<const FontFamily::Face*> candidates;
        for (const auto& f : fam->faces)
            if (f.assetId.starts_with("rbxasset://"))
                candidates.push_back(&f);
        std::ranges::stable_sort(candidates, {}, [&](const FontFamily::Face* f) {
            return std::tuple(f->italic != italic, std::abs(f->weight - weight), f->weight < weight);
        });
        for (const FontFamily::Face* candidate : candidates) {
            result = loadFile(candidate->assetId);
            if (result)
                break;
        }
    }
    if (!result && !fallbacks().empty())
        result = fallbacks().front();
    resolved_.emplace(key, result);
    return result;
}

const std::vector<const FontFace*>& FontLibrary::fallbacks()
{
    if (!fallbacksLoaded_) {
        fallbacksLoaded_ = true;
        // In the client's order; the CJK family is download-only.
        static constexpr std::string_view files[] = {
            "rbxasset://fonts/Arimo-Regular.ttf",
            "rbxasset://fonts/NotoSansThaiUI-Regular.ttf",
            "rbxasset://fonts/NotoSansSinhalaUI-Regular.ttf",
            "rbxasset://fonts/NotoSansMyanmarUI-Regular.ttf",
            "rbxasset://fonts/NotoSansKhmerUI-Regular.ttf",
            "rbxasset://fonts/NotoSansGeorgian-Regular.ttf",
            "rbxasset://fonts/NotoSansDevanagariUI-Regular.ttf",
            "rbxasset://fonts/NotoSansBengaliUI-Regular.ttf",
            "rbxasset://fonts/NotoNaskhArabicUI-Regular.ttf",
            "rbxasset://fonts/TwemojiMozilla.ttf",
        };
        for (std::string_view file : files)
            if (const FontFace* f = loadFile(std::string(file)))
                fallbacks_.push_back(f);
    }
    return fallbacks_;
}

FontLibrary::Glyph FontLibrary::glyph(const FontFace* primary, char32_t codepoint)
{
    if (primary)
        if (const std::uint16_t g = primary->glyphIndex(codepoint))
            return {primary, g};
    std::scoped_lock lock(mutex_);
    for (const FontFace* f : fallbacks())
        if (const std::uint16_t g = f->glyphIndex(codepoint))
            return {f, g};
    return {primary, 0};
}

FontLibrary::Glyph FontLibrary::colorGlyph(char32_t codepoint)
{
    std::scoped_lock lock(mutex_);
    for (const FontFace* f : fallbacks())
        if (f->hasColor())
            if (const std::uint16_t g = f->glyphIndex(codepoint))
                return {f, g};
    return {};
}

} // namespace lunar::ui
