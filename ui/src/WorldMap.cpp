#include <lunar/ui/WorldMap.hpp>

#include <lunar/render/Camera.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/ui/TextLayout.hpp>
#include <lunar/ui/UiRenderer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

namespace lunar::ui {

namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr float fullFieldOfView = 50.0f; // degrees
constexpr float fullMinPitch = 0.03f;    // down to street level
constexpr float fullMaxPitch = 1.5f;
constexpr double fullRedrawInterval = 0.1; // seconds, for moving parts while the view holds still

// The dark "tactical" look (GUESS: picked by eye): near black around the
// place, small monospaced text, green for what's active.
constexpr std::array<float, 4> minimapBackground{0.075f, 0.082f, 0.09f, 1.0f};
constexpr std::array<float, 4> fullBackground{0.063f, 0.071f, 0.078f, 1.0f};
constexpr Color4 selfColor{0.55f, 0.97f, 0.62f, 1.0f};
constexpr Color4 outline{0.02f, 0.025f, 0.03f, 0.9f};
constexpr Color4 frameColor{0.42f, 0.47f, 0.44f, 0.95f};
constexpr Color4 panelColor{0.063f, 0.071f, 0.078f, 0.88f};
constexpr Color4 panelEdge{0.3f, 0.34f, 0.32f, 0.7f};
constexpr Color4 activeColor{0.3f, 0.78f, 0.42f, 1.0f};
constexpr Color4 textColor{0.82f, 0.86f, 0.84f, 1.0f};
constexpr Color4 dimText{0.55f, 0.6f, 0.58f, 1.0f};

// An empty image standing for a GPU picture; only its address matters.
std::shared_ptr<const lunar::Image> placeholder()
{
    auto image = std::make_shared<lunar::Image>();
    image->width = 1;
    image->height = 1;
    return image;
}

// A white navigation arrow pointing up, antialiased: tip, two wings and a
// notch between them.
std::shared_ptr<const lunar::Image> arrowSprite()
{
    constexpr std::uint32_t size = 64;
    const std::array<std::array<float, 2>, 4> points{{{32, 4}, {57, 58}, {32, 45}, {7, 58}}};
    auto inside = [&](float x, float y) {
        // Concave quad: the union of triangles (tip, right, notch) and (tip, notch, left).
        auto edge = [](std::array<float, 2> a, std::array<float, 2> b, float px, float py) {
            return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
        };
        auto triangle = [&](std::array<float, 2> a, std::array<float, 2> b, std::array<float, 2> c) {
            const float e0 = edge(a, b, x, y);
            const float e1 = edge(b, c, x, y);
            const float e2 = edge(c, a, x, y);
            return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
        };
        return triangle(points[0], points[1], points[2]) || triangle(points[0], points[2], points[3]);
    };
    auto image = std::make_shared<lunar::Image>();
    image->width = size;
    image->height = size;
    image->rgba.assign(std::size_t{size} * size * 4, 255);
    constexpr int samples = 4;
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            int covered = 0;
            for (int sy = 0; sy < samples; ++sy)
                for (int sx = 0; sx < samples; ++sx)
                    covered += inside(static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / samples,
                                      static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / samples)
                                   ? 1
                                   : 0;
            image->rgba[((std::size_t{y} * size + x) * 4) + 3]
                = static_cast<std::uint8_t>(covered * 255 / (samples * samples));
        }
    return image;
}

// Where a character is: its HumanoidRootPart, else its primary part, else
// any part of it.
const BasePart* characterRoot(const Model& character)
{
    if (const auto* root = dynamic_cast<const BasePart*>(character.findFirstChild("HumanoidRootPart")))
        return root;
    if (const BasePart* primary = character.primaryPart())
        return primary;
    for (const InstancePtr& child : character.children())
        if (const auto* part = dynamic_cast<const BasePart*>(child.get()))
            return part;
    return nullptr;
}

// Radians clockwise from north (-Z) of a direction, seen from above.
float headingOf(Vector3 direction)
{
    return std::atan2(direction.x, -direction.z);
}

} // namespace

std::expected<std::unique_ptr<WorldMap>, std::string> WorldMap::create(render::Context& context, std::uint32_t slots,
                                                                       FontLibrary& fonts, GlyphAtlas& atlas)
{
    std::unique_ptr<WorldMap> map(new WorldMap(fonts, atlas));
    auto renderer = render::MapRenderer::create(context, slots);
    if (!renderer)
        return std::unexpected(renderer.error());
    map->renderer_ = std::move(*renderer);
    map->minimapImage_ = placeholder();
    map->fullImage_ = placeholder();
    map->arrowImage_ = arrowSprite();
    return map;
}

WorldMap::~WorldMap() = default;

void WorldMap::setGame(DataModel* game)
{
    game_ = game ? game->shared_from_this() : nullptr;
}

void WorldMap::setMesh(std::shared_ptr<const render::MapMesh> mesh)
{
    const bool first = !mesh_ && mesh;
    mesh_ = mesh;
    renderer_->setMesh(std::move(mesh));
    minimapDrawnAt_ = -1.0; // redraw both now
    fullDrawnAt_ = -1.0;
    if (first && !framed_)
        frameAll();
}

