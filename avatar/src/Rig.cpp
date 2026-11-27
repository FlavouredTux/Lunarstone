#include <lunar/avatar/Rig.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lunar::avatar {

namespace {

// One rigid link: other = self * offset.
struct Link {
    BasePart* other;
    CFrame offset;
};

// Where the walk over an assembly's joints starts: `root`, unless it isn't
// anchored and an anchored part is joined to it, which then holds the
// assembly where it is, as an anchored part is the root of its assembly in
// Roblox. For a server-driven NPC, the rig's Root hangs by a ManualWeld off the anchored
// Figure (outside the rig model), which the server moves; walked from Root,
// Figure was put back where Root was each frame and the NPC never moved
// a hundred studs under the room it was made in [replay].
template <class LinkT>
BasePart* walkStart(const std::unordered_map<BasePart*, std::vector<LinkT>>& links, BasePart* root)
{
    if (root->anchored())
        return root;
    std::unordered_set<BasePart*> seen{root};
    std::deque<BasePart*> queue{root};
    while (!queue.empty()) {
        BasePart* part = queue.front();
        queue.pop_front();
        if (part->anchored())
            return part;
        if (auto found = links.find(part); found != links.end())
            for (const LinkT& link : found->second)
                if (seen.insert(link.other).second)
                    queue.push_back(link.other);
    }
    return root;
}

void addLink(std::unordered_map<BasePart*, std::vector<Link>>& links, BasePart* part0, BasePart* part1,
             const CFrame& offset)
{
    links[part0].push_back({part1, offset});
    links[part1].push_back({part0, offset.inverse()});
}

} // namespace

BasePart* findRootPart(const Instance& character)
{
    if (auto* root = instanceCast<BasePart>(character.findFirstChild("HumanoidRootPart")))
        return root;
    if (const auto* model = instanceCast<const Model>(&character); model && model->primaryPart())
        return model->primaryPart();
    return instanceCast<BasePart>(character.findFirstChild("Torso"));
}

CFrame jointOffset(const JointInstance& joint)
{
    if (const auto* motor6d = instanceCast<const Motor6D>(&joint))
        return joint.c0() * motor6d->transform() * joint.c1().inverse();
    if (const auto* motor = instanceCast<const Motor>(&joint))
        return joint.c0() * CFrame::angles(0.0f, 0.0f, motor->currentAngle()) * joint.c1().inverse();
    return joint.c0() * joint.c1().inverse();
}

namespace {

// An attachment's part and its CFrame in that part's space (attachments can
// sit under other attachments).
std::pair<BasePart*, CFrame> attachmentFrame(const Attachment* attachment)
{
    CFrame frame;
    const Instance* node = attachment;
    while (const auto* a = instanceCast<const Attachment>(node)) {
        frame = a->cframe() * frame;
        node = a->parent();
    }
    return {const_cast<BasePart*>(instanceCast<const BasePart>(node)), frame};
}

// The constraints that hold two parts rigidly, and those parts.
bool rigidConstraint(const Instance& instance)
{
    return instance.isA("AnimationConstraint") || instance.isA("RigidConstraint");
}

std::pair<BasePart*, BasePart*> constraintParts(const Constraint& constraint)
{
    return {attachmentFrame(constraint.attachment0()).first, attachmentFrame(constraint.attachment1()).first};
}

} // namespace

std::optional<CFrame> constraintOffset(const Constraint& constraint)
{
    const auto [part0, frame0] = attachmentFrame(constraint.attachment0());
    const auto [part1, frame1] = attachmentFrame(constraint.attachment1());
    if (!part0 || !part1)
        return std::nullopt;
    if (const auto* animation = instanceCast<const AnimationConstraint>(&constraint))
        return frame0 * animation->transform() * frame1.inverse();
    return frame0 * frame1.inverse();
}

