// Instance userdata: properties through the engine's property system, methods,
// events and child lookup, plus the Instance global.
//
// Lookup order is the client's (__index, 0x101497f0e): members (properties,
// methods, events), then children, then members under the name with its
// first letter capitalised (part.position). Method calls go through
// __namecall (0x1014987d4), which only looks for methods.
// Members the real client has but the engine doesn't implement yet still
// resolve: properties read their type's default (and remember what scripts
// write), functions raise "<Class>:<Name> is not implemented in Lunarstone",
// events are ordinary signals nobody fires yet. Unknown names get the
// client's "%s is not a valid member of %s \"%s\"".

#include "InstanceApi.hpp"
#include "Characters.hpp"
#include "RobloxApi.hpp"
#include "Services.hpp"
#include "Userdata.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/PlaceFile.hpp>
#include <lunar/Seats.hpp>
#include <lunar/Selector.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace lunar::lua {

namespace {

InstancePtr* rawInstance(lua_State* L, int idx)
{
    return static_cast<InstancePtr*>(lua_touserdatatagged(L, idx, InstanceTag));
}

Instance* optInstance(lua_State* L, int idx)
{
    return lua_isnoneornil(L, idx) ? nullptr : &checkInstance(L, idx);
}

[[noreturn]] void notAMember(lua_State* L, const Instance& instance, std::string_view member)
{
    raise(L, std::format("{} is not a valid member of {} \"{}\"", member, instance.className(), instance.fullName()));
}

// --- Methods -----------------------------------------------------------------

int findFirstChild(lua_State* L)
{
    return returnInstance(L, self(L).findFirstChild(checkString(L, 2), lua_toboolean(L, 3)));
}

int findFirstChildOfClass(lua_State* L)
{
    return returnInstance(L, self(L).findFirstChildOfClass(checkString(L, 2)));
}

int findFirstChildWhichIsA(lua_State* L)
{
    return returnInstance(L, self(L).findFirstChildWhichIsA(checkString(L, 2), lua_toboolean(L, 3)));
}

int findFirstDescendant(lua_State* L)
{
    return returnInstance(L, self(L).findFirstChild(checkString(L, 2), true));
}

int findFirstAncestor(lua_State* L)
{
    return returnInstance(L, self(L).findFirstAncestor(checkString(L, 2)));
}

int findFirstAncestorOfClass(lua_State* L)
{
    return returnInstance(L, self(L).findFirstAncestorOfClass(checkString(L, 2)));
}

int findFirstAncestorWhichIsA(lua_State* L)
{
    return returnInstance(L, self(L).findFirstAncestorWhichIsA(checkString(L, 2)));
}

int getChildren(lua_State* L)
{
    return pushInstances(L, self(L).children());
}

int getDescendants(lua_State* L)
{
    return pushInstances(L, self(L).descendants());
}

// Instance:QueryDescendants(selector): see lunar/Selector.hpp.
int queryDescendantsMethod(lua_State* L)
{
    auto found = queryDescendants(self(L), checkString(L, 2));
    if (!found)
        raise(L, std::format("QueryDescendants: invalid selector: {}", found.error()));
    return pushInstances(L, *found);
}

int isAMethod(lua_State* L)
{
    lua_pushboolean(L, isA(self(L), checkString(L, 2)));
    return 1;
}

int isAncestorOf(lua_State* L)
{
    Instance& instance = self(L);
    lua_pushboolean(L, instance.isAncestorOf(optInstance(L, 2)));
    return 1;
}

int isDescendantOf(lua_State* L)
{
    Instance& instance = self(L);
    lua_pushboolean(L, instance.isDescendantOf(optInstance(L, 2)));
    return 1;
}

int getFullName(lua_State* L)
{
    pushString(L, self(L).fullName());
    return 1;
}

int destroy(lua_State* L)
{
    Instance& instance = self(L);
    guard(L, [&] { instance.destroy(); });
    return 0;
}

// Deprecated Remove: unparents the instance and its descendants stay intact.
int remove(lua_State* L)
{
    Instance& instance = self(L);
    guard(L, [&] { instance.setParent(nullptr); });
    return 0;
}

int clearAllChildren(lua_State* L)
{
    Instance& instance = self(L);
    guard(L, [&] { instance.clearAllChildren(); });
    return 0;
}

int clone(lua_State* L)
{
    InstancePtr copy = self(L).clone();
    return returnInstance(L, copy.get());
}

int getPropertyChangedSignal(lua_State* L)
{
    Instance& instance = self(L);
    std::string_view property = checkString(L, 2);
    if (!instance.classInfo().findProperty(property)) {
        const api::MemberData* member = api::findMember(instance.className(), property);
        if (!member || member->kind != api::MemberKind::Property)
            raise(L, std::format("{} is not a valid property name.", property));
    }
    pushPropertySignal(L, instance, std::string(property));
    return 1;
}

int getAttribute(lua_State* L)
{
    pushValue(L, self(L).getAttribute(checkString(L, 2)));
    return 1;
}

int setAttribute(lua_State* L)
{
    Instance& instance = self(L);
    std::string_view name = checkString(L, 2);
    std::optional<Value> value = toValue(L, 3);
    if (!value)
        raise(L, std::format("{} is not a supported attribute type", luaL_typename(L, 3)));
    guard(L, [&] { instance.setAttribute(name, *value); });
    return 0;
}

int getAttributes(lua_State* L)
{
    const auto& attributes = self(L).attributes();
    lua_createtable(L, 0, static_cast<int>(attributes.size()));
    for (const auto& [name, value] : attributes) {
        pushValue(L, value);
        lua_setfield(L, -2, name.c_str());
    }
    return 1;
}

int getAttributeChangedSignal(lua_State* L)
{
    Instance& instance = self(L);
    pushAttributeSignal(L, instance, std::string(checkString(L, 2)));
    return 1;
}

int addTag(lua_State* L)
{
    Instance& instance = self(L);
    addTagAndNotify(L, instance, checkString(L, 2));
    return 0;
}

int removeTag(lua_State* L)
{
    Instance& instance = self(L);
    removeTagAndNotify(L, instance, checkString(L, 2));
    return 0;
}

int hasTag(lua_State* L)
{
    lua_pushboolean(L, self(L).hasTag(checkString(L, 2)));
    return 1;
}

int getTags(lua_State* L)
{
    const auto& tags = self(L).tags();
    lua_createtable(L, static_cast<int>(tags.size()), 0);
    int i = 0;
    for (const std::string& tag : tags) {
        pushString(L, tag);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int getActor(lua_State* L)
{
    return returnInstance(L, self(L).findFirstAncestorOfClass("Actor"));
}

int getDebugId(lua_State* L)
{
    pushString(L, std::format("{:x}", reinterpret_cast<std::uintptr_t>(&self(L))));
    return 1;
}

// Returns the child right away if it's there, otherwise yields until a child
// with that name is added or the timeout runs out (then nil).
int waitForChild(lua_State* L)
{
    Instance& instance = self(L);
    std::string name(checkString(L, 2));
    if (Instance* child = instance.findFirstChild(name))
        return returnInstance(L, child);
    std::optional<double> timeout;
    if (!lua_isnoneornil(L, 3))
        timeout = luaL_checknumber(L, 3);
    checkYieldable(L);

    Runtime& rt = Runtime::of(L);
    const std::uint64_t token = rt.suspend(L);
    rt.attach(L, token, instance.childAdded.connect([&rt, L, token, name](Instance* child) {
        if (child->name() == name)
            rt.wakeWith(L, token, {packed(Value(child->shared_from_this()))}, "WaitForChild");
    }));
    if (timeout) {
        rt.after(*timeout, [&rt, L, token] {
            rt.wake(
                L, token,
                [](lua_State* thread) {
                    lua_pushnil(thread);
                    return 1;
                },
                "WaitForChild");
        });
    } else {
        rt.after(5.0, [&rt, L, token,
                       warning = std::format("Infinite yield possible on '{}:WaitForChild(\"{}\")'",
                                             instance.fullName(), name)] {
            if (rt.isSuspended(L, token))
                rt.output(MessageType::Warning, warning);
        });
    }
    return lua_yield(L, 0);
}

int getService(lua_State* L)
{
    Instance& instance = self(L);
    auto* provider = dynamic_cast<ServiceProvider*>(&instance);
    if (!provider)
        raise(L, "Expected ':' not '.' calling member function GetService");
    return returnInstance(L, findOrCreateService(L, *provider, checkString(L, 2), true));
}

int findService(lua_State* L)
{
    Instance& instance = self(L);
    auto* provider = dynamic_cast<ServiceProvider*>(&instance);
    if (!provider)
        raise(L, "Expected ':' not '.' calling member function FindService");
    return returnInstance(L, findOrCreateService(L, *provider, checkString(L, 2), false));
}

int isLoaded(lua_State* L)
{
    self(L);
    lua_pushboolean(L, true);
    return 1;
}

// --- Models and parts --------------------------------------------------------

int getPivot(lua_State* L)
{
    Instance& instance = self(L);
    if (auto* model = dynamic_cast<Model*>(&instance))
        pushCFrame(L, model->getPivot());
    else if (auto* part = dynamic_cast<BasePart*>(&instance))
        pushCFrame(L, part->getPivot());
    else
        pushCFrame(L, CFrame());
    return 1;
}

void traceNonFinite(lua_State* L, const Instance& instance, std::string_view key, const std::optional<Value>& value);

int pivotTo(lua_State* L)
{
    Instance& instance = self(L);
    const CFrame& target = checkCFrame(L, 2);
    traceNonFinite(L, instance, "PivotTo", Value(target));
    const BasePart::CarryAssembly carry;
    if (auto* model = dynamic_cast<Model*>(&instance))
        model->pivotTo(target);
    else if (auto* part = dynamic_cast<BasePart*>(&instance))
        part->pivotTo(target);
    return 0;
}

int getBoundingBox(lua_State* L)
{
    auto* model = dynamic_cast<Model*>(&self(L));
    auto [cframe, size] = model ? model->getBoundingBox() : std::pair<CFrame, Vector3>{};
    pushCFrame(L, cframe);
    pushVector3(L, size);
    return 2;
}

int getExtentsSize(lua_State* L)
{
    auto* model = dynamic_cast<Model*>(&self(L));
    pushVector3(L, model ? model->getExtentsSize() : Vector3{});
    return 1;
}

int setPrimaryPartCFrame(lua_State* L)
{
    auto* model = dynamic_cast<Model*>(&self(L));
    const CFrame& target = checkCFrame(L, 2);
    traceNonFinite(L, self(L), "SetPrimaryPartCFrame", Value(target));
    if (model && model->primaryPart())
        model->pivotTo(target * model->primaryPart()->pivotOffset());
    else if (model)
        raise(L,
              concat("Model:SetPrimaryPartCFrame() failed because no PrimaryPart has been ",
                     "set, or the PrimaryPart no longer exists. Please set ", "Model.PrimaryPart before using this."));
    return 0;
}

int getPrimaryPartCFrame(lua_State* L)
{
    auto* model = dynamic_cast<Model*>(&self(L));
    pushCFrame(L, model && model->primaryPart() ? model->primaryPart()->cframe() : CFrame());
    return 1;
}

// Workspace:BulkMoveTo(parts, cframes, mode): each part to its CFrame, as
// setting their CFrames one by one does; the mode only says which events
// fire, and no script sees them in between.
int bulkMoveTo(lua_State* L)
{
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    const int count = lua_objlen(L, 2);
    if (lua_objlen(L, 3) != count)
        raise(L, "BulkMoveTo: the part list and the CFrame list must be the same length");
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 2, i);
        auto* part = dynamic_cast<BasePart*>(toInstance(L, -1));
        lua_rawgeti(L, 3, i);
        const CFrame* cframe = toCFrame(L, -1);
        if (!part || !cframe)
            raise(L, part ? "BulkMoveTo: the CFrame list may only hold CFrames"
                          : "BulkMoveTo: the part list may only hold BaseParts");
        const BasePart::CarryAssembly carry;
        part->setCFrame(*cframe);
        lua_pop(L, 2);
    }
    return 0;
}

int moveTo(lua_State* L)
{
    auto* model = dynamic_cast<Model*>(&self(L));
    const Vector3 target = checkVector3(L, 2);
    if (model) {
        CFrame pivot = model->getPivot();
        pivot.position = target;
        model->pivotTo(pivot);
        // Obstructions (parts, terrain) push the model up onto them.
        Runtime& rt = Runtime::of(L);
        if (rt.shared().raycast)
            liftOutOfObstructions(*model,
                                  [&](const RaycastQuery& query) { return rt.shared().raycast(rt.game(), query); });
    }
    return 0;
}

int getScale(lua_State* L)
{
    auto* model = dynamic_cast<Model*>(&self(L));
    pushNumber(L, model ? static_cast<double>(model->scaleFactor()) : 1.0);
    return 1;
}

// Motor:SetDesiredAngle(angle): where the motor turns to (stepMotors), as
// setting DesiredAngle does. The R6 Animate script swings the limbs by it.
int motorSetDesiredAngle(lua_State* L)
{
    Instance& instance = self(L);
    const double angle = luaL_checknumber(L, 2);
    if (auto* motor = dynamic_cast<Motor*>(&instance)) {
        motor->setDesiredAngle(static_cast<float>(angle));
        turnMotor(Runtime::of(L).shared(), *motor);
    }
    return 0;
}

int scaleTo(lua_State* L)
{
    auto* model = dynamic_cast<Model*>(&self(L));
    const double scale = luaL_checknumber(L, 2);
    if (!(scale > 0.0) || !std::isfinite(scale))
        raise(L, "Model:ScaleTo() scale must be positive"); // GUESS: the client's wording
    if (model)
        model->scaleTo(static_cast<float>(scale));
    return 0;
}

int getMass(lua_State* L)
{
    auto* part = dynamic_cast<BasePart*>(&self(L));
    if (!part) {
        pushNumber(L, 0);
        return 1;
    }
    const Vector3 size = part->size();
    pushNumber(L, static_cast<double>(size.x * size.y * size.z) * 0.7);
    return 1;
}

// ParticleEmitter:Emit(count) and :Clear(), as the client does them: by
// firing OnEmitRequested (with the count, 16 by default) and
// OnClearRequested, which whatever simulates the particles listens to (and
// which the server replicates).
int particleEmit(lua_State* L)
{
    Instance& emitter = self(L);
    const double count = lua_isnoneornil(L, 2) ? 16.0 : luaL_checknumber(L, 2);
    const Value arguments[1] = {count};
    emitter.fireEvent("OnEmitRequested", arguments);
    return 0;
}

int particleClear(lua_State* L)
{
    self(L).fireEvent("OnClearRequested");
    return 0;
}

// Physics queries the physics side hasn't wired yet: nothing touches.
int emptyList(lua_State* L)
{
    self(L);
    lua_newtable(L);
    return 1;
}

// BasePart:GetJoints: the joints (welds, Motor6Ds, WeldConstraints) and
// constraints that name the part or one of its attachments, from wherever
// they are in the place: a seated character's SeatWeld is in the seat.
int getJoints(lua_State* L)
{
    auto* part = dynamic_cast<BasePart*>(&self(L));
    std::vector<InstancePtr> joints;
    Instance* top = part;
    while (top && top->parent())
        top = top->parent();
    if (part && top)
        for (const InstancePtr& d : top->descendants()) {
            const BasePart* p0 = nullptr;
            const BasePart* p1 = nullptr;
            if (auto* j = dynamic_cast<JointInstance*>(d.get())) {
                p0 = j->part0();
                p1 = j->part1();
            } else if (auto* w = dynamic_cast<WeldConstraint*>(d.get())) {
                p0 = w->part0();
                p1 = w->part1();
            } else if (auto* c = dynamic_cast<Constraint*>(d.get())) {
                p0 = c->attachment0() ? dynamic_cast<const BasePart*>(c->attachment0()->parent()) : nullptr;
                p1 = c->attachment1() ? dynamic_cast<const BasePart*>(c->attachment1()->parent()) : nullptr;
            }
            if (p0 == part || p1 == part)
                joints.push_back(d);
        }
    return pushInstances(L, joints);
}

// BasePart:GetRootPart(): the root of the part's assembly, picked as the
// physics picks it (World::makeAssembly): an anchored part, then
// RootPriority, then not Massless, then HumanoidRootPart, then the biggest.
// The joints are looked for in the part's outermost Model below the
// Workspace, else among the part's siblings and in the part (the camera's
// Popper asks for every part it hits, every frame: no whole-Workspace
// scans). GUESS: joints that hold a loose part sit there.
int getRootPart(lua_State* L)
{
    auto* part = dynamic_cast<BasePart*>(&self(L));
    if (!part)
        return returnInstance(L, nullptr);
    Instance* scope = nullptr;
    for (Instance* a = part->parent(); a && a->parent(); a = a->parent())
        if (dynamic_cast<Model*>(a) && !dynamic_cast<Workspace*>(a))
            scope = a;
    std::vector<std::pair<BasePart*, BasePart*>> links;
    auto look = [&](const Instance& joint) {
        if (auto* j = dynamic_cast<const JointInstance*>(&joint)) {
            if (j->enabled() && j->part0() && j->part1())
                links.emplace_back(j->part0(), j->part1());
        } else if (auto* w = dynamic_cast<const WeldConstraint*>(&joint)) {
            if (w->enabled() && w->part0() && w->part1())
                links.emplace_back(w->part0(), w->part1());
        }
    };
    if (scope) {
        for (const InstancePtr& d : scope->descendants())
            look(*d);
    } else {
        if (part->parent())
            for (const InstancePtr& c : part->parent()->children())
                look(*c);
        for (const InstancePtr& d : part->descendants())
            look(*d);
    }
    std::vector<BasePart*> members{part};
    for (std::size_t i = 0; i < members.size(); ++i)
        for (const auto& [a, b] : links) {
            BasePart* other = a == members[i] ? b : b == members[i] ? a : nullptr;
            if (other && std::ranges::find(members, other) == members.end())
                members.push_back(other);
        }
    auto rank = [](const BasePart* p) {
        const Vector3 size = p->size();
        return std::tuple(p->anchored(), p->rootPriority(), !p->massless(), p->name() == "HumanoidRootPart",
                          size.x * size.y * size.z);
    };
    return returnInstance(L, *std::ranges::max_element(members, {}, rank));
}

// Seat:Sit / VehicleSeat:Sit(humanoid).
int seatSit(lua_State* L)
{
    auto* seat = dynamic_cast<BasePart*>(&self(L));
    auto* humanoid = dynamic_cast<Humanoid*>(&checkInstance(L, 2));
    if (seat && humanoid)
        guard(L, [&] { sit(*seat, *humanoid); });
    return 0;
}

// Trail:Clear(): the segments drawn so far go (the renderer's Ribbons
// watch Trail::clears).
int trailClear(lua_State* L)
{
    if (auto* trail = dynamic_cast<Trail*>(&self(L)))
        trail->clear();
    return 0;
}

// Beam:SetTextureOffset(offset = 0): where the texture's scrolling is.
int beamSetTextureOffset(lua_State* L)
{
    const auto offset = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    if (auto* beam = dynamic_cast<Beam*>(&self(L)))
        beam->setTextureOffset(offset);
    return 0;
}

int breakJoints(lua_State* L)
{
    Instance& instance = self(L);
    for (const InstancePtr& d : instance.descendants())
        if (d->isA("JointInstance") || d->isA("WeldConstraint"))
            d->destroy();
    return 0;
}

constexpr auto instanceMethods = std::to_array<Method>({
    {"AddTag", "Instance", addTag},
    {"ClearAllChildren", "Instance", clearAllChildren},
    {"Clone", "Instance", clone},
    {"Destroy", "Instance", destroy},
    {"FindFirstAncestor", "Instance", findFirstAncestor},
    {"FindFirstAncestorOfClass", "Instance", findFirstAncestorOfClass},
    {"FindFirstAncestorWhichIsA", "Instance", findFirstAncestorWhichIsA},
    {"FindFirstChild", "Instance", findFirstChild},
    {"FindFirstChildOfClass", "Instance", findFirstChildOfClass},
    {"FindFirstChildWhichIsA", "Instance", findFirstChildWhichIsA},
    {"FindFirstDescendant", "Instance", findFirstDescendant},
    {"GetActor", "Instance", getActor},
    {"GetAttribute", "Instance", getAttribute},
    {"GetAttributeChangedSignal", "Instance", getAttributeChangedSignal},
    {"GetAttributes", "Instance", getAttributes},
    {"GetChildren", "Instance", getChildren},
    {"GetDebugId", "Instance", getDebugId},
    {"GetDescendants", "Instance", getDescendants},
    {"GetFullName", "Instance", getFullName},
    {"GetPropertyChangedSignal", "Instance", getPropertyChangedSignal},
    {"GetTags", "Instance", getTags},
    {"HasTag", "Instance", hasTag},
    {"IsA", "Instance", isAMethod},
    {"IsAncestorOf", "Instance", isAncestorOf},
    {"IsDescendantOf", "Instance", isDescendantOf},
    {"Remove", "Instance", remove},
    {"RemoveTag", "Instance", removeTag},
    {"SetAttribute", "Instance", setAttribute},
    {"WaitForChild", "Instance", waitForChild},
    {"QueryDescendants", "Instance", queryDescendantsMethod},
    // The deprecated lower-case spellings the client still binds.
    {"children", "Instance", getChildren},
    {"clone", "Instance", clone},
    {"destroy", "Instance", destroy},
    {"findFirstChild", "Instance", findFirstChild},
    {"getChildren", "Instance", getChildren},
    {"isA", "Instance", isAMethod},
    {"isDescendantOf", "Instance", isDescendantOf},
    {"remove", "Instance", remove},
    {"FindService", "ServiceProvider", findService},
    {"GetService", "ServiceProvider", getService},
    {"getService", "ServiceProvider", getService},
    {"service", "ServiceProvider", getService},
    {"IsLoaded", "DataModel", isLoaded},
    {"GetPivot", "PVInstance", getPivot},
    {"PivotTo", "PVInstance", pivotTo},
    {"GetBoundingBox", "Model", getBoundingBox},
    {"GetExtentsSize", "Model", getExtentsSize},
    {"GetPrimaryPartCFrame", "Model", getPrimaryPartCFrame},
    {"GetScale", "Model", getScale},
    {"ScaleTo", "Model", scaleTo},
    {"MoveTo", "Model", moveTo},
    {"BulkMoveTo", "WorldRoot", bulkMoveTo},
    {"SetPrimaryPartCFrame", "Model", setPrimaryPartCFrame},
    {"BreakJoints", "Model", breakJoints},
    {"BreakJoints", "BasePart", breakJoints},
    {"Clear", "Trail", trailClear},
    {"SetTextureOffset", "Beam", beamSetTextureOffset},
    {"GetMass", "BasePart", getMass},
    {"GetTouchingParts", "BasePart", emptyList},
    {"GetConnectedParts", "BasePart", emptyList},
    {"GetJoints", "BasePart", getJoints},
    {"GetRootPart", "BasePart", getRootPart},
    {"Sit", "Seat", seatSit},
    {"Sit", "VehicleSeat", seatSit},
    {"SetDesiredAngle", "Motor", motorSetDesiredAngle},
    {"Emit", "ParticleEmitter", particleEmit},
    {"Clear", "ParticleEmitter", particleClear},
});

// Every method, Instance's and the services', in one table with an index by name.
struct MethodTable {
    std::vector<Method> methods;
    std::vector<std::uint8_t> callCapability; // per method, from the API data
    std::vector<bool> consoleOnly;            // per method: consoleMethods()'
    std::unordered_map<std::string_view, std::vector<int>> byName;
};

const MethodTable& methodTable()
{
    static const MethodTable table = [] {
        MethodTable t;
        // Class-specific methods first: CollectionService:AddTag(instance, tag)
        // must win over Instance:AddTag(tag).
        t.methods.assign(toolMethods().begin(), toolMethods().end());
        for (std::span<const Method> more :
             {serviceMethods(), textChatMethods(), inputActionMethods(), emoteMethods(), socialMethods(), animationMethods(), humanoidMethods(), promptMethods(), appMethods(), localizationMethods(), styleMethods(), appHttpMethods()})
            t.methods.insert(t.methods.end(), more.begin(), more.end());
        for (const Method& m : instanceMethods)
            t.methods.push_back(m);
        t.consoleOnly.assign(t.methods.size(), false);
        t.methods.insert(t.methods.end(), consoleMethods().begin(), consoleMethods().end());
        t.consoleOnly.resize(t.methods.size(), true);
        for (std::size_t i = 0; i < t.methods.size(); ++i) {
            t.byName[t.methods[i].name].push_back(static_cast<int>(i));
            const api::MemberData* member = api::findMember(t.methods[i].requiredClass, t.methods[i].name);
            t.callCapability.push_back(member ? member->readCapability : 0);
        }
        return t;
    }();
    return table;
}

// Index into the method table of the method the instance has under that name,
// as the thread asking sees it: the console's own are no member to the game's.
std::optional<int> findMethod(lua_State* L, const Instance& instance, std::string_view name)
{
    const MethodTable& table = methodTable();
    auto it = table.byName.find(name);
    if (it == table.byName.end())
        return std::nullopt;
    for (int i : it->second) {
        const auto at = static_cast<std::size_t>(i);
        if (!isA(instance, table.methods[at].requiredClass))
            continue;
        if (table.consoleOnly[at] && !Runtime::of(L).accessOf(L).console)
            continue;
        return i;
    }
    return std::nullopt;
}

std::string capitalised(std::string_view name)
{
    if (name.empty() || name.front() < 'a' || name.front() > 'z')
        return {};
    std::string result(name);
    result.front() = static_cast<char>(result.front() - 'a' + 'A');
    return result;
}

// --- Security -----------------------------------------------------------------

// What the client says when a member needs a capability the thread lacks
// (0x10459e2a4 -> 0x1016fac20): "The current thread cannot read
// 'RobloxLocked' (lacking capability Plugin)". The verbs are the callers':
// "read" (0x1014a4623), "write" (0x10149e942), "call" (0x101496c34) [code].
// The game's own scripts run with none of these capabilities; the menu's Luau
// tab runs with all of them, and only that code can reach these members.
void checkCapability(lua_State* L, std::uint8_t capability, std::string_view verb, std::string_view member)
{
    if (!Runtime::of(L).accessOf(L).has(capability))
        raise(L, std::format("The current thread cannot {} '{}' (lacking capability {})", verb, member,
                             api::capabilityName(capability)));
}

// The API data's entry for a property the engine implements, which says what
// reading and writing it need.
const api::MemberData* apiMemberOf(const Instance& instance, const Property& property)
{
    static std::unordered_map<const Property*, const api::MemberData*> members;
    auto it = members.find(&property);
    if (it == members.end())
        it = members.emplace(&property, api::findMember(instance.className(), property.name)).first;
    return it->second;
}

std::uint8_t readCapability(const Instance& instance, const Property& property)
{
    const api::MemberData* member = apiMemberOf(instance, property);
    return member ? member->readCapability : 0;
}

std::uint8_t writeCapability(const Instance& instance, const Property& property)
{
    const api::MemberData* member = apiMemberOf(instance, property);
    return member ? member->writeCapability : 0;
}

// --- Members the engine doesn't implement --------------------------------------

std::map<std::string, Packed, std::less<>>* extraValues(lua_State* L, const Instance& instance, bool create)
{
    auto& values = Runtime::of(L).shared().state->extraProperties;
    auto it = values.find(&instance);
    if (it != values.end() && it->second.first.lock().get() != &instance) {
        values.erase(it);
        it = values.end();
    }
    if (it == values.end()) {
        if (!create)
            return nullptr;
        if (values.size() > 1024)
            std::erase_if(values, [](const auto& item) { return item.second.first.expired(); });
        it = values
                 .emplace(&instance, std::pair{std::const_pointer_cast<Instance>(instance.shared_from_this()),
                                               std::map<std::string, Packed, std::less<>>{}})
                 .first;
    }
    return &it->second.second;
}

// The default a property of this Luau type reads as.
void pushDefault(lua_State* L, std::string_view type)
{
    if (type == "boolean")
        lua_pushboolean(L, false);
    else if (type == "number")
        lua_pushnumber(L, 0);
    else if (type == "string" || type == "Content")
        lua_pushliteral(L, "");
    else if (type == "Vector3")
        pushVector3(L, {});
    else if (type == "CFrame")
        pushCFrame(L, CFrame());
    else if (type == "Color3")
        pushColor3(L, {});
    else if (type == "Vector2")
        push<Vector2>(L, {});
    else if (type == "UDim2")
        push<UDim2>(L, {});
    else if (type == "UDim")
        push<UDim>(L, {});
    else if (type == "BrickColor")
        push<BrickColor>(L, {});
    else if (type == "NumberRange")
        push<NumberRange>(L, {});
    else if (type == "Rect")
        push<Rect>(L, {});
    else if (type == "NumberSequence")
        push<NumberSequence>(L, NumberSequence{});
    else if (type == "ColorSequence")
        push<ColorSequence>(L, ColorSequence{});
    else if (type == "Font")
        push<Font>(L, Font{});
    else if (type.starts_with("Enum.")) {
        const EnumCatalogEntry* entry = findEnumEntry(type.substr(5));
        if (entry && !entry->items.empty())
            pushEnumItem(L, entry->items.front());
        else
            lua_pushnil(L);
    } else
        lua_pushnil(L);
}

// Legacy names Roblox's loader still takes a value from but has no getter
// for, so that not even setscriptable makes them readable [sUNC: "Shouldn't
// be able to access siz" once it is set scriptable, where formFactorRaw is
// read]. What they load is another property's, which gethiddenproperty reads
// them as [Myriad: gethiddenproperty(part, "siz") is part.Size, hidden;
// myriad.tools: 30 of the 36 executors at 125/140 or better pass it]. GUESS:
// siz is the oldest name a part's size was saved under; it is the only one
// known.
std::optional<std::string_view> writeOnlyMember(const Instance& instance, std::string_view name)
{
    if (name == "siz" && instance.isA("BasePart"))
        return "Size";
    return std::nullopt;
}

} // namespace

