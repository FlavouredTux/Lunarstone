#include <lunar/render/Ribbons.hpp>

#include <lunar/Classes.hpp>
#include <lunar/render/Frustum.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace lunar::render {

namespace {

// The sequences at time t in [0, 1], linear between keypoints (sorted by
// time). Colours mix in sRGB. Envelopes don't apply to beams and trails.
float sequenceAt(const NumberSequence& sequence, float t)
{
    const auto& k = sequence.keypoints;
    if (k.empty())
        return 0.0f;
    if (t <= k.front().time)
        return k.front().value;
    for (std::size_t i = 1; i < k.size(); ++i)
        if (t <= k[i].time) {
            const float span = k[i].time - k[i - 1].time;
            const float f = span > 0.0f ? (t - k[i - 1].time) / span : 1.0f;
            return k[i - 1].value + (k[i].value - k[i - 1].value) * f;
        }
    return k.back().value;
}

Color3 sequenceAt(const ColorSequence& sequence, float t)
{
    const auto& k = sequence.keypoints;
    if (k.empty())
        return {1.0f, 1.0f, 1.0f};
    if (t <= k.front().time)
        return k.front().value;
    for (std::size_t i = 1; i < k.size(); ++i)
        if (t <= k[i].time) {
            const float span = k[i].time - k[i - 1].time;
            const float f = span > 0.0f ? (t - k[i - 1].time) / span : 1.0f;
            const Color3 a = k[i - 1].value;
            const Color3 b = k[i].value;
            return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
        }
    return k.back().value;
}

Vector3 lerp(Vector3 a, Vector3 b, float t)
{
    return a + (b - a) * t;
}

Vector3 normalized(Vector3 v, Vector3 fallback)
{
    const float length = v.magnitude();
    return length > 1e-6f ? v / length : fallback;
}

// A rotation as a unit quaternion, for turning one attachment's frame into
// the other's along a beam.
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    static Quat of(const CFrame& frame)
    {
        const auto& r = frame.r;
        Quat q;
        const float trace = r[0][0] + r[1][1] + r[2][2];
        if (trace > 0.0f) {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q = {(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, 0.25f * s};
        } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
            const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
            q = {0.25f * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s, (r[2][1] - r[1][2]) / s};
        } else if (r[1][1] > r[2][2]) {
            const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
            q = {(r[0][1] + r[1][0]) / s, 0.25f * s, (r[1][2] + r[2][1]) / s, (r[0][2] - r[2][0]) / s};
        } else {
            const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
            q = {(r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25f * s, (r[1][0] - r[0][1]) / s};
        }
        const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return length > 0.0f ? Quat{q.x / length, q.y / length, q.z / length, q.w / length} : Quat{};
    }

    // The shorter way round from a to b.
    static Quat slerp(Quat a, Quat b, float t)
    {
        float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (cosine < 0.0f) {
            b = {-b.x, -b.y, -b.z, -b.w};
            cosine = -cosine;
        }
        float wa = 1.0f - t;
        float wb = t;
        if (cosine < 0.9995f) {
            const float angle = std::acos(cosine);
            const float sine = std::sin(angle);
            wa = std::sin((1.0f - t) * angle) / sine;
            wb = std::sin(t * angle) / sine;
        }
        Quat q{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
        const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return {q.x / length, q.y / length, q.z / length, q.w / length};
    }

    // The rotated Y axis (the rotation matrix's second column).
    Vector3 up() const { return {2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w)}; }
};

// One cross-section of a ribbon: the middle, half the width across it, and
// how it looks there.
struct Section {
    Vector3 middle;
    Vector3 half;
    float u = 0.0f;
    Color3 color;
    float opacity = 1.0f;
    bool start = false; // no quad joins it to the section before
};

// Quads between neighbouring sections as one batch: the half-width vector's
// + side gets v = 0, its - side v = 1, and every vertex q = its section's
// half width (EffectVertex::uvq).
bool appendSections(std::span<const Section> sections, const ImagePtr& texture, float lightEmission,
                    float lightInfluence, float brightness, EffectGeometry& out)
{
    if (sections.size() < 2)
        return false;
    bool visible = false;
    float widest = 0.0f;
    Vector3 low = sections.front().middle;
    Vector3 high = low;
    for (const Section& s : sections) {
        visible = visible || s.opacity > 0.0f;
        widest = std::max(widest, s.half.magnitude());
        low = {std::min(low.x, s.middle.x), std::min(low.y, s.middle.y), std::min(low.z, s.middle.z)};
        high = {std::max(high.x, s.middle.x), std::max(high.y, s.middle.y), std::max(high.z, s.middle.z)};
    }
    if (!visible || widest <= 0.0f)
        return false;

    const auto first = static_cast<std::uint32_t>(out.vertices.size());
    const auto firstIndex = static_cast<std::uint32_t>(out.indices.size());
    // q may not be 0: where a ribbon narrows to a point it's a sliver of the widest.
    const float minQ = widest * 1e-3f;
    for (const Section& s : sections) {
        const float q = std::max(s.half.magnitude(), minQ);
        for (const float side : {1.0f, -1.0f}) {
            EffectVertex& v = out.vertices.emplace_back();
            v.position = s.middle + s.half * side;
            const float vCoord = side > 0.0f ? 0.0f : 1.0f;
            v.uvq[0] = s.u * q;
            v.uvq[1] = vCoord * q;
            v.uvq[2] = q;
            v.color[0] = s.color.r;
            v.color[1] = s.color.g;
            v.color[2] = s.color.b;
            v.color[3] = std::clamp(s.opacity, 0.0f, 1.0f);
        }
    }
    for (std::uint32_t i = 1; i < sections.size(); ++i) {
        if (sections[i].start)
            continue;
        const std::uint32_t a = first + 2 * (i - 1); // + side of the section before
        const std::uint32_t b = a + 1;
        const std::uint32_t c = a + 2;
        const std::uint32_t d = a + 3;
        for (std::uint32_t index : {a, b, c, b, d, c})
            out.indices.push_back(index);
    }
    const auto count = static_cast<std::uint32_t>(out.indices.size()) - firstIndex;
    if (count == 0) {
        out.vertices.resize(first);
        return false;
    }
    EffectBatch& batch = out.batches.emplace_back();
    batch.firstIndex = firstIndex;
    batch.indexCount = count;
    batch.texture = texture;
    batch.lightEmission = lightEmission;
    batch.lightInfluence = lightInfluence;
    batch.brightness = brightness;
    batch.center = (low + high) * 0.5f;
    batch.radius = (high - low).magnitude() * 0.5f + widest;
    return true;
}

// The client's BeamNode thins a beam's segments with distance: all of them
// within 200 studs of the nearer... farther end, a tenth from 920 on.
int beamSegments(int segments, float distance)
{
    const float scale = std::clamp(1.0f + (distance - 200.0f) / -800.0f, 0.1f, 1.0f);
    return std::max(1, static_cast<int>(static_cast<float>(std::clamp(segments, 1, 1000)) * scale));
}

} // namespace

