#include <lunar/ui/TextLayout.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <unordered_map>

namespace lunar::ui {

namespace {

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

std::string lowerAscii(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    return out;
}

std::optional<float> parseNumber(std::string_view s)
{
    s = trim(s);
    float v = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size())
        return std::nullopt;
    return v;
}

// "#RRGGBB" or "rgb(r, g, b)".
std::optional<Color3> parseColor(std::string_view s)
{
    s = trim(s);
    if (s.size() == 7 && s[0] == '#') {
        unsigned value = 0;
        auto [end, ec] = std::from_chars(s.data() + 1, s.data() + 7, value, 16);
        if (ec != std::errc{} || end != s.data() + 7)
            return std::nullopt;
        return Color3::fromRGB(static_cast<int>(value >> 16), static_cast<int>((value >> 8) & 255),
                               static_cast<int>(value & 255));
    }
    if (lowerAscii(s).starts_with("rgb(") && s.back() == ')') {
        std::string_view inner = s.substr(4, s.size() - 5);
        int channels[3]{};
        for (int& channel : channels) {
            const std::size_t comma = inner.find(',');
            auto n = parseNumber(inner.substr(0, comma));
            if (!n)
                return std::nullopt;
            channel = static_cast<int>(*n);
            inner = comma == std::string_view::npos ? std::string_view{} : inner.substr(comma + 1);
        }
        return Color3::fromRGB(channels[0], channels[1], channels[2]);
    }
    return std::nullopt;
}

std::optional<Enum::FontWeight> parseWeight(std::string_view s)
{
    const std::string w = lowerAscii(trim(s));
    static constexpr std::pair<std::string_view, Enum::FontWeight> names[] = {
        {"thin", Enum::FontWeight::Thin},     {"extralight", Enum::FontWeight::ExtraLight},
        {"light", Enum::FontWeight::Light},   {"regular", Enum::FontWeight::Regular},
        {"medium", Enum::FontWeight::Medium}, {"semibold", Enum::FontWeight::SemiBold},
        {"bold", Enum::FontWeight::Bold},     {"extrabold", Enum::FontWeight::ExtraBold},
        {"heavy", Enum::FontWeight::Heavy},
    };
    for (const auto& [name, weight] : names)
        if (w == name)
            return weight;
    if (auto n = parseNumber(w)) {
        const int rounded = std::clamp(static_cast<int>(std::lround(*n / 100.0f)) * 100, 100, 900);
        return static_cast<Enum::FontWeight>(rounded);
    }
    return std::nullopt;
}

// Attributes of a tag: name="value" pairs, single or double quotes.
std::optional<std::vector<std::pair<std::string, std::string>>> parseAttributes(std::string_view s)
{
    std::vector<std::pair<std::string, std::string>> attributes;
    for (;;) {
        s = trim(s);
        if (s.empty())
            return attributes;
        const std::size_t eq = s.find('=');
        if (eq == std::string_view::npos)
            return std::nullopt;
        std::string name = lowerAscii(trim(s.substr(0, eq)));
        s = trim(s.substr(eq + 1));
        if (s.empty() || (s[0] != '"' && s[0] != '\''))
            return std::nullopt;
        const char quote = s[0];
        const std::size_t close = s.find(quote, 1);
        if (close == std::string_view::npos)
            return std::nullopt;
        attributes.emplace_back(std::move(name), std::string(s.substr(1, close - 1)));
        s = s.substr(close + 1);
    }
}

} // namespace

