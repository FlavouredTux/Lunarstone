// The datatypes beyond Vector3/Color3/CFrame and the Enum global. Messages
// pinned here are the 0.742 client's own strings; tostring formats follow the
// engine's toString(Value).

#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>

#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

using namespace lunar;

// --- Enum ------------------------------------------------------------------------

TEST(script_enum_global_items_and_types)
{
    Harness h;
    CHECK_EQ(h.print("print(Enum.Material.Plastic, Enum.Material.Plastic.Value, Enum.Material.Plastic.Name)"),
             "Enum.Material.Plastic 256 Plastic");
    CHECK_EQ(h.print("print(Enum.Material, Enum, typeof(Enum), typeof(Enum.Material), typeof(Enum.Material.Wood))"),
             "Material Enums Enums Enum EnumItem");
    h.run(R"(
        assert(Enum.KeyCode.W == Enum.KeyCode.W and rawequal(Enum.KeyCode.W, Enum.KeyCode.W))
        assert(Enum.KeyCode.W ~= Enum.KeyCode.S)
        assert(Enum.Material == Enum.Material)
        local t = {[Enum.KeyCode.Space] = "jump"}
        assert(t[Enum.KeyCode.Space] == "jump")
        assert(Enum.Material.Wood.EnumType == Enum.Material)
        assert(Enum.Material.Wood:IsA("Material") and not Enum.Material.Wood:IsA("KeyCode"))
        assert(Enum.Material:FromName("Slate") == Enum.Material.Slate)
        assert(Enum.Material:FromValue(512) == Enum.Material.Wood)
        assert(Enum.Material:FromName("Nope") == nil and Enum.Material:FromValue(-5) == nil)
        -- Enums the engine doesn't model come from the client's tables.
        assert(Enum.UserInputType.Keyboard.Value == 8)
        assert(Enum.EasingStyle.Quad.Value == 3 and Enum.EasingDirection.Out.Value == 1)
        assert(Enum.SignalBehavior.Deferred.Value == 2)
        local items = Enum.EasingDirection:GetEnumItems()
        assert(#items == 3 and items[1] == Enum.EasingDirection.In and items[3] == Enum.EasingDirection.InOut)
        assert(#Enum:GetEnums() > 500)
    )");
    CHECK_EQ(h.error("local _ = Enum.Nope"), "Nope is not a valid member of \"Enum\"");
    CHECK_EQ(h.error("local _ = Enum.Material.Nope"), "Nope is not a valid member of \"Enum.Material\"");
    CHECK_EQ(h.error("local _ = Enum.Material.Wood.Nope"), "Nope is not a valid member of \"Enum.Material.Wood\"");
}

TEST(script_enum_items_assign_to_engine_properties)
{
    Harness h;
    h.run(R"(
        local p = Instance.new("Part")
        p.Material = Enum.Material.Neon
        assert(p.Material == Enum.Material.Neon)
        p.Material = "Wood"
        assert(p.Material == Enum.Material.Wood)
        p.Material = 784
        assert(p.Material == Enum.Material.Marble)
        p.Shape = Enum.PartType.Ball
        assert(p.Shape == Enum.PartType.Ball)
    )");
}

namespace {

// Just enough JSON for enums.json: {"Enum": {"Item": 1, ...}, ...}.
std::map<std::string, std::map<std::string, long long>> parseEnumsJson(std::string_view text)
{
    std::map<std::string, std::map<std::string, long long>> result;
    std::size_t i = 0;
    auto skip = [&] {
        while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ','))
            ++i;
    };
    auto string = [&] {
        std::string out;
        ++i; // opening quote
        while (i < text.size() && text[i] != '"')
            out += text[i++];
        ++i;
        return out;
    };
    skip();
    ++i; // {
    for (skip(); i < text.size() && text[i] == '"'; skip()) {
        std::string name = string();
        skip();
        ++i; // :
        skip();
        ++i; // {
        auto& items = result[name];
        for (skip(); text[i] == '"'; skip()) {
            std::string item = string();
            skip();
            ++i; // :
            skip();
            std::size_t end = i;
            while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-'))
                ++end;
            items[item] = std::stoll(std::string(text.substr(i, end - i)));
            i = end;
        }
        ++i; // }
    }
    return result;
}

} // namespace