void WorldMap::setMoving(std::shared_ptr<const render::MapMesh> mesh)
{
    moving_ = mesh;
    renderer_->setMoving(std::move(mesh));
}

void WorldMap::attach(UiRenderer& renderer)
{
    renderer.setPrepass(this, [this](VkCommandBuffer cmd, std::uint32_t slot) { renderer_->record(cmd, slot); });
    renderer.setExternalImage(minimapImage_.get(), [this] { return renderer_->imageView(0); });
    renderer.setExternalImage(fullImage_.get(), [this] { return renderer_->imageView(1); });
}

void WorldMap::detach(UiRenderer& renderer)
{
    renderer.setPrepass(this, {});
    renderer.setExternalImage(minimapImage_.get(), {});
    renderer.setExternalImage(fullImage_.get(), {});
}

float WorldMap::cameraHeading() const
{
    auto game = std::static_pointer_cast<DataModel>(game_.lock());
    if (!game)
        return 0.0f;
    const lunar::Camera* camera = game->workspace()->currentCamera();
    if (!camera)
        return 0.0f;
    const Vector3 look = camera->cframe().lookVector();
    if (std::abs(look.x) + std::abs(look.z) < 1e-4f) // straight down: go by its up vector
        return headingOf(camera->cframe().upVector());
    return headingOf(look);
}

std::optional<Vector3> WorldMap::selfPosition() const
{
    for (const Marker& marker : markers_)
        if (marker.self)
            return marker.position;
    // No character: wherever the camera is.
    auto game = std::static_pointer_cast<DataModel>(game_.lock());
    if (game)
        if (const lunar::Camera* camera = game->workspace()->currentCamera())
            return camera->focus().position;
    return std::nullopt;
}

void WorldMap::gatherMarkers()
{
    markers_.clear();
    auto game = std::static_pointer_cast<DataModel>(game_.lock());
    if (!game)
        return;
    const auto* players = static_cast<const Players*>(game->findService("Players"));
    if (!players)
        return;
    const Player* local = players->localPlayer();
    for (const InstancePtr& child : players->children()) {
        const auto* player = dynamic_cast<const Player*>(child.get());
        if (!player || !player->character())
            continue;
        // See MapSettings::showOtherPlayers: not on real servers.
        if (player != local && !settings_.showOtherPlayers)
            continue;
        const BasePart* root = characterRoot(*player->character());
        if (!root)
            continue;
        Marker marker;
        marker.position = root->position();
        marker.look = root->cframe().lookVector();
        marker.name = player->displayName().empty() ? player->name() : player->displayName();
        marker.self = player == local;
        // Teammates in their team's colour; everyone else the same red. GUESS: the look is ours.
        marker.color = !player->neutral() ? player->teamColor().color() : Color3::fromRGB(235, 95, 80);
        markers_.push_back(std::move(marker));
    }
}

void WorldMap::update(double time, Vector2 screen, float pixelScale)
{
    time_ = time;
    screen_ = screen;
    scale_ = pixelScale;
    heading_ = cameraHeading();
    gatherMarkers();

    // The minimap's picture: straight down around the character, now and
    // then, a little bigger than shown.
    const RectF rect = minimapRect();
    if (minimapShown() && rect.w >= 8.0f) {
        const Vector3 center = selfPosition().value_or(Vector3{});
        const float span = settings_.minimapSpan;
        const float pictureSpan = span * minimapMargin;
        const float pictureSize = std::ceil(rect.w * minimapMargin);
        const float slack = (pictureSpan - span) * 0.5f;
        const float dx = center.x - minimapCenter_.x;
        const float dz = center.z - minimapCenter_.z;
        const bool due = minimapDrawnAt_ < 0.0 || time - minimapDrawnAt_ >= 1.0 / static_cast<double>(minimapRate);
        const bool stale = pictureSpan != minimapPictureSpan_ || pictureSize != minimapPictureSize_
                           || std::max(std::abs(dx), std::abs(dz)) > slack * 0.8f;
        if (due || stale) {
            // Whole texels, so the picture doesn't shimmer as it moves.
            const float texel = pictureSpan / pictureSize;
            minimapCenter_ = {std::round(center.x / texel) * texel, center.y, std::round(center.z / texel) * texel};
            const float top = mesh_ && !mesh_->empty() ? mesh_->bounds.max.y : center.y + 200.0f;
            const float bottom = mesh_ && !mesh_->empty() ? mesh_->bounds.min.y : center.y - 200.0f;
            const Vector3 eye{minimapCenter_.x, top + 50.0f, minimapCenter_.z};
            const render::Mat4 view = render::viewMatrix(render::Camera::lookAt(eye, eye - Vector3{0, 1, 0}));
            render::MapRenderer::View request;
            request.viewProjection
                = render::orthographic(pictureSpan, pictureSpan, 1.0f, eye.y - bottom + 50.0f) * view;
            request.eye = {0.0f, 1.0f, 0.0f};
            request.orthographic = true;
            const auto pixels = static_cast<std::uint32_t>(pictureSize);
            request.extent = {pixels, pixels};
            request.background = minimapBackground;
            renderer_->request(0, request);
            minimapPictureSpan_ = pictureSpan;
            minimapPictureSize_ = pictureSize;
            minimapDrawnAt_ = time;
        }
    }

    if (fullOpen_ && !framed_)
        frameAll();
}

