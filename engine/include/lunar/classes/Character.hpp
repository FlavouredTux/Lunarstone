#pragma once

// Humanoids and what characters are dressed and animated with, plus seats and
// tools. Included through <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/classes/World.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace lunar {

// Humanoid's events, fired by the character controller in the order Roblox
// fires them. A cloned Humanoid starts with no connections.
struct HumanoidEvents {
    HumanoidEvents() = default;
    HumanoidEvents(const HumanoidEvents&) {}
    HumanoidEvents& operator=(const HumanoidEvents&) { return *this; }
    ~HumanoidEvents() = default;

    Signal<Enum::HumanoidStateType /*old*/, Enum::HumanoidStateType /*new*/> stateChanged;
    Signal<bool /*enabled*/, Enum::HumanoidStateType> stateEnabledChanged;
    Signal<float /*speed*/> running;
    Signal<float /*speed*/> climbing;
    Signal<float /*speed*/> swimming;
    Signal<bool /*active*/> jumping;
    Signal<bool /*active*/> freeFalling;
    Signal<bool /*active*/> fallingDown;
    Signal<bool /*active*/> gettingUp;
    Signal<bool /*active*/> ragdoll;
    Signal<bool /*active*/> platformStanding;
    Signal<bool /*active*/> strafing;
    Signal<bool /*active*/, BasePart* /*seat*/> seated;
    Signal<> died;
    Signal<float /*health*/> healthChanged;
    Signal<bool /*reached*/> moveToFinished;
};

// What runs a Humanoid's state machine and walks it (the physics character
// controller). Scripts' ChangeState, Move and MoveTo reach it through
// Humanoid::driver(); a Humanoid nothing simulates has none.
class HumanoidDriver {
public:
    virtual void changeState(Enum::HumanoidStateType state) = 0;
    virtual void move(Vector3 direction, bool relativeToCamera) = 0;
    virtual void moveTo(Vector3 point, BasePart* part) = 0;

protected:
    HumanoidDriver() = default;
    HumanoidDriver(const HumanoidDriver&) = default;
    HumanoidDriver& operator=(const HumanoidDriver&) = default;
    ~HumanoidDriver() = default;
};

class Humanoid final : public InstanceImpl<Humanoid, Instance> {
public:
    LUNAR_FIELD(float, health, Health, 100.0f)
    LUNAR_FIELD(float, maxHealth, MaxHealth, 100.0f)
    LUNAR_FIELD(float, walkSpeed, WalkSpeed, 16.0f)
    LUNAR_FIELD(float, jumpPower, JumpPower, 50.0f)
    LUNAR_FIELD(float, jumpHeight, JumpHeight, 7.2f)
    LUNAR_FIELD(bool, useJumpPower, UseJumpPower, true)
    LUNAR_FIELD(float, hipHeight, HipHeight, 0.0f)
    LUNAR_FIELD(float, maxSlopeAngle, MaxSlopeAngle, 89.0f)
    LUNAR_FIELD(Enum::HumanoidRigType, rigType, RigType, Enum::HumanoidRigType::R6)
    LUNAR_FIELD(std::string, displayName, DisplayName)
    LUNAR_FIELD(bool, autoJumpEnabled, AutoJumpEnabled, true)
    LUNAR_FIELD(bool, autoRotate, AutoRotate, true)
    LUNAR_FIELD(bool, automaticScalingEnabled, AutomaticScalingEnabled, true)
    LUNAR_FIELD(bool, breakJointsOnDeath, BreakJointsOnDeath, true)
    LUNAR_FIELD(bool, requiresNeck, RequiresNeck, true)
    LUNAR_FIELD(bool, evaluateStateMachine, EvaluateStateMachine, true)
    LUNAR_FIELD(Enum::HumanoidCollisionType, collisionType, CollisionType, Enum::HumanoidCollisionType::OuterBox)
    LUNAR_FIELD(Enum::HumanoidDisplayDistanceType, displayDistanceType, DisplayDistanceType,
                Enum::HumanoidDisplayDistanceType::Viewer)
    LUNAR_FIELD(Enum::HumanoidHealthDisplayType, healthDisplayType, HealthDisplayType,
                Enum::HumanoidHealthDisplayType::DisplayWhenDamaged)
    LUNAR_FIELD(float, healthDisplayDistance, HealthDisplayDistance, 100.0f)
    LUNAR_FIELD(float, nameDisplayDistance, NameDisplayDistance, 100.0f)
    LUNAR_FIELD(Enum::NameOcclusion, nameOcclusion, NameOcclusion, Enum::NameOcclusion::OccludeAll)

