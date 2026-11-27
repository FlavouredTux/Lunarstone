#pragma once

#include <lunar/Classes.hpp>
#include <lunar/avatar/Clothing.hpp>
#include <lunar/physics/PlayerCamera.hpp>
#include <lunar/physics/World.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
namespace physics = lunar::physics;

// What a live game's frame needs beside the scripts: the classic clothing
// on its characters, and the CameraModule over its replica. --net-play and
// the benchmark (--net-replay --bench-replay) build both the same way.

// Classic clothing (Shirt, Pants, ShirtGraphic) on the characters of a live
// game, composited as --render does but without stalling a frame on a
// download: a character is dressed once its templates are in, and again
// when its clothes or limbs arrive later (each a short while after the
// last change, so a character that streams in part by part is dressed once).
// The drawing happens on a thread of the wardrobe's own: a frame only reads
// what a character wears and, a frame or two later, puts the result on.
class Wardrobe {
public:
    explicit Wardrobe(lunar::Workspace& workspace)
        : clothing_(std::make_shared<lunar::assets::AssetCache>()), workspace_(workspace)
    {
        for (const lunar::InstancePtr& d : workspace.descendants())
            consider(*d);
        added_ = workspace.descendantAdded.connect([this](lunar::Instance* instance) { consider(*instance); });
    }
    ~Wardrobe()
    {
        added_.disconnect();
        // Told under the lock, so the thread can't miss it; a dressing it's
        // drawing finishes first.
        drawer_.request_stop();
        {
            const std::scoped_lock lock(mutex_);
            queued_.notify_all();
        }
        if (drawer_.joinable())
            drawer_.join();
    }
    Wardrobe(const Wardrobe&) = delete;
    Wardrobe& operator=(const Wardrobe&) = delete;

    lunar::avatar::ClothingCompositor& clothing() { return clothing_; }

    // Puts on the dressings drawn since the last call, in the order they
    // were asked for (`dressed` hears of each so its parts are drawn again
    // with the new textures), then asks for the characters that are ready,
    // for a few milliseconds a frame (a server full of them arrives at once).
    void update(const std::function<void(lunar::Model&)>& dressed)
    {
        std::vector<Job> done;
        {
            const std::scoped_lock lock(mutex_);
            done.swap(done_);
        }
        for (Job& job : done) {
            auto model = std::dynamic_pointer_cast<lunar::Model>(job.model.lock());
            if (model && model->isDescendantOf(&workspace_) && clothing_.finish(*model, job.dressing)) {
                ++dressed_;
                dressed(*model);
            }
        }
        const auto now = Clock::now();
        for (auto it = waiting_.begin(); it != waiting_.end() && Clock::now() - now < std::chrono::milliseconds(4);) {
            auto model = std::dynamic_pointer_cast<lunar::Model>(it->second.model.lock());
            if (!model || !model->isDescendantOf(&workspace_) || !model->findFirstChildOfClass("Humanoid")) {
                it = waiting_.erase(it);
                continue;
            }
            if (now - it->second.touched < std::chrono::milliseconds(250) || !clothing_.ready(*model)) {
                ++it;
                continue;
            }
            if (auto dressing = clothing_.prepare(*model)) {
                {
                    const std::scoped_lock lock(mutex_);
                    queue_.push_back({model, std::move(*dressing)});
                }
                if (!drawer_.joinable())
                    drawer_ = std::jthread([this](const std::stop_token& stop) { draw(stop); });
                else
                    queued_.notify_one();
            }
            it = waiting_.erase(it);
        }
    }
    int dressedCount() const { return dressed_; }

private:
    struct Waiting {
        std::weak_ptr<lunar::Instance> model;
        Clock::time_point touched;
    };
    struct Job {
        std::weak_ptr<lunar::Instance> model;
        lunar::avatar::ClothingCompositor::Dressing dressing;
    };

    // A Humanoid, a piece of clothing or a limb arrived: its model is a
    // character to dress (again).
    void consider(lunar::Instance& instance)
    {
        if (!instance.isA("Humanoid") && !instance.isA("Clothing") && !instance.isA("ShirtGraphic")
            && !instance.isA("MeshPart"))
            return;
        auto* model = dynamic_cast<lunar::Model*>(instance.parent());
        if (!model || (instance.isA("MeshPart") && !model->findFirstChildOfClass("Humanoid")))
            return;
        waiting_[model] = {model->weak_from_this(), Clock::now()};
    }