bool hiddenMember(const Instance& instance, std::string_view name)
{
    const api::MemberData* member = api::findMember(instance.className(), name);
    return member && (!member->scriptable || member->readCapability != 0);
}

namespace {

// Whether indexing reaches the member: a property Roblox keeps from scripts
// (NotScriptable) isn't one to them, or to the console, unless setscriptable
// made it so on this instance; the lower-case spellings of such members then
// fall through to the capitalised name, as the client's lookup does (part.size
// is Size).
bool scriptableHere(lua_State* L, const Instance& instance, const api::MemberData* member)
{
    return !member || member->kind != api::MemberKind::Property || member->scriptable
           || Runtime::of(L).scriptableOverride(instance, member->name);
}

} // namespace

// The name a value of a member the engine doesn't implement is kept under:
// Fire's and Smoke's saved names (size_xml) and old spellings (size) are the
// same value as the scripted one (Size) [sUNC: after sethiddenproperty(fire,
// "size_xml", 1), fire.Size is what gethiddenproperty reads].
namespace {

std::string_view storedName(const Instance& instance, std::string_view name)
{
    struct Entry {
        std::string_view className;
        std::string_view name;
        std::string_view stored;
    };
    for (const Entry& entry : {
             Entry{"Fire", "size_xml", "Size"}, Entry{"Fire", "size", "Size"}, Entry{"Fire", "heat_xml", "Heat"},
             Entry{"Fire", "heat", "Heat"}, Entry{"Smoke", "size_xml", "Size"},
             Entry{"Smoke", "opacity_xml", "Opacity"}, Entry{"Smoke", "riseVelocity_xml", "RiseVelocity"},
         })
        if (entry.name == name && entry.className == instance.className())
            return entry.stored;
    return name;
}

} // namespace

