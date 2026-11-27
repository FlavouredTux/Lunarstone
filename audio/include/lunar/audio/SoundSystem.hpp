#pragma once

#include <lunar/Classes.hpp>
#include <lunar/audio/Mixer.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lunar::assets {
class AssetCache;
}

namespace lunar::audio {

// The game's Sounds, heard: every Sound in the game gets a voice in the
// mixer once its SoundId has loaded (fetched and decoded on loader threads,
// what's wanted to play now first), playing while Playing is set, from TimePosition, at its Volume
// times its SoundGroup's. Sounds in a part or attachment are heard from
// there, fading with distance by their RollOffMode and panned left or right
// of the listener; others play as they are. Ended, DidLoop and Loaded fire
// for scripts, and TimePosition, IsLoaded and TimeLength follow the audio.
class SoundSystem {
public:
    SoundSystem(DataModel& game, std::shared_ptr<assets::AssetCache> cache, bool output = true);
    ~SoundSystem();
    SoundSystem(const SoundSystem&) = delete;
    SoundSystem& operator=(const SoundSystem&) = delete;

    // Once a frame, on the game's thread: listener is where sounds are
    // heard from (the camera).
    void update(const CFrame& listener);

    // Everything quieter by this (0 to 1; the settings' master volume).
    void setMasterVolume(float volume) { master_ = volume; }

    Mixer& mixer() { return mixer_; }
    struct Stats {
        std::size_t sounds = 0;  // followed
        std::size_t playing = 0; // heard now
        std::size_t clips = 0;   // loaded
        std::size_t bytes = 0;   // their samples' memory
        std::size_t failed = 0;  // that couldn't be
    };
    Stats stats() const;

    // The gain at `distance` studs for a roll-off mode between min and max (Roblox's curves).
    static float rollOff(Enum::RollOffMode mode, float distance, float minDistance, float maxDistance);

private:
    struct Tracked {
        std::weak_ptr<Sound> sound;
        Mixer::Voice voice = 0;
        std::string soundId;               // what the voice's clip is for
        bool playing = false;              // as last seen
        bool asked = false;                // Playing, as last seen (LUNAR_SOUND_TRACE)
        double askedAt = 0.0;
        double written = 0.0;              // the TimePosition we last set
        std::uint64_t ended = 0;
        std::uint64_t loops = 0;
        std::uint64_t seek = 0; // the number of the last seek we asked of the voice
        bool idle = false;      // stopped, and its voice told so: nothing to do until it plays
    };
    void track(Sound& sound);
    // Sounds that DescendantRemoving told of and that didn't come straight
    // back (a move within the game): gone, their voices with them, and
    // PlayOnRemove's played.
    struct Leaving {
        const Instance* address = nullptr;
        std::weak_ptr<Sound> sound;
        std::shared_ptr<const Clip> onRemove; // PlayOnRemove's, as it was when it left
        float volume = 0.0f;
        float speed = 1.0f;
    };
    void settle();
    static bool sameOwner(const std::weak_ptr<Sound>& a, const std::weak_ptr<Sound>& b);
    // An id's length once it has loaded (it's asked for until then), and
    // its samples when `samples` (they're decoded if they aren't): null
    // until both are there.
    struct Found {
        std::optional<double> seconds;
        std::shared_ptr<const Clip> clip;
    };
    Found find(const std::string& soundId, bool samples);
    void load(std::stop_token stop, bool urgentOnly);
    bool audible(const Sound& sound) const;

    DataModel& game_;
    std::shared_ptr<assets::AssetCache> cache_;
    Mixer mixer_;
    float master_ = 1.0f;
    std::unordered_map<const Instance*, Tracked> tracked_;
    std::vector<Leaving> leaving_;
    std::vector<Connection> connections_;
    // Sounds being removed while PlayOnRemove: played out once.
    std::vector<std::pair<Mixer::Voice, std::uint64_t>> oneShots_;

    // A loaded sound: its length, and its samples for as long as they're
    // kept. Long ones keep only their file between plays (decoded they're
    // tens of megabytes each), decoded again when they play.
    struct Loaded {
        bool done = false;
        bool failed = false;
        double seconds = 0.0;
        std::shared_ptr<const Clip> clip;
        std::shared_ptr<const std::string> file; // a long one's, for decoding it again
        bool decoding = false;
        bool queued = false; // waiting in urgent_ or queue_ for a loader
        bool urgent = false; // a sound wants to play it now
        int retries = 0;     // fetches Roblox answered "later" (429, 5xx)
        double lastUsed = 0.0; // when it last played, in update()s' clock
    };
    void enqueue(const std::string& soundId, Loaded& entry, bool urgent);
    // Decoded samples bigger than this aren't kept between plays.
    static constexpr std::size_t keepBytes = std::size_t{2} << 20;
    static constexpr double keepSeconds = 30.0; // how long after its last play a long one keeps its samples
    // Loaders at once: a burst of a game's sounds (a fight, a join) mostly
    // waits on the network, not on decoding. The last few only load what's
    // wanted now, so that never waits for a loader behind the rest.
    static constexpr int loaderCount = 6;
    static constexpr int urgentOnlyLoaders = 2;
    // How often a fetch that Roblox answered "later" is asked again, and
    // the longest wait between asks (1 s, doubling up to this).
    static constexpr int maxRetries = 6;
    static constexpr int maxRetryWait = 16;
    double now_ = 0.0;
    mutable std::mutex loadMutex_;
    std::condition_variable_any wake_;
    std::unordered_map<std::string, Loaded> clips_;
    // Waiting to load: first what a sound wants to play now, then the rest
    // in the order they were met; rate-limited ones until they may be asked again.
    std::deque<std::string> urgent_;
    std::deque<std::string> queue_;
    std::multimap<std::chrono::steady_clock::time_point, std::string> later_;
    std::atomic<std::size_t> failures_ = 0;
    std::vector<std::jthread> loaders_;
};

} // namespace lunar::audio