// --- TrailHistory ------------------------------------------------------------------

void TrailHistory::update(Vector3 p0, Vector3 p1, double time, const Settings& settings)
{
    const float lifetime = std::clamp(settings.lifetime, 0.01f, 20.0f);
    if (following_) {
        points_.pop_back();
        following_ = false;
    }
    if (settings.enabled) {
        Point now{p0, p1, time, 0.0f, !wasEnabled_ || points_.empty()};
        if (points_.empty()) {
            points_.push_back(now);
        } else {
            const Point& last = points_.back();
            const Vector3 middle = (p0 + p1) * 0.5f;
            now.traveled = last.traveled + (middle - (last.p0 + last.p1) * 0.5f).magnitude();
            // A new segment once either attachment is MinLength from the
            // last one laid; until then the newest just follows them.
            const float moved = std::max((p0 - last.p0).magnitude(), (p1 - last.p1).magnitude());
            points_.push_back(now);
            following_ = !now.start && moved < settings.minLength;
        }
    }
    wasEnabled_ = settings.enabled;
    expire(time, lifetime);
    if (settings.maxLength > 0.0f)
        shorten(settings.maxLength);
}

void TrailHistory::clear()
{
    points_.clear();
    following_ = false;
}

void TrailHistory::expire(double time, float lifetimeSeconds)
{
    const auto lifetime = static_cast<double>(lifetimeSeconds);
    // Ages fall towards the end of the list: k is the first one alive.
    std::size_t k = 0;
    while (k < points_.size() && time - points_[k].born > lifetime)
        ++k;
    if (k == 0)
        return;
    if (k == points_.size()) {
        clear();
        return;
    }
    Point& old = points_[k - 1];
    const Point& next = points_[k];
    if (next.start) {
        points_.erase(points_.begin(), points_.begin() + static_cast<std::ptrdiff_t>(k));
        points_.front().start = true;
        return;
    }
    // GUESS: the client shortens the oldest segment smoothly rather than
    // dropping it whole.
    const double span = next.born - old.born;
    const auto f = static_cast<float>(span > 0.0 ? (next.born - (time - lifetime)) / span : 0.0);
    old.p0 = lerp(next.p0, old.p0, f);
    old.p1 = lerp(next.p1, old.p1, f);
    old.traveled = next.traveled + (old.traveled - next.traveled) * f;
    old.born = time - lifetime;
    points_.erase(points_.begin(), points_.begin() + static_cast<std::ptrdiff_t>(k - 1));
}

