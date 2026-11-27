#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/ui/Font.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace lunar::ui;

namespace {

// The fonts ship with the Roblox client, which isn't part of the repository.
std::shared_ptr<FontFace> contentFont(const char* file)
{
    const auto path = lunar::assets::AssetConfig::fromEnvironment().contentDir / "fonts" / file;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        SKIP("no Roblox content folder with fonts");
    std::stringstream bytes;
    bytes << in.rdbuf();
    auto face = FontFace::load(bytes.str());
    CHECK(face.has_value());
    return *face;
}

int coveredPixels(const GlyphBitmap& bitmap)
{
    int n = 0;
    for (std::uint8_t c : bitmap.coverage)
        n += c > 127;
    return n;
}

} // namespace

TEST(font_utf8_round_trip)
{
    const std::u32string points = U"aé中\U0001F600";
    std::string text;
    for (char32_t cp : points)
        appendUtf8(text, cp);
    CHECK_EQ(text.size(), std::size_t{1 + 2 + 3 + 4});
    std::size_t pos = 0;
    for (char32_t cp : points)
        CHECK(decodeUtf8(text, pos) == cp);
    CHECK_EQ(pos, text.size());

    // Overlong, truncated and stray continuation bytes become U+FFFD one byte at a time.
    const std::string bad = "\xC0\xAF\xE4\xB8";
    pos = 0;
    CHECK(decodeUtf8(bad, pos) == U'�');
    CHECK_EQ(pos, std::size_t{1});
}

TEST(font_rejects_garbage)
{
    CHECK(!FontFace::load("").has_value());
    CHECK(!FontFace::load(std::string(64, 'x')).has_value());
}

TEST(font_rasterizes_a_square)
{
    // A 10x10 unit square at scale 1: 100 fully covered pixels, nothing partial.
    Outline square;
    square.moveTo(0, 0);
    square.lineTo(10, 0);
    square.lineTo(10, 10);
    square.lineTo(0, 10);
    const GlyphBitmap bitmap = rasterize(square, 1.0f);
    CHECK_EQ(bitmap.width, 10);
    CHECK_EQ(bitmap.height, 10);
    CHECK_EQ(bitmap.top, 10);
    for (std::uint8_t c : bitmap.coverage)
        CHECK_EQ(int{c}, 255);

    // Half a pixel to the right: the edge columns are half covered.
    const GlyphBitmap shifted = rasterize(square, 1.0f, 0.5f);
    CHECK_EQ(shifted.width, 11);
    CHECK_EQ(int{shifted.coverage[0]}, 128);
    CHECK_EQ(int{shifted.coverage[5]}, 255);
    CHECK_EQ(int{shifted.coverage[10]}, 128);

    // A hole wound the other way stays empty.
    Outline ring = square;
    ring.moveTo(3, 3);
    ring.lineTo(3, 7);
    ring.lineTo(7, 7);
    ring.lineTo(7, 3);
    const GlyphBitmap holed = rasterize(ring, 1.0f);
    CHECK_EQ(int{holed.coverage[5 * 10 + 5]}, 0);
    CHECK_EQ(int{holed.coverage[1 * 10 + 1]}, 255);
}

TEST(font_rasterizes_a_triangle_by_area)
{
    // Right triangle over a 4x4 box: total coverage is half the box.
    Outline tri;
    tri.moveTo(0, 0);
    tri.lineTo(4, 0);
    tri.lineTo(0, 4);
    const GlyphBitmap bitmap = rasterize(tri, 1.0f);
    int sum = 0;
    for (std::uint8_t c : bitmap.coverage)
        sum += c;
    CHECK(std::abs(sum - 8 * 255) <= 8);
}