// The engine's enums and the Enum global against the real client's values.
// Needs the research dump, so it only runs where that exists.
TEST(script_enum_values_match_the_client)
{
    const std::filesystem::path path = LUNAR_RESEARCH_DIR "/spelunk/enums.json";
    if (!std::filesystem::exists(path))
        SKIP("no spelunk/enums.json here");
    auto text = readFile(path);
    CHECK(text.has_value());
    auto client = parseEnumsJson(*text);
    CHECK(client.size() > 500);

    std::vector<std::string> mismatches;
    for (const EnumInfo* info : enums::all()) {
        auto it = client.find(std::string(info->name));
        // The engine's own LevelOfDetailSetting has no client counterpart by that name.
        if (it == client.end()) {
            if (info->name != "LevelOfDetailSetting")
                mismatches.push_back(std::format("Enum.{} isn't in the client", info->name));
            continue;
        }
        for (const EnumItem& item : info->items) {
            auto value = it->second.find(std::string(item.name));
            if (value == it->second.end())
                mismatches.push_back(std::format("Enum.{}.{} isn't in the client", info->name, item.name));
            else if (std::cmp_not_equal(value->second, item.value))
                mismatches.push_back(std::format("Enum.{}.{} is {}, the client says {}", info->name, item.name,
                                                 item.value, value->second));
        }
    }
    if (!mismatches.empty())
        lt::fail(std::format("{} engine enum mismatches, first: {}", mismatches.size(), mismatches.front()),
                 std::source_location::current());

    // Pin the surviving baseline items, including their values, through Lua.
    // The supported release may add items; a count from 0.742 cannot verify
    // today's catalog. ProjectServiceOperationResult was removed in 0.745.
    Harness h;
    for (const auto& [name, items] : client) {
        if (name == "ProjectServiceOperationResult")
            continue;
        std::string checks;
        for (const auto& [item, value] : items)
            checks += std::format("assert(Enum['{}']['{}'].Value == {})\n", name, item, value);
        h.run(checks);
    }
}

TEST(script_vector3_and_cframe_legacy_spellings)
{
    Harness h;
    h.run(R"(
        local v = Vector3.new(3, 4, 0)
        assert(v.magnitude == 5 and v.unit == v.Unit and v:lerp(Vector3.zero, 1) == Vector3.zero)
        assert(Vector3.FromNormalId(Enum.NormalId.Top) == Vector3.yAxis and Vector3.FromAxis(Enum.Axis.Z) == Vector3.zAxis)
        local cf = CFrame.new(1, 2, 3)
        assert(cf:toObjectSpace(cf) == CFrame.new() and cf:pointToWorldSpace(Vector3.zero) == cf.Position)
        assert(cf.rightVector == Vector3.xAxis and cf.upVector == Vector3.yAxis)
        assert(cf:FuzzyEq(CFrame.new(1, 2, 3.000001)))
        assert(math.abs(CFrame.new():AngleBetween(CFrame.Angles(0, 1, 0)) - 1) < 1e-5)
        local turn = CFrame.fromRotationBetweenVectors(Vector3.xAxis, Vector3.yAxis)
        assert((turn:VectorToWorldSpace(Vector3.xAxis) - Vector3.yAxis).Magnitude < 1e-5)
        assert((CFrame.lookAlong(Vector3.zero, Vector3.xAxis).LookVector - Vector3.xAxis).Magnitude < 1e-5)
    )");
}

