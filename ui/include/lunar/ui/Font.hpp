#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lunar::ui {

// A glyph outline in font units, y up. Contours are closed implicitly.
struct Outline {
    enum class Verb : std::uint8_t { Move, Line, Quad, Cubic };
    struct Point {
        float x = 0.0f;
        float y = 0.0f;
    };
    std::vector<Verb> verbs;
    std::vector<Point> points; // Move/Line: 1 point, Quad: 2, Cubic: 3

    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadTo(float cx, float cy, float x, float y);
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    bool empty() const { return verbs.empty(); }

    struct Bounds {
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    };
    // Bounds of every point, control points included.
    Bounds controlBounds() const;
};

// One OpenType/TrueType font: either TrueType outlines (glyf/loca) or CFF
// (Type 2 charstrings, as in Builder Sans). Reads the tables needed to lay
// out and draw text: cmap (formats 4 and 12), hhea/hmtx metrics, OS/2,
// pair kerning from GPOS (PairPos formats 1 and 2) or the old kern table.
// Color glyphs from COLR version 0 and CPAL's first palette.
// No hinting, no shaping beyond kerning.
class FontFace {
public:
    // One layer of a color glyph: an ordinary glyph in one color.
    struct ColorLayer {
        std::uint16_t glyph = 0;
        float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
        bool foreground = false; // in the text's own color instead
    };

    static std::expected<std::shared_ptr<FontFace>, std::string> load(std::string bytes);

    // Vertical metrics in font units (hhea, which is what FreeType reports
    // as ascender/descender unless the font asks for typo metrics).
    int unitsPerEm() const { return unitsPerEm_; }
    int ascender() const { return ascender_; }
    int descender() const { return descender_; } // negative
    int lineGap() const { return lineGap_; }
    std::uint16_t glyphCount() const { return glyphCount_; }
    bool isCff() const { return cff_.charStrings != 0; }
    const std::string& familyName() const { return familyName_; }

    // 0 (.notdef) when the font has no glyph for the code point.
    std::uint16_t glyphIndex(char32_t codepoint) const;
    bool hasGlyph(char32_t codepoint) const { return glyphIndex(codepoint) != 0; }
    int advance(std::uint16_t glyph) const;
    int leftSideBearing(std::uint16_t glyph) const;
    // Horizontal adjustment between two glyphs, font units (usually <= 0).
    int kerning(std::uint16_t left, std::uint16_t right) const;
    // The ligature the font's standard ligatures ('liga', 'rlig', 'clig', 'ccmp':
    // GSUB ligature lookups) make of the glyphs from the first on: the glyph and
    // how many it stands for. Nothing when none starts there. (Roblox's
    // BuilderIcons font draws each icon as the ligature of its name, Twemoji
    // each emoji sequence.)
    struct Ligature {
        std::uint16_t glyph = 0;
        std::size_t length = 0;
    };
    std::optional<Ligature> ligature(std::span<const std::uint16_t> glyphs) const;
    Outline outline(std::uint16_t glyph) const;
    // Whether any glyph is drawn in color.
    bool hasColor() const { return !colorGlyphs_.empty(); }
    // A color glyph's layers, bottom first: it draws as these and not its
    // own outline (Twemoji's are empty). Nothing for any other glyph.
    std::span<const ColorLayer> colorLayers(std::uint16_t glyph) const;

    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

private:
    FontFace() = default;
    std::expected<void, std::string> parse();
    void parseCmap(std::uint32_t offset);
    void parseGpos(std::uint32_t offset);
    void parseGsub(std::uint32_t offset);
    void parseKern(std::uint32_t offset, std::uint32_t length);
    void parseColor(std::uint32_t colr, std::uint32_t cpal);
    std::expected<void, std::string> parseCff(std::uint32_t offset, std::uint32_t length);
    void glyfOutline(std::uint16_t glyph, Outline& out, float m[6], int depth) const;
    void cffOutline(std::uint16_t glyph, Outline& out) const;

    struct Table {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };
    std::span<const std::uint8_t> bytes() const
    {
        return {reinterpret_cast<const std::uint8_t*>(data_.data()), data_.size()};
    }

    std::string data_;
    std::unordered_map<std::uint32_t, Table> tables_;
    std::string familyName_;
    int unitsPerEm_ = 1000;
    int ascender_ = 0;
    int descender_ = 0;
    int lineGap_ = 0;
    std::uint16_t glyphCount_ = 0;
    std::uint16_t metricCount_ = 0;
    bool longLoca_ = false;
    std::uint32_t hmtx_ = 0;
    std::uint32_t loca_ = 0;
    std::uint32_t glyf_ = 0;
    std::uint32_t glyfLength_ = 0;

    // cmap: sorted ranges of code points mapping to glyphs.
    struct CmapRange {
        char32_t first = 0;
        char32_t last = 0;
        std::uint32_t glyph = 0; // glyph of `first` when delta, else index into glyphIds_
        bool delta = true;       // glyph = range.glyph + (cp - first), mod 65536
    };
    std::vector<CmapRange> cmap_;
    std::vector<std::uint16_t> glyphIds_;

    // Kerning, packed as (left << 16 | right) -> value.
    std::unordered_map<std::uint32_t, std::int16_t> pairKerning_;
    struct ClassKerning {
        std::vector<std::uint16_t> coverage; // sorted glyphs the subtable applies to (as first)
        std::unordered_map<std::uint16_t, std::uint16_t> class1;
        std::unordered_map<std::uint16_t, std::uint16_t> class2;
        std::uint16_t class2Count = 0;
        std::vector<std::int16_t> values; // class1 * class2Count + class2
    };
    std::vector<ClassKerning> classKerning_;
    // By first glyph, the ligatures starting with it in lookup and
    // preference order: the rest of their glyphs and what they become.
    struct LigatureRule {
        std::vector<std::uint16_t> rest;
        std::uint16_t glyph = 0;
    };
    std::unordered_map<std::uint16_t, std::vector<LigatureRule>> ligatures_;

    // Each color glyph's first layer in colorLayers_ and how many it has.
    struct ColorGlyph {
        std::uint16_t first = 0;
        std::uint16_t count = 0;
    };
    std::unordered_map<std::uint16_t, ColorGlyph> colorGlyphs_;
    std::vector<ColorLayer> colorLayers_;

    struct Cff {
        std::uint32_t charStrings = 0; // absolute offsets of INDEXes
        std::uint32_t globalSubrs = 0;
        std::uint32_t localSubrs = 0; // non-CID fonts
        std::uint32_t fdSelect = 0;   // CID fonts
        std::vector<std::uint32_t> fdLocalSubrs;
    };
    Cff cff_;
};

// A glyph's coverage bitmap: 0 = outside, 255 = fully covered. left/top
// place it relative to the pen position on the baseline (top is how far
// the first row is above the baseline, in pixels, y down after that).
struct GlyphBitmap {
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    std::vector<std::uint8_t> coverage; // width * height, rows top to bottom
};

// Rasterizes an outline scaled by `scale` pixels per font unit, shifted
// right by `offsetX` pixels (0 <= offsetX < 1, for subpixel positioning).
// Exact area coverage with the non-zero rule for non-overlapping contours.
GlyphBitmap rasterize(const Outline& outline, float scale, float offsetX = 0.0f);

// Decodes one UTF-8 code point at `pos`, advancing it. Malformed bytes come
// back as U+FFFD, one byte at a time.
char32_t decodeUtf8(std::string_view text, std::size_t& pos);
void appendUtf8(std::string& out, char32_t codepoint);

} // namespace lunar::ui
