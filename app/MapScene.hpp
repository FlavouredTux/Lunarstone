#pragma once

#include "PlaceScene.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/MapMesh.hpp>
#include <lunar/ui/WorldMap.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Feeds Lunarstone's map (ui::WorldMap) from a PlaceScene: the place's
// static parts plus the terrain at a coarse level, merged into one
// render::MapMesh on a worker thread, and the parts that move as boxes.
//
// Static: anchored parts outside characters. Moving: unanchored ones, and
// anchored ones seen to move (a battle bus on a tween), which leave the
// static mesh at its next rebuild. Characters are left out: the map shows
// players as markers. The static mesh is rebuilt when its parts come or go
// or move, at most every rebuildInterval seconds; the boxes are redone
// movingRate times a second. Reads the game only on the calling thread.
class MapScene {
public:
    static constexpr double rebuildInterval = 5.0;
    static constexpr double movingRate = 5.0;
    static constexpr std::size_t maxMovingBoxes = 4000;
    // Static items checked for having moved per update, round robin.
    static constexpr std::size_t movedChecksPerUpdate = 4000;
    // Seconds between sorting the parts again after the scene re-extracted
    // them (bullets and debris make it do that often).
    static constexpr double classifyInterval = 0.5;
    // Parts that don't collide and are longer than this (studs) are left out.
    static constexpr float hugeNonColliding = 256.0f;

    MapScene(PlaceScene& scene, lunar::render::MapMeshOptions options = {});
    MapScene(const MapScene&) = delete;
    MapScene& operator=(const MapScene&) = delete;
    ~MapScene();

    // Once a frame (seconds of play time): hands the map what finished.
    void update(double time, lunar::ui::WorldMap& map);
    // The same without a map: what finished waits in takeMesh/takeMoving.
    void update(double time);
    std::shared_ptr<const lunar::render::MapMesh> takeMesh();
    std::shared_ptr<const lunar::render::MapMesh> takeMoving() { return std::move(moving_); }

    struct Counters {
        std::size_t rebuilds = 0;    // static meshes asked for
        std::size_t movingParts = 0; // drawn as boxes last time
        std::size_t moved = 0;       // anchored parts seen to move
    };
    const Counters& counters() const { return counters_; }
    // Builds the static mesh and the boxes right away, waiting for it.
    void buildNow(lunar::ui::WorldMap& map);

    // Terrain at its map level, the latest static mesh (null before the
    // first) and the boxes, for reports.
    std::shared_ptr<const lunar::render::MapMesh> mesh() const;
    int terrainLevel() const { return terrainLevel_; }

private:
    struct Job {
        std::vector<lunar::render::DrawItem> items;
        std::vector<std::weak_ptr<const lunar::Instance>> owners; // per item
    };

    // Sorts the scene's part items into static, moving and left out.
    void classify();
    std::optional<Job> snapshot();
    std::shared_ptr<const lunar::render::MapMesh> build(Job job);
    std::shared_ptr<const lunar::render::MapMesh> boxes();
    void meshTerrain();
    void work(const std::stop_token& stop);

    PlaceScene& scene_;
    lunar::render::MapMeshOptions options_;
    enum class Kind : std::uint8_t { Skip, Static, Moving };
    std::vector<Kind> kinds_;                 // per part item
    std::vector<lunar::render::Mat4> placed_; // each static item's model when first seen
    // What's known of each part, kept across extractions: whether it can be
    // on the map at all (not in a character, not a huge non-colliding
    // volume), whether it was seen moving, and where it was first seen, so
    // a part that keeps moving is still caught. Keys are only compared.
    struct Known {
        bool candidate = false;
        bool moved = false;
        lunar::render::Mat4 placed;
    };
    std::unordered_map<const lunar::Instance*, Known> known_;
    double classifiedAt_ = -1e9;
    std::uint64_t extractions_ = ~std::uint64_t{0};
    std::size_t movedCursor_ = 0;
    std::unordered_map<const lunar::MeshData*, std::pair<lunar::render::MeshPtr, lunar::render::Bounds>> meshBounds_;
    bool staticDirty_ = true;
    double changedAt_ = -1e9;
    std::size_t staticHash_ = 0; // of the static parts' owners, to spot a different set
    double builtAt_ = -1e9;
    double boxedAt_ = -1e9;

    // Terrain meshed once at a coarse level (the place's terrain doesn't change).
    std::vector<std::shared_ptr<const lunar::terrain::TerrainMeshChunk>> terrain_;
    int terrainLevel_ = 0;

    // The worker: one job at a time, the newest wins.
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable done_;
    std::optional<Job> pending_;
    bool busy_ = false;
    std::shared_ptr<const lunar::render::MapMesh> finished_; // not yet handed to the map
    std::shared_ptr<const lunar::render::MapMesh> latest_;
    bool terrainMeshed_ = false;
    std::shared_ptr<const lunar::render::MapMesh> moving_; // not yet taken
    Counters counters_;
    std::jthread worker_; // last: stopped and joined first
};