std::size_t solveJoints(const Instance& scope, BasePart& root)
{
    std::unordered_map<BasePart*, std::vector<Link>> links;
    auto visit = [&](const Instance& instance) {
        if (const auto* joint = instanceCast<const JointInstance>(&instance)) {
            if (joint->enabled() && joint->part0() && joint->part1() && joint->part0() != joint->part1())
                addLink(links, joint->part0(), joint->part1(), jointOffset(*joint));
        } else if (const auto* weld = instanceCast<const WeldConstraint>(&instance)) {
            BasePart* part0 = weld->part0();
            BasePart* part1 = weld->part1();
            if (weld->enabled() && part0 && part1 && part0 != part1)
                addLink(links, part0, part1, part0->cframe().inverse() * part1->cframe());
        } else if (const auto* constraint = instanceCast<const Constraint>(&instance);
                   constraint && rigidConstraint(instance) && constraint->enabled()) {
            const auto [part0, part1] = constraintParts(*constraint);
            if (part0 && part1 && part0 != part1)
                addLink(links, part0, part1, *constraintOffset(*constraint));
        }
    };
    visit(scope);
    for (const InstancePtr& descendant : scope.descendants())
        visit(*descendant);

    // Breadth first from the root (an anchored part joined to it, if any),
    // so each part is placed from the joint nearest the root, the way
    // Roblox's spanning tree of an assembly goes.
    BasePart* start = walkStart(links, &root);
    std::unordered_set<BasePart*> placed{start};
    std::deque<BasePart*> queue{start};
    std::size_t count = 0;
    while (!queue.empty()) {
        BasePart* part = queue.front();
        queue.pop_front();
        auto found = links.find(part);
        if (found == links.end())
            continue;
        const CFrame frame = part->cframe();
        for (const Link& link : found->second) {
            if (!placed.insert(link.other).second)
                continue;
            link.other->setCFrame(frame * link.offset);
            queue.push_back(link.other);
            ++count;
        }
    }
    return count;
}

std::size_t solveCharacterJoints(const Instance& character)
{
    BasePart* root = findRootPart(character);
    return root ? solveJoints(character, *root) : 0;
}

CharacterJointSolver::CharacterJointSolver(const std::shared_ptr<Model>& character) : character_(character) {}

CharacterJointSolver::~CharacterJointSolver()
{
    forget();
}

void CharacterJointSolver::forget()
{
    for (Connection& watch : watches_)
        watch.disconnect();
    watches_.clear();
}

