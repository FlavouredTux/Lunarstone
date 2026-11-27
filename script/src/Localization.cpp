// LocalizationTable, Translator and LocalizationService's translators.

#include "InstanceApi.hpp"
#include "Runtime.hpp"
#include "Userdata.hpp"

#include <lunar/Classes.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::lua {

namespace {

int newTranslator(lua_State* L, std::string_view locale, LocalizationTable* table)
{
    auto translator = std::make_shared<Translator>();
    translator->setName("Translator");
    translator->assignLocale(std::string(locale));
    translator->assignTable(table);
    return returnInstance(L, translator.get());
}

// LocalizationService:GetTranslatorForPlayerAsync(player) (and the older
// GetTranslatorForPlayer, TopbarPlus's) and GetTranslatorForLocaleAsync(locale): a Translator for the locale. The
// game's localization tables aren't fetched, so it has nothing to translate
// with. GUESS: a player's locale is en-us (Player.LocaleId isn't modelled);
// the client yields while it loads the tables, this returns at once.
int localizationGetTranslatorForPlayer(lua_State* L)
{
    self(L);
    if (!dynamic_cast<Player*>(toInstance(L, 2)))
        raise(L, "Argument 1 missing or nil");
    return newTranslator(L, "en-us", nullptr);
}

int localizationGetTranslatorForLocale(lua_State* L)
{
    self(L);
    return newTranslator(L, checkString(L, 2), nullptr);
}

LocalizationTable& checkTable(lua_State* L)
{
    return static_cast<LocalizationTable&>(self(L));
}

// A field of the table at idx that's a string, or empty.
std::string stringField(lua_State* L, int idx, const char* name)
{
    lua_getfield(L, idx, name);
    std::string out;
    if (lua_type(L, -1) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        out.assign(text, length);
    }
    lua_pop(L, 1);
    return out;
}

// LocalizationTable:SetEntries(entries): an array of {Key, Source, Context,
// Example, Values = {[locale] = text}} replacing what the table held.
int tableSetEntries(lua_State* L)
{
    LocalizationTable& table = checkTable(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<LocalizationTable::Entry> entries;
    const int count = lua_objlen(L, 2);
    entries.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 2, i);
        if (!lua_istable(L, -1))
            raise(L, std::format("SetEntries: entry {} is not a table", i));
        const int entry = lua_gettop(L);
        LocalizationTable::Entry e{.key = stringField(L, entry, "Key"),
                                   .source = stringField(L, entry, "Source"),
                                   .context = stringField(L, entry, "Context"),
                                   .example = stringField(L, entry, "Example"),
                                   .values = {}};
        lua_getfield(L, entry, "Values");
        if (lua_istable(L, -1))
            for (lua_pushnil(L); lua_next(L, -2) != 0; lua_pop(L, 1))
                if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING)
                    e.values.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
        lua_pop(L, 2);
        entries.push_back(std::move(e));
    }
    table.setEntries(std::move(entries));
    return 0;
}

