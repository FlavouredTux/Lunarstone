#include "MapScene.hpp"

#include <lunar/terrain/TerrainMesher.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>

namespace {

namespace render = lunar::render;

// Whether a part belongs to a character: a Model with a Humanoid above it.
bool inCharacter(const lunar::Instance& part)
{
    for (const lunar::Instance* at = part.parent(); at; at = at->parent())
        if (at->isA("Model") && at->findFirstChildOfClass("Humanoid"))
            return true;
    return false;
}

int floorDiv(int value, int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

} // namespace

MapScene::MapScene(PlaceScene& scene, render::MapMeshOptions options)
    : scene_(scene), options_(options), worker_([this](const std::stop_token& stop) { work(stop); })
{
}

MapScene::~MapScene()
{
    worker_.request_stop();
    wake_.notify_all();
}

std::shared_ptr<const render::MapMesh> MapScene::mesh() const
{
    std::scoped_lock lock(mutex_);
    return latest_;
}

void MapScene::classify()
{
    const auto& items = scene_.items();
    const auto& owners = scene_.owners();
    const std::size_t count = std::min(scene_.partItemCount(), owners.size());
    kinds_.assign(count, Kind::Skip);
    placed_.assign(count, render::Mat4{});
    for (std::size_t i = 0; i < count; ++i) {
        if (!owners[i] || items[i].decal)
            continue;
        auto [it, fresh] = known_.try_emplace(owners[i]);
        Known& known = it->second;
        const auto* part = static_cast<const lunar::BasePart*>(owners[i]); // the scene's owners are parts
        if (fresh) {
            // Huge parts nothing can stand on are sky, clouds and zone
            // volumes (Duvall's storm vortex layers), not the place's
            // layout. GUESS: the size.
            const lunar::Vector3 size = part->size();
            known.candidate
                = !inCharacter(*part) && (part->canCollide() || std::max({size.x, size.y, size.z}) <= hugeNonColliding);
            known.placed = items[i].model;
        }
        if (!known.candidate)
            continue;
        kinds_[i] = part->anchored() && !known.moved ? Kind::Static : Kind::Moving;
        placed_[i] = known.placed;
    }
    // Forget parts that are gone now and then.
    if (known_.size() > 2 * count + 1024) {
        std::unordered_map<const lunar::Instance*, Known> kept;
        for (std::size_t i = 0; i < count; ++i)
            if (auto it = known_.find(owners[i]); it != known_.end())
                kept.insert(*it);
        known_ = std::move(kept);
    }
    extractions_ = scene_.extractions();
    movedCursor_ = 0;
}

std::optional<MapScene::Job> MapScene::snapshot()
{
    Job job;
    const auto& items = scene_.items();
    for (std::size_t i = 0; i < kinds_.size(); ++i) {
        if (kinds_[i] != Kind::Static)
            continue;
        const render::DrawItem& from = items[i];
        // Only what the builder reads, so the copy stays light.
        render::DrawItem& to = job.items.emplace_back();
        to.model = from.model;
        to.color = from.color;
        to.transparency = from.transparency;
        to.mesh = from.mesh;
        to.texture = from.texture;
        to.textureColor = from.textureColor;
        to.alphaMode = from.alphaMode;
        job.owners.push_back(scene_.owners()[i]->weak_from_this());
    }
    return job;
}

void MapScene::meshTerrain()
{
    const lunar::Terrain* terrain = scene_.terrain();
    if (!terrain || terrain->chunks().empty())
        return;
    // As coarse a level as keeps the surface around 150k cells across the
    // ground, no finer than level 1. GUESS: tuned by eye and by size.
    std::set<std::pair<int, int>> columns;
    for (const lunar::TerrainChunk& chunk : terrain->chunks())
        columns.emplace(chunk.position[0], chunk.position[2]);
    int level = 1;
    while (level < 4
           && static_cast<double>(columns.size()) * 1024.0 / std::pow(4.0, static_cast<double>(level)) > 150000.0)
        ++level;
    terrainLevel_ = level;
    // The nodes over every block with terrain and the air ones just below
    // it (a block owns the surface into its high neighbours), as TerrainLod does.
    std::set<std::array<int, 3>> nodes;
    for (const lunar::TerrainChunk& chunk : terrain->chunks())
        for (int axis = -1; axis < 3; ++axis) {
            std::array<int, 3> block = chunk.position;
            if (axis >= 0)
                block[static_cast<std::size_t>(axis)] -= 1;
            nodes.insert(
                {floorDiv(block[0], 1 << level), floorDiv(block[1], 1 << level), floorDiv(block[2], 1 << level)});
        }
    lunar::terrain::NodeMesher mesher(*terrain);
    for (const std::array<int, 3>& node : nodes) {
        auto mesh = std::make_shared<lunar::terrain::TerrainMeshChunk>(mesher.mesh(level, node, false));
        if (!mesh->empty())
            terrain_.push_back(std::move(mesh));
    }
}

std::shared_ptr<const render::MapMesh> MapScene::build(Job job)
{
    const auto start = std::chrono::steady_clock::now();
    if (!terrainMeshed_) {
        meshTerrain();
        terrainMeshed_ = true;
    }
    render::MapMeshBuilder builder(options_);
    for (std::size_t i = 0; i < job.items.size(); ++i)
        builder.addItem(job.items[i], job.owners[i]);
    const auto& colors = scene_.terrainColors();
    for (const auto& chunk : terrain_) {
        builder.addTerrain(chunk->solid, std::span(chunk->palette).first(chunk->paletteSize), colors);
        builder.addWater(chunk->water, 4.0f * static_cast<float>(1 << terrainLevel_));
    }
    auto mesh = std::make_shared<render::MapMesh>(builder.finish());
    mesh->stats.milliseconds
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return mesh;
}

std::shared_ptr<const render::MapMesh> MapScene::boxes()
{
    const auto& items = scene_.items();
    render::MapMeshBuilder builder(options_);
    std::size_t count = 0;
    for (std::size_t i = 0; i < kinds_.size() && count < maxMovingBoxes; ++i) {
        if (kinds_[i] != Kind::Moving)
            continue;
        const render::DrawItem& item = items[i];
        if (item.transparency > options_.maxTransparency)
            continue;
        render::Mat4 model = item.model;
        if (item.mesh) {
            auto [it, added] = meshBounds_.try_emplace(item.mesh.get());
            if (added)
                it->second = {item.mesh, render::meshBounds(item.mesh.get())};
            const render::Bounds& bounds = it->second.second;
            model = model * render::Mat4::translation(bounds.center()) * render::Mat4::scale(bounds.extent());
        }
        // The box's longest side, as the builder culls static parts.
        auto length = [&](std::size_t c) {
            return std::sqrt((model.m[c][0] * model.m[c][0]) + (model.m[c][1] * model.m[c][1])
                             + (model.m[c][2] * model.m[c][2]));
        };
        if (std::max({length(0), length(1), length(2)}) < options_.minSize)
            continue;
        builder.addBox(model, item.color, scene_.owners()[i]->weak_from_this());
        ++count;
    }
    counters_.movingParts = count;
    return std::make_shared<render::MapMesh>(builder.finish());
}

void MapScene::work(const std::stop_token& stop)
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [&] { return pending_.has_value(); }))
                return;
            if (!pending_)
                continue;
            job = std::move(*pending_);
            pending_.reset();
            busy_ = true;
        }
        auto mesh = build(std::move(job));
        {
            std::scoped_lock lock(mutex_);
            finished_ = mesh;
            latest_ = std::move(mesh);
            busy_ = false;
        }
        done_.notify_all();
    }
}