RectF WorldMap::minimapRect() const
{
    const float size = std::round(settings_.minimapSize * scale_);
    const float margin = std::round(16.0f * scale_);
    // Clear of the top bar (and the player list's first rows) at the top.
    const float top = std::round(44.0f * scale_);
    const float bottom = std::round(24.0f * scale_);
    const bool left
        = settings_.corner == MapSettings::Corner::TopLeft || settings_.corner == MapSettings::Corner::BottomLeft;
    const bool upper
        = settings_.corner == MapSettings::Corner::TopLeft || settings_.corner == MapSettings::Corner::TopRight;
    const float x = left ? margin : screen_.x - margin - size;
    const float y = upper ? top : screen_.y - bottom - size;
    return {x, y, size, size};
}

bool WorldMap::pointerOverMinimap() const
{
    if (!minimapShown())
        return false;
    const RectF rect = minimapRect();
    const float rx = mouseX_ - (rect.x + rect.w * 0.5f);
    const float ry = mouseY_ - (rect.y + rect.h * 0.5f);
    return rx * rx + ry * ry <= rect.w * rect.w * 0.25f;
}

bool WorldMap::handleMinimap(const platform::Event& event)
{
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        mouseX_ = static_cast<float>(move->x) * scale_;
        mouseY_ = static_cast<float>(move->y) * scale_;
        return false;
    }
    const auto* scroll = std::get_if<platform::ScrollEvent>(&event);
    if (!scroll || !pointerOverMinimap())
        return false;
    // Out to the whole place at most.
    float most = 4000.0f;
    if (mesh_ && !mesh_->empty())
        most = std::max(mesh_->bounds.extent().x, mesh_->bounds.extent().z) * 1.1f;
    most = std::max(most, minMinimapSpan);
    settings_.minimapSpan
        = std::clamp(settings_.minimapSpan * std::pow(0.8f, static_cast<float>(scroll->dy)), minMinimapSpan, most);
    return true;
}

void WorldMap::text(DrawList& list, std::string_view text, RectF box, float size, Color4 color, Enum::TextXAlignment x,
                    Enum::TextYAlignment y, const RectF& clip, Enum::Font font)
{
    TextParams params;
    params.text = text;
    params.style.font = fontFromEnum(font);
    params.style.size = size * scale_;
    params.style.color = {color.r, color.g, color.b};
    params.style.transparency = 1.0f - color.a;
    params.xAlignment = x;
    params.yAlignment = y;
    params.box = {box.w, box.h};
    params.truncate = Enum::TextTruncate::AtEnd;
    list.text(*atlas_, layoutText(*fonts_, params), box.x, box.y, clip, {0.0f, 0.0f, 0.0f, 0.6f * color.a});
}

void WorldMap::arrow(DrawList& list, float x, float y, float size, float angle, Color4 color, const RectF& clip)
{
    // A dark copy a little bigger behind it, for an outline.
    for (int pass = 0; pass < 2; ++pass) {
        const float s = pass == 0 ? size + 4.0f * scale_ : size;
        Quad q;
        q.rect = {x - s * 0.5f, y - s * 0.5f, s, s};
        q.kind = QuadKind::Image;
        q.texture = list.texture(arrowImage_);
        q.color = pass == 0 ? outline : color;
        q.rotation = angle;
        q.pivotX = x;
        q.pivotY = y;
        q.clip = clip;
        list.push(q);
    }
}

void WorldMap::dot(DrawList& list, float x, float y, float radius, Color4 color, const RectF& clip) const
{
    Quad q;
    const float r = radius + 1.5f * scale_;
    q.rect = {x - r, y - r, 2 * r, 2 * r};
    q.radii = {r, r, r, r};
    q.color = outline;
    q.clip = clip;
    list.push(q);
    q.rect = {x - radius, y - radius, 2 * radius, 2 * radius};
    q.radii = {radius, radius, radius, radius};
    q.color = color;
    list.push(q);
}

