#include <lunar/Classes.hpp>

#include <lunar/ClassRegistry.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <ranges>
#include <utility>
#include <vector>

namespace lunar {

// --- ServiceProvider, DataModel ---------------------------------------------------------

Instance* ServiceProvider::findService(std::string_view className) const
{
    for (const InstancePtr& child : children())
        if (child->className() == className && child->classInfo().kind == ClassKind::Service)
            return child.get();
    return nullptr;
}

Instance* ServiceProvider::getService(std::string_view className)
{
    if (Instance* service = findService(className))
        return service;

    const ClassInfo* info = ClassRegistry::find(className);
    if (!info || info->kind != ClassKind::Service)
        throw Error(std::format("'{}' is not a valid Service name", className));

    InstancePtr service = info->factory();
    // The client names a few services apart from their class (strings in
    // the 0.742 binary): game["Run Service"] works, game.RunService doesn't.
    if (className == "RunService")
        service->setName("Run Service");
    else if (className == "TeleportService")
        service->setName("Teleport Service");
    else if (className == "RolloutValidationService")
        service->setName("Rollout Validation Service");
    service->setParent(this);
    service->parentLocked_ = true;
    return service.get(); // we hold it as a child from here on
}

std::shared_ptr<DataModel> DataModel::create()
{
    auto game = std::make_shared<DataModel>();
    game->getService<Workspace>();
    return game;
}

// --- Model ------------------------------------------------------------------------------

namespace {

// Bounds of every descendant part, measured in `frame`'s space.
std::pair<CFrame, Vector3> partBounds(const Instance& root, const CFrame& frame)
{
    constexpr float inf = std::numeric_limits<float>::infinity();
    Vector3 lo{inf, inf, inf};
    Vector3 hi{-inf, -inf, -inf};
    bool any = false;
    for (const InstancePtr& descendant : root.descendants()) {
        auto* part = instanceCast<const BasePart>(descendant.get());
        if (!part)
            continue;
        any = true;
        Vector3 half = part->size() * 0.5f;
        for (int corner = 0; corner < 8; ++corner) {
            Vector3 local{corner & 1 ? half.x : -half.x, corner & 2 ? half.y : -half.y, corner & 4 ? half.z : -half.z};
            Vector3 p = frame.pointToObjectSpace(part->cframe() * local);
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    if (!any)
        return {CFrame(), Vector3{}};
    return {frame * CFrame((lo + hi) * 0.5f), hi - lo};
}

} // namespace

CFrame Model::getPivot() const
{
    if (BasePart* part = primaryPart())
        return part->getPivot();
    if (worldPivotData())
        return *worldPivotData();
    return partBounds(*this, CFrame()).first;
}

void Model::pivotTo(const CFrame& pivot)
{
    // The pivot made a rotation first, as the part it lands on will be: a
    // skewed one would stretch every other part's offset each time it moves
    // (Studio keeps them rigid).
    const CFrame delta = pivot.orthonormalized() * getPivot().inverse();
    // Every part's place first, then the moves: setting one part of a welded
    // assembly moves the rest with it, so reading as we go would move those twice.
    std::vector<std::pair<BasePart*, CFrame>> moves;
    for (const InstancePtr& descendant : descendants())
        if (auto* part = instanceCast<BasePart>(descendant.get()))
            moves.emplace_back(part, delta * part->cframe());
    for (const auto& [part, cframe] : moves)
        part->setCFrame(cframe);
    if (!primaryPart())
        setWorldPivotData(pivot);
}

// Parts' sizes and places, attachments' and joints' offsets, and meshes'
// offsets, with a file mesh's Scale (block and head meshes follow the
// part's Size already). GUESS: what else the client scales (lights'
// ranges, particles, beams...) isn't checked; those stay as they are.
void Model::scaleTo(float scale)
{
    const float k = scale / scaleFactor();
    const CFrame pivot = getPivot();
    const CFrame toPivot = pivot.inverse();
    auto scaled = [k](CFrame cframe) {
        cframe.position = cframe.position * k;
        return cframe;
    };
    std::vector<std::pair<BasePart*, CFrame>> moves;
    for (const InstancePtr& descendant : descendants()) {
        Instance* d = descendant.get();
        if (auto* part = instanceCast<BasePart>(d)) {
            moves.emplace_back(part, pivot * scaled(toPivot * part->cframe()));
            part->setSize(part->size() * k);
        } else if (auto* attachment = instanceCast<Attachment>(d)) {
            attachment->setCFrame(scaled(attachment->cframe()));
        } else if (auto* joint = instanceCast<JointInstance>(d)) {
            joint->setC0(scaled(joint->c0()));
            joint->setC1(scaled(joint->c1()));
        } else if (auto* mesh = instanceCast<DataModelMesh>(d)) {
            mesh->setOffset(mesh->offset() * k);
            const auto* special = instanceCast<SpecialMesh>(d);
            if (instanceCast<FileMesh>(d) && (!special || special->meshType() == Enum::MeshType::FileMesh))
                mesh->setScale(mesh->scale() * k);
        }
    }
    for (const auto& [part, cframe] : moves)
        part->setCFrame(cframe);
    setScaleFactor(scale);
}

std::pair<CFrame, Vector3> Model::getBoundingBox() const
{
    return partBounds(*this, getPivot().rotation());
}

// --- Workspace ----------------------------------------------------------------------------

Camera* Workspace::ensureCurrentCamera()
{
    if (Camera* camera = currentCamera(); camera && !camera->destroyed()) {
        // A live server's CurrentCamera points at its own Camera, which never
        // replicates: the client keeps that one as its own, in the Workspace
        // like every client's, so what games put in it (the game's first-person
        // rig) is in the world.
        if (!camera->parent())
            camera->setParent(this);
        return camera;
    }
    auto camera = std::make_shared<Camera>();
    camera->setParent(this);
    setCurrentCamera(camera.get());
    return camera.get(); // owned by us as a child now
}

bool Workspace::collisionGroupsCollide(std::string_view a, std::string_view b) const
{
    auto find = [&](std::string_view name) {
        auto it = name.size() == 1 && static_cast<unsigned char>(name.front()) < 0x20
                      ? std::ranges::find(collisionGroups_, static_cast<int>(name.front()), &CollisionGroup::id)
                      : std::ranges::find(collisionGroups_, name, &CollisionGroup::name);
        return it == collisionGroups_.end() ? CollisionGroup{} : *it;
    };
    CollisionGroup first = find(a), second = find(b);
    auto bit = [](const CollisionGroup& group, int id) { return id >= 0 && id < 32 && ((group.mask >> id) & 1) != 0; };
    return bit(first, second.id) && bit(second, first.id);
}

// --- Terrain ------------------------------------------------------------------------------

namespace {

std::uint64_t chunkKey(int x, int y, int z)
{
    auto part = [](int v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v) & 0x1FFFFF); };
    return part(x) << 42 | part(y) << 21 | part(z);
}

int floorDiv(int value, int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

} // namespace

TerrainVoxel TerrainChunk::Run::voxel() const
{
    return {Terrain::materials.at(material), occupancy, water};
}

TerrainVoxel TerrainChunk::voxel(int x, int y, int z) const
{
    int index = x + size * (z + size * y);
    for (const Run& run : runs) {
        if (std::cmp_less(index, run.count))
            return run.voxel();
        index -= run.count;
    }
    return {};
}

void Terrain::setChunks(std::vector<TerrainChunk> chunks)
{
    chunks_ = std::move(chunks);
    ++revision_;
    chunkIndex_.clear();
    for (auto [i, chunk] : std::views::enumerate(chunks_))
        chunkIndex_[chunkKey(chunk.position[0], chunk.position[1], chunk.position[2])] = static_cast<std::size_t>(i);
}

bool Terrain::write(const Block& block)
{
    if (block.side <= 0 || block.side > 1024)
        return false;
    const auto cells = static_cast<std::size_t>(block.side) * static_cast<std::size_t>(block.side)
                       * static_cast<std::size_t>(block.side);
    std::size_t covered = 0;
    for (const TerrainChunk::Run& run : block.runs)
        covered += run.count;
    if (!block.runs.empty() && covered != cells)
        return false;
    if (deferWrites_)
        queued_.push_back(block);
    else
        writeNow(std::span(&block, 1));
    return true;
}

void Terrain::applyQueuedWrites()
{
    if (queued_.empty())
        return;
    writeNow(queued_);
    queued_.clear();
}

void Terrain::writeNow(std::span<const Block> blocks)
{
    using Run = TerrainChunk::Run;
    constexpr int size = TerrainChunk::size;
    constexpr std::size_t chunkCells = std::size_t{size} * size * size;
    // A cell as one number, its material, occupancy and water in the low
    // three bytes: cells copy and compare as words. (They were Runs of one,
    // and a server's stream of 16-cell blocks, each rewriting its 32-cell
    // chunk, spent most of a join's main thread here.)
    auto pack = [](const Run& run) {
        return std::uint32_t{run.material} | std::uint32_t{run.occupancy} << 8 | std::uint32_t{run.water} << 16;
    };
    // Each chunk a block touches, a cell a number, in the order blocks came.
    std::unordered_map<std::uint64_t, std::pair<std::array<int, 3>, std::vector<std::uint32_t>>> touched;
    auto cellsOf = [&](int cx, int cy, int cz) -> std::vector<std::uint32_t>& {
        auto [it, fresh] = touched.try_emplace(chunkKey(cx, cy, cz));
        if (fresh) {
            it->second.first = {cx, cy, cz};
            std::vector<std::uint32_t>& out = it->second.second;
            out.reserve(chunkCells);
            if (const TerrainChunk* chunk = findChunk(cx, cy, cz))
                for (const Run& run : chunk->runs)
                    out.insert(out.end(),
                               std::min<std::size_t>(run.count, chunkCells - std::min(out.size(), chunkCells)),
                               pack(run));
            out.resize(chunkCells, 0);
        }
        return it->second.second;
    };
    std::vector<std::uint32_t> cube;
    for (const Block& block : blocks) {
        const int side = block.side;
        const auto cubeCells
            = static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
        cube.clear();
        cube.reserve(cubeCells);
        for (const Run& run : block.runs)
            cube.insert(cube.end(), std::min<std::size_t>(run.count, cubeCells - std::min(cube.size(), cubeCells)),
                        pack(run));
        cube.resize(cubeCells, 0);
        // Row by row (X fastest, then Z, then Y), each row a copy into every
        // chunk it crosses, the chunks first met in the order the cells
        // are, as when it went a cell at a time.
        std::size_t i = 0;
        for (int y = 0; y < side; ++y)
            for (int z = 0; z < side; ++z) {
                const int gy = block.origin[1] + y, gz = block.origin[2] + z;
                const int cy = floorDiv(gy, size), cz = floorDiv(gz, size);
                const auto ly = static_cast<std::size_t>(gy - cy * size);
                const auto lz = static_cast<std::size_t>(gz - cz * size);
                for (int x = 0; x < side;) {
                    const int gx = block.origin[0] + x;
                    const int cx = floorDiv(gx, size);
                    const auto lx = static_cast<std::size_t>(gx - cx * size);
                    const auto count = std::min<std::size_t>(static_cast<std::size_t>(side - x), size - lx);
                    const std::size_t at = lx + std::size_t{size} * (lz + std::size_t{size} * ly);
                    std::copy_n(cube.begin() + static_cast<std::ptrdiff_t>(i), count,
                                cellsOf(cx, cy, cz).begin() + static_cast<std::ptrdiff_t>(at));
                    i += count;
                    x += static_cast<int>(count);
                }
            }
    }
    // Back into runs, at most 0xFFFF cells each; a chunk left all air goes.
    for (auto& [key, entry] : touched) {
        const auto& [position, cells] = entry;
        TerrainChunk chunk;
        chunk.position = position;
        bool occupied = false;
        for (std::size_t at = 0; at < cells.size();) {
            const std::uint32_t cell = cells[at];
            std::size_t end = at + 1;
            const std::size_t limit = std::min(cells.size(), at + 0xFFFF);
            while (end < limit && cells[end] == cell)
                ++end;
            const auto material = static_cast<std::uint8_t>(cell);
            occupied = occupied || material != 0;
            chunk.runs.push_back(Run{static_cast<std::uint16_t>(end - at), material,
                                     static_cast<std::uint8_t>(cell >> 8), static_cast<std::uint8_t>(cell >> 16)});
            at = end;
        }
        const auto it = chunkIndex_.find(key);
        if (occupied && it != chunkIndex_.end()) {
            chunks_[it->second] = std::move(chunk);
        } else if (occupied) {
            chunkIndex_[key] = chunks_.size();
            chunks_.push_back(std::move(chunk));
        } else if (it != chunkIndex_.end()) {
            const std::size_t gone = it->second;
            chunkIndex_.erase(it);
            if (gone + 1 != chunks_.size()) {
                chunks_[gone] = std::move(chunks_.back());
                const auto& moved = chunks_[gone].position;
                chunkIndex_[chunkKey(moved[0], moved[1], moved[2])] = gone;
            }
            chunks_.pop_back();
        }
    }
    ++revision_;
}

TerrainVoxel Terrain::voxelAt(int x, int y, int z) const
{
    constexpr int size = TerrainChunk::size;
    int cx = floorDiv(x, size), cy = floorDiv(y, size), cz = floorDiv(z, size);
    auto it = chunkIndex_.find(chunkKey(cx, cy, cz));
    if (it == chunkIndex_.end())
        return {};
    return chunks_[it->second].voxel(x - cx * size, y - cy * size, z - cz * size);
}

const TerrainChunk* Terrain::findChunk(int x, int y, int z) const
{
    auto it = chunkIndex_.find(chunkKey(x, y, z));
    return it == chunkIndex_.end() ? nullptr : &chunks_[it->second];
}

Color3 Terrain::getMaterialColor(Enum::Material material) const
{
    auto it = std::ranges::find(materials, material);
    if (it == materials.end())
        throw Error(std::format("{} is not a terrain material", enums::item(material).name));
    return materialColors_[static_cast<std::size_t>(it - materials.begin())];
}

void Terrain::setMaterialColor(Enum::Material material, Color3 color)
{
    auto it = std::ranges::find(materials, material);
    if (it == materials.end() || material == Enum::Material::Air || material == Enum::Material::Water)
        throw Error(std::format("{} has no terrain color", enums::item(material).name));
    materialColors_[static_cast<std::size_t>(it - materials.begin())] = color;
}

// --- Attachment ---------------------------------------------------------------------------

CFrame Attachment::parentFrame() const
{
    if (auto* part = instanceCast<const BasePart>(parent()))
        return part->cframe();
    if (auto* attachment = instanceCast<const Attachment>(parent()))
        return attachment->worldCFrame();
    return {};
}

CFrame Attachment::worldCFrame() const
{
    return parentFrame() * cframe();
}

void Attachment::setWorldCFrame(CFrame world)
{
    setCFrame(parentFrame().inverse() * world);
}

void Attachment::setWorldPosition(Vector3 position)
{
    CFrame world = worldCFrame();
    world.position = position;
    setWorldCFrame(world);
}

Vector3 Attachment::orientation() const
{
    return cframe().toOrientation() * (180.0f / std::numbers::pi_v<float>);
}

void Attachment::setOrientation(Vector3 degrees)
{
    const Vector3 r = degrees * (std::numbers::pi_v<float> / 180.0f);
    setCFrame(CFrame(cframe().position) * CFrame::fromOrientation(r.x, r.y, r.z));
}

Vector3 Attachment::worldOrientation() const
{
    return worldCFrame().toOrientation() * (180.0f / std::numbers::pi_v<float>);
}

void Attachment::setWorldOrientation(Vector3 degrees)
{
    const Vector3 r = degrees * (std::numbers::pi_v<float> / 180.0f);
    setWorldCFrame(CFrame(worldCFrame().position) * CFrame::fromOrientation(r.x, r.y, r.z));
}

CFrame Attachment::withAxes(CFrame frame, Vector3 axis, Vector3 secondary)
{
    if (axis.magnitude() < 1e-6f)
        return frame;
    const Vector3 x = axis.unit();
    const Vector3 z = x.cross(secondary);
    if (z.magnitude() < 1e-6f)
        return frame;
    const Vector3 back = z.unit();
    const Vector3 y = back.cross(x);
    const std::array<Vector3, 3> columns{x, y, back};
    for (std::size_t c = 0; c < 3; ++c) {
        frame.r[0][c] = columns[c].x;
        frame.r[1][c] = columns[c].y;
        frame.r[2][c] = columns[c].z;
    }
    return frame;
}

// --- Constraints ---------------------------------------------------------------------------
// The client reads these off the attachments' world frames whenever asked
// (sub_101147e56, sub_101103934): 0 unless the constraint is active.

namespace {

float attachmentDistance(const Constraint& c)
{
    if (!c.attached())
        return 0.0f;
    return (c.attachment1()->worldPosition() - c.attachment0()->worldPosition()).magnitude();
}

// Attachment1's secondary axis from Attachment0's about Attachment0's primary one, in degrees.
float turnAngle(const Constraint& c)
{
    if (!c.attached())
        return 0.0f;
    const CFrame a = c.attachment0()->worldCFrame();
    const Vector3 s0 = a.upVector();
    const Vector3 s1 = c.attachment1()->worldCFrame().upVector();
    return std::atan2(a.rightVector().dot(s0.cross(s1)), s0.dot(s1)) * 180.0f / std::numbers::pi_v<float>;
}

} // namespace

float RopeConstraint::currentDistance() const
{
    return attachmentDistance(*this);
}

float RodConstraint::currentDistance() const
{
    return attachmentDistance(*this);
}

float SpringConstraint::currentLength() const
{
    return attachmentDistance(*this);
}

float HingeConstraint::currentAngle() const
{
    return turnAngle(*this);
}

// About Attachment0's primary axis tilted InclinationAngle toward its
// secondary one, from the secondary axis tilted the same way (sub_1010f9fc6).
float CylindricalConstraint::currentAngle() const
{
    if (!attached())
        return 0.0f;
    const CFrame a = attachment0()->worldCFrame();
    const float tilt = inclinationAngle() * std::numbers::pi_v<float> / 180.0f;
    const Vector3 axis = a.rightVector() * std::cos(tilt) + a.upVector() * std::sin(tilt);
    const Vector3 s0 = a.upVector() * std::cos(tilt) - a.rightVector() * std::sin(tilt);
    const Vector3 s1 = attachment1()->worldCFrame().upVector();
    return std::atan2(axis.dot(s0.cross(s1)), s0.dot(s1)) * 180.0f / std::numbers::pi_v<float>;
}

float SlidingBallConstraint::currentPosition() const
{
    if (!attached())
        return 0.0f;
    const CFrame a = attachment0()->worldCFrame();
    return (attachment1()->worldPosition() - a.position).dot(a.rightVector());
}

// --- Humanoid ------------------------------------------------------------------------------

BasePart* Humanoid::rootPart() const
{
    Instance* character = parent();
    return character ? instanceCast<BasePart>(character->findFirstChild("HumanoidRootPart")) : nullptr;
}

// --- GuiBase2d ----------------------------------------------------------------------------

std::optional<GuiBase2d::LaidOutFrom> GuiBase2d::layoutInputs() const
{
    const auto* object = instanceCast<const GuiObject>(this);
    if (!object)
        return std::nullopt;
    // Outside a GUI (a template kept under a script, or not parented yet) it
    // goes against an empty rectangle at the origin, so its offsets still
    // count [Studio: a 60x60 TextButton under a LocalScript reads 60, 60].
    const Instance* container = parent();
    while (container && container->isA("Folder"))
        container = container->parent();
    const auto* parentGui = instanceCast<const GuiBase2d>(container);
    return LaidOutFrom{object->size(), object->position(), object->anchorPoint(), parent(),
                       parentGui ? parentGui->absoluteRect() : AbsoluteRect{}};
}

namespace {

void fireAt(const Instance& instance, std::string_view event, int x, int y)
{
    const std::array<Value, 2> args{Value(static_cast<double>(x)), Value(static_cast<double>(y))};
    instance.fireEvent(event, args);
}

} // namespace

void GuiObject::fireMouseEnter(int x, int y)
{
    mouseEnter.fire(x, y);
    fireAt(*this, "MouseEnter", x, y);
}

void GuiObject::fireMouseLeave(int x, int y)
{
    mouseLeave.fire(x, y);
    fireAt(*this, "MouseLeave", x, y);
}

void GuiObject::fireMouseMoved(int x, int y)
{
    mouseMoved.fire(x, y);
    fireAt(*this, "MouseMoved", x, y);
}

void GuiButton::firePress(int x, int y)
{
    mouseButton1Down.fire();
    fireAt(*this, "MouseButton1Down", x, y);
}

void GuiButton::fireRelease(int x, int y, bool over)
{
    mouseButton1Up.fire();
    fireAt(*this, "MouseButton1Up", x, y);
    if (!over)
        return;
    mouseButton1Click.fire();
    fireEvent("MouseButton1Click");
    if (!active() || !interactable()) return;
    activated.fire();
    // Activated(inputObject, clickCount): the button's release.
    auto input = std::make_shared<InputObject>();
    input->setUserInputType(Enum::UserInputType::MouseButton1);
    input->setUserInputState(Enum::UserInputState::End);
    input->setPosition(Vector3{static_cast<float>(x), static_cast<float>(y), 0.0f});
    const std::array<Value, 2> args{Value(InstancePtr(input)), Value(1.0)};
    fireEvent("Activated", args);
}

void GuiButton::fireRightClick(int x, int y)
{
    mouseButton2Click.fire();
    fireEvent("MouseButton2Click");
    (void)x;
    (void)y;
}

void TextBox::fireFocused()
{
    if (auto* game = instanceCast<DataModel>(findFirstAncestorWhichIsA("DataModel"))) {
        auto* input = game->getService<UserInputService>();
        input->assignFocusedTextBox(this);
        const std::array<Value, 1> args{shared_from_this()};
        input->fireEvent("TextBoxFocused", args);
    }
    focused.fire();
    fireEvent("Focused");
}

void UserInputService::assignFocusedTextBox(TextBox* box)
{
    focusedTextBox_.reset(box);
}

TextBox* UserInputService::focusedTextBox() const
{
    return focusedTextBox_.get();
}

void TextBox::fireFocusLost(bool enterPressed)
{
    if (auto* game = instanceCast<DataModel>(findFirstAncestorWhichIsA("DataModel"))) {
        auto* input = game->getService<UserInputService>();
        if (input->focusedTextBox() == this) input->assignFocusedTextBox(nullptr);
        const std::array<Value, 1> args{shared_from_this()};
        input->fireEvent("TextBoxFocusReleased", args);
    }
    focusLost.fire(enterPressed);
    const std::array<Value, 2> args{Value(enterPressed), Value()};
    fireEvent("FocusLost", args);
}

namespace {

// FontSize's steps in pixels, by the enum's value (Size28 and up were added
// after Size48, so the numbers don't climb in order).
constexpr std::array<float, 15> fontSizeSteps{8, 9, 10, 11, 12, 14, 18, 24, 36, 48, 28, 32, 42, 60, 96};

} // namespace

float fontSizePixels(Enum::FontSize size)
{
    const auto index = static_cast<std::size_t>(size);
    return index < fontSizeSteps.size() ? fontSizeSteps[index] : 14.0f;
}

Enum::FontSize nearestFontSize(float pixels)
{
    std::size_t best = 0;
    for (std::size_t i = 1; i < fontSizeSteps.size(); ++i)
        if (std::abs(fontSizeSteps[i] - pixels) < std::abs(fontSizeSteps[best] - pixels))
            best = i;
    return static_cast<Enum::FontSize>(best);
}

void GuiBase2d::assignAbsolute(Vector2 position, Vector2 size, float rotation)
{
    laidOutFrom_ = layoutInputs();
    assignReadOnly(absolutePosition_, position, "AbsolutePosition");
    assignReadOnly(absoluteSize_, size, "AbsoluteSize");
    assignReadOnly(absoluteRotation_, rotation, "AbsoluteRotation");
}

GuiBase2d::AbsoluteRect GuiBase2d::absoluteRect() const
{
    const std::optional<LaidOutFrom> inputs = layoutInputs();
    if (!inputs || inputs == laidOutFrom_)
        return {absolutePosition_, absoluteSize_};
    // Changed since the last layout: Size and Position against the parent,
    // without the layout objects (UIListLayout and friends) the next full
    // layout applies.
    const AbsoluteRect& in = inputs->parentRect;
    const Vector2 size{in.size.x * inputs->size.x.scale + static_cast<float>(inputs->size.x.offset),
                       in.size.y * inputs->size.y.scale + static_cast<float>(inputs->size.y.offset)};
    const Vector2 position{in.position.x + in.size.x * inputs->position.x.scale
                               + static_cast<float>(inputs->position.x.offset) - inputs->anchorPoint.x * size.x,
                           in.position.y + in.size.y * inputs->position.y.scale
                               + static_cast<float>(inputs->position.y.offset) - inputs->anchorPoint.y * size.y};
    return {position, size};
}

// --- UIPageLayout -------------------------------------------------------------------------

std::vector<GuiObject*> UIPageLayout::pages() const
{
    std::vector<GuiObject*> out;
    if (const Instance* holder = parent())
        for (const InstancePtr& child : holder->children())
            if (auto* page = instanceCast<GuiObject>(child.get()); page && page->visible())
                out.push_back(page);
    if (sortOrder() == Enum::SortOrder::Name)
        std::ranges::stable_sort(out, {}, [](const GuiObject* g) { return g->name(); });
    else if (sortOrder() == Enum::SortOrder::LayoutOrder)
        std::ranges::stable_sort(out, {}, [](const GuiObject* g) { return g->layoutOrder(); });
    return out;
}

GuiObject* UIPageLayout::currentPage() const
{
    const std::vector<GuiObject*> all = pages();
    if (const InstancePtr page = currentPage_.lock())
        if (const auto it = std::ranges::find(all, page.get()); it != all.end())
            return *it;
    return all.empty() ? nullptr : all.front();
}

void UIPageLayout::assignCurrentPage(GuiObject* page)
{
    if (currentPage_.lock().get() == page)
        return;
    currentPage_ = page ? page->weak_from_this() : std::weak_ptr<Instance>{};
    changed.fire("CurrentPage");
}

void UIPageLayout::assignSlide(Instance* from, float progress)
{
    if (slide_.from.lock().get() == from && slide_.progress == progress)
        return;
    slide_.from = from ? from->weak_from_this() : std::weak_ptr<Instance>{};
    slide_.progress = progress;
    slideSteps_.fetch_add(1, std::memory_order_relaxed);
}

// --- GuiObject ----------------------------------------------------------------------------

void GuiObject::setTransparency(float transparency)
{
    setBackgroundTransparency(transparency);
    if (auto* label = instanceCast<TextLabel>(this))
        label->setTextTransparency(transparency);
    else if (auto* button = instanceCast<TextButton>(this))
        button->setTextTransparency(transparency);
    else if (auto* box = instanceCast<TextBox>(this))
        box->setTextTransparency(transparency);
}

// --- Tools -----------------------------------------------------------------------------

void Tool::setGripPos(Vector3 position)
{
    CFrame cf = grip();
    cf.position = position;
    setGrip(cf);
}

// The pieces replace a column of the rotation as they are, without
// re-orthonormalizing, like the client's setters.
void Tool::setGripForward(Vector3 forward)
{
    CFrame cf = grip();
    cf.r[0][2] = -forward.x;
    cf.r[1][2] = -forward.y;
    cf.r[2][2] = -forward.z;
    setGrip(cf);
}

void Tool::setGripRight(Vector3 right)
{
    CFrame cf = grip();
    cf.r[0][0] = right.x;
    cf.r[1][0] = right.y;
    cf.r[2][0] = right.z;
    setGrip(cf);
}

void Tool::setGripUp(Vector3 up)
{
    CFrame cf = grip();
    cf.r[0][1] = up.x;
    cf.r[1][1] = up.y;
    cf.r[2][1] = up.z;
    setGrip(cf);
}

BasePart* Tool::handle() const
{
    return instanceCast<BasePart>(findFirstChild("Handle"));
}

// --- StarterGui ------------------------------------------------------------------------

namespace {

constexpr int coreGuiTypeCount = 10;

std::uint32_t coreGuiBits(Enum::CoreGuiType type)
{
    if (type == Enum::CoreGuiType::All)
        return (1u << coreGuiTypeCount) - 1u;
    return 1u << static_cast<std::uint32_t>(type);
}

} // namespace

bool StarterGui::coreGuiEnabled(Enum::CoreGuiType type) const
{
    return (coreGuiDisabled_ & coreGuiBits(type)) == 0;
}

void StarterGui::setCoreGuiEnabled(Enum::CoreGuiType type, bool enabled)
{
    const std::uint32_t previous = coreGuiDisabled_;
    if (enabled)
        coreGuiDisabled_ &= ~coreGuiBits(type);
    else
        coreGuiDisabled_ |= coreGuiBits(type);
    if (previous != coreGuiDisabled_) {
        const std::array<Value, 2> arguments{enums::item(type), enabled};
        fireEvent("CoreGuiChangedSignal", arguments);
    }
}

void StarterGui::postSystemChat(SystemChatMessage message)
{
    if (systemChat_.size() >= 100)
        systemChat_.erase(systemChat_.begin());
    systemChat_.push_back(std::move(message));
}

std::vector<StarterGui::SystemChatMessage> StarterGui::takeSystemChat()
{
    std::vector<SystemChatMessage> messages;
    messages.swap(systemChat_);
    return messages;
}

void StarterGui::postNotification(Notification notification)
{
    if (notifications_.size() >= 50) notifications_.erase(notifications_.begin());
    notifications_.push_back(std::move(notification));
}

std::vector<StarterGui::Notification> StarterGui::takeNotifications()
{
    std::vector<Notification> notifications;
    notifications.swap(notifications_);
    return notifications;
}

bool Humanoid::stateEnabled(Enum::HumanoidStateType state) const
{
    const auto bit = static_cast<std::uint32_t>(state);
    return bit >= 32 || (runtime_.disabledStates & (1u << bit)) == 0;
}

void Humanoid::setStateEnabled(Enum::HumanoidStateType state, bool enabled)
{
    const auto bit = static_cast<std::uint32_t>(state);
    if (bit >= 32 || stateEnabled(state) == enabled)
        return;
    runtime_.disabledStates ^= 1u << bit;
    events.stateEnabledChanged.fire(enabled, state);
}

// --- AnimationTrack ------------------------------------------------------------------------

void AnimationTrack::setLooped(bool looped)
{
    if (!playback_)
        return;
    // Stored even when unchanged: an unloaded track reads false, and the
    // clip's own Loop mustn't win once it loads.
    const bool was = playback_->looped();
    playback_->setLooped(looped);
    if (was != looped)
        changed.fire("Looped");
}

void AnimationTrack::setPriority(Enum::AnimationPriority priority)
{
    if (!playback_)
        return;
    const Enum::AnimationPriority was = playback_->priority();
    playback_->setPriority(priority);
    if (was != priority)
        changed.fire("Priority");
}

void AnimationTrack::setTimePosition(float time)
{
    if (playback_)
        playback_->setTimePosition(time);
}

void AnimationTrack::bind(std::shared_ptr<AnimationPlayback> playback, Animation* animation)
{
    playback_ = std::move(playback);
    keptAnimation_ = animation ? std::static_pointer_cast<Animation>(animation->shared_from_this()) : nullptr;
    setRef(animation_, animation, "Animation");
}

// --- Lighting ------------------------------------------------------------------------------

void Lighting::setClockTime(double hours)
{
    hours = std::fmod(hours, 24.0);
    if (hours < 0)
        hours += 24.0;
    if (hours == clockTime_)
        return;
    clockTime_ = hours;
    changed.fire("ClockTime");
    changed.fire("TimeOfDay");
}

std::string Lighting::timeOfDay() const
{
    auto seconds = std::lround(clockTime_ * 3600.0) % 86400;
    return std::format("{:02}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
}

void Lighting::setTimeOfDay(std::string time)
{
    std::array<double, 3> parts{};
    std::size_t count = 0;
    for (auto field : time | std::views::split(':')) {
        std::string_view text(field);
        double value = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (count == parts.size() || error != std::errc{} || end != text.data() + text.size())
            throw Error(std::format("'{}' is not a valid TimeOfDay", time));
        parts[count++] = value;
    }
    setClockTime(parts[0] + parts[1] / 60.0 + parts[2] / 3600.0);
}

void StarterPlayer::applyCharacterSettings(Humanoid& humanoid) const
{
    humanoid.setWalkSpeed(characterWalkSpeed());
    humanoid.setJumpPower(characterJumpPower());
    humanoid.setJumpHeight(characterJumpHeight());
    humanoid.setUseJumpPower(characterUseJumpPower());
    humanoid.setMaxSlopeAngle(characterMaxSlopeAngle());
    humanoid.setAutoJumpEnabled(autoJumpEnabled());
    humanoid.setBreakJointsOnDeath(characterBreakJointsOnDeath());
    humanoid.setHealthDisplayDistance(healthDisplayDistance());
    humanoid.setNameDisplayDistance(nameDisplayDistance());
}

const MaterialVariant* findMaterialVariant(const MaterialService& service, Enum::Material material,
                                           std::string_view partVariant)
{
    auto named = [&](std::string_view name) -> const MaterialVariant* {
        for (const InstancePtr& d : service.descendants())
            if (const auto* variant = instanceCast<const MaterialVariant>(d.get());
                variant && variant->name() == name && variant->baseMaterial() == material)
                return variant;
        return nullptr;
    };
    if (!partVariant.empty())
        if (const MaterialVariant* variant = named(partVariant))
            return variant;
    // <Material>Name, which names the material itself unless a place changed it.
    const std::string_view materialName = enums::item(material).name;
    // Neon, Glass and the like have none.
    const Property* property = service.classInfo().findProperty(std::format("{}Name", materialName));
    if (!property)
        return nullptr;
    const Value value = property->get(service);
    const auto* name = std::get_if<std::string>(&value);
    if (!name || name->empty() || *name == materialName)
        return nullptr;
    return named(*name);
}

// --- LocalizationTable ---------------------------------------------------------------

namespace {

bool sameLocale(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) {
        return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
    });
}

} // namespace

std::optional<std::string> LocalizationTable::textIn(const Entry& entry, std::string_view locale) const
{
    for (const auto& [id, text] : entry.values)
        if (sameLocale(id, locale) && !text.empty())
            return text;
    if (sameLocale(locale, sourceLocaleId()) && !entry.source.empty())
        return entry.source;
    return std::nullopt;
}

std::optional<std::string> LocalizationTable::textOf(std::string_view key, std::string_view locale) const
{
    for (const Entry& entry : entries_)
        if (entry.key == key)
            return textIn(entry, locale);
    return std::nullopt;
}

std::optional<std::string> LocalizationTable::translationOf(std::string_view source, std::string_view locale) const
{
    for (const Entry& entry : entries_)
        if (entry.source == source)
            return textIn(entry, locale);
    return std::nullopt;
}

// --- Styles --------------------------------------------------------------------------

std::vector<StyleRule*> StyleBase::styleRules() const
{
    std::vector<StyleRule*> rules;
    if (hasRuleList_) {
        for (const Ref<StyleRule>& rule : ruleList_)
            if (StyleRule* r = rule.get())
                rules.push_back(r);
        return rules;
    }
    for (const InstancePtr& child : children())
        if (auto* rule = dynamic_cast<StyleRule*>(child.get()))
            rules.push_back(rule);
    return rules;
}

void StyleBase::assignStyleRules(std::vector<StyleRule*> rules)
{
    ruleList_.clear();
    for (StyleRule* rule : rules)
        ruleList_.emplace_back().reset(rule);
    hasRuleList_ = true;
    fireEvent("StyleRulesChanged");
}

const Value* StyleRule::styleProperty(std::string_view name) const
{
    for (const auto& [key, value] : properties_)
        if (key == name)
            return &value;
    return nullptr;
}

void StyleRule::setStyleProperty(std::string name, Value value)
{
    const Value nameValue(name);
    if (auto it = std::ranges::find(properties_, name, &std::pair<std::string, Value>::first); it != properties_.end())
        it->second = std::move(value);
    else
        properties_.emplace_back(std::move(name), std::move(value));
    fireEvent("StyleRulePropertyChanged", std::span(&nameValue, 1));
}

void StyleRule::setStyleProperties(std::vector<std::pair<std::string, Value>> properties)
{
    properties_ = std::move(properties);
    fireEvent("StyleRulePropertyChanged");
}

std::vector<StyleSheet*> StyleSheet::derives() const
{
    std::vector<StyleSheet*> sheets;
    for (const Ref<StyleSheet>& sheet : derives_)
        if (StyleSheet* s = sheet.get())
            sheets.push_back(s);
    return sheets;
}

void StyleSheet::assignDerives(std::vector<StyleSheet*> sheets)
{
    derives_.clear();
    for (StyleSheet* sheet : sheets)
        derives_.emplace_back().reset(sheet);
    fireEvent("StyleRulesChanged");
}

} // namespace lunar
