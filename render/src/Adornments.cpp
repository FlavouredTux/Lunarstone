#include <lunar/render/Adornments.hpp>

#include <lunar/Classes.hpp>
#include <lunar/render/Frustum.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <optional>

namespace lunar::render {

namespace {

// Where an adornee is: its pivot (a part's CFrame, a model's pivot), and the
// box around it (the part itself, the model's bounding box).
struct Placed {
    CFrame pivot;
    CFrame box;
    Vector3 size;
};

std::optional<Placed> placeOf(const Instance* adornee, Vector3 offset)
{
    std::optional<Placed> placed;
    if (const auto* part = instanceCast<const BasePart>(adornee)) {
        placed = Placed{part->cframe(), part->cframe(), part->size()};
    } else if (const auto* model = instanceCast<const Model>(adornee)) {
        const auto [box, size] = model->getBoundingBox();
        placed = Placed{model->getPivot(), box, size};
    }
    if (placed) {
        placed->pivot.position = placed->pivot.position + offset;
        placed->box.position = placed->box.position + offset;
    }
    return placed;
}

const Instance* adorneeOf(const GuiBase3d& adornment)
{
    if (const auto* a = instanceCast<const InstanceAdornment>(&adornment))
        return a->adornee();
    if (const auto* a = instanceCast<const PVAdornment>(&adornment))
        return a->adornee();
    if (const auto* a = instanceCast<const PartAdornment>(&adornment))
        return a->adornee();
    return nullptr;
}

// One batch's triangles, flat coloured. The effect pipeline draws both sides
// of everything, so faces turned away from the eye are left out here: for
// these closed shapes that's the far side, which a see-through box would
// otherwise show through its near side.
class Builder {
public:
    Builder(EffectGeometry& out, const AdornView& view)
        : out_(out), view_(view), eye_(view.camera.position), firstIndex_(out.indices.size())
    {
    }

    // The colour what's added next takes; false when it's fully see-through.
    bool color(Color3 color, float transparency)
    {
        rgba_ = {color.r, color.g, color.b, 1.0f - std::clamp(transparency, 0.0f, 1.0f)};
        return rgba_[3] > 0.0f;
    }

    // A flat polygon, its corners in order around it, facing `normal`.
    void face(std::initializer_list<Vector3> corners, Vector3 normal, bool bothSides = false)
    {
        if (!bothSides && normal.dot(eye_ - *corners.begin()) <= 0.0f)
            return;
        const auto base = static_cast<std::uint32_t>(out_.vertices.size());
        for (const Vector3 p : corners)
            vertex(p, 0.0f, 0.0f);
        for (std::uint32_t i = 1; i + 1 < corners.size(); ++i)
            out_.indices.insert(out_.indices.end(), {base, base + i, base + i + 1});
    }