    // Runtime state, never saved.
    LUNAR_FIELD(bool, jump, Jump, false)
    LUNAR_FIELD(bool, sit, Sit, false)
    LUNAR_FIELD(bool, platformStand, PlatformStand, false)
    LUNAR_FIELD(Vector3, cameraOffset, CameraOffset)
    LUNAR_FIELD(Vector3, walkToPoint, WalkToPoint)
    LUNAR_REF(BasePart, walkToPart, WalkToPart)
    LUNAR_FIELD(Vector3, targetPoint, TargetPoint)
    // Set by the engine's character controller; read-only for scripts.
    LUNAR_PROPERTY() Vector3 moveDirection() const { return moveDirection_; }
    LUNAR_PROPERTY() Enum::Material floorMaterial() const { return floorMaterial_; }
    LUNAR_PROPERTY() BasePart* seatPart() const { return seatPart_.get(); }
    void updateMovement(Vector3 moveDirection, Enum::Material floorMaterial)
    {
        setProperty(moveDirection_, moveDirection, "MoveDirection");
        setProperty(floorMaterial_, floorMaterial, "FloorMaterial");
    }
    void assignSeatPart(BasePart* seat) { setRef(seatPart_, seat, "SeatPart"); }

    // The character's HumanoidRootPart: a sibling part by that name.
    LUNAR_PROPERTY() BasePart* rootPart() const;

    // Humanoid:GetState(); the character controller moves it.
    Enum::HumanoidStateType getState() const { return state_; }
    void assignState(Enum::HumanoidStateType state) { state_ = state; }

    HumanoidEvents events;

    // Whoever simulates this Humanoid, or null. The driver sets and clears it.
    HumanoidDriver* driver() const { return runtime_.driver; }
    void setDriver(HumanoidDriver* driver) { runtime_.driver = driver; }

    // Humanoid:SetStateEnabled / GetStateEnabled. Every state starts
    // enabled; a change fires events.stateEnabledChanged. Kept here rather
    // than in the driver so scripts can set it before anything simulates.
    bool stateEnabled(Enum::HumanoidStateType state) const;
    void setStateEnabled(Enum::HumanoidStateType state, bool enabled);

    // An R6 character's composited clothing (one texture for the torso and
    // limbs, which then draw as the content folder's avatar/meshes); empty
    // for none. Engine-side, not a property: the client's Humanoid owns its
    // composited texture the same way.
    const std::string& bodyTexture() const { return bodyTexture_; }
    void assignBodyTexture(std::string id) { bodyTexture_ = std::move(id); }

private:
    // Runtime state a clone doesn't take: no driver, every state enabled.
    struct RuntimeSlot {
        RuntimeSlot() = default;
        RuntimeSlot(const RuntimeSlot&) {}
        // NOLINTNEXTLINE(bugprone-unhandled-self-assignment): copies nothing on purpose
        RuntimeSlot& operator=(const RuntimeSlot&) { return *this; }
        ~RuntimeSlot() = default;
        HumanoidDriver* driver = nullptr;
        std::uint32_t disabledStates = 0; // bit per HumanoidStateType value
    };

    Enum::HumanoidStateType state_ = Enum::HumanoidStateType::None; // until a controller runs it
    RuntimeSlot runtime_;
    Vector3 moveDirection_;
    Enum::Material floorMaterial_ = Enum::Material::Air;
    Ref<BasePart> seatPart_;
    std::string bodyTexture_;
};