// The same walk as solveJoints, with each link remembering its joint instead of its offset.
void CharacterJointSolver::rebuild()
{
    forget();
    joints_.clear();
    steps_.clear();
    *dirty_ = false;
    const std::shared_ptr<Model> character = character_.lock();
    root_ = character ? findRootPart(*character) : nullptr;
    if (!character)
        return;
    auto markDirty = [dirty = std::weak_ptr<bool>(dirty_)](auto&&...) {
        if (auto flag = dirty.lock())
            *flag = true;
    };
    watches_.push_back(character->descendantAdded.connect(markDirty));
    watches_.push_back(character->descendantRemoving.connect(markDirty));
    watches_.push_back(character->changed.connect([markDirty](std::string_view property) {
        if (property == "PrimaryPart")
            markDirty();
    }));
    if (!root_)
        return;

    struct Link {
        BasePart* other;
        std::size_t joint;
        bool reverse;
    };
    std::unordered_map<BasePart*, std::vector<Link>> links;
    auto visit = [&](const Instance& instance) {
        Joint joint;
        bool enabled = false;
        if (const auto* j = instanceCast<const JointInstance>(&instance)) {
            joint = {&instance, j, nullptr, nullptr, j->part0(), j->part1(), {}};
            enabled = j->enabled();
        } else if (const auto* weld = instanceCast<const WeldConstraint>(&instance)) {
            joint = {&instance, nullptr, weld, nullptr, weld->part0(), weld->part1(), {}};
            enabled = weld->enabled();
        } else if (const auto* constraint = instanceCast<const Constraint>(&instance);
                   constraint && rigidConstraint(instance)) {
            const auto [part0, part1] = constraintParts(*constraint);
            joint = {&instance, nullptr, nullptr, constraint, part0, part1, {}};
            enabled = constraint->enabled();
        } else {
            return;
        }
        watches_.push_back(const_cast<Instance&>(instance).changed.connect([markDirty](std::string_view property) {
            if (property == "Part0" || property == "Part1" || property == "Enabled" || property == "Attachment0"
                || property == "Attachment1")
                markDirty();
        }));
        if (!enabled || !joint.part0 || !joint.part1 || joint.part0 == joint.part1)
            return;
        links[joint.part0].push_back({joint.part1, joints_.size(), false});
        links[joint.part1].push_back({joint.part0, joints_.size(), true});
        joints_.push_back(joint);
    };
    // In place: a character's effects come and go, and each time this runs
    // again (descendants() would copy a pointer to every part of it).
    auto walk = [&](this const auto& self, const Instance& at) -> void {
        visit(at);
        for (const InstancePtr& child : at.children())
            self(*child);
    };
    walk(*character);

    // Which part is anchored decides where the walk starts, and a part
    // outside the character (an NPC's Figure part) can go without the character
    // hearing of it: either redoes the walk.
    for (const auto& [part, partLinks] : links) {
        watches_.push_back(part->changed.connect([markDirty](std::string_view property) {
            if (property == "Anchored")
                markDirty();
        }));
        if (!part->isDescendantOf(character.get()))
            watches_.push_back(part->ancestryChanged.connect(markDirty));
    }
    BasePart* start = walkStart(links, root_);
    std::unordered_set<BasePart*> placed{start};
    std::deque<BasePart*> queue{start};
    while (!queue.empty()) {
        BasePart* part = queue.front();
        queue.pop_front();
        auto found = links.find(part);
        if (found == links.end())
            continue;
        for (const Link& link : found->second) {
            if (!placed.insert(link.other).second)
                continue;
            steps_.push_back({part, link.other, link.joint, link.reverse});
            queue.push_back(link.other);
        }
    }
}

std::size_t CharacterJointSolver::solve()
{
    if (*dirty_)
        rebuild();
    if (!root_ || character_.expired())
        return 0;
    // Every offset first, as solveJoints takes them all before moving anything.
    for (Joint& joint : joints_) {
        if (joint.joint)
            joint.offset = jointOffset(*joint.joint);
        else if (joint.constraint)
            joint.offset = constraintOffset(*joint.constraint).value_or(joint.offset);
        else
            joint.offset = joint.part0->cframe().inverse() * joint.part1->cframe();
    }
    for (const Step& step : steps_) {
        const CFrame& offset = joints_[step.joint].offset;
        step.child->setCFrame(step.parent->cframe() * (step.reverse ? offset.inverse() : offset));
    }
    return steps_.size();
}

