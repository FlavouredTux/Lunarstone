#include "DecodedTextures.hpp"

#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/assets/ImageDecode.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/assets/Web.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <thread>
#include <utility>

namespace lunar::assets {

namespace {

// Decals nest at most a level or two; a cycle of indirections must end.
constexpr int maxIndirections = 3;

std::string firstDecalTexture(const Instance& instance)
{
    if (const auto* decal = dynamic_cast<const Decal*>(&instance); decal && !decal->texture().empty())
        return decal->texture();
    for (const auto& child : instance.children())
        if (std::string found = firstDecalTexture(*child); !found.empty())
            return found;
    return {};
}

// The text between <tag> and </tag>, trimmed.
std::string_view xmlElement(std::string_view xml, std::string_view tag)
{
    const std::string open = std::format("<{}>", tag);
    const std::string close = std::format("</{}>", tag);
    const std::size_t start = xml.find(open);
    if (start == std::string_view::npos)
        return {};
    const std::size_t from = start + open.size();
    const std::size_t end = xml.find(close, from);
    if (end == std::string_view::npos)
        return {};
    std::string_view value = xml.substr(from, end - from);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\n' || value.front() == '\r'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\n' || value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

} // namespace

std::string followImageIndirection(std::string_view bytes)
{
    if (!bytes.starts_with("<roblox"))
        return {};
    // TexturePacks (MaterialVariant and SurfaceAppearance PBR sets) name each
    // map by bare asset id.
    if (bytes.contains("<texturepack_version>")) {
        const std::string_view color = xmlElement(bytes, "color");
        if (color.empty() || color.find_first_not_of("0123456789") != std::string_view::npos)
            return {};
        return std::format("rbxassetid://{}", color);
    }
    // A Decal asset id: a model holding the Decal. try, since a broken
    // model should only fail this texture.
    try {
        auto model = loadModel(bytes);
        if (!model)
            return {};
        for (const auto& root : model->roots)
            if (std::string found = firstDecalTexture(*root); !found.empty())
                return found;
    } catch (const std::exception&) {
        return {};
    }
    return {};
}

struct TextureLoader::Entry {
    ContentId id; // meaningless when the content id didn't parse, but then it never loads
    TextureState state = TextureState::Unknown;
    std::shared_ptr<const Image> image;
    std::string error;
    std::vector<std::pair<std::string, Callback>> callbacks;
};

unsigned TextureLoader::defaultThreads()
{
    return std::max(4u, std::thread::hardware_concurrency() / 2);
}

TextureLoader::TextureLoader(std::shared_ptr<AssetCache> cache, unsigned threads, std::uint32_t maxDimension)
    : cache_(std::move(cache)), maxDimension_(maxDimension),
      derived_(std::make_unique<DerivedCache>(cache_->config().decodedDir, cache_->config().decodedCacheBytes)),
      decoded_(std::make_unique<DecodedTextures>(*derived_, std::max(maxDimension, 1u)))
{
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i)
        workers_.emplace_back([this](const std::stop_token& stop) { workerLoop(stop); });
}

TextureLoader::~TextureLoader()
{
    // Stop and join here, while the queue and maps the workers use still exist.
    // Every worker is told first and then woken under the lock, rather than
    // trusting the stop token's own wakeup: a teleport froze joining an idle
    // worker that never woke.
    for (std::jthread& worker : workers_)
        worker.request_stop();
    {
        const std::scoped_lock lock(mutex_);
        changed_.notify_all();
    }
    workers_.clear();
}

std::shared_ptr<TextureLoader::Entry> TextureLoader::entryFor(std::string_view contentId, bool start)
{
    std::unique_lock lock(mutex_);
    std::shared_ptr<Entry> entry;
    if (auto found = byId_.find(std::string(contentId)); found != byId_.end()) {
        entry = found->second;
    } else {
        auto id = parseContentId(contentId);
        if (!id) {
            entry = std::make_shared<Entry>();
            entry->state = TextureState::Failed;
            entry->error = id.error();
        } else {
            auto& shared = byKey_[id->key()];
            if (!shared) {
                shared = std::make_shared<Entry>();
                shared->id = std::move(*id);
            }
            entry = shared;
        }
        byId_.emplace(contentId, entry);
    }

    if (start && entry->state == TextureState::Unknown) {
        entry->state = TextureState::Loading;
        ++pending_;
        queue_.push_back(entry);
        lock.unlock();
        changed_.notify_all();
    }
    return entry;
}

void TextureLoader::request(std::string_view contentId)
{
    entryFor(contentId, true);
}

void TextureLoader::request(std::string_view contentId, Callback done)
{
    auto entry = entryFor(contentId, true);
    std::unique_lock lock(mutex_);
    if (entry->state == TextureState::Loading) {
        entry->callbacks.emplace_back(contentId, std::move(done));
        return;
    }
    auto image = entry->image;
    std::string error = entry->error;
    lock.unlock();
    done(contentId, image, error);
}

std::shared_ptr<const Image> TextureLoader::image(std::string_view contentId)
{
    auto entry = entryFor(contentId, true);
    const std::scoped_lock lock(mutex_);
    return entry->image;
}

TextureState TextureLoader::state(std::string_view contentId) const
{
    const std::scoped_lock lock(mutex_);
    auto found = byId_.find(std::string(contentId));
    return found == byId_.end() ? TextureState::Unknown : found->second->state;
}

std::string TextureLoader::error(std::string_view contentId) const
{
    const std::scoped_lock lock(mutex_);
    auto found = byId_.find(std::string(contentId));
    return found == byId_.end() ? std::string() : found->second->error;
}

std::expected<std::shared_ptr<const Image>, std::string> TextureLoader::load(std::string_view contentId)
{
    auto entry = entryFor(contentId, false);
    std::unique_lock lock(mutex_);
    if (entry->state == TextureState::Unknown) {
        entry->state = TextureState::Loading;
        ++pending_;
        lock.unlock();
        run(entry);
        lock.lock();
    }
    changed_.wait(lock, [&] { return entry->state != TextureState::Loading; });
    if (entry->state == TextureState::Failed)
        return std::unexpected(entry->error);
    return entry->image;
}

void TextureLoader::wait()
{
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return pending_ == 0; });
}