void MapScene::update(double time, lunar::ui::WorldMap& map)
{
    update(time);
    if (auto mesh = takeMesh())
        map.setMesh(std::move(mesh));
    if (auto moving = takeMoving())
        map.setMoving(std::move(moving));
}

std::shared_ptr<const render::MapMesh> MapScene::takeMesh()
{
    std::scoped_lock lock(mutex_);
    return std::move(finished_);
}

void MapScene::update(double time)
{
    if (scene_.extractions() != extractions_) {
        // The items' indices changed: nothing below may use the old ones
        // until the parts are sorted again, at most every classifyInterval.
        if (time - classifiedAt_ < classifyInterval)
            return;
        classifiedAt_ = time;
        // Parts came or went: rebuild once things settle, if the static set changed.
        classify();
        std::size_t hash = 0;
        for (std::size_t i = 0; i < kinds_.size(); ++i)
            if (kinds_[i] == Kind::Static)
                hash = (hash ^ reinterpret_cast<std::uintptr_t>(scene_.owners()[i])) * 0x100000001b3ull;
        if (hash != staticHash_) {
            staticHash_ = hash;
            staticDirty_ = true;
            changedAt_ = time;
        }
    }

    // A slice of the static parts each frame: any that moved become boxes.
    const auto& items = scene_.items();
    const auto& owners = scene_.owners();
    const std::size_t n = std::min(kinds_.size(), movedChecksPerUpdate);
    for (std::size_t step = 0; step < n && !kinds_.empty(); ++step) {
        const std::size_t i = movedCursor_++ % kinds_.size();
        if (kinds_[i] == Kind::Static && items[i].model != placed_[i]) {
            kinds_[i] = Kind::Moving;
            known_[owners[i]].moved = true;
            ++counters_.moved;
            staticDirty_ = true;
            changedAt_ = time;
        }
    }

    // Debounced: a second after the last change, at most every rebuildInterval.
    if (staticDirty_ && time - changedAt_ >= 1.0 && time - builtAt_ >= rebuildInterval) {
        if (auto job = snapshot()) {
            std::scoped_lock lock(mutex_);
            pending_ = std::move(*job);
        }
        wake_.notify_all();
        staticDirty_ = false;
        builtAt_ = time;
        ++counters_.rebuilds;
    }
    if (time - boxedAt_ >= 1.0 / movingRate) {
        moving_ = boxes();
        boxedAt_ = time;
    }
}

void MapScene::buildNow(lunar::ui::WorldMap& map)
{
    classify();
    if (auto job = snapshot()) {
        std::unique_lock lock(mutex_);
        pending_ = std::move(*job);
        wake_.notify_all();
        done_.wait(lock, [&] { return !pending_ && !busy_ && finished_; });
        map.setMesh(std::move(finished_));
        finished_ = nullptr;
    }
    staticDirty_ = false;
    map.setMoving(boxes());
}