class Seat final : public InstanceImpl<Seat, Part> {
public:
    LUNAR_FIELD(bool, disabled, Disabled, false)
    // Who sits here; the engine sets it (see <lunar/Seats.hpp>).
    LUNAR_PROPERTY() Humanoid* occupant() const { return occupant_.get(); }
    void assignOccupant(Humanoid* humanoid) { setRef(occupant_, humanoid, "Occupant"); }

private:
    Ref<Humanoid> occupant_;
};

class VehicleSeat final : public InstanceImpl<VehicleSeat, BasePart> {
public:
    LUNAR_FIELD(bool, disabled, Disabled, false)
    LUNAR_FIELD(bool, headsUpDisplay, HeadsUpDisplay, true)
    LUNAR_FIELD(float, maxSpeed, MaxSpeed, 25.0f)
    LUNAR_FIELD(float, torque, Torque, 10.0f)
    LUNAR_FIELD(float, turnSpeed, TurnSpeed, 1.0f)
    LUNAR_FIELD(int, steer, Steer, 0)
    LUNAR_FIELD(float, steerFloat, SteerFloat, 0.0f)
    LUNAR_FIELD(int, throttle, Throttle, 0)
    LUNAR_FIELD(float, throttleFloat, ThrottleFloat, 0.0f)
    LUNAR_PROPERTY() Humanoid* occupant() const { return occupant_.get(); }
    void assignOccupant(Humanoid* humanoid) { setRef(occupant_, humanoid, "Occupant"); }

private:
    Ref<Humanoid> occupant_;
};

// --- Animation ---------------------------------------------------------------------

class Animation final : public InstanceImpl<Animation, Instance> {
public:
    LUNAR_FIELD(std::string, animationId, AnimationId)
};

class Animator final : public InstanceImpl<Animator, Instance> {
public:
    LUNAR_FIELD(bool, preferLodEnabled, PreferLodEnabled, true)
};

class AnimationController final : public InstanceImpl<AnimationController, Instance> {};

// The playing side of an AnimationTrack: the animation library supplies it
// (an avatar::AnimationTrack on the rig's Animator) and the instance below
// only forwards to it.
class AnimationPlayback {
public:
    virtual ~AnimationPlayback() = default;
    virtual float length() const = 0;
    virtual bool isPlaying() const = 0;
    virtual bool looped() const = 0;
    virtual void setLooped(bool looped) = 0;
    virtual Enum::AnimationPriority priority() const = 0;
    virtual void setPriority(Enum::AnimationPriority priority) = 0;
    virtual float speed() const = 0;
    virtual float timePosition() const = 0;
    virtual void setTimePosition(float time) = 0;
    virtual float weightCurrent() const = 0;
    virtual float weightTarget() const = 0;

protected:
    AnimationPlayback() = default;
    AnimationPlayback(const AnimationPlayback&) = default;
    AnimationPlayback& operator=(const AnimationPlayback&) = default;
};

// What LoadAnimation returns. Without a playback (nothing to animate) it
// reads as a track that never loads: Length 0, not playing.
class AnimationTrack final : public InstanceImpl<AnimationTrack, Instance, ClassKind::NotCreatable> {
public:
    LUNAR_PROPERTY() Animation* animation() const { return animation_.get(); }
    LUNAR_PROPERTY() float length() const { return playback_ ? playback_->length() : 0.0f; }
    LUNAR_PROPERTY() bool isPlaying() const { return playback_ && playback_->isPlaying(); }
    LUNAR_PROPERTY() bool looped() const { return playback_ && playback_->looped(); }
    void setLooped(bool looped);
    LUNAR_PROPERTY() Enum::AnimationPriority priority() const
    {
        return playback_ ? playback_->priority() : Enum::AnimationPriority::Action;
    }
    void setPriority(Enum::AnimationPriority priority);
    LUNAR_PROPERTY() float speed() const { return playback_ ? playback_->speed() : 1.0f; }
    LUNAR_PROPERTY() float timePosition() const { return playback_ ? playback_->timePosition() : 0.0f; }
    void setTimePosition(float time);
    LUNAR_PROPERTY() float weightCurrent() const { return playback_ ? playback_->weightCurrent() : 0.0f; }
    LUNAR_PROPERTY() float weightTarget() const { return playback_ ? playback_->weightTarget() : 0.0f; }

