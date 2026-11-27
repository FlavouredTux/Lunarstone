#pragma once

#include <lunar/Error.hpp>
#include <lunar/Property.hpp>
#include <lunar/Signal.hpp>
#include <lunar/Value.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace lunar {

class ServiceProvider;

// A reference property's storage: doesn't keep the target alive, so a
// destroyed part reads back as nil once nothing else owns it.
template <class T>
class Ref {
public:
    T* get() const { return ptr_.lock().get(); }
    void reset(T* target)
    {
        ptr_ = target ? std::static_pointer_cast<T>(target->shared_from_this()) : std::weak_ptr<T>();
    }

private:
    std::weak_ptr<T> ptr_;
};

enum class ClassKind : std::uint8_t {
    Abstract,     // never instantiated (Instance, PVInstance, BasePart)
    Creatable,    // Instance.new and Clone work
    Service,      // created on demand by ServiceProvider::getService
    NotCreatable, // exists, but only the engine makes it (DataModel)
};

struct ClassInfo {
    std::string_view name;
    const ClassInfo* base = nullptr;
    ClassKind kind = ClassKind::Abstract;
    InstancePtr (*factory)() = nullptr;   // null for Abstract classes
    std::span<const Property> properties; // declared by this class, not its bases

    bool isA(std::string_view className) const;
    // Searches this class, then its bases.
    const Property* findProperty(std::string_view name) const;
};

// Root of the object tree. Always owned through std::shared_ptr: a parent keeps
// its children alive, a child only points back at its parent.
class Instance : public std::enable_shared_from_this<Instance> {
public:
    Instance& operator=(const Instance&) = delete;
    virtual ~Instance();

    static const ClassInfo& staticClassInfo();
    virtual const ClassInfo& classInfo() const { return staticClassInfo(); }
    bool isA(std::string_view className) const { return classInfo().isA(className); }

    LUNAR_PROPERTY() std::string_view className() const { return classInfo().name; }

    LUNAR_PROPERTY() const std::string& name() const { return name_; }
    void setName(std::string name);

    LUNAR_PROPERTY() bool archivable() const { return archivable_; }
    void setArchivable(bool archivable);

    LUNAR_PROPERTY() Instance* parent() const { return parent_; }
    void setParent(Instance* newParent);
    void setParent(const InstancePtr& newParent) { setParent(newParent.get()); }
    void setParent(std::nullptr_t) { setParent(static_cast<Instance*>(nullptr)); }

    // Property access by name, the way scripts, place files and replication see it.
    Value get(std::string_view property) const;
    void set(std::string_view property, const Value& value);
    // Every property of the class, base classes first.
    std::vector<const Property*> properties() const;

    // Attributes, custom values anyone can attach. getAttribute gives nil when
    // there's none; setting nil removes one. setAttribute throws for names or
    // value types Roblox rejects (Instances, for one). Names starting with RBX
    // are Roblox's own: files carry them, but scripts shouldn't set them.
    Value getAttribute(std::string_view name) const;
    void setAttribute(std::string_view name, const Value& value);
    const std::vector<std::pair<std::string, Value>>& attributes() const { return attributes_; }

    // CollectionService tags, in the order they were added.
    bool hasTag(std::string_view tag) const;
    void addTag(std::string_view tag);
    void removeTag(std::string_view tag);
    const std::vector<std::string>& tags() const { return tags_; }

    // Values the server or a file gave for properties the engine doesn't
    // model (a MeshPart's AeroMeshData), kept as they came for the console's
    // gethiddenproperty: by the name they were saved under. Null when there
    // is none of that name.
    const Value* unmodelled(std::string_view name) const;
    void setUnmodelled(std::string_view name, Value value);

    std::span<const InstancePtr> children() const { return children_; }
    std::vector<InstancePtr> descendants() const;

    Instance* findFirstChild(std::string_view name, bool recursive = false) const;
    Instance* findFirstChildOfClass(std::string_view className) const;
    Instance* findFirstChildWhichIsA(std::string_view className, bool recursive = false) const;
    Instance* findFirstAncestor(std::string_view name) const;
    Instance* findFirstAncestorOfClass(std::string_view className) const;
    Instance* findFirstAncestorWhichIsA(std::string_view className) const;

