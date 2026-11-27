#include <lunar/terrain/TerrainMaterials.hpp>

#include <lunar/classes/World.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace lunar::terrain {

static_assert(terrainMaterialCount == Terrain::materials.size());

namespace {

// Just enough JSON for the client's terrain files: a tree of objects,
// arrays, numbers, strings and literals.
struct Json;
using JsonObject = std::map<std::string, Json, std::less<>>;
using JsonArray = std::vector<Json>;

struct Json {
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<JsonArray>, std::shared_ptr<JsonObject>>
        value;

    const Json* get(std::string_view key) const
    {
        const auto* object = std::get_if<std::shared_ptr<JsonObject>>(&value);
        if (!object)
            return nullptr;
        const auto it = (*object)->find(key);
        return it == (*object)->end() ? nullptr : &it->second;
    }
    const JsonArray* array() const
    {
        const auto* a = std::get_if<std::shared_ptr<JsonArray>>(&value);
        return a ? a->get() : nullptr;
    }
    std::optional<double> number() const
    {
        const auto* d = std::get_if<double>(&value);
        return d ? std::optional{*d} : std::nullopt;
    }
    const std::string* string() const { return std::get_if<std::string>(&value); }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::expected<Json, std::string> document()
    {
        auto value = parseValue(0);
        if (value) {
            skipSpace();
            if (at_ != text_.size())
                return fail("trailing characters");
        }
        return value;
    }

private:
    static constexpr int maxDepth = 64;

    std::unexpected<std::string> fail(std::string_view what) const
    {
        return std::unexpected(std::format("JSON: {} at byte {}", what, at_));
    }