    // Ties the instance to what plays it and the Animation it was loaded from.
    void bind(std::shared_ptr<AnimationPlayback> playback, Animation* animation);
    const std::shared_ptr<AnimationPlayback>& playback() const { return playback_; }

private:
    std::shared_ptr<AnimationPlayback> playback_;
    Ref<Animation> animation_;
    // The track keeps its Animation: scripts load one made with Instance.new
    // and let it go (the game does), and still read track.Animation.AnimationId.
    std::shared_ptr<Animation> keptAnimation_;
};

// Studio's record of the rig an animation was authored on.
class AnimationRigData final : public InstanceImpl<AnimationRigData, Instance> {};

// Base of the animation formats (KeyframeSequence, CurveAnimation).
class AnimationClip : public InstanceImpl<AnimationClip, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(bool, loop, Loop, true)
    LUNAR_FIELD(Enum::AnimationPriority, priority, Priority, Enum::AnimationPriority::Action)
};

class KeyframeSequence final : public InstanceImpl<KeyframeSequence, AnimationClip> {
public:
    LUNAR_FIELD(float, authoredHipHeight, AuthoredHipHeight, 2.0f)
};

class Keyframe final : public InstanceImpl<Keyframe, Instance> {
public:
    LUNAR_FIELD(float, time, Time, 0.0f)
};

// A named event at its parent Keyframe's time (AnimationTrack:GetMarkerReachedSignal).
class KeyframeMarker final : public InstanceImpl<KeyframeMarker, Instance> {
public:
    LUNAR_FIELD(std::string, value, Value)
};

class PoseBase : public InstanceImpl<PoseBase, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Enum::PoseEasingDirection, easingDirection, EasingDirection, Enum::PoseEasingDirection::In)
    LUNAR_FIELD(Enum::PoseEasingStyle, easingStyle, EasingStyle, Enum::PoseEasingStyle::Linear)
    LUNAR_FIELD(float, weight, Weight, 1.0f)
};

class Pose final : public InstanceImpl<Pose, PoseBase> {
public:
    LUNAR_FIELD(CFrame, cframe, CFrame)
};

class NumberPose final : public InstanceImpl<NumberPose, PoseBase> {
public:
    LUNAR_FIELD(double, value, Value, 0.0)
};

// --- Appearance --------------------------------------------------------------------

class CharacterAppearance : public InstanceImpl<CharacterAppearance, Instance, ClassKind::Abstract> {};

class BodyColors final : public InstanceImpl<BodyColors, CharacterAppearance> {
public:
    LUNAR_FIELD(Color3, headColor3, HeadColor3, BrickColor{24}.color())
    LUNAR_FIELD(Color3, torsoColor3, TorsoColor3, BrickColor{23}.color())
    LUNAR_FIELD(Color3, leftArmColor3, LeftArmColor3, BrickColor{24}.color())
    LUNAR_FIELD(Color3, rightArmColor3, RightArmColor3, BrickColor{24}.color())
    LUNAR_FIELD(Color3, leftLegColor3, LeftLegColor3, BrickColor{119}.color())
    LUNAR_FIELD(Color3, rightLegColor3, RightLegColor3, BrickColor{119}.color())

    // The same colors as the nearest BrickColors (the older API).
    LUNAR_PROPERTY() BrickColor headColor() const { return BrickColor::nearest(headColor3()); }
    void setHeadColor(BrickColor color) { setHeadColor3(color.color()); }
    LUNAR_PROPERTY() BrickColor torsoColor() const { return BrickColor::nearest(torsoColor3()); }
    void setTorsoColor(BrickColor color) { setTorsoColor3(color.color()); }
    LUNAR_PROPERTY() BrickColor leftArmColor() const { return BrickColor::nearest(leftArmColor3()); }
    void setLeftArmColor(BrickColor color) { setLeftArmColor3(color.color()); }
    LUNAR_PROPERTY() BrickColor rightArmColor() const { return BrickColor::nearest(rightArmColor3()); }
    void setRightArmColor(BrickColor color) { setRightArmColor3(color.color()); }
    LUNAR_PROPERTY() BrickColor leftLegColor() const { return BrickColor::nearest(leftLegColor3()); }
    void setLeftLegColor(BrickColor color) { setLeftLegColor3(color.color()); }
    LUNAR_PROPERTY() BrickColor rightLegColor() const { return BrickColor::nearest(rightLegColor3()); }
    void setRightLegColor(BrickColor color) { setRightLegColor3(color.color()); }
};