// What those members of Fire and Smoke hold when nothing has set them: the
// engine has only placeholders for the two classes, so these are the values
// the client itself starts them at. Taken by the name storedName gives.
// GUESS: the values themselves. The dump gives the members' names and types and
// nothing else; what is here is what Fire and Smoke are documented to start at.
std::optional<Value> hiddenDefault(std::string_view className, std::string_view name)
{
    struct Entry {
        std::string_view className;
        std::string_view name;
        double value;
    };
    for (const Entry& entry : {
             Entry{"Fire", "Size", 5}, Entry{"Fire", "Heat", 5}, Entry{"Smoke", "Size", 1},
             Entry{"Smoke", "Opacity", 1}, Entry{"Smoke", "RiseVelocity", 1.5},
         })
        if (entry.className == className && entry.name == name)
            return Value(entry.value);
    return std::nullopt;
}

namespace {

// A function of the real client Lunarstone doesn't implement: it checks self
// like any method, then says so.
int notImplemented(lua_State* L)
{
    Instance& instance = self(L);
    raise(L, std::format("{}:{} is not implemented in Lunarstone", instance.className(), methodName(L)));
}

// Its closure, one per member: like the client's methods, indexing it twice
// gives the same function (scripts cache methods and compare them).
void pushNotImplemented(lua_State* L, const api::MemberData& member)
{
    Runtime& rt = Runtime::of(L);
    lua_getref(L, rt.unimplementedMethods);
    lua_pushlightuserdata(L, const_cast<api::MemberData*>(&member));
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        const std::string_view owner = api::declaringClass(member);
        lua_pushlstring(L, member.name.data(), member.name.size());
        lua_pushlstring(L, owner.data(), owner.size());
        lua_pushinteger(L, member.readCapability);
        lua_pushcclosure(L, notImplemented, lua_tostring(L, -3), 3);
        lua_pushlightuserdata(L, const_cast<api::MemberData*>(&member));
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);
}