    // The drawer's loop: one dressing at a time, in order.
    void draw(const std::stop_token& stop)
    {
        std::unique_lock lock(mutex_);
        while (queued_.wait(lock, stop, [&] { return !queue_.empty(); })) {
            Job job = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            clothing_.composite(job.dressing);
            lock.lock();
            done_.push_back(std::move(job));
        }
    }

    lunar::avatar::ClothingCompositor clothing_;
    lunar::Workspace& workspace_;
    std::unordered_map<const lunar::Instance*, Waiting> waiting_;
    lunar::Connection added_;
    int dressed_ = 0;
    std::mutex mutex_;
    std::condition_variable_any queued_;
    std::deque<Job> queue_; // asked for, not drawn yet
    std::vector<Job> done_; // drawn, not put on yet
    std::jthread drawer_;   // last: stopped and joined first
};

// The CameraModule for a live game: Lunarstone's PlayerCamera following
// Camera.CameraSubject, which becomes our Humanoid whenever our character
// (re)spawns, with Poppercam's raycasts answered by a physics world kept
// over the replica's Workspace (never stepped: nothing simulates here). A
// Scriptable camera is the game's own.
class LiveCamera {
public:
    explicit LiveCamera(lunar::Workspace& workspace) : workspace_(workspace), world_(workspace) {}

    lunar::Camera& camera() { return *workspace_.ensureCurrentCamera(); }
    bool scripted() { return camera().cameraType() == lunar::Enum::CameraType::Scriptable; }

    void update(float dt, const physics::PlayerCamera::Input& in, lunar::Player* local)
    {
        lunar::Camera& current = camera();
        if (&current != camera_) {
            camera_ = &current;
            player_ = std::make_unique<physics::PlayerCamera>(world_, current);
            subject_.reset();
        }
        player_->setMouseLockAllowed(mouseLockAllowed_);
        lunar::Model* character = local ? local->character() : nullptr;
        auto* humanoid
            = character ? dynamic_cast<lunar::Humanoid*>(character->findFirstChildOfClass("Humanoid")) : nullptr;
        if (humanoid && subject_.lock().get() != humanoid) {
            player_->setSubject(humanoid);
            subject_ = humanoid->weak_from_this();
        }
        input_ = in;
        if (scripted() || !current.cameraSubject())
            return;
        world_.refresh();
        if (local)
            player_->setZoomLimits(*local);
        player_->update(dt, in);
    }

    // The menu's Shift Lock Switch.
    void setMouseLockAllowed(bool allowed) { mouseLockAllowed_ = allowed; }

    // Where our character faces: the camera's look while rotation is
    // camera-relative, as UserGameSettings.RotationType says (set by the
    // scripts: a game's own Shift Lock defined in Luau) or our CameraModule's
    // own mouse lock and first person would set it; else none.
    std::optional<lunar::Vector3> facing(bool scriptsCameraRelative)
    {
        const bool ours = player_ && !scripted() && player_->cameraRelativeRotation();
        if (!ours && !scriptsCameraRelative)
            return std::nullopt;
        return camera().cframe().lookVector();
    }
    // Why facing() gives a direction, for the log; empty when it gives none.
    std::string facingReason(bool scriptsCameraRelative)
    {
        if (player_ && !scripted() && player_->mouseLocked())
            return "shift lock";
        if (player_ && !scripted() && player_->firstPerson())
            return "first person";
        return scriptsCameraRelative ? "UserGameSettings.RotationType" : "";
    }

    // The mouse is captured when the camera wants it (right button held,
    // first person) or a script set UserInputService.MouseBehavior to lock it.
    bool wantsPointerLock(lunar::DataModel& game)
    {
        const auto* uis = dynamic_cast<const lunar::UserInputService*>(game.findService("UserInputService"));
        if (uis && uis->mouseBehavior() != lunar::Enum::MouseBehavior::Default)
            return true;
        return player_ && !scripted() && player_->pointerLocked(input_);
    }

private:
    lunar::Workspace& workspace_;
    physics::World world_;
    lunar::Camera* camera_ = nullptr;
    std::unique_ptr<physics::PlayerCamera> player_;
    std::weak_ptr<lunar::Instance> subject_;
    physics::PlayerCamera::Input input_;
    bool mouseLockAllowed_ = false;
};
