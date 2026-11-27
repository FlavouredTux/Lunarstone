#pragma once

// Basic classes with no world presence: containers, events and the
// ServiceProvider base. Included through <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Instance.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar {

class Folder final : public InstanceImpl<Folder, Instance> {};

class Configuration final : public InstanceImpl<Configuration, Instance> {};

// Translations by key, each with its text per locale: what SetEntries fills
// (the Lua app's UI libraries build theirs from generated tables).
class LocalizationTable final : public InstanceImpl<LocalizationTable, Instance> {
public:
    struct Entry {
        std::string key, source, context, example;
        std::vector<std::pair<std::string, std::string>> values; // locale, text
    };

    LUNAR_FIELD(std::string, sourceLocaleId, SourceLocaleId, "en-us")

    const std::vector<Entry>& entries() const { return entries_; }
    void setEntries(std::vector<Entry> entries) { entries_ = std::move(entries); }

    // The text `key` has in `locale`: its value there, else its source when
    // that's the table's own locale. Locale ids match whatever their case.
    std::optional<std::string> textOf(std::string_view key, std::string_view locale) const;
    // The same for the entry whose source text is `source`.
    std::optional<std::string> translationOf(std::string_view source, std::string_view locale) const;

private:
    std::optional<std::string> textIn(const Entry& entry, std::string_view locale) const;

    std::vector<Entry> entries_;
};

// A locale and the table it translates from (none for LocalizationService's
// translators, which pass text through: games' tables aren't fetched).
class Translator final : public InstanceImpl<Translator, Instance, ClassKind::NotCreatable> {
public:
    LUNAR_PROPERTY() const std::string& localeId() const { return localeId_; }
    void assignLocale(std::string locale) { localeId_ = std::move(locale); }
    LocalizationTable* table() const { return table_.get(); }
    void assignTable(LocalizationTable* table) { table_.reset(table); }

private:
    std::string localeId_ = "en-us";
    Ref<LocalizationTable> table_;
};

// What TextService:GetTextBoundsAsync measures: the text, its font and
// size, and the width it wraps at (0: one line).
class GetTextBoundsParams final : public InstanceImpl<GetTextBoundsParams, Instance> {
public:
    LUNAR_FIELD(std::string, text, Text)
    LUNAR_FIELD(Font, font, Font)
    LUNAR_FIELD(float, size, Size, 14.0f)
    LUNAR_FIELD(float, width, Width, 0.0f)
    LUNAR_FIELD(bool, richText, RichText, false)
};

// What HttpService:RequestInternal makes: a request that Start sends once.
class HttpRequest final : public InstanceImpl<HttpRequest, Instance, ClassKind::NotCreatable> {
public:
    struct Spec {
        std::string url;
        std::string method = "GET";
        std::vector<std::string> headers; // "Name: value"
        std::string body;
    };

    const Spec& spec() const { return spec_; }
    void assignSpec(Spec spec) { spec_ = std::move(spec); }
    // Start may happen once; Cancel drops the answer when it comes.
    bool started() const { return started_; }
    void markStarted() { started_ = true; }
    bool cancelled() const { return cancelled_; }
    void cancel() { cancelled_ = true; }

private:
    Spec spec_;
    bool started_ = false;
    bool cancelled_ = false;
};

// The asset a union's AssetId names: MeshData is its geometry (an obfuscated
// CSG mesh, assets::parseCsgMesh), ChildData the model it was built from.
class PartOperationAsset final : public InstanceImpl<PartOperationAsset, Instance> {
public:
    LUNAR_FIELD(std::string, meshData, MeshData)
    LUNAR_FIELD(std::string, childData, ChildData)
};

// Events and callbacks scripts wire up; no state of their own.
class BindableEvent final : public InstanceImpl<BindableEvent, Instance> {};
class BindableFunction final : public InstanceImpl<BindableFunction, Instance> {};
class BaseRemoteEvent : public InstanceImpl<BaseRemoteEvent, Instance, ClassKind::Abstract> {};
class RemoteEvent final : public InstanceImpl<RemoteEvent, BaseRemoteEvent> {};
class RemoteFunction final : public InstanceImpl<RemoteFunction, Instance> {};
class UnreliableRemoteEvent final : public InstanceImpl<UnreliableRemoteEvent, BaseRemoteEvent> {};

// Anything with a position in the world.
class PVInstance : public InstanceImpl<PVInstance, Instance, ClassKind::Abstract> {};

class ServiceProvider : public InstanceImpl<ServiceProvider, Instance, ClassKind::Abstract> {
public:
    // Returns the service, creating and parenting it on first use.
    Instance* getService(std::string_view className);
    Instance* findService(std::string_view className) const;

    template <class T>
    T* getService()
    {
        return static_cast<T*>(getService(T::staticClassInfo().name));
    }
};

// Stand-in for a class the engine doesn't implement yet, so loaded trees keep
// their shape. Reports the original ClassName; has no properties of its own.
// Its ClassInfo is a ClassRegistry::placeholder, never an engine class's
// (instanceCast would take it for one).
class UnknownInstance final : public Instance {
public:
    explicit UnknownInstance(const ClassInfo& info) : info_(&info) { initName(Instance::defaultName(info.name)); }
    UnknownInstance(const UnknownInstance&) = default;

    const ClassInfo& classInfo() const override { return *info_; }

protected:
    InstancePtr cloneSelf() const override { return std::make_shared<UnknownInstance>(*this); }

private:
    const ClassInfo* info_;
};

} // namespace lunar
