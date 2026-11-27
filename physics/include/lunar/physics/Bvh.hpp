#pragma once

#include <lunar/physics/Geometry.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace lunar::physics {

// Bounding volume hierarchy over a fixed set of boxes, built top-down by
// splitting at the median centroid of the longest axis. Items are the
// indices of the boxes it was built from, or ids given with them.
class Bvh {
public:
    Bvh() = default;
    explicit Bvh(std::span<const Aabb> boxes) { build(boxes); }

    void build(std::span<const Aabb> boxes);
    // boxes[i] belongs to item ids[i].
    void build(std::span<const Aabb> boxes, std::span<const std::uint32_t> ids);
    // Same items, new boxes (indexed by item): grows and shrinks the nodes
    // without rebuilding the tree. Quality drops if items move far; rebuild then.
    void refit(std::span<const Aabb> boxes);
    // One item's new box, refitting only the nodes above it. False when the
    // tree doesn't hold the item.
    bool update(std::uint32_t item, const Aabb& box);
    bool contains(std::uint32_t item) const { return item < positionOf_.size() && positionOf_[item] != absent; }
    bool empty() const { return nodes_.empty(); }
    std::size_t size() const { return items_.size(); }
    Aabb bounds() const { return nodes_.empty() ? Aabb{} : nodes_.front().box; }

    // Calls fn(item) for every box overlapping `box`.
    template <class Fn>
    void query(const Aabb& box, Fn fn) const
    {
        if (nodes_.empty())
            return;
        std::uint32_t stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node& node = nodes_[stack[--top]];
            if (!node.box.overlaps(box))
                continue;
            if (node.count > 0) {
                for (std::uint32_t i = 0; i < node.count; ++i)
                    if (boxes_[node.first + i].overlaps(box))
                        fn(items_[node.first + i]);
            } else if (top + 2 <= 64) {
                stack[top++] = node.first;
                stack[top++] = node.first + 1;
            }
        }
    }

    // Walks the boxes a ray (origin + t * direction, t in [0, 1]) passes,
    // nearest first. fn(item, maxT) returns the new maxT: a hit's fraction
    // to shorten the ray, or maxT unchanged.
    template <class Fn>
    void raycast(Vector3 origin, Vector3 direction, Fn fn) const
    {
        if (nodes_.empty())
            return;
        const Vector3 inv{1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z};
        float maxT = 1.0f;
        struct Entry {
            std::uint32_t node;
            float t;
        };
        Entry stack[64];
        int top = 0;
        if (auto t = rayAabb(nodes_[0].box, origin, inv, maxT))
            stack[top++] = {0, *t};
        while (top > 0) {
            const Entry entry = stack[--top];
            if (entry.t > maxT)
                continue;
            const Node& node = nodes_[entry.node];
            if (node.count > 0) {
                for (std::uint32_t i = 0; i < node.count; ++i)
                    if (rayAabb(boxes_[node.first + i], origin, inv, maxT))
                        maxT = fn(items_[node.first + i], maxT);
                continue;
            }
            auto ta = rayAabb(nodes_[node.first].box, origin, inv, maxT);
            auto tb = rayAabb(nodes_[node.first + 1].box, origin, inv, maxT);
            // Push the far child first so the near one pops next.
            if (ta && tb && *ta < *tb) {
                if (top + 2 > 64)
                    continue;
                stack[top++] = {node.first + 1, *tb};
                stack[top++] = {node.first, *ta};
            } else {
                // b is nearer (or the only one): a goes underneath.
                if (ta && top < 64)
                    stack[top++] = {node.first, *ta};
                if (tb && top < 64)
                    stack[top++] = {node.first + 1, *tb};
            }
        }
    }

private:
    struct Node {
        Aabb box;
        std::uint32_t first = 0; // leaf: first item; inner: left child (right is first + 1)
        std::uint32_t count = 0; // items in a leaf, 0 for inner nodes
    };

    static constexpr std::uint32_t absent = ~0u;

    void buildNodes(std::span<const Aabb> boxes);

    std::vector<Node> nodes_;
    std::vector<std::uint32_t> items_;
    std::vector<Aabb> boxes_;               // items_' boxes, in the same order
    std::vector<std::uint32_t> parents_;    // per node; the root's is itself
    std::vector<std::uint32_t> leafOf_;     // per position in items_: its leaf
    std::vector<std::uint32_t> positionOf_; // per item: its position in items_, or absent
};

} // namespace lunar::physics
