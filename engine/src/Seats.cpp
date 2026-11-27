#include <lunar/Seats.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

namespace lunar {

namespace {

// Both SeatWeld CFrames are turned this way (sub_1031abfc8: looking down
// with -Z up), so the root part stands upright above the seat.
CFrame seatTurn()
{
    return CFrame::angles(-std::numbers::pi_v<float> * 0.5f, 0.0f, 0.0f);
}

Instance* character(const Humanoid& humanoid)
{
    return humanoid.parent();
}

// A rig attachment's position in a part of the character, if both are there.
const Attachment* rigAttachment(const Humanoid& humanoid, std::string_view part, std::string_view name)
{
    Instance* model = character(humanoid);
    Instance* limb = model ? model->findFirstChild(part) : nullptr;
    return limb ? instanceCast<const Attachment>(limb->findFirstChild(name)) : nullptr;
}

void setOccupant(BasePart& seat, Humanoid* humanoid)
{
    if (auto* s = instanceCast<Seat>(&seat))
        s->assignOccupant(humanoid);
    else if (auto* v = instanceCast<VehicleSeat>(&seat))
        v->assignOccupant(humanoid);
}

} // namespace

bool isSeat(const Instance& instance)
{
    return instance.isA("Seat") || instance.isA("VehicleSeat");
}

bool seatDisabled(const BasePart& seat)
{
    if (auto* s = instanceCast<const Seat>(&seat))
        return s->disabled();
    if (auto* v = instanceCast<const VehicleSeat>(&seat))
        return v->disabled();
    return true;
}

Humanoid* seatOccupant(const BasePart& seat)
{
    if (auto* s = instanceCast<const Seat>(&seat))
        return s->occupant();
    if (auto* v = instanceCast<const VehicleSeat>(&seat))
        return v->occupant();
    return nullptr;
}

CFrame seatWeldC0(const BasePart& seat)
{
    return CFrame(0.0f, seat.size().y * 0.5f, 0.0f) * seatTurn();
}

CFrame seatWeldC1(const Humanoid& humanoid)
{
    float height = 0.0f;
    if (humanoid.rigType() == Enum::HumanoidRigType::R6) {
        // GUESS: the size the binary reads (+0x1c0 of the root part's
        // primitive) is the root part's height: 2 for R6, giving the
        // classic -1.5.
        const BasePart* root = humanoid.rootPart();
        height = -(std::max(root ? root->size().y : 2.0f, 0.05f) * 0.5f + 0.5f);
    } else {
        // From the root part down the rig to the bottom of the left thigh,
        // lying flat (its Z half-size is then its height).
        if (auto* a = rigAttachment(humanoid, "HumanoidRootPart", "RootRigAttachment"))
            height += a->position().y;
        if (auto* a = rigAttachment(humanoid, "LowerTorso", "RootRigAttachment"))
            height -= a->position().y;
        if (auto* a = rigAttachment(humanoid, "LowerTorso", "LeftHipRigAttachment"))
            height += a->position().y;
        if (auto* a = rigAttachment(humanoid, "LeftUpperLeg", "LeftHipRigAttachment"))
            height += a->position().z;
        Instance* model = character(humanoid);
        if (auto* thigh = model ? instanceCast<const BasePart>(model->findFirstChild("LeftUpperLeg")) : nullptr)
            height -= thigh->size().z * 0.5f;
    }
    return CFrame(0.0f, height, 0.0f) * seatTurn();
}

Weld* seatWeld(const BasePart& seat, const Humanoid& humanoid)
{
    const BasePart* root = humanoid.rootPart();
    for (const InstancePtr& child : seat.children())
        if (auto* weld = instanceCast<Weld>(child.get());
            weld && weld->name() == "SeatWeld" && weld->part0() == &seat && root && weld->part1() == root)
            return weld;
    return nullptr;
}

bool sit(BasePart& seat, Humanoid& humanoid)
{
    BasePart* root = humanoid.rootPart();
    if (!root || seatOccupant(seat))
        return false;
    if (humanoid.seatPart())
        leaveSeat(humanoid);
    // Into the seat at once, as a new weld snaps its Part1 into place: the
    // whole character moves with its root part.
    const CFrame seated = seat.cframe() * seatWeldC0(seat) * seatWeldC1(humanoid).inverse();
    if (auto* model = instanceCast<Model>(humanoid.parent()); model && model->primaryPart() == root)
        model->pivotTo(seated * root->cframe().inverse() * model->getPivot());
    else
        root->setCFrame(seated);
    root->setAssemblyLinearVelocity(seat.assemblyLinearVelocity());
    auto weld = std::make_shared<Weld>();
    weld->setName("SeatWeld");
    weld->setPart0(&seat);
    weld->setPart1(root);
    weld->setC0(seatWeldC0(seat));
    weld->setC1(seatWeldC1(humanoid));
    weld->setParent(&seat);
    setOccupant(seat, &humanoid);
    humanoid.assignSeatPart(&seat);
    humanoid.setSit(true);
    return true;
}

void leaveSeat(Humanoid& humanoid)
{
    BasePart* seat = humanoid.seatPart();
    if (seat) {
        // Hold on: destroying the weld can fire scripts that let go of the seat.
        const InstancePtr keep = seat->shared_from_this();
        while (Weld* weld = seatWeld(*seat, humanoid))
            weld->destroy();
        if (seatOccupant(*seat) == &humanoid)
            setOccupant(*seat, nullptr);
    }
    humanoid.assignSeatPart(nullptr);
    humanoid.setSit(false);
}

void driveVehicleSeat(VehicleSeat& seat, Vector3 move)
{
    // GUESS: the integer properties round the floats, as the VehicleController's settings read back.
    const float throttle = std::clamp(-move.z, -1.0f, 1.0f);
    const float steer = std::clamp(move.x, -1.0f, 1.0f);
    seat.setThrottleFloat(throttle);
    seat.setSteerFloat(steer);
    seat.setThrottle(static_cast<int>(std::lround(throttle)));
    seat.setSteer(static_cast<int>(std::lround(steer)));
}

} // namespace lunar