std::size_t TextureLoader::pending() const
{
    const std::scoped_lock lock(mutex_);
    return pending_;
}

std::uint64_t TextureLoader::generation() const
{
    const std::scoped_lock lock(mutex_);
    return generation_;
}

TextureProvider TextureLoader::provider()
{
    return [this](std::string_view contentId) { return image(contentId); };
}

void TextureLoader::workerLoop(const std::stop_token& stop)
{
    const HttpStopScope downloads(stop);
    while (true) {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait(lock, stop, [&] { return !queue_.empty(); }))
                return;
            entry = std::move(queue_.front());
            queue_.pop_front();
        }
        run(entry);
    }
}

void TextureLoader::run(const std::shared_ptr<Entry>& entry)
{
    // id is set before the entry can be Loading and never changes after.
    ContentId id = entry->id;
    const std::string key = id.key();
    try {
        for (int hop = 0;; ++hop) {
            auto bytes = cache_->fetch(id);
            if (!bytes) {
                finish(entry, nullptr, bytes.error());
                return;
            }
            if (std::string next = followImageIndirection(**bytes); !next.empty()) {
                auto nextId = parseContentId(next);
                if (!nextId || hop >= maxIndirections) {
                    finish(entry, nullptr,
                           std::format("{}: points at {}, which {}", key, next,
                                       nextId ? "goes on too long" : nextId.error()));
                    return;
                }
                id = std::move(*nextId);
                continue;
            }
            // Decoded before: the pixels come from the file, as they're read.
            if (std::optional<Image> kept = decoded_->find(**bytes)) {
                finish(entry, std::make_shared<const Image>(std::move(*kept)), {});
                return;
            }
            auto image = decodeImage(**bytes);
            if (!image) {
                finish(entry, nullptr, std::format("{}: {}", key, image.error()));
                return;
            }
            finish(entry,
                   std::make_shared<const Image>(
                       decoded_->keep(**bytes, limitImageSize(std::move(*image), maxDimension_))),
                   {});
            return;
        }
    } catch (const std::exception& e) {
        finish(entry, nullptr, std::format("{}: {}", key, e.what()));
    }
}

void TextureLoader::finish(const std::shared_ptr<Entry>& entry, std::shared_ptr<const Image> image, std::string error)
{
    std::vector<std::pair<std::string, Callback>> callbacks;
    {
        const std::scoped_lock lock(mutex_);
        entry->state = image ? TextureState::Ready : TextureState::Failed;
        entry->image = std::move(image);
        entry->error = std::move(error);
        callbacks = std::exchange(entry->callbacks, {});
    }
    changed_.notify_all();
    // The entry no longer changes, so it's safe to read without the lock.
    for (auto& [contentId, done] : callbacks)
        done(contentId, entry->image, entry->error);
    // Only now does wait() see it done, so its callbacks have all run.
    {
        const std::scoped_lock lock(mutex_);
        --pending_;
        ++generation_;
    }
    changed_.notify_all();
}

} // namespace lunar::assets