    bool isAncestorOf(const Instance* descendant) const;
    bool isDescendantOf(const Instance* ancestor) const;

    // Dot-separated path, without the DataModel: "Workspace.Model.Part".
    std::string fullName() const;

    // Parent becomes nil and locked, connections drop, children get destroyed too.
    void destroy();
    void clearAllChildren();
    bool destroyed() const { return destroyed_; }

    // Deep copy with no parent. Null if this or its class can't be cloned;
    // non-archivable descendants are skipped. Reference properties pointing
    // inside the copied subtree point at the copies, like Roblox does
    // (Model.PrimaryPart, Weld.Part0, ObjectValue.Value...).
    InstancePtr clone() const;
    // Native client copies of shipped scripts include non-archivable nodes;
    // the script-visible Clone method continues to honor Archivable.
    InstancePtr cloneForClient() const;
    // This one alone: its class and property values, no children, no parent
    // (Instance.fromExisting). Null if its class can't be copied.
    InstancePtr copy() const { return cloneSelf(); }

    Signal<Instance*> childAdded;
    Signal<Instance*> childRemoved;
    Signal<Instance*> descendantAdded;
    Signal<Instance*> descendantRemoving;
    Signal<Instance* /*child*/, Instance* /*parent*/> ancestryChanged;
    Signal<std::string_view /*property*/> changed;
    Signal<std::string_view /*attribute*/> attributeChanged;
    Signal<> destroying;
    // Every other Roblox event of the instance's class (Touched, Activated,
    // Heartbeat...), by name. Whoever implements one fires it here and
    // scripts connected to that event run; nothing needs declaring per event.
    Signal<std::string_view /*event*/, std::span<const Value> /*arguments*/> eventFired;
    void fireEvent(std::string_view event, std::span<const Value> arguments = {}) const
    {
        eventFired.fire(event, arguments);
    }

protected:
    Instance() = default;
    // Copies properties only, never hierarchy or connections.
    Instance(const Instance& other);

    void initName(std::string_view name) { name_ = name; }
    // A new instance's Name: its class's, except where the client gives
    // another (a SpecialMesh is "Mesh"). The server leaves a Name at that
    // default out of what it sends, so scripts find the game's part.Mesh by it.
    static std::string_view defaultName(std::string_view className);
    virtual InstancePtr cloneSelf() const { return nullptr; }

    template <class T>
    void setProperty(T& field, T value, std::string_view property)
    {
        if (field == value)
            return;
        field = std::move(value);
        changed.fire(property);
    }

    template <class T>
    void setRef(Ref<T>& field, T* target, std::string_view property)
    {
        if (field.get() == target)
            return;
        field.reset(target);
        changed.fire(property);
    }

private:
    friend class ServiceProvider;

    [[noreturn]] void throwParentLocked(const Instance* newParent) const;
    const Property& requireProperty(std::string_view name) const;
    void reparent(Instance* newParent);
    void destroyInternal();
    std::vector<InstancePtr> subtree();
    InstancePtr cloneTree(std::vector<std::pair<const Instance*, Instance*>>& copies, bool includeNonArchivable) const;
    InstancePtr cloneImpl(bool includeNonArchivable) const;

    std::string name_;
    Instance* parent_ = nullptr;
    std::vector<InstancePtr> children_;
    std::vector<std::pair<std::string, Value>> attributes_;
    std::vector<std::string> tags_;
    // Made with the first one: most instances have none. The names are
    // interned, so each entry is a view.
    std::unique_ptr<std::vector<std::pair<std::string_view, Value>>> unmodelled_;
    bool archivable_ = true;
    bool parentLocked_ = false;
    bool destroyed_ = false;
};