void WorldMap::drawMinimap(DrawList& list)
{
    if (!minimapShown())
        return;
    const RectF rect = minimapRect();
    if (rect.w < 8.0f || minimapPictureSpan_ <= 0.0f)
        return;
    const RectF all{0, 0, screen_.x, screen_.y};
    const float radius = rect.w * 0.5f;
    const float cx = rect.x + radius;
    const float cy = rect.y + radius;
    const Vector3 center = selfPosition().value_or(minimapCenter_);
    const float span = settings_.minimapSpan;
    const float rotation = -heading_;
    const float c = std::cos(rotation);
    const float s = std::sin(rotation);

    // Shadow, then the picture as a circle turned with the camera, sliding
    // from where it was drawn to where the character is now.
    Quad shadow;
    shadow.rect = {rect.x - 3 * scale_, rect.y - 1 * scale_, rect.w + 6 * scale_, rect.h + 6 * scale_};
    const float sr = shadow.rect.w * 0.5f;
    shadow.radii = {sr, sr, sr, sr};
    shadow.color = {0, 0, 0, 0.45f};
    shadow.clip = all;
    list.push(shadow);

    Quad picture;
    picture.rect = rect;
    picture.radii = {radius, radius, radius, radius};
    picture.kind = QuadKind::Image;
    picture.texture = list.texture(minimapImage_);
    picture.color = {1, 1, 1, 1};
    const float u = 0.5f + (center.x - minimapCenter_.x) / minimapPictureSpan_;
    const float v = 0.5f + (center.z - minimapCenter_.z) / minimapPictureSpan_;
    const float half = span * 0.5f / minimapPictureSpan_;
    picture.uv = {u - half, v - half, u + half, v + half};
    picture.rotation = rotation;
    picture.pivotX = cx;
    picture.pivotY = cy;
    picture.clip = all;
    list.push(picture);

    // Other players, pinned to the rim when they're off the map.
    const float perStud = rect.w / span;
    for (const Marker& marker : markers_) {
        if (marker.self)
            continue;
        const float lx = (marker.position.x - center.x) * perStud;
        const float ly = (marker.position.z - center.z) * perStud;
        float x = lx * c - ly * s;
        float y = lx * s + ly * c;
        const float length = std::sqrt(x * x + y * y);
        const float edge = radius - 6.0f * scale_;
        float alpha = 1.0f;
        if (length > edge) {
            x *= edge / length;
            y *= edge / length;
            alpha = 0.6f;
        }
        dot(list, cx + x, cy + y, 4.0f * scale_, Color4::from(marker.color, 1.0f - alpha), all);
    }

    // The rim, north on it, and the character in the middle.
    Quad rim;
    rim.rect = rect;
    rim.radii = {radius, radius, radius, radius};
    rim.stroke = 1.5f * scale_;
    rim.color = frameColor;
    rim.clip = all;
    list.push(rim);
    const float nx = cx + s * radius;
    const float ny = cy - c * radius;
    const float badge = 8.0f * scale_;
    dot(list, nx, ny, badge, panelColor, all);
    text(list, "N", {nx - badge, ny - badge, 2 * badge, 2 * badge}, 12, activeColor, Enum::TextXAlignment::Center,
         Enum::TextYAlignment::Center, all);
    for (const Marker& marker : markers_)
        if (marker.self)
            arrow(list, cx, cy, 18.0f * scale_, headingOf(marker.look) + rotation, selfColor, all);
    if (markers_.empty() || std::ranges::none_of(markers_, &Marker::self))
        dot(list, cx, cy, 3.0f * scale_, selfColor, all);
}

namespace {

// The inverse of a 4x4 matrix, or the identity when it has none.
render::Mat4 inverse(const render::Mat4& m)
{
    // Cofactors, with a[i] = m(row i / 4, column i % 4).
    std::array<float, 16> a{};
    for (std::size_t i = 0; i < 16; ++i)
        a[i] = m(i / 4, i % 4);
    std::array<float, 16> inv{};
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14]
             + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14]
             - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13]
             + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13]
              - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14]
             - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14]
             + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13]
             - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13]
              + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7]
             - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14]
             - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13]
              + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13]
              - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7]
             + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7]
             - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7]
              + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6]
              - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    render::Mat4 out;
    if (std::abs(det) < 1e-30f)
        return out;
    for (std::size_t i = 0; i < 16; ++i)
        out.m[i % 4][i / 4] = inv[i] / det;
    return out;
}

// Where a ray (origin + t * direction) enters a box, if it does at t >= 0.
std::optional<float> rayBox(Vector3 origin, Vector3 direction, Vector3 low, Vector3 high)
{
    float near = 0.0f;
    float far = std::numeric_limits<float>::max();
    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {direction.x, direction.y, direction.z};
    const float lo[3] = {low.x, low.y, low.z};
    const float hi[3] = {high.x, high.y, high.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-12f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis])
                return std::nullopt;
            continue;
        }
        float t0 = (lo[axis] - o[axis]) / d[axis];
        float t1 = (hi[axis] - o[axis]) / d[axis];
        if (t0 > t1)
            std::swap(t0, t1);
        near = std::max(near, t0);
        far = std::min(far, t1);
        if (near > far)
            return std::nullopt;
    }
    return near;
}

render::Mat4 instanceMatrix(const render::MapInstance& instance)
{
    render::Mat4 m;
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 4; ++c)
            m.m[c][r] = instance.rows[r][c];
    return m;
}

std::string studs(float value)
{
    return value < 10.0f ? std::format("{:.1f}", value) : std::format("{:.0f}", value);
}

} // namespace

void WorldMap::setFullOpen(bool open)
{
    if (open == fullOpen_)
        return;
    fullOpen_ = open;
    drag_ = Drag::None;
    fullDrawnAt_ = -1.0;
    hover_.reset();
    renderer_->setHighlight(nullptr);
    if (open && !framed_)
        frameAll();
}

