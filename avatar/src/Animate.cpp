#include <lunar/avatar/Animate.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace lunar::avatar {

namespace {

using AnimationTable = std::map<std::string, std::vector<AnimationChoice>, std::less<>>;

std::string asset(std::uint64_t id)
{
    return std::format("http://www.roblox.com/asset/?id={}", id);
}

// humanoidAnimateR15_DEPRECATED_20260813.rbxm, animNames.
AnimationTable makeR15Defaults()
{
    auto one = [](std::uint64_t id) { return std::vector<AnimationChoice>{{asset(id), 10.0}}; };
    auto three = [](std::uint64_t a, std::uint64_t b, std::uint64_t c) {
        return std::vector<AnimationChoice>{{asset(a), 10.0}, {asset(b), 10.0}, {asset(c), 10.0}};
    };
    return {
        {"idle", {{asset(507766666), 1.0}, {asset(507766951), 1.0}, {asset(507766388), 9.0}}},
        {"walk", one(507777826)},
        {"run", one(507767714)},
        {"swim", one(507784897)},
        {"swimidle", one(507785072)},
        {"jump", one(507765000)},
        {"fall", one(507767968)},
        {"climb", one(507765644)},
        {"sit", one(2506281703)},
        {"toolnone", one(507768375)},
        {"toolslash", one(522635514)},
        {"toollunge", one(522638767)},
        {"wave", one(507770239)},
        {"point", one(507770453)},
        {"dance", three(507771019, 507771955, 507772104)},
        {"dance2", three(507776043, 507776720, 507776879)},
        {"dance3", three(507777268, 507777451, 507777623)},
        {"laugh", one(507770818)},
        {"cheer", one(507770677)},
    };
}

// humanoidAnimatePlayEmote.rbxm (the R6 script), animNames. Its "run" entry
// names a file ("run.xml") that no longer exists; the script's own children
// point run at the walk animation, which is what this uses.
AnimationTable makeR6Defaults()
{
    auto one = [](std::uint64_t id) { return std::vector<AnimationChoice>{{asset(id), 10.0}}; };
    auto three = [](std::uint64_t a, std::uint64_t b, std::uint64_t c) {
        return std::vector<AnimationChoice>{{asset(a), 10.0}, {asset(b), 10.0}, {asset(c), 10.0}};
    };
    return {
        {"idle", {{asset(180435571), 9.0}, {asset(180435792), 1.0}}},
        {"walk", one(180426354)},
        {"run", one(180426354)},
        {"jump", one(125750702)},
        {"fall", one(180436148)},
        {"climb", one(180436334)},
        {"sit", one(178130996)},
        {"toolnone", one(182393478)},
        {"toolslash", one(129967390)},
        {"toollunge", one(129967478)},
        {"wave", one(128777973)},
        {"point", one(128853357)},
        {"dance1", three(182435998, 182491037, 182491065)},
        {"dance2", three(182436842, 182491248, 182491277)},
        {"dance3", three(182436935, 182491368, 182491423)},
        {"laugh", one(129423131)},
        {"cheer", one(129423030)},
    };
}

// emoteNames: membership marks an emote, the value whether it loops.
std::optional<bool> emoteLoops(Enum::HumanoidRigType rig, std::string_view name)
{
    static const std::map<std::string, bool, std::less<>> r15{
        {"wave", false},  {"point", false}, {"dance", true},  {"dance2", true},
        {"dance3", true}, {"laugh", false}, {"cheer", false},
    };
    static const std::map<std::string, bool, std::less<>> r6{
        {"wave", false},  {"point", false}, {"dance1", true}, {"dance2", true},
        {"dance3", true}, {"laugh", false}, {"cheer", false},
    };
    const auto& table = rig == Enum::HumanoidRigType::R15 ? r15 : r6;
    if (auto found = table.find(name); found != table.end())
        return found->second;
    return std::nullopt;
}

bool isEmote(Enum::HumanoidRigType rig, std::string_view name)
{
    return emoteLoops(rig, name).has_value();
}

// The script's constants.
constexpr float emoteTransitionTime = 0.1f;   // EMOTE_TRANSITION_TIME
constexpr float humanoidHipHeight = 2.0f;     // HumanoidHipHeight (R15)
constexpr float toolTransitionTime = 0.1f;    // both scripts
constexpr float r15JumpAnimDuration = 0.31f;  // R15 jumpAnimDuration
constexpr float r15FallTransitionTime = 0.2f; // R15 fallTransitionTime
constexpr float r6JumpAnimDuration = 0.3f;    // R6 jumpAnimDuration
constexpr float r6FallTransitionTime = 0.3f;  // R6 fallTransitionTime
constexpr float smallButNotZero = 0.0001f;

} // namespace

