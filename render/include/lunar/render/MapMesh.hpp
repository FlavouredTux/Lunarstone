#pragma once

#include <lunar/Math.hpp>
#include <lunar/Mesh.hpp>
#include <lunar/render/Scene.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace lunar::render {

// Lunarstone's own map of the place (the real client has none): the whole
// place as one cheap, flat-coloured model for the minimap and the
// full-screen map. No textures, no materials: every vertex carries a
// colour, and faces are shaded by their own normal, which the map shader
// works out per pixel, so corners can be shared between faces.
//
// Parts are instances of a few shapes (each distinct mesh boiled down by
// vertex clustering, and the unit cube), so a forest of one tree costs one
// small mesh plus a matrix per tree. Terrain and water are world-space
// triangles. Both are sorted into squares of the ground (tiles) so a view
// can skip what it doesn't see.

// What a vertex belongs to, in its colour's alpha: the shader shades and
// outlines each kind its own way.
enum class MapKind : std::uint8_t { Terrain = 0, Water = 128, Part = 255 };

// One vertex: a position (world space for terrain, the shape's own space
// for shapes) and an sRGB colour, already in the map's palette.
struct MapVertex {
    float position[3]{};
    std::uint32_t color = 0; // RGBA8, red in the low byte, MapKind in alpha
};

// One placed shape: the rows of its model matrix (shape space to world)
// and a colour the shape's vertex colours are multiplied by.
struct MapInstance {
    float rows[3][4]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    std::uint32_t color = 0xFFFFFFFFu;
};

// A shape's triangles in the shared vertex and index arrays, and its
// outline: pairs of indices (a line list) for the edges where its faces
// fold, after its triangles.
struct MapShape {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t firstEdge = 0;
    std::uint32_t edgeCount = 0; // indices, two per edge
    std::int32_t vertexOffset = 0;
    Bounds bounds; // shape space
};

// Instances of one shape in one tile, a range of MapMesh::instances. The
// instances are sorted by shape, then tile, so a view that sees several
// tiles in a row draws them at once.
struct MapBatch {
    std::uint32_t shape = 0;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
    Bounds bounds; // world space, around the instances
};

// World-space triangles of one tile (terrain and water), a range of
// MapMesh::indices over absolute vertex numbers, drawn with instance 0
// (the identity); then every terrain triangle's edges as a line list.
struct MapTile {
    Bounds bounds;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t firstEdge = 0;
    std::uint32_t edgeCount = 0;
};

struct MapMesh {
    std::vector<MapVertex> vertices;
    std::vector<std::uint32_t> indices; // three per triangle: world tiles first, then shapes
    std::vector<MapTile> tiles;         // world triangles, consecutive ranges
    std::vector<MapShape> shapes;
    std::vector<MapInstance> instances; // the identity first, then by shape and tile
    // The part each instance stands for (empty for the identity and boxes
    // added without one), for pointing at parts on the map. Only to be
    // locked on the game's thread.
    std::vector<std::weak_ptr<const Instance>> owners;
    std::vector<MapBatch> batches; // in instance order
    Bounds bounds{};               // around everything; a zero box when empty
    float terrainCell = 4.0f;      // studs across the terrain mesh's cells

    struct Stats {
        std::size_t parts = 0;       // items drawn
        std::size_t small = 0;       // left out as too small
        std::size_t clear = 0;       // left out as too see-through
        std::size_t boxes = 0;       // drawn as their box: tiny detailed meshes, meshes that cluster to nothing
        std::size_t meshes = 0;      // distinct meshes turned into shapes
        std::size_t foliage = 0;     // cut-out foliage drawn as a thin column
        std::size_t terrain = 0;     // terrain triangles
        std::size_t water = 0;       // water triangles
        std::uint64_t triangles = 0; // drawn for the whole map, instances counted each
        double milliseconds = 0.0;   // building it (set by whoever times it)
    } stats;

    bool empty() const { return tiles.empty() && batches.empty(); }
    // What the GPU holds of it.
    std::size_t bytes() const
    {
        return vertices.size() * sizeof(MapVertex) + indices.size() * sizeof(std::uint32_t)
               + instances.size() * sizeof(MapInstance);
    }
};

struct MapMeshOptions {
    // Parts whose box's longest side is shorter than this are left out:
    // from above they would be a pixel or two. GUESS: tuned by eye.
    float minSize = 4.0f;
    // More see-through parts (glass, invisible walls) are left out.
    float maxTransparency = 0.7f;
    // Meshes cut out of a texture with less than this much of it opaque
    // (on average) count as foliage. GUESS: tuned on Duvall's trees.
    float foliageAlpha = 0.75f;
    // Meshes are clustered on a grid of cells this many studs across (at
    // their largest instance's scale), but never finer than minCells nor
    // coarser than maxCells along their longest side. GUESS: tuned by eye.
    float cellStuds = 4.0f;
    int minCells = 4;
    int maxCells = 160;
    // Water is clustered this many times coarser than the terrain mesh's
    // cells: it's flat, and the terrain hides its shore.
    float waterCells = 4.0f;
    float tileSize = 256.0f;
    // Terrain water, a flat muted blue-grey rather than Terrain.WaterColor
    // (which is meant to be seen through). GUESS: picked by eye.
    Color3 water = Color3::fromRGB(40, 52, 64);
};