bool WorldMap::handleHotkey(const platform::Event& event)
{
    const auto* key = std::get_if<platform::KeyEvent>(&event);
    if (!key || !key->pressed || key->repeat || !settings_.fullMapKey || key->key != *settings_.fullMapKey)
        return false;
    setFullOpen(!fullOpen_);
    return true;
}

void WorldMap::setMode(Mode mode)
{
    if (mode == mode_)
        return;
    // Keep looking at about the same ground.
    const float halfFov = fullFieldOfView * pi / 360.0f;
    if (mode == Mode::TwoD)
        orbit_.span = std::max(2.0f * orbit_.distance * std::tan(halfFov), 20.0f);
    else
        orbit_.distance = std::max(orbit_.span * 0.5f / std::tan(halfFov), 10.0f);
    mode_ = mode;
    drag_ = Drag::None;
    hoverDirty_ = true;
}

void WorldMap::frameAll()
{
    framed_ = true;
    if (!mesh_ || mesh_->empty()) {
        orbit_.target = selfPosition().value_or(Vector3{});
        orbit_.distance = 400.0f;
        orbit_.span = 800.0f;
        return;
    }
    const render::Bounds& b = mesh_->bounds;
    orbit_.target = {b.center().x, b.min.y + (b.extent().y * 0.25f), b.center().z};
    const float radius = 0.5f * std::sqrt((b.extent().x * b.extent().x) + (b.extent().z * b.extent().z));
    orbit_.distance = std::max(radius, 50.0f) / std::sin(fullFieldOfView * pi / 360.0f) * 0.85f;
    orbit_.span = std::max({b.extent().x, b.extent().z, 50.0f}) * 1.05f;
    orbit_.yaw = -cameraHeading();
    orbit_.pitch = 0.95f;
}

void WorldMap::findSelf()
{
    if (const auto self = selfPosition())
        orbit_.target = *self;
    orbit_.distance = std::min(orbit_.distance, 300.0f);
    orbit_.span = std::min(orbit_.span, 400.0f);
    hoverDirty_ = true;
}

Vector3 WorldMap::fullEye() const
{
    if (mode_ == Mode::TwoD)
        return {0.0f, 1.0f, 0.0f}; // orthographic: the direction towards the camera
    const Vector3 offset{std::sin(orbit_.yaw) * std::cos(orbit_.pitch), std::sin(orbit_.pitch),
                         std::cos(orbit_.yaw) * std::cos(orbit_.pitch)};
    return orbit_.target + offset * orbit_.distance;
}

render::Mat4 WorldMap::fullViewProjection(RectF area) const
{
    const float aspect = area.w / std::max(area.h, 1.0f);
    float reach = 2000.0f;
    float top = orbit_.target.y + 500.0f;
    float bottom = orbit_.target.y - 500.0f;
    if (mesh_ && !mesh_->empty()) {
        reach = mesh_->bounds.extent().magnitude();
        top = mesh_->bounds.max.y;
        bottom = mesh_->bounds.min.y;
    }
    if (mode_ == Mode::TwoD) {
        const Vector3 eye{orbit_.target.x, top + 50.0f, orbit_.target.z};
        const render::Mat4 view = render::viewMatrix(render::Camera::lookAt(eye, eye - Vector3{0, 1, 0}));
        return render::orthographic(orbit_.span * aspect, orbit_.span, 1.0f, eye.y - bottom + 50.0f) * view;
    }
    render::Camera camera;
    camera.cframe = render::Camera::lookAt(fullEye(), orbit_.target);
    camera.fieldOfView = fullFieldOfView;
    camera.nearPlane = std::clamp(orbit_.distance * 0.01f, 0.2f, 20.0f);
    camera.farPlane = orbit_.distance + reach + 500.0f;
    return camera.viewProjection(aspect);
}

void WorldMap::layoutButtons(RectF area)
{
    const float k = scale_;
    buttons_.clear();
    float x = area.x + 18 * k;
    const float y = area.y + 58 * k;
    auto add = [&](std::string label, float width, bool active, void (WorldMap::*action)()) {
        buttons_.push_back({{x, y, width * k, 20 * k}, std::move(label), active, action});
        x += (width + 4) * k;
    };
    add("2D", 32, mode_ == Mode::TwoD, &WorldMap::twoD);
    add("3D", 32, mode_ == Mode::ThreeD, &WorldMap::threeD);
    add("Center", 60, false, &WorldMap::findSelf);
    add("Close", 52, false, &WorldMap::close);
}

void WorldMap::pointAt(float x, float y)
{
    mouseX_ = x;
    mouseY_ = y;
    hoverDirty_ = true;
}

