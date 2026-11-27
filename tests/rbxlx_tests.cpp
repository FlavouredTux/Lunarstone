#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Rbxlx.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

using namespace lunar;

namespace {

// Shaped like what Studio writes, trimmed down.
constexpr std::string_view samplePlace
    = R"(<roblox xmlns:xmime="http://www.w3.org/2005/05/xmlmime" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://www.roblox.com/roblox.xsd" version="4">
	<Meta name="ExplicitAutoJoints">true</Meta>
	<External>null</External>
	<External>nil</External>
	<Item class="Workspace" referent="RBX0">
		<Properties>
			<float name="Gravity">196.2</float>
			<string name="Name">Workspace</string>
		</Properties>
		<Item class="Camera" referent="RBX1">
			<Properties>
				<string name="Name">Camera</string>
				<float name="FieldOfView">70</float>
			</Properties>
		</Item>
		<Item class="Model" referent="RBX2">
			<Properties>
				<string name="Name">Tower</string>
				<Ref name="PrimaryPart">RBX4</Ref>
			</Properties>
			<Item class="Part" referent="RBX3">
				<Properties>
					<string name="Name">Base</string>
					<bool name="Anchored">true</bool>
					<int name="BrickColor">194</int>
					<float name="Wobble">1</float>
					<CoordinateFrame name="CFrame">
						<X>0</X><Y>0.5</Y><Z>0</Z>
						<R00>1</R00><R01>0</R01><R02>0</R02>
						<R10>0</R10><R11>1</R11><R12>0</R12>
						<R20>0</R20><R21>0</R21><R22>1</R22>
					</CoordinateFrame>
					<Color3uint8 name="Color3uint8">4294901760</Color3uint8>
					<Vector3 name="size"><X>8</X><Y>1</Y><Z>8</Z></Vector3>
					<float name="Transparency">0.25</float>
				</Properties>
			</Item>
			<Item class="Part" referent="RBX4">
				<Properties>
					<string name="Name">Top &amp; Roof</string>
					<int name="BrickColor">21</int>
					<float name="Wobble">2</float>
					<CoordinateFrame name="CFrame">
						<X>0</X><Y>10</Y><Z>0</Z>
						<R00>0</R00><R01>0</R01><R02>1</R02>
						<R10>0</R10><R11>1</R11><R12>0</R12>
						<R20>-1</R20><R21>0</R21><R22>0</R22>
					</CoordinateFrame>
					<bool name="CanCollide">false</bool>
					<Color3 name="Color"><R>0</R><G>0.5</G><B>1</B></Color3>
				</Properties>
			</Item>
		</Item>
	</Item>
	<Item class="Lighting" referent="RBX5">
		<Properties>
			<string name="Name">Lighting</string>
			<float name="Brightness">2</float>
		</Properties>
	</Item>
	<Item class="ReplicatedStorage" referent="RBX6">
		<Properties>
			<string name="Name">ReplicatedStorage</string>
		</Properties>
		<Item class="FancyGadget" referent="RBX7">
			<Properties>
				<string name="Name">Spawner</string>
				<ProtectedString name="Source"><![CDATA[print("hi") -- <not a tag>]]></ProtectedString>
			</Properties>
			<Item class="Folder" referent="RBX8">
				<Properties>
					<string name="Name">Config</string>
				</Properties>
			</Item>
		</Item>
	</Item>
	<Item class="ServerScriptService" referent="RBX9">
		<Properties>
			<string name="Name">ServerScriptService</string>
		</Properties>
		<Item class="Script" referent="RBX10">
			<Properties>
				<string name="Name">Main</string>
				<bool name="Disabled">true</bool>
				<token name="RunContext">1</token>
				<ProtectedString name="Source"><![CDATA[print("hi") -- <not a tag>]]></ProtectedString>
			</Properties>
		</Item>
	</Item>
	<SharedStrings></SharedStrings>
</roblox>
)";

bool hasWarning(const std::vector<std::string>& warnings, std::string_view text)
{
    return std::ranges::any_of(warnings, [&](const std::string& w) { return w == text; });
}

} // namespace

TEST(rbxlx_builds_the_tree)
{
    auto place = rbxlx::loadPlace(samplePlace);
    CHECK(place.has_value());
    auto& game = *place->game;

    CHECK_EQ(game.children().size(), 4u);
    CHECK(game.findFirstChild("Workspace") == game.workspace());
    CHECK(game.findService("Workspace") == game.workspace());

    Instance* tower = game.workspace()->findFirstChild("Tower");
    CHECK(tower != nullptr);
    CHECK_EQ(tower->className(), "Model");
    CHECK_EQ(tower->children().size(), 2u);
    CHECK(game.workspace()->findFirstChild("Camera") != nullptr);
}

TEST(rbxlx_sets_part_properties)
{
    auto place = rbxlx::loadPlace(samplePlace);
    auto* base = dynamic_cast<Part*>(place->game->workspace()->findFirstChild("Base", true));
    CHECK(base != nullptr);
    CHECK(base->anchored());
    CHECK(base->size() == (Vector3{8.0f, 1.0f, 8.0f}));
    CHECK(base->position() == (Vector3{0.0f, 0.5f, 0.0f}));
    CHECK(base->color() == (Color3{1.0f, 0.0f, 0.0f}));
    CHECK_EQ(base->transparency(), 0.25f);
    CHECK(base->canCollide()); // not in the file, keeps the default

    auto* top = dynamic_cast<Part*>(place->game->workspace()->findFirstChild("Top & Roof", true));
    CHECK(top != nullptr);
    CHECK(!top->canCollide());
    CHECK(top->cframe().lookVector() == (Vector3{-1.0f, 0.0f, 0.0f}));
    CHECK(top->color() == (Color3{0.0f, 0.5f, 1.0f}));
}