    // A box of `size` at `frame`.
    void box(const CFrame& frame, Vector3 size)
    {
        const std::array axes{frame.rightVector(), frame.upVector(), -frame.lookVector()};
        const std::array half{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
        for (std::size_t i = 0; i < 3; ++i)
            for (const float sign : {1.0f, -1.0f}) {
                const Vector3 normal = axes[i] * sign;
                const Vector3 center = frame.position + normal * half[i];
                const Vector3 u = axes[(i + 1) % 3] * half[(i + 1) % 3];
                const Vector3 v = axes[(i + 2) % 3] * half[(i + 2) % 3];
                face({center - u - v, center + u - v, center + u + v, center - u + v}, normal);
            }
    }

    void sphere(Vector3 center, float radius)
    {
        constexpr int rings = 12;
        constexpr int segments = 24;
        auto at = [&](float theta, float phi) {
            return Vector3{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
        };
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segments; ++j) {
                const float t0 = std::numbers::pi_v<float> * static_cast<float>(i) / rings;
                const float t1 = std::numbers::pi_v<float> * static_cast<float>(i + 1) / rings;
                const float p0 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(j) / segments;
                const float p1 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(j + 1) / segments;
                const Vector3 normal = at((t0 + t1) * 0.5f, (p0 + p1) * 0.5f);
                face({center + at(t0, p0) * radius, center + at(t0, p1) * radius, center + at(t1, p1) * radius,
                      center + at(t1, p0) * radius},
                     normal);
            }
    }

    // GUESS: along the frame's Z axis, centred on it, swept from its X axis
    // towards its Y axis through `degrees`; a tube when inner > 0.
    void cylinder(const CFrame& frame, float height, float radius, float inner, float degrees)
    {
        degrees = std::clamp(degrees, 0.0f, 360.0f);
        radius = std::max(radius, 0.0f);
        inner = std::clamp(inner, 0.0f, radius);
        if (degrees <= 0.0f || radius <= 0.0f)
            return;
        const int segments = std::max(3, static_cast<int>(std::ceil(32.0f * degrees / 360.0f)));
        const float sweep = degrees * std::numbers::pi_v<float> / 180.0f;
        const float h = height * 0.5f;
        auto point
            = [&](float r, float a, float z) { return frame.pointToWorldSpace({r * std::cos(a), r * std::sin(a), z}); };
        auto direction = [&](float x, float y, float z) { return frame.vectorToWorldSpace({x, y, z}); };
        for (int i = 0; i < segments; ++i) {
            const float a0 = sweep * static_cast<float>(i) / static_cast<float>(segments);
            const float a1 = sweep * static_cast<float>(i + 1) / static_cast<float>(segments);
            const float am = (a0 + a1) * 0.5f;
            const Vector3 out = direction(std::cos(am), std::sin(am), 0.0f);
            face({point(radius, a0, -h), point(radius, a1, -h), point(radius, a1, h), point(radius, a0, h)}, out);
            if (inner > 0.0f)
                face({point(inner, a0, -h), point(inner, a1, -h), point(inner, a1, h), point(inner, a0, h)}, -out);
            for (const float z : {h, -h}) {
                const Vector3 normal = direction(0.0f, 0.0f, z > 0.0f ? 1.0f : -1.0f);
                if (inner > 0.0f)
                    face({point(inner, a0, z), point(radius, a0, z), point(radius, a1, z), point(inner, a1, z)},
                         normal);
                else
                    face({point(0.0f, a0, z), point(radius, a0, z), point(radius, a1, z)}, normal);
            }
        }
        if (degrees < 360.0f) // the cut's two ends
            for (const float a : {0.0f, sweep}) {
                const float side = a == 0.0f ? -1.0f : 1.0f;
                const Vector3 normal = direction(-std::sin(a) * side, std::cos(a) * side, 0.0f);
                face({point(inner, a, -h), point(radius, a, -h), point(radius, a, h), point(inner, a, h)}, normal);
            }
    }

    // GUESS: its base round the frame's position, its tip `height` along the
    // LookVector. Hollow leaves the base off and shows the inside.
    void cone(const CFrame& frame, float height, float radius, bool hollow)
    {
        if (radius <= 0.0f || height == 0.0f)
            return;
        constexpr int segments = 32;
        const Vector3 tip = frame.pointToWorldSpace({0.0f, 0.0f, -height});
        auto rim = [&](float a) { return frame.pointToWorldSpace({radius * std::cos(a), radius * std::sin(a), 0.0f}); };
        for (int i = 0; i < segments; ++i) {
            const float a0 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / segments;
            const float a1 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i + 1) / segments;
            const float am = (a0 + a1) * 0.5f;
            const Vector3 normal = frame.vectorToWorldSpace({std::cos(am) * height, std::sin(am) * height, -radius});
            face({rim(a0), rim(a1), tip}, normal, hollow);
            if (!hollow)
                face({rim(a1), rim(a0), frame.position}, frame.vectorToWorldSpace({0.0f, 0.0f, 1.0f}));
        }
    }

