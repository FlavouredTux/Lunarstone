#include <lunar/render/BuiltinMaterials.hpp>

#include <algorithm>

namespace lunar::render {

namespace {

using enum Enum::Material;

// In slot order. The 2022 column is the set registered when the byte at
// 0x1078761e8 is set (the newer uploads); with it clear the client registers
// an older upload of the same 2022 textures (9920... ids) instead.
constexpr std::array<BuiltinMaterial, builtinMaterialSlots> materials{{
    {Plastic, {}, {}},                                                                                            // 0
    {SmoothPlastic, {}, {}},                                                                                      // 1
    {Snow, {9439632387, 9439632006, 1.0f, true}, {106058971152252, 113863498687141, 1.0f, true}},                 // 2
    {Sandstone, {9873521380, 9439596530, 1.0f, true}, {109869731739139, 86625778869912, 1.0f, true}},             // 3
    {Neon, {}, {}},                                                                                               // 4
    {Wood, {7547303225, 7547190548, 1.0f, false}, {73489845523797, 93921150047881, 1.0f, true}},                  // 5
    {WoodPlanks, {7547332968, 7547188159, 2.0f, true}, {78212019144684, 137375490342653, 1.0f, true}},            // 6
    {Marble, {7547177270, 7547176060, 2.0f, false}, {104749709354369, 120223557836613, 1.0f, true}},              // 7
    {Slate, {7547298114, 7547297808, 1.0f, false}, {130174615834794, 78438403538623, 1.0f, true}},                // 8
    {Concrete, {7546653951, 7546653707, 1.0f, false}, {115089551732435, 137750621165234, 1.0f, true}},            // 9
    {Granite, {7547164710, 7546654648, 1.0f, false}, {125518382244973, 72852823001993, 1.0f, true}},              // 10
    {Brick, {7546650097, 7546649654, 2.0f, true}, {85077012310177, 95213713051668, 1.0f, true}},                  // 11
    {Pebble, {7547291361, 7546645052, 1.0f, false}, {87820215580582, 99000704060990, 1.0f, true}},                // 12
    {CorrodedMetal, {7547184629, 7547181182, 1.0f, true}, {122577965407434, 79556365947166, 1.0f, true}},         // 13
    {DiamondPlate, {7547162198, 7546654536, 1.0f, false}, {124320368131155, 100666168561647, 1.0f, true}},        // 14
    {Foil, {7546645012, 7546644903, 1.0f, false}, {110583221457338, 89740061683493, 1.0f, true}},                 // 15
    {Metal, {7547288171, 7547287997, 1.0f, false}, {71003343637114, 118738373988974, 1.0f, true}},                // 16
    {Grass, {7547169285, 7547168653, 2.0f, false}, {106637943980559, 125141052412328, 1.0f, true}},               // 17
    {Sand, {7547295153, 7547294810, 1.0f, false}, {117101875286338, 130221748450775, 1.0f, true}},                // 18
    {Fabric, {7547101130, 7547100915, 1.0f, true}, {132264823269423, 81464013824650, 1.0f, true}},                // 19
    {Ice, {7547171356, 7547171198, 1.0f, false}, {91952184132897, 118351132266496, 1.0f, true}},                  // 20
    {Cobblestone, {7546652947, 7546652689, 2.0f, true}, {129616831094994, 110426889496760, 1.0f, true}},          // 21
    {Glass, {7547304948, 7547304785, 1.0f, true}, {88609722573469, 113911490507844, 1.0f, true}},                 // 22
    {ForceField, {}, {}},                                                                                         // 23
    {Mud, {9873319819, 9439509827, 1.0f, true}, {81832637858593, 112041271350774, 1.0f, true}},                   // 24
    {Limestone, {9873289812, 9439415191, 1.0f, true}, {136792039489433, 82212494843564, 1.0f, true}},             // 25
    {Basalt, {9873270487, 9438412214, 1.0f, true}, {103124999245988, 102996046482757, 1.0f, true}},               // 26
    {Ground, {9439044431, 9439043558, 1.0f, true}, {122356188009715, 118762838103482, 1.0f, true}},               // 27
    {LeafyGrass, {9873288083, 9439080781, 1.0f, true}, {75665810846977, 74631298728768, 1.0f, true}},             // 28
    {Rock, {9873515198, 9439538417, 1.0f, true}, {112102163340384, 82372403221805, 1.0f, true}},                  // 29
    {Pavement, {9873322398, 9439519281, 1.0f, true}, {100821413010093, 124047559416349, 1.0f, true}},             // 30
    {Glacier, {9438851661, 9438812958, 1.0f, true}, {85752927082668, 102830670878238, 1.0f, true}},               // 31
    {CrackedLava, {9438582231, 9438508790, 1.0f, true}, {121276091081821, 75628265850422, 1.0f, true}},           // 32
    {Salt, {9439566986, 9439565809, 1.0f, true}, {137066264708856, 131613202488773, 1.0f, true}},                 // 33
    {Asphalt, {9873267379, 9429449876, 1.0f, true}, {77679553820203, 71222370436216, 1.0f, true}},                // 34
    {Cardboard, {14108654559, 14108654002, 1.0f, true}, {112994935967349, 119546905525688, 1.0f, true}},          // 35
    {Carpet, {14108663921, 14108663154, 1.0f, true}, {119478636506120, 75396874665519, 1.0f, true}},              // 36
    {CeramicTiles, {17429458668, 17429425915, 1.0f, true}, {126380627739075, 90712331270927, 1.0f, true}},        // 37
    {ClayRoofTiles, {18147685875, 18147683410, 1.0f, true}, {114168170326154, 78625983931327, 1.0f, true}},       // 38
    {RoofShingles, {91579661064923, 77534750680073, 1.0f, true}, {107847976513485, 121573023089057, 1.0f, true}}, // 39
    {Leather, {14108671010, 14108670486, 1.0f, true}, {111918862958038, 133238057809217, 1.0f, true}},            // 40
    {Plaster, {14108672569, 14108671870, 1.0f, true}, {104555660139073, 100805432506296, 1.0f, true}},            // 41
    {Rubber, {14108675388, 14108674698, 1.0f, true}, {112374357118714, 72883976673716, 1.0f, true}},              // 42
}};

static_assert(std::ranges::all_of(materials,
                                  [](const BuiltinMaterial& m) {
                                      return (m.pre2022.pack == 0) == (m.v2022.pack == 0);
                                  }),
              "a material has textures in one set only");

} // namespace

std::optional<std::size_t> builtinMaterialSlot(Enum::Material material)
{
    const auto it = std::ranges::find(materials, material, &BuiltinMaterial::material);
    if (it == materials.end())
        return std::nullopt;
    return static_cast<std::size_t>(it - materials.begin());
}

const BuiltinMaterial& builtinMaterial(std::size_t slot)
{
    return materials.at(slot);
}

std::optional<BuiltinMaterialPack> builtinMaterialPack(Enum::Material material, bool use2022)
{
    const std::optional<std::size_t> slot = builtinMaterialSlot(material);
    if (!slot)
        return std::nullopt;
    const BuiltinMaterialPack& pack = use2022 ? materials[*slot].v2022 : materials[*slot].pre2022;
    if (pack.pack == 0)
        return std::nullopt;
    return pack;
}

bool partTintMask(Enum::Material material, const BuiltinMaterialPack& pack)
{
    return pack.tintMask && material != Glass;
}

} // namespace lunar::render