class Clothing : public InstanceImpl<Clothing, CharacterAppearance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(Color3, color3, Color3, Color3(1.0f, 1.0f, 1.0f))
};

class Shirt final : public InstanceImpl<Shirt, Clothing> {
public:
    LUNAR_FIELD(std::string, shirtTemplate, ShirtTemplate)
};

class Pants final : public InstanceImpl<Pants, Clothing> {
public:
    LUNAR_FIELD(std::string, pantsTemplate, PantsTemplate)
};

class ShirtGraphic final : public InstanceImpl<ShirtGraphic, CharacterAppearance> {
public:
    LUNAR_FIELD(std::string, graphic, Graphic)
    LUNAR_FIELD(Color3, color3, Color3, Color3(1.0f, 1.0f, 1.0f))
};

class CharacterMesh final : public InstanceImpl<CharacterMesh, CharacterAppearance> {
public:
    LUNAR_FIELD(std::int64_t, baseTextureId, BaseTextureId, 0)
    LUNAR_FIELD(Enum::BodyPart, bodyPart, BodyPart, Enum::BodyPart::Head)
    LUNAR_FIELD(std::int64_t, meshId, MeshId, 0)
    LUNAR_FIELD(std::int64_t, overlayTextureId, OverlayTextureId, 0)
};

// Worn items: their Handle part attaches to the character.
class Accoutrement : public InstanceImpl<Accoutrement, Instance> {
public:
    LUNAR_FIELD(CFrame, attachmentPoint, AttachmentPoint)
};

class Accessory final : public InstanceImpl<Accessory, Accoutrement> {
public:
    LUNAR_FIELD(Enum::AccessoryType, accessoryType, AccessoryType, Enum::AccessoryType::Unknown)
};
class Hat final : public InstanceImpl<Hat, Accoutrement> {};

// Layered clothing: cages a WrapLayer (clothing) is fitted onto a WrapTarget (body).
class BaseWrap : public InstanceImpl<BaseWrap, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(std::string, cageMeshId, CageMeshId)
    LUNAR_FIELD(CFrame, cageOrigin, CageOrigin)
    LUNAR_FIELD(CFrame, importOrigin, ImportOrigin)
    LUNAR_FIELD(std::string, hsrAssetId, HSRAssetId)
};

class WrapTarget final : public InstanceImpl<WrapTarget, BaseWrap> {
public:
    LUNAR_FIELD(float, stiffness, Stiffness, 0.9f)
};

class WrapLayer final : public InstanceImpl<WrapLayer, BaseWrap> {
public:
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(int, order, Order, 1)
    LUNAR_FIELD(float, puffiness, Puffiness, 1.0f)
    LUNAR_FIELD(std::string, referenceMeshId, ReferenceMeshId)
    LUNAR_FIELD(CFrame, referenceOrigin, ReferenceOrigin)
    LUNAR_FIELD(float, shrinkFactor, ShrinkFactor, 0.0f)
};

class WrapTextureTransfer final : public InstanceImpl<WrapTextureTransfer, Instance> {
public:
    LUNAR_FIELD(std::string, referenceCageMeshContent, ReferenceCageMeshContent)
    LUNAR_FIELD(Vector2, uvMinBound, UVMinBound)
    LUNAR_FIELD(Vector2, uvMaxBound, UVMaxBound, Vector2(1.0f, 1.0f))
};

// Facial animation controls of a dynamic head.
class FaceControls final : public InstanceImpl<FaceControls, Instance> {};