    void skipSpace()
    {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_])))
            ++at_;
    }

    bool take(char c)
    {
        skipSpace();
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    std::expected<std::string, std::string> parseString()
    {
        if (!take('"'))
            return fail("expected a string");
        std::string out;
        while (at_ < text_.size() && text_[at_] != '"') {
            char c = text_[at_++];
            if (c == '\\' && at_ < text_.size()) {
                // Escapes are rare here; keep the character after the backslash.
                c = text_[at_++];
                if (c == 'n')
                    c = '\n';
                else if (c == 't')
                    c = '\t';
            }
            out.push_back(c);
        }
        if (at_ >= text_.size())
            return fail("unterminated string");
        ++at_;
        return out;
    }

    std::expected<Json, std::string> parseValue(int depth)
    {
        if (depth > maxDepth)
            return fail("nested too deeply");
        skipSpace();
        if (at_ >= text_.size())
            return fail("unexpected end");
        const char c = text_[at_];
        if (c == '{') {
            ++at_;
            auto object = std::make_shared<JsonObject>();
            if (!take('}')) {
                do {
                    auto key = parseString();
                    if (!key)
                        return std::unexpected(key.error());
                    if (!take(':'))
                        return fail("expected ':'");
                    auto value = parseValue(depth + 1);
                    if (!value)
                        return value;
                    object->insert_or_assign(std::move(*key), std::move(*value));
                } while (take(','));
                if (!take('}'))
                    return fail("expected '}'");
            }
            return Json{std::move(object)};
        }
        if (c == '[') {
            ++at_;
            auto array = std::make_shared<JsonArray>();
            if (!take(']')) {
                do {
                    auto value = parseValue(depth + 1);
                    if (!value)
                        return value;
                    array->push_back(std::move(*value));
                } while (take(','));
                if (!take(']'))
                    return fail("expected ']'");
            }
            return Json{std::move(array)};
        }
        if (c == '"') {
            auto s = parseString();
            if (!s)
                return std::unexpected(s.error());
            return Json{std::move(*s)};
        }
        for (const auto& [word, value] : {std::pair<std::string_view, Json>{"true", Json{true}},
                                          std::pair<std::string_view, Json>{"false", Json{false}},
                                          std::pair<std::string_view, Json>{"null", Json{}}}) {
            if (text_.substr(at_).starts_with(word)) {
                at_ += word.size();
                return value;
            }
        }
        double number = 0.0;
        const char* begin = text_.data() + at_;
        const auto [end, error] = std::from_chars(begin, text_.data() + text_.size(), number);
        if (error != std::errc{} || end == begin)
            return fail("unexpected character");
        at_ += static_cast<std::size_t>(end - begin);
        return Json{number};
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

bool sameName(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

constexpr std::array<std::string_view, terrainMaterialCount> materialNames{
    "Air",     "Water",       "Grass", "Slate",      "Concrete", "Brick",     "Sand",     "WoodPlanks",
    "Rock",    "Glacier",     "Snow",  "Sandstone",  "Mud",      "Basalt",    "Ground",   "CrackedLava",
    "Asphalt", "Cobblestone", "Ice",   "LeafyGrass", "Salt",     "Limestone", "Pavement",
};

// Where a material of the file goes in Terrain::materials, by name.
std::optional<std::size_t> materialIndex(std::string_view name)
{
    for (std::size_t i = 0; i < materialNames.size(); ++i)
        if (sameName(materialNames[i], name))
            return i;
    return std::nullopt;
}

float numberOr(const Json* json, std::string_view key, float fallback)
{
    if (!json)
        return fallback;
    const Json* value = json->get(key);
    const std::optional<double> number = value ? value->number() : std::nullopt;
    return number ? static_cast<float>(*number) : fallback;
}

TerrainTextureParams textureParams(const Json& json, const TerrainTextureParams& fallback)
{
    TerrainTextureParams out = fallback;
    out.tiling = numberOr(&json, "tiling", out.tiling);
    out.detiling = numberOr(&json, "detiling", out.detiling);
    out.rotation = numberOr(&json, "rotation", out.rotation);
    if (const Json* blend = json.get("blend")) {
        out.blendMin = numberOr(blend, "min", out.blendMin);
        out.blendMax = numberOr(blend, "max", out.blendMax);
        out.blendScale = numberOr(blend, "scale", out.blendScale);
    }
    if (out.tiling <= 0.0f)
        out.tiling = fallback.tiling;
    return out;
}

// Calls fn(index, entry) for each material entry in the file.
template <typename Fn>
std::expected<void, std::string> forEachMaterial(std::string_view text, const Fn& fn)
{
    auto document = Parser(text).document();
    if (!document)
        return std::unexpected(document.error());
    const Json* list = document->get("materials");
    if (!list || !list->array())
        return std::unexpected("JSON: no materials list");
    for (const Json& entry : *list->array()) {
        const Json* name = entry.get("name");
        if (!name || !name->string())
            continue;
        if (const std::optional<std::size_t> index = materialIndex(*name->string()))
            fn(*index, entry);
    }
    return {};
}

} // namespace

std::expected<TerrainMaterialTable, std::string> parseTerrainMaterials(std::string_view json)
{
    TerrainMaterialTable table;
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i].name = materialNames[i];
    auto parsed = forEachMaterial(json, [&](std::size_t index, const Json& entry) {
        TerrainMaterialParams& m = table[index];
        m.name = *entry.get("name")->string();
        if (const Json* color = entry.get("base_color"); color && color->array() && color->array()->size() == 3) {
            const JsonArray& c = *color->array();
            m.baseColor
                = Color3::fromRGB(static_cast<std::uint8_t>(std::clamp(c[0].number().value_or(0.0), 0.0, 255.0)),
                                  static_cast<std::uint8_t>(std::clamp(c[1].number().value_or(0.0), 0.0, 255.0)),
                                  static_cast<std::uint8_t>(std::clamp(c[2].number().value_or(0.0), 0.0, 255.0)));
        }
        // A material level "blend" is the default for its textures.
        TerrainTextureParams base;
        if (const Json* blend = entry.get("blend")) {
            base.blendMin = numberOr(blend, "min", base.blendMin);
            base.blendMax = numberOr(blend, "max", base.blendMax);
            base.blendScale = numberOr(blend, "scale", base.blendScale);
        }
        if (const Json* all = entry.get("texture"))
            base = textureParams(*all, base);
        m.top = m.side = m.bottom = base;
        if (const Json* top = entry.get("texture_top"))
            m.top = textureParams(*top, base);
        if (const Json* side = entry.get("texture_side"))
            m.side = m.bottom = textureParams(*side, base);
        if (const Json* bottom = entry.get("texture_bottom"))
            m.bottom = textureParams(*bottom, m.side);
    });
    if (!parsed)
        return std::unexpected(parsed.error());
    return table;
}

std::expected<void, std::string> parseTerrainTransitions(std::string_view json, TerrainMaterialTable& table)
{
    return forEachMaterial(json, [&](std::size_t index, const Json& entry) {
        TerrainTransitionParams& t = table[index].transition;
        const Json* transition = entry.get("transition");
        t.dominance = numberOr(transition, "dominance", t.dominance);
        t.materialEdgeBlur = numberOr(transition, "materialEdgeBlur", t.materialEdgeBlur);
        t.noiseSize = numberOr(transition, "noiseSize", t.noiseSize);
        t.edgeDissolve = numberOr(transition, "edgeDissolve", t.edgeDissolve);
        for (const char* key : {"texture", "texture_top"}) {
            const Json* texture = entry.get(key);
            const Json* emissive = texture ? texture->get("hasEmissiveMask") : nullptr;
            if (emissive && std::holds_alternative<bool>(emissive->value) && std::get<bool>(emissive->value))
                t.emissiveStrength = numberOr(texture, "emissiveStrength", t.emissiveStrength);
        }
    });
}

} // namespace lunar::terrain