// The map's palette: a dark, almost grey "tactical" look with a touch of
// green, keeping only how light a colour is (and a hint of its hue), so
// the map reads like a massing model. Terrain sits darker than parts.
// GUESS: the numbers are picked by eye.
Color3 mapPartColor(Color3 color);
Color3 mapTerrainColor(Color3 color);

// sRGB colour packed as MapVertex::color.
std::uint32_t packMapColor(Color3 color, MapKind kind = MapKind::Part);
Color3 unpackMapColor(std::uint32_t color);

// Collects items, boxes and terrain into a MapMesh. Not thread safe, but
// needs nothing but its inputs: it can run on a worker thread over a copy of
// the scene's items.
class MapMeshBuilder {
public:
    explicit MapMeshBuilder(MapMeshOptions options = {});

    // A part's item: its mesh's coarsest level of detail boiled down (the
    // unit box for null meshes), coloured by its colour times the vertex
    // colours, or by its texture's mean colour when it has one. Decals,
    // terrain items and parts the options leave out are skipped; false then.
    bool addItem(const DrawItem& item, std::weak_ptr<const Instance> owner = {});
    // A box: the unit cube moved by model (partMatrix), e.g. a moving part.
    void addBox(const Mat4& model, Color3 color, std::weak_ptr<const Instance> owner = {});
    // A terrain mesh in world space (TerrainMeshChunk::solid): each vertex
    // takes the colour of its heaviest material, palette[slot] indexing
    // materialColors (Terrain::materials order).
    void addTerrain(const MeshData& solid, std::span<const std::uint8_t> palette,
                    std::span<const Color3> materialColors);
    // Terrain water in world space, in MapMeshOptions::water, clustered on
    // cells waterCells times `cellStuds` across (the terrain mesh's cells).
    void addWater(const MeshData& water, float cellStuds = 4.0f);

    // Everything added so far; the builder starts over (keeping its shapes'
    // cache for the next build).
    MapMesh finish();

    const MapMeshOptions& options() const { return options_; }

    // A mesh boiled down by vertex clustering: every corner moved to the
    // mean of its cell of a grid `cell` units across, triangles that
    // collapse dropped, as are repeats. Positions in the mesh's space;
    // colours are the corners' means. The grid starts at the mesh's lowest
    // corner, or with worldGrid at the origin, so pieces clustered apart
    // still meet. Public for the tests.
    struct Clustered {
        std::vector<Vector3> positions;
        std::vector<Color3> colors;
        std::vector<std::uint32_t> indices;
    };
    static Clustered cluster(std::span<const Vector3> positions, std::span<const Color3> colors,
                             std::span<const std::uint32_t> indices, float cell, bool worldGrid = false);
    // The edges of a triangle list worth outlining, as pairs of indices:
    // with foldDegrees, where the faces on either side meet at more than
    // that angle (and open edges); without, every edge once. Public for the tests.
    static std::vector<std::uint32_t> edges(std::span<const Vector3> positions, std::span<const std::uint32_t> indices,
                                            std::optional<float> foldDegrees);

private:
    struct Pending {
        Mat4 model;
        Color3 color;
        MeshPtr mesh; // null: the unit cube
        std::weak_ptr<const Instance> owner;
    };
    struct Source {
        MeshPtr mesh;
        Bounds bounds;           // of its coarsest level, mesh space
        float worldLongest = 0;  // the longest side of its biggest instance this build
        int cells = 0;           // what its shape was clustered with
        std::uint32_t shape = 0; // in shapes_, valid when cells > 0
        bool box = false;        // clustered to nothing: drawn as its box
        bool empty = false;      // no triangles at all: left out
    };
    struct Shape {
        std::vector<MapVertex> vertices; // shape space
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> edges;
        Bounds bounds;
    };
    struct Tile {
        std::vector<MapVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> edges;
        Bounds bounds;
        bool any = false;
    };

    Source& source(const MeshPtr& mesh);
    Color3 textureMean(const DrawItem& item);
    void appendWorld(std::span<const Vector3> positions, std::span<const Color3> colors,
                     std::span<const std::uint32_t> indices, MapKind kind);

    MapMeshOptions options_;
    std::vector<Pending> pending_;
    std::unordered_map<const MeshData*, Source> sources_;
    std::vector<Shape> shapes_; // 0 is the unit cube
    struct Mean {
        ImagePtr image;
        Color3 rgb;
        float alpha = 1.0f;
    };
    std::unordered_map<const lunar::Image*, Mean> means_;
    std::unordered_map<std::uint64_t, Tile> tiles_;
    MapMesh::Stats stats_;
    float terrainCell_ = 4.0f;
};

} // namespace lunar::render
