#pragma once

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/ui/Font.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lunar::ui {

struct LayoutCache; // TextLayout.cpp's

// One row of the client's Enum.Font table: what Font.fromEnum gives and a
// size factor applied to TextSize. From RobloxPlayer 0.742, the 56-byte
// records at 0x1072ec6f8 {enum, name, family, weight | style << 32, scale}.
struct EnumFontInfo {
    Enum::Font font;
    std::string_view name;
    std::string_view family; // rbxasset://fonts/families/*.json
    Enum::FontWeight weight;
    Enum::FontStyle style;
    float sizeScale; // 1.5 for Legacy, 1 for everything else
};

// nullopt for Enum.Font.Unknown and values outside the table.
std::optional<EnumFontInfo> enumFontInfo(Enum::Font font);
lunar::Font fontFromEnum(Enum::Font font);
// The size factor for a family asset: Legacy's 1.5 for LegacyArial/LegacyArimo, else 1.
float familySizeScale(std::string_view family);

// The client swaps some family assets for others before loading them
// (0.742, sub_102e3c4b0's content redirect table): Gotham to Montserrat,
// Arial to Arimo, Legacy Arial to Legacy Arimo. Others come back unchanged.
std::string_view redirectFamily(std::string_view family);

// A family JSON (rbxasset://fonts/families/*.json): named faces, each a
// weight, style and font asset.
struct FontFamily {
    struct Face {
        std::string name;
        int weight = 400;
        bool italic = false;
        std::string assetId;
    };
    std::string name;
    std::vector<Face> faces;
};
std::optional<FontFamily> parseFontFamily(std::string_view text);

// Loads and caches the fonts in the client's content folder. Faces resolve
// by family, then the closest weight among faces whose font file is in the
// content folder (the same style preferred), the way CSS matching works;
// faces only available as rbxassetid downloads aren't loaded. Missing
// glyphs fall back through Arimo and the Noto fonts the client lists after
// it (0.742, sub_1013c9aa6). Thread-safe.
class FontLibrary {
public:
    explicit FontLibrary(std::filesystem::path contentDir);

    // Null when nothing of the family can be loaded (and no fallback either).
    const FontFace* face(const lunar::Font& font);
    const FontFace* face(Enum::Font font) { return face(fontFromEnum(font)); }

    struct Glyph {
        const FontFace* face = nullptr;
        std::uint16_t index = 0;
    };
    // The primary face's glyph, else the first fallback that has one, else
    // the primary's .notdef.
    Glyph glyph(const FontFace* primary, char32_t codepoint);
    // The code point's glyph in the first fallback with color glyphs, for
    // emoji sequences; no face when none has it.
    Glyph colorGlyph(char32_t codepoint);

    const std::filesystem::path& contentDir() const { return contentDir_; }
    // Where layoutText keeps what it laid out with these fonts.
    std::shared_ptr<LayoutCache>& layoutCache() { return layoutCache_; }

private:
    const FontFace* loadFile(const std::string& assetId);
    const FontFamily* family(std::string_view uri);
    const std::vector<const FontFace*>& fallbacks();

    std::filesystem::path contentDir_;
    std::recursive_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<FontFace>> files_; // by asset id, null if it failed
    std::unordered_map<std::string, std::optional<FontFamily>> families_;
    std::unordered_map<std::string, const FontFace*> resolved_;
    std::vector<const FontFace*> fallbacks_;
    bool fallbacksLoaded_ = false;
    std::shared_ptr<LayoutCache> layoutCache_;
};

} // namespace lunar::ui