void TrailHistory::shorten(float maxLength)
{
    float length = 0.0f;
    for (std::size_t i = points_.size(); i-- > 1;) {
        if (points_[i].start)
            continue; // a gap isn't trail
        const Point& newer = points_[i];
        Point& older = points_[i - 1];
        const Vector3 a = (newer.p0 + newer.p1) * 0.5f;
        const Vector3 b = (older.p0 + older.p1) * 0.5f;
        const float step = (b - a).magnitude();
        if (length + step <= maxLength) {
            length += step;
            continue;
        }
        const float f = step > 0.0f ? (maxLength - length) / step : 0.0f;
        older.p0 = lerp(newer.p0, older.p0, f);
        older.p1 = lerp(newer.p1, older.p1, f);
        older.born = newer.born + (older.born - newer.born) * static_cast<double>(f);
        older.traveled = newer.traveled + (older.traveled - newer.traveled) * f;
        older.start = true;
        points_.erase(points_.begin(), points_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        return;
    }
}

// --- Geometry ---------------------------------------------------------------------

bool appendBeam(const Beam& beam, const CFrame& a0, const CFrame& a1, const RibbonView& view, float scroll,
                const ImagePtr& texture, EffectGeometry& out)
{
    const float width0 = std::max(beam.width0(), 0.0f);
    const float width1 = std::max(beam.width1(), 0.0f);
    if (width0 <= 0.0f && width1 <= 0.0f)
        return false;
    // The cubic Bezier through the attachments, its inner control points
    // CurveSize0 along Attachment0's X axis and CurveSize1 back along
    // Attachment1's.
    const Vector3 p0 = a0.position;
    const Vector3 p3 = a1.position;
    const Vector3 p1 = p0 + a0.rightVector() * beam.curveSize0();
    const Vector3 p2 = p3 - a1.rightVector() * beam.curveSize1();
    const bool straight = beam.curveSize0() == 0.0f && beam.curveSize1() == 0.0f;
    const int segments
        = beamSegments(beam.segments(), std::max((p0 - view.eye).magnitude(), (p3 - view.eye).magnitude()));

    // Points along it at even steps of the curve's parameter, those closer
    // than 0.05 studs to the last dropped (but the end), as the client's
    // BeamNode does; then measured along the polyline, whose length
    // fraction is what colour, transparency and width follow.
    constexpr float closest = 0.05f;
    // Reused from beam to beam: most of a frame's beams are a few sections.
    thread_local std::vector<Section> sections;
    thread_local std::vector<float> along;
    sections.resize(static_cast<std::size_t>(segments) + 1);
    along.resize(sections.size());
    std::size_t count = 0;
    float length = 0.0f;
    auto bezier = [&](float t) {
        const float s = 1.0f - t;
        return p0 * (s * s * s) + p1 * (3.0f * s * s * t) + p2 * (3.0f * s * t * t) + p3 * (t * t * t);
    };
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const Vector3 p = straight ? lerp(p0, p3, t) : bezier(t);
        if (count > 0) {
            const float step = (p - sections[count - 1].middle).magnitude();
            if (step < closest && i != segments)
                continue;
            length += step;
        }
        sections[count] = {.middle = p};
        along[count] = length;
        ++count;
    }
    if (count < 2 || length <= 1.1920929e-07f)
        return false;

    // Stretch repeats the texture TextureLength times along the beam; Wrap
    // and Static every TextureLength studs. TextureSpeed moves it from
    // Attachment0 towards Attachment1.
    const float textureLength = beam.textureLength();
    const float perStud = beam.textureMode() == Enum::TextureMode::Stretch
                              ? textureLength / length
                              : (textureLength > 0.0f ? 1.0f / textureLength : 0.0f);
    const float opacityScale = 1.0f - std::clamp(beam.localTransparencyModifier(), 0.0f, 1.0f);
    const Quat q0 = Quat::of(a0);
    const Quat q1 = Quat::of(a1);
    // Most beams' attachments are turned alike: no turning between them.
    const bool turns = std::abs(q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w) < 0.99999f;
    const Vector3 up0 = q0.up();
    const Vector3 towardsCamera = -view.forward * beam.zOffset();
    for (std::size_t i = 0; i < count; ++i) {
        Section& s = sections[i];
        const float t = along[i] / length;
        const float width = width0 + (width1 - width0) * t;
        Vector3 across;
        if (beam.faceCamera()) {
            // Square to the curve and to the camera.
            const Vector3 tangent = sections[std::min(i + 1, count - 1)].middle - sections[i > 0 ? i - 1 : 0].middle;
            across = normalized(tangent.cross(view.eye - s.middle), a0.upVector());
        } else {
            // Across the attachments' Y axes, turned from one to the other
            // along the beam (BeamNode slerps their rotations).
            across = turns ? Quat::slerp(q0, q1, t).up() : up0;
        }
        s.half = across * (width * 0.5f);
        s.middle = s.middle + towardsCamera;
        s.u = along[i] * perStud - scroll;
        s.color = sequenceAt(beam.color(), t);
        s.opacity = (1.0f - std::clamp(sequenceAt(beam.transparency(), t), 0.0f, 1.0f)) * opacityScale;
    }
    return appendSections(std::span(sections).first(count), texture, beam.lightEmission(), beam.lightInfluence(),
                          beam.brightness(), out);
}

