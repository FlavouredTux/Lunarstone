#include <lunar/ui/Font.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mdspan>
#include <optional>

namespace lunar::ui {

namespace {

constexpr std::uint32_t tag(const char (&s)[5])
{
    return std::uint32_t{static_cast<unsigned char>(s[0])} << 24 | std::uint32_t{static_cast<unsigned char>(s[1])} << 16
           | std::uint32_t{static_cast<unsigned char>(s[2])} << 8 | std::uint32_t{static_cast<unsigned char>(s[3])};
}

// Big-endian reads that give 0 past the end, so a truncated font can't read
// out of bounds; parse() checks the sizes that matter.
struct Reader {
    std::span<const std::uint8_t> data;

    std::uint8_t u8(std::size_t at) const { return at < data.size() ? data[at] : 0; }
    std::uint16_t u16(std::size_t at) const { return static_cast<std::uint16_t>(u8(at) << 8 | u8(at + 1)); }
    std::int16_t s16(std::size_t at) const { return std::bit_cast<std::int16_t>(u16(at)); }
    std::uint32_t u32(std::size_t at) const { return std::uint32_t{u16(at)} << 16 | u16(at + 2); }
    std::uint32_t offsetN(std::size_t at, int size) const
    {
        std::uint32_t v = 0;
        for (int i = 0; i < size; ++i)
            v = v << 8 | u8(at + static_cast<std::size_t>(i));
        return v;
    }
};

// A CFF INDEX: count, offset size, offsets, data.
struct CffIndex {
    std::uint32_t start = 0; // of the INDEX
    std::uint16_t count = 0;
    int offSize = 0;
    std::uint32_t dataStart = 0; // byte before the first object (offsets are 1-based)
    std::uint32_t end = 0;       // first byte after the INDEX