const std::map<std::string, std::vector<AnimationChoice>, std::less<>>& defaultAnimations(Enum::HumanoidRigType rig)
{
    static const AnimationTable r15 = makeR15Defaults();
    static const AnimationTable r6 = makeR6Defaults();
    return rig == Enum::HumanoidRigType::R15 ? r15 : r6;
}

AnimateController::AnimateController(Model& character, Animator& animator, ClipLoader& clips, std::uint32_t seed)
    : character_(&character), animator_(&animator), clips_(&clips), random_(seed)
{
    if (Humanoid* h = humanoid())
        rig_ = h->rigType();
    reconfigure();
    // "initialize to idle"
    playAnimation("idle", 0.1f);
    pose_ = Pose::Standing;
}

AnimateController::~AnimateController()
{
    stopAllAnimations();
    stopToolAnimations();
}

Humanoid* AnimateController::humanoid() const
{
    return instanceCast<Humanoid>(character_->findFirstChildOfClass("Humanoid"));
}

void AnimateController::reconfigure()
{
    for (const auto& [name, fallback] : defaultAnimations(rig_))
        configureAnimationSet(name, fallback);
}

void AnimateController::configureAnimationSet(const std::string& name, const std::vector<AnimationChoice>& fallback)
{
    Set set;
    // "check for config values": the Animate object's StringValue of that name.
    if (Instance* script = character_->findFirstChild("Animate")) {
        if (Instance* config = script->findFirstChild(name)) {
            for (const InstancePtr& child : config->children()) {
                const auto* animation = instanceCast<const Animation>(child.get());
                if (!animation)
                    continue;
                double weight = 1.0;
                if (const auto* value = instanceCast<const NumberValue>(animation->findFirstChild("Weight")))
                    weight = value->value();
                set.choices.push_back({animation->animationId(), weight, animation->name(), {}});
                set.totalWeight += weight;
            }
        }
    }
    // "fallback to defaults"
    if (set.choices.empty()) {
        for (const AnimationChoice& choice : fallback) {
            set.choices.push_back({choice.animationId, choice.weight, name, {}});
            set.totalWeight += choice.weight;
        }
    }
    for (std::size_t i = 0; i < set.choices.size(); ++i) {
        set.choices[i].key = std::format("{}/{}/{}", name, i, set.choices[i].animationId);
        // "preload anims"
        clips_->clip(set.choices[i].animationId);
    }
    animTable_[name] = std::move(set);
}

TrackPtr AnimateController::load(const Choice& anim)
{
    TrackPtr track = animator_->loadAnimation(clips_->source(anim.animationId), anim.name);
    track->setAnimationId(anim.animationId);
    return track;
}

std::string AnimateController::stopAllAnimations()
{
    std::string oldAnim = currentAnim_;
    // "return to idle if finishing an emote"
    if (emoteLoops(rig_, oldAnim) == false)
        oldAnim = "idle";
    if (currentlyPlayingEmote_) {
        oldAnim = "idle";
        currentlyPlayingEmote_ = false;
    }
    currentAnim_.clear();
    currentAnimInstanceId_.clear();
    currentAnimKeyframeHandler_.disconnect();
    if (currentAnimTrack_) {
        currentAnimTrack_->stop();
        currentAnimTrack_->destroy();
        currentAnimTrack_ = nullptr;
    }
    // "clean up walk if there is one"
    runAnimKeyframeHandler_.disconnect();
    if (runAnimTrack_) {
        runAnimTrack_->stop();
        runAnimTrack_->destroy();
        runAnimTrack_ = nullptr;
    }
    return oldAnim;
}