// A key the code computes skips the VM's own X/Y/Z, so the metatable answers
// them (sUNC's obfuscated VM indexes everything that way).
TEST(script_vector3_answers_computed_keys)
{
    Harness h;
    CHECK_EQ(h.print(R"(
        local v, out = Vector3.new(1, 2, 3), {}
        for _, key in {"X", "Y", "Z", "x", "y", "z", "Magnitude"} do
            table.insert(out, string.format("%g", v[key]))
        end
        print(table.concat(out, " "))
    )"),
             "1 2 3 1 2 3 3.74166");
}

// --- Vector2, UDim, UDim2, Rect ------------------------------------------------------------

TEST(script_vector2)
{
    Harness h;
    h.run(R"(
        local v = Vector2.new(3, 4)
        assert(v.X == 3 and v.Y == 4 and v.Magnitude == 5)
        assert(v.Unit == Vector2.new(0.6, 0.8))
        assert(v + Vector2.one == Vector2.new(4, 5) and v - v == Vector2.zero)
        assert(v * 2 == Vector2.new(6, 8) and 2 * v == Vector2.new(6, 8) and v * v == Vector2.new(9, 16))
        assert(v / 2 == Vector2.new(1.5, 2) and Vector2.new(7, 9) // 2 == Vector2.new(3, 4))
        assert(-v == Vector2.new(-3, -4))
        assert(v:Dot(Vector2.xAxis) == 3 and Vector2.xAxis:Cross(Vector2.yAxis) == 1)
        assert(Vector2.zero:Lerp(Vector2.one, 0.5) == Vector2.new(0.5, 0.5))
        assert(Vector2.new(1, 5):Max(Vector2.new(4, 2)) == Vector2.new(4, 5))
        assert(Vector2.new(-1.5, 2.5):Floor() == Vector2.new(-2, 2) and Vector2.new(-1.5, 2.5):Abs() == Vector2.new(1.5, 2.5))
        assert(math.abs(Vector2.xAxis:Angle(Vector2.yAxis) - math.pi / 2) < 1e-6)
        assert(math.abs(Vector2.yAxis:Angle(Vector2.xAxis, true) + math.pi / 2) < 1e-6)
        assert(Vector2.new(1, 1):FuzzyEq(Vector2.new(1, 1.000001)))
        assert(typeof(v) == "Vector2" and Vector2.new() == Vector2.zero)
    )");
    CHECK_EQ(h.print("print(Vector2.new(1, 2.5))"), "1, 2.5");
    CHECK_EQ(h.error("local _ = Vector2.one * 'x'"),
             "attempt to multiply a Vector2 with an incompatible value type or nil");
    CHECK_EQ(h.error("local _ = Vector2.one.Nope"), "Nope is not a valid member of Vector2");
}

TEST(script_udim_udim2_rect)
{
    Harness h;
    h.run(R"(
        local u = UDim.new(0.5, 10)
        assert(u.Scale == 0.5 and u.Offset == 10)
        assert(u + UDim.new(0.25, 5) == UDim.new(0.75, 15) and u - u == UDim.new())
        local u2 = UDim2.new(0.5, 10, 1, -5)
        assert(u2.X == UDim.new(0.5, 10) and u2.Y == UDim.new(1, -5))
        assert(u2.Width == u2.X and u2.Height == u2.Y)
        assert(UDim2.new(UDim.new(1, 2), UDim.new(3, 4)) == UDim2.new(1, 2, 3, 4))
        assert(UDim2.fromScale(1, 0.5) == UDim2.new(1, 0, 0.5, 0))
        assert(UDim2.fromOffset(10, 20) == UDim2.new(0, 10, 0, 20))
        assert(UDim2.new():Lerp(UDim2.new(1, 100, 1, 100), 0.5) == UDim2.new(0.5, 50, 0.5, 50))
        assert(u2 + UDim2.new(0, 1, 0, 1) == UDim2.new(0.5, 11, 1, -4))
        local r = Rect.new(1, 2, 11, 22)
        assert(r.Min == Vector2.new(1, 2) and r.Max == Vector2.new(11, 22) and r.Width == 10 and r.Height == 20)
        assert(Rect.new(Vector2.new(1, 2), Vector2.new(11, 22)) == r)
    )");
    CHECK_EQ(h.print("print(UDim.new(0.5, 10), UDim2.new(0.5, 10, 1, -5), Rect.new(1, 2, 3, 4))"),
             "0.5, 10 {0.5, 10}, {1, -5} 1, 2, 3, 4");
}

// --- Ranges and sequences ----------------------------------------------------------

TEST(script_number_range_and_sequences)
{
    Harness h;
    h.run(R"(
        local r = NumberRange.new(1, 3)
        assert(r.Min == 1 and r.Max == 3 and NumberRange.new(2) == NumberRange.new(2, 2))
        local s = NumberSequence.new(0, 1)
        assert(#s.Keypoints == 2 and s.Keypoints[2].Value == 1 and s.Keypoints[2].Time == 1)
        local k = NumberSequenceKeypoint.new(0.5, 2, 0.1)
        assert(k.Time == 0.5 and k.Value == 2 and math.abs(k.Envelope - 0.1) < 1e-6)
        local custom = NumberSequence.new({NumberSequenceKeypoint.new(0, 1), k, NumberSequenceKeypoint.new(1, 0)})
        assert(#custom.Keypoints == 3)
        local c = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1))
        assert(c.Keypoints[1].Value == Color3.new(1, 0, 0) and c.Keypoints[2].Time == 1)
        assert(ColorSequence.new(Color3.new(1, 1, 1)) == ColorSequence.new(Color3.new(1, 1, 1), Color3.new(1, 1, 1)))
    )");
    CHECK_EQ(h.print("print(NumberRange.new(1, 2))"), "1 2 ");
    CHECK_EQ(h.print("print(NumberSequence.new(0, 1))"), "0 0 0 1 1 0 ");
    CHECK_EQ(h.print("print(ColorSequence.new(Color3.new(1, 0, 0)))"), "0 1 0 0 0 1 1 0 0 0 ");
    CHECK_EQ(h.error("NumberRange.new(2, 1)"), "NumberRange: invalid range");
    CHECK_EQ(h.error("NumberSequence.new({NumberSequenceKeypoint.new(0, 1)})"),
             "NumberSequence: requires at least 2 keypoints");
    CHECK_EQ(h.error("NumberSequence.new({NumberSequenceKeypoint.new(0, 1), NumberSequenceKeypoint.new(0.5, 1)})"),
             "NumberSequence must end at time=1.0");
    CHECK_EQ(h.error("NumberSequence.new({NumberSequenceKeypoint.new(0.1, 1), NumberSequenceKeypoint.new(1, 1)})"),
             "NumberSequence must start at time=0.0");
    CHECK_EQ(h.error("NumberSequence.new({1, 2})"),
             "NumberSequence.new(): expected 'NumberSequenceKeypoint' at index 1");
    CHECK_EQ(h.error("ColorSequence.new(5)"), "ColorSequence.new(): table expected.");
}

// --- BrickColor, Faces, Axes, PhysicalProperties, Vector3int16, Font -----------------

TEST(script_brick_color)
{
    Harness h;
    h.run(R"(
        local b = BrickColor.new("Bright red")
        assert(b.Number == 21 and b.Name == "Bright red" and b.Color == Color3.fromRGB(196, 40, 28))
        assert(BrickColor.new(21) == b and BrickColor.Red() == b)
        assert(BrickColor.new(Color3.fromRGB(196, 40, 28)) == b)
        assert(BrickColor.new(1, 0, 0) == BrickColor.new(Color3.new(1, 0, 0)))
        assert(BrickColor.White().Number == 1 and BrickColor.Black().Number == 26)
        assert(BrickColor.new("no such color") == BrickColor.new(194))
        assert(typeof(BrickColor.random()) == "BrickColor")
        assert(math.abs(b.r - 196 / 255) < 1e-6)
    )");
    CHECK_EQ(h.print("print(BrickColor.new(194))"), "Medium stone grey");
    CHECK_EQ(h.error("BrickColor.palette(500)"), "palette index out of bounds (500)");
}

TEST(script_faces_axes_physical_properties)
{
    Harness h;
    h.run(R"(
        local f = Faces.new(Enum.NormalId.Top, Enum.NormalId.Front)
        assert(f.Top and f.Front and not f.Left)
        local a = Axes.new(Enum.Axis.X, Enum.NormalId.Top)
        assert(a.X and a.Y and not a.Z and a.Left and a.Top and not a.Front)
        local p = PhysicalProperties.new(Enum.Material.Plastic)
        assert(math.abs(p.Density - 0.7) < 1e-6 and math.abs(p.Friction - 0.3) < 1e-6 and p.FrictionWeight == 1)
        local q = PhysicalProperties.new(1, 0.5, 0.25, 2, 3)
        assert(q.Density == 1 and q.Elasticity == 0.25 and q.FrictionWeight == 2 and q.ElasticityWeight == 3)
    )");
    CHECK_EQ(h.print("print(Faces.new(Enum.NormalId.Top, Enum.NormalId.Front), Axes.new(Enum.Axis.X, Enum.Axis.Z))"),
             "Top, Front X, Z");
    CHECK_EQ(h.print("print(PhysicalProperties.new(1, 0.5, 0.25))"), "1, 0.5, 0.25, 1, 1");
    CHECK_EQ(h.error("Faces.new(1)"), "Faces.new expects Enum.NormalId inputs");
    CHECK_EQ(h.error("Axes.new(Enum.Material.Wood)"), "Axes.new expects Enum.Axis or Enum.NormalId inputs");
    CHECK(h.error("PhysicalProperties.new(1, 2)").starts_with("Invalid number of arguments: 2. Four ways"));
}

TEST(script_vector3int16_and_font)
{
    Harness h;
    h.run(R"(
        local v = Vector3int16.new(1, 2, 3)
        assert(v.X == 1 and v.Y == 2 and v.Z == 3)
        assert(v + Vector3int16.new(1, 1, 1) == Vector3int16.new(2, 3, 4) and v * 2 == Vector3int16.new(2, 4, 6))
        local f = Font.fromEnum(Enum.Font.GothamBold)
        assert(f.Family == "rbxasset://fonts/families/GothamSSm.json" and f.Weight == Enum.FontWeight.Bold and f.Bold)
        assert(Font.fromName("Roboto").Family == "rbxasset://fonts/families/Roboto.json")
        assert(Font.fromId(123).Family == "rbxassetid://123")
        assert(Font.new("x", Enum.FontWeight.Light, Enum.FontStyle.Italic).Style == Enum.FontStyle.Italic)
        assert(Font.fromEnum(Enum.Font.SourceSansItalic).Style == Enum.FontStyle.Italic)
        -- Font's members can be assigned.
        local g = Font.new("rbxasset://fonts/families/SourceSansPro.json")
        g.Family = "rbxasset://fonts/families/Roboto.json"
        g.Bold = true
        assert(g.Family == "rbxasset://fonts/families/Roboto.json" and g.Weight == Enum.FontWeight.Bold and g.Bold)
        g.Bold = false
        g.Style = Enum.FontStyle.Italic
        assert(g.Weight == Enum.FontWeight.Regular and g.Style == Enum.FontStyle.Italic)
        assert(not pcall(function() g.Size = 3 end))
    )");
    CHECK_EQ(h.print("print(Vector3int16.new(1, -2, 3))"), "1, -2, 3");
    CHECK_EQ(h.print("print(Font.fromEnum(Enum.Font.SourceSans))"),
             "Font { Family = rbxasset://fonts/families/SourceSansPro.json, Weight = Regular, Style = Normal }");
    CHECK_EQ(h.error("Font.fromEnum(Enum.Font.Unknown)"), "Enum.Font.Unknown can't be passed to Font.fromEnum");
}

TEST(script_new_datatypes_round_trip_through_properties)
{
    Harness h;
    h.run(R"(
        local light = Instance.new("PointLight")
        light.Color = Color3.new(1, 0, 0)
        local emitter = Instance.new("ParticleEmitter")
        emitter.Size = NumberSequence.new(1, 2)
        emitter.Color = ColorSequence.new(Color3.new(0, 1, 0))
        emitter.Lifetime = NumberRange.new(1, 5)
        assert(emitter.Size == NumberSequence.new(1, 2))
        assert(emitter.Lifetime == NumberRange.new(1, 5))
        local part = Instance.new("Part")
        part.BrickColor = BrickColor.new("Bright blue")
        assert(part.BrickColor == BrickColor.new("Bright blue"))
        local frame = Instance.new("Frame")
        frame.Size = UDim2.new(0.5, 10, 0, 20)
        assert(frame.Size == UDim2.new(0.5, 10, 0, 20))
    )");
}

// --- Script-only datatypes ----------------------------------------------------------

TEST(script_tween_info_ray_region)
{
    Harness h;
    h.run(R"(
        local t = TweenInfo.new()
        assert(t.Time == 1 and t.EasingStyle == Enum.EasingStyle.Quad and t.EasingDirection == Enum.EasingDirection.Out)
        assert(t.RepeatCount == 0 and t.Reverses == false and t.DelayTime == 0)
        local t2 = TweenInfo.new(2, Enum.EasingStyle.Linear, Enum.EasingDirection.In, 3, true, 0.5)
        assert(t2.Time == 2 and t2.RepeatCount == 3 and t2.Reverses and t2.DelayTime == 0.5)
        local r = Ray.new(Vector3.zero, Vector3.new(0, 0, -10))
        assert(r.Origin == Vector3.zero and r.Direction == Vector3.new(0, 0, -10) and r.Unit.Direction == Vector3.new(0, 0, -1))
        assert(r:ClosestPoint(Vector3.new(5, 0, -3)) == Vector3.new(0, 0, -3) and r:Distance(Vector3.new(5, 0, -3)) == 5)
        local region = Region3.new(Vector3.new(0, 0, 0), Vector3.new(4, 2, 6))
        assert(region.Size == Vector3.new(4, 2, 6) and region.CFrame.Position == Vector3.new(2, 1, 3))
        assert(Region3.new(Vector3.new(1, 1, 1), Vector3.new(3, 3, 3)):ExpandToGrid(4).Size == Vector3.new(4, 4, 4))
    )");
    CHECK_EQ(h.print("print(TweenInfo.new(2, Enum.EasingStyle.Linear))"),
             "Time:2 DelayTime:0 RepeatCount:0 Reverses:False EasingDirection:Out EasingStyle:Linear");
    CHECK_EQ(h.print("print(Ray.new(Vector3.new(1, 2, 3), Vector3.new(0, 0, -1)))"), "{1, 2, 3}, {0, 0, -1}");
    CHECK_EQ(h.error("TweenInfo.new('x')"), "TweenInfo.new first argument expects a number for time.");
    // TweenInfo.new also accepts numeric values for easing enums.
    CHECK_EQ(h.print("local t = TweenInfo.new(1, 2, 1) print(t.EasingStyle, t.EasingDirection)"),
             "Enum.EasingStyle.Back Enum.EasingDirection.Out");
    CHECK_EQ(h.error("TweenInfo.new(1, {})"), "TweenInfo.new second argument expects Enum.EasingStyle input");
}

TEST(script_date_time)
{
    Harness h;
    h.run(R"(
        local t = DateTime.fromUniversalTime(2020, 1, 2, 10, 30, 45, 123)
        assert(t.UnixTimestampMillis == 1577961045123 and t.UnixTimestamp == 1577961045)
        local u = t:ToUniversalTime()
        assert(u.Year == 2020 and u.Month == 1 and u.Day == 2 and u.Hour == 10 and u.Minute == 30)
        assert(u.Second == 45 and u.Millisecond == 123)
        assert(t:ToIsoDate() == "2020-01-02T10:30:45Z")
        assert(DateTime.fromIsoDate("2020-01-02T10:30:45Z").UnixTimestamp == 1577961045)
        assert(DateTime.fromIsoDate("2020-01-02T12:30:45+02:00").UnixTimestamp == 1577961045)
        assert(DateTime.fromIsoDate("garbage") == nil)
        assert(DateTime.fromUnixTimestamp(1577961045) == DateTime.fromUnixTimestampMillis(1577961045000))
        assert(t:FormatUniversalTime("YYYY-MM-DD HH:mm:ss dddd MMMM Do", "en-us") == "2020-01-02 10:30:45 Thursday January 2nd")
        assert(DateTime.now().UnixTimestamp > 1577961045)
        assert(DateTime.fromUnixTimestamp(1) < DateTime.fromUnixTimestamp(2))
    )");
}

TEST(script_random_is_deterministic_per_seed)
{
    Harness h;
    h.run(R"(
        local a, b = Random.new(42), Random.new(42)
        for _ = 1, 10 do
            assert(a:NextInteger(1, 100) == b:NextInteger(1, 100))
            local n = a:NextNumber()
            assert(n == b:NextNumber() and n >= 0 and n < 1)
        end
        local c = a:Clone()
        assert(c:NextNumber(5, 10) == a:NextNumber(5, 10))
        for _ = 1, 100 do
            local i = a:NextInteger(-3, 3)
            assert(i >= -3 and i <= 3 and i == math.floor(i))
        end
        assert(math.abs(a:NextUnitVector().Magnitude - 1) < 1e-5)
        local t = {1, 2, 3, 4, 5}
        a:Shuffle(t)
        table.sort(t)
        assert(t[1] == 1 and t[5] == 5)
    )");
    // Arguments past the ones used are ignored, as the client does; only a
    // lone NextNumber bound raises.
    h.run(R"(
        assert(Random.new(7, 2):NextNumber() == Random.new(7):NextNumber())
        local x = Random.new(1):NextNumber(5, 10, "extra")
        assert(x >= 5 and x <= 10)
    )");
    CHECK_EQ(h.error("Random.new():NextNumber(5)"), "Random:NextNumber requires 0 or 2 arguments");
    CHECK_EQ(h.error("Random.new():Shuffle({1, nil, 3})"),
             concat("Attempted to shuffle table containing nil values. This is not ",
                    "allowed because the table length may change."));
}

TEST(script_raycast_and_overlap_params)
{
    Harness h;
    h.run(R"(
        local p = RaycastParams.new()
        assert(p.FilterType == Enum.RaycastFilterType.Exclude and p.IgnoreWater == false and p.CollisionGroup == "Default")
        local a, b = Instance.new("Part"), Instance.new("Part")
        p.FilterDescendantsInstances = {a, {b}}
        assert(#p.FilterDescendantsInstances == 2)
        p:AddToFilter(Instance.new("Folder"))
        assert(#p.FilterDescendantsInstances == 3)
        p.FilterType = Enum.RaycastFilterType.Include
        p.IgnoreWater = true
        assert(p.FilterType == Enum.RaycastFilterType.Include and p.IgnoreWater)
        local o = OverlapParams.new()
        o.MaxParts = 5
        assert(o.MaxParts == 5)
        assert(RaycastParams.new({IgnoreWater = true}).IgnoreWater)
    )");
}

TEST(script_vector2_has_lower_case_x_and_y)
{
    Harness h;
    CHECK_EQ(h.print("local v = Vector2.new(3, 4) print(v.x, v.y, v.magnitude)"), "3 4 5");
}

TEST(script_a_part_makes_its_cframe_a_rotation_like_studio)
{
    // Every number here is what Studio's BasePart.CFrame gave back for the same
    // assignment: Gram-Schmidt over the columns (right, up, then back projected),
    // the identity for what can't be one, and (0, -1e6, 0) for a NaN position.
    Harness h;
    h.run(R"lua(
        local p = Instance.new("Part")
        local function near(cf, want, what, within)
            local got = {cf:GetComponents()}
            for i = 1, 12 do
                assert(math.abs(got[i] - want[i]) < (within or 1e-4),
                    string.format("%s: component %d is %s, Studio's %s", what, i, tostring(got[i]), tostring(want[i])))
            end
        end
        local identity = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}
        p.CFrame = CFrame.Angles(0.3, 0.5, 0.7) * CFrame.new(0, 0, 0, 1.01, 0.02, 0, -0.01, 0.99, 0.03, 0, 0.01, 1)
        near(p.CFrame, {0, 0, 0, 0.67678, -0.55381, 0.48504, 0.71744, 0.64389, -0.26586, -0.16508, 0.52792, 0.83310}, "noisy")
        p.CFrame = CFrame.new(1, 2, 3, 2, 0.3, 0, 0, 1.5, 0, 0, 0, 1)
        near(p.CFrame, {1, 2, 3, 1, 0, 0, 0, 1, 0, 0, 0, 1}, "skewed")
        for name, cf in {
            ["right zero"] = CFrame.new(0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1),
            ["back zero"] = CFrame.new(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0),
            ["up along right"] = CFrame.new(0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1),
            ["all zero"] = CFrame.new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        } do
            p.CFrame = cf
            near(p.CFrame, identity, name)
        end
        p.CFrame = CFrame.new(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, -1)
        near(p.CFrame, {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, -1}, "left-handed")
        p.CFrame = CFrame.new(0/0, 1, 2)
        near(p.CFrame, {0, -1e6, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, "NaN position")

        -- A model moved by its primary part each frame stays a rigid model (it
        -- grew without bound, then went NaN: the game's characters vanished).
        local m = Instance.new("Model")
        local root = Instance.new("Part", m)
        local arm = Instance.new("Part", m)
        m.PrimaryPart = root
        arm.CFrame = CFrame.new(3, 0, 0)
        for i = 1, 200 do
            m:SetPrimaryPartCFrame(CFrame.new(i * 0.01, 0, 0) * CFrame.Angles(0, i * 0.37, 0)
                * CFrame.new(0, 0, 0, 1.001, 0, 0, 0, 1, 0.002, 0, 0, 1))
        end
        near(root.CFrame, {2, 0, 0, 0.17172, 0, -0.98515, 0, 1, 0, 0.98515, 0, 0.17172}, "root after 200", 1e-3)
        near(arm.CFrame, {2.51522, 0, 2.95583, 0.17172, 0, -0.98515, 0, 1, 0, 0.98515, 0, 0.17172}, "arm after 200", 1e-3) -- 200 steps of float rounding

        -- CFrame.fromMatrix without a back vector: right x up made a unit, and
        -- zeros (not NaN) where there's none to make.
        near(CFrame.fromMatrix(Vector3.zero, Vector3.xAxis * 2, Vector3.yAxis * 3), {0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 1}, "fromMatrix scaled")
        near(CFrame.fromMatrix(Vector3.zero, Vector3.zero, Vector3.zero), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, "fromMatrix zero")
        near(CFrame.fromMatrix(Vector3.zero, Vector3.xAxis, Vector3.xAxis), {0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0}, "fromMatrix parallel")
    )lua");
}