    static CffIndex at(const Reader& r, std::uint32_t pos)
    {
        CffIndex index;
        index.start = pos;
        index.count = r.u16(pos);
        if (index.count == 0) {
            index.end = pos + 2;
            return index;
        }
        index.offSize = r.u8(pos + 2);
        const std::uint32_t offsets = pos + 3;
        index.dataStart = offsets + (index.count + 1u) * static_cast<std::uint32_t>(index.offSize) - 1;
        index.end = index.dataStart
                    + r.offsetN(offsets + index.count * static_cast<std::uint32_t>(index.offSize), index.offSize);
        return index;
    }
    // Absolute [begin, end) of object i.
    std::pair<std::uint32_t, std::uint32_t> object(const Reader& r, std::uint32_t i) const
    {
        if (i >= count)
            return {0, 0};
        const std::uint32_t offsets = start + 3;
        const auto size = static_cast<std::uint32_t>(offSize);
        return {dataStart + r.offsetN(offsets + i * size, offSize),
                dataStart + r.offsetN(offsets + (i + 1) * size, offSize)};
    }
};

// Top and Private DICT operands we care about.
struct CffDict {
    std::unordered_map<int, std::vector<double>> ops; // 12 x -> 1200 + x
};

CffDict parseDict(const Reader& r, std::uint32_t begin, std::uint32_t end)
{
    CffDict dict;
    std::vector<double> operands;
    std::uint32_t p = begin;
    while (p < end) {
        const int b0 = r.u8(p);
        if (b0 <= 21) {
            int op = b0;
            ++p;
            if (b0 == 12) {
                op = 1200 + r.u8(p);
                ++p;
            }
            dict.ops[op] = std::move(operands);
            operands.clear();
        } else if (b0 == 28) {
            operands.push_back(r.s16(p + 1));
            p += 3;
        } else if (b0 == 29) {
            operands.push_back(static_cast<double>(std::bit_cast<std::int32_t>(r.u32(p + 1))));
            p += 5;
        } else if (b0 == 30) {
            // Real number, nibbles; we only need it to skip correctly.
            std::string text;
            ++p;
            bool done = false;
            while (!done && p < end) {
                const int byte = r.u8(p++);
                for (int nibble : {byte >> 4, byte & 15}) {
                    if (nibble <= 9)
                        text += static_cast<char>('0' + nibble);
                    else if (nibble == 0xa)
                        text += '.';
                    else if (nibble == 0xb)
                        text += 'E';
                    else if (nibble == 0xc)
                        text += "E-";
                    else if (nibble == 0xe)
                        text += '-';
                    else if (nibble == 0xf) {
                        done = true;
                        break;
                    }
                }
            }
            operands.push_back(std::strtod(text.c_str(), nullptr));
        } else if (b0 >= 32 && b0 <= 246) {
            operands.push_back(b0 - 139);
            ++p;
        } else if (b0 >= 247 && b0 <= 250) {
            operands.push_back((b0 - 247) * 256 + r.u8(p + 1) + 108);
            p += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            operands.push_back(-(b0 - 251) * 256 - r.u8(p + 1) - 108);
            p += 2;
        } else {
            ++p; // reserved
        }
    }
    return dict;
}

int subrBias(std::uint16_t count)
{
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

} // namespace

// --- Outline -------------------------------------------------------------------------

void Outline::moveTo(float x, float y)
{
    verbs.push_back(Verb::Move);
    points.push_back({x, y});
}

void Outline::lineTo(float x, float y)
{
    verbs.push_back(Verb::Line);
    points.push_back({x, y});
}

void Outline::quadTo(float cx, float cy, float x, float y)
{
    verbs.push_back(Verb::Quad);
    points.push_back({cx, cy});
    points.push_back({x, y});
}

void Outline::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    verbs.push_back(Verb::Cubic);
    points.push_back({c1x, c1y});
    points.push_back({c2x, c2y});
    points.push_back({x, y});
}

Outline::Bounds Outline::controlBounds() const
{
    if (points.empty())
        return {};
    Bounds b{points[0].x, points[0].y, points[0].x, points[0].y};
    for (const Point& p : points) {
        b.minX = std::min(b.minX, p.x);
        b.minY = std::min(b.minY, p.y);
        b.maxX = std::max(b.maxX, p.x);
        b.maxY = std::max(b.maxY, p.y);
    }
    return b;
}

// --- FontFace ------------------------------------------------------------------------

std::expected<std::shared_ptr<FontFace>, std::string> FontFace::load(std::string bytes)
{
    std::shared_ptr<FontFace> face(new FontFace());
    face->data_ = std::move(bytes);
    if (auto ok = face->parse(); !ok)
        return std::unexpected(ok.error());
    return face;
}

std::expected<void, std::string> FontFace::parse()
{
    const Reader r{bytes()};
    if (data_.size() < 12)
        return std::unexpected("font file too short");
    const std::uint32_t version = r.u32(0);
    if (version != 0x00010000 && version != tag("OTTO") && version != tag("true"))
        return std::unexpected("not a TrueType/OpenType font");
    const std::uint16_t numTables = r.u16(4);
    for (std::uint16_t i = 0; i < numTables; ++i) {
        const std::size_t rec = 12 + std::size_t{i} * 16;
        const Table table{r.u32(rec + 8), r.u32(rec + 12)};
        if (std::size_t{table.offset} + table.length > data_.size())
            return std::unexpected("font table out of bounds");
        tables_[r.u32(rec)] = table;
    }
    auto find = [&](std::uint32_t t) -> const Table* {
        auto it = tables_.find(t);
        return it == tables_.end() ? nullptr : &it->second;
    };
    const Table* head = find(tag("head"));
    const Table* hhea = find(tag("hhea"));
    const Table* maxp = find(tag("maxp"));
    const Table* hmtx = find(tag("hmtx"));
    const Table* cmap = find(tag("cmap"));
    if (!head || !hhea || !maxp || !hmtx || !cmap)
        return std::unexpected("font lacks a required table (head, hhea, maxp, hmtx or cmap)");

    unitsPerEm_ = r.u16(head->offset + 18);
    if (unitsPerEm_ < 16)
        return std::unexpected("bad unitsPerEm");
    longLoca_ = r.s16(head->offset + 50) != 0;
    ascender_ = r.s16(hhea->offset + 4);
    descender_ = r.s16(hhea->offset + 6);
    lineGap_ = r.s16(hhea->offset + 8);
    metricCount_ = r.u16(hhea->offset + 34);
    glyphCount_ = r.u16(maxp->offset + 4);
    hmtx_ = hmtx->offset;
    if (metricCount_ == 0 || std::size_t{metricCount_} * 4 > hmtx->length)
        return std::unexpected("bad hmtx");

    parseCmap(cmap->offset);

    if (const Table* name = find(tag("name"))) {
        // Family name (ID 1), from a Windows Unicode or Mac Roman record.
        const std::uint16_t count = r.u16(name->offset + 2);
        const std::uint32_t strings = name->offset + r.u16(name->offset + 4);
        for (std::uint16_t i = 0; i < count && familyName_.empty(); ++i) {
            const std::size_t rec = name->offset + 6 + std::size_t{i} * 12;
            if (r.u16(rec + 6) != 1)
                continue;
            const std::uint16_t platform = r.u16(rec);
            const std::uint16_t length = r.u16(rec + 8);
            const std::uint32_t at = strings + r.u16(rec + 10);
            if (platform == 3)
                for (std::uint32_t k = 0; k + 1 < length; k += 2)
                    appendUtf8(familyName_, r.u16(at + k));
            else if (platform == 1)
                for (std::uint32_t k = 0; k < length; ++k)
                    familyName_ += static_cast<char>(r.u8(at + k));
        }
    }

    if (const Table* cff = find(tag("CFF "))) {
        if (auto ok = parseCff(cff->offset, cff->length); !ok)
            return ok;
    } else {
        const Table* loca = find(tag("loca"));
        const Table* glyf = find(tag("glyf"));
        if (!loca || !glyf)
            return std::unexpected("font has neither glyf nor CFF outlines");
        if (loca->length < (std::size_t{glyphCount_} + 1) * (longLoca_ ? 4 : 2))
            return std::unexpected("loca too short");
        loca_ = loca->offset;
        glyf_ = glyf->offset;
        glyfLength_ = glyf->length;
    }

    if (const Table* gpos = find(tag("GPOS")))
        parseGpos(gpos->offset);
    if (const Table* gsub = find(tag("GSUB")))
        parseGsub(gsub->offset);
    if (const Table* colr = find(tag("COLR")))
        if (const Table* cpal = find(tag("CPAL")))
            parseColor(colr->offset, cpal->offset);
    if (pairKerning_.empty() && classKerning_.empty())
        if (const Table* kern = find(tag("kern")))
            parseKern(kern->offset, kern->length);
    return {};
}

void FontFace::parseCmap(std::uint32_t offset)
{
    const Reader r{bytes()};
    const std::uint16_t count = r.u16(offset + 2);
    // Prefer full Unicode (format 12), then BMP (format 4).
    std::uint32_t best = 0;
    int bestScore = -1;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t rec = offset + 4 + std::size_t{i} * 8;
        const std::uint16_t platform = r.u16(rec);
        const std::uint16_t encoding = r.u16(rec + 2);
        const std::uint32_t sub = offset + r.u32(rec + 4);
        const std::uint16_t format = r.u16(sub);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode || (format != 4 && format != 12))
            continue;
        const int score = format == 12 ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            best = sub;
        }
    }
    if (bestScore < 0)
        return;
    if (r.u16(best) == 12) {
        const std::uint32_t groups = r.u32(best + 12);
        for (std::uint32_t g = 0; g < groups; ++g) {
            const std::size_t at = best + 16 + std::size_t{g} * 12;
            cmap_.push_back({r.u32(at), r.u32(at + 4), r.u32(at + 8), true});
        }
    } else {
        const std::uint16_t segX2 = r.u16(best + 6);
        const std::uint32_t ends = best + 14;
        const std::uint32_t starts = ends + segX2 + 2;
        const std::uint32_t deltas = starts + segX2;
        const std::uint32_t rangeOffsets = deltas + segX2;
        for (std::uint32_t s = 0; s < segX2; s += 2) {
            const std::uint16_t end = r.u16(ends + s);
            const std::uint16_t start = r.u16(starts + s);
            const std::uint16_t delta = r.u16(deltas + s);
            const std::uint16_t ro = r.u16(rangeOffsets + s);
            if (start > end || start == 0xFFFF)
                continue;
            if (ro == 0) {
                cmap_.push_back({start, end, static_cast<std::uint16_t>(start + delta), true});
            } else {
                // glyphIdArray entries, then the delta on non-zero ones.
                CmapRange range{start, end, static_cast<std::uint32_t>(glyphIds_.size()), false};
                for (std::uint32_t cp = start; cp <= end; ++cp) {
                    const std::uint32_t at = rangeOffsets + s + ro + (cp - start) * 2;
                    std::uint16_t g = r.u16(at);
                    if (g != 0)
                        g = static_cast<std::uint16_t>(g + delta);
                    glyphIds_.push_back(g);
                }
                cmap_.push_back(range);
            }
        }
    }
    std::ranges::sort(cmap_, {}, &CmapRange::first);
}

std::uint16_t FontFace::glyphIndex(char32_t codepoint) const
{
    auto it = std::ranges::upper_bound(cmap_, codepoint, {}, &CmapRange::first);
    if (it == cmap_.begin())
        return 0;
    --it;
    if (codepoint > it->last)
        return 0;
    std::uint32_t glyph = 0;
    if (it->delta)
        glyph = (it->glyph + (codepoint - it->first)) & 0xFFFF;
    else
        glyph = glyphIds_[it->glyph + (codepoint - it->first)];
    return glyph < glyphCount_ ? static_cast<std::uint16_t>(glyph) : 0;
}

int FontFace::advance(std::uint16_t glyph) const
{
    const Reader r{bytes()};
    const std::uint16_t i = std::min<std::uint16_t>(glyph, static_cast<std::uint16_t>(metricCount_ - 1));
    return r.u16(hmtx_ + std::size_t{i} * 4);
}