void noteUnimplemented(lua_State* L, const Instance& instance, std::string_view member)
{
    static std::set<std::string, std::less<>> reported;
    std::string key = std::format("{}.{}", instance.className(), member);
    if (reported.insert(key).second)
        Runtime::of(L).notice(MessageType::Info, std::format("Lunarstone: {} isn't implemented yet", key));
}

// --- Metamethods -------------------------------------------------------------

// Old scripts spell members with a lowercase first letter (script.parent,
// part.brickColor, obj:findFirstChild) and still run in the client, which
// tries the capitalised name when the exact one is neither a member nor a
// child [code: 0x101497f0e, 0x1014987d4].
// Old names the client still answers to for renamed properties [live: The
// Mimic's head turning reads Camera.CoordinateFrame].
struct PropertyAlias {
    std::string_view className;
    std::string_view old;
    std::string_view current;
};
constexpr std::array propertyAliases{
    PropertyAlias{"Camera", "CoordinateFrame", "CFrame"},
    PropertyAlias{"Player", "userId", "UserId"},
    PropertyAlias{"Sound", "Pitch", "PlaybackSpeed"},
};

// The property by its name or an old one.
const Property* findNamedProperty(const Instance& instance, std::string_view key)
{
    if (const Property* property = instance.classInfo().findProperty(key))
        return property;
    for (const PropertyAlias& alias : propertyAliases)
        if (alias.old == key && instance.isA(alias.className))
            return instance.classInfo().findProperty(alias.current);
    return nullptr;
}