int tableGetEntries(lua_State* L)
{
    const LocalizationTable& table = checkTable(L);
    const auto& entries = table.entries();
    lua_createtable(L, static_cast<int>(entries.size()), 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const LocalizationTable::Entry& e = entries[i];
        lua_createtable(L, 0, 5);
        for (const auto& [name, text] : std::array<std::pair<const char*, const std::string*>, 4>{
                 {{"Key", &e.key}, {"Source", &e.source}, {"Context", &e.context}, {"Example", &e.example}}}) {
            pushString(L, *text);
            lua_setfield(L, -2, name);
        }
        lua_createtable(L, 0, static_cast<int>(e.values.size()));
        for (const auto& [locale, text] : e.values) {
            pushString(L, text);
            lua_setfield(L, -2, locale.c_str());
        }
        lua_setfield(L, -2, "Values");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// GetString(targetLocale, key).
int tableGetString(lua_State* L)
{
    const LocalizationTable& table = checkTable(L);
    const std::string_view locale = checkString(L, 2), key = checkString(L, 3);
    const auto text = table.textOf(key, locale);
    if (!text)
        raise(L, std::format("LocalizationTable:GetString: no translation for key \"{}\" in locale {}", key, locale));
    pushString(L, *text);
    return 1;
}

// A LocalizationTable's own Translator for a locale, which reads the table.
int tableGetTranslator(lua_State* L)
{
    LocalizationTable& table = checkTable(L);
    return newTranslator(L, checkString(L, 2), &table);
}

// Roblox's format strings: {name} takes args.name, {1} args[1], each with an
// optional :format after the key. GUESS: int rounds, fixed shows two
// decimals, anything else shows the value as tostring does; a placeholder
// with no argument stays as written.
std::string formatWithArgs(lua_State* L, std::string_view text, int argsIdx)
{
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t open = text.find('{', i);
        const std::size_t close = open == std::string_view::npos ? open : text.find('}', open);
        if (close == std::string_view::npos) {
            out += text.substr(i);
            break;
        }
        out += text.substr(i, open - i);
        const std::string_view inner = text.substr(open + 1, close - open - 1);
        const std::string_view key = inner.substr(0, inner.find(':'));
        const std::string_view format = key.size() < inner.size() ? inner.substr(key.size() + 1) : std::string_view();
        i = close + 1;
        if (!lua_istable(L, argsIdx)) {
            out += text.substr(open, close - open + 1);
            continue;
        }
        int index = 0;
        const auto [end, ec] = std::from_chars(key.data(), key.data() + key.size(), index);
        if (ec == std::errc() && end == key.data() + key.size()) {
            lua_rawgeti(L, argsIdx, index);
        } else {
            lua_pushlstring(L, key.data(), key.size());
            lua_rawget(L, argsIdx);
        }
        if (lua_isnil(L, -1)) {
            out += text.substr(open, close - open + 1);
        } else if (lua_type(L, -1) == LUA_TNUMBER && format == "int") {
            out += std::format("{}", std::llround(lua_tonumber(L, -1)));
        } else if (lua_type(L, -1) == LUA_TNUMBER && format == "fixed") {
            out += std::format("{:.2f}", lua_tonumber(L, -1));
        } else {
            std::size_t length = 0;
            const char* value = luaL_tolstring(L, -1, &length);
            out.append(value, length);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return out;
}

Translator& checkTranslator(lua_State* L)
{
    return static_cast<Translator&>(self(L));
}

// Translator:FormatByKey(key, args): the key's text in the translator's
// locale with the arguments put in. An error when there's none, which the
// app's libraries catch to fall back to en-us.
int translatorFormatByKey(lua_State* L)
{
    const Translator& translator = checkTranslator(L);
    const std::string_view key = checkString(L, 2);
    const LocalizationTable* table = translator.table();
    const auto text = table ? table->textOf(key, translator.localeId()) : std::nullopt;
    if (!text)
        raise(L, std::format("Translator:FormatByKey: no translation for key \"{}\" in locale {}", key,
                             translator.localeId()));
    const std::string formatted = formatWithArgs(L, *text, 3);
    pushString(L, formatted);
    return 1;
}

// Translator:Translate(context, text): the text's translation where the
// table has one, else the text as it is, which is what a game written in
// English shows an en-us player [live: Midnight Racing's server list builds
// each entry through it, and stopped at the first].
int translatorTranslate(lua_State* L)
{
    const Translator& translator = checkTranslator(L);
    const std::string_view text = checkString(L, 3);
    const LocalizationTable* table = translator.table();
    const auto translated = table ? table->translationOf(text, translator.localeId()) : std::nullopt;
    pushString(L, translated ? std::string_view(*translated) : text);
    return 1;
}

constexpr auto methods = std::to_array<Method>({
    {"GetTranslatorForPlayerAsync", "LocalizationService", localizationGetTranslatorForPlayer},
    {"GetTranslatorForPlayer", "LocalizationService", localizationGetTranslatorForPlayer},
    {"GetTranslatorForLocaleAsync", "LocalizationService", localizationGetTranslatorForLocale},
    {"SetEntries", "LocalizationTable", tableSetEntries},
    {"GetEntries", "LocalizationTable", tableGetEntries},
    {"GetString", "LocalizationTable", tableGetString},
    {"GetTranslator", "LocalizationTable", tableGetTranslator},
    {"FormatByKey", "Translator", translatorFormatByKey},
    {"Translate", "Translator", translatorTranslate},
});

} // namespace

std::span<const Method> localizationMethods()
{
    return methods;
}

} // namespace lunar::lua