// The part whose box the ray through the pointer meets first. Terrain
// doesn't stop it: parts behind a hill can be pointed at.
void WorldMap::hoverAt(RectF area)
{
    hoverDirty_ = false;
    std::optional<Hover> found;
    const bool overPanel
        = std::ranges::any_of(buttons_, [&](const Button& b) { return b.rect.contains(mouseX_, mouseY_); })
          || mouseY_ > area.bottom() - 22 * scale_ || !area.contains(mouseX_, mouseY_);
    if (!overPanel) {
        const render::Mat4 toWorld = inverse(fullViewProjection(area));
        const float nx = (mouseX_ - area.x) / area.w * 2.0f - 1.0f;
        const float ny = (mouseY_ - area.y) / area.h * 2.0f - 1.0f;
        auto unproject = [&](float depth) {
            const render::Vec4 p = toWorld * render::Vec4{nx, ny, depth, 1.0f};
            return Vector3{p.x / p.w, p.y / p.w, p.z / p.w};
        };
        const Vector3 origin = unproject(1.0f); // reversed Z: the near plane
        const Vector3 direction = unproject(0.01f) - origin;
        float best = std::numeric_limits<float>::max();
        for (const auto* mesh : {mesh_.get(), moving_.get()}) {
            if (!mesh)
                continue;
            for (const render::MapBatch& batch : mesh->batches) {
                const auto enter = rayBox(origin, direction, batch.bounds.min, batch.bounds.max);
                if (!enter || *enter > best)
                    continue;
                const render::Bounds& local = mesh->shapes[batch.shape].bounds;
                for (std::uint32_t i = batch.firstInstance; i < batch.firstInstance + batch.instanceCount; ++i) {
                    if (i >= mesh->owners.size() || mesh->owners[i].expired())
                        continue;
                    const render::Mat4 model = instanceMatrix(mesh->instances[i]);
                    const render::Mat4 toLocal = inverse(model);
                    const Vector3 o = toLocal.transformPoint(origin);
                    const Vector3 d = toLocal.transformPoint(origin + direction) - o;
                    const auto t = rayBox(o, d, local.min, local.max);
                    if (!t || *t >= best)
                        continue;
                    best = *t;
                    found = Hover{mesh->owners[i], model * render::Mat4::translation(local.center())
                                                       * render::Mat4::scale(local.extent())};
                    // A ground mesh's box would tint the whole view: outline only.
                    const Vector3 diagonal = model.transformPoint(local.max) - model.transformPoint(local.min);
                    found->filled = diagonal.magnitude() < 240.0f;
                }
            }
        }
    }
    const bool changed
        = found.has_value() != hover_.has_value() || (found && hover_ && found->part.lock() != hover_->part.lock());
    hover_ = std::move(found);
    if (changed) {
        if (hover_) {
            render::MapMeshBuilder builder;
            builder.addBox(hover_->box, {1, 1, 1});
            renderer_->setHighlight(std::make_shared<render::MapMesh>(builder.finish()));
        } else {
            renderer_->setHighlight(nullptr);
        }
        fullDrawnAt_ = -1.0;
    }
}

bool WorldMap::handleFull(const platform::Event& event, RectF area)
{
    if (!fullOpen_)
        return false;
    const float k = scale_;
    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        if (!key->pressed || key->repeat)
            return true;
        if (key->key == platform::Key::Escape || (settings_.fullMapKey && key->key == *settings_.fullMapKey))
            setFullOpen(false);
        else if (key->key == platform::Key::C || key->key == platform::Key::F)
            findSelf();
        else if (key->key == platform::Key::Num2)
            setMode(Mode::TwoD);
        else if (key->key == platform::Key::Num3)
            setMode(Mode::ThreeD);
        return true;
    }
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        mouseX_ = static_cast<float>(button->x) * k;
        mouseY_ = static_cast<float>(button->y) * k;
        if (button->pressed) {
            pressX_ = mouseX_;
            pressY_ = mouseY_;
            const bool onButton
                = std::ranges::any_of(buttons_, [&](const Button& b) { return b.rect.contains(mouseX_, mouseY_); });
            if (!onButton && area.contains(mouseX_, mouseY_))
                drag_ = button->button == platform::MouseButton::Left && mode_ == Mode::ThreeD ? Drag::Turn : Drag::Pan;
            return true;
        }
        drag_ = Drag::None;
        hoverDirty_ = true;
        if (button->button == platform::MouseButton::Left)
            for (const Button& b : buttons_)
                if (b.rect.contains(mouseX_, mouseY_) && b.rect.contains(pressX_, pressY_) && b.action) {
                    (this->*b.action)();
                    break;
                }
        return true;
    }
    if (const auto* move = std::get_if<platform::MouseMoveEvent>(&event)) {
        const float x = static_cast<float>(move->x) * k;
        const float y = static_cast<float>(move->y) * k;
        const float dx = x - mouseX_;
        const float dy = y - mouseY_;
        mouseX_ = x;
        mouseY_ = y;
        if (drag_ == Drag::Turn) {
            orbit_.yaw -= dx * 0.006f;
            orbit_.pitch = std::clamp(orbit_.pitch + (dy * 0.005f), fullMinPitch, fullMaxPitch);
        } else if (drag_ == Drag::Pan && mode_ == Mode::TwoD) {
            const float perPixel = orbit_.span / std::max(area.h, 1.0f);
            orbit_.target = orbit_.target - Vector3{dx * perPixel, 0.0f, dy * perPixel};
        } else if (drag_ == Drag::Pan) {
            // The ground follows the pointer, about.
            const float perPixel
                = 2.0f * orbit_.distance * std::tan(fullFieldOfView * pi / 360.0f) / std::max(area.h, 1.0f);
            const Vector3 right{std::cos(orbit_.yaw), 0.0f, -std::sin(orbit_.yaw)};
            const Vector3 forward{-std::sin(orbit_.yaw), 0.0f, -std::cos(orbit_.yaw)};
            orbit_.target = orbit_.target - right * (dx * perPixel)
                            + forward * (dy * perPixel / std::max(std::sin(orbit_.pitch), 0.3f));
        }
        hoverDirty_ = true;
        return false;
    }
    if (const auto* scroll = std::get_if<platform::ScrollEvent>(&event)) {
        float most = 20000.0f;
        if (mesh_ && !mesh_->empty())
            most = mesh_->bounds.extent().magnitude() * 3.0f;
        const float factor = std::pow(0.85f, static_cast<float>(scroll->dy));
        if (mode_ == Mode::TwoD)
            orbit_.span = std::clamp(orbit_.span * factor, 20.0f, std::max(most, 100.0f));
        else
            orbit_.distance = std::clamp(orbit_.distance * factor, 6.0f, std::max(most, 100.0f));
        hoverDirty_ = true;
        return true;
    }
    return true; // modal: nothing reaches the game
}