const Property* findProperty(const Instance& instance, std::string_view key)
{
    if (const Property* property = findNamedProperty(instance, key))
        return property;
    const std::string upper = capitalised(key);
    return upper.empty() ? nullptr : instance.classInfo().findProperty(upper);
}

// The same for members only the API data knows (classes and properties
// Lunarstone doesn't implement), for assignment.
const api::MemberData* findApiMember(const Instance& instance, std::string_view key)
{
    if (const api::MemberData* member = api::findMember(instance.className(), key))
        return member;
    const std::string upper = capitalised(key);
    const api::MemberData* member = upper.empty() ? nullptr : api::findMember(instance.className(), upper);
    return member && member->kind == api::MemberKind::Property ? member : nullptr;
}

// research: LUNAR_TRACE_SCRIPT=<part of a script's full name> prints, once
// each, the members code from scripts so named reads (methods and events
// included) and assigns on instances: what a script looks at, obfuscated or
// not. Matched on the chunks of the nearest four calling functions, so a
// module's code counts whichever thread runs it.
void traceMember(lua_State* L, const Instance& instance, std::string_view key, bool assign)
{
    static const char* const filter = std::getenv("LUNAR_TRACE_SCRIPT");
    if (!filter || !*filter)
        return;
    lua_Debug ar{};
    bool from = false;
    for (int level = 1; level <= 4 && !from; ++level)
        from = lua_getinfo(L, level, "s", &ar) && ar.source && std::string_view(ar.source).contains(filter);
    if (!from)
        return;
    static std::set<std::string, std::less<>> seen;
    std::string line = std::format("{}{}.{}", assign ? "set " : "", instance.className(), key);
    // LUNAR_TRACE_ALL: every access, with the instance's name and the line
    // (as many as it says, 20000 when it says no number).
    static const char* const allValue = std::getenv("LUNAR_TRACE_ALL");
    static const bool all = allValue != nullptr;
    // With LUNAR_TRACE_CONSOLE too, in order with the console's trace.
    static const bool inOutput = std::getenv("LUNAR_TRACE_CONSOLE") != nullptr;
    static long left = all && std::atol(allValue) > 1 ? std::atol(allValue) : 20000;
    if (all && left-- > 0)
        line = std::format("trace: {} {} :{}", line, instance.name(), lua_getinfo(L, 1, "l", &ar) ? ar.currentline : -1);
    else if (!all && seen.insert(line).second)
        line = std::format("trace: {}", line);
    else
        return;
    if (inOutput)
        Runtime::of(L).notice(MessageType::Info, line);
    else
        std::fprintf(stderr, "%s\n", line.c_str());
}

