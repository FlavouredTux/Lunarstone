#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/TextLayout.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::ui;

namespace {

FontLibrary& fonts()
{
    static FontLibrary library(assets::AssetConfig::fromEnvironment().contentDir);
    if (!std::filesystem::exists(library.contentDir() / "fonts/families/SourceSansPro.json"))
        SKIP("no Roblox content folder with fonts");
    return library;
}

TextParams params(std::string_view text, float size, Vector2 box)
{
    TextParams p;
    p.text = text;
    p.style.font = fontFromEnum(Enum::Font::SourceSans);
    p.style.size = size;
    p.box = box;
    p.xAlignment = Enum::TextXAlignment::Left;
    p.yAlignment = Enum::TextYAlignment::Top;
    return p;
}

EnumFontInfo info(Enum::Font font)
{
    const auto found = enumFontInfo(font);
    if (!found)
        throw lt::Failure{"no table entry"};
    return *found;
}

} // namespace

// Pinned against the client's table at 0x1072ec6f8.
TEST(text_enum_font_table)
{
    const auto legacy = info(Enum::Font::Legacy);
    CHECK_EQ(legacy.family, std::string_view("rbxasset://fonts/families/LegacyArial.json"));
    CHECK_EQ(legacy.sizeScale, 1.5f);
    const auto gothamBlack = info(Enum::Font::GothamBlack);
    CHECK_EQ(gothamBlack.family, std::string_view("rbxasset://fonts/families/GothamSSm.json"));
    CHECK(gothamBlack.weight == Enum::FontWeight::Heavy);
    const auto italic = info(Enum::Font::SourceSansItalic);
    CHECK(italic.style == Enum::FontStyle::Italic);
    CHECK(info(Enum::Font::BuilderSansExtraBold).weight == Enum::FontWeight::ExtraBold);
    CHECK(!enumFontInfo(Enum::Font::Unknown).has_value());
    CHECK_EQ(redirectFamily("rbxasset://fonts/families/GothamSSm.json"),
             std::string_view("rbxasset://fonts/families/Montserrat.json"));
    CHECK_EQ(redirectFamily("rbxasset://fonts/families/Arial.json"),
             std::string_view("rbxasset://fonts/families/Arimo.json"));
    CHECK_EQ(familySizeScale("rbxasset://fonts/families/LegacyArial.json"), 1.5f);
    CHECK_EQ(familySizeScale("rbxasset://fonts/families/SourceSansPro.json"), 1.0f);
}

TEST(text_emoji_sequence_is_one_color_glyph)
{
    const TextLayout layout = layoutText(fonts(), params("\U0001F441\uFE0F\u200D\U0001F5E8\uFE0F x", 20, {400, 40}));
    CHECK_EQ(layout.glyphs.size(), std::size_t{2}); // the space places none
    const PlacedGlyph& emoji = layout.glyphs[0];
    CHECK(!emoji.face->colorLayers(emoji.glyph).empty());
    CHECK(!layout.glyphs[1].face->hasColor());
    // One emoji and a space wide, about the text's size.
    CHECK(layout.glyphs[1].x > 20.0f && layout.glyphs[1].x < 32.0f);
}

TEST(text_keycap_and_selector_emoji)
{
    // The digit is in the text's own font too, the sword may be: U+FE0F asks for the emoji.
    const TextLayout keycap = layoutText(fonts(), params("1\uFE0F\u20E3", 20, {400, 40}));
    CHECK_EQ(keycap.glyphs.size(), std::size_t{1});
    CHECK(!keycap.glyphs[0].face->colorLayers(keycap.glyphs[0].glyph).empty());

    const TextLayout sword = layoutText(fonts(), params("\u2694\uFE0F", 20, {400, 40}));
    CHECK_EQ(sword.glyphs.size(), std::size_t{1});
    CHECK(!sword.glyphs[0].face->colorLayers(sword.glyphs[0].glyph).empty());

    // A plain digit stays plain.
    const TextLayout digit = layoutText(fonts(), params("1", 20, {400, 40}));
    CHECK(!digit.glyphs[0].face->hasColor());
}

TEST(text_joiners_and_selectors_are_never_drawn)
{
    const TextLayout plain = layoutText(fonts(), params("ab", 20, {400, 40}));
    for (const char* text : {"a\u200Db", "a\u200Cb", "a\uFE0Fb", "a\uFE0Eb"}) {
        const TextLayout layout = layoutText(fonts(), params(text, 20, {400, 40}));
        CHECK_EQ(layout.glyphs.size(), std::size_t{2});
        CHECK_EQ(layout.glyphs[1].x, plain.glyphs[1].x);
    }
    // Two emoji the font has no sequence for stay two, without the joiner.
    const TextLayout two = layoutText(fonts(), params("\U0001F600\u200D\U0001F600", 20, {400, 40}));
    CHECK_EQ(two.glyphs.size(), std::size_t{2});
    CHECK(two.glyphs[1].face->hasColor());
}