// The common case of a property: a stored value with a getter, a set<Name>
// setter that fires Changed, and a private field holding it. Name is the
// Roblox property name; the default value goes last, and can be left out for
// types whose default constructor gives the right value. Leaves access public.
//     LUNAR_FIELD(float, range, Range, 8.0f)
//     LUNAR_FIELD(std::string, soundId, SoundId)
// NOLINTBEGIN(bugprone-macro-parentheses): Type and getter are a type and a name
#define LUNAR_FIELD(Type, getter, Name, ...)                                                                           \
public:                                                                                                                \
    LUNAR_PROPERTY(#Name) const Type& getter() const                                                                   \
    {                                                                                                                  \
        return getter##_;                                                                                              \
    }                                                                                                                  \
    void set##Name(Type value)                                                                                         \
    {                                                                                                                  \
        this->setProperty(getter##_, std::move(value), #Name);                                                         \
    }                                                                                                                  \
                                                                                                                       \
private:                                                                                                               \
    Type getter##_ __VA_OPT__(= __VA_ARGS__);                                                                          \
                                                                                                                       \
public:

// The same for a reference to another instance (nil by default).
//     LUNAR_REF(BasePart, part0, Part0)
#define LUNAR_REF(Type, getter, Name)                                                                                  \
public:                                                                                                                \
    LUNAR_PROPERTY(#Name) Type* getter() const                                                                         \
    {                                                                                                                  \
        return getter##_.get();                                                                                        \
    }                                                                                                                  \
    void set##Name(Type* target)                                                                                       \
    {                                                                                                                  \
        this->setRef(getter##_, target, #Name);                                                                        \
    }                                                                                                                  \
                                                                                                                       \
private:                                                                                                               \
    ::lunar::Ref<Type> getter##_;                                                                                      \
                                                                                                                       \
public:
// NOLINTEND(bugprone-macro-parentheses)

// CRTP base every engine class derives through. ClassRegistry finds every
// class in namespace lunar deriving directly from InstanceImpl<itself, ...>,
// so declaring one is enough to register it. Supplies the ClassInfo (class
// name and properties come from the C++ type via reflection), the Instance.new
// factory and the property copy used by Clone.
template <class T, class Base, ClassKind Kind = ClassKind::Creatable>
class InstanceImpl : public Base {
public:
    // The class whose ClassInfo staticClassInfo() is (instanceCast checks a
    // class has its own).
    using ImplementedClass = T;

    static const ClassInfo& staticClassInfo()
    {
        static constexpr std::string_view name = detail::className<T>();
        InstancePtr (*factory)() = nullptr;
        if constexpr (Kind != ClassKind::Abstract)
            factory = [] -> InstancePtr { return std::make_shared<T>(); };
        static const ClassInfo info{name, &Base::staticClassInfo(), Kind, factory, detail::ownProperties<T>()};
        return info;
    }

    const ClassInfo& classInfo() const override { return staticClassInfo(); }

protected:
    InstancePtr cloneSelf() const override
    {
        if constexpr (Kind == ClassKind::Creatable)
            return std::make_shared<T>(static_cast<const T&>(*this));
        else
            return nullptr;
    }

private:
    // Only T itself can derive from InstanceImpl<T, ...>.
    friend T;
    InstanceImpl() { this->initName(Instance::defaultName(staticClassInfo().name)); }
    InstanceImpl(const InstanceImpl&) = default;
};

// dynamic_cast for instances, by their classes' ClassInfo chain: a few
// pointer compares where the C++ type_info walk compares mangled names with
// strcmp, which made it most of the GUI's layout time. T must be an engine
// class (deriving from InstanceImpl<T, ...>, so that its ClassInfo is its
// own); a ClassInfo in an instance's chain is then always a C++ base of it.
template <class T, class From>
    requires std::derived_from<From, Instance>
             && std::same_as<typename std::remove_const_t<T>::ImplementedClass, std::remove_const_t<T>>
auto instanceCast(From* instance) -> std::conditional_t<std::is_const_v<From>, const std::remove_const_t<T>*, T*>
{
    if (!instance)
        return nullptr;
    const ClassInfo* target = &std::remove_const_t<T>::staticClassInfo();
    for (const ClassInfo* at = &instance->classInfo(); at; at = at->base)
        if (at == target)
            return static_cast<std::conditional_t<std::is_const_v<From>, const std::remove_const_t<T>*, T*>>(instance);
    return nullptr;
}

} // namespace lunar
