#include "render_gpu.hpp"

#include <lunar/Classes.hpp>
#include <lunar/render/MapMesh.hpp>
#include <lunar/render/MapRenderer.hpp>
#include <lunar/render/Primitives.hpp>

#include <cmath>
#include <memory>
#include <vector>

using lunar::CFrame;
using lunar::Color3;
using lunar::Vector3;
using namespace lunar::render;

namespace {

DrawItem box(Vector3 position, Vector3 size, Color3 color = {0.5f, 0.5f, 0.5f})
{
    return {partMatrix(CFrame(position), size), color};
}

bool near(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

// A flat square of terrain in world space, two triangles, all one material.
lunar::MeshData terrainSquare(float size, float y)
{
    lunar::MeshData mesh;
    for (const auto& [x, z] : {std::pair{0.0f, 0.0f}, {size, 0.0f}, {size, size}, {0.0f, size}}) {
        lunar::MeshVertex v;
        v.position = {x, y, z};
        v.materialWeights[0] = 255;
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 2, 1, 0, 3, 2};
    return mesh;
}

std::size_t instancesOfShape(const MapMesh& mesh, std::uint32_t shape)
{
    std::size_t count = 0;
    for (const MapBatch& batch : mesh.batches)
        count += batch.shape == shape ? batch.instanceCount : 0;
    return count;
}

} // namespace

TEST(map_mesh_leaves_out_small_and_clear_parts)
{
    MapMeshBuilder builder;
    CHECK(builder.addItem(box({0, 0, 0}, {8, 1, 8})));
    CHECK(!builder.addItem(box({10, 0, 0}, {1, 1, 1}))); // shorter than minSize
    DrawItem glass = box({20, 0, 0}, {10, 10, 1});
    glass.transparency = 0.9f;
    CHECK(!builder.addItem(glass));
    DrawItem decal = box({30, 0, 0}, {10, 10, 1});
    decal.decal = true;
    CHECK(!builder.addItem(decal));
    const MapMesh mesh = builder.finish();
    CHECK_EQ(mesh.stats.parts, std::size_t{1});
    CHECK_EQ(mesh.stats.small, std::size_t{1});
    CHECK_EQ(mesh.stats.clear, std::size_t{1});
    // The identity plus one cube.
    CHECK_EQ(mesh.instances.size(), std::size_t{2});
    CHECK_EQ(mesh.owners.size(), mesh.instances.size());
    CHECK_EQ(mesh.shapes.size(), std::size_t{1});
    CHECK_EQ(mesh.shapes[0].indexCount, 36u);
    CHECK_EQ(mesh.shapes[0].edgeCount, 24u); // the cube's 12 folds
    CHECK(near(mesh.bounds.min.x, -4.0f) && near(mesh.bounds.max.x, 4.0f));
    CHECK(near(mesh.bounds.min.y, -0.5f) && near(mesh.bounds.max.y, 0.5f));
    CHECK_EQ(mesh.stats.triangles, std::uint64_t{12});
}

TEST(map_mesh_instances_share_shapes_and_batch_by_tile)
{
    MapMeshOptions options;
    options.tileSize = 100.0f;
    MapMeshBuilder builder(options);
    const auto ball = primitiveMesh(Primitive::Ball);
    for (int i = 0; i < 4; ++i) {
        DrawItem item = box({static_cast<float>(i) * 60.0f, 0, 0}, {10, 10, 10});
        item.mesh = ball;
        CHECK(builder.addItem(item));
    }
    CHECK(builder.addItem(box({0, 0, 0}, {6, 6, 6})));
    const MapMesh mesh = builder.finish();
    CHECK_EQ(mesh.shapes.size(), std::size_t{2}); // the cube and the ball, once each
    CHECK_EQ(mesh.instances.size(), std::size_t{6});
    // Balls at x 0, 60 | 120, 180 | ... fall in three tiles of 100 studs.
    std::size_t ballBatches = 0;
    for (const MapBatch& batch : mesh.batches)
        if (instancesOfShape(mesh, batch.shape) == 4)
            ++ballBatches;
    CHECK_EQ(ballBatches, std::size_t{2});
    // Batches run over consecutive instances, after the identity.
    std::uint32_t next = 1;
    for (const MapBatch& batch : mesh.batches) {
        CHECK_EQ(batch.firstInstance, next);
        next += batch.instanceCount;
    }
    CHECK_EQ(next, static_cast<std::uint32_t>(mesh.instances.size()));
}

TEST(map_mesh_colours_follow_the_palette)
{
    MapMeshBuilder builder;
    builder.addItem(box({0, 0, 0}, {8, 8, 8}, Color3::fromRGB(255, 255, 255)));
    builder.addItem(box({20, 0, 0}, {8, 8, 8}, Color3::fromRGB(0, 0, 0)));
    const MapMesh mesh = builder.finish();
    const Color3 white = unpackMapColor(mesh.instances[1].color);
    const Color3 black = unpackMapColor(mesh.instances[2].color);
    // Dark greys: white parts lighter than black ones, nothing bright.
    CHECK(white.g > black.g + 0.2f);
    CHECK(white.g < 0.6f);
    CHECK(black.g > 0.1f);
    CHECK(near(mapPartColor({1, 1, 1}).g, white.g, 0.01f));
    // Parts carry their kind in alpha.
    CHECK_EQ(mesh.vertices[0].color >> 24, static_cast<std::uint32_t>(MapKind::Part));
}

TEST(map_mesh_terrain_and_water)
{
    MapMeshOptions options;
    options.tileSize = 50.0f;
    MapMeshBuilder builder(options);
    const lunar::MeshData ground = terrainSquare(100.0f, 0.0f);
    const std::uint8_t palette[1] = {2}; // Terrain::materials[2], Grass
    const std::vector<Color3> colors(lunar::Terrain::materials.size(), Color3::fromRGB(106, 127, 63));
    builder.addTerrain(ground, palette, colors);
    builder.addWater(terrainSquare(100.0f, -2.0f), 4.0f);
    const MapMesh mesh = builder.finish();
    CHECK_EQ(mesh.stats.terrain, std::size_t{2});
    CHECK(mesh.stats.water >= 1);
    CHECK(!mesh.tiles.empty());
    CHECK(near(mesh.terrainCell, 4.0f));
    bool sawTerrain = false;
    bool sawWater = false;
    for (const MapVertex& v : mesh.vertices) {
        const auto kind = static_cast<MapKind>(v.color >> 24);
        if (kind == MapKind::Terrain) {
            sawTerrain = true;
            const Color3 c = unpackMapColor(v.color);
            CHECK(near(c.g, mapTerrainColor(colors[2]).g, 0.01f));
        } else if (kind == MapKind::Water) {
            sawWater = true;
            CHECK(near(v.position[1], -2.0f));
        }
    }
    CHECK(sawTerrain && sawWater);
    // Terrain triangles come with their edges as lines; water has none.
    std::uint32_t edges = 0;
    for (const MapTile& tile : mesh.tiles)
        edges += tile.edgeCount;
    CHECK(edges >= 10); // two triangles cut across tiles: at least their five edges, twice
    CHECK(near(mesh.bounds.min.y, -2.0f) && near(mesh.bounds.max.y, 0.0f));
}

TEST(map_mesh_cluster_merges_close_corners)
{
    // A cube made of 24 corners (split per face) welds back to 8 on a fine grid...
    const lunar::MeshData cube = makePrimitive(Primitive::Block);
    std::vector<Vector3> positions;
    positions.reserve(cube.vertices.size());
    for (const lunar::MeshVertex& v : cube.vertices)
        positions.push_back(v.position);
    const auto fine = MapMeshBuilder::cluster(positions, {}, cube.indices, 0.1f);
    CHECK_EQ(fine.positions.size(), std::size_t{8});
    CHECK_EQ(fine.indices.size(), std::size_t{36});
    // ...and collapses to nothing when one cell holds it all.
    const auto coarse = MapMeshBuilder::cluster(positions, {}, cube.indices, 10.0f);
    CHECK(coarse.indices.empty());
    // A finely tessellated ball keeps far fewer triangles on a coarse grid.
    const lunar::MeshData ball = makePrimitive(Primitive::Ball, 48);
    positions.clear();
    for (const lunar::MeshVertex& v : ball.vertices)
        positions.push_back(v.position);
    const auto reduced = MapMeshBuilder::cluster(positions, {}, ball.indices, 0.25f);
    CHECK(!reduced.indices.empty());
    CHECK(reduced.indices.size() * 4 < ball.indices.size());
}

TEST(map_mesh_edges)
{
    const lunar::MeshData cube = makePrimitive(Primitive::Block);
    std::vector<Vector3> positions;
    positions.reserve(cube.vertices.size());
    for (const lunar::MeshVertex& v : cube.vertices)
        positions.push_back(v.position);
    const auto welded = MapMeshBuilder::cluster(positions, {}, cube.indices, 0.1f);
    // Folds only: the 12 edges, not the face diagonals.
    CHECK_EQ(MapMeshBuilder::edges(welded.positions, welded.indices, 30.0f).size(), std::size_t{24});
    // Every edge: 12 plus 6 diagonals.
    CHECK_EQ(MapMeshBuilder::edges(welded.positions, welded.indices, std::nullopt).size(), std::size_t{36});
}

TEST(map_mesh_foliage_becomes_a_post)
{
    MapMeshBuilder builder;
    auto leaves = std::make_shared<lunar::Image>();
    leaves->width = 2;
    leaves->height = 2;
    leaves->rgba = {0, 200, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; // a quarter opaque
    DrawItem tree = box({0, 10, 0}, {20, 20, 20});
    tree.mesh = primitiveMesh(Primitive::Ball);
    tree.texture = leaves;
    tree.alphaMode = TextureAlpha::Transparency;
    CHECK(builder.addItem(tree));
    const MapMesh mesh = builder.finish();
    CHECK_EQ(mesh.stats.foliage, std::size_t{1});
    CHECK_EQ(mesh.shapes.size(), std::size_t{1}); // the cube only
    // Thin and no taller than the tree.
    CHECK(mesh.bounds.extent().x < 3.0f);
    CHECK(mesh.bounds.max.y <= 20.0f + 1e-3f);
    // Its colour is the visible texels' (green), in the palette.
    const Color3 c = unpackMapColor(mesh.instances[1].color);
    CHECK(near(c.g, mapPartColor(Color3{0, 200.0f / 255.0f, 0}).g, 0.02f));
}

TEST(map_mesh_keeps_owners)
{
    auto part = std::make_shared<lunar::Part>();
    MapMeshBuilder builder;
    builder.addItem(box({0, 0, 0}, {8, 8, 8}), part);
    builder.addBox(partMatrix(CFrame(20, 0, 0), {8, 8, 8}), {1, 1, 1});
    const MapMesh mesh = builder.finish();
    CHECK_EQ(mesh.owners.size(), std::size_t{3});
    std::size_t owned = 0;
    for (const auto& owner : mesh.owners)
        owned += owner.lock() == part ? 1u : 0u;
    CHECK_EQ(owned, std::size_t{1});
}

TEST(map_orthographic_reversed_z)
{
    const Mat4 p = orthographic(100.0f, 50.0f, 1.0f, 101.0f);
    const Vec4 nearPoint = p * Vec4{50.0f, 25.0f, -1.0f, 1.0f};
    const Vec4 farPoint = p * Vec4{-50.0f, -25.0f, -101.0f, 1.0f};
    CHECK(near(nearPoint.x, 1.0f) && near(nearPoint.y, -1.0f) && near(nearPoint.z, 1.0f));
    CHECK(near(farPoint.x, -1.0f) && near(farPoint.y, 1.0f) && near(farPoint.z, 0.0f));
}

GPU_TEST(map_renderer_draws_views)
{
    auto renderer = MapRenderer::create(context, 2);
    if (!renderer)
        throw lt::Failure{renderer.error()};
    MapMeshBuilder builder;
    builder.addItem(box({0, 0, 0}, {8, 8, 8}));
    const std::uint8_t palette[1] = {2};
    const std::vector<Color3> colors(lunar::Terrain::materials.size(), Color3{0.4f, 0.5f, 0.3f});
    builder.addTerrain(terrainSquare(64.0f, -4.0f), palette, colors);
    (*renderer)->setMesh(std::make_shared<MapMesh>(builder.finish()));
    MapMeshBuilder moving;
    moving.addBox(partMatrix(CFrame(10, 0, 0), {4, 4, 4}), {1, 0, 0});
    (*renderer)->setMoving(std::make_shared<MapMesh>(moving.finish()));
    moving.addBox(partMatrix(CFrame(0, 0, 0), {8, 8, 8}), {1, 1, 1});
    (*renderer)->setHighlight(std::make_shared<MapMesh>(moving.finish()));

    MapRenderer::View top;
    const Vector3 eye{0, 100, 0};
    top.viewProjection = orthographic(128, 128, 1, 200) * viewMatrix(Camera::lookAt(eye, {0, 0, 0}));
    top.eye = {0, 1, 0};
    top.orthographic = true;
    top.extent = {64, 64};
    top.highlight = true;
    Camera camera;
    camera.cframe = Camera::lookAt({60, 60, 60}, {0, 0, 0});
    MapRenderer::View tilted;
    tilted.viewProjection = camera.viewProjection(1.5f);
    tilted.eye = camera.cframe.position;
    tilted.extent = {96, 64};
    for (std::uint32_t frame = 0; frame < 3; ++frame) {
        (*renderer)->request(0, top);
        (*renderer)->request(1, tilted);
        context.submitAndWait([&](VkCommandBuffer cmd) { (*renderer)->record(cmd, frame % 2); });
        CHECK_EQ((*renderer)->stats().views, 2u);
        CHECK((*renderer)->stats().triangles >= 12);
    }
    CHECK((*renderer)->imageView(0) != VK_NULL_HANDLE);
    CHECK((*renderer)->imageView(1) != VK_NULL_HANDLE);
    CHECK((*renderer)->stats().meshBytes > 0);
    // A resize makes new images; nothing asked for draws nothing.
    tilted.extent = {80, 40};
    (*renderer)->request(1, tilted);
    context.submitAndWait([&](VkCommandBuffer cmd) { (*renderer)->record(cmd, 1); });
    CHECK_EQ((*renderer)->extent(1).width, 80u);
    context.submitAndWait([&](VkCommandBuffer cmd) { (*renderer)->record(cmd, 0); });
    CHECK_EQ((*renderer)->stats().views, 0u);
}