// research: LUNAR_TRACE_NONFINITE=1 logs where a script assigns a CFrame or
// Vector3 that isn't finite (NaN or infinite) to an instance's property, or
// moves a model or part to one (PivotTo, SetPrimaryPartCFrame), with the Luau
// stack, once a place in the code: what sends a part to nowhere.
void traceNonFinite(lua_State* L, const Instance& instance, std::string_view key, const std::optional<Value>& value)
{
    static const bool on = std::getenv("LUNAR_TRACE_NONFINITE") != nullptr;
    if (!on || !value)
        return;
    auto finite = [](float f) { return std::isfinite(f); };
    bool bad = false;
    if (const auto* cf = std::get_if<CFrame>(&*value)) {
        bad = !finite(cf->position.x) || !finite(cf->position.y) || !finite(cf->position.z);
        for (const auto& row : cf->r)
            bad = bad || !std::ranges::all_of(row, finite);
    } else if (const auto* v = std::get_if<Vector3>(&*value)) {
        bad = !finite(v->x) || !finite(v->y) || !finite(v->z);
    }
    if (!bad)
        return;
    static std::set<std::string, std::less<>> seen;
    const std::string stack = lua_debugtrace(L);
    if (!seen.insert(std::string(key) + stack).second)
        return;
    std::fprintf(stderr, "nonfinite: %s.%s (%s) set to %s\n%s\n", std::string(instance.className()).c_str(), std::string(key).c_str(),
                 instance.fullName().c_str(), toString(*value).c_str(), stack.c_str());
}

// Pushes the instance's member called exactly `key` (a property, method or
// event, or a member only the API data knows) and returns true, or returns
// false if it has none. Properties only the API data knows count only with
// `apiProperties`: Lunarstone doesn't know their values, and a child of that
// name is the better answer (game.Workspace is the DataModel's Workspace
// property, which the engine doesn't model).
bool pushMember(lua_State* L, Instance& instance, std::string_view key, bool apiProperties = true)
{
    Runtime& rt = Runtime::of(L);
    if (const Property* property = findNamedProperty(instance, key)) {
        if (!scriptableHere(L, instance, apiMemberOf(instance, *property)))
            return false;
        checkCapability(L, readCapability(instance, *property), "read", property->name);
        if (rt.shared().guiLayoutQuery && (key.starts_with("Absolute") || key == "TextBounds" || key == "TextFits" || key == "CurrentPage")
            && (instance.isA("GuiBase2d") || instance.isA("UIGridStyleLayout"))) rt.shared().guiLayoutQuery(&instance);
        // A server has no LocalPlayer.
        if (key == "LocalPlayer" && rt.role() == ScriptRole::Server && instance.isA("Players")) {
            lua_pushnil(L);
            return true;
        }
        pushValue(L, guard(L, [&] { return property->get(instance); }));
        return true;
    }
    if (auto method = findMethod(L, instance, key)) {
        lua_getref(L, rt.instanceMethods);
        lua_rawgeti(L, -1, *method + 1);
        lua_remove(L, -2);
        return true;
    }
    if (int event = findBuiltinEvent(key); event >= 0) {
        pushSignal(L, instance, event);
        return true;
    }
    const api::MemberData* member = api::findMember(instance.className(), key);
    if (!member || (member->kind == api::MemberKind::Property && !apiProperties))
        return false;
    switch (member->kind) {
    case api::MemberKind::Event:
        pushHubSignal(L, instance, std::string(key));
        return true;
    case api::MemberKind::Property:
        if (!member->scriptable) {
            if (!scriptableHere(L, instance, member))
                return false;
            if (writeOnlyMember(instance, member->name))
                raise(L, std::format("{} is not a valid member of {} \"{}\"", member->name, instance.className(),
                                     instance.name()));
            bool hidden = false;
            readHiddenMember(L, instance, member->name, hidden);
            return true;
        }
        checkCapability(L, member->readCapability, "read", member->name);
        if (key == "IsLoaded" && (instance.isA("ImageLabel") || instance.isA("ImageButton"))) {
            const std::string content = std::get<std::string>(findNamedProperty(instance, "Image")->get(instance));
            auto& shared = rt.shared();
            const bool loaded = shared.imageLoaded && !content.empty() && shared.imageLoaded(content);
            auto& observed = shared.imageStates[&instance];
            if (observed.instance.lock().get() != &instance)
                observed = {instance.shared_from_this(), content, loaded};
            lua_pushboolean(L, loaded);
            return true;
        }
        if (auto* values = extraValues(L, instance, false); values && values->contains(storedName(instance, member->name))) {
            unpack(L, values->find(storedName(instance, member->name))->second);
            return true;
        }
        if (instance.className() == "PlayerMouse" && pushMouseRay(L, instance, member->name))
            return true;
        // game.workspace, the old spelling, has no child of its name to find.
        if (member->name == "workspace" && instance.isA("DataModel")) {
            returnInstance(L, instance.findFirstChildOfClass("Workspace"));
            return true;
        }
        noteUnimplemented(L, instance, key);
        if (std::optional<Value> value = hiddenDefault(instance.className(), storedName(instance, member->name)))
            pushValue(L, *value);
        else
            pushDefault(L, member->type);
        return true;
    case api::MemberKind::Function:
    case api::MemberKind::YieldFunction:
        noteUnimplemented(L, instance, key);
        pushNotImplemented(L, *member);
        return true;
    case api::MemberKind::Callback:
        raise(L, std::format("{} is a callback member of {}; {}", key, instance.className(),
                             "you can only set the callback value, get is not available"));
    }
    return false;
}

// The instance a metamethod was called on. Its error has no position in
// front, like the client's own: called bare (pcall(mt.__namecall)), the
// message is the same whatever called it, a hook on the metamethod in
// between included [sUNC: a game script compares it frame to frame].
Instance& checkSelf(lua_State* L)
{
    if (Instance* instance = toInstance(L, 1))
        return *instance;
    if (lua_isnone(L, 1))
        raise(L, "missing argument #1 (Instance expected)");
    raise(L, std::format("invalid argument #1 (Instance expected, got {})", luaL_typename(L, 1)));
}

int indexImpl(lua_State* L)
{
    Instance& instance = checkSelf(L);
    std::string_view key = checkString(L, 2);
    traceMember(L, instance, key, false);
    if (pushMember(L, instance, key, false))
        return 1;
    if (Instance* child = instance.findFirstChild(key))
        return returnInstance(L, child);
    if (pushMember(L, instance, key))
        return 1;
    if (const std::string upper = capitalised(key); !upper.empty() && pushMember(L, instance, upper))
        return 1;
    notAMember(L, instance, key);
}

// obj:Method(...): the client's __namecall resolves only methods (by the
// exact name, then capitalised) and runs them in its own frame; anything
// else is "not a valid member", whatever a child or property of that name.
// Called directly instead of by a method call, there is no name: the
// client's 0x1014987d4 then raises "Trying to call method on object of
// type: `<Class>` with incorrect arguments." [code]
int namecallImpl(lua_State* L)
{
    Instance& instance = checkSelf(L);
    const char* atom = lua_namecallatom(L, nullptr);
    if (!atom)
        raise(L, std::format("Trying to call method on object of type: `{}` with incorrect arguments.",
                             instance.className()));
    const std::string_view name = atom;
    traceMember(L, instance, name, false);
    std::optional<int> method = findMethod(L, instance, name);
    const std::string upper = method ? std::string() : capitalised(name);
    if (!method && !upper.empty())
        method = findMethod(L, instance, upper);
    if (method) {
        const MethodTable& table = methodTable();
        const auto i = static_cast<std::size_t>(*method);
        checkCapability(L, table.callCapability[i], "call", table.methods[i].name);
        return table.methods[i].function(L);
    }
    // Functions only the API data knows.
    const auto function = [&](std::string_view key) {
        const api::MemberData* member = key.empty() ? nullptr : api::findMember(instance.className(), key);
        const bool callable
            = member && (member->kind == api::MemberKind::Function || member->kind == api::MemberKind::YieldFunction);
        return callable ? member : nullptr;
    };
    const api::MemberData* member = function(name);
    if (!member)
        member = function(upper);
    if (member) {
        checkCapability(L, member->readCapability, "call", member->name);
        noteUnimplemented(L, instance, member->name);
        raise(L, std::format("{}:{} is not implemented in Lunarstone", instance.className(), member->name));
    }
    notAMember(L, instance, name);
}