float AnimateController::getHeightScale() const
{
    Humanoid* h = humanoid();
    if (!h)
        return character_->scaleFactor();
    // "When auto scaling is not enabled, the rig scale stands in for a computed scale."
    if (!h->automaticScalingEnabled())
        return character_->scaleFactor();
    float scale = h->hipHeight() / humanoidHipHeight;
    if (Instance* script = character_->findFirstChild("Animate")) {
        if (const auto* dampening = instanceCast<const NumberValue>(script->findFirstChild("ScaleDampeningPercent")))
            scale = 1.0f
                    + (h->hipHeight() - humanoidHipHeight) * static_cast<float>(dampening->value()) / humanoidHipHeight;
    }
    return scale;
}

float AnimateController::rootMotionCompensation(float speed) const
{
    const float speedScaled = speed * 1.25f;
    return speedScaled / getHeightScale();
}

void AnimateController::setRunSpeed(float speed)
{
    constexpr float normalizedWalkSpeed = 0.5f; // "established empirically using current `913402848` walk animation"
    constexpr float normalizedRunSpeed = 1.0f;
    const float runSpeed = rootMotionCompensation(speed);
    float walkAnimationWeight = smallButNotZero;
    float runAnimationWeight = smallButNotZero;
    float timeWarp = 1.0f;
    if (runSpeed <= normalizedWalkSpeed) {
        walkAnimationWeight = 1.0f;
        timeWarp = runSpeed / normalizedWalkSpeed;
    } else if (runSpeed < normalizedRunSpeed) {
        const float fadeInRun = (runSpeed - normalizedWalkSpeed) / (normalizedRunSpeed - normalizedWalkSpeed);
        walkAnimationWeight = 1.0f - fadeInRun;
        runAnimationWeight = fadeInRun;
    } else {
        timeWarp = runSpeed / normalizedRunSpeed;
        runAnimationWeight = 1.0f;
    }
    currentAnimTrack_->adjustWeight(walkAnimationWeight);
    runAnimTrack_->adjustWeight(runAnimationWeight);
    currentAnimTrack_->adjustSpeed(timeWarp);
    runAnimTrack_->adjustSpeed(timeWarp);
}

void AnimateController::setAnimationSpeed(float speed)
{
    if (rig_ == Enum::HumanoidRigType::R15 && currentAnim_ == "walk" && runAnimTrack_) {
        setRunSpeed(speed);
        return;
    }
    if (speed != currentAnimSpeed_) {
        currentAnimSpeed_ = speed;
        if (currentAnimTrack_)
            currentAnimTrack_->adjustSpeed(currentAnimSpeed_);
    }
}

void AnimateController::keyFrameReachedFunc(const std::string& frameName)
{
    if (frameName != "End")
        return;
    if (rig_ == Enum::HumanoidRigType::R15 && currentAnim_ == "walk") {
        if (runAnimTrack_ && !runAnimTrack_->looped())
            runAnimTrack_->setTimePosition(0.0f);
        if (currentAnimTrack_ && !currentAnimTrack_->looped())
            currentAnimTrack_->setTimePosition(0.0f);
        return;
    }
    std::string repeatAnim = currentAnim_;
    // "return to idle if finishing an emote"
    if (emoteLoops(rig_, repeatAnim) == false)
        repeatAnim = "idle";
    if (currentlyPlayingEmote_) {
        if (currentAnimTrack_ && currentAnimTrack_->looped())
            return; // "Allow the emote to loop"
        repeatAnim = "idle";
        currentlyPlayingEmote_ = false;
    }
    if (repeatAnim.empty())
        return;
    const float animSpeed = currentAnimSpeed_;
    // The R15 script blends back in over 0.15 s, the R6 one cuts.
    playAnimation(repeatAnim, rig_ == Enum::HumanoidRigType::R15 ? 0.15f : 0.0f);
    setAnimationSpeed(animSpeed);
}