TEST(text_family_json)
{
    auto family = parseFontFamily(R"({"name": "X", "faces": [
        {"name": "Regular", "weight": 400, "style": "normal", "assetId": "rbxasset://fonts/X.ttf"},
        {"name": "Bold Italic", "weight": 700, "style": "italic", "assetId": "rbxassetid://5"}]})");
    CHECK(family.has_value());
    CHECK_EQ(family->name, std::string("X"));
    CHECK_EQ(family->faces.size(), std::size_t{2});
    CHECK_EQ(family->faces[1].weight, 700);
    CHECK(family->faces[1].italic);
    CHECK(!parseFontFamily("{\"faces\": 3}").has_value());
    CHECK(!parseFontFamily("not json").has_value());
}

TEST(text_rich_text_markup)
{
    TextStyle base;
    base.color = Color3{1, 1, 1};
    auto runs = parseRichText("a <b>bold</b> <font color=\"#FF0000\" size='20'>red</font>&lt;3<br/>x", base);
    CHECK_EQ(runs.size(), std::size_t{5});
    CHECK_EQ(runs[0].text, std::string("a "));
    CHECK_EQ(runs[1].text, std::string("bold"));
    CHECK(runs[1].style.font.weight == Enum::FontWeight::Bold);
    CHECK_EQ(runs[3].text, std::string("red"));
    CHECK(runs[3].style.color == Color3::fromRGB(255, 0, 0));
    CHECK_EQ(runs[3].style.size, 20.0f);
    CHECK_EQ(runs[4].text, std::string("<3\nx"));

    // Unknown and unbalanced tags stay as text.
    auto literal = parseRichText("<what>x</b>", base);
    CHECK_EQ(literal.size(), std::size_t{1});
    CHECK_EQ(literal[0].text, std::string("<what>x</b>"));

    auto rgb = parseRichText("<font color=\"rgb(0, 128, 255)\" weight=\"heavy\">c</font>", base);
    CHECK(rgb[0].style.color == Color3::fromRGB(0, 128, 255));
    CHECK(rgb[0].style.font.weight == Enum::FontWeight::Heavy);
    auto comment = parseRichText("a<!-- hidden -->b", base);
    CHECK_EQ(comment[0].text, std::string("ab"));
}

TEST(text_line_height_is_text_size)
{
    auto& lib = fonts();
    // One line is TextSize tall, two lines twice that.
    CHECK_EQ(measureText(lib, "Hello", fontFromEnum(Enum::Font::SourceSans), 18).y, 18.0f);
    CHECK_EQ(measureText(lib, "Hello\nWorld", fontFromEnum(Enum::Font::SourceSans), 18).y, 36.0f);
    // Legacy's 1.5 scale applies to the line height too.
    CHECK_EQ(measureText(lib, "Hello", fontFromEnum(Enum::Font::Legacy), 10).y, 15.0f);
    // Width: advances of "AV" at 18 px with Source Sans' kerning, per the font tables.
    const FontFace* face = lib.face(Enum::Font::SourceSans);
    CHECK(face != nullptr);
    const float s = 18.0f / static_cast<float>(984 + 273);
    const float expected = static_cast<float>(544 + 544 - 14) * s; // A and V share an advance in Source Sans
    const float width = measureText(lib, "AV", fontFromEnum(Enum::Font::SourceSans), 18).x;
    CHECK(std::abs(width - expected) < 0.75f);
}

TEST(text_wrapping_breaks_at_spaces)
{
    auto& lib = fonts();
    TextParams p = params("the quick brown fox jumps over the lazy dog", 18, {120, 200});
    p.wrapped = true;
    const TextLayout layout = layoutText(lib, p);
    CHECK(layout.lines.size() > 2);
    for (const TextLine& line : layout.lines)
        CHECK(line.width <= 120.0f);
    CHECK_EQ(layout.bounds.y, 18.0f * static_cast<float>(layout.lines.size()));
    // Lines start on words: the first glyph of each line follows a space in the source.
    for (std::size_t l = 1; l < layout.lines.size(); ++l) {
        const PlacedGlyph& first = layout.glyphs[layout.lines[l].firstGlyph];
        CHECK_EQ(layout.plain[first.source - 1], ' ');
        CHECK_EQ(first.x, 0.0f);
    }

    // Unwrapped, it's one line wider than the box and doesn't fit.
    p.wrapped = false;
    const TextLayout single = layoutText(lib, p);
    CHECK_EQ(single.lines.size(), std::size_t{1});
    CHECK(!single.fits);
}

