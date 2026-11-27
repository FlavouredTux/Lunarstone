#include <lunar/Instance.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <mutex>
#include <ranges>
#include <set>
#include <unordered_map>
#include <utility>

namespace lunar {

namespace {

std::string_view nameOrNull(const Instance* instance)
{
    return instance ? std::string_view(instance->name()) : "NULL";
}

// Preorder search, the same order descendants() returns.
template <class Pred>
Instance* findFirst(const Instance& root, bool recursive, const Pred& pred)
{
    for (const InstancePtr& child : root.children()) {
        if (pred(*child))
            return child.get();
        if (recursive)
            if (Instance* found = findFirst(*child, true, pred))
                return found;
    }
    return nullptr;
}

template <class Pred>
Instance* findAncestor(const Instance& start, const Pred& pred)
{
    for (Instance* ancestor = start.parent(); ancestor; ancestor = ancestor->parent())
        if (pred(*ancestor))
            return ancestor;
    return nullptr;
}

} // namespace

std::string_view detail::valueClassName(const Instance& instance)
{
    return instance.className();
}

bool ClassInfo::isA(std::string_view className) const
{
    for (const ClassInfo* info = this; info; info = info->base)
        if (info->name == className)
            return true;
    return false;
}

const Property* ClassInfo::findProperty(std::string_view propertyName) const
{
    for (const ClassInfo* info = this; info; info = info->base)
        for (const Property& property : info->properties)
            if (property.name == propertyName)
                return &property;
    return nullptr;
}

const ClassInfo& Instance::staticClassInfo()
{
    static const ClassInfo info{"Instance", nullptr, ClassKind::Abstract, nullptr, detail::ownProperties<Instance>()};
    return info;
}

// The names Roblox gives these classes' new instances (each is a string in
// the 0.742 binary; GUESS: the list isn't complete, the rest keep their
// class's name). A server leaves a Name that's its class's default off the
// wire, so a wrong one here renames the instance [live: Midnight Racing's
// Lighting.Blur and Lighting.DepthOfField arrived as BlurEffect and
// DepthOfFieldEffect, and its scripts raised 1500 times looking for them].
std::string_view Instance::defaultName(std::string_view className)
{
    static constexpr std::pair<std::string_view, std::string_view> names[] = {
        {"BlurEffect", "Blur"},
        {"BloomEffect", "Bloom"},
        {"ColorCorrectionEffect", "ColorCorrection"},
        {"ColorGradingEffect", "ColorGrading"},
        {"DepthOfFieldEffect", "DepthOfField"},
        {"SunRaysEffect", "SunRays"},
        {"SpecialMesh", "Mesh"},
        {"BlockMesh", "Mesh"},
        {"CylinderMesh", "Mesh"},
        {"FileMesh", "Mesh"},
        {"TouchTransmitter", "TouchInterest"},
        {"BodyColors", "Body Colors"},
        {"ShirtGraphic", "Shirt Graphic"},
        {"UnionOperation", "Union"},
        {"NegateOperation", "NegativePart"},
        {"IntersectOperation", "Intersection"},
        {"WedgePart", "Wedge"},
        {"CornerWedgePart", "CornerWedge"},
        {"TrussPart", "Truss"},
    };
    for (const auto& [cls, name] : names)
        if (cls == className)
            return name;
    return className;
}

Instance::Instance(const Instance& other)
    : std::enable_shared_from_this<Instance>(other), name_(other.name_), attributes_(other.attributes_),
      tags_(other.tags_),
      unmodelled_(other.unmodelled_ ? std::make_unique<std::vector<std::pair<std::string_view, Value>>>(*other.unmodelled_)
                                    : nullptr),
      archivable_(other.archivable_)
{
}

namespace {

// The names unmodelled values are kept under, one copy of each for the run:
// there are few of them, and a session sets them a hundred thousand times.
std::string_view internName(std::string_view name)
{
    static std::mutex mutex;
    static std::set<std::string, std::less<>> names;
    const std::scoped_lock lock(mutex);
    auto it = names.find(name);
    if (it == names.end())
        it = names.emplace(name).first;
    return *it;
}

} // namespace

const Value* Instance::unmodelled(std::string_view name) const
{
    if (!unmodelled_)
        return nullptr;
    auto it = std::ranges::find(*unmodelled_, name, &std::pair<std::string_view, Value>::first);
    return it == unmodelled_->end() ? nullptr : &it->second;
}

void Instance::setUnmodelled(std::string_view name, Value value)
{
    if (!unmodelled_)
        unmodelled_ = std::make_unique<std::vector<std::pair<std::string_view, Value>>>();
    auto it = std::ranges::find(*unmodelled_, name, &std::pair<std::string_view, Value>::first);
    if (it != unmodelled_->end())
        it->second = std::move(value);
    else
        unmodelled_->emplace_back(internName(name), std::move(value));
}

Instance::~Instance()
{
    // Children still referenced elsewhere just lose their parent. No events fire here.
    for (const InstancePtr& child : children_)
        child->parent_ = nullptr;
}

void Instance::setName(std::string name)
{
    setProperty(name_, std::move(name), "Name");
}

void Instance::setArchivable(bool archivable)
{
    setProperty(archivable_, archivable, "Archivable");
}

namespace {

void checkAttributeName(std::string_view name)
{
    if (name.empty() || name.size() > 100)
        throw Error(std::format("Attribute name '{}' must be between 1 and 100 characters", name));
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_')
            throw Error(std::format("Attribute name '{}' can only contain letters, digits and underscores", name));
}

bool attributeTypeAllowed(const Value& value)
{
    return !std::holds_alternative<InstancePtr>(value) && !std::holds_alternative<Faces>(value)
           && !std::holds_alternative<Axes>(value) && !std::holds_alternative<PhysicalProperties>(value)
           && !std::holds_alternative<Vector3int16>(value);
}

} // namespace

Value Instance::getAttribute(std::string_view name) const
{
    auto it = std::ranges::find(attributes_, name, &std::pair<std::string, Value>::first);
    return it == attributes_.end() ? Value() : it->second;
}

void Instance::setAttribute(std::string_view name, const Value& value)
{
    checkAttributeName(name);
    if (!attributeTypeAllowed(value))
        throw Error(std::format("{} is not a supported attribute type", typeName(value)));

    auto it = std::ranges::find(attributes_, name, &std::pair<std::string, Value>::first);
    if (std::holds_alternative<std::monostate>(value)) {
        if (it == attributes_.end())
            return;
        attributes_.erase(it);
    } else if (it == attributes_.end()) {
        attributes_.emplace_back(name, value);
    } else if (it->second == value) {
        return;
    } else {
        it->second = value;
    }
    attributeChanged.fire(name);
}

bool Instance::hasTag(std::string_view tag) const
{
    return std::ranges::find(tags_, tag) != tags_.end();
}

void Instance::addTag(std::string_view tag)
{
    if (!hasTag(tag))
        tags_.emplace_back(tag);
}

void Instance::removeTag(std::string_view tag)
{
    std::erase(tags_, tag);
}

Value Instance::get(std::string_view property) const
{
    return requireProperty(property).get(*this);
}

void Instance::set(std::string_view property, const Value& value)
{
    const Property& p = requireProperty(property);
    if (p.readOnly())
        throw Error(std::format("Unable to assign property {}. Property is read only", p.name));
    p.set(*this, value);
}

const Property& Instance::requireProperty(std::string_view propertyName) const
{
    if (const Property* property = classInfo().findProperty(propertyName))
        return *property;
    throw Error(std::format("{} is not a valid member of {} \"{}\"", propertyName, className(), fullName()));
}

std::vector<const Property*> Instance::properties() const
{
    std::vector<const ClassInfo*> chain;
    for (const ClassInfo* info = &classInfo(); info; info = info->base)
        chain.push_back(info);

    std::vector<const Property*> result;
    for (const ClassInfo* info : chain | std::views::reverse)
        for (const Property& property : info->properties)
            result.push_back(&property);
    return result;
}

void Instance::setParent(Instance* newParent)
{
    if (newParent == parent_)
        return;
    if (parentLocked_)
        throwParentLocked(newParent);
    if (newParent == this)
        throw Error(std::format("Attempt to set {} as its own parent", fullName()));
    if (newParent && newParent->isDescendantOf(this))
        throw Error(std::format("Attempt to set parent of {} to {} would result in circular reference", fullName(),
                                newParent->fullName()));
    reparent(newParent);
}

void Instance::throwParentLocked(const Instance* newParent) const
{
    throw Error(std::format("The Parent property of {} is locked, current parent: {}, new parent {}", name_,
                            nameOrNull(parent_), nameOrNull(newParent)));
}

namespace {

// Whether anything listens for what moving `instance` from `from` to `to`
// announces besides its own Changed: the ancestors' DescendantRemoving and
// DescendantAdded, the parents' ChildRemoved and ChildAdded, and the
// AncestryChanged of it and everything under it.
bool reparentHeard(const Instance& instance, const Instance* from, const Instance* to)
{
    for (const Instance* ancestor = from; ancestor; ancestor = ancestor->parent())
        if (!ancestor->descendantRemoving.empty())
            return true;
    for (const Instance* ancestor = to; ancestor; ancestor = ancestor->parent())
        if (!ancestor->descendantAdded.empty())
            return true;
    if ((from && !from->childRemoved.empty()) || (to && !to->childAdded.empty()))
        return true;
    auto heard = [](this const auto& self, const Instance& node) -> bool {
        if (!node.ancestryChanged.empty())
            return true;
        for (const InstancePtr& child : node.children())
            if (self(*child))
                return true;
        return false;
    };
    return heard(instance);
}

} // namespace

void Instance::reparent(Instance* newParent)
{
    InstancePtr self = shared_from_this();
    Instance* oldParent = parent_;
    // Nobody to tell (an instance being built, a model loading): no handler
    // can run before Changed, so the move is the same without the list of
    // everything moving or the events nobody hears. A place file's or a
    // model's every instance moves this way as it's loaded.
    if (!reparentHeard(*this, oldParent, newParent)) {
        if (oldParent)
            std::erase(oldParent->children_, self);
        parent_ = newParent;
        if (newParent)
            newParent->children_.push_back(self);
        changed.fire("Parent");
        return;
    }
    std::vector<InstancePtr> moving = subtree();

    if (oldParent) {
        for (Instance* ancestor = oldParent; ancestor; ancestor = ancestor->parent_)
            for (const InstancePtr& instance : moving)
                ancestor->descendantRemoving.fire(instance.get());
        std::erase(oldParent->children_, self);
    }

    parent_ = newParent;
    if (newParent)
        newParent->children_.push_back(self);

    if (oldParent)
        oldParent->childRemoved.fire(this);
    if (newParent) {
        newParent->childAdded.fire(this);
        for (Instance* ancestor = newParent; ancestor; ancestor = ancestor->parent_)
            for (const InstancePtr& instance : moving)
                ancestor->descendantAdded.fire(instance.get());
    }

    for (const InstancePtr& instance : moving)
        instance->ancestryChanged.fire(this, newParent);
    changed.fire("Parent");
}

std::vector<InstancePtr> Instance::subtree()
{
    std::vector<InstancePtr> result = descendants();
    result.insert(result.begin(), shared_from_this());
    return result;
}

std::vector<InstancePtr> Instance::descendants() const
{
    std::vector<InstancePtr> result;
    auto visit = [&](this const auto& self, const Instance& instance) -> void {
        for (const InstancePtr& child : instance.children_) {
            result.push_back(child);
            self(*child);
        }
    };
    visit(*this);
    return result;
}

Instance* Instance::findFirstChild(std::string_view name, bool recursive) const
{
    return findFirst(*this, recursive, [&](const Instance& i) { return i.name_ == name; });
}

Instance* Instance::findFirstChildOfClass(std::string_view className) const
{
    return findFirst(*this, false, [&](const Instance& i) { return i.className() == className; });
}

Instance* Instance::findFirstChildWhichIsA(std::string_view className, bool recursive) const
{
    return findFirst(*this, recursive, [&](const Instance& i) { return i.isA(className); });
}

Instance* Instance::findFirstAncestor(std::string_view name) const
{
    return findAncestor(*this, [&](const Instance& i) { return i.name_ == name; });
}

Instance* Instance::findFirstAncestorOfClass(std::string_view className) const
{
    return findAncestor(*this, [&](const Instance& i) { return i.className() == className; });
}

Instance* Instance::findFirstAncestorWhichIsA(std::string_view className) const
{
    return findAncestor(*this, [&](const Instance& i) { return i.isA(className); });
}

bool Instance::isAncestorOf(const Instance* descendant) const
{
    return descendant && descendant->isDescendantOf(this);
}

bool Instance::isDescendantOf(const Instance* ancestor) const
{
    for (const Instance* p = parent_; p; p = p->parent_)
        if (p == ancestor)
            return true;
    return false;
}

std::string Instance::fullName() const
{
    std::vector<std::string_view> names;
    for (const Instance* i = this; i; i = i->parent_) {
        if (i != this && i->isA("DataModel"))
            break;
        names.push_back(i->name_);
    }
    return names | std::views::reverse | std::views::join_with('.') | std::ranges::to<std::string>();
}

void Instance::destroy()
{
    if (destroyed_)
        return;
    if (parentLocked_)
        throwParentLocked(nullptr);
    destroyInternal();
}

void Instance::destroyInternal()
{
    if (destroyed_)
        return;
    InstancePtr self = shared_from_this();
    destroyed_ = true;
    destroying.fire();

    if (parent_)
        reparent(nullptr);
    parentLocked_ = true;

    for (const InstancePtr& child : std::vector(children_))
        child->destroyInternal();

    childAdded.disconnectAll();
    childRemoved.disconnectAll();
    descendantAdded.disconnectAll();
    descendantRemoving.disconnectAll();
    ancestryChanged.disconnectAll();
    changed.disconnectAll();
    attributeChanged.disconnectAll();
    destroying.disconnectAll();
    eventFired.disconnectAll();
}

void Instance::clearAllChildren()
{
    for (const InstancePtr& child : std::vector(children_))
        child->destroy();
}

InstancePtr Instance::cloneTree(std::vector<std::pair<const Instance*, Instance*>>& copies, bool includeNonArchivable) const
{
    if (!archivable_ && !includeNonArchivable)
        return nullptr;
    InstancePtr copy = cloneSelf();
    if (!copy)
        return nullptr;
    copies.emplace_back(this, copy.get());
    for (const InstancePtr& child : children_) {
        if (InstancePtr childCopy = child->cloneTree(copies, includeNonArchivable)) {
            childCopy->parent_ = copy.get();
            copy->children_.push_back(std::move(childCopy));
        }
    }
    return copy;
}

InstancePtr Instance::clone() const
{
    return cloneImpl(false);
}

InstancePtr Instance::cloneForClient() const
{
    return cloneImpl(true);
}

InstancePtr Instance::cloneImpl(bool includeNonArchivable) const
{
    std::vector<std::pair<const Instance*, Instance*>> copies;
    InstancePtr root = cloneTree(copies, includeNonArchivable);
    if (!root)
        return nullptr;

    std::unordered_map<const Instance*, Instance*> copyOf(copies.begin(), copies.end());
    for (auto [original, copy] : copies) {
        for (const Property* property : copy->properties()) {
            if (!property->isRef || property->readOnly() || property->name == "Parent")
                continue;
            Value target = property->get(*copy);
            auto* instance = std::get_if<InstancePtr>(&target);
            if (!instance || !*instance)
                continue;
            if (auto it = copyOf.find(instance->get()); it != copyOf.end())
                property->set(*copy, Value(it->second->shared_from_this()));
        }
    }
    return root;
}

} // namespace lunar