TEST(rbxlx_resolves_refs)
{
    auto place = rbxlx::loadPlace(samplePlace);
    auto* tower = dynamic_cast<Model*>(place->game->workspace()->findFirstChild("Tower"));
    CHECK(tower != nullptr);
    CHECK(tower->primaryPart() != nullptr);
    CHECK_EQ(tower->primaryPart()->name(), "Top & Roof");
}

TEST(rbxlx_keeps_unknown_classes_as_placeholders)
{
    auto place = rbxlx::loadPlace(samplePlace);
    auto& game = *place->game;

    Instance* config = game.findFirstChild("Config", true);
    CHECK(config != nullptr);
    CHECK_EQ(config->className(), "Folder");
    CHECK_EQ(config->fullName(), "ReplicatedStorage.Spawner.Config");
    Instance* gadget = config->parent();
    CHECK_EQ(gadget->className(), "FancyGadget");
    CHECK(gadget->isA("Instance"));
    CHECK(dynamic_cast<UnknownInstance*>(gadget) != nullptr);

    InstancePtr copy = gadget->clone();
    CHECK(copy != nullptr);
    CHECK_EQ(copy->className(), "FancyGadget");
    CHECK(copy->findFirstChild("Config") != nullptr);

    // Placeholders still can't be made by name.
    CHECK(ClassRegistry::find("FancyGadget") == nullptr);
    CHECK_THROWS(ClassRegistry::create("FancyGadget"), Error);
}

TEST(rbxlx_loads_services_and_scripts)
{
    auto place = rbxlx::loadPlace(samplePlace);
    auto& game = *place->game;
    auto* lighting = dynamic_cast<Lighting*>(game.findFirstChild("Lighting"));
    CHECK(lighting != nullptr);
    CHECK(lighting == game.findService("Lighting"));
    CHECK_EQ(lighting->brightness(), 2.0f);
    CHECK_EQ(dynamic_cast<Workspace*>(game.workspace())->gravity(), 196.2f);
    CHECK_EQ(dynamic_cast<Camera*>(game.workspace()->findFirstChild("Camera"))->fieldOfView(), 70.0f);

    auto* script = dynamic_cast<Script*>(game.findFirstChild("Main", true));
    CHECK(script != nullptr);
    CHECK_EQ(script->source(), R"(print("hi") -- <not a tag>)");
    CHECK(script->disabled());
    CHECK(!script->enabled());
    CHECK(script->runContext() == Enum::RunContext::Server);
}

TEST(rbxlx_warns_about_bad_values_and_refs)
{
    auto model = rbxlx::loadModel(R"(<roblox version="4">
	<Item class="Model" referent="A">
		<Properties>
			<Ref name="PrimaryPart">NOPE</Ref>
		</Properties>
		<Item class="Part" referent="B">
			<Properties>
				<float name="Transparency">lots</float>
				<Vector3 name="size"><X>1</X><Y>2</Y></Vector3>
				<string name="Anchored">yes</string>
			</Properties>
		</Item>
	</Item>
</roblox>)");
    CHECK(model.has_value());
    CHECK_EQ(model->roots.size(), 1u);
    CHECK(model->roots[0]->parent() == nullptr);
    const auto& warnings = model->warnings;
    CHECK(hasWarning(warnings, "Model.PrimaryPart points at missing referent NOPE"));
    CHECK(hasWarning(warnings, "Part.Transparency: 'lots' is not a number"));
    CHECK(hasWarning(warnings, "Part.size: missing <Z>"));
    CHECK(hasWarning(warnings, "Part.Anchored: Unable to assign property Anchored. boolean expected, got string"));
}

TEST(rbxlx_rejects_bad_files)
{
    auto binary = rbxlx::loadPlace("<roblox!\x89\xff\r\n\x1a\n...");
    CHECK(!binary.has_value());
    CHECK_EQ(binary.error(), "this is a binary file, only the XML format is supported so far");

    auto broken = rbxlx::loadPlace("<roblox>\n<Item class=\"Part\">\n</roblox>");
    CHECK(!broken.has_value());
    CHECK_EQ(broken.error(), "XML error on line 3: mismatched closing tag, expected </Item>");

    auto wrongRoot = rbxlx::loadModel("<html></html>");
    CHECK_EQ(wrongRoot.error(), "expected a <roblox> root element, got <html>");

    CHECK(!readFile("/definitely/not/here.rbxlx").has_value());
}

TEST(rbxlx_shares_collision_blobs)
{
    // Two MeshParts naming one <SharedString> get the same bytes, not copies.
    auto model = rbxlx::loadModel(R"(<roblox version="4">
	<Item class="MeshPart">
		<Properties>
			<SharedString name="PhysicalConfigData">k1</SharedString>
		</Properties>
	</Item>
	<Item class="MeshPart">
		<Properties>
			<SharedString name="PhysicalConfigData">k1</SharedString>
			<BinaryString name="PhysicsData">Q1NHUEhT</BinaryString>
		</Properties>
	</Item>
	<SharedStrings>
		<SharedString md5="k1">Q1NHUEhTAAAAAEJMT0NL</SharedString>
	</SharedStrings>
</roblox>)");
    CHECK(model.has_value());
    CHECK(model->warnings.empty());
    CHECK_EQ(model->roots.size(), 2u);
    const auto& a = static_cast<const MeshPart&>(*model->roots[0]);
    const auto& b = static_cast<const MeshPart&>(*model->roots[1]);
    CHECK(a.physicalConfigData() && a.physicalConfigData() == b.physicalConfigData());
    CHECK(*a.physicalConfigData() == std::string("CSGPHS\0\0\0\0BLOCK", 15));
    CHECK(b.physicsData() && *b.physicsData() == "CSGPHS");
}
