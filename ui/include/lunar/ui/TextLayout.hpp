#pragma once

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/Math.hpp>
#include <lunar/ui/FontLibrary.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::ui {

// How a stretch of text looks. Rich text changes these per run.
struct TextStyle {
    lunar::Font font;
    float size = 14.0f; // TextSize, before the family's size scale
    Color3 color;
    float transparency = 0.0f;
    bool underline = false;
    bool strikethrough = false;
    bool uppercase = false;
    std::optional<Color3> strokeColor; // rich text <stroke>; else the object's TextStroke
    float strokeTransparency = 1.0f;
    float strokeThickness = 1.0f;

    friend bool operator==(const TextStyle&, const TextStyle&) = default;
};

struct TextRun {
    std::string text;
    TextStyle style;
};

// Parses Roblox rich text markup into runs: <b>, <i>, <u>, <s>, <br/>,
// <uppercase>/<uc>, <font color size face family weight transparency>,
// <stroke color thickness transparency>, comments and the five escapes.
// Unknown or malformed tags are kept as literal text, like Roblox does.
std::vector<TextRun> parseRichText(std::string_view text, const TextStyle& base);

struct TextParams {
    std::string_view text;
    TextStyle style;
    float lineHeight = 1.0f;
    bool wrapped = false;
    bool scaled = false; // TextScaled: biggest size that fits, wrapping as needed
    bool richText = false;
    Enum::TextXAlignment xAlignment = Enum::TextXAlignment::Center;
    Enum::TextYAlignment yAlignment = Enum::TextYAlignment::Center;
    Enum::TextTruncate truncate = Enum::TextTruncate::None;
    Vector2 box;                  // the text's area, pixels
    int maxVisibleGraphemes = -1; // -1 shows everything
    int minScaledSize = 1;        // UITextSizeConstraint for TextScaled
    int maxScaledSize = 100;
    // A member added here is one for TextLayout.cpp's sameButText too.

    friend bool operator==(const TextParams&, const TextParams&) = default;
};

// A glyph placed in the box: (x, y) is the pen position on the baseline,
// pixels from the box's top-left corner.
struct PlacedGlyph {
    const FontFace* face = nullptr;
    std::uint16_t glyph = 0;
    float x = 0.0f;
    float y = 0.0f;
    float pixelsPerUnit = 0.0f; // scale from font units
    std::uint32_t run = 0;      // index into TextLayout::styles
    std::uint32_t source = 0;   // byte offset of the character in the text after markup
};

struct TextDecoration {
    float x0 = 0.0f, x1 = 0.0f, y = 0.0f, thickness = 1.0f;
    std::uint32_t run = 0;
};

struct TextLine {
    std::uint32_t firstGlyph = 0;
    std::uint32_t glyphCount = 0;
    float x = 0.0f;        // left edge after alignment
    float top = 0.0f;      // of the line box
    float baseline = 0.0f; // from the box top
    float width = 0.0f;    // without trailing spaces
    float height = 0.0f;
};

struct TextLayout {
    std::vector<PlacedGlyph> glyphs;
    std::vector<TextDecoration> decorations;
    std::vector<TextLine> lines;
    std::vector<TextStyle> styles; // per run
    std::string plain;             // the text without markup, what PlacedGlyph::source indexes
    Vector2 bounds;                // TextBounds
    float textSize = 0.0f;         // the size used (TextScaled picks its own)
    bool fits = true;              // TextFits
};

TextLayout layoutText(FontLibrary& fonts, const TextParams& params);
// The same, shared with the fonts' layout cache rather than copied out of
// it: the GUI lays out the same texts every frame.
std::shared_ptr<const TextLayout> layoutTextShared(FontLibrary& fonts, const TextParams& params);

// TextService:GetTextSize: the bounds of text at a size, wrapped at width
// (0 for no limit).
Vector2 measureText(FontLibrary& fonts, std::string_view text, const lunar::Font& font, float size, float width = 0.0f);

// Pixels per font unit for a face at a TextSize. Roblox's TextSize is the
// height of a line, so the font's ascender-to-descender span is scaled to
// it (guess from TextBounds.Y == TextSize for one line; the client's
// FreeType setup isn't traced yet).
float pixelsPerUnit(const FontFace& face, float effectiveSize);

} // namespace lunar::ui