int newIndexImpl(lua_State* L)
{
    Instance& instance = checkSelf(L);
    std::string_view key = checkString(L, 2);
    traceMember(L, instance, key, true);

    const Property* property = findProperty(instance, key);
    if (property && !scriptableHere(L, instance, apiMemberOf(instance, *property)))
        notAMember(L, instance, key);
    if (!property) {
        const api::MemberData* member = findApiMember(instance, key);
        if (member && member->kind == api::MemberKind::Callback) {
            setCallback(L, instance, key, 3);
            return 0;
        }
        if (member && member->kind == api::MemberKind::Property && !member->scriptable) {
            if (!scriptableHere(L, instance, member))
                notAMember(L, instance, key);
            writeHiddenMember(L, instance, member->name, 3);
            return 0;
        }
        if (member && member->kind == api::MemberKind::Property) {
            checkCapability(L, member->writeCapability, "write", member->name);
            if (member->name == "IsLoaded" && (instance.isA("ImageLabel") || instance.isA("ImageButton")))
                raise(L, "Unable to assign property IsLoaded. Property is read only");
            noteUnimplemented(L, instance, key);
            auto* values = extraValues(L, instance, true);
            values->insert_or_assign(std::string(storedName(instance, member->name)), pack(L, 3));
            instance.changed.fire(member->name);
            return 0;
        }
        notAMember(L, instance, key);
    }
    // The client's setter checks the capability first (0x10149e8f9).
    checkCapability(L, writeCapability(instance, *property), "write", property->name);
    if (property->readOnly())
        raise(L, std::format("Unable to assign property {}. Property is read only", property->name));
    std::optional<Value> value = toValue(L, 3);
    traceNonFinite(L, instance, key, value);
    // Like Lua's own coercions: numbers assigned to string properties become
    // their text (label.Text = 5), numeric strings assigned to numbers their value.
    if (property->type == "string" && lua_type(L, 3) == LUA_TNUMBER) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, 3, &length);
        value = Value(std::string(text, length));
        lua_pop(L, 1);
    } else if (property->type == "number" && lua_type(L, 3) == LUA_TSTRING && lua_isnumber(L, 3)) {
        value = Value(lua_tonumber(L, 3));
    } else if (property->type == "boolean") {
        // Booleans take any value by its truth: Roblox's own ControlModule
        // sets Humanoid.Jump to `false or nil` every frame on a desktop.
        // GUESS: values other than nil and booleans count as true as in Lua.
        value = Value(lua_toboolean(L, 3) != 0);
    }
    if (!value) {
        auto* other = toInstance(L, 3);
        raise(L, std::format("Unable to assign property {}. {} expected, got {}", property->name, property->type,
                             other ? other->className() : luaL_typename(L, 3)));
    }
    // A script's CFrame moves the part's whole assembly (BasePart::CarryAssembly).
    std::optional<BasePart::CarryAssembly> carry;
    if (property->name == "CFrame" && dynamic_cast<BasePart*>(&instance))
        carry.emplace();
    guard(L, [&] { property->set(instance, *value); });
    // A motor turns towards a DesiredAngle set either way.
    if (property->name == "DesiredAngle" && dynamic_cast<Motor*>(&instance))
        turnMotor(Runtime::of(L).shared(), instance);
    return 0;
}

// research: under LUNAR_TRACE_SLOW, each member's reads, writes and calls are
// timed (inclusive of the Lua they set off) for the slow-resume report.
struct NativeTimer {
    lua_State* L;
    const char* kind;
    bool on = slowTraceOn();
    double started = on ? slowClock() : 0.0;
    NativeTimer(lua_State* state, const char* what) : L(state), kind(what) {}
    NativeTimer(const NativeTimer&) = delete;
    NativeTimer& operator=(const NativeTimer&) = delete;
    ~NativeTimer()
    {
        if (!on)
            return;
        const Instance* instance = toInstance(L, 1);
        const char* member = kind[0] == 'c' ? lua_namecallatom(L, nullptr) : lua_tostring(L, 2);
        noteSlowNative(std::format("{} {}.{}", kind, instance ? instance->className() : "?", member ? member : "?"),
                       slowClock() - started);
    }
};

int index(lua_State* L)
{
    const NativeTimer timer(L, "get");
    return indexImpl(L);
}

int namecall(lua_State* L)
{
    const NativeTimer timer(L, "call");
    return namecallImpl(L);
}

int newIndex(lua_State* L)
{
    const NativeTimer timer(L, "set");
    return newIndexImpl(L);
}

int toString(lua_State* L)
{
    pushString(L, checkInstance(L, 1).name());
    return 1;
}

int instanceNew(lua_State* L)
{
    std::string_view className = checkString(L, 1);
    Instance* parent = optInstance(L, 2);
    InstancePtr instance;
    const ClassInfo* info = ClassRegistry::find(className);
    // A Player only a thread that may write players makes: identity 8 and not 6
    // [sUNC's setthreadidentity: "capability[3]"]. GUESS: that WritePlayer is
    // the capability, the one 8 has and 6 doesn't that names players.
    constexpr std::uint8_t writePlayer = 3;
    if (info && (info->kind == ClassKind::Creatable
                 || (className == "Player" && info->factory && Runtime::of(L).accessOf(L).has(writePlayer)))) {
        instance = info->factory();
    } else if (const api::ClassData* data = api::findClass(className);
               data && (data->flags & api::ClassCreatable) != 0 && !info) {
        // Creatable in Roblox but not modelled yet: a placeholder keeps the tree's shape.
        instance = std::make_shared<UnknownInstance>(ClassRegistry::placeholder(className));
    } else {
        raise(L, std::format("Unable to create an Instance of type \"{}\"", className));
    }
    if (parent)
        guard(L, [&] { instance->setParent(parent); });
    return returnInstance(L, instance.get());
}

// Instance.fromExisting(existing): a new instance of its class with its
// property values, nothing under it and no parent. GUESS: the error for a
// class that can't be copied.
int instanceFromExisting(lua_State* L)
{
    const Instance& existing = checkInstance(L, 1);
    InstancePtr made = existing.copy();
    if (!made)
        raise(L, std::format("Unable to create an Instance of type \"{}\"", existing.className()));
    return returnInstance(L, made.get());
}

} // namespace

// The console's gethiddenproperty: the member by name, with no capability
// check -- reading what the game's scripts could not reach is the point -- and
// whether it is such a member. A member the engine has no value of its own for
// reads as what Roblox starts it at, or as the type's zero where there is no
// better answer.
void readHiddenMember(lua_State* L, Instance& instance, std::string_view name, bool& hidden)
{
    hidden = hiddenMember(instance, name);
    if (const auto loads = writeOnlyMember(instance, name))
        name = *loads;
    const api::MemberData* member = api::findMember(instance.className(), name);
    // The engine's own property, or the one the name is saved as (Health_XML
    // is Health, formFactorRaw a part's FormFactor); a name nothing knows is
    // taken as indexing takes it, capitalised.
    const Property* property = findNamedProperty(instance, name);
    if (!property)
        property = findSerializedProperty(instance, name);
    if (!property && !member)
        property = findProperty(instance, name);
    if (property && property->get) {
        pushValue(L, guard(L, [&] { return property->get(instance); }));
        return;
    }
    if (!member)
        notAMember(L, instance, name);
    const std::string_view stored = storedName(instance, member->name);
    if (auto* values = extraValues(L, instance, false); values && values->contains(stored)) {
        unpack(L, values->find(stored)->second);
        return;
    }
    if (const Value* kept = instance.unmodelled(member->name))
        pushValue(L, *kept);
    else if (std::optional<Value> value = hiddenDefault(instance.className(), stored))
        pushValue(L, *value);
    else
        pushDefault(L, member->type);
}