int FontFace::leftSideBearing(std::uint16_t glyph) const
{
    const Reader r{bytes()};
    if (glyph < metricCount_)
        return r.s16(hmtx_ + std::size_t{glyph} * 4 + 2);
    return r.s16(hmtx_ + std::size_t{metricCount_} * 4 + (std::size_t{glyph} - metricCount_) * 2);
}

int FontFace::kerning(std::uint16_t left, std::uint16_t right) const
{
    if (auto it = pairKerning_.find(std::uint32_t{left} << 16 | right); it != pairKerning_.end())
        return it->second;
    for (const ClassKerning& k : classKerning_) {
        if (!std::ranges::binary_search(k.coverage, left))
            continue;
        auto c1 = k.class1.find(left);
        auto c2 = k.class2.find(right);
        const std::uint16_t a = c1 == k.class1.end() ? 0 : c1->second;
        const std::uint16_t b = c2 == k.class2.end() ? 0 : c2->second;
        const std::size_t index = std::size_t{a} * k.class2Count + b;
        if (index < k.values.size() && k.values[index] != 0)
            return k.values[index];
    }
    return 0;
}

namespace {

// Coverage table -> glyphs, in coverage index order.
std::vector<std::uint16_t> readCoverage(const Reader& r, std::uint32_t at)
{
    std::vector<std::uint16_t> glyphs;
    const std::uint16_t format = r.u16(at);
    const std::uint16_t count = r.u16(at + 2);
    if (format == 1) {
        for (std::uint16_t i = 0; i < count; ++i)
            glyphs.push_back(r.u16(at + 4 + std::size_t{i} * 2));
    } else if (format == 2) {
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::size_t rec = at + 4 + std::size_t{i} * 6;
            const std::uint16_t first = r.u16(rec);
            const std::uint16_t last = r.u16(rec + 2);
            for (std::uint32_t g = first; g <= last && last - first < 0x8000; ++g)
                glyphs.push_back(static_cast<std::uint16_t>(g));
        }
    }
    return glyphs;
}

std::unordered_map<std::uint16_t, std::uint16_t> readClassDef(const Reader& r, std::uint32_t at)
{
    std::unordered_map<std::uint16_t, std::uint16_t> classes;
    const std::uint16_t format = r.u16(at);
    if (format == 1) {
        const std::uint16_t start = r.u16(at + 2);
        const std::uint16_t count = r.u16(at + 4);
        for (std::uint16_t i = 0; i < count; ++i)
            classes[static_cast<std::uint16_t>(start + i)] = r.u16(at + 6 + std::size_t{i} * 2);
    } else if (format == 2) {
        const std::uint16_t count = r.u16(at + 2);
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::size_t rec = at + 4 + std::size_t{i} * 6;
            const std::uint16_t first = r.u16(rec);
            const std::uint16_t last = r.u16(rec + 2);
            const std::uint16_t cls = r.u16(rec + 4);
            for (std::uint32_t g = first; g <= last && last - first < 0x8000; ++g)
                classes[static_cast<std::uint16_t>(g)] = cls;
        }
    }
    return classes;
}

int valueRecordSize(std::uint16_t format)
{
    return std::popcount(static_cast<unsigned>(format & 0xFF)) * 2;
}

// XAdvance of a value record (0 if the format has none).
std::int16_t xAdvance(const Reader& r, std::size_t at, std::uint16_t format)
{
    if (!(format & 4))
        return 0;
    const int skip = std::popcount(static_cast<unsigned>(format & 3)) * 2;
    return r.s16(at + static_cast<std::size_t>(skip));
}

} // namespace

void FontFace::parseGpos(std::uint32_t offset)
{
    const Reader r{bytes()};
    const std::uint32_t features = offset + r.u16(offset + 6);
    const std::uint32_t lookups = offset + r.u16(offset + 8);
    // Every lookup a 'kern' feature uses, in lookup order.
    std::vector<std::uint16_t> indices;
    const std::uint16_t featureCount = r.u16(features);
    for (std::uint16_t f = 0; f < featureCount; ++f) {
        const std::size_t rec = features + 2 + std::size_t{f} * 6;
        if (r.u32(rec) != tag("kern"))
            continue;
        const std::uint32_t feature = features + r.u16(rec + 4);
        const std::uint16_t count = r.u16(feature + 2);
        for (std::uint16_t i = 0; i < count; ++i)
            indices.push_back(r.u16(feature + 4 + std::size_t{i} * 2));
    }
    std::ranges::sort(indices);
    indices.erase(std::ranges::unique(indices).begin(), indices.end());

    const std::uint16_t lookupCount = r.u16(lookups);
    for (std::uint16_t li : indices) {
        if (li >= lookupCount)
            continue;
        const std::uint32_t lookup = lookups + r.u16(lookups + 2 + std::size_t{li} * 2);
        const std::uint16_t type = r.u16(lookup);
        const std::uint16_t subCount = r.u16(lookup + 4);
        for (std::uint16_t s = 0; s < subCount; ++s) {
            std::uint32_t sub = lookup + r.u16(lookup + 6 + std::size_t{s} * 2);
            if (type == 9) { // extension
                if (r.u16(sub + 2) != 2)
                    continue;
                sub += r.u32(sub + 4);
            } else if (type != 2) {
                continue;
            }
            const std::uint16_t format = r.u16(sub);
            const std::vector<std::uint16_t> coverage = readCoverage(r, sub + r.u16(sub + 2));
            const std::uint16_t vf1 = r.u16(sub + 4);
            const std::uint16_t vf2 = r.u16(sub + 6);
            const int size1 = valueRecordSize(vf1);
            const int size2 = valueRecordSize(vf2);
            if (format == 1) {
                const std::uint16_t setCount = r.u16(sub + 8);
                for (std::uint16_t i = 0; i < setCount && i < coverage.size(); ++i) {
                    const std::uint32_t set = sub + r.u16(sub + 10 + std::size_t{i} * 2);
                    const std::uint16_t pairs = r.u16(set);
                    const auto stride = static_cast<std::size_t>(2) + static_cast<std::size_t>(size1)
                                        + static_cast<std::size_t>(size2);
                    for (std::uint16_t p = 0; p < pairs; ++p) {
                        const std::size_t rec = set + 2 + p * stride;
                        const std::int16_t value = xAdvance(r, rec + 2, vf1);
                        // Earlier subtables win, like a shaper applying the first match.
                        pairKerning_.try_emplace(std::uint32_t{coverage[i]} << 16 | r.u16(rec), value);
                    }
                }
            } else if (format == 2) {
                ClassKerning k;
                k.coverage = coverage;
                std::ranges::sort(k.coverage);
                k.class1 = readClassDef(r, sub + r.u16(sub + 8));
                k.class2 = readClassDef(r, sub + r.u16(sub + 10));
                const std::uint16_t class1Count = r.u16(sub + 12);
                k.class2Count = r.u16(sub + 14);
                const auto stride = static_cast<std::size_t>(size1) + static_cast<std::size_t>(size2);
                k.values.resize(std::size_t{class1Count} * k.class2Count);
                for (std::size_t i = 0; i < k.values.size(); ++i)
                    k.values[i] = xAdvance(r, sub + 16 + i * stride, vf1);
                classKerning_.push_back(std::move(k));
            }
        }
    }
}