// What a character should look like: scales, animations, clothing and, as
// children, body part and accessory descriptions.
class HumanoidDescription final : public InstanceImpl<HumanoidDescription, Instance> {
public:
    using Emotes = std::map<std::string, std::set<std::int64_t>, std::less<>>;
    using EquippedEmotes = std::map<int, std::string>;
    const Emotes& emotes() const { return emoteState_.available; }
    const EquippedEmotes& equippedEmotes() const { return emoteState_.equipped; }
    void setEmotes(Emotes emotes);
    void addEmote(std::string name, std::int64_t id);
    void removeEmote(std::string_view name);
    void setEquippedEmotes(EquippedEmotes emotes);
    Signal<const Emotes&>& emotesChanged() { return emoteState_.availableChanged; }
    Signal<const EquippedEmotes&>& equippedEmotesChanged() { return emoteState_.equippedChanged; }
    LUNAR_PROPERTY() std::string emotesDataInternal() const;
    void setEmotesDataInternal(std::string data);
    LUNAR_PROPERTY() std::string equippedEmotesDataInternal() const;
    void setEquippedEmotesDataInternal(std::string data);
    LUNAR_FIELD(float, bodyTypeScale, BodyTypeScale, 0.3f)
    LUNAR_FIELD(float, depthScale, DepthScale, 1.0f)
    LUNAR_FIELD(float, headScale, HeadScale, 1.0f)
    LUNAR_FIELD(float, heightScale, HeightScale, 1.0f)
    LUNAR_FIELD(float, proportionScale, ProportionScale, 1.0f)
    LUNAR_FIELD(float, widthScale, WidthScale, 1.0f)
    LUNAR_FIELD(std::int64_t, face, Face, 0)
    LUNAR_FIELD(std::int64_t, graphicTShirt, GraphicTShirt, 0)
    LUNAR_FIELD(std::int64_t, pants, Pants, 0)
    LUNAR_FIELD(std::int64_t, shirt, Shirt, 0)
    LUNAR_FIELD(std::int64_t, climbAnimation, ClimbAnimation, 0)
    LUNAR_FIELD(std::int64_t, fallAnimation, FallAnimation, 0)
    LUNAR_FIELD(std::int64_t, idleAnimation, IdleAnimation, 0)
    LUNAR_FIELD(std::int64_t, jumpAnimation, JumpAnimation, 0)
    LUNAR_FIELD(std::int64_t, moodAnimation, MoodAnimation, 0)
    LUNAR_FIELD(std::int64_t, runAnimation, RunAnimation, 0)
    LUNAR_FIELD(std::int64_t, swimAnimation, SwimAnimation, 0)
    LUNAR_FIELD(std::int64_t, walkAnimation, WalkAnimation, 0)
    LUNAR_FIELD(bool, useAvatarSettings, UseAvatarSettings, false)
    // Body part asset ids (0 keeps the character's own part).
    LUNAR_FIELD(std::int64_t, head, Head, 0)
    LUNAR_FIELD(std::int64_t, torso, Torso, 0)
    LUNAR_FIELD(std::int64_t, leftArm, LeftArm, 0)
    LUNAR_FIELD(std::int64_t, rightArm, RightArm, 0)
    LUNAR_FIELD(std::int64_t, leftLeg, LeftLeg, 0)
    LUNAR_FIELD(std::int64_t, rightLeg, RightLeg, 0)
    LUNAR_FIELD(Color3, headColor, HeadColor)
    LUNAR_FIELD(Color3, torsoColor, TorsoColor)
    LUNAR_FIELD(Color3, leftArmColor, LeftArmColor)
    LUNAR_FIELD(Color3, rightArmColor, RightArmColor)
    LUNAR_FIELD(Color3, leftLegColor, LeftLegColor)
    LUNAR_FIELD(Color3, rightLegColor, RightLegColor)
    // Rigid accessories: comma-separated asset ids per slot.
    LUNAR_FIELD(std::string, hatAccessory, HatAccessory)
    LUNAR_FIELD(std::string, hairAccessory, HairAccessory)
    LUNAR_FIELD(std::string, faceAccessory, FaceAccessory)
    LUNAR_FIELD(std::string, neckAccessory, NeckAccessory)
    LUNAR_FIELD(std::string, shouldersAccessory, ShouldersAccessory)
    LUNAR_FIELD(std::string, frontAccessory, FrontAccessory)
    LUNAR_FIELD(std::string, backAccessory, BackAccessory)
    LUNAR_FIELD(std::string, waistAccessory, WaistAccessory)

private:
    struct EmoteState {
        EmoteState() = default;
        EmoteState(const EmoteState& other) : available(other.available), equipped(other.equipped) {}
        Emotes available;
        EquippedEmotes equipped;
        Signal<const Emotes&> availableChanged;
        Signal<const EquippedEmotes&> equippedChanged;
    };
    EmoteState emoteState_;
};