std::vector<TextRun> parseRichText(std::string_view text, const TextStyle& base)
{
    std::vector<TextRun> runs;
    struct Open {
        std::string name;
        TextStyle style; // in effect inside the tag
    };
    std::vector<Open> stack;
    auto current = [&]() -> const TextStyle& { return stack.empty() ? base : stack.back().style; };
    auto emit = [&](std::string_view s) {
        if (s.empty())
            return;
        if (!runs.empty() && runs.back().style == current())
            runs.back().text += s;
        else
            runs.push_back({std::string(s), current()});
    };

    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '&') {
            static constexpr std::pair<std::string_view, std::string_view> escapes[] = {
                {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"},
            };
            bool matched = false;
            for (const auto& [from, to] : escapes) {
                if (text.substr(i, from.size()) == from) {
                    emit(to);
                    i += from.size();
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                emit("&");
                ++i;
            }
            continue;
        }
        if (c != '<') {
            const std::size_t next = text.find_first_of("<&", i);
            const std::size_t end = next == std::string_view::npos ? text.size() : next;
            emit(text.substr(i, end - i));
            i = end;
            continue;
        }
        if (text.substr(i, 4) == "<!--") {
            const std::size_t close = text.find("-->", i + 4);
            if (close != std::string_view::npos) {
                i = close + 3;
                continue;
            }
        }
        const std::size_t close = text.find('>', i);
        if (close == std::string_view::npos) {
            emit(text.substr(i));
            break;
        }
        std::string_view body = trim(text.substr(i + 1, close - i - 1));
        const std::string_view literal = text.substr(i, close - i + 1);
        i = close + 1;

        if (!body.empty() && body[0] == '/') {
            const std::string name = lowerAscii(trim(body.substr(1)));
            if (!stack.empty() && stack.back().name == name)
                stack.pop_back();
            else
                emit(literal);
            continue;
        }
        bool selfClosing = false;
        if (!body.empty() && body.back() == '/') {
            selfClosing = true;
            body = trim(body.substr(0, body.size() - 1));
        }
        const std::size_t space = body.find_first_of(" \t");
        const std::string name = lowerAscii(body.substr(0, space));
        const std::string_view rest = space == std::string_view::npos ? std::string_view{} : body.substr(space);

        if (name == "br") {
            emit("\n");
            continue;
        }
        TextStyle style = current();
        bool known = true;
        if (name == "b") {
            style.font.weight = Enum::FontWeight::Bold;
        } else if (name == "i") {
            style.font.style = Enum::FontStyle::Italic;
        } else if (name == "u") {
            style.underline = true;
        } else if (name == "s") {
            style.strikethrough = true;
        } else if (name == "uppercase" || name == "uc") {
            style.uppercase = true;
        } else if (name == "smallcaps" || name == "sc" || name == "mark") {
            // Accepted; drawn as plain text.
        } else if (name == "font" || name == "stroke") {
            auto attributes = parseAttributes(rest);
            if (!attributes) {
                known = false;
            } else {
                for (const auto& [key, value] : *attributes) {
                    if (name == "font") {
                        if (key == "color") {
                            if (auto color = parseColor(value))
                                style.color = *color;
                        } else if (key == "size") {
                            if (auto size = parseNumber(value))
                                style.size = *size;
                        } else if (key == "face") {
                            for (int f = 0; f < 64; ++f) {
                                auto info = enumFontInfo(static_cast<Enum::Font>(f));
                                if (info && lowerAscii(info->name) == lowerAscii(value)) {
                                    style.font.family = std::string(info->family);
                                    break;
                                }
                            }
                        } else if (key == "family") {
                            style.font.family = value;
                        } else if (key == "weight") {
                            if (auto weight = parseWeight(value))
                                style.font.weight = *weight;
                        } else if (key == "transparency") {
                            if (auto t = parseNumber(value))
                                style.transparency = std::clamp(*t, 0.0f, 1.0f);
                        }
                    } else {
                        if (key == "color") {
                            if (auto color = parseColor(value))
                                style.strokeColor = *color;
                        } else if (key == "thickness") {
                            if (auto t = parseNumber(value))
                                style.strokeThickness = std::max(*t, 0.0f);
                        } else if (key == "transparency") {
                            if (auto t = parseNumber(value))
                                style.strokeTransparency = std::clamp(*t, 0.0f, 1.0f);
                        }
                    }
                }
                if (name == "stroke" && !style.strokeColor)
                    style.strokeColor = Color3{};
                if (name == "stroke" && style.strokeTransparency >= 1.0f)
                    style.strokeTransparency = 0.0f;
            }
        } else {
            known = false;
        }
        if (!known) {
            emit(literal);
            continue;
        }
        if (!selfClosing)
            stack.push_back({name, style});
    }
    return runs;
}