bool appendTrail(const Trail& trail, const TrailHistory& history, double time, const RibbonView& view,
                 const ImagePtr& texture, EffectGeometry& out)
{
    const std::span<const TrailHistory::Point> points = history.points();
    if (points.size() < 2)
        return false;
    const float lifetime = std::clamp(trail.lifetime(), 0.01f, 20.0f);
    thread_local std::vector<Section> sections;
    thread_local std::vector<float> fromHead; // studs from the newest point back along it (not across gaps)
    sections.assign(points.size(), {});
    fromHead.resize(points.size());
    float total = 0.0f;
    for (std::size_t i = points.size(); i-- > 0;) {
        sections[i].middle = (points[i].p0 + points[i].p1) * 0.5f;
        if (i + 1 < points.size() && !points[i + 1].start)
            total += (sections[i + 1].middle - sections[i].middle).magnitude();
        fromHead[i] = total;
    }
    if (total <= 1e-4f)
        return false; // it hasn't moved
    const float textureLength = trail.textureLength();
    const float opacityScale = 1.0f - std::clamp(trail.localTransparencyModifier(), 0.0f, 1.0f);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const TrailHistory::Point& p = points[i];
        Section& s = sections[i];
        s.start = p.start;
        // Colour, transparency and width follow each point's age.
        const float age = std::clamp(static_cast<float>(time - p.born) / lifetime, 0.0f, 1.0f);
        const float scale = std::clamp(sequenceAt(trail.widthScale(), age), 0.0f, 1.0f);
        const Vector3 span = p.p1 - p.p0;
        if (trail.faceCamera()) {
            // As wide as the attachments are apart, turned about the trail
            // to face the camera, Attachment1's side kept on the + side.
            const bool before = i > 0 && !p.start;
            const bool after = i + 1 < points.size() && !points[i + 1].start;
            const Vector3 tangent = sections[after ? i + 1 : i].middle - sections[before ? i - 1 : i].middle;
            Vector3 across = normalized(tangent.cross(view.eye - s.middle), normalized(span, {0.0f, 1.0f, 0.0f}));
            if (across.dot(span) < 0.0f)
                across = -across;
            s.half = across * (span.magnitude() * 0.5f * scale);
        } else {
            s.half = span * (0.5f * scale);
        }
        // Stretch spreads TextureLength repeats over the trail as it is now;
        // Wrap tiles every TextureLength studs from the attachments back;
        // Static pins the tiles where they were laid. GUESS: u grows
        // towards the tail in all three.
        switch (trail.textureMode()) {
        case Enum::TextureMode::Stretch:
            s.u = total > 0.0f ? fromHead[i] / total * textureLength : 0.0f;
            break;
        case Enum::TextureMode::Wrap:
            s.u = textureLength > 0.0f ? fromHead[i] / textureLength : 0.0f;
            break;
        case Enum::TextureMode::Static:
            s.u = textureLength > 0.0f ? -p.traveled / textureLength : 0.0f;
            break;
        }
        s.color = sequenceAt(trail.color(), age);
        s.opacity = (1.0f - std::clamp(sequenceAt(trail.transparency(), age), 0.0f, 1.0f)) * opacityScale;
    }
    return appendSections(sections, texture, trail.lightEmission(), trail.lightInfluence(), trail.brightness(), out);
}

