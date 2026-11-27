#include <lunar/ui/DrawList.hpp>

#include <algorithm>
#include <chrono>
#include <bit>
#include <cmath>
#include <functional>
#include <mdspan>
#include <string_view>
#include <vector>

namespace lunar::ui {

RectF RectF::intersect(const RectF& o) const
{
    const float x0 = std::max(x, o.x);
    const float y0 = std::max(y, o.y);
    const float x1 = std::min(right(), o.right());
    const float y1 = std::min(bottom(), o.bottom());
    return {x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f)};
}

namespace {

std::uint8_t toByte(float v)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

Color3 sampleColor(const ColorSequence& seq, float t)
{
    const auto& k = seq.keypoints;
    if (k.empty())
        return {1, 1, 1};
    if (t <= k.front().time)
        return k.front().value;
    for (std::size_t i = 1; i < k.size(); ++i) {
        if (t <= k[i].time) {
            const float span = k[i].time - k[i - 1].time;
            const float f = span > 0 ? (t - k[i - 1].time) / span : 0.0f;
            const Color3 a = k[i - 1].value;
            const Color3 b = k[i].value;
            return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
        }
    }
    return k.back().value;
}

float sampleNumber(const NumberSequence& seq, float t)
{
    const auto& k = seq.keypoints;
    if (k.empty())
        return 0.0f;
    if (t <= k.front().time)
        return k.front().value;
    for (std::size_t i = 1; i < k.size(); ++i) {
        if (t <= k[i].time) {
            const float span = k[i].time - k[i - 1].time;
            const float f = span > 0 ? (t - k[i - 1].time) / span : 0.0f;
            return k[i - 1].value + (k[i].value - k[i - 1].value) * f;
        }
    }
    return k.back().value;
}

// Grows a coverage bitmap by radius pixels (max over a disc), for strokes.
// The bitmap grown by `radius` pixels, each pixel the strongest coverage within
// that round reach (dx*dx + dy*dy <= radius*radius + radius). Done by rows: the
// strongest of the pixels within `h` to the left or right, made one width at a
// time (each extends the one before by two reads), then for every row offset
// the row of the width that offset's reach allows.
GlyphBitmap dilate(const GlyphBitmap& in, int radius)
{
    GlyphBitmap out;
    out.width = in.width + 2 * radius;
    out.height = in.height + 2 * radius;
    out.left = in.left - radius;
    out.top = in.top + radius;
    const auto width = static_cast<std::size_t>(out.width);
    out.coverage.assign(width * static_cast<std::size_t>(out.height), 0);
    if (in.width <= 0 || in.height <= 0)
        return out;

    // The widest reach at each row offset, and the widths worth keeping.
    std::vector<int> reach(static_cast<std::size_t>(2 * radius + 1));
    int widest = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        const int limit = radius * radius + radius - dy * dy;
        int h = 0;
        while ((h + 1) * (h + 1) <= limit)
            ++h;
        reach[static_cast<std::size_t>(dy + radius)] = h;
        widest = std::max(widest, h);
    }

    // across[h]: each source row laid out in the output's columns, each pixel
    // the strongest within h columns either side.
    std::vector<std::vector<std::uint8_t>> across(static_cast<std::size_t>(widest + 1));
    across[0].assign(width * static_cast<std::size_t>(in.height), 0);
    for (int sy = 0; sy < in.height; ++sy)
        std::copy_n(in.coverage.data() + static_cast<std::size_t>(sy) * static_cast<std::size_t>(in.width), in.width,
                    across[0].data() + static_cast<std::size_t>(sy) * width + static_cast<std::size_t>(radius));
    for (int h = 1; h <= widest; ++h) {
        across[static_cast<std::size_t>(h)] = across[static_cast<std::size_t>(h - 1)];
        for (int sy = 0; sy < in.height; ++sy) {
            const std::uint8_t* base = across[0].data() + static_cast<std::size_t>(sy) * width;
            std::uint8_t* row = across[static_cast<std::size_t>(h)].data() + static_cast<std::size_t>(sy) * width;
            for (int x = 0; x < out.width; ++x) {
                int best = row[x];
                if (x - h >= 0)
                    best = std::max<int>(best, base[x - h]);
                if (x + h < out.width)
                    best = std::max<int>(best, base[x + h]);
                row[x] = static_cast<std::uint8_t>(best);
            }
        }
    }

