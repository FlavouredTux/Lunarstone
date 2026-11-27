#include "framework.hpp"
#include "script_harness.hpp"

#include <lunar/PlaceFile.hpp>

using namespace lunar;

TEST(emotes_serialized_description_properties_survive_loading_and_cloning)
{
    auto model = loadModel(R"(<roblox version="4"><Item class="HumanoidDescription"><Properties>
        <string name="EmotesDataInternal">Wave^25^10^25^\Smile^30^\</string>
        <string name="EquippedEmotesDataInternal">3^Wave\1^Smile\</string>
        </Properties></Item></roblox>)");
    CHECK(model.has_value());
    auto* description = instanceCast<HumanoidDescription>(model->roots.front().get());
    CHECK(description != nullptr);
    CHECK_EQ(description->emotes().at("Wave"), (std::set<std::int64_t>{10, 25}));
    CHECK_EQ(description->equippedEmotes().at(3), "Wave");
    CHECK_EQ(description->emotesDataInternal(), "Smile^30^\\Wave^10^25^\\");
    CHECK_EQ(description->equippedEmotesDataInternal(), "1^Smile\\3^Wave\\");
    auto clone = std::dynamic_pointer_cast<HumanoidDescription>(description->clone());
    CHECK(clone != nullptr);
    clone->removeEmote("Wave");
    CHECK(description->emotes().contains("Wave"));
    CHECK(!clone->emotes().contains("Wave"));
}

TEST(emotes_script_api_normalizes_ids_and_fires_change_events)
{
    Harness h;
    h.run(R"(
        local description = Instance.new("HumanoidDescription")
        local changed = 0
        description.EmotesChanged:Connect(function(emotes)
            changed += 1
            assert(type(emotes) == "table")
        end)
        description:SetEmotes({Wave={25, 10, 25, "ignored"}, Empty={}, NotAnArray=false})
        local emotes = description:GetEmotes()
        assert(#emotes.Wave == 2 and emotes.Wave[1] == 10 and emotes.Wave[2] == 25)
        assert(emotes.Empty == nil and emotes.NotAnArray == nil)
        emotes.Wave[1] = -1
        assert(description:GetEmotes().Wave[1] == 10)
        description:AddEmote("Wave", 10)
        description:AddEmote("Smile", 30)
        description:RemoveEmote("Wave")
        assert(description:GetEmotes().Wave == nil)
        assert(changed == 4)
        print("emotes ready")
    )");
    CHECK_EQ(h.lines, std::vector<std::string>{"emotes ready"});
}

TEST(emotes_equipped_slots_accept_names_and_explicit_slot_tables)
{
    Harness h;
    h.run(R"(
        local description = Instance.new("HumanoidDescription")
        description:SetEmotes({Wave={10}, Smile={30}})
        local count = 0
        description.EquippedEmotesChanged:Connect(function(slots)
            count += 1
            assert(#slots == 2)
        end)
        description:SetEquippedEmotes({"Wave", "Smile"})
        local slots = description:GetEquippedEmotes()
        assert(slots[1].Slot == 1 and slots[1].Name == "Wave")
        description:SetEquippedEmotes({{Slot=5, Name="Wave"}, {Slot=2, Name="Smile"}})
        slots = description:GetEquippedEmotes()
        assert(slots[1].Slot == 2 and slots[1].Name == "Smile")
        assert(slots[2].Slot == 5 and slots[2].Name == "Wave")
        assert(count == 2)
        local clone = description:Clone()
        clone:AddEmote("Other", 50)
        assert(description:GetEmotes().Other == nil)
        print("slots ready")
    )");
    CHECK_EQ(h.lines, std::vector<std::string>{"slots ready"});
}

TEST(emotes_applied_description_snapshot_keeps_non_archivable_avatar_data)
{
    Harness h;
    auto character = std::make_shared<Model>();
    character->setName("Rig");
    character->setParent(h.game->workspace());
    auto humanoid = std::make_shared<Humanoid>();
    humanoid->setParent(character);
    auto description = std::make_shared<HumanoidDescription>();
    description->setArchivable(false);
    description->addEmote("Wave", 10);
    description->setEquippedEmotes({{1, "Wave"}});
    description->setParent(humanoid);
    h.run(R"(
        local snapshot = workspace.Rig.Humanoid:GetAppliedDescription()
        assert(snapshot:GetEmotes().Wave[1] == 10)
        assert(snapshot:GetEquippedEmotes()[1].Name == "Wave")
        snapshot:RemoveEmote("Wave")
        assert(workspace.Rig.Humanoid:GetAppliedDescription():GetEmotes().Wave[1] == 10)
        print("snapshot ready")
    )");
    CHECK_EQ(h.lines, std::vector<std::string>{"snapshot ready"});
}