class BodyPartDescription final : public InstanceImpl<BodyPartDescription, Instance> {
public:
    LUNAR_FIELD(std::int64_t, assetId, AssetId, 0)
    LUNAR_FIELD(Enum::BodyPart, bodyPart, BodyPart, Enum::BodyPart::Head)
    LUNAR_FIELD(Color3, color, Color)
    LUNAR_FIELD(std::string, headShape, HeadShape)
    LUNAR_REF(Instance, instance, Instance)
};

class AccessoryDescription final : public InstanceImpl<AccessoryDescription, Instance> {
public:
    LUNAR_FIELD(Enum::AccessoryType, accessoryType, AccessoryType, Enum::AccessoryType::Unknown)
    LUNAR_FIELD(std::int64_t, assetId, AssetId, 0)
    LUNAR_REF(Instance, instance, Instance)
    LUNAR_FIELD(bool, isLayered, IsLayered, false)
    LUNAR_FIELD(int, order, Order, 0)
    LUNAR_FIELD(Vector3, position, Position)
    LUNAR_FIELD(float, puffiness, Puffiness, 1.0f)
    LUNAR_FIELD(Vector3, rotation, Rotation)
    LUNAR_FIELD(Vector3, scale, Scale, Vector3(1.0f, 1.0f, 1.0f))
};

// --- Tools -------------------------------------------------------------------------

class BackpackItem : public InstanceImpl<BackpackItem, Model, ClassKind::Abstract> {
public:
    LUNAR_FIELD(std::string, textureId, TextureId)
};

class Tool final : public InstanceImpl<Tool, BackpackItem> {
public:
    LUNAR_FIELD(bool, canBeDropped, CanBeDropped, true)
    LUNAR_FIELD(bool, enabled, Enabled, true)
    // Where the Handle sits in the hand: the RightGrip weld's C1.
    LUNAR_FIELD(CFrame, grip, Grip)
    LUNAR_FIELD(bool, manualActivationOnly, ManualActivationOnly, false)
    LUNAR_FIELD(bool, requiresHandle, RequiresHandle, true)
    LUNAR_FIELD(std::string, toolTip, ToolTip)

    // Grip in pieces, the older API: its position and rotation columns
    // (GripForward is the look vector, the third column negated). Setting
    // one replaces just that part of Grip.
    LUNAR_PROPERTY() Vector3 gripPos() const { return grip().position; }
    void setGripPos(Vector3 position);
    LUNAR_PROPERTY() Vector3 gripForward() const { return grip().lookVector(); }
    void setGripForward(Vector3 forward);
    LUNAR_PROPERTY() Vector3 gripRight() const { return grip().rightVector(); }
    void setGripRight(Vector3 right);
    LUNAR_PROPERTY() Vector3 gripUp() const { return grip().upVector(); }
    void setGripUp(Vector3 up);

    // The part the character holds: a BasePart child named "Handle".
    BasePart* handle() const;
};

// The pre-Tool backpack item (deprecated): selecting one runs its scripts
// or a built-in command. Kept so old places load and scripts can read it.
class HopperBin final : public InstanceImpl<HopperBin, BackpackItem> {
public:
    LUNAR_FIELD(bool, active, Active, false)
    LUNAR_FIELD(Enum::BinType, binType, BinType, Enum::BinType::Script)
};

} // namespace lunar
