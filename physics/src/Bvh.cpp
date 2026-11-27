#include <lunar/physics/Bvh.hpp>

#include <algorithm>
#include <numeric>

namespace lunar::physics {

namespace {

constexpr std::uint32_t leafSize = 4;

float axisOf(Vector3 v, int axis)
{
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

} // namespace

void Bvh::build(std::span<const Aabb> boxes)
{
    items_.resize(boxes.size());
    std::ranges::iota(items_, 0u);
    buildNodes(boxes);
}

void Bvh::build(std::span<const Aabb> boxes, std::span<const std::uint32_t> ids)
{
    // Built over positions in boxes, then renamed to the ids.
    items_.resize(boxes.size());
    std::ranges::iota(items_, 0u);
    buildNodes(boxes);
    for (std::uint32_t& item : items_)
        item = ids[item];
    positionOf_.assign(ids.empty() ? 0 : std::ranges::max(ids) + 1, absent);
    for (std::uint32_t k = 0; k < items_.size(); ++k)
        positionOf_[items_[k]] = k;
}

void Bvh::buildNodes(std::span<const Aabb> boxes)
{
    nodes_.clear();
    boxes_.clear();
    parents_.clear();
    leafOf_.assign(items_.size(), 0);
    positionOf_.assign(items_.size(), absent);
    if (boxes.empty())
        return;
    nodes_.reserve(2 * boxes.size() / leafSize + 2);

    struct Task {
        std::uint32_t node;
        std::uint32_t begin;
        std::uint32_t end;
    };
    std::vector<Task> tasks{{0, 0, static_cast<std::uint32_t>(boxes.size())}};
    nodes_.emplace_back();
    parents_.push_back(0);
    while (!tasks.empty()) {
        const Task task = tasks.back();
        tasks.pop_back();
        Aabb box;
        Aabb centers;
        for (std::uint32_t i = task.begin; i < task.end; ++i) {
            box.expand(boxes[items_[i]]);
            centers.expand(boxes[items_[i]].center());
        }
        nodes_[task.node].box = box;
        const std::uint32_t count = task.end - task.begin;
        if (count <= leafSize) {
            nodes_[task.node].first = task.begin;
            nodes_[task.node].count = count;
            for (std::uint32_t i = task.begin; i < task.end; ++i)
                leafOf_[i] = task.node;
            continue;
        }
        const Vector3 spread = centers.extent();
        const int axis = spread.x >= spread.y && spread.x >= spread.z ? 0 : spread.y >= spread.z ? 1 : 2;
        const std::uint32_t mid = task.begin + count / 2;
        std::nth_element(items_.begin() + task.begin, items_.begin() + mid, items_.begin() + task.end,
                         [&](std::uint32_t a, std::uint32_t b) {
                             return axisOf(boxes[a].center(), axis) < axisOf(boxes[b].center(), axis);
                         });
        const auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.emplace_back();
        nodes_.emplace_back();
        parents_.push_back(task.node);
        parents_.push_back(task.node);
        nodes_[task.node].first = left;
        nodes_[task.node].count = 0;
        tasks.push_back({left, task.begin, mid});
        tasks.push_back({left + 1, mid, task.end});
    }
    boxes_.clear();
    boxes_.reserve(items_.size());
    for (std::uint32_t k = 0; k < items_.size(); ++k) {
        boxes_.push_back(boxes[items_[k]]);
        positionOf_[items_[k]] = k;
    }
}

void Bvh::refit(std::span<const Aabb> boxes)
{
    for (std::size_t k = 0; k < items_.size(); ++k)
        boxes_[k] = boxes[items_[k]];
    // Children always come after their parent, so walking backwards sees them first.
    for (std::size_t n = nodes_.size(); n-- > 0;) {
        Node& node = nodes_[n];
        Aabb box;
        if (node.count > 0) {
            for (std::uint32_t i = 0; i < node.count; ++i)
                box.expand(boxes_[node.first + i]);
        } else {
            box.expand(nodes_[node.first].box);
            box.expand(nodes_[node.first + 1].box);
        }
        node.box = box;
    }
}

bool Bvh::update(std::uint32_t item, const Aabb& box)
{
    if (!contains(item))
        return false;
    const std::uint32_t position = positionOf_[item];
    boxes_[position] = box;
    std::uint32_t n = leafOf_[position];
    while (true) {
        Node& node = nodes_[n];
        Aabb fitted;
        if (node.count > 0) {
            for (std::uint32_t i = 0; i < node.count; ++i)
                fitted.expand(boxes_[node.first + i]);
        } else {
            fitted.expand(nodes_[node.first].box);
            fitted.expand(nodes_[node.first + 1].box);
        }
        node.box = fitted;
        if (n == 0)
            return true;
        n = parents_[n];
    }
}

} // namespace lunar::physics