float pixelsPerUnit(const FontFace& face, float effectiveSize)
{
    const int span = std::max(face.ascender() - face.descender(), 1);
    return effectiveSize / static_cast<float>(span);
}

namespace {

// A shaped character: its glyph, where it may break and how far it moves the pen.
struct Item {
    const FontFace* face = nullptr;
    std::uint16_t glyph = 0;
    char32_t codepoint = 0;
    float advance = 0.0f; // including kerning with the previous item
    float scale = 0.0f;
    float lineSize = 0.0f; // effective text size: the line's height contribution
    float ascent = 0.0f;   // pixels above the baseline
    std::uint32_t run = 0;
    std::uint32_t source = 0;
};

struct Shaped {
    std::vector<Item> items;
    std::vector<TextStyle> styles;
    std::string plain;
};

char32_t toUpper(char32_t c)
{
    if ((c >= U'a' && c <= U'z') || (c >= U'à' && c <= U'þ' && c != U'÷'))
        return c - 32;
    return c;
}

constexpr char32_t zeroWidthJoiner = 0x200D, emojiSelector = 0xFE0F, keycap = 0x20E3;

// Joiners and variation selectors: they change what's around them and are
// never drawn, whatever glyph a font has for them (Arimo's ZWJ is a visible mark).
bool isInvisibleFormat(char32_t c)
{
    return c == 0x200C || c == zeroWidthJoiner || (c >= 0xFE00 && c <= emojiSelector);
}

Shaped shape(FontLibrary& fonts, const std::vector<TextRun>& runs, float sizeOverride, int maxGraphemes)
{
    Shaped out;
    int graphemes = 0;
    const FontFace* previousFace = nullptr;
    std::uint16_t previousGlyph = 0;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        TextStyle style = runs[r].style;
        if (sizeOverride > 0.0f)
            style.size = sizeOverride;
        out.styles.push_back(style);
        const FontFace* primary = fonts.face(style.font);
        const float effective = style.size * familySizeScale(style.font.family);
        const std::string_view text = runs[r].text;
        const std::size_t runStart = out.items.size();
        std::size_t pos = 0;
        while (pos < text.size()) {
            const auto source = static_cast<std::uint32_t>(out.plain.size());
            const std::size_t begin = pos;
            char32_t cp = decodeUtf8(text, pos);
            out.plain.append(text.substr(begin, pos - begin));
            if (maxGraphemes >= 0 && graphemes >= maxGraphemes)
                continue;
            ++graphemes;
            if (style.uppercase)
                cp = toUpper(cp);
            Item item;
            item.codepoint = cp;
            item.run = static_cast<std::uint32_t>(r);
            item.source = source;
            item.lineSize = effective;
            if (cp == U'\n' || cp == U'\r') {
                out.items.push_back(item);
                continue;
            }
            if (cp == U'\t')
                cp = U' ';
            FontLibrary::Glyph g = fonts.glyph(primary, cp);
            // An emoji sequence stays in the color font, for its ligature:
            // the text's own font may have some of it (a ZWJ, a digit, a
            // sign). A lone U+FE0F only asks for a glyph with colors.
            std::size_t after = pos;
            const char32_t next = after < text.size() ? decodeUtf8(text, after) : 0;
            const char32_t afterNext = after < text.size() ? decodeUtf8(text, after) : 0;
            const Item* before = out.items.size() > runStart ? &out.items.back() : nullptr;
            const bool sequence = next == keycap || (next == emojiSelector && afterNext == keycap)
                                  || (before && before->face && before->face->hasColor()
                                      && (isInvisibleFormat(cp) || before->codepoint == zeroWidthJoiner));
            if (sequence || next == emojiSelector)
                if (const FontLibrary::Glyph color = fonts.colorGlyph(cp);
                    color.face && (sequence || !color.face->colorLayers(color.index).empty()))
                    g = color;
            if (!g.face) {
                out.items.push_back(item);
                continue;
            }
            item.face = g.face;
            item.glyph = g.index;
            item.scale = pixelsPerUnit(*g.face, effective);
            item.ascent = static_cast<float>(g.face->ascender()) * item.scale;
            out.items.push_back(item);
        }
        // The font's standard ligatures over the run's glyphs: neighbours in
        // the same face and size that make one become it (fi, and every
        // BuilderIcons icon, the ligature of its name). The ligature stands
        // where its first glyph did.
        std::vector<std::uint16_t> glyphs;
        for (std::size_t i = runStart; i < out.items.size(); ++i) {
            const Item& first = out.items[i];
            if (!first.face)
                continue;
            glyphs.clear();
            for (std::size_t j = i; j < out.items.size() && glyphs.size() < 64; ++j) {
                const Item& next = out.items[j];
                if (next.face != first.face || next.scale != first.scale)
                    break;
                glyphs.push_back(next.glyph);
            }
            if (const auto ligature = first.face->ligature(glyphs)) {
                out.items[i].glyph = ligature->glyph;
                out.items.erase(out.items.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                out.items.begin() + static_cast<std::ptrdiff_t>(i + ligature->length));
            }
        }
        // What no ligature took of the joiners and selectors.
        out.items.erase(std::remove_if(out.items.begin() + static_cast<std::ptrdiff_t>(runStart), out.items.end(),
                                       [](const Item& item) { return isInvisibleFormat(item.codepoint); }),
                        out.items.end());
        // Advances, with kerning against the glyph before in the same face and size.
        for (std::size_t i = runStart; i < out.items.size(); ++i) {
            Item& item = out.items[i];
            if (!item.face) {
                if (item.codepoint == U'\n' || item.codepoint == U'\r')
                    previousFace = nullptr;
                continue;
            }
            item.advance = static_cast<float>(item.face->advance(item.glyph)) * item.scale;
            if (previousFace == item.face && i > 0 && out.items[i - 1].scale == item.scale)
                item.advance += static_cast<float>(item.face->kerning(previousGlyph, item.glyph)) * item.scale;
            previousFace = item.face;
            previousGlyph = item.glyph;
        }
    }
    return out;
}