void FontFace::parseGsub(std::uint32_t offset)
{
    const Reader r{bytes()};
    const std::uint32_t features = offset + r.u16(offset + 6);
    const std::uint32_t lookups = offset + r.u16(offset + 8);
    // Every lookup the standard ligature features use, in lookup order (a
    // shaper's defaults: what text gets without asking).
    std::vector<std::uint16_t> indices;
    const std::uint16_t featureCount = r.u16(features);
    for (std::uint16_t f = 0; f < featureCount; ++f) {
        const std::size_t rec = features + 2 + std::size_t{f} * 6;
        const std::uint32_t name = r.u32(rec);
        if (name != tag("liga") && name != tag("rlig") && name != tag("clig") && name != tag("ccmp"))
            continue;
        const std::uint32_t feature = features + r.u16(rec + 4);
        const std::uint16_t count = r.u16(feature + 2);
        for (std::uint16_t i = 0; i < count; ++i)
            indices.push_back(r.u16(feature + 4 + std::size_t{i} * 2));
    }
    std::ranges::sort(indices);
    indices.erase(std::ranges::unique(indices).begin(), indices.end());

    const std::uint16_t lookupCount = r.u16(lookups);
    for (std::uint16_t li : indices) {
        if (li >= lookupCount)
            continue;
        const std::uint32_t lookup = lookups + r.u16(lookups + 2 + std::size_t{li} * 2);
        const std::uint16_t type = r.u16(lookup);
        const std::uint16_t subCount = r.u16(lookup + 4);
        for (std::uint16_t s = 0; s < subCount; ++s) {
            std::uint32_t sub = lookup + r.u16(lookup + 6 + std::size_t{s} * 2);
            if (type == 7) { // extension
                if (r.u16(sub + 2) != 4)
                    continue;
                sub += r.u32(sub + 4);
            } else if (type != 4) {
                continue; // single, contextual and the rest: not ligatures
            }
            if (r.u16(sub) != 1)
                continue;
            const std::vector<std::uint16_t> coverage = readCoverage(r, sub + r.u16(sub + 2));
            const std::uint16_t setCount = r.u16(sub + 4);
            for (std::uint16_t i = 0; i < setCount && i < coverage.size(); ++i) {
                const std::uint32_t set = sub + r.u16(sub + 6 + std::size_t{i} * 2);
                const std::uint16_t count = r.u16(set);
                std::vector<LigatureRule>& rules = ligatures_[coverage[i]];
                for (std::uint16_t l = 0; l < count; ++l) {
                    const std::uint32_t lig = set + r.u16(set + 2 + std::size_t{l} * 2);
                    const std::uint16_t components = r.u16(lig + 2);
                    if (components < 2)
                        continue;
                    LigatureRule rule{.rest = {}, .glyph = r.u16(lig)};
                    rule.rest.reserve(components - 1u);
                    for (std::uint16_t c = 1; c < components; ++c)
                        rule.rest.push_back(r.u16(lig + 4 + std::size_t{c - 1u} * 2));
                    rules.push_back(std::move(rule));
                }
            }
        }
    }
}

std::optional<FontFace::Ligature> FontFace::ligature(std::span<const std::uint16_t> glyphs) const
{
    if (glyphs.size() < 2)
        return std::nullopt;
    const auto found = ligatures_.find(glyphs[0]);
    if (found == ligatures_.end())
        return std::nullopt;
    // The first that matches, as the font orders them (longer ones first).
    for (const LigatureRule& rule : found->second)
        if (rule.rest.size() < glyphs.size() && std::ranges::equal(rule.rest, glyphs.subspan(1, rule.rest.size())))
            return Ligature{rule.glyph, rule.rest.size() + 1};
    return std::nullopt;
}

void FontFace::parseColor(std::uint32_t colr, std::uint32_t cpal)
{
    const Reader r{bytes()};
    // CPAL's first palette: blue, green, red, alpha.
    const std::uint16_t entries = r.u16(cpal + 2);
    const std::uint32_t palette = cpal + r.u32(cpal + 8) + std::uint32_t{r.u16(cpal + 12)} * 4;
    const std::uint32_t layers = colr + r.u32(colr + 8);
    const std::uint16_t layerCount = r.u16(colr + 12);
    colorLayers_.reserve(layerCount);
    for (std::uint16_t i = 0; i < layerCount; ++i) {
        const std::uint32_t rec = layers + std::uint32_t{i} * 4;
        const std::uint16_t index = r.u16(rec + 2);
        ColorLayer layer{.glyph = r.u16(rec)};
        if (index < entries) {
            const std::uint32_t color = palette + std::uint32_t{index} * 4;
            layer.b = static_cast<float>(r.u8(color)) / 255.0f;
            layer.g = static_cast<float>(r.u8(color + 1)) / 255.0f;
            layer.r = static_cast<float>(r.u8(color + 2)) / 255.0f;
            layer.a = static_cast<float>(r.u8(color + 3)) / 255.0f;
        } else {
            layer.foreground = true; // 0xFFFF
        }
        colorLayers_.push_back(layer);
    }
    const std::uint32_t bases = colr + r.u32(colr + 4);
    const std::uint16_t baseCount = r.u16(colr + 2);
    for (std::uint16_t i = 0; i < baseCount; ++i) {
        const std::uint32_t rec = bases + std::uint32_t{i} * 6;
        const ColorGlyph glyph{r.u16(rec + 2), r.u16(rec + 4)};
        if (std::size_t{glyph.first} + glyph.count <= colorLayers_.size())
            colorGlyphs_[r.u16(rec)] = glyph;
    }
}

std::span<const FontFace::ColorLayer> FontFace::colorLayers(std::uint16_t glyph) const
{
    if (colorGlyphs_.empty())
        return {};
    const auto found = colorGlyphs_.find(glyph);
    if (found == colorGlyphs_.end())
        return {};
    return std::span(colorLayers_).subspan(found->second.first, found->second.count);
}

void FontFace::parseKern(std::uint32_t offset, std::uint32_t length)
{
    const Reader r{bytes()};
    if (r.u16(offset) != 0)
        return; // Apple's version 1 layout isn't used by these fonts
    const std::uint16_t count = r.u16(offset + 2);
    std::uint32_t sub = offset + 4;
    for (std::uint16_t i = 0; i < count && sub < offset + length; ++i) {
        const std::uint16_t subLength = r.u16(sub + 2);
        const std::uint16_t coverage = r.u16(sub + 4);
        if ((coverage >> 8) == 0 && (coverage & 1)) { // format 0, horizontal
            const std::uint16_t pairs = r.u16(sub + 6);
            for (std::uint16_t p = 0; p < pairs; ++p) {
                const std::size_t rec = sub + 14 + std::size_t{p} * 6;
                pairKerning_.try_emplace(r.u32(rec), r.s16(rec + 4));
            }
        }
        sub += subLength;
    }
}