namespace {

struct PartScale {
    std::string_view part;
    Vector3 scale;
};

// The client's Rthro scale tables, per R15 part: A at BodyProportionScale 0,
// B at 1, for Classic parts and for Proportions parts.
// ClassicA (0x107bc4728)
constexpr PartScale classicA[] = {
    {"Head", {0.941999972f, 0.941999972f, 0.941999972f}},
    {"UpperTorso", {1.03299999f, 1.30900002f, 1.13999999f}},
    {"LowerTorso", {1.03299999f, 1.30900002f, 1.13999999f}},
    {"LeftUpperArm", {1.12899995f, 1.34200001f, 1.13199997f}},
    {"LeftLowerArm", {1.12899995f, 1.34200001f, 1.13199997f}},
    {"LeftHand", {1.06599998f, 1.17400002f, 1.23099995f}},
    {"RightUpperArm", {1.12899995f, 1.34200001f, 1.13199997f}},
    {"RightLowerArm", {1.12899995f, 1.34200001f, 1.13199997f}},
    {"RightHand", {1.06599998f, 1.17400002f, 1.23099995f}},
    {"LeftUpperLeg", {1.023f, 1.50600004f, 1.023f}},
    {"LeftLowerLeg", {1.023f, 1.50600004f, 1.023f}},
    {"LeftFoot", {1.079f, 1.26699996f, 1.12899995f}},
    {"RightUpperLeg", {1.023f, 1.50600004f, 1.023f}},
    {"RightLowerLeg", {1.023f, 1.50600004f, 1.023f}},
    {"RightFoot", {1.079f, 1.26699996f, 1.12899995f}},
};

// ClassicB (0x107bc4750)
constexpr PartScale classicB[] = {
    {"Head", {0.896289229f, 0.941999972f, 0.896289229f}},
    {"UpperTorso", {0.905346155f, 1.20423186f, 1.01333332f}},
    {"LowerTorso", {0.985687017f, 1.00460482f, 1.01333332f}},
    {"LeftUpperArm", {1.00355554f, 1.20792091f, 1.00622225f}},
    {"LeftLowerArm", {1.00355554f, 1.20792091f, 1.00622225f}},
    {"LeftHand", {0.947555542f, 1.17400002f, 1.09422219f}},
    {"RightUpperArm", {1.00355554f, 1.20792091f, 1.00622225f}},
    {"RightLowerArm", {1.00355554f, 1.20792091f, 1.00622225f}},
    {"RightHand", {0.947555542f, 1.17400002f, 1.09422219f}},
    {"LeftUpperLeg", {0.976145089f, 1.40093017f, 0.909333348f}},
    {"LeftLowerLeg", {0.976145089f, 1.30051816f, 0.909333348f}},
    {"LeftFoot", {1.02958012f, 1.1332736f, 1.00355554f}},
    {"RightUpperLeg", {0.976145089f, 1.40093017f, 0.909333348f}},
    {"RightLowerLeg", {0.976145089f, 1.30051816f, 0.909333348f}},
    {"RightFoot", {1.02958012f, 1.1332736f, 1.00355554f}},
};

// PropA (0x107bc4778)
constexpr PartScale propA[] = {
    {"Head", {0.625f, 0.625f, 0.625f}},
    {"UpperTorso", {0.98619324f, 1.2285012f, 1.08225107f}},
    {"LowerTorso", {0.98619324f, 1.2285012f, 1.08225107f}},
    {"LeftUpperArm", {0.892060637f, 1.46842885f, 1.03305781f}},
    {"LeftLowerArm", {0.892060637f, 1.46842885f, 1.03305781f}},
    {"LeftHand", {0.719424486f, 1.03412616f, 0.832639456f}},
    {"RightUpperArm", {0.892060637f, 1.46842885f, 1.03305781f}},
    {"RightLowerArm", {0.892060637f, 1.46842885f, 1.03305781f}},
    {"RightHand", {0.719424486f, 1.03412616f, 0.832639456f}},
    {"LeftUpperLeg", {1.02249491f, 1.2285012f, 0.946969688f}},
    {"LeftLowerLeg", {1.02249491f, 1.2285012f, 0.946969688f}},
    {"LeftFoot", {0.71225071f, 1.04931796f, 1.07411385f}},
    {"RightUpperLeg", {1.02249491f, 1.2285012f, 0.946969688f}},
    {"RightLowerLeg", {1.02249491f, 1.2285012f, 0.946969688f}},
    {"RightFoot", {0.71225071f, 1.04931796f, 1.07411385f}},
};

// PropB (0x107bc47a0)
constexpr PartScale propB[] = {
    {"Head", {0.594671726f, 0.625f, 0.594671726f}},
    {"UpperTorso", {0.864323616f, 1.13017595f, 0.962000966f}},
    {"LowerTorso", {0.941024125f, 0.942825198f, 0.962000966f}},
    {"LeftUpperArm", {0.792942762f, 1.3217181f, 0.918273628f}},
    {"LeftLowerArm", {0.792942762f, 1.3217181f, 0.918273628f}},
    {"LeftHand", {0.639488399f, 1.03412616f, 0.740123987f}},
    {"RightUpperArm", {0.792942762f, 1.3217181f, 0.918273628f}},
    {"RightLowerArm", {0.792942762f, 1.3217181f, 0.918273628f}},
    {"RightHand", {0.639488399f, 1.03412616f, 0.740123987f}},
    {"LeftUpperLeg", {0.975663126f, 1.14279175f, 0.84175086f}},
    {"LeftLowerLeg", {0.975663126f, 1.06088185f, 0.84175086f}},
    {"LeftFoot", {0.679628551f, 0.938567042f, 0.954767883f}},
    {"RightUpperLeg", {0.975663126f, 1.14279175f, 0.84175086f}},
    {"RightLowerLeg", {0.975663126f, 1.06088185f, 0.84175086f}},
    {"RightFoot", {0.679628551f, 0.938567042f, 0.954767883f}},
};

Vector3 lookup(std::span<const PartScale> table, std::string_view part)
{
    for (const PartScale& entry : table)
        if (entry.part == part)
            return entry.scale;
    return {1.0f, 1.0f, 1.0f};
}

Vector3 times(Vector3 a, Vector3 b)
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

Vector3 divided(Vector3 a, Vector3 b)
{
    return {a.x / b.x, a.y / b.y, a.z / b.z};
}

// The Vector3Value child of that name, made from `fallback` if missing.
Vector3 originalValue(Instance& parent, std::string_view name, Vector3 fallback)
{
    if (auto* value = instanceCast<Vector3Value>(parent.findFirstChild(name)))
        return value->value();
    auto value = std::make_shared<Vector3Value>();
    value->setName(std::string(name));
    value->setValue(fallback);
    value->setParent(&parent);
    return fallback;
}

void addJoint(std::vector<Motor6D*>& joints, BasePart& part0, BasePart& part1, const CFrame& c0, const CFrame& c1,
              Instance& parent, const std::string& name)
{
    auto* motor = instanceCast<Motor6D>(parent.findFirstChild(name));
    if (!motor) {
        auto created = std::make_shared<Motor6D>();
        created->setName(name);
        created->setParent(&parent);
        motor = created.get();
    }
    motor->setPart0(&part0);
    motor->setPart1(&part1);
    motor->setC0(c0);
    motor->setC1(c1);
    joints.push_back(motor);
}

void joinFrom(BasePart& part, const std::vector<BasePart*>& parts, std::unordered_set<BasePart*>& visited,
              std::vector<Motor6D*>& joints)
{
    constexpr std::string_view marker = "RigAttachment";
    visited.insert(&part);
    // Children may be added (new joints) while walking; take a copy.
    const std::vector<InstancePtr> children(part.children().begin(), part.children().end());
    for (const InstancePtr& child : children) {
        auto* own = instanceCast<Attachment>(child.get());
        if (!own)
            continue;
        const std::size_t at = own->name().find(marker);
        if (at == std::string::npos)
            continue;
        const std::string jointName = own->name().substr(0, at);
        if (part.findFirstChild(jointName))
            continue;
        for (BasePart* other : parts) {
            if (other == &part || visited.contains(other))
                continue;
            auto* match = instanceCast<Attachment>(other->findFirstChild(own->name()));
            if (!match)
                continue;
            addJoint(joints, part, *other, own->cframe(), match->cframe(), *other, jointName);
            joinFrom(*other, parts, visited, joints);
            break;
        }
    }
}

} // namespace