    for (int y = 0; y < out.height; ++y) {
        std::uint8_t* dst = out.coverage.data() + static_cast<std::size_t>(y) * width;
        for (int dy = -radius; dy <= radius; ++dy) {
            const int sy = y - radius + dy;
            if (sy < 0 || sy >= in.height)
                continue;
            const std::uint8_t* row = across[static_cast<std::size_t>(reach[static_cast<std::size_t>(dy + radius)])]
                                          .data()
                                      + static_cast<std::size_t>(sy) * width;
            for (int x = 0; x < out.width; ++x)
                dst[x] = std::max(dst[x], row[x]);
        }
    }
    return out;
}

} // namespace

GradientRamp GradientRamp::from(const ColorSequence& color, const NumberSequence& transparency)
{
    GradientRamp ramp;
    for (std::uint32_t i = 0; i < width; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(width - 1);
        const Color3 c = sampleColor(color, t);
        const float alpha = 1.0f - sampleNumber(transparency, t);
        ramp.rgba[i * 4 + 0] = toByte(c.r);
        ramp.rgba[i * 4 + 1] = toByte(c.g);
        ramp.rgba[i * 4 + 2] = toByte(c.b);
        ramp.rgba[i * 4 + 3] = toByte(alpha);
    }
    return ramp;
}

// --- GlyphAtlas ------------------------------------------------------------------------

// The pixels come with the first glyph: a GUI that's never drawn (--headless)
// measures text without them.
GlyphAtlas::GlyphAtlas() = default;

std::size_t GlyphAtlas::KeyHash::operator()(const Key& k) const
{
    auto h = static_cast<std::size_t>(std::bit_cast<std::uintptr_t>(k.face));
    h = h * 31 + k.glyph;
    h = h * 31 + k.size;
    h = h * 31 + static_cast<std::size_t>(k.stroke);
    h = h * 31 + static_cast<std::size_t>(k.subpixel);
    return h;
}

void GlyphAtlas::beginFrame()
{
    ++frame_;
    if (full_)
        repack();
}

// Packs the glyphs the last frame drew again, tallest first, and forgets the rest.
void GlyphAtlas::repack()
{
    full_ = false;
    if (pixels_.empty())
        pixels_.assign(static_cast<std::size_t>(size) * size, 0);
    const std::vector<std::uint8_t> old = pixels_;
    std::ranges::fill(pixels_, std::uint8_t{0});
    shelfX_ = shelfY_ = shelfHeight_ = 0;
    std::vector<Slot*> kept;
    for (auto it = entries_.begin(); it != entries_.end();) {
        Slot& slot = it->second;
        if (slot.used + 1 < frame_) {
            it = entries_.erase(it);
            continue;
        }
        if (slot.entry)
            kept.push_back(&slot);
        ++it;
    }
    std::ranges::sort(kept, std::greater{}, [](const Slot* s) { return s->entry->height; });
    std::mdspan from(old.data(), size, size);
    std::mdspan to(pixels_.data(), size, size);
    for (Slot* slot : kept) {
        Entry& e = *slot->entry;
        int x = 0;
        int y = 0;
        if (!allocate(e.width, e.height, x, y)) {
            slot->used = 0; // gone at the next repack; asked again, it's rasterized again
            slot->entry.reset();
            continue;
        }
        for (int row = 0; row < e.height; ++row)
            for (int col = 0; col < e.width; ++col)
                to[y + row, x + col] = from[e.y + row, e.x + col];
        e.x = x;
        e.y = y;
    }
    std::erase_if(entries_, [](const auto& item) { return !item.second.entry && item.second.used == 0; });
    dirtyBegin_ = 0;
    dirtyEnd_ = size;
    ++generation_;
}

bool GlyphAtlas::allocate(int w, int h, int& x, int& y)
{
    constexpr int padding = 1; // keeps bilinear sampling from bleeding between glyphs
    if (w + padding > size || h + padding > size)
        return false;
    if (shelfX_ + w + padding > size) {
        shelfY_ += shelfHeight_;
        shelfX_ = 0;
        shelfHeight_ = 0;
    }
    if (shelfY_ + h + padding > size)
        return false;
    x = shelfX_;
    y = shelfY_;
    shelfX_ += w + padding;
    shelfHeight_ = std::max(shelfHeight_, h + padding);
    return true;
}