std::expected<void, std::string> FontFace::parseCff(std::uint32_t offset, std::uint32_t length)
{
    const Reader r{bytes().subspan(0, std::size_t{offset} + length)};
    const std::uint32_t headerSize = r.u8(offset + 2);
    const CffIndex names = CffIndex::at(r, offset + headerSize);
    const CffIndex topDicts = CffIndex::at(r, names.end);
    const CffIndex strings = CffIndex::at(r, topDicts.end);
    const CffIndex globals = CffIndex::at(r, strings.end);
    if (topDicts.count == 0)
        return std::unexpected("CFF without a Top DICT");
    cff_.globalSubrs = globals.start;

    const auto [topBegin, topEnd] = topDicts.object(r, 0);
    const CffDict top = parseDict(r, topBegin, topEnd);
    auto first = [&](const CffDict& d, int op) -> std::optional<double> {
        auto it = d.ops.find(op);
        if (it == d.ops.end() || it->second.empty())
            return std::nullopt;
        return it->second.back();
    };
    if (auto cs = top.ops.find(1206); cs != top.ops.end() && !cs->second.empty() && cs->second[0] != 2)
        return std::unexpected("only Type 2 charstrings are supported");
    const auto charStrings = first(top, 17);
    if (!charStrings)
        return std::unexpected("CFF without CharStrings");
    cff_.charStrings = offset + static_cast<std::uint32_t>(*charStrings);

    auto localSubrsOf = [&](const CffDict& dict) -> std::uint32_t {
        auto priv = dict.ops.find(18);
        if (priv == dict.ops.end() || priv->second.size() < 2)
            return 0;
        const auto size = static_cast<std::uint32_t>(priv->second[0]);
        const std::uint32_t at = offset + static_cast<std::uint32_t>(priv->second[1]);
        const CffDict privDict = parseDict(r, at, at + size);
        if (auto subrs = first(privDict, 19))
            return at + static_cast<std::uint32_t>(*subrs);
        return 0;
    };

    if (top.ops.contains(1230)) { // ROS: CID-keyed
        const auto fdArray = first(top, 1236);
        const auto fdSelect = first(top, 1237);
        if (!fdArray || !fdSelect)
            return std::unexpected("CID CFF without FDArray/FDSelect");
        cff_.fdSelect = offset + static_cast<std::uint32_t>(*fdSelect);
        const CffIndex fds = CffIndex::at(r, offset + static_cast<std::uint32_t>(*fdArray));
        for (std::uint16_t i = 0; i < fds.count; ++i) {
            const auto [b, e] = fds.object(r, i);
            cff_.fdLocalSubrs.push_back(localSubrsOf(parseDict(r, b, e)));
        }
    } else {
        cff_.localSubrs = localSubrsOf(top);
    }
    return {};
}

Outline FontFace::outline(std::uint16_t glyph) const
{
    Outline out;
    if (glyph >= glyphCount_)
        return out;
    if (isCff()) {
        cffOutline(glyph, out);
    } else {
        float identity[6] = {1, 0, 0, 1, 0, 0};
        glyfOutline(glyph, out, identity, 0);
    }
    return out;
}

void FontFace::glyfOutline(std::uint16_t glyph, Outline& out, float m[6], int depth) const
{
    const Reader r{bytes()};
    if (depth > 8)
        return;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    if (longLoca_) {
        begin = r.u32(loca_ + std::size_t{glyph} * 4);
        end = r.u32(loca_ + std::size_t{glyph} * 4 + 4);
    } else {
        begin = r.u16(loca_ + std::size_t{glyph} * 2) * 2u;
        end = r.u16(loca_ + std::size_t{glyph} * 2 + 2) * 2u;
    }
    if (end <= begin || end > glyfLength_)
        return;
    const std::uint32_t g = glyf_ + begin;
    const std::int16_t contours = r.s16(g);
    auto transform
        = [&](float x, float y) { return Outline::Point{m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]}; };

    if (contours >= 0) {
        const std::uint32_t endPts = g + 10;
        const auto n = static_cast<std::uint16_t>(contours);
        if (n == 0)
            return;
        const auto pointCount = static_cast<std::uint16_t>(r.u16(endPts + (n - 1u) * 2) + 1);
        const std::uint16_t instructions = r.u16(endPts + n * 2u);
        std::uint32_t p = endPts + n * 2u + 2 + instructions;

        std::vector<std::uint8_t> flags(pointCount);
        for (std::uint16_t i = 0; i < pointCount;) {
            const std::uint8_t f = r.u8(p++);
            flags[i++] = f;
            if (f & 8) {
                std::uint8_t repeat = r.u8(p++);
                while (repeat-- > 0 && i < pointCount)
                    flags[i++] = f;
            }
        }
        std::vector<Outline::Point> pts(pointCount);
        int coord = 0;
        for (std::uint16_t i = 0; i < pointCount; ++i) {
            const std::uint8_t f = flags[i];
            if (f & 2) {
                const int d = r.u8(p++);
                coord += (f & 16) ? d : -d;
            } else if (!(f & 16)) {
                coord += r.s16(p);
                p += 2;
            }
            pts[i].x = static_cast<float>(coord);
        }
        coord = 0;
        for (std::uint16_t i = 0; i < pointCount; ++i) {
            const std::uint8_t f = flags[i];
            if (f & 4) {
                const int d = r.u8(p++);
                coord += (f & 32) ? d : -d;
            } else if (!(f & 32)) {
                coord += r.s16(p);
                p += 2;
            }
            pts[i].y = static_cast<float>(coord);
        }
        for (auto& pt : pts)
            pt = transform(pt.x, pt.y);

        std::uint16_t start = 0;
        for (std::uint16_t c = 0; c < n; ++c) {
            const std::uint16_t last = r.u16(endPts + c * 2u);
            if (last < start || last >= pointCount)
                break;
            const auto count = static_cast<std::uint16_t>(last - start + 1);
            auto on = [&](std::uint16_t k) { return (flags[start + k % count] & 1) != 0; };
            auto pt = [&](std::uint16_t k) { return pts[start + k % count]; };
            // Start on an on-curve point, or the midpoint of two off-curve ones.
            std::uint16_t first = 0;
            while (first < count && !on(first))
                ++first;
            Outline::Point startPoint;
            if (first == count) {
                const auto a = pt(0);
                const auto b = pt(1);
                startPoint = {(a.x + b.x) / 2, (a.y + b.y) / 2};
                first = 0;
            } else {
                startPoint = pt(first);
            }
            out.moveTo(startPoint.x, startPoint.y);
            std::optional<Outline::Point> control;
            for (std::uint16_t k = 1; k <= count; ++k) {
                const auto idx = static_cast<std::uint16_t>(first + k);
                const auto q = pt(idx);
                if (on(idx)) {
                    if (control)
                        out.quadTo(control->x, control->y, q.x, q.y);
                    else
                        out.lineTo(q.x, q.y);
                    control.reset();
                } else {
                    if (control) {
                        const Outline::Point mid{(control->x + q.x) / 2, (control->y + q.y) / 2};
                        out.quadTo(control->x, control->y, mid.x, mid.y);
                    }
                    control = q;
                }
            }
            if (control)
                out.quadTo(control->x, control->y, startPoint.x, startPoint.y);
            start = static_cast<std::uint16_t>(last + 1);
        }
        return;
    }

    // Composite glyph.
    std::uint32_t p = g + 10;
    for (;;) {
        const std::uint16_t flags = r.u16(p);
        const std::uint16_t component = r.u16(p + 2);
        p += 4;
        float dx = 0;
        float dy = 0;
        if (flags & 1) {
            dx = r.s16(p);
            dy = r.s16(p + 2);
            p += 4;
        } else {
            dx = static_cast<std::int8_t>(r.u8(p));
            dy = static_cast<std::int8_t>(r.u8(p + 1));
            p += 2;
        }
        if (!(flags & 2))
            dx = dy = 0; // point matching: not supported, place at the origin
        float a = 1, b = 0, c = 0, d = 1;
        auto f2dot14 = [&](std::uint32_t at) { return static_cast<float>(r.s16(at)) / 16384.0f; };
        if (flags & 8) {
            a = d = f2dot14(p);
            p += 2;
        } else if (flags & 0x40) {
            a = f2dot14(p);
            d = f2dot14(p + 2);
            p += 4;
        } else if (flags & 0x80) {
            a = f2dot14(p);
            b = f2dot14(p + 2);
            c = f2dot14(p + 4);
            d = f2dot14(p + 6);
            p += 8;
        }
        // m * [a c dx; b d dy]
        float child[6] = {
            m[0] * a + m[2] * b, m[1] * a + m[3] * b,          m[0] * c + m[2] * d,
            m[1] * c + m[3] * d, m[0] * dx + m[2] * dy + m[4], m[1] * dx + m[3] * dy + m[5],
        };
        glyfOutline(component, out, child, depth + 1);
        if (!(flags & 0x20))
            break;
    }
}

