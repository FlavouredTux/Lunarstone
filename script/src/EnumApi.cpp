// The Enum global, Enum types and EnumItems. Every enum of the real client is
// here (from RobloxApiData.cpp); enums the engine also knows share its
// EnumInfo, so their items assign straight to engine properties.
//
// Member names and error wording come from the 0.742 client's strings:
// GetEnums, GetEnumItems, FromName, FromValue, `%s is not a valid member of
// "Enum"`, `... of "Enum.%s"`, `... of "Enum.%s.%s"` and tostring "Enum.%s.%s".

#include "RobloxApi.hpp"
#include "Userdata.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace lunar::lua {

namespace {

struct Catalog {
    std::vector<EnumCatalogEntry> entries; // sorted by name
    std::deque<EnumInfo> ownedInfos;
    std::unordered_map<const EnumInfo*, const EnumCatalogEntry*> byInfo;
};

void addEntry(Catalog& catalog, std::string_view name, const api::EnumData* data)
{
    EnumCatalogEntry entry;
    entry.name = name;
    entry.info = enums::find(name);
    if (!entry.info)
        entry.info = &catalog.ownedInfos.emplace_back(EnumInfo{name, {}});
    if (data) {
        for (const api::EnumItemData& item : api::itemsOf(*data)) {
            // An alias sharing a value (KeyCode.Unknown, None's 0) keeps its own name.
            const EnumItem* known = entry.info->find(item.value);
            entry.items.push_back(known && known->name == item.name ? *known
                                                                    : EnumItem{entry.info, item.name, item.value});
        }
    } else {
        entry.items.assign(entry.info->items.begin(), entry.info->items.end());
    }
    catalog.entries.push_back(std::move(entry));
}

Catalog buildCatalog()
{
    Catalog catalog;
    for (const api::EnumData& data : api::allEnums())
        addEntry(catalog, data.name, &data);
    // Engine enums the client dump lacks still deserve an Enum entry.
    for (const EnumInfo* info : enums::all())
        if (!api::findEnum(info->name))
            addEntry(catalog, info->name, nullptr);
    std::ranges::sort(catalog.entries, {}, &EnumCatalogEntry::name);
    for (EnumCatalogEntry& entry : catalog.entries) {
        // Owned infos list the full item set.
        for (EnumInfo& owned : catalog.ownedInfos)
            if (&owned == entry.info)
                owned.items = entry.items;
        catalog.byInfo.emplace(entry.info, &entry);
    }
    return catalog;
}

const Catalog& catalog()
{
    static const Catalog instance = buildCatalog();
    return instance;
}

constexpr const char* enumItemCacheKey = "lunar.enumitems";

// --- EnumItem ----------------------------------------------------------------

std::string enumTypeName(const EnumItem& item)
{
    return item.enumType ? std::string(item.enumType->name) : std::string("?");
}

int enumItemIsA(lua_State* L)
{
    const EnumItem& item = check<EnumItem>(L, 1);
    lua_pushboolean(L, item.enumType && item.enumType->name == checkString(L, 2));
    return 1;
}

int enumItemIndex(lua_State* L)
{
    const EnumItem& item = check<EnumItem>(L, 1);
    std::string_view key = checkString(L, 2);
    if (key == "Name") {
        pushString(L, item.name);
    } else if (key == "Value") {
        pushNumber(L, item.value);
    } else if (key == "EnumType") {
        const EnumCatalogEntry* entry = findEnumEntry(item.enumType);
        if (!entry)
            lua_pushnil(L);
        else
            push<EnumType>(L, EnumType{entry});
    } else if (key == "IsA") {
        lua_pushcfunction(L, enumItemIsA, "IsA");
    } else {
        raise(L, std::format("{} is not a valid member of \"Enum.{}.{}\"", key, enumTypeName(item), item.name));
    }
    return 1;
}

int enumItemToString(lua_State* L)
{
    const EnumItem& item = check<EnumItem>(L, 1);
    pushString(L, std::format("Enum.{}.{}", enumTypeName(item), item.name));
    return 1;
}

// --- Enum ----------------------------------------------------------------------

const EnumCatalogEntry& checkEnum(lua_State* L, int idx)
{
    return *check<EnumType>(L, idx).entry;
}

int enumGetEnumItems(lua_State* L)
{
    const EnumCatalogEntry& entry = checkEnum(L, 1);
    lua_createtable(L, static_cast<int>(entry.items.size()), 0);
    int i = 0;
    for (const EnumItem& item : entry.items) {
        pushEnumItem(L, item);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int enumFromName(lua_State* L)
{
    const EnumCatalogEntry& entry = checkEnum(L, 1);
    std::string_view name = checkString(L, 2);
    auto it = std::ranges::find(entry.items, name, &EnumItem::name);
    if (it == entry.items.end())
        lua_pushnil(L);
    else
        pushEnumItem(L, *it);
    return 1;
}

int enumFromValue(lua_State* L)
{
    const EnumCatalogEntry& entry = checkEnum(L, 1);
    const double value = luaL_checknumber(L, 2);
    auto it = std::ranges::find_if(entry.items, [&](const EnumItem& item) { return item.value == value; });
    if (it == entry.items.end())
        lua_pushnil(L);
    else
        pushEnumItem(L, *it);
    return 1;
}

// Items Roblox renamed still answer to their old names (the API dump's
// LegacyNames: Font.GothamSemibold, RaycastFilterType.Blacklist [live: The
// Mimic's monster targeting raycasts with Blacklist]).
int enumIndex(lua_State* L)
{
    const EnumCatalogEntry& entry = checkEnum(L, 1);
    std::string_view key = checkString(L, 2);
    for (const api::EnumLegacyName& legacy : api::legacyEnumNames())
        if (legacy.enumName == entry.name && legacy.legacy == key)
            key = legacy.current;
    if (auto it = std::ranges::find(entry.items, key, &EnumItem::name); it != entry.items.end()) {
        pushEnumItem(L, *it);
        return 1;
    }
    if (key == "GetEnumItems")
        lua_pushcfunction(L, enumGetEnumItems, "GetEnumItems");
    else if (key == "FromName")
        lua_pushcfunction(L, enumFromName, "FromName");
    else if (key == "FromValue")
        lua_pushcfunction(L, enumFromValue, "FromValue");
    else
        raise(L, std::format("{} is not a valid member of \"Enum.{}\"", key, entry.name));
    return 1;
}

int enumToString(lua_State* L)
{
    pushString(L, checkEnum(L, 1).name);
    return 1;
}

int enumEq(lua_State* L)
{
    const EnumType* a = to<EnumType>(L, 1);
    const EnumType* b = to<EnumType>(L, 2);
    lua_pushboolean(L, a && b && a->entry == b->entry);
    return 1;
}

// --- Enums -----------------------------------------------------------------------

void pushEnum(lua_State* L, const EnumCatalogEntry& entry)
{
    // Cached like items, so Enum.Material == Enum.Material holds rawequal too.
    lua_getfield(L, LUA_REGISTRYINDEX, enumItemCacheKey);
    lua_pushlightuserdata(L, const_cast<EnumCatalogEntry*>(&entry));
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        push<EnumType>(L, EnumType{&entry});
        lua_pushlightuserdata(L, const_cast<EnumCatalogEntry*>(&entry));
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);
}

int enumsGetEnums(lua_State* L)
{
    const auto& entries = catalog().entries;
    lua_createtable(L, static_cast<int>(entries.size()), 0);
    int i = 0;
    for (const EnumCatalogEntry& entry : entries) {
        pushEnum(L, entry);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int enumsIndex(lua_State* L)
{
    std::string_view key = checkString(L, 2);
    if (const EnumCatalogEntry* entry = findEnumEntry(key)) {
        pushEnum(L, *entry);
        return 1;
    }
    if (key == "GetEnums") {
        lua_pushcfunction(L, enumsGetEnums, "GetEnums");
        return 1;
    }
    raise(L, std::format("{} is not a valid member of \"Enum\"", key));
}

int enumsToString(lua_State* L)
{
    lua_pushliteral(L, "Enums");
    return 1;
}

void registerSimple(lua_State* L, int tag, const char* typeName, lua_CFunction index, lua_CFunction toString,
                    lua_CFunction eq)
{
    lua_newtable(L);
    pushMetamethod(L, index);
    lua_setfield(L, -2, "__index");
    pushMetamethod(L, toString);
    lua_setfield(L, -2, "__tostring");
    if (eq) {
        pushMetamethod(L, eq);
        lua_setfield(L, -2, "__eq");
    }
    registerType(L, tag, typeName);
}

} // namespace

std::span<const EnumCatalogEntry> enumCatalog()
{
    return catalog().entries;
}

// For enums::findAny: every enum of the client, the owned ones with their items.
const EnumInfo* catalogEnum(std::string_view name)
{
    const EnumCatalogEntry* entry = findEnumEntry(name);
    return entry ? entry->info : nullptr;
}

const EnumCatalogEntry* findEnumEntry(std::string_view name)
{
    const auto& entries = catalog().entries;
    auto it = std::ranges::lower_bound(entries, name, {}, &EnumCatalogEntry::name);
    return it != entries.end() && it->name == name ? &*it : nullptr;
}

const EnumCatalogEntry* findEnumEntry(const EnumInfo* info)
{
    const auto& byInfo = catalog().byInfo;
    auto it = byInfo.find(info);
    if (it != byInfo.end())
        return it->second;
    return info ? findEnumEntry(info->name) : nullptr;
}

const EnumItem* findEnumItem(std::string_view enumName, std::string_view itemName)
{
    const EnumCatalogEntry* entry = findEnumEntry(enumName);
    if (!entry)
        return nullptr;
    auto it = std::ranges::find(entry->items, itemName, &EnumItem::name);
    return it == entry->items.end() ? nullptr : &*it;
}

EnumItem enumItemNamed(std::string_view enumName, std::string_view itemName)
{
    return enumItem(enumName, itemName);
}

EnumItem enumItem(std::string_view enumName, std::string_view itemName)
{
    const EnumItem* item = findEnumItem(enumName, itemName);
    return item ? *item : EnumItem{};
}

std::optional<EnumItem> toEnumItem(lua_State* L, int idx, std::string_view enumName, bool strict)
{
    const EnumCatalogEntry* entry = findEnumEntry(enumName);
    if (!entry)
        return std::nullopt;
    if (const EnumItem* item = to<EnumItem>(L, idx)) {
        if (item->enumType && item->enumType->name == enumName)
            return *item;
        return std::nullopt;
    }
    if (strict)
        return std::nullopt;
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const double value = lua_tonumber(L, idx);
        auto it = std::ranges::find_if(entry->items, [&](const EnumItem& item) { return item.value == value; });
        if (it != entry->items.end())
            return *it;
    } else if (lua_type(L, idx) == LUA_TSTRING) {
        auto it = std::ranges::find(entry->items, std::string_view(lua_tostring(L, idx)), &EnumItem::name);
        if (it != entry->items.end())
            return *it;
    }
    return std::nullopt;
}

void pushEnumItem(lua_State* L, const EnumItem& item)
{
    // Items of the catalog are cached by address: one userdata per item, so
    // they work as table keys and compare rawequal like Roblox's.
    const EnumItem* stable = nullptr;
    if (const EnumCatalogEntry* entry = findEnumEntry(item.enumType)) {
        auto it = std::ranges::find(entry->items, item.value, &EnumItem::value);
        if (it != entry->items.end())
            stable = &*it;
    }
    if (!stable) {
        push<EnumItem>(L, item);
        return;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, enumItemCacheKey);
    lua_pushlightuserdata(L, const_cast<EnumItem*>(stable));
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        push<EnumItem>(L, *stable);
        lua_pushlightuserdata(L, const_cast<EnumItem*>(stable));
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);
}

void openEnums(lua_State* L)
{
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, enumItemCacheKey);

    registerSimple(L, EnumItemTag, "EnumItem", enumItemIndex, enumItemToString, eqOf<EnumItem>);
    registerSimple(L, EnumTag, "Enum", enumIndex, enumToString, enumEq);
    registerSimple(L, EnumsTag, "Enums", enumsIndex, enumsToString, nullptr);

    push<EnumsType>(L, EnumsType{});
    lua_setglobal(L, "Enum");
}

} // namespace lunar::lua