const GlyphAtlas::Entry* GlyphAtlas::get(const FontFace* face, std::uint16_t glyph, float pixelsPerUnit,
                                         int strokeRadius, int subpixel)
{
    if (!face)
        return nullptr;
    // std::lround of the scale in 2^-20ths, never negative, without the
    // libm call: the float times 2^20 is exact, and plus a half in a double.
    const Key key{face, glyph, static_cast<std::uint32_t>(static_cast<double>(pixelsPerUnit * 1048576.0f) + 0.5),
                  strokeRadius, subpixel};
    if (auto it = entries_.find(key); it != entries_.end()) {
        it->second.used = frame_;
        return it->second.entry ? &*it->second.entry : nullptr;
    }

    ++rasterized_;
    const auto rasterStart = std::chrono::steady_clock::now();
    GlyphBitmap bitmap = rasterize(face->outline(glyph), pixelsPerUnit,
                                   static_cast<float>(subpixel) / static_cast<float>(subpixelSteps));
    if (strokeRadius > 0 && bitmap.width > 0)
        bitmap = dilate(bitmap, strokeRadius);
    {
        const double ms
            = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rasterStart).count();
        rasterStats_.milliseconds += ms;
        if (ms > rasterStats_.worstMilliseconds)
            rasterStats_ = {rasterStats_.milliseconds, ms, pixelsPerUnit * static_cast<float>(face->unitsPerEm()),
                            strokeRadius, bitmap.width, bitmap.height};
    }
    if (bitmap.width <= 0 || bitmap.height <= 0) {
        entries_.emplace(key, Slot{std::nullopt, frame_});
        return nullptr;
    }

    int x = 0;
    int y = 0;
    if (!allocate(bitmap.width, bitmap.height, x, y)) {
        full_ = true; // left out of this frame; the next one makes room
        return nullptr;
    }
    if (pixels_.empty())
        pixels_.assign(static_cast<std::size_t>(size) * size, 0);
    std::mdspan atlas(pixels_.data(), size, size);
    for (int row = 0; row < bitmap.height; ++row)
        for (int col = 0; col < bitmap.width; ++col)
            atlas[y + row, x + col]
                = bitmap.coverage[(static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.width))
                                  + static_cast<std::size_t>(col)];
    dirtyBegin_ = std::min(dirtyBegin_, y);
    dirtyEnd_ = std::max(dirtyEnd_, y + bitmap.height);
    Slot& slot = entries_[key];
    slot = {Entry{x, y, bitmap.width, bitmap.height, bitmap.left, bitmap.top}, frame_};
    return &*slot.entry;
}

std::pair<int, int> GlyphAtlas::takeDirty()
{
    if (dirtyBegin_ >= dirtyEnd_)
        return {0, 0};
    const std::pair<int, int> dirty{dirtyBegin_, dirtyEnd_};
    dirtyBegin_ = size;
    dirtyEnd_ = 0;
    return dirty;
}

// --- DrawList --------------------------------------------------------------------------

void DrawList::clear()
{
    quads.clear();
    textures.clear();
    gradients.clear();
    layers.clear();
    textureSlots_.clear();
    gradientSlots_.clear();
}

void DrawList::rect(const RectF& r, Color4 color, const RectF& clip)
{
    if (color.a <= 0.0f)
        return;
    Quad q;
    q.rect = r;
    q.color = color;
    q.clip = clip;
    quads.push_back(q);
}

std::uint32_t DrawList::texture(const std::shared_ptr<const Image>& image)
{
    if (!image || image->empty())
        return 0;
    if (auto it = textureSlots_.find(image.get()); it != textureSlots_.end())
        return it->second;
    textures.push_back(image);
    const auto slot = static_cast<std::uint32_t>(textures.size());
    textureSlots_.emplace(image.get(), slot);
    return slot;
}

std::uint32_t DrawList::gradient(const GradientRamp& ramp)
{
    // Objects with the same gradient share its row: the renderer has room for
    // UiRenderer::maxGradients of them, and past that a quad draws without one.
    const std::string_view bytes(reinterpret_cast<const char*>(ramp.rgba.data()), ramp.rgba.size());
    const std::size_t hash = std::hash<std::string_view>{}(bytes);
    auto [first, last] = gradientSlots_.equal_range(hash);
    for (auto it = first; it != last; ++it)
        if (gradients[it->second - 1].rgba == ramp.rgba)
            return it->second;
    gradients.push_back(ramp);
    const auto slot = static_cast<std::uint32_t>(gradients.size());
    gradientSlots_.emplace(hash, slot);
    return slot;
}