// The console's sethiddenproperty: the same member, assigned the value at idx,
// with the coercions newIndex gives and no capability check.
void writeHiddenMember(lua_State* L, Instance& instance, std::string_view name, int idx)
{
    const Property* property = findNamedProperty(instance, name);
    if (!property)
        property = findSerializedProperty(instance, name);
    if (!property && !api::findMember(instance.className(), name))
        property = findProperty(instance, name);
    if (!property) {
        const api::MemberData* member = api::findMember(instance.className(), name);
        if (!member || member->kind != api::MemberKind::Property)
            notAMember(L, instance, name);
        extraValues(L, instance, true)->insert_or_assign(std::string(storedName(instance, member->name)), pack(L, idx));
        instance.changed.fire(member->name);
        return;
    }
    if (property->readOnly())
        raise(L, std::format("Unable to assign property {}. Property is read only", property->name));
    std::optional<Value> value = toValue(L, idx);
    if (property->type == "string" && lua_type(L, idx) == LUA_TNUMBER) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, idx, &length);
        value = Value(std::string(text, length));
        lua_pop(L, 1);
    } else if (property->type == "number" && lua_type(L, idx) == LUA_TSTRING && lua_isnumber(L, idx)) {
        value = Value(lua_tonumber(L, idx));
    } else if (property->type == "boolean") {
        value = Value(lua_toboolean(L, idx) != 0);
    }
    if (!value) {
        auto* other = toInstance(L, idx);
        raise(L, std::format("Unable to assign property {}. {} expected, got {}", property->name, property->type,
                             other ? other->className() : luaL_typename(L, idx)));
    }
    guard(L, [&] { property->set(instance, *value); });
}

// The client's method thunks (0x101496b70 and its siblings) first check the
// method's capability, then take self only if it is an instance of the
// method's class: LocalPlayer.Kick(workspace) is "Expected ':' not '.'
// calling member function Kick" [code]. A method running from __namecall has
// no upvalues; namecall did both.
Instance& self(lua_State* L)
{
    checkCapability(L, static_cast<std::uint8_t>(lua_tointeger(L, lua_upvalueindex(3))), "call", methodName(L));
    Instance* instance = toInstance(L, 1);
    const char* requiredClass = lua_tostring(L, lua_upvalueindex(2));
    if (instance && (!requiredClass || isA(*instance, requiredClass)))
        return *instance;
    expectedColon(L);
}

void expectedColon(lua_State* L)
{
    raise(L, std::format("Expected ':' not '.' calling member function {}", methodName(L)));
}

const char* methodName(lua_State* L)
{
    if (const char* name = lua_tostring(L, lua_upvalueindex(1)))
        return name;
    const char* name = lua_namecallatom(L, nullptr);
    return name ? name : "?";
}

bool isA(const Instance& instance, std::string_view className)
{
    return instance.isA(className) || api::isA(instance.className(), className);
}

int pushInstances(lua_State* L, std::span<const InstancePtr> instances)
{
    lua_createtable(L, static_cast<int>(instances.size()), 0);
    int i = 0;
    for (const InstancePtr& instance : instances) {
        pushInstance(L, instance.get());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int returnInstance(lua_State* L, const Instance* instance)
{
    pushInstance(L, instance);
    return 1;
}

void updateImageLoading(Shared& shared)
{
    if (!shared.imageLoaded)
        return;
    std::vector<InstancePtr> observed;
    std::erase_if(shared.imageStates, [](const auto& item) { return item.second.instance.expired(); });
    for (const auto& [key, state] : shared.imageStates) {
        (void)key;
        if (auto instance = state.instance.lock())
            observed.push_back(std::move(instance));
    }
    for (const auto& instance : observed) {
        const std::string content = std::get<std::string>(findNamedProperty(*instance, "Image")->get(*instance));
        const bool loaded = !content.empty() && shared.imageLoaded(content);
        auto& state = shared.imageStates[instance.get()];
        const bool changed = state.loaded != loaded;
        state = {instance, content, loaded};
        if (changed)
            instance->changed.fire("IsLoaded");
    }
}

Instance* findOrCreateService(lua_State* L, ServiceProvider& provider, std::string_view name, bool create)
{
    if (Instance* service = provider.findService(name))
        return service;
    // Services the engine doesn't model sit in the tree as placeholders.
    for (const InstancePtr& child : provider.children())
        if (child->className() == name && dynamic_cast<UnknownInstance*>(child.get()))
            return child.get();
    if (!create)
        return nullptr;
    if (const ClassInfo* info = ClassRegistry::find(name); info && info->kind == ClassKind::Service)
        return guard(L, [&] { return provider.getService(name); });
    const api::ClassData* data = api::findClass(name);
    if (!data || (data->flags & api::ClassService) == 0)
        raise(L, std::format("'{}' is not a valid Service name", name));
    auto service = std::make_shared<UnknownInstance>(ClassRegistry::placeholder(name));
    service->setParent(&provider);
    return service.get();
}

void pushInstance(lua_State* L, const Instance* instance)
{
    if (!instance) {
        lua_pushnil(L);
        return;
    }
    // One userdata per instance while any script holds it, so rawequal works
    // and instances can be table keys.
    lua_getref(L, Runtime::of(L).instanceCache);
    lua_pushlightuserdata(L, const_cast<Instance*>(instance));
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        // First time a script is handed this instance, remember it: the
        // console's getnilinstances looks through what the VM has seen.
        Runtime::of(L).noteInstance(*instance);
        void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(InstancePtr), InstanceTag);
        new (memory) InstancePtr(std::const_pointer_cast<Instance>(instance->shared_from_this()));
        lua_pushlightuserdata(L, const_cast<Instance*>(instance));
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);
}

Instance* toInstance(lua_State* L, int idx)
{
    InstancePtr* instance = rawInstance(L, idx);
    return instance ? instance->get() : nullptr;
}

Instance& checkInstance(lua_State* L, int idx)
{
    if (Instance* instance = toInstance(L, idx))
        return *instance;
    luaL_typeerror(L, idx, "Instance");
}

void setUnmodelledProperty(lua_State* L, const Instance& instance, std::string_view name, Value value)
{
    extraValues(L, instance, true)->insert_or_assign(std::string(name), packed(std::move(value)));
}

void openInstances(lua_State* L)
{
    Runtime& rt = Runtime::of(L);

    // Weak-valued cache: Instance* -> userdata.
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    rt.instanceCache = lua_ref(L, -1);
    lua_pop(L, 1);

    const MethodTable& table = methodTable();
    lua_createtable(L, static_cast<int>(table.methods.size()), 0);
    for (std::size_t i = 0; i < table.methods.size(); ++i) {
        const Method& method = table.methods[i];
        lua_pushlstring(L, method.name.data(), method.name.size());
        lua_pushlstring(L, method.requiredClass.data(), method.requiredClass.size());
        lua_pushinteger(L, table.callCapability[i]);
        lua_pushcclosure(L, method.function, lua_tostring(L, -3), 3);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_setreadonly(L, -1, true);
    rt.instanceMethods = lua_ref(L, -1);
    lua_pop(L, 1);

    // Closures of the functions Lunarstone doesn't implement, by member.
    lua_newtable(L);
    rt.unimplementedMethods = lua_ref(L, -1);
    lua_pop(L, 1);

    lua_newtable(L);
    pushMetamethod(L, index);
    lua_setfield(L, -2, "__index");
    pushMetamethod(L, newIndex);
    lua_setfield(L, -2, "__newindex");
    pushMetamethod(L, toString);
    lua_setfield(L, -2, "__tostring");
    pushMetamethod(L, namecall);
    lua_setfield(L, -2, "__namecall");
    registerType(L, InstanceTag, "Instance",
                 [](lua_State*, void* memory) { static_cast<InstancePtr*>(memory)->~InstancePtr(); });

    lua_newtable(L);
    lua_pushcfunction(L, instanceNew, "new");
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, instanceFromExisting, "fromExisting");
    lua_setfield(L, -2, "fromExisting");
    lua_setglobal(L, "Instance");
}

} // namespace lunar::lua
