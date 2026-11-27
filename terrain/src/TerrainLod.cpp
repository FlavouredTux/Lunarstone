#include <lunar/terrain/TerrainLod.hpp>

#include <lunar/classes/World.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace lunar::terrain {

namespace {

using Int3 = std::array<int, 3>;

constexpr float studsPerBlock = TerrainMeshChunk::size * 4.0f;
constexpr int keyBias = 1 << 19; // node coordinates fit in 20 bits each

constexpr std::uint64_t keyOf(int level, Int3 p)
{
    auto field = [](int v) { return static_cast<std::uint64_t>(v + keyBias) & 0xfffff; };
    return (std::uint64_t{static_cast<unsigned>(level)} << 60) | (field(p[0]) << 40) | (field(p[1]) << 20)
           | field(p[2]);
}

constexpr int floorDiv(int value, int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

std::size_t meshBytes(const TerrainMeshChunk& chunk)
{
    std::size_t bytes = 0;
    for (const MeshData* mesh : {&chunk.solid, &chunk.water})
        bytes += mesh->vertices.size() * sizeof(MeshVertex) + mesh->indices.size() * sizeof(std::uint32_t);
    return bytes;
}

// A node once drawn as its children goes back to one mesh only this much
// farther out than it split, so standing at the line doesn't flip it every
// frame. GUESS: the client keeps a second distance table at 0.75 of the
// first, which reads like the same band.
constexpr float mergeFactor = 1.0f / 0.75f;

} // namespace

float lodDistance(int level)
{
    // The client builds this table at startup: pow(2.2, level) - 1, times
    // 200 / 1.2, kept squared times 1.2 (and a second one times 0.9).
    return (std::pow(2.2f, static_cast<float>(level)) - 1.0f) * 200.0f;
}

TerrainLod::TerrainLod(const Terrain& terrain, LodOptions options)
    : terrain_(terrain), options_(options), scale_(options.distanceScale)
{
    options_.levels = std::clamp(options_.levels, 1, 8);
    // The blocks that can hold surface: those with terrain and the air ones
    // just below them (a block owns the edges into its high neighbours).
    const int top = options_.levels - 1;
    for (const TerrainChunk& chunk : terrain.chunks()) {
        for (int axis = -1; axis < 3; ++axis) {
            Int3 block = chunk.position;
            if (axis >= 0)
                block[static_cast<std::size_t>(axis)] -= 1;
            for (int level = 0; level <= top; ++level) {
                const Int3 node{floorDiv(block[0], 1 << level), floorDiv(block[1], 1 << level),
                                floorDiv(block[2], 1 << level)};
                if (!nodes_.try_emplace(keyOf(level, node)).second)
                    break; // and so are its parents
                if (level == top)
                    roots_.push_back(keyOf(level, node));
            }
        }
    }

    unsigned threads = options_.threads;
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    for (unsigned i = 0; i < threads; ++i)
        workers_.emplace_back([this](const std::stop_token& stop) { work(stop); });
}

TerrainLod::~TerrainLod()
{
    for (std::jthread& worker : workers_)
        worker.request_stop();
    work_.notify_all();
}

void TerrainLod::work(const std::stop_token& stop)
{
    NodeMesher mesher(terrain_);
    for (;;) {
        std::uint64_t key = 0;
        {
            std::unique_lock lock(mutex_);
            if (!work_.wait(lock, stop, [&] { return !queue_.empty(); }))
                return;
            key = queue_.back().key;
            queue_.pop_back();
            inFlight_.insert(key);
        }
        auto field = [&](int shift) { return static_cast<int>((key >> shift) & 0xfffff) - keyBias; };
        const int level = static_cast<int>(key >> 60);
        auto mesh = std::make_shared<TerrainMeshChunk>(mesher.mesh(level, {field(40), field(20), field(0)}));
        {
            std::scoped_lock lock(mutex_);
            inFlight_.erase(key);
            done_.emplace_back(key, std::move(mesh));
        }
        arrivals_.notify_all();
    }
}

// Across the ground only, like the client: it measures from the camera
// moved to the height of the node's middle, so height never counts.
float TerrainLod::distanceTo(int level, Int3 position) const
{
    const float size = studsPerBlock * static_cast<float>(1 << level);
    const float eye[3] = {eye_.x, eye_.y, eye_.z};
    float sum = 0.0f;
    for (const std::size_t a : {0uz, 2uz}) {
        const float lo = static_cast<float>(position[a]) * size;
        const float d = std::max({lo - eye[a], 0.0f, eye[a] - (lo + size)});
        sum += d * d;
    }
    return std::sqrt(sum);
}

void TerrainLod::draw(std::uint64_t key, Node& node)
{
    node.lastUsed = frame_;
    drawn_.push_back(key);
    if (node.mesh) {
        chosen_.push_back(key);
        drawnBytes_ += node.bytes;
    }
}

void TerrainLod::want(std::uint64_t key, Node& node, float distance, int level)
{
    node.lastUsed = frame_;
    if (node.state == State::Idle || node.state == State::Queued) {
        // Nearest first; at the same distance the coarser node, which covers more.
        requests_.push_back({distance - static_cast<float>(level) * 1e-3f, key});
        settled_ = false;
    } else if (node.state == State::Arrived)
        settled_ = false;
}

// Draws the node, or its children where it wants more detail. True if what
// it drew covers all of the node's ground.
bool TerrainLod::select(std::uint64_t key, int level, Int3 position)
{
    Node& node = nodes_.at(key);
    const float distance = distanceTo(level, position);
    const float split = lodDistance(level) * scale_ * (node.split ? mergeFactor : 1.0f);
    if (level > 0 && distance < split) {
        const std::size_t mark = chosen_.size(), drawnMark = drawn_.size();
        const std::size_t bytesMark = drawnBytes_;
        bool covered = true;
        for (int i = 0; i < 8; ++i) {
            const Int3 child{position[0] * 2 + (i & 1), position[1] * 2 + ((i >> 1) & 1),
                             position[2] * 2 + ((i >> 2) & 1)};
            const std::uint64_t childKey = keyOf(level - 1, child);
            if (nodes_.contains(childKey))
                covered = select(childKey, level - 1, child) && covered;
        }
        if (covered) {
            node.split = true;
            return true;
        }
        // Children still coming: this node until they're all in.
        want(key, node, distance, level);
        if (node.state == State::Ready) {
            chosen_.resize(mark);
            drawn_.resize(drawnMark);
            drawnBytes_ = bytesMark;
            draw(key, node);
            return true;
        }
        return false;
    }
    node.split = false;
    want(key, node, distance, level);
    if (node.state == State::Ready) {
        draw(key, node);
        return true;
    }
    return level > 0 && drawReadyBelow(level, position);
}

// While a node isn't in yet: whatever finer meshes are, without asking for more.
bool TerrainLod::drawReadyBelow(int level, Int3 position)
{
    bool covered = true;
    for (int i = 0; i < 8; ++i) {
        const Int3 child{position[0] * 2 + (i & 1), position[1] * 2 + ((i >> 1) & 1), position[2] * 2 + ((i >> 2) & 1)};
        const std::uint64_t childKey = keyOf(level - 1, child);
        const auto it = nodes_.find(childKey);
        if (it == nodes_.end())
            continue;
        if (it->second.state == State::Ready)
            draw(childKey, it->second);
        else
            covered = level - 1 > 0 && drawReadyBelow(level - 1, child) && covered;
    }
    return covered;
}

void TerrainLod::takeArrivals()
{
    {
        std::scoped_lock lock(mutex_);
        for (auto& [key, mesh] : done_) {
            Node& node = nodes_.at(key);
            if (node.state == State::Ready || node.state == State::Arrived)
                continue; // meshed twice
            node.state = State::Arrived;
            node.bytes = meshBytes(*mesh);
            node.mesh = mesh->empty() ? nullptr : std::move(mesh);
            cachedBytes_ += node.bytes;
            arrived_.push_back(key);
        }
        done_.clear();
    }
    // Take in up to the budget's worth (always one), nearest first.
    std::ranges::sort(arrived_, {}, [&](std::uint64_t key) {
        const int level = static_cast<int>(key >> 60);
        auto field = [&](int shift) { return static_cast<int>((key >> shift) & 0xfffff) - keyBias; };
        return distanceTo(level, {field(40), field(20), field(0)});
    });
    std::size_t taken = 0, bytes = 0;
    while (taken < arrived_.size()
           && (taken == 0 || bytes + nodes_.at(arrived_[taken]).bytes <= options_.admitPerUpdate)) {
        Node& node = nodes_.at(arrived_[taken]);
        node.state = State::Ready;
        bytes += node.bytes;
        ++taken;
    }
    arrived_.erase(arrived_.begin(), arrived_.begin() + static_cast<std::ptrdiff_t>(taken));
}

void TerrainLod::requeue()
{
    std::ranges::sort(requests_, std::greater{}, &Request::priority);
    {
        std::scoped_lock lock(mutex_);
        for (const Request& old : queue_) {
            Node& node = nodes_.at(old.key);
            if (node.state == State::Queued)
                node.state = State::Idle;
        }
        queue_.clear();
        for (const Request& request : requests_) {
            Node& node = nodes_.at(request.key);
            if (inFlight_.contains(request.key) || node.state == State::Queued)
                continue;
            node.state = State::Queued;
            queue_.push_back(request);
        }
    }
    if (!requests_.empty())
        work_.notify_all();
}

void TerrainLod::evict()
{
    if (cachedBytes_ <= options_.cacheBudget)
        return;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> idle; // last used, key
    for (const auto& [key, node] : nodes_)
        if (node.state == State::Ready && node.lastUsed != frame_)
            idle.emplace_back(node.lastUsed, key);
    std::ranges::sort(idle);
    for (const auto& [lastUsed, key] : idle) {
        if (cachedBytes_ <= options_.cacheBudget)
            break;
        Node& node = nodes_.at(key);
        cachedBytes_ -= node.bytes;
        node = {};
    }
}

bool TerrainLod::update(Vector3 eye)
{
    ++frame_;
    eye_ = eye;
    takeArrivals();

    chosen_.clear();
    drawnBefore_.swap(drawn_);
    drawn_.clear();
    requests_.clear();
    drawnBytes_ = 0;
    settled_ = arrived_.empty();
    for (const std::uint64_t root : roots_) {
        auto field = [&](int shift) { return static_cast<int>((root >> shift) & 0xfffff) - keyBias; };
        select(root, options_.levels - 1, {field(40), field(20), field(0)});
    }
    requeue();
    evict();

    // Over the draw budget: pull the distances in a little each update until
    // it fits; well under it, let them back out.
    if (drawnBytes_ > options_.drawBudget)
        scale_ = std::max(0.2f, scale_ * 0.9f);
    else if (drawnBytes_ * 5 < options_.drawBudget * 3 && scale_ < options_.distanceScale)
        scale_ = std::min(options_.distanceScale, scale_ * 1.02f);

    std::ranges::sort(drawn_);
    const bool changed = drawn_ != drawnBefore_;
    if (changed)
        startFades();
    return makeSelection() || changed;
}

// What came in fades in over the next transitionFrames updates while what
// went fades out, in complementary dither patterns, so a level switch
// dissolves rather than pops.
void TerrainLod::startFades()
{
    for (const std::uint64_t key : chosen_) {
        if (std::ranges::binary_search(drawnBefore_, key))
            continue;
        Node& node = nodes_.at(key);
        node.fadeStart = frame_;
        // Back before it finished fading out: drawn whole again at once.
        if (std::erase_if(fadingOut_, [&](const Fade& fade) { return fade.mesh == node.mesh; }) > 0)
            node.fadeStart = 0;
    }
    for (const std::uint64_t key : drawnBefore_) {
        if (std::ranges::binary_search(drawn_, key))
            continue;
        const auto it = nodes_.find(key);
        if (it != nodes_.end() && it->second.mesh && options_.transitionFrames > 0)
            fadingOut_.push_back({it->second.mesh, frame_});
    }
}

float TerrainLod::fadeProgress(std::uint64_t start) const
{
    if (start == 0 || options_.transitionFrames <= 0)
        return 1.0f;
    return std::min(1.0f, static_cast<float>(frame_ - start + 1) / static_cast<float>(options_.transitionFrames));
}

// Fills selection_ from chosen_ and the fades; true while anything fades.
bool TerrainLod::makeSelection()
{
    bool fading = false;
    selection_.clear();
    for (const std::uint64_t key : chosen_) {
        Node& node = nodes_.at(key);
        const float progress = fadeProgress(node.fadeStart);
        if (progress >= 1.0f)
            node.fadeStart = 0;
        fading = fading || progress < 1.0f;
        selection_.push_back({node.mesh, 0.0f, progress});
    }
    std::erase_if(fadingOut_, [&](const Fade& fade) { return fadeProgress(fade.start) >= 1.0f; });
    for (const Fade& fade : fadingOut_) {
        selection_.push_back({fade.mesh, fadeProgress(fade.start), 1.0f});
        fading = true;
    }
    return fading;
}

void TerrainLod::settle(Vector3 eye)
{
    for (;;) {
        update(eye);
        if (settled_) {
            // Nobody watched it come in: no fades.
            fadingOut_.clear();
            for (const std::uint64_t key : chosen_)
                nodes_.at(key).fadeStart = 0;
            makeSelection();
            return;
        }
        std::unique_lock lock(mutex_);
        arrivals_.wait_for(lock, std::chrono::milliseconds(20), [&] { return !done_.empty(); });
    }
}

TerrainLod::Stats TerrainLod::stats() const
{
    Stats stats;
    for (const std::uint64_t key : chosen_) {
        const MeshPtr& mesh = nodes_.at(key).mesh;
        ++stats.nodes[std::min<std::size_t>(mesh->level, stats.nodes.size() - 1)];
        stats.triangles += (mesh->solid.indices.size() + mesh->water.indices.size()) / 3;
    }
    stats.drawnBytes = drawnBytes_;
    stats.cachedBytes = cachedBytes_;
    {
        std::scoped_lock lock(mutex_);
        stats.queued = queue_.size() + inFlight_.size();
    }
    stats.distanceScale = scale_;
    return stats;
}

} // namespace lunar::terrain