std::size_t AnimateController::rollAnimation(const std::string& animName)
{
    const Set& set = animTable_.at(animName);
    // math.random(1, totalWeight): an integer roll; Luau truncates a
    // fractional upper bound.
    const auto top = std::max<long long>(1, static_cast<long long>(std::floor(set.totalWeight)));
    double roll = static_cast<double>(std::uniform_int_distribution<long long>(1, top)(random_));
    std::size_t idx = 0;
    while (idx + 1 < set.choices.size() && roll > set.choices[idx].weight) {
        roll -= set.choices[idx].weight;
        ++idx;
    }
    return idx;
}

void AnimateController::switchToAnim(const Choice& anim, const std::string& animName, float transitionTime)
{
    if (anim.key == currentAnimInstanceId_)
        return;
    if (currentAnimTrack_) {
        currentAnimTrack_->stop(transitionTime);
        currentAnimTrack_->destroy();
    }
    if (runAnimTrack_) {
        runAnimTrack_->stop(transitionTime);
        runAnimTrack_->destroy();
        runAnimTrack_ = nullptr;
    }
    currentAnimSpeed_ = 1.0f;
    // "load it to the humanoid; get AnimationTrack"
    currentAnimTrack_ = load(anim);
    currentAnimTrack_->setPriority(Enum::AnimationPriority::Core);
    currentAnimTrack_->play(transitionTime);
    currentAnim_ = animName;
    currentAnimInstanceId_ = anim.key;

    // "set up keyframe name triggers"
    currentAnimKeyframeHandler_.disconnect();
    currentAnimKeyframeHandler_
        = currentAnimTrack_->keyframeReached.connect([this](const std::string& name) { keyFrameReachedFunc(name); });

    // "check to see if we need to blend a walk/run animation" (R15 only)
    if (rig_ == Enum::HumanoidRigType::R15 && animName == "walk" && animTable_.contains("run")) {
        const std::size_t runIdx = rollAnimation("run");
        runAnimTrack_ = load(animTable_.at("run").choices[runIdx]);
        runAnimTrack_->setPriority(Enum::AnimationPriority::Core);
        runAnimTrack_->play(transitionTime);
        runAnimKeyframeHandler_.disconnect();
        runAnimKeyframeHandler_
            = runAnimTrack_->keyframeReached.connect([this](const std::string& name) { keyFrameReachedFunc(name); });
    }
}

void AnimateController::playAnimation(const std::string& animName, float transitionTime)
{
    auto found = animTable_.find(animName);
    if (found == animTable_.end() || found->second.choices.empty())
        return;
    const std::size_t idx = rollAnimation(animName);
    const Choice anim = found->second.choices[idx];
    switchToAnim(anim, animName, transitionTime);
    currentlyPlayingEmote_ = false;
}

void AnimateController::playToolAnimation(const std::string& animName, float transitionTime,
                                          std::optional<Enum::AnimationPriority> priority)
{
    auto found = animTable_.find(animName);
    if (found == animTable_.end() || found->second.choices.empty())
        return;
    const Choice anim = found->second.choices[rollAnimation(animName)];
    if (anim.key == toolAnimInstanceId_)
        return;
    if (toolAnimTrack_) {
        toolAnimTrack_->stop();
        toolAnimTrack_->destroy();
        transitionTime = 0.0f;
    }
    toolAnimTrack_ = load(anim);
    if (priority)
        toolAnimTrack_->setPriority(*priority);
    toolAnimTrack_->play(transitionTime);
    toolAnimName_ = animName;
    toolAnimInstanceId_ = anim.key;
    currentToolAnimKeyframeHandler_ = toolAnimTrack_->keyframeReached.connect([this](const std::string& frameName) {
        if (frameName == "End")
            playToolAnimation(toolAnimName_, 0.0f, std::nullopt);
    });
}