bool isSpace(char32_t c)
{
    return c == U' ' || c == U'　';
}

struct LineSpan {
    std::size_t begin = 0;
    std::size_t end = 0; // exclusive, without the newline
    float width = 0.0f;  // without trailing spaces
};

// Kerning is folded into each item's advance, so a line's width is the
// sum, minus the first item's kerning against whatever preceded it.
std::vector<LineSpan> breakLines(const std::vector<Item>& items, float maxWidth, bool wrap, bool* overflow)
{
    std::vector<LineSpan> lines;
    std::size_t start = 0;
    while (start <= items.size()) {
        float width = 0.0f;
        std::size_t i = start;
        std::size_t lastBreak = std::string::npos; // index after a space run
        bool forced = false;
        for (; i < items.size(); ++i) {
            const Item& item = items[i];
            if (item.codepoint == U'\n' || item.codepoint == U'\r') {
                forced = true;
                break;
            }
            const float advance = i == start && item.face
                                      ? static_cast<float>(item.face->advance(item.glyph)) * item.scale
                                      : item.advance;
            if (wrap && !isSpace(item.codepoint) && width + advance > maxWidth + 0.01f && i > start) {
                if (lastBreak != std::string::npos && lastBreak > start) {
                    i = lastBreak;
                } else if (overflow) {
                    *overflow = true; // a word wider than the box gets split
                }
                break;
            }
            width += advance;
            if (isSpace(item.codepoint) && (i + 1 >= items.size() || !isSpace(items[i + 1].codepoint)))
                lastBreak = i + 1;
        }
        LineSpan line{start, i, 0.0f};
        // Width without trailing spaces.
        std::size_t visibleEnd = line.end;
        while (visibleEnd > line.begin && isSpace(items[visibleEnd - 1].codepoint))
            --visibleEnd;
        for (std::size_t k = line.begin; k < visibleEnd; ++k) {
            const Item& item = items[k];
            line.width += k == line.begin && item.face ? static_cast<float>(item.face->advance(item.glyph)) * item.scale
                                                       : item.advance;
        }
        lines.push_back(line);
        if (forced) {
            start = i + 1;
            if (start == items.size()) {
                lines.push_back({start, start, 0.0f}); // text ending in a newline has an empty last line
                break;
            }
        } else if (i >= items.size()) {
            break;
        } else {
            start = i;
        }
    }
    return lines;
}