Vector3 rthroScale(std::string_view partName, std::string_view scaleType, float bodyType, float proportion)
{
    const bool classic = scaleType != "ProportionsNormal" && scaleType != "ProportionsSlender";
    const Vector3 a = lookup(classic ? classicA : propA, partName);
    const Vector3 b = lookup(classic ? classicB : propB, partName);
    const Vector3 blended = a + (b - a) * proportion;
    const Vector3 one{1.0f, 1.0f, 1.0f};
    const Vector3 v = one + (blended - one) * bodyType;
    if (classic)
        return v;
    return divided(v, scaleType == "ProportionsNormal" ? a : b);
}

bool applyAutomaticScaling(Instance& character)
{
    auto* humanoid = instanceCast<Humanoid>(character.findFirstChildOfClass("Humanoid"));
    if (!humanoid)
        return false;
    bool any = false;
    auto read = [&](std::string_view name, double fallback, double low, double high) {
        auto* value = instanceCast<NumberValue>(humanoid->findFirstChild(name));
        if (!value)
            return static_cast<float>(fallback);
        any = true;
        const double clamped = std::clamp(value->value(), low, high);
        value->setValue(clamped); // the clamp is written back
        return static_cast<float>(clamped);
    };
    constexpr double huge = 1e30;
    const float height = read("BodyHeightScale", 1.0, 0.01, huge);
    const float width = read("BodyWidthScale", 1.0, 0.01, huge);
    const float depth = read("BodyDepthScale", 1.0, 0.01, huge);
    const float head = read("HeadScale", 1.0, 0.01, huge);
    const float bodyType = read("BodyTypeScale", 0.0, 0.0, 1.0);
    const float proportion = read("BodyProportionScale", 0.0, 0.0, 1.0);
    if (!any)
        return false;

    for (const InstancePtr& child : character.children()) {
        auto* part = instanceCast<BasePart>(child.get());
        if (!part)
            continue;
        std::string scaleType;
        if (const auto* type = instanceCast<const StringValue>(part->findFirstChild("AvatarPartScaleType")))
            scaleType = type->value();
        const Vector3 axes = part->name() == "Head" ? Vector3{head, head, head} : Vector3{width, height, depth};
        const Vector3 scale = times(axes, rthroScale(part->name(), scaleType, bodyType, proportion));
        const Vector3 originalSize = originalValue(*part, "OriginalSize", part->size());
        part->setSize(times(originalSize, scale));
        for (const InstancePtr& item : part->children()) {
            auto* attachment = instanceCast<Attachment>(item.get());
            if (!attachment)
                continue;
            const Vector3 original = originalValue(*attachment, "OriginalPosition", attachment->position());
            attachment->setPosition(times(original, scale));
        }
    }
    return true;
}

