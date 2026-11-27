#include <lunar/physics/Character.hpp>

#include <string>
#include <unordered_map>

namespace lunar::physics {

namespace {

struct PartSpec {
    std::string_view name;
    Vector3 size;
    Vector3 center; // feet at y = 0
    BrickColor color;
    bool collides;
};

struct JointSpec {
    std::string_view name;
    std::string_view part0;
    std::string_view part1;
    Vector3 at; // where the joint sits, character space
};

// Layout: legs 2 studs (the hip height), torso 2, head 1.2; arms hang at the sides.
constexpr BrickColor skin{24};   // Bright yellow
constexpr BrickColor shirt{23};  // Bright blue
constexpr BrickColor pants{119}; // Br. yellowish green

constexpr PartSpec parts[]{
    {"HumanoidRootPart", {2.0f, 2.0f, 1.0f}, {0.0f, 3.0f, 0.0f}, BrickColor{194}, true},
    {"LowerTorso", {2.0f, 0.4f, 1.0f}, {0.0f, 2.2f, 0.0f}, shirt, true},
    {"UpperTorso", {2.0f, 1.6f, 1.0f}, {0.0f, 3.2f, 0.0f}, shirt, true},
    {"Head", {1.2f, 1.2f, 1.2f}, {0.0f, 4.6f, 0.0f}, skin, true},
    {"LeftUpperArm", {1.0f, 1.0f, 1.0f}, {-1.5f, 3.5f, 0.0f}, skin, false},
    {"LeftLowerArm", {1.0f, 1.0f, 1.0f}, {-1.5f, 2.5f, 0.0f}, skin, false},
    {"LeftHand", {1.0f, 0.2f, 1.0f}, {-1.5f, 1.9f, 0.0f}, skin, false},
    {"RightUpperArm", {1.0f, 1.0f, 1.0f}, {1.5f, 3.5f, 0.0f}, skin, false},
    {"RightLowerArm", {1.0f, 1.0f, 1.0f}, {1.5f, 2.5f, 0.0f}, skin, false},
    {"RightHand", {1.0f, 0.2f, 1.0f}, {1.5f, 1.9f, 0.0f}, skin, false},
    {"LeftUpperLeg", {1.0f, 1.0f, 1.0f}, {-0.5f, 1.5f, 0.0f}, pants, false},
    {"LeftLowerLeg", {1.0f, 0.8f, 1.0f}, {-0.5f, 0.6f, 0.0f}, pants, false},
    {"LeftFoot", {1.0f, 0.2f, 1.0f}, {-0.5f, 0.1f, 0.0f}, pants, false},
    {"RightUpperLeg", {1.0f, 1.0f, 1.0f}, {0.5f, 1.5f, 0.0f}, pants, false},
    {"RightLowerLeg", {1.0f, 0.8f, 1.0f}, {0.5f, 0.6f, 0.0f}, pants, false},
    {"RightFoot", {1.0f, 0.2f, 1.0f}, {0.5f, 0.1f, 0.0f}, pants, false},
};

// Each Motor6D lives in its Part1, as in the R15 rig.
constexpr JointSpec joints[]{
    {"Root", "HumanoidRootPart", "LowerTorso", {0.0f, 2.2f, 0.0f}},
    {"Waist", "LowerTorso", "UpperTorso", {0.0f, 2.4f, 0.0f}},
    {"Neck", "UpperTorso", "Head", {0.0f, 4.0f, 0.0f}},
    {"LeftShoulder", "UpperTorso", "LeftUpperArm", {-1.5f, 3.8f, 0.0f}},
    {"LeftElbow", "LeftUpperArm", "LeftLowerArm", {-1.5f, 3.0f, 0.0f}},
    {"LeftWrist", "LeftLowerArm", "LeftHand", {-1.5f, 2.0f, 0.0f}},
    {"RightShoulder", "UpperTorso", "RightUpperArm", {1.5f, 3.8f, 0.0f}},
    {"RightElbow", "RightUpperArm", "RightLowerArm", {1.5f, 3.0f, 0.0f}},
    {"RightWrist", "RightLowerArm", "RightHand", {1.5f, 2.0f, 0.0f}},
    {"LeftHip", "LowerTorso", "LeftUpperLeg", {-0.5f, 2.0f, 0.0f}},
    {"LeftKnee", "LeftUpperLeg", "LeftLowerLeg", {-0.5f, 1.0f, 0.0f}},
    {"LeftAnkle", "LeftLowerLeg", "LeftFoot", {-0.5f, 0.2f, 0.0f}},
    {"RightHip", "LowerTorso", "RightUpperLeg", {0.5f, 2.0f, 0.0f}},
    {"RightKnee", "RightUpperLeg", "RightLowerLeg", {0.5f, 1.0f, 0.0f}},
    {"RightAnkle", "RightLowerLeg", "RightFoot", {0.5f, 0.2f, 0.0f}},
};

} // namespace

std::shared_ptr<Model> buildPlaceholderCharacter(std::string_view name)
{
    auto model = std::make_shared<Model>();
    model->setName(std::string(name));
    std::unordered_map<std::string_view, Part*> byName;
    for (const PartSpec& spec : parts) {
        auto part = std::make_shared<Part>();
        part->setName(std::string(spec.name));
        part->setSize(spec.size);
        part->setPosition(spec.center);
        part->setBrickColor(spec.color);
        part->setCanCollide(spec.collides);
        part->setTopSurface(Enum::SurfaceType::Smooth);
        part->setBottomSurface(Enum::SurfaceType::Smooth);
        if (spec.name == "HumanoidRootPart")
            part->setTransparency(1.0f);
        part->setParent(model.get());
        byName[spec.name] = part.get();
    }
    for (const JointSpec& spec : joints) {
        Part* p0 = byName.at(spec.part0);
        Part* p1 = byName.at(spec.part1);
        auto motor = std::make_shared<Motor6D>();
        motor->setName(std::string(spec.name));
        motor->setPart0(p0);
        motor->setPart1(p1);
        motor->setC0(CFrame(spec.at - p0->position()));
        motor->setC1(CFrame(spec.at - p1->position()));
        motor->setParent(p1);
    }
    auto humanoid = std::make_shared<Humanoid>();
    humanoid->setRigType(Enum::HumanoidRigType::R15);
    humanoid->setHipHeight(2.0f);
    humanoid->setParent(model.get());
    model->setPrimaryPart(byName.at("HumanoidRootPart"));
    return model;
}

} // namespace lunar::physics