void FontFace::cffOutline(std::uint16_t glyph, Outline& out) const
{
    const Reader r{bytes()};
    const CffIndex charStrings = CffIndex::at(r, cff_.charStrings);
    const CffIndex globals = CffIndex::at(r, cff_.globalSubrs);
    std::uint32_t localAt = cff_.localSubrs;
    if (cff_.fdSelect) {
        const std::uint8_t format = r.u8(cff_.fdSelect);
        std::uint32_t fd = 0;
        if (format == 0) {
            fd = r.u8(cff_.fdSelect + 1 + glyph);
        } else if (format == 3) {
            const std::uint16_t ranges = r.u16(cff_.fdSelect + 1);
            for (std::uint16_t i = 0; i < ranges; ++i) {
                const std::size_t rec = cff_.fdSelect + 3 + std::size_t{i} * 3;
                const std::uint16_t next = r.u16(rec + 3);
                if (glyph >= r.u16(rec) && glyph < next) {
                    fd = r.u8(rec + 2);
                    break;
                }
            }
        }
        localAt = fd < cff_.fdLocalSubrs.size() ? cff_.fdLocalSubrs[fd] : 0;
    }
    const CffIndex locals = localAt ? CffIndex::at(r, localAt) : CffIndex{};
    const int globalBias = subrBias(globals.count);
    const int localBias = subrBias(locals.count);

    std::array<float, 48> stack{};
    std::size_t sp = 0;
    int stems = 0;
    bool haveWidth = false;
    bool open = false;
    float x = 0;
    float y = 0;

    auto clearWidth = [&](bool odd) {
        // The first stack-clearing operator may carry the advance width first.
        if (!haveWidth && sp > 0 && odd) {
            std::move(stack.begin() + 1, stack.begin() + static_cast<std::ptrdiff_t>(sp), stack.begin());
            --sp;
        }
        haveWidth = true;
    };
    auto moveTo = [&](float nx, float ny) {
        x = nx;
        y = ny;
        out.moveTo(x, y);
        open = true;
    };
    auto lineTo = [&](float nx, float ny) {
        x = nx;
        y = ny;
        out.lineTo(x, y);
    };
    auto curveTo = [&](float ax, float ay, float bx, float by, float cx, float cy) {
        out.cubicTo(ax, ay, bx, by, cx, cy);
        x = cx;
        y = cy;
    };

    // Recursive charstring execution; returns false on endchar.
    auto run = [&](this auto& self, std::uint32_t begin, std::uint32_t end, int depth) -> bool {
        if (depth > 10)
            return false;
        std::uint32_t p = begin;
        while (p < end) {
            const int b0 = r.u8(p);
            if (b0 >= 32 || b0 == 28) {
                float v = 0;
                if (b0 == 28) {
                    v = r.s16(p + 1);
                    p += 3;
                } else if (b0 <= 246) {
                    v = static_cast<float>(b0 - 139);
                    p += 1;
                } else if (b0 <= 250) {
                    v = static_cast<float>((b0 - 247) * 256 + r.u8(p + 1) + 108);
                    p += 2;
                } else if (b0 <= 254) {
                    v = static_cast<float>(-(b0 - 251) * 256 - r.u8(p + 1) - 108);
                    p += 2;
                } else {
                    v = static_cast<float>(std::bit_cast<std::int32_t>(r.u32(p + 1))) / 65536.0f;
                    p += 5;
                }
                if (sp < stack.size())
                    stack[sp++] = v;
                continue;
            }
            ++p;
            switch (b0) {
            case 1:  // hstem
            case 3:  // vstem
            case 18: // hstemhm
            case 23: // vstemhm
                clearWidth(sp % 2 == 1);
                stems += static_cast<int>(sp / 2);
                sp = 0;
                break;
            case 19: // hintmask
            case 20: // cntrmask
                clearWidth(sp % 2 == 1);
                stems += static_cast<int>(sp / 2); // implied vstem
                sp = 0;
                p += static_cast<std::uint32_t>((stems + 7) / 8);
                break;
            case 21: // rmoveto
                clearWidth(sp > 2);
                moveTo(x + stack[0], y + stack[1]);
                sp = 0;
                break;
            case 22: // hmoveto
                clearWidth(sp > 1);
                moveTo(x + stack[0], y);
                sp = 0;
                break;
            case 4: // vmoveto
                clearWidth(sp > 1);
                moveTo(x, y + stack[0]);
                sp = 0;
                break;
            case 5: // rlineto
                for (std::size_t i = 0; i + 1 < sp; i += 2)
                    lineTo(x + stack[i], y + stack[i + 1]);
                sp = 0;
                break;
            case 6:   // hlineto
            case 7: { // vlineto
                bool horizontal = b0 == 6;
                for (std::size_t i = 0; i < sp; ++i, horizontal = !horizontal)
                    horizontal ? lineTo(x + stack[i], y) : lineTo(x, y + stack[i]);
                sp = 0;
                break;
            }
            case 8: // rrcurveto
                for (std::size_t i = 0; i + 5 < sp; i += 6) {
                    const float ax = x + stack[i], ay = y + stack[i + 1];
                    const float bx = ax + stack[i + 2], by = ay + stack[i + 3];
                    curveTo(ax, ay, bx, by, bx + stack[i + 4], by + stack[i + 5]);
                }
                sp = 0;
                break;
            case 24: { // rcurveline
                std::size_t i = 0;
                for (; i + 7 < sp; i += 6) {
                    const float ax = x + stack[i], ay = y + stack[i + 1];
                    const float bx = ax + stack[i + 2], by = ay + stack[i + 3];
                    curveTo(ax, ay, bx, by, bx + stack[i + 4], by + stack[i + 5]);
                }
                if (i + 1 < sp)
                    lineTo(x + stack[i], y + stack[i + 1]);
                sp = 0;
                break;
            }
            case 25: { // rlinecurve
                std::size_t i = 0;
                for (; i + 7 < sp; i += 2)
                    lineTo(x + stack[i], y + stack[i + 1]);
                if (i + 5 < sp) {
                    const float ax = x + stack[i], ay = y + stack[i + 1];
                    const float bx = ax + stack[i + 2], by = ay + stack[i + 3];
                    curveTo(ax, ay, bx, by, bx + stack[i + 4], by + stack[i + 5]);
                }
                sp = 0;
                break;
            }
            case 26: { // vvcurveto
                std::size_t i = 0;
                float dx1 = 0;
                if (sp % 4 == 1)
                    dx1 = stack[i++];
                for (; i + 3 < sp; i += 4) {
                    const float ax = x + dx1, ay = y + stack[i];
                    const float bx = ax + stack[i + 1], by = ay + stack[i + 2];
                    curveTo(ax, ay, bx, by, bx, by + stack[i + 3]);
                    dx1 = 0;
                }
                sp = 0;
                break;
            }
            case 27: { // hhcurveto
                std::size_t i = 0;
                float dy1 = 0;
                if (sp % 4 == 1)
                    dy1 = stack[i++];
                for (; i + 3 < sp; i += 4) {
                    const float ax = x + stack[i], ay = y + dy1;
                    const float bx = ax + stack[i + 1], by = ay + stack[i + 2];
                    curveTo(ax, ay, bx, by, bx + stack[i + 3], by);
                    dy1 = 0;
                }
                sp = 0;
                break;
            }
            case 30:   // vhcurveto
            case 31: { // hvcurveto
                bool horizontal = b0 == 31;
                std::size_t i = 0;
                while (i + 3 < sp) {
                    const bool last = i + 8 > sp; // the final curve may take a 5th argument
                    const float extra = (last && sp - i == 5) ? stack[i + 4] : 0.0f;
                    if (horizontal) {
                        const float ax = x + stack[i], ay = y;
                        const float bx = ax + stack[i + 1], by = ay + stack[i + 2];
                        curveTo(ax, ay, bx, by, bx + extra, by + stack[i + 3]);
                    } else {
                        const float ax = x, ay = y + stack[i];
                        const float bx = ax + stack[i + 1], by = ay + stack[i + 2];
                        curveTo(ax, ay, bx, by, bx + stack[i + 3], by + extra);
                    }
                    i += 4;
                    horizontal = !horizontal;
                }
                sp = 0;
                break;
            }
            case 10:   // callsubr
            case 29: { // callgsubr
                if (sp == 0)
                    return false;
                const CffIndex& subrs = b0 == 10 ? locals : globals;
                const int index = static_cast<int>(stack[--sp]) + (b0 == 10 ? localBias : globalBias);
                if (index < 0 || index >= int{subrs.count})
                    return false;
                const auto [sb, se] = subrs.object(r, static_cast<std::uint32_t>(index));
                if (!self(sb, se, depth + 1))
                    return false;
                break;
            }
            case 11: // return
                return true;
            case 14: // endchar
                clearWidth(sp == 1 || sp == 5);
                sp = 0;
                return false;
            case 12: {
                const int b1 = r.u8(p++);
                const auto& s = stack;
                if (b1 == 35 && sp >= 13) { // flex
                    const float ax = x + s[0], ay = y + s[1], bx = ax + s[2], by = ay + s[3];
                    const float cx = bx + s[4], cy = by + s[5];
                    curveTo(ax, ay, bx, by, cx, cy);
                    const float dx = cx + s[6], dy = cy + s[7], ex = dx + s[8], ey = dy + s[9];
                    curveTo(dx, dy, ex, ey, ex + s[10], ey + s[11]);
                } else if (b1 == 34 && sp >= 7) { // hflex
                    const float y0 = y;
                    const float ax = x + s[0], ay = y, bx = ax + s[1], by = ay + s[2];
                    const float cx = bx + s[3];
                    curveTo(ax, ay, bx, by, cx, by);
                    const float dx = cx + s[4], ex = dx + s[5];
                    curveTo(dx, by, ex, y0, ex + s[6], y0);
                } else if (b1 == 36 && sp >= 9) { // hflex1
                    const float y0 = y;
                    const float ax = x + s[0], ay = y + s[1], bx = ax + s[2], by = ay + s[3];
                    const float cx = bx + s[4];
                    curveTo(ax, ay, bx, by, cx, by);
                    const float dx = cx + s[5], ex = dx + s[6], ey = by + s[7];
                    curveTo(dx, by, ex, ey, ex + s[8], y0);
                } else if (b1 == 37 && sp >= 11) { // flex1
                    const float x0 = x, y0 = y;
                    const float ax = x + s[0], ay = y + s[1], bx = ax + s[2], by = ay + s[3];
                    const float cx = bx + s[4], cy = by + s[5];
                    curveTo(ax, ay, bx, by, cx, cy);
                    const float dx = cx + s[6], dy = cy + s[7], ex = dx + s[8], ey = dy + s[9];
                    if (std::abs(ex - x0) > std::abs(ey - y0))
                        curveTo(dx, dy, ex, ey, ex + s[10], y0);
                    else
                        curveTo(dx, dy, ex, ey, x0, ey + s[10]);
                }
                sp = 0; // arithmetic operators are deprecated and unused by these fonts
                break;
            }
            default:
                sp = 0;
                break;
            }
        }
        return true;
    };

    const auto [begin, end] = charStrings.object(r, glyph);
    run(begin, end, 0);
    (void)open;
}