    // `thickness` pixels wide on screen (a stud in a twentieth's place when
    // the frame's size isn't known), from the frame's position along its
    // LookVector (GUESS) for `length`, turned to face the eye.
    void line(const CFrame& frame, float length, float thickness)
    {
        const Vector3 p0 = frame.position;
        const Vector3 p1 = p0 + frame.lookVector() * length;
        auto width = [&](Vector3 p) {
            return view_.pixelsPerStud > 0.0f ? thickness * (p - eye_).magnitude() / view_.pixelsPerStud
                                              : thickness * 0.05f;
        };
        Vector3 side = (p1 - p0).cross(eye_ - (p0 + p1) * 0.5f);
        if (side.dot(side) < 1e-12f)
            return;
        side = side.unit() * 0.5f;
        const Vector3 s0 = side * width(p0);
        const Vector3 s1 = side * width(p1);
        face({p0 - s0, p0 + s0, p1 + s1, p1 - s1}, {}, true);
    }

    // An image `size` studs across at `center`, turned to face the eye
    // (GUESS: ImageHandleAdornment always faces the camera).
    void image(Vector3 center, Vector2 size)
    {
        const Vector3 right = view_.camera.rightVector() * (size.x * 0.5f);
        const Vector3 up = view_.camera.upVector() * (size.y * 0.5f);
        const auto base = static_cast<std::uint32_t>(out_.vertices.size());
        vertex(center - right + up, 0.0f, 0.0f);
        vertex(center + right + up, 1.0f, 0.0f);
        vertex(center + right - up, 1.0f, 1.0f);
        vertex(center - right - up, 0.0f, 1.0f);
        out_.indices.insert(out_.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    // Closes the batch; false when nothing was added (all of it turned away).
    bool finish(bool onTop, ImagePtr texture = nullptr)
    {
        if (out_.indices.size() == firstIndex_)
            return false;
        EffectBatch batch;
        batch.firstIndex = static_cast<std::uint32_t>(firstIndex_);
        batch.indexCount = static_cast<std::uint32_t>(out_.indices.size() - firstIndex_);
        batch.texture = std::move(texture);
        batch.lightInfluence = 0.0f;
        batch.brightness = 1.0f;
        batch.toneMapped = false; // the colour as given
        batch.clampUv = true;
        batch.onTop = onTop;
        batch.center = (low_ + high_) * 0.5f;
        batch.radius = (high_ - low_).magnitude() * 0.5f;
        out_.batches.push_back(batch);
        return true;
    }

private:
    void vertex(Vector3 p, float u, float v)
    {
        EffectVertex vertex;
        vertex.position = p;
        vertex.uvq[0] = u;
        vertex.uvq[1] = v;
        std::ranges::copy(rgba_, vertex.color);
        out_.vertices.push_back(vertex);
        low_ = {std::min(low_.x, p.x), std::min(low_.y, p.y), std::min(low_.z, p.z)};
        high_ = {std::max(high_.x, p.x), std::max(high_.y, p.y), std::max(high_.z, p.z)};
    }

    EffectGeometry& out_;
    const AdornView& view_;
    Vector3 eye_;
    std::size_t firstIndex_;
    std::array<float, 4> rgba_{1.0f, 1.0f, 1.0f, 1.0f};
    Vector3 low_{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
    Vector3 high_{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest()};
};

// A SelectionBox: its lines along the box's twelve edges, LineThickness
// thick (bars meeting at the corners), and the faces in SurfaceColor3 when
// SurfaceTransparency lets them show.
void selectionBox(Builder& b, const SelectionBox& box, const Placed& placed)
{
    if (b.color(box.surfaceColor3(), box.surfaceTransparency()))
        b.box(placed.box, placed.size + Vector3(0.02f, 0.02f, 0.02f));
    if (!b.color(box.color3(), box.transparency()))
        return;
    const float t = std::max(box.lineThickness(), 0.0f);
    if (t <= 0.0f)
        return;
    const std::array half{placed.size.x * 0.5f, placed.size.y * 0.5f, placed.size.z * 0.5f};
    for (std::size_t axis = 0; axis < 3; ++axis)
        for (const float s1 : {-1.0f, 1.0f})
            for (const float s2 : {-1.0f, 1.0f}) {
                std::array<float, 3> at{};
                at[(axis + 1) % 3] = s1 * half[(axis + 1) % 3];
                at[(axis + 2) % 3] = s2 * half[(axis + 2) % 3];
                std::array<float, 3> extent{t, t, t};
                extent[axis] = 2.0f * half[axis] + t;
                CFrame bar = placed.box;
                bar.position = placed.box.pointToWorldSpace({at[0], at[1], at[2]});
                b.box(bar, {extent[0], extent[1], extent[2]});
            }
}

// Where a HandleAdornment is: its CFrame from the Adornee's pivot, moved first
// by SizeRelativeOffset halves of the Adornee's size (GUESS: in the pivot's
// space, before CFrame).
CFrame handleFrame(const HandleAdornment& handle, const Placed& placed)
{
    const Vector3 relative = handle.sizeRelativeOffset();
    const Vector3 offset{relative.x * placed.size.x * 0.5f, relative.y * placed.size.y * 0.5f,
                         relative.z * placed.size.z * 0.5f};
    return placed.pivot * CFrame(offset) * handle.cframe();
}

} // namespace

bool appendAdornment(const GuiBase3d& adornment, const AdornView& view, Vector3 offset, const ImagePtr& image,
                     EffectGeometry& out)
{
    if (!adornment.visible())
        return false;
    const std::optional<Placed> placed = placeOf(adorneeOf(adornment), offset);
    if (!placed)
        return false;
    Builder b(out, view);
    if (const auto* box = instanceCast<const SelectionBox>(&adornment)) {
        selectionBox(b, *box, *placed);
        return b.finish(false);
    }
    if (const auto* sphere = instanceCast<const SelectionSphere>(&adornment)) {
        // GUESS: round the box's centre, through its corners, in SurfaceColor3.
        if (b.color(sphere->surfaceColor3(), sphere->surfaceTransparency()))
            b.sphere(placed->box.position, placed->size.magnitude() * 0.5f);
        return b.finish(false);
    }
    if (const auto* surface = instanceCast<const SurfaceSelection>(&adornment)) {
        if (!b.color(surface->color3(), surface->transparency()))
            return false;
        // The face TargetSurface names, a hair out from it.
        const auto face = static_cast<std::size_t>(surface->targetSurface());
        const std::array axes{placed->box.rightVector(), placed->box.upVector(), -placed->box.lookVector()};
        const std::array half{placed->size.x * 0.5f, placed->size.y * 0.5f, placed->size.z * 0.5f};
        const std::size_t axis = face % 3; // Right, Top, Back, then Left, Bottom, Front
        const Vector3 normal = axes[axis] * (face < 3 ? 1.0f : -1.0f);
        const Vector3 center = placed->box.position + normal * (half[axis] + 0.01f);
        const Vector3 u = axes[(axis + 1) % 3] * half[(axis + 1) % 3];
        const Vector3 v = axes[(axis + 2) % 3] * half[(axis + 2) % 3];
        b.face({center - u - v, center + u - v, center + u + v, center - u + v}, normal);
        return b.finish(false);
    }
    const auto* handle = instanceCast<const HandleAdornment>(&adornment);
    if (!handle || !b.color(handle->color3(), handle->transparency()))
        return false;
    const CFrame frame = handleFrame(*handle, *placed);
    if (const auto* box = instanceCast<const BoxHandleAdornment>(handle)) {
        b.box(frame, box->size());
    } else if (const auto* sphere = instanceCast<const SphereHandleAdornment>(handle)) {
        b.sphere(frame.position, sphere->radius());
    } else if (const auto* cylinder = instanceCast<const CylinderHandleAdornment>(handle)) {
        b.cylinder(frame, cylinder->height(), cylinder->radius(), cylinder->innerRadius(), cylinder->angle());
    } else if (const auto* cone = instanceCast<const ConeHandleAdornment>(handle)) {
        b.cone(frame, cone->height(), cone->radius(), cone->hollow());
    } else if (const auto* line = instanceCast<const LineHandleAdornment>(handle)) {
        b.line(frame, line->length(), line->thickness());
    } else if (const auto* picture = instanceCast<const ImageHandleAdornment>(handle)) {
        if (!image)
            return false; // not loaded yet: nothing rather than a white square
        b.image(frame.position, picture->size());
        return b.finish(handle->alwaysOnTop(), image);
    }
    return b.finish(handle->alwaysOnTop());
}

Adornments::Adornments(DataModel& game, TextureProvider textures) : game_(game), textures_(std::move(textures))
{
    for (const InstancePtr& d : game.descendants())
        add(d.get());
    added_ = game.descendantAdded.connect([this](Instance* instance) { add(instance); });
    removing_ = game.descendantRemoving.connect([this](Instance* instance) { remove(instance); });
}

Adornments::~Adornments()
{
    added_.disconnect();
    removing_.disconnect();
}

void Adornments::add(Instance* instance)
{
    if (!instanceCast<GuiBase3d>(instance))
        return;
    if (!index_.try_emplace(instance, entries_.size()).second)
        return;
    Entry& entry = entries_.emplace_back();
    entry.key = instance;
    entry.instance = instance->weak_from_this();
}

void Adornments::remove(Instance* instance)
{
    if (auto it = index_.find(instance); it != index_.end())
        drop(it->second);
}

void Adornments::drop(std::size_t i)
{
    index_.erase(entries_[i].key);
    if (i + 1 != entries_.size()) {
        entries_[i] = std::move(entries_.back());
        index_[entries_[i].key] = i;
    }
    entries_.pop_back();
}

void Adornments::collect(const EffectView& view, EffectGeometry& out)
{
    const Frustum frustum(view.viewProjection);
    const AdornView adornView{view.camera.cframe, view.pixelsPerStud};
    const Vector3 eye = view.camera.cframe.position;
    const Instance* workspace = game_.workspace();
    // Kept where the client draws adornments from: the Workspace, a PlayerGui
    // or CoreGui (not StarterGui, which only holds what's copied out).
    auto kept = [&](const Instance& adornment) {
        for (const Instance* p = adornment.parent(); p; p = p->parent())
            if (p == workspace || instanceCast<const PlayerGui>(p) || instanceCast<const CoreGui>(p))
                return true;
        return false;
    };
    drawn_ = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const std::shared_ptr<const Instance> instance = entries_[i].instance.lock();
        if (!instance) {
            drop(i--);
            continue;
        }
        const auto& adornment = static_cast<const GuiBase3d&>(*instance);
        if (!adornment.visible() || !kept(adornment))
            continue;
        const Instance* adornee = adorneeOf(adornment);
        if (!adornee || !adornee->isDescendantOf(workspace))
            continue;
        // The moving instance itself too: a whole character as the Adornee.
        const bool moves = moving_ && (adornee == moving_ || adornee->isDescendantOf(moving_));
        const Vector3 offset = moves ? offset_ : Vector3{};
        Entry& entry = entries_[i];
        if (const auto* picture = instanceCast<const ImageHandleAdornment>(&adornment)) {
            if (picture->image() != entry.imageId) {
                entry.imageId = picture->image();
                entry.image = nullptr;
            }
            if (!entry.image && !entry.imageId.empty() && textures_)
                entry.image = textures_(entry.imageId);
        }
        const std::size_t vertices = out.vertices.size();
        const std::size_t indices = out.indices.size();
        if (!appendAdornment(adornment, adornView, offset, entry.image, out))
            continue;
        const EffectBatch& batch = out.batches.back();
        const Vector3 away = batch.center - eye;
        const float reach = view.drawDistance + batch.radius;
        if (away.dot(away) > reach * reach || !frustum.intersects(batch.center, batch.radius)) {
            out.batches.pop_back();
            out.vertices.resize(vertices);
            out.indices.resize(indices);
            continue;
        }
        ++drawn_;
    }
}

} // namespace lunar::render