void AnimateController::stopToolAnimations()
{
    currentToolAnimKeyframeHandler_.disconnect();
    toolAnimName_.clear();
    toolAnimInstanceId_.clear();
    if (toolAnimTrack_) {
        toolAnimTrack_->stop();
        toolAnimTrack_->destroy();
        toolAnimTrack_ = nullptr;
    }
}

void AnimateController::animateTool()
{
    if (toolAnim_ == "None")
        playToolAnimation("toolnone", toolTransitionTime, Enum::AnimationPriority::Idle);
    else if (toolAnim_ == "Slash")
        playToolAnimation("toolslash", 0.0f, Enum::AnimationPriority::Action);
    else if (toolAnim_ == "Lunge")
        playToolAnimation("toollunge", 0.0f, Enum::AnimationPriority::Action);
}

// --- State change handlers -----------------------------------------------------------

void AnimateController::onRunning(float speed)
{
    if (rig_ != Enum::HumanoidRigType::R15) {
        speed /= character_->scaleFactor();
        if (speed > 0.01f) {
            playAnimation("walk", 0.1f);
            // Only the stock walk is scaled; a custom one plays at its own speed.
            if (currentAnimInstanceId_.ends_with("/" + asset(180426354)))
                setAnimationSpeed(speed / 14.5f);
            pose_ = Pose::Running;
        } else if (!isEmote(rig_, currentAnim_)) {
            playAnimation("idle", 0.1f);
            pose_ = Pose::Standing;
        }
        return;
    }

    const float heightScale = getHeightScale();
    // The ControllerManager branch of the script (ground-relative speed when
    // EvaluateStateMachine is off) isn't here: our controller reports the
    // speed relative to the floor already.
    Humanoid* h = humanoid();
    const bool movedDuringEmote = currentlyPlayingEmote_ && h && h->moveDirection() == Vector3{};
    const float speedThreshold = movedDuringEmote && h ? h->walkSpeed() / heightScale : 0.75f;
    if (speed > speedThreshold * heightScale) {
        constexpr float scale = 16.0f;
        playAnimation("walk", 0.2f);
        setAnimationSpeed(speed / scale);
        pose_ = Pose::Running;
    } else if (!isEmote(rig_, currentAnim_) && !currentlyPlayingEmote_) {
        playAnimation("idle", 0.2f);
        pose_ = Pose::Standing;
    }
}

void AnimateController::onDied()
{
    pose_ = Pose::Dead;
}

void AnimateController::onJumping()
{
    playAnimation("jump", 0.1f);
    jumpAnimTime_ = rig_ == Enum::HumanoidRigType::R15 ? r15JumpAnimDuration : r6JumpAnimDuration;
    pose_ = Pose::Jumping;
}

void AnimateController::onClimbing(float speed)
{
    if (rig_ == Enum::HumanoidRigType::R15) {
        speed /= getHeightScale();
        playAnimation("climb", 0.1f);
        setAnimationSpeed(speed / 5.0f);
    } else {
        speed /= character_->scaleFactor();
        playAnimation("climb", 0.1f);
        setAnimationSpeed(speed / 12.0f);
    }
    pose_ = Pose::Climbing;
}

void AnimateController::onGettingUp()
{
    pose_ = Pose::GettingUp;
}

void AnimateController::onFreeFall()
{
    if (jumpAnimTime_ <= 0.0)
        playAnimation("fall", rig_ == Enum::HumanoidRigType::R15 ? r15FallTransitionTime : r6FallTransitionTime);
    pose_ = Pose::FreeFall;
}

void AnimateController::onFallingDown()
{
    pose_ = Pose::FallingDown;
}

void AnimateController::onSeated()
{
    pose_ = Pose::Seated;
}

void AnimateController::onPlatformStanding()
{
    pose_ = Pose::PlatformStanding;
}