// --- Rasterizer ----------------------------------------------------------------------

namespace {

// Signed-area accumulation (the approach of font-rs): every line adds its
// coverage to the cells it crosses, a prefix sum along each row then gives
// the winding-weighted area per pixel.
class Accumulator {
public:
    Accumulator(int width, int height)
        : width_(width), height_(height),
          cells_(static_cast<std::size_t>(width + 2) * static_cast<std::size_t>(height), 0.0f)
    {
    }

    void line(float ax, float ay, float bx, float by)
    {
        if (ay == by)
            return;
        float dir = 1.0f;
        if (ay > by) {
            dir = -1.0f;
            std::swap(ax, bx);
            std::swap(ay, by);
        }
        const float dxdy = (bx - ax) / (by - ay);
        float x = ax;
        float y0 = ay;
        if (y0 < 0.0f) {
            x -= y0 * dxdy;
            y0 = 0.0f;
        }
        const float y1 = std::min(by, static_cast<float>(height_));
        const int stride = width_ + 2;
        for (int row = static_cast<int>(y0); static_cast<float>(row) < y1; ++row) {
            const float rowTop = std::max(static_cast<float>(row), y0);
            const float rowBottom = std::min(static_cast<float>(row + 1), y1);
            const float dy = rowBottom - rowTop;
            const float xNext = x + dxdy * dy;
            const float d = dy * dir;
            // Clamped to the bitmap; bounds come from ceil/floor so this only trims rounding.
            const float x0 = std::clamp(std::min(x, xNext), 0.0f, static_cast<float>(width_));
            const float x1 = std::clamp(std::max(x, xNext), 0.0f, static_cast<float>(width_));
            float* cells = cells_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
            const float x0floor = std::floor(x0);
            const auto x0i = static_cast<int>(x0floor);
            const float x1ceil = std::ceil(x1);
            const auto x1i = static_cast<int>(x1ceil);
            if (x1i <= x0i + 1) {
                // Within one cell: the part right of the line's mean x.
                const float xmf = 0.5f * (x0 + x1) - x0floor;
                cells[x0i] += d - d * xmf;
                cells[x0i + 1] += d * xmf;
            } else {
                // Spans cells: split the trapezoid exactly.
                const float s = 1.0f / (x1 - x0);
                const float x0f = x0 - x0floor;
                const float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
                const float x1f = x1 - x1ceil + 1.0f;
                const float am = 0.5f * s * x1f * x1f;
                cells[x0i] += d * a0;
                if (x1i == x0i + 2) {
                    cells[x0i + 1] += d * (1.0f - a0 - am);
                } else {
                    const float a1 = s * (1.5f - x0f);
                    cells[x0i + 1] += d * (a1 - a0);
                    for (int k = x0i + 2; k < x1i - 1; ++k)
                        cells[k] += d * s;
                    const float a2 = a1 + static_cast<float>(x1i - x0i - 3) * s;
                    cells[x1i - 1] += d * (1.0f - a2 - am);
                }
                cells[x1i] += d * am;
            }
            x = xNext;
        }
    }