TEST(text_alignment)
{
    auto& lib = fonts();
    TextParams p = params("Hi", 20, {200, 100});
    p.xAlignment = Enum::TextXAlignment::Right;
    p.yAlignment = Enum::TextYAlignment::Bottom;
    const TextLayout right = layoutText(lib, p);
    CHECK(std::abs(right.lines[0].x + right.lines[0].width - 200.0f) < 0.01f);
    CHECK_EQ(right.lines[0].top, 80.0f);
    p.xAlignment = Enum::TextXAlignment::Center;
    p.yAlignment = Enum::TextYAlignment::Center;
    const TextLayout center = layoutText(lib, p);
    CHECK(std::abs(center.lines[0].x * 2 + center.lines[0].width - 200.0f) < 0.01f);
    CHECK_EQ(center.lines[0].top, 40.0f);
}

TEST(text_scaled_fits_the_box)
{
    auto& lib = fonts();
    TextParams p = params("Scaled", 14, {300, 50});
    p.scaled = true;
    const TextLayout layout = layoutText(lib, p);
    CHECK(layout.fits);
    CHECK(layout.textSize >= 40.0f); // limited by the height of 50
    CHECK(layout.textSize <= 50.0f);
    // One more pixel would not fit.
    TextParams bigger = p;
    bigger.scaled = false;
    bigger.wrapped = true;
    bigger.style.size = layout.textSize + 1;
    CHECK(!layoutText(lib, bigger).fits);

    // UITextSizeConstraint caps it.
    p.maxScaledSize = 20;
    CHECK_EQ(layoutText(lib, p).textSize, 20.0f);
}

TEST(text_truncate_at_end)
{
    auto& lib = fonts();
    TextParams p = params("a long line of text that will not fit", 18, {100, 18});
    p.truncate = Enum::TextTruncate::AtEnd;
    const TextLayout layout = layoutText(lib, p);
    CHECK_EQ(layout.lines.size(), std::size_t{1});
    CHECK(layout.lines[0].width <= 100.0f);
    // Ends in three dots.
    const std::size_t n = layout.glyphs.size();
    CHECK(n >= 3);
    const FontFace* face = lib.face(Enum::Font::SourceSans);
    for (std::size_t i = n - 3; i < n; ++i)
        CHECK_EQ(layout.glyphs[i].glyph, face->glyphIndex('.'));
}

TEST(text_max_visible_graphemes)
{
    auto& lib = fonts();
    TextParams p = params("abcdef", 18, {300, 30});
    p.maxVisibleGraphemes = 3;
    CHECK_EQ(layoutText(lib, p).glyphs.size(), std::size_t{3});
}

// The layout cache hands the same layout to the same parameters, and a
// different one when anything but the text differs too: every parameter
// is part of what it's looked up by.
TEST(text_layout_cache_shares_only_like_layouts)
{
    auto& lib = fonts();
    const TextParams base = params("cached text", 18, {200, 50});
    const auto first = layoutTextShared(lib, base);
    CHECK(first != nullptr);
    CHECK(layoutTextShared(lib, base) == first);
    CHECK(layoutText(lib, base).glyphs.size() == first->glyphs.size());
    std::vector<TextParams> others(14, base);
    others[0].text = "cached text!";
    others[1].style.size = 19;
    others[2].style.font.family = "rbxasset://fonts/families/Arial.json";
    others[3].style.color = {1.0f, 0.0f, 0.0f};
    others[4].lineHeight = 1.5f;
    others[5].wrapped = !base.wrapped;
    others[6].scaled = !base.scaled;
    others[7].richText = !base.richText;
    others[8].xAlignment = Enum::TextXAlignment::Right;
    others[9].yAlignment = Enum::TextYAlignment::Bottom;
    others[10].truncate = Enum::TextTruncate::AtEnd;
    others[11].box = {201, 50};
    others[12].maxVisibleGraphemes = 3;
    others[13].maxScaledSize = 40;
    for (const TextParams& other : others) {
        const auto layout = layoutTextShared(lib, other);
        CHECK(layout != first);
        CHECK(layoutTextShared(lib, other) == layout);
    }
    TextParams constrained = base;
    constrained.minScaledSize = 5;
    CHECK(layoutTextShared(lib, constrained) != first);
}