// --- Ribbons ------------------------------------------------------------------------

Ribbons::Ribbons(Instance& workspace, TextureProvider textures) : workspace_(workspace), textures_(std::move(textures))
{
    for (const InstancePtr& d : workspace.descendants())
        add(d.get());
    added_ = workspace.descendantAdded.connect([this](Instance* instance) { add(instance); });
    removing_ = workspace.descendantRemoving.connect([this](Instance* instance) { remove(instance); });
}

Ribbons::~Ribbons()
{
    added_.disconnect();
    removing_.disconnect();
}

void Ribbons::add(Instance* instance)
{
    const bool beam = instanceCast<Beam>(instance) != nullptr;
    const bool trail = !beam && instanceCast<Trail>(instance) != nullptr;
    if (!beam && !trail)
        return;
    if (!index_.try_emplace(instance, entries_.size()).second)
        return;
    Entry& entry = entries_.emplace_back();
    entry.key = instance;
    entry.instance = instance->weak_from_this();
    entry.trail = trail;
    ++(trail ? trails_ : beams_);
    // Asked for now, so it's loading (or loaded, for a caller that waits)
    // before the ribbon first shows.
    textureOf(entry,
              trail ? static_cast<const Trail*>(instance)->texture() : static_cast<const Beam*>(instance)->texture());
}

void Ribbons::remove(Instance* instance)
{
    if (auto it = index_.find(instance); it != index_.end())
        drop(it->second);
}

void Ribbons::drop(std::size_t i)
{
    --(entries_[i].trail ? trails_ : beams_);
    index_.erase(entries_[i].key);
    if (i + 1 != entries_.size()) {
        entries_[i] = std::move(entries_.back());
        index_[entries_[i].key] = i;
    }
    entries_.pop_back();
}

CFrame Ribbons::placed(const Attachment& attachment) const
{
    CFrame frame = attachment.worldCFrame();
    if (moving_ && attachment.isDescendantOf(moving_))
        frame.position = frame.position + offset_;
    return frame;
}

const ImagePtr& Ribbons::textureOf(Entry& entry, const std::string& id)
{
    if (id != entry.textureId) {
        entry.textureId = id;
        entry.texture = nullptr;
    }
    if (!entry.texture && !id.empty() && textures_)
        entry.texture = textures_(id);
    return entry.texture;
}