void WorldMap::drawFull(DrawList& list, RectF area)
{
    if (!fullOpen_ || area.w < 8.0f || area.h < 8.0f)
        return;
    const float k = scale_;
    layoutButtons(area);
    if (hoverDirty_ && drag_ == Drag::None)
        hoverAt(area);
    const render::Mat4 viewProjection = fullViewProjection(area);
    const auto width = static_cast<std::uint32_t>(std::round(area.w));
    const auto height = static_cast<std::uint32_t>(std::round(area.h));
    if (viewProjection != fullDrawn_ || fullDrawnAt_ < 0.0 || time_ - fullDrawnAt_ >= fullRedrawInterval
        || renderer_->extent(1).width != width || renderer_->extent(1).height != height) {
        render::MapRenderer::View request;
        request.viewProjection = viewProjection;
        request.eye = fullEye();
        request.orthographic = mode_ == Mode::TwoD;
        request.extent = {width, height};
        request.background = fullBackground;
        request.highlight = hover_.has_value();
        request.highlightFaces = hover_ && hover_->filled;
        renderer_->request(1, request);
        fullDrawn_ = viewProjection;
        fullDrawnAt_ = time_;
    }

    Quad picture;
    picture.rect = area;
    picture.kind = QuadKind::Image;
    picture.texture = list.texture(fullImage_);
    picture.color = {1, 1, 1, 1};
    picture.clip = area;
    list.push(picture);

    auto project = [&](Vector3 p) -> std::optional<std::array<float, 2>> {
        const render::Vec4 clip = viewProjection * render::Vec4{p.x, p.y, p.z, 1.0f};
        if (clip.w <= 1e-3f)
            return std::nullopt;
        return std::array<float, 2>{area.x + (clip.x / clip.w * 0.5f + 0.5f) * area.w,
                                    area.y + (clip.y / clip.w * 0.5f + 0.5f) * area.h};
    };
    auto screenAngle = [&](Vector3 from, Vector3 direction) -> std::optional<float> {
        const auto a = project(from);
        const auto b = project(from + direction);
        if (!a || !b)
            return std::nullopt;
        return std::atan2((*b)[0] - (*a)[0], -((*b)[1] - (*a)[1]));
    };

    // Players: a marker, the name and how far away (unless it would land
    // on a name already written).
    const std::optional<Vector3> self = selfPosition();
    std::vector<RectF> labels;
    for (const Marker& marker : markers_) {
        const auto at = project(marker.position);
        if (!at || !area.contains((*at)[0], (*at)[1]))
            continue;
        const float x = (*at)[0];
        const float y = (*at)[1];
        std::string label = marker.name;
        if (marker.self) {
            const Vector3 flat{marker.look.x, 0.0f, marker.look.z};
            const float angle
                = screenAngle(marker.position, flat.magnitude() > 1e-4f ? flat.unit() * 8.0f : Vector3{0, 0, -8.0f})
                      .value_or(0.0f);
            arrow(list, x, y, 16.0f * k, angle, selfColor, area);
        } else {
            dot(list, x, y, 4.0f * k, Color4::from(marker.color), area);
            if (self)
                label += std::format("  {} studs", studs((marker.position - *self).magnitude()));
        }
        const RectF box{x + 10 * k, y - 8 * k, (static_cast<float>(label.size()) * 7.0f + 4.0f) * k, 16 * k};
        if (std::ranges::any_of(labels,
                                [&](const RectF& r) { return r.intersect(box).w > 0.0f && r.intersect(box).h > 0.0f; }))
            continue;
        labels.push_back(box);
        text(list, label, {box.x, box.y, 220 * k, box.h}, 12, marker.self ? selfColor : textColor,
             Enum::TextXAlignment::Left, Enum::TextYAlignment::Center, area);
    }

    // The part under the pointer: what the client knows of it.
    if (hover_) {
        const auto part = std::dynamic_pointer_cast<const BasePart>(hover_->part.lock());
        if (part) {
            std::vector<std::string> lines{part->name()};
            if (self)
                lines.push_back(std::format("dist   {} studs", studs((part->position() - *self).magnitude())));
            const Vector3 size = part->size();
            lines.push_back(std::format("size   {} x {} x {}", studs(size.x), studs(size.y), studs(size.z)));
            lines.push_back(std::format("collide {}", part->canCollide() ? "true" : "false"));
            const float lineHeight = 15 * k;
            const RectF box{std::min(mouseX_ + 16 * k, area.right() - 230 * k), mouseY_ + 14 * k, 220 * k,
                            (static_cast<float>(lines.size()) * lineHeight) + 10 * k};
            Quad back;
            back.rect = box;
            back.color = panelColor;
            back.radii = {3 * k, 3 * k, 3 * k, 3 * k};
            back.clip = area;
            list.push(back);
            back.color = activeColor;
            back.stroke = 1.0f * k;
            list.push(back);
            for (std::size_t i = 0; i < lines.size(); ++i)
                text(list, lines[i],
                     {box.x + 8 * k, box.y + 5 * k + static_cast<float>(i) * lineHeight, box.w - 16 * k, lineHeight},
                     12, i == 0 ? activeColor : textColor, Enum::TextXAlignment::Left, Enum::TextYAlignment::Center,
                     area);
        }
    }

    // The panel: the place, what's in it, the modes and the way out.
    const RectF panel{area.x + 10 * k, area.y + 10 * k, 300 * k, 76 * k};
    Quad back;
    back.rect = panel;
    back.color = panelColor;
    back.radii = {4 * k, 4 * k, 4 * k, 4 * k};
    back.clip = area;
    list.push(back);
    back.color = panelEdge;
    back.stroke = 1.0f * k;
    list.push(back);
    text(list, title_.empty() ? std::string("Map") : title_,
         {panel.x + 8 * k, panel.y + 6 * k, panel.w - 16 * k, 16 * k}, 14, textColor, Enum::TextXAlignment::Left,
         Enum::TextYAlignment::Center, area);
    std::size_t players = 0;
    if (auto game = std::static_pointer_cast<DataModel>(game_.lock()))
        if (const Instance* service = game->findService("Players"))
            for (const InstancePtr& child : service->children())
                players += dynamic_cast<const Player*>(child.get()) ? 1u : 0u;
    const std::size_t parts = (mesh_ ? mesh_->stats.parts : 0) + (moving_ ? moving_->stats.parts : 0);
    text(list, std::format("{} parts  {} players", parts, players),
         {panel.x + 8 * k, panel.y + 24 * k, panel.w - 16 * k, 14 * k}, 12, dimText, Enum::TextXAlignment::Left,
         Enum::TextYAlignment::Center, area);
    for (const Button& b : buttons_) {
        const bool hover = b.rect.contains(mouseX_, mouseY_);
        Quad q;
        q.rect = b.rect;
        q.radii = {3 * k, 3 * k, 3 * k, 3 * k};
        q.color = b.active ? activeColor : hover ? Color4{0.2f, 0.23f, 0.22f, 1.0f} : Color4{0.13f, 0.15f, 0.15f, 1.0f};
        q.clip = area;
        list.push(q);
        text(list, b.label, b.rect, 12, b.active ? Color4{0.04f, 0.07f, 0.05f, 1.0f} : textColor,
             Enum::TextXAlignment::Center, Enum::TextYAlignment::Center, area);
    }

    // North, in 3D where the map turns.
    if (mode_ == Mode::ThreeD) {
        const float r = 14.0f * k;
        const float cx = area.right() - 14 * k - r;
        const float cy = area.y + 14 * k + r;
        dot(list, cx, cy, r, panelColor, area);
        const float north = screenAngle(orbit_.target, {0.0f, 0.0f, -10.0f}).value_or(0.0f);
        arrow(list, cx, cy, 14.0f * k, north, activeColor, area);
        text(list, "N", {cx - 30 * k - r, cy - 8 * k, 24 * k, 16 * k}, 12, dimText, Enum::TextXAlignment::Right,
             Enum::TextYAlignment::Center, area);
    }

    // What the mouse and keys do, along the bottom.
    const RectF bar{area.x, area.bottom() - 22 * k, area.w, 22 * k};
    list.rect(bar, panelColor, area);
    const std::string close
        = settings_.fullMapKey ? std::format("{}/Esc close", platform::keyName(*settings_.fullMapKey)) : "Esc close";
    const std::string help
        = mode_ == Mode::ThreeD
              ? std::format("LMB drag orbit   RMB drag pan   Wheel zoom   C center   2/3 mode   {}", close)
              : std::format("Drag pan   Wheel zoom   C center   2/3 mode   {}", close);
    text(list, help, {bar.x + 10 * k, bar.y, bar.w - 20 * k, bar.h}, 12, dimText, Enum::TextXAlignment::Left,
         Enum::TextYAlignment::Center, area);
}

} // namespace lunar::ui