TextLayout build(FontLibrary& fonts, const TextParams& params, const std::vector<TextRun>& runs, float size)
{
    TextLayout layout;
    layout.textSize = size;
    Shaped shaped = shape(fonts, runs, params.scaled ? size : 0.0f, params.maxVisibleGraphemes);
    layout.styles = std::move(shaped.styles);
    layout.plain = std::move(shaped.plain);
    const bool wrap = params.wrapped || params.scaled;
    bool overflow = false;
    std::vector<LineSpan> spans = breakLines(shaped.items, params.box.x, wrap, &overflow);

    const float baseSize = size * familySizeScale(params.style.font.family);
    // Line heights: the biggest size on the line (rich text can mix sizes).
    std::vector<float> heights;
    std::vector<float> ascents;
    for (const LineSpan& span : spans) {
        float h = 0.0f;
        float a = 0.0f;
        for (std::size_t k = span.begin; k < span.end; ++k) {
            h = std::max(h, shaped.items[k].lineSize);
            a = std::max(a, shaped.items[k].ascent);
        }
        if (h == 0.0f) {
            h = baseSize;
            if (const FontFace* face = fonts.face(params.style.font))
                a = static_cast<float>(face->ascender()) * pixelsPerUnit(*face, baseSize);
        }
        heights.push_back(h);
        ascents.push_back(a);
    }

    float totalHeight = 0.0f;
    float maxWidth = 0.0f;
    for (std::size_t l = 0; l < spans.size(); ++l) {
        totalHeight += heights[l] * params.lineHeight;
        maxWidth = std::max(maxWidth, spans[l].width);
    }
    layout.bounds = {maxWidth, totalHeight};
    layout.fits = !overflow && totalHeight <= params.box.y + 0.01f && (wrap || maxWidth <= params.box.x + 0.01f);

    float y = 0.0f;
    switch (params.yAlignment) {
    case Enum::TextYAlignment::Top:
        break;
    case Enum::TextYAlignment::Center:
        y = (params.box.y - totalHeight) / 2.0f;
        break;
    case Enum::TextYAlignment::Bottom:
        y = params.box.y - totalHeight;
        break;
    }
    y = std::round(y); // text sits on whole pixels (guess; keeps glyph rows crisp)

    std::size_t visibleLines = spans.size();
    if (params.truncate != Enum::TextTruncate::None && totalHeight > params.box.y + 0.01f) {
        float h = 0.0f;
        visibleLines = 0;
        while (visibleLines < spans.size() && h + heights[visibleLines] * params.lineHeight <= params.box.y + 0.01f)
            h += heights[visibleLines++] * params.lineHeight;
        visibleLines = std::max<std::size_t>(visibleLines, 1);
    }

    for (std::size_t l = 0; l < visibleLines; ++l) {
        LineSpan span = spans[l];
        const float lineBox = heights[l] * params.lineHeight;
        TextLine line;
        line.top = y;
        line.height = lineBox;
        // The ascender-descender span centred in the line box.
        line.baseline = std::round(y + (lineBox - heights[l]) / 2.0f + ascents[l]);
        bool ellipsis = false;
        const bool lastVisible = l + 1 == visibleLines;
        if (params.truncate != Enum::TextTruncate::None
            && ((lastVisible && visibleLines < spans.size()) || span.width > params.box.x + 0.01f)) {
            // Drop characters until the line plus "..." fits.
            const FontFace* face = fonts.face(params.style.font);
            float dots = 0.0f;
            if (face) {
                const FontLibrary::Glyph dot = fonts.glyph(face, U'.');
                dots = 3.0f * static_cast<float>(dot.face->advance(dot.index)) * pixelsPerUnit(*face, baseSize);
            }
            while (span.end > span.begin && span.width + dots > params.box.x + 0.01f) {
                --span.end;
                span.width = 0.0f;
                for (std::size_t k = span.begin; k < span.end; ++k)
                    span.width += shaped.items[k].advance;
            }
            if (params.truncate == Enum::TextTruncate::SplitWord || span.end == spans[l].end) {
                // SplitWord cuts anywhere; AtEnd backs up to a word boundary when it can.
            } else {
                std::size_t end = span.end;
                while (end > span.begin && !isSpace(shaped.items[end - 1].codepoint))
                    --end;
                if (end > span.begin) {
                    span.end = end;
                    while (span.end > span.begin && isSpace(shaped.items[span.end - 1].codepoint))
                        --span.end;
                    span.width = 0.0f;
                    for (std::size_t k = span.begin; k < span.end; ++k)
                        span.width += shaped.items[k].advance;
                }
            }
            ellipsis = true;
            span.width += dots;
        }

        float x = 0.0f;
        switch (params.xAlignment) {
        case Enum::TextXAlignment::Left:
            break;
        case Enum::TextXAlignment::Center:
            x = (params.box.x - span.width) / 2.0f;
            break;
        case Enum::TextXAlignment::Right:
            x = params.box.x - span.width;
            break;
        }
        line.x = x;
        line.width = span.width;
        line.firstGlyph = static_cast<std::uint32_t>(layout.glyphs.size());
        float pen = x;
        for (std::size_t k = span.begin; k < span.end; ++k) {
            const Item& item = shaped.items[k];
            if (k != span.begin)
                pen += item.advance - (item.face ? static_cast<float>(item.face->advance(item.glyph)) * item.scale : 0);
            if (item.face && !isSpace(item.codepoint))
                layout.glyphs.push_back({item.face, item.glyph, pen, line.baseline, item.scale, item.run, item.source});
            if (item.face)
                pen += static_cast<float>(item.face->advance(item.glyph)) * item.scale;
        }
        if (ellipsis) {
            if (const FontFace* face = fonts.face(params.style.font)) {
                const FontLibrary::Glyph dot = fonts.glyph(face, U'.');
                const float s = pixelsPerUnit(*face, baseSize);
                const std::uint32_t run = span.end > span.begin ? shaped.items[span.end - 1].run : 0;
                for (int d = 0; d < 3; ++d) {
                    layout.glyphs.push_back({dot.face, dot.index, pen, line.baseline, s, run,
                                             static_cast<std::uint32_t>(layout.plain.size())});
                    pen += static_cast<float>(dot.face->advance(dot.index)) * s;
                }
            }
        }
        line.glyphCount = static_cast<std::uint32_t>(layout.glyphs.size()) - line.firstGlyph;

        // Underline and strikethrough per run segment.
        for (std::size_t k = span.begin; k < span.end;) {
            const std::uint32_t run = shaped.items[k].run;
            const TextStyle& style = layout.styles[run];
            std::size_t e = k;
            while (e < span.end && shaped.items[e].run == run)
                ++e;
            if (style.underline || style.strikethrough) {
                float x0 = x;
                for (std::size_t q = span.begin; q < k; ++q)
                    x0 += shaped.items[q].advance;
                float x1 = x0;
                for (std::size_t q = k; q < e; ++q)
                    x1 += shaped.items[q].advance;
                const float runSize = shaped.items[k].lineSize;
                const float thickness = std::max(1.0f, std::round(runSize / 14.0f));
                if (style.underline)
                    layout.decorations.push_back({x0, x1, line.baseline + std::round(runSize * 0.1f), thickness, run});
                if (style.strikethrough)
                    layout.decorations.push_back({x0, x1, line.baseline - std::round(runSize * 0.3f), thickness, run});
            }
            k = e;
        }
        layout.lines.push_back(line);
        y += lineBox;
    }
    return layout;
}