void Ribbons::collect(const EffectView& view, EffectGeometry& out)
{
    const Frustum frustum(view.viewProjection);
    const RibbonView ribbonView{view.camera.cframe.position, view.camera.cframe.lookVector()};
    const float reach2 = view.drawDistance * view.drawDistance;
    auto shows = [&](Vector3 a, Vector3 b, float slack) {
        // The sphere around the two ends, grown by how far the ribbon may
        // stray from their line.
        const Vector3 center = (a + b) * 0.5f;
        const float radius = (b - a).magnitude() * 0.5f + slack;
        const Vector3 offset = center - ribbonView.eye;
        const float reach = std::sqrt(reach2) + radius;
        return offset.dot(offset) <= reach * reach && frustum.intersects(center, radius);
    };
    beamsDrawn_ = 0;
    trailsDrawn_ = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const std::shared_ptr<const Instance> instance = entries_[i].instance.lock();
        if (!instance) {
            drop(i--);
            continue;
        }
        Entry& entry = entries_[i];
        const bool seen = std::exchange(entry.seen, true);
        if (!entry.trail) {
            const auto& beam = static_cast<const Beam&>(*instance);
            // TextureSpeed's cycles add up from frame to frame;
            // SetTextureOffset puts the texture back where it says.
            if (beam.textureOffsetSets() != entry.offsetSets) {
                entry.offsetSets = beam.textureOffsetSets();
                entry.scroll = -beam.textureOffset();
            }
            const double elapsed = seen ? std::max(view.time - entry.scrolledAt, 0.0) : 0.0;
            entry.scrolledAt = view.time;
            entry.scroll = static_cast<float>(
                std::fmod(static_cast<double>(entry.scroll) + elapsed * static_cast<double>(beam.textureSpeed()), 1.0));
            const Attachment* a0 = beam.attachment0();
            const Attachment* a1 = beam.attachment1();
            if (!beam.enabled() || !a0 || !a1 || !a0->isDescendantOf(&workspace_) || !a1->isDescendantOf(&workspace_))
                continue;
            const CFrame f0 = placed(*a0);
            const CFrame f1 = placed(*a1);
            const float slack = std::max(std::abs(beam.curveSize0()), std::abs(beam.curveSize1()))
                                + std::max(beam.width0(), beam.width1()) * 0.5f + std::abs(beam.zOffset());
            if (!shows(f0.position, f1.position, slack))
                continue;
            if (appendBeam(beam, f0, f1, ribbonView, entry.scroll, textureOf(entry, beam.texture()), out))
                ++beamsDrawn_;
            continue;
        }

        // Trails remember every frame, seen or not.
        const auto& trail = static_cast<const Trail&>(*instance);
        const Attachment* a0 = trail.attachment0();
        const Attachment* a1 = trail.attachment1();
        if (a0 != entry.attachment0 || a1 != entry.attachment1 || trail.clears() != entry.clears
            || (seen && view.time < entry.updatedAt)) {
            entry.history.clear();
            entry.attachment0 = a0;
            entry.attachment1 = a1;
            entry.clears = trail.clears();
        }
        entry.updatedAt = view.time;
        const bool attached = a0 && a1 && a0->isDescendantOf(&workspace_) && a1->isDescendantOf(&workspace_);
        const Vector3 p0 = attached ? placed(*a0).position : Vector3{};
        const Vector3 p1 = attached ? placed(*a1).position : Vector3{};
        entry.history.update(p0, p1, view.time,
                             {trail.lifetime(), trail.minLength(), trail.maxLength(), trail.enabled() && attached});
        const std::span<const TrailHistory::Point> points = entry.history.points();
        if (points.size() < 2)
            continue;
        Vector3 low = points.front().p0;
        Vector3 high = low;
        for (const TrailHistory::Point& p : points)
            for (const Vector3 v : {p.p0, p.p1}) {
                low = {std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z)};
                high = {std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z)};
            }
        if (!shows(low, high, 0.0f))
            continue;
        if (appendTrail(trail, entry.history, view.time, ribbonView, textureOf(entry, trail.texture()), out))
            ++trailsDrawn_;
    }
}

} // namespace lunar::render
