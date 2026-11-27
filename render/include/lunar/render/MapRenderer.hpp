#pragma once

#include <lunar/Math.hpp>
#include <lunar/render/Allocator.hpp>
#include <lunar/render/MapMesh.hpp>
#include <lunar/render/Mat4.hpp>
#include <lunar/render/Vulkan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace lunar::render {

class Context;

// View space to Vulkan clip space for a box width x height studs across,
// with reversed Z like perspective(): nearZ maps to depth 1, farZ to 0.
Mat4 orthographic(float width, float height, float nearZ, float farZ);

// Draws MapMeshes into images of its own for the UI to show (Lunarstone's
// minimap and full-screen map): one tiny pipeline, no textures, no shadows,
// no light grid. A static mesh (the place) plus a small one that changes
// (moving parts as boxes), into up to maxViews images, each only on the
// frames it's asked for; between them the last picture stays.
//
// record() runs outside any rendering scope, e.g. from the UI's prepass, in
// the frame's command buffer. Each image is left in
// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; as for MeshRenderer, recording
// into a slot is only allowed once the GPU finished that slot's last frame.
class MapRenderer {
public:
    // sRGB-encoded values in a UNORM image, the way the UI samples images.
    static constexpr VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr std::size_t maxViews = 2;

    struct View {
        Mat4 viewProjection; // reversed Z (orthographic(), perspective())
        // The camera's position, or for orthographic views the direction
        // towards it.
        Vector3 eye;
        bool orthographic = false;
        VkExtent2D extent{};
        std::array<float, 4> background{0.0f, 0.0f, 0.0f, 0.0f}; // sRGB, straight alpha
        bool moving = true;                                      // the moving mesh too
        bool highlight = false;                                  // and the highlight mesh's outline
        bool highlightFaces = true;                              // and its see-through faces
    };

    static std::expected<std::unique_ptr<MapRenderer>, std::string> create(Context& context, std::uint32_t slots = 2);
    MapRenderer(const MapRenderer&) = delete;
    MapRenderer& operator=(const MapRenderer&) = delete;
    ~MapRenderer();

    // Uploaded by the next record(); null clears.
    void setMesh(std::shared_ptr<const MapMesh> mesh);
    void setMoving(std::shared_ptr<const MapMesh> mesh) { setDynamic(Moving, std::move(mesh)); }
    // Drawn see-through over everything in views that ask for it, e.g.
    // the box of the part the pointer is on.
    void setHighlight(std::shared_ptr<const MapMesh> mesh) { setDynamic(Highlight, std::move(mesh)); }
    const std::shared_ptr<const MapMesh>& mesh() const { return mesh_; }

    // Draws view `index` (below maxViews) on the next record().
    void request(std::size_t index, const View& view);
    void record(VkCommandBuffer cmd, std::uint32_t slot);

    // The view's image once drawn, else null. Changes when its size does.
    VkImageView imageView(std::size_t index) const;
    VkExtent2D extent(std::size_t index) const { return views_[index].extent; }

    struct Stats {
        std::uint32_t views = 0;     // drawn by the last record()
        std::uint32_t draws = 0;     // ...in this many draw calls
        std::uint64_t triangles = 0; // ...of this many triangles
        std::uint64_t meshBytes = 0; // the static mesh on the GPU
        std::uint64_t movingBytes = 0;
        std::uint64_t imageBytes = 0; // colour and depth of every view
    };
    const Stats& stats() const { return stats_; }

private:
    struct Target {
        Image color;
        Image depth;
        VkExtent2D extent{};
        bool drawn = false;
        bool requested = false;
        View view;
    };
    // A MapMesh's three arrays in buffers: vertices, indices and instances,
    // at these byte offsets (one buffer for the moving mesh, three for the static one).
    struct Buffers {
        VkBuffer vertices = VK_NULL_HANDLE;
        VkBuffer indices = VK_NULL_HANDLE;
        VkBuffer instances = VK_NULL_HANDLE;
        VkDeviceSize vertexOffset = 0;
        VkDeviceSize indexOffset = 0;
        VkDeviceSize instanceOffset = 0;
    };
    // Small meshes that change, copied into each slot's own host-visible buffer.
    enum DynamicKind : std::uint8_t { Moving, Highlight, DynamicKinds };
    struct Dynamic {
        std::shared_ptr<const MapMesh> mesh;
        std::uint64_t version = 0;
    };
    struct SlotDynamic {
        Buffer buffer;
        std::uint64_t version = 0;
        Buffers buffers;
        bool ready = false;
    };
    struct Slot {
        std::vector<Buffer> staging;
        std::array<SlotDynamic, DynamicKinds> dynamic;
    };
    struct Retired {
        std::uint64_t frame = 0; // freed once this many frames later every slot has moved on
        Buffer buffer;
        Image image;
    };

    explicit MapRenderer(Context& context) : context_(&context) {}
    void createPipeline();
    void upload(VkCommandBuffer cmd, Slot& slot);
    void retire(Buffer buffer);
    void retire(Image image);
    void setDynamic(DynamicKind kind, std::shared_ptr<const MapMesh> mesh);
    void uploadDynamic(Slot& slot, DynamicKind kind);
    void draw(VkCommandBuffer cmd, Target& target, Slot& slot);
    // One mesh's world tiles and shape batches, the ones the view sees, as
    // faces or as outlines.
    void drawMesh(VkCommandBuffer cmd, const MapMesh& mesh, const Buffers& buffers, const Mat4& viewProjection,
                  bool outlines);

    Context* context_;
    UniquePipelineLayout layout_;
    UniquePipeline faces_;
    UniquePipeline outlines_;
    UniquePipeline highlightFaces_; // blended, over everything
    UniquePipeline highlightOutlines_;
    std::vector<Slot> slots_;
    std::array<Target, maxViews> views_;
    std::vector<Retired> retired_;

    std::shared_ptr<const MapMesh> mesh_;
    bool meshDirty_ = false;
    std::shared_ptr<const MapMesh> uploaded_; // what vertices_ and indices_ hold
    Buffer vertices_;
    Buffer indices_;
    Buffer instances_;
    std::array<Dynamic, DynamicKinds> dynamic_;
    std::uint64_t frame_ = 0;
    Stats stats_;
};

} // namespace lunar::render
