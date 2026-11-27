#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/Value.hpp>

#include <algorithm>
#include <string>

using namespace lunar;

TEST(brickcolor_palette)
{
    CHECK_EQ(BrickColor::palette().size(), 208u);
    BrickColor red = BrickColor::fromNumber(21);
    CHECK_EQ(red.name(), "Bright red");
    CHECK(red.color() == Color3::fromRGB(196, 40, 28));
    CHECK_EQ(BrickColor{}.name(), "Medium stone grey");
    CHECK_EQ(BrickColor::fromNumber(4).number, 194); // not in the palette
    CHECK_EQ(BrickColor::fromName("Really black").number, 1003);
    CHECK_EQ(BrickColor::fromName("nope").number, 194);
    CHECK_EQ(BrickColor::nearest(Color3{1, 0, 0}).name(), "Really red");
    CHECK_EQ(BrickColor::nearest(Color3::fromRGB(160, 160, 165)).number, 194);
    CHECK(std::ranges::is_sorted(BrickColor::palette(), {}, &BrickColor::Entry::number));
}

TEST(enum_registry)
{
    const EnumInfo* material = enums::find("Material");
    CHECK(material != nullptr);
    CHECK_EQ(material->name, "Material");
    CHECK_EQ(material->find("Plastic")->value, 256u);
    CHECK_EQ(material->find(288u)->name, "Neon");
    CHECK(material->find("Lava") == nullptr);
    CHECK(enums::find("NotAnEnum") == nullptr);
    CHECK(&enums::info<Enum::Material>() == material);

    auto all = enums::all();
    CHECK(std::ranges::is_sorted(all, {}, &EnumInfo::name));
    CHECK(std::ranges::find(all, &enums::info<Enum::PartType>()) != all.end());

    EnumItem ball = enums::item(Enum::PartType::Ball);
    CHECK_EQ(ball.name, "Ball");
    CHECK_EQ(ball.value, 0u);
    CHECK(ball.enumType == enums::find("PartType"));
    CHECK(ball == *enums::find("PartType")->find("Ball"));
    CHECK(!(ball == enums::item(Enum::NormalId::Right))); // same value, other enum
    CHECK_EQ(enums::item(static_cast<Enum::PartType>(99)).name, "");
}

TEST(value_type_names)
{
    CHECK_EQ(typeName(Value(Vector2{1, 2})), "Vector2");
    CHECK_EQ(typeName(Value(UDim2{})), "UDim2");
    CHECK_EQ(typeName(Value(BrickColor{})), "BrickColor");
    CHECK_EQ(typeName(Value(enums::item(Enum::Material::Wood))), "EnumItem");
    CHECK_EQ(typeName(Value(NumberSequence(1.0f))), "NumberSequence");
    CHECK_EQ(typeName(Value(Font{})), "Font");
    CHECK_EQ(typeName(Value(Vector3int16{})), "Vector3int16");
}

TEST(value_to_string)
{
    CHECK_EQ(toString(Value(Vector2{1, 2.5f})), "1, 2.5");
    CHECK_EQ(toString(Value(UDim{0.5f, 10})), "0.5, 10");
    CHECK_EQ(toString(Value(UDim2{{1, 2}, {3, 4}})), "{1, 2}, {3, 4}");
    CHECK_EQ(toString(Value(Rect{{0, 1}, {2, 3}})), "0, 1, 2, 3");
    CHECK_EQ(toString(Value(BrickColor::fromNumber(1))), "White");
    CHECK_EQ(toString(Value(enums::item(Enum::Material::Neon))), "Enum.Material.Neon");
    CHECK_EQ(toString(Value(Faces{.right = true, .front = true})), "Right, Front");
    CHECK_EQ(toString(Value(Axes{.x = true, .z = true})), "X, Z");
    CHECK_EQ(toString(Value(NumberSequence(2.0f))), "0 2 0 1 2 0 ");
    CHECK_EQ(toString(Value(NumberRange{1, 2})), "1 2 ");
    CHECK_EQ(toString(Value(Vector3int16{1, -2, 3})), "1, -2, 3");
    CHECK_EQ(toString(Value(Font{})),
             "Font { Family = rbxasset://fonts/families/SourceSansPro.json, Weight = Regular, Style = Normal }");
}

TEST(faces_and_axes_bits)
{
    Faces f = Faces::fromBits(0b100101);
    CHECK(f == (Faces{.right = true, .back = true, .front = true}));
    CHECK_EQ(f.bits(), 0b100101);
    CHECK_EQ(Axes::fromBits(6).bits(), 6);
    CHECK(!Axes::fromBits(6).x);
}