    void finish(std::vector<std::uint8_t>& out) const
    {
        out.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0);
        std::mdspan pixels(out.data(), height_, width_);
        const int stride = width_ + 2;
        for (int row = 0; row < height_; ++row) {
            float sum = 0.0f;
            const float* cells = cells_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
            for (int col = 0; col < width_; ++col) {
                sum += cells[col];
                const float coverage = std::min(std::abs(sum), 1.0f);
                pixels[row, col] = static_cast<std::uint8_t>(std::lround(coverage * 255.0f));
            }
        }
    }

private:
    int width_;
    int height_;
    std::vector<float> cells_;
};

} // namespace

GlyphBitmap rasterize(const Outline& outline, float scale, float offsetX)
{
    GlyphBitmap bitmap;
    if (outline.empty() || scale <= 0.0f)
        return bitmap;
    const Outline::Bounds b = outline.controlBounds();
    const int left = static_cast<int>(std::floor(b.minX * scale + offsetX));
    const int right = static_cast<int>(std::ceil(b.maxX * scale + offsetX));
    const int top = static_cast<int>(std::ceil(b.maxY * scale));
    const int bottom = static_cast<int>(std::floor(b.minY * scale));
    bitmap.width = right - left;
    bitmap.height = top - bottom;
    bitmap.left = left;
    bitmap.top = top;
    if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.width > 4096 || bitmap.height > 4096) {
        bitmap = {};
        return bitmap;
    }

    Accumulator acc(bitmap.width, bitmap.height);
    // Font units to bitmap pixels, y down.
    auto map = [&](Outline::Point p) {
        return Outline::Point{p.x * scale + offsetX - static_cast<float>(left), static_cast<float>(top) - p.y * scale};
    };
    Outline::Point start{};
    Outline::Point pen{};
    auto lineTo = [&](Outline::Point q) {
        acc.line(pen.x, pen.y, q.x, q.y);
        pen = q;
    };
    std::size_t pi = 0;
    for (Outline::Verb verb : outline.verbs) {
        switch (verb) {
        case Outline::Verb::Move:
            lineTo(start);
            start = pen = map(outline.points[pi++]);
            break;
        case Outline::Verb::Line:
            lineTo(map(outline.points[pi++]));
            break;
        case Outline::Verb::Quad: {
            const auto c = map(outline.points[pi]);
            const auto e = map(outline.points[pi + 1]);
            pi += 2;
            const float dd = std::hypot(pen.x - 2 * c.x + e.x, pen.y - 2 * c.y + e.y);
            const int steps = std::clamp(static_cast<int>(std::sqrt(dd * 4.0f)), 1, 64);
            const Outline::Point p0 = pen;
            for (int i = 1; i <= steps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                const float u = 1 - t;
                lineTo({u * u * p0.x + 2 * u * t * c.x + t * t * e.x, u * u * p0.y + 2 * u * t * c.y + t * t * e.y});
            }
            break;
        }
        case Outline::Verb::Cubic: {
            const auto c1 = map(outline.points[pi]);
            const auto c2 = map(outline.points[pi + 1]);
            const auto e = map(outline.points[pi + 2]);
            pi += 3;
            const Outline::Point p0 = pen;
            const float dd = std::max(std::hypot(p0.x - 2 * c1.x + c2.x, p0.y - 2 * c1.y + c2.y),
                                      std::hypot(c1.x - 2 * c2.x + e.x, c1.y - 2 * c2.y + e.y));
            const int steps = std::clamp(static_cast<int>(std::sqrt(dd * 6.0f)), 1, 64);
            for (int i = 1; i <= steps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                const float u = 1 - t;
                const float a = u * u * u, bb = 3 * u * u * t, cc = 3 * u * t * t, d = t * t * t;
                lineTo({a * p0.x + bb * c1.x + cc * c2.x + d * e.x, a * p0.y + bb * c1.y + cc * c2.y + d * e.y});
            }
            break;
        }
        }
    }
    lineTo(start);
    acc.finish(bitmap.coverage);
    return bitmap;
}

// --- UTF-8 ---------------------------------------------------------------------------

char32_t decodeUtf8(std::string_view text, std::size_t& pos)
{
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char b0 = byte(pos);
    int length = 0;
    char32_t cp = 0;
    if (b0 < 0x80) {
        ++pos;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0) {
        length = 2;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        length = 3;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        length = 4;
        cp = b0 & 0x07u;
    } else {
        ++pos;
        return U'�';
    }
    if (pos + static_cast<std::size_t>(length) > text.size()) {
        ++pos;
        return U'�';
    }
    for (int i = 1; i < length; ++i) {
        const unsigned char b = byte(pos + static_cast<std::size_t>(i));
        if ((b & 0xC0) != 0x80) {
            ++pos;
            return U'�';
        }
        cp = cp << 6 | (b & 0x3Fu);
    }
    static constexpr char32_t minimum[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < minimum[length] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++pos;
        return U'�';
    }
    pos += static_cast<std::size_t>(length);
    return cp;
}

void appendUtf8(std::string& out, char32_t codepoint)
{
    const char32_t cp = codepoint;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace lunar::ui