void DrawList::text(GlyphAtlas& atlas, const TextLayout& layout, float originX, float originY, const RectF& clip,
                    Color4 textStroke, float rotation, float pivotX, float pivotY, float strokeWidth)
{
    const float inv = 1.0f / static_cast<float>(GlyphAtlas::size);
    // Pen positions split into a whole pixel and one of the atlas's subpixel
    // steps. Big glyphs keep to whole pixels: a quarter pixel doesn't show at
    // that size, and four copies of each would crowd the atlas.
    auto split = [](float x, int& subpixel, const PlacedGlyph& g) {
        if (g.face && g.pixelsPerUnit * static_cast<float>(g.face->unitsPerEm()) >= 32.0f) {
            subpixel = 0;
            return std::round(x);
        }
        const float whole = std::floor(x);
        // std::lround of what's never negative, without the libm call: a
        // float plus a half is exact in a double.
        subpixel = static_cast<int>(static_cast<double>((x - whole) * GlyphAtlas::subpixelSteps) + 0.5);
        if (subpixel == GlyphAtlas::subpixelSteps) {
            subpixel = 0;
            return whole + 1.0f;
        }
        return whole;
    };
    auto glyphQuad = [&](const GlyphAtlas::Entry& e, float wholeX, float penY, Color4 color) {
        Quad q;
        // Glyph bitmaps map 1:1 to screen pixels.
        const float x = wholeX + static_cast<float>(e.left);
        const float y = std::round(originY + penY) - static_cast<float>(e.top);
        q.rect = {x, y, static_cast<float>(e.width), static_cast<float>(e.height)};
        q.uv = {static_cast<float>(e.x) * inv, static_cast<float>(e.y) * inv, static_cast<float>(e.x + e.width) * inv,
                static_cast<float>(e.y + e.height) * inv};
        q.color = color;
        q.kind = QuadKind::Glyph;
        q.clip = clip;
        q.rotation = rotation;
        q.pivotX = pivotX;
        q.pivotY = pivotY;
        quads.push_back(q);
    };

    // A color glyph draws as its layers, bottom first.
    auto layersOf = [](const PlacedGlyph& g) {
        return g.face ? g.face->colorLayers(g.glyph) : std::span<const FontFace::ColorLayer>{};
    };

    // Strokes under every glyph first, so neighbours' strokes don't cover letters.
    const int strokeRadius = std::max(1, static_cast<int>(std::lround(strokeWidth)));
    for (const PlacedGlyph& g : layout.glyphs) {
        const TextStyle& style = layout.styles[g.run];
        Color4 stroke = textStroke;
        int radius = strokeRadius;
        if (style.strokeColor) {
            stroke = Color4::from(*style.strokeColor, style.strokeTransparency);
            radius = std::max(1, static_cast<int>(std::lround(style.strokeThickness)));
        }
        stroke.a *= 1.0f - style.transparency;
        if (stroke.a <= 0.0f)
            continue;
        int subpixel = 0;
        const float whole = split(originX + g.x, subpixel, g);
        if (const GlyphAtlas::Entry* e = atlas.get(g.face, g.glyph, g.pixelsPerUnit, radius, subpixel))
            glyphQuad(*e, whole, g.y, stroke);
        for (const FontFace::ColorLayer& layer : layersOf(g))
            if (const GlyphAtlas::Entry* e = atlas.get(g.face, layer.glyph, g.pixelsPerUnit, radius, subpixel))
                glyphQuad(*e, whole, g.y, stroke);
    }
    for (const PlacedGlyph& g : layout.glyphs) {
        const TextStyle& style = layout.styles[g.run];
        const Color4 color = Color4::from(style.color, style.transparency);
        if (color.a <= 0.0f)
            continue;
        int subpixel = 0;
        const float whole = split(originX + g.x, subpixel, g);
        if (const GlyphAtlas::Entry* e = atlas.get(g.face, g.glyph, g.pixelsPerUnit, 0, subpixel))
            glyphQuad(*e, whole, g.y, color);
        for (const FontFace::ColorLayer& layer : layersOf(g))
            if (const GlyphAtlas::Entry* e = atlas.get(g.face, layer.glyph, g.pixelsPerUnit, 0, subpixel))
                glyphQuad(*e, whole, g.y,
                          layer.foreground ? color : Color4{layer.r, layer.g, layer.b, layer.a * color.a});
    }
    for (const TextDecoration& d : layout.decorations) {
        const TextStyle& style = layout.styles[d.run];
        Quad q;
        q.rect = {std::round(originX + d.x0), std::round(originY + d.y), std::round(d.x1 - d.x0), d.thickness};
        q.color = Color4::from(style.color, style.transparency);
        q.clip = clip;
        q.rotation = rotation;
        q.pivotX = pivotX;
        q.pivotY = pivotY;
        quads.push_back(q);
    }
}

} // namespace lunar::ui
