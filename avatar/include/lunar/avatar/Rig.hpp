#pragma once

// Character rigs: the joints that hold a character's parts together, and
// placing the parts from them every frame.

#include <lunar/Classes.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace lunar::avatar {

// The part a character is assembled around: its HumanoidRootPart, else the
// model's PrimaryPart, else a part named Torso (old R6 models). Null if none.
BasePart* findRootPart(const Instance& character);

// Where a JointInstance puts Part1 relative to Part0:
//     Part1.CFrame = Part0.CFrame * C0 * Transform * C1:Inverse()
// Transform is Motor6D.Transform (the animated offset; identity for other
// joints). A legacy Motor turns by CurrentAngle about C0's Z axis instead.
CFrame jointOffset(const JointInstance& joint);

// The same for the constraints that hold parts rigidly: an
// AnimationConstraint (the joints of upgraded avatars, see Animator.hpp)
// puts Attachment1's part relative to Attachment0's as a Motor6D would with
// the attachments' CFrames for C0 and C1,
//     Part1.CFrame = Part0.CFrame * A0.CFrame * Transform * A1.CFrame:Inverse()
// and a RigidConstraint (which holds accessories on upgraded avatars)
// the same without Transform. GUESS: the client steps a kinematic
// AnimationConstraint like that; the docs say it acts as a Motor6D.
// Nullopt when an attachment or its part is missing.
std::optional<CFrame> constraintOffset(const Constraint& constraint);

// Moves every part rigidly connected to root (through enabled JointInstances,
// WeldConstraints, AnimationConstraints and RigidConstraints below `scope`,
// followed both ways from Part0 to Part1 and back) so that each joint
// holds. Root itself stays put; parts not connected to it aren't touched.
// When root isn't anchored and an anchored part is connected to it, that
// part stays put instead and root moves with the rest, as an anchored part
// is the root of its assembly in Roblox.
// This is the kinematic half of what Roblox does when it steps an
// assembly: the root's CFrame comes from physics (or the caller),
// everything else from the joints, so it runs after the Animator wrote the
// joints' Transform. Returns how many parts it placed.
//
// WeldConstraints have no offsets of their own: they keep whatever offset
// their parts had when the call started, which is how Roblox's "welds the
// parts where they are" plays out for parts that only ever move together.
std::size_t solveJoints(const Instance& scope, BasePart& root);

// solveJoints from the character's findRootPart, if it has one.
std::size_t solveCharacterJoints(const Instance& character);

// solveCharacterJoints for one character, every frame: which joints hold
// which parts, and the order they're placed in, are worked out once and
// again only after the character gains or loses a descendant, changes its
// PrimaryPart or one of its joints changes Part0, Part1 or Enabled. The
// offsets (Motor6D.Transform, C0, C1, the parts a WeldConstraint holds) are
// read fresh each time, so it places the parts exactly as
// solveCharacterJoints would, without walking the character every frame.
class CharacterJointSolver {
public:
    explicit CharacterJointSolver(const std::shared_ptr<Model>& character);
    ~CharacterJointSolver();
    CharacterJointSolver(const CharacterJointSolver&) = delete;
    CharacterJointSolver& operator=(const CharacterJointSolver&) = delete;

    // Places the parts; returns how many, like solveCharacterJoints.
    std::size_t solve();

private:
    struct Joint {
        const Instance* instance = nullptr;
        // Exactly one of these three.
        const JointInstance* joint = nullptr;
        const WeldConstraint* weld = nullptr;
        const Constraint* constraint = nullptr; // AnimationConstraint or RigidConstraint
        BasePart* part0 = nullptr;
        BasePart* part1 = nullptr;
        CFrame offset; // this frame's
    };
    struct Step {
        BasePart* parent = nullptr;
        BasePart* child = nullptr;
        std::size_t joint = 0;
        bool reverse = false; // child = parent * offset:Inverse()
    };
    void rebuild();
    void forget();

    std::weak_ptr<Model> character_;
    BasePart* root_ = nullptr;
    std::vector<Joint> joints_;
    std::vector<Step> steps_; // breadth first from the root
    std::vector<Connection> watches_;
    std::shared_ptr<bool> dirty_ = std::make_shared<bool>(true);
};

// Humanoid:BuildRigFromAttachments() (0x10363f16a). With the Humanoid's
// AutomaticScalingEnabled it first rescales the parts (applyAutomaticScaling),
// then joins the parts (joinRigAttachments) and, for R15, sets HipHeight
// from the legs (computeR15HipHeight). Returns the joints made or updated;
// empty without a HumanoidRootPart.
std::vector<Motor6D*> buildRigFromAttachments(Instance& character);

// The joining step (0x10363ccd8, joint creation 0x10363d887): a depth-first
// walk from the root part. For each attachment of the current part whose
// name contains "RigAttachment", the joint is named by what comes before it
// ("NeckRigAttachment" gives "Neck"); unless the current part already has a
// child of that name, the first other unvisited part with an attachment of
// the same name gets a Motor6D (reused if it already has one of that name)
// with Part0 = this part, Part1 = that part, C0/C1 = the two attachments'
// CFrames, parented to Part1; then the walk continues into it. Nothing is
// deleted.
std::vector<Motor6D*> joinRigAttachments(Instance& character, BasePart& root);

// The scale an R15 part gets from the Humanoid's BodyTypeScale and
// BodyProportionScale (0x103658726): 1 + bodyType * (lerp(A, B, proportion)
// - 1), divided by the part's own proportions (its AvatarPartScaleType:
// Classic divides by nothing, ProportionsNormal by A, ProportionsSlender
// by B). A and B are the client's per-part Rthro tables. GUESS (from the
// documented behaviour, not traced): proportion is the lerp parameter.
Vector3 rthroScale(std::string_view partName, std::string_view scaleType, float bodyType, float proportion);

// Humanoid automatic scaling (0x103639c56, 0x103658fe8): if the Humanoid has
// any of the BodyHeightScale/BodyWidthScale/BodyDepthScale/HeadScale/
// BodyTypeScale/BodyProportionScale NumberValues, every part's Size becomes
// OriginalSize * (width, height, depth) * rthroScale (HeadScale on all axes
// for the Head), and every attachment's Position its OriginalPosition times
// the same. Missing OriginalSize/OriginalPosition values are made from the
// current ones first. Returns whether it scaled anything.
bool applyAutomaticScaling(Instance& character);

// R15 HipHeight from the leg chain (0x10363c42c): per side, the distance
// from the root part's bottom down through the hip, knee and ankle rig
// attachments to the foot's sole; the shorter side when they're within 5%
// of each other (FInt SimilarHipHeightPercentage 50 / 1000), else the
// longer. Nullopt when a part or attachment is missing.
std::optional<float> computeR15HipHeight(const Instance& character);

// The joints the client builds for an R6 character (0x10363fc4a), whose
// rig file has none: RootJoint in the HumanoidRootPart, Neck, Left/Right
// Shoulder and Left/Right Hip in the Torso, all Motor6Ds. Returns them.
std::vector<Motor6D*> buildR6Joints(Instance& character);

} // namespace lunar::avatar