void AnimateController::onSwimming(float speed)
{
    if (rig_ != Enum::HumanoidRigType::R15) {
        pose_ = speed > 0.0f ? Pose::Running : Pose::Standing;
        return;
    }
    speed /= getHeightScale();
    if (speed > 1.0f) {
        constexpr float scale = 10.0f;
        playAnimation("swim", 0.4f);
        setAnimationSpeed(speed / scale);
        pose_ = Pose::Swimming;
    } else {
        playAnimation("swimidle", 0.4f);
        pose_ = Pose::Standing;
    }
}

void AnimateController::onStateChanged(HumanoidState state, float speed)
{
    switch (state) {
    case HumanoidState::Running:
    case HumanoidState::RunningNoPhysics:
        onRunning(speed);
        break;
    case HumanoidState::Dead:
        onDied();
        break;
    case HumanoidState::Jumping:
        onJumping();
        break;
    case HumanoidState::Climbing:
        onClimbing(speed);
        break;
    case HumanoidState::GettingUp:
        onGettingUp();
        break;
    case HumanoidState::Freefall:
        onFreeFall();
        break;
    case HumanoidState::FallingDown:
        onFallingDown();
        break;
    case HumanoidState::Seated:
        onSeated();
        break;
    case HumanoidState::PlatformStanding:
        onPlatformStanding();
        break;
    case HumanoidState::Swimming:
        onSwimming(speed);
        break;
    default:
        break;
    }
}

bool AnimateController::playEmote(std::string_view emote)
{
    // "Only play emotes when idling"
    if (pose_ != Pose::Standing || !isEmote(rig_, emote))
        return false;
    playAnimation(std::string(emote), emoteTransitionTime);
    return true;
}

void AnimateController::stepAnimate(double currentTime)
{
    const double deltaTime = currentTime - lastTick_;
    lastTick_ = currentTime;
    if (jumpAnimTime_ > 0.0)
        jumpAnimTime_ -= deltaTime;

    const bool r15 = rig_ == Enum::HumanoidRigType::R15;
    if (pose_ == Pose::FreeFall && jumpAnimTime_ <= 0.0) {
        playAnimation("fall", r15 ? r15FallTransitionTime : r6FallTransitionTime);
    } else if (pose_ == Pose::Seated) {
        playAnimation("sit", 0.5f);
        return;
    } else if (pose_ == Pose::Running) {
        playAnimation("walk", r15 ? 0.2f : 0.1f);
    } else if (pose_ == Pose::Dead || pose_ == Pose::GettingUp || pose_ == Pose::FallingDown
               || pose_ == Pose::PlatformStanding) {
        // The R6 script also swings the limb motors' DesiredAngle here
        // (amplitude 0.1); legacy Motor angles aren't simulated, so it's left out.
        stopAllAnimations();
    }

    // "Tool Animation handling"
    auto* tool = instanceCast<Tool>(character_->findFirstChildOfClass("Tool"));
    if (tool && tool->findFirstChild("Handle")) {
        Instance* animStringValueObject = nullptr;
        for (const InstancePtr& child : tool->children())
            if (child->name() == "toolanim" && child->className() == "StringValue") {
                animStringValueObject = child.get();
                break;
            }
        if (animStringValueObject) {
            toolAnim_ = static_cast<StringValue*>(animStringValueObject)->value();
            // "message recieved, delete StringValue"
            animStringValueObject->setParent(nullptr);
            toolAnimTime_ = currentTime + 0.3;
        }
        if (currentTime > toolAnimTime_) {
            toolAnimTime_ = 0.0;
            toolAnim_ = "None";
        }
        animateTool();
    } else {
        stopToolAnimations();
        toolAnim_ = "None";
        toolAnimTime_ = 0.0;
    }
}

void AnimateController::update(float dt)
{
    clock_ += static_cast<double>(dt);
    // `while Character.Parent ~= nil do wait(0.1) stepAnimate(time) end`
    while (clock_ >= nextStep_) {
        stepAnimate(nextStep_);
        nextStep_ += 0.1;
    }
}

} // namespace lunar::avatar