std::optional<float> computeR15HipHeight(const Instance& character)
{
    auto part = [&](std::string_view name) { return instanceCast<const BasePart>(character.findFirstChild(name)); };
    auto y = [](const BasePart* p, std::string_view attachment) -> std::optional<float> {
        if (const auto* a = instanceCast<const Attachment>(p->findFirstChild(attachment)))
            return a->position().y;
        return std::nullopt;
    };
    const BasePart* root = part("HumanoidRootPart");
    const BasePart* lowerTorso = part("LowerTorso");
    if (!root || !lowerTorso)
        return std::nullopt;
    auto side = [&](std::string_view s) -> std::optional<float> {
        const std::string prefix(s);
        const BasePart* upper = part(prefix + "UpperLeg");
        const BasePart* lower = part(prefix + "LowerLeg");
        const BasePart* foot = part(prefix + "Foot");
        if (!upper || !lower || !foot)
            return std::nullopt;
        const std::string hip = prefix + "HipRigAttachment";
        const std::string knee = prefix + "KneeRigAttachment";
        const std::string ankle = prefix + "AnkleRigAttachment";
        const std::optional<float> values[] = {y(root, "RootRigAttachment"),
                                               y(lowerTorso, "RootRigAttachment"),
                                               y(lowerTorso, hip),
                                               y(upper, hip),
                                               y(upper, knee),
                                               y(lower, knee),
                                               y(lower, ankle),
                                               y(foot, ankle)};
        for (const auto& value : values)
            if (!value)
                return std::nullopt;
        return -*values[0] - 0.5f * root->size().y + *values[1] - *values[2] + *values[3] - *values[4] + *values[5]
               - *values[6] + *values[7] + 0.5f * foot->size().y;
    };
    const std::optional<float> left = side("Left");
    const std::optional<float> right = side("Right");
    if (!left || !right)
        return std::nullopt;
    constexpr float similar = 50.0f / 1000.0f;
    if (std::abs(*left - *right) < similar * std::max(*left, *right))
        return std::min(*left, *right);
    return std::max(*left, *right);
}