// Values from the font files, cross-checked with fontTools.
TEST(font_source_sans_metrics)
{
    auto face = contentFont("SourceSansPro-Regular.ttf");
    CHECK(!face->isCff());
    CHECK_EQ(face->familyName(), std::string("Source Sans Pro"));
    CHECK_EQ(face->unitsPerEm(), 1000);
    CHECK_EQ(face->ascender(), 984);
    CHECK_EQ(face->descender(), -273);
    const std::uint16_t a = face->glyphIndex('A');
    CHECK_EQ(int{a}, 2);
    CHECK_EQ(face->advance(a), 544);
    CHECK_EQ(face->leftSideBearing(a), 3);
    const Outline::Bounds b = face->outline(a).controlBounds();
    CHECK_EQ(b.minX, 3.0f);
    CHECK_EQ(b.minY, 0.0f);
    CHECK_EQ(b.maxX, 541.0f);
    CHECK_EQ(b.maxY, 656.0f);
    CHECK_EQ(face->kerning(a, face->glyphIndex('V')), -14);
    CHECK_EQ(int{face->glyphIndex(0x10FFFD)}, 0);
}

TEST(font_builder_sans_cff_metrics)
{
    auto face = contentFont("BuilderSans-Regular.otf");
    CHECK(face->isCff());
    CHECK_EQ(face->ascender(), 980);
    CHECK_EQ(face->descender(), -280);
    const std::uint16_t a = face->glyphIndex('A');
    CHECK_EQ(int{a}, 6);
    CHECK_EQ(face->advance(a), 629);
    const Outline::Bounds b = face->outline(a).controlBounds();
    CHECK_EQ(b.minX, 12.0f);
    CHECK_EQ(b.maxX, 617.0f);
    CHECK_EQ(b.maxY, 700.0f);
    CHECK(face->kerning(a, face->glyphIndex('V')) < 0);

    // 'o' at 100 px: a ring, hollow in the middle.
    const GlyphBitmap o = rasterize(face->outline(face->glyphIndex('o')), 0.1f);
    CHECK(o.width > 30);
    CHECK(o.height > 30);
    CHECK_EQ(int{o.coverage[static_cast<std::size_t>(o.height / 2 * o.width + o.width / 2)]}, 0);
    CHECK(coveredPixels(o) > 300);
}

TEST(font_twemoji_color_layers)
{
    // Twemoji's emoji have no outlines of their own: only layers, each a glyph in a color.
    auto face = contentFont("TwemojiMozilla.ttf");
    CHECK(face->hasColor());
    const std::uint16_t eye = face->glyphIndex(U'\U0001F441');
    CHECK(eye != 0);
    CHECK(face->outline(eye).verbs.empty());
    const auto layers = face->colorLayers(eye);
    CHECK_EQ(layers.size(), std::size_t{5});
    for (const FontFace::ColorLayer& layer : layers) {
        CHECK(!layer.foreground);
        CHECK(layer.a > 0.0f);
        CHECK(!face->outline(layer.glyph).verbs.empty());
    }
    CHECK(face->colorLayers(layers[0].glyph).empty());

    auto plain = contentFont("Arimo-Regular.ttf");
    CHECK(!plain->hasColor());
    CHECK(plain->colorLayers(plain->glyphIndex('e')).empty());
}

TEST(font_twemoji_sequence_ligature)
{
    // The eye in a speech bubble, a ccmp ligature of five code points.
    auto face = contentFont("TwemojiMozilla.ttf");
    const std::vector<std::uint16_t> glyphs{face->glyphIndex(U'\U0001F441'), face->glyphIndex(0xFE0F),
                                            face->glyphIndex(0x200D), face->glyphIndex(U'\U0001F5E8'),
                                            face->glyphIndex(0xFE0F)};
    const auto ligature = face->ligature(glyphs);
    CHECK(ligature.has_value());
    CHECK_EQ(ligature->length, std::size_t{5});
    CHECK(!face->colorLayers(ligature->glyph).empty());
}

TEST(font_arimo_composite_glyphs)
{
    // Accented letters in Arimo are composites of a base and an accent.
    auto face = contentFont("Arimo-Regular.ttf");
    const Outline plain = face->outline(face->glyphIndex('e'));
    const Outline accented = face->outline(face->glyphIndex(U'é'));
    CHECK(accented.verbs.size() > plain.verbs.size());
    CHECK(accented.controlBounds().maxY > plain.controlBounds().maxY);
}