TextLayout layoutUncached(FontLibrary& fonts, const TextParams& params)
{
    const std::vector<TextRun> runs = params.richText ? parseRichText(params.text, params.style)
                                                      : std::vector<TextRun>{{std::string(params.text), params.style}};
    if (!params.scaled)
        return build(fonts, params, runs, params.style.size);

    // TextScaled: the largest whole size that fits, wrapping at word
    // boundaries. Fitting is monotonic in the size, so bisect.
    int lo = std::max(params.minScaledSize, 1);
    int hi = std::max(params.maxScaledSize, lo);
    if (!build(fonts, params, runs, static_cast<float>(lo)).fits)
        return build(fonts, params, runs, static_cast<float>(lo));
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (build(fonts, params, runs, static_cast<float>(mid)).fits)
            lo = mid;
        else
            hi = mid - 1;
    }
    return build(fonts, params, runs, static_cast<float>(lo));
}

} // namespace

// The layouts made with one FontLibrary, by everything they were made from:
// the GUIs lay out the same text every frame (a third of a millisecond on
// Duvall). Fonts load from the content folder as they're asked for, so the
// same parameters always give the same layout.
struct LayoutCache {
    struct Entry {
        std::string text;
        TextParams params; // its text left empty
        std::shared_ptr<const TextLayout> layout;
    };
    // More than this many and they all go (texts that change every frame).
    static constexpr std::size_t limit = 1024;
    std::mutex mutex;
    std::unordered_map<std::size_t, std::vector<Entry>> byHash;
    std::size_t count = 0;
};