std::vector<Motor6D*> joinRigAttachments(Instance& character, BasePart& root)
{
    std::vector<BasePart*> parts;
    for (const InstancePtr& descendant : character.descendants())
        if (auto* part = instanceCast<BasePart>(descendant.get()))
            parts.push_back(part);
    std::vector<Motor6D*> joints;
    std::unordered_set<BasePart*> visited;
    joinFrom(root, parts, visited, joints);
    return joints;
}

std::vector<Motor6D*> buildRigFromAttachments(Instance& character)
{
    auto* root = instanceCast<BasePart>(character.findFirstChild("HumanoidRootPart"));
    if (!root)
        return {};
    auto* humanoid = instanceCast<Humanoid>(character.findFirstChildOfClass("Humanoid"));
    const bool scaling = humanoid && humanoid->automaticScalingEnabled();
    if (scaling)
        applyAutomaticScaling(character);
    // NOT DONE: the client measures both legs first and stretches the
    // shorter one when they differ (both over 0.1 studs).
    std::vector<Motor6D*> joints = joinRigAttachments(character, *root);
    if (scaling && humanoid->rigType() == Enum::HumanoidRigType::R15)
        if (std::optional<float> hipHeight = computeR15HipHeight(character);
            hipHeight && std::abs(*hipHeight - humanoid->hipHeight()) > 1e-5f * std::max(1.0f, *hipHeight))
            humanoid->setHipHeight(*hipHeight);
    return joints;
}

std::vector<Motor6D*> buildR6Joints(Instance& character)
{
    std::vector<Motor6D*> joints;
    auto part = [&](std::string_view name) { return instanceCast<BasePart>(character.findFirstChild(name)); };
    // The rotation table at 0x107ba38d0, rows of the matrix.
    auto rotation = [](Vector3 position, std::array<float, 9> m) {
        CFrame cf(position);
        for (std::size_t i = 0; i < 9; ++i)
            cf.r[i / 3][i % 3] = m[i];
        return cf;
    };
    constexpr std::array<float, 9> r0{0, 0, 1, 0, 1, 0, -1, 0, 0};
    constexpr std::array<float, 9> r1{-1, 0, 0, 0, 0, 1, 0, 1, 0};
    constexpr std::array<float, 9> r3{0, 0, -1, 0, 1, 0, 1, 0, 0};
    BasePart* root = part("HumanoidRootPart");
    BasePart* torso = part("Torso");
    if (!torso)
        return joints;
    if (root)
        addJoint(joints, *root, *torso, rotation({}, r1), rotation({}, r1), *root, "RootJoint");
    struct Limb {
        std::string_view joint;
        std::string_view part;
        Vector3 c0;
        Vector3 c1;
        const std::array<float, 9>& r;
    };
    const Limb limbs[] = {
        {"Right Shoulder", "Right Arm", {1.0f, 0.5f, 0.0f}, {-0.5f, 0.5f, 0.0f}, r0},
        {"Left Shoulder", "Left Arm", {-1.0f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}, r3},
        {"Right Hip", "Right Leg", {1.0f, -1.0f, 0.0f}, {0.5f, 1.0f, 0.0f}, r0},
        {"Left Hip", "Left Leg", {-1.0f, -1.0f, 0.0f}, {-0.5f, 1.0f, 0.0f}, r3},
        {"Neck", "Head", {0.0f, 1.0f, 0.0f}, {0.0f, -0.5f, 0.0f}, r1},
    };
    for (const Limb& limb : limbs)
        if (BasePart* p = part(limb.part))
            addJoint(joints, *torso, *p, rotation(limb.c0, limb.r), rotation(limb.c1, limb.r), *torso,
                     std::string(limb.joint));
    return joints;
}

} // namespace lunar::avatar