namespace {

std::size_t layoutHash(const TextParams& params)
{
    std::size_t hash = std::hash<std::string_view>{}(params.text);
    for (const float f : {params.style.size, params.box.x, params.box.y})
        hash = hash * 1099511628211ull ^ std::bit_cast<std::uint32_t>(f);
    return hash;
}

// Whether two sets of parameters are the same but for their text, without
// copying either (a copy is a copy of the font's family name).
bool sameButText(const TextParams& a, const TextParams& b)
{
    return a.style == b.style && a.lineHeight == b.lineHeight && a.wrapped == b.wrapped && a.scaled == b.scaled
           && a.richText == b.richText && a.xAlignment == b.xAlignment && a.yAlignment == b.yAlignment
           && a.truncate == b.truncate && a.box == b.box && a.maxVisibleGraphemes == b.maxVisibleGraphemes
           && a.minScaledSize == b.minScaledSize && a.maxScaledSize == b.maxScaledSize;
}

} // namespace

std::shared_ptr<const TextLayout> layoutTextShared(FontLibrary& fonts, const TextParams& params)
{
    std::shared_ptr<LayoutCache>& slot = fonts.layoutCache();
    if (!slot)
        slot = std::make_shared<LayoutCache>();
    LayoutCache& cache = *slot;
    const std::size_t hash = layoutHash(params);
    {
        const std::scoped_lock lock(cache.mutex);
        if (auto it = cache.byHash.find(hash); it != cache.byHash.end())
            for (const LayoutCache::Entry& entry : it->second)
                if (entry.text == params.text && sameButText(entry.params, params))
                    return entry.layout;
    }
    auto layout = std::make_shared<const TextLayout>(layoutUncached(fonts, params));
    TextParams key = params;
    key.text = {};
    const std::scoped_lock lock(cache.mutex);
    if (cache.count >= LayoutCache::limit) {
        cache.byHash.clear();
        cache.count = 0;
    }
    cache.byHash[hash].push_back({std::string(params.text), std::move(key), layout});
    ++cache.count;
    return layout;
}

TextLayout layoutText(FontLibrary& fonts, const TextParams& params)
{
    return *layoutTextShared(fonts, params);
}

Vector2 measureText(FontLibrary& fonts, std::string_view text, const lunar::Font& font, float size, float width)
{
    TextParams params;
    params.text = text;
    params.style.font = font;
    params.style.size = size;
    params.wrapped = width > 0.0f;
    params.box = {width > 0.0f ? width : 1e9f, 1e9f};
    params.xAlignment = Enum::TextXAlignment::Left;
    params.yAlignment = Enum::TextYAlignment::Top;
    return layoutText(fonts, params).bounds;
}

} // namespace lunar::ui
