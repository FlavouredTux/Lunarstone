#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#if __cpp_impl_reflection
#include <meta>
#endif

namespace lunar {

struct EnumInfo;

// One item of a Roblox enum, the Luau EnumItem: Enum.Material.Plastic has
// enumType Material, name "Plastic" and value 256. Items compare by enum and value.
struct EnumItem {
    const EnumInfo* enumType = nullptr;
    std::string_view name;
    std::uint32_t value = 0;

    friend bool operator==(const EnumItem& a, const EnumItem& b)
    {
        return a.enumType == b.enumType && a.value == b.value;
    }
};

// A Roblox enum (Enum.Material) and its items in declaration order.
struct EnumInfo {
    std::string_view name;
    std::span<const EnumItem> items;

    const EnumItem* find(std::string_view itemName) const;
    const EnumItem* find(std::uint32_t value) const;
};

// The Roblox enums the engine knows, as C++ enums so properties can use them
// directly. Item names and values match Roblox's, since place files store the
// value and scripts use the name. Every enum declared in this namespace is
// picked up by reflection: lunar::enums::find("Material") and EnumItem values
// need nothing else.
namespace Enum {

// All of them are 32 bits wide: that's how Roblox stores enum values.
// NOLINTBEGIN(performance-enum-size)
// clang-format off
enum class Material : std::uint32_t {
    Plastic = 256, SmoothPlastic = 272, Neon = 288, Wood = 512, WoodPlanks = 528, Marble = 784, Basalt = 788,
    Slate = 800, CrackedLava = 804, Concrete = 816, Limestone = 820, Granite = 832, Pavement = 836, Brick = 848,
    Pebble = 864, Cobblestone = 880, Rock = 896, Sandstone = 912, CorrodedMetal = 1040, DiamondPlate = 1056,
    Foil = 1072, Metal = 1088, Grass = 1280, LeafyGrass = 1284, Sand = 1296, Fabric = 1312, Snow = 1328, Mud = 1344,
    Ground = 1360, Asphalt = 1376, Salt = 1392, Ice = 1536, Glacier = 1552, Glass = 1568, ForceField = 1584,
    Air = 1792, Water = 2048, Cardboard = 2304, Carpet = 2305, CeramicTiles = 2306, ClayRoofTiles = 2307,
    RoofShingles = 2308, Leather = 2309, Plaster = 2310, Rubber = 2311,
};
enum class PartType : std::uint32_t { Ball, Block, Cylinder, Wedge, CornerWedge };
enum class FormFactor : std::uint32_t { Symmetric, Brick, Plate, Custom };
enum class SurfaceType : std::uint32_t {
    Smooth, Glue, Weld, Studs, Inlet, Universal, Hinge, Motor, SteppingMotor, SmoothNoOutlines = 10,
};
enum class NormalId : std::uint32_t { Right, Top, Back, Left, Bottom, Front };
enum class Axis : std::uint32_t { X, Y, Z };
enum class CollisionFidelity : std::uint32_t { Default, Hull, Box, PreciseConvexDecomposition, Tunable };
enum class RenderFidelity : std::uint32_t { Automatic, Precise, Performance };
enum class MeshType : std::uint32_t {
    Head, Torso, Wedge, Sphere, Cylinder, FileMesh, Brick, Prism, Pyramid, ParallelRamp, RightAngleRamp, CornerWedge,
};
enum class LevelOfDetailSetting : std::uint32_t { Low, Medium, High };
enum class ModelLevelOfDetail : std::uint32_t { Automatic, StreamingMesh, Disabled, SLIM = 4 };
enum class ModelStreamingMode : std::uint32_t { Default, Atomic, Persistent, PersistentPerPlayer, Nonatomic };

enum class HumanoidRigType : std::uint32_t { R6, R15 };
enum class HumanoidDisplayDistanceType : std::uint32_t { Viewer, Subject, None };
enum class HumanoidHealthDisplayType : std::uint32_t { DisplayWhenDamaged, AlwaysOn, AlwaysOff };
enum class HumanoidCollisionType : std::uint32_t { OuterBox, InnerBox };
enum class NameOcclusion : std::uint32_t { NoOcclusion, EnemyOcclusion, OccludeAll };
enum class BodyPart : std::uint32_t { Head, Torso, LeftArm, RightArm, LeftLeg, RightLeg };
enum class HumanoidStateType : std::uint32_t {
    FallingDown = 0, Ragdoll = 1, GettingUp = 2, Jumping = 3, Swimming = 4, Freefall = 5, Flying = 6, Landed = 7,
    Running = 8, RunningNoPhysics = 10, StrafingNoPhysics = 11, Climbing = 12, Seated = 13, PlatformStanding = 14,
    Dead = 15, Physics = 16, None = 18,
};
enum class RaycastFilterType : std::uint32_t { Exclude, Include };

enum class CameraType : std::uint32_t { Fixed, Attach, Watch, Track, Follow, Custom, Scriptable, Orbital };
enum class FieldOfViewMode : std::uint32_t { Vertical, Diagonal, MaxAxis };
enum class CameraMode : std::uint32_t { Classic, LockFirstPerson };
enum class DevCameraOcclusionMode : std::uint32_t { Zoom, Invisicam };
enum class DevComputerCameraMovementMode : std::uint32_t { UserChoice, Classic, Follow, Orbital, CameraToggle };
enum class DevTouchCameraMovementMode : std::uint32_t { UserChoice, Classic, Follow, Orbital };
enum class DevComputerMovementMode : std::uint32_t { UserChoice, KeyboardMouse, ClickToMove, Scriptable };
enum class DevTouchMovementMode : std::uint32_t {
    UserChoice, Thumbstick, DPad, Thumbpad, ClickToMove, Scriptable, DynamicThumbstick,
};

enum class RunContext : std::uint32_t { Legacy, Server, Client, Plugin };
enum class SignalBehavior : std::uint32_t { Default, Immediate, Deferred, AncestryDeferred };
enum class Technology : std::uint32_t { Legacy, Voxel, Compatibility, ShadowMap, Future, Unified };
enum class RollOffMode : std::uint32_t { Inverse, Linear, LinearSquare, InverseTapered };
enum class ReverbType : std::uint32_t {
    NoReverb, GenericReverb, PaddedCell, Room, Bathroom, LivingRoom, StoneRoom, Auditorium, ConcertHall, Cave, Arena,
    Hangar, CarpettedHallway, Hallway, StoneCorridor, Alley, Forest, City, Mountains, Quarry, Plain, ParkingLot,
    SewerPipe, UnderWater,
};
enum class ListenerType : std::uint32_t { Camera, CFrame, ObjectPosition, ObjectCFrame };

enum class FontWeight : std::uint32_t {
    Thin = 100, ExtraLight = 200, Light = 300, Regular = 400, Medium = 500, SemiBold = 600, Bold = 700,
    ExtraBold = 800, Heavy = 900,
};
enum class FontStyle : std::uint32_t { Normal, Italic };

enum class ParticleOrientation : std::uint32_t {
    FacingCamera, FacingCameraWorldUp, VelocityParallel, VelocityPerpendicular,
};
enum class ParticleEmitterShape : std::uint32_t { Box, Sphere, Cylinder, Disc };
enum class ParticleEmitterShapeInOut : std::uint32_t { Outward, Inward, InAndOut };
enum class ParticleEmitterShapeStyle : std::uint32_t { Volume, Surface };
enum class ParticleFlipbookLayout : std::uint32_t { None, Grid2x2, Grid4x4, Grid8x8, Custom };
enum class ParticleFlipbookMode : std::uint32_t { Loop, OneShot, PingPong, Random };
enum class TextureMode : std::uint32_t { Stretch, Wrap, Static };

enum class AnimationPriority : std::uint32_t {
    Idle, Movement, Action, Action2, Action3, Action4, Core = 1000,
};
enum class PoseEasingStyle : std::uint32_t { Linear, Constant, Elastic, Cubic, Bounce, CubicV2 };
enum class PoseEasingDirection : std::uint32_t { In, Out, InOut };
enum class ActuatorType : std::uint32_t { None, Motor, Servo };
enum class PositionAlignmentMode : std::uint32_t { OneAttachment, TwoAttachment };
enum class OrientationAlignmentMode : std::uint32_t { OneAttachment, TwoAttachment };
enum class AlphaMode : std::uint32_t { Overlay, Transparency, TintMask, Opaque };
enum class MaterialPattern : std::uint32_t { Regular, Organic };
enum class ProximityPromptExclusivity : std::uint32_t { OnePerButton, OneGlobally, AlwaysShow };
enum class ProximityPromptStyle : std::uint32_t { Default, Custom };
enum class ProximityPromptInputType : std::uint32_t { Keyboard, Gamepad, Touch };
enum class HighlightDepthMode : std::uint32_t { AlwaysOnTop, Occluded };
enum class AdornCullingMode : std::uint32_t { Automatic, Never };
enum class AdornShading : std::uint32_t { Default, Shaded, XRay, XRayShaded, AlwaysOnTop };
enum class HandlesStyle : std::uint32_t { Resize, Movement };
enum class FontSize : std::uint32_t {
    Size8, Size9, Size10, Size11, Size12, Size14, Size18, Size24, Size36, Size48, Size28, Size32, Size42, Size60, Size96
};
enum class UIFlexAlignment : std::uint32_t { None, Fill, SpaceAround, SpaceBetween, SpaceEvenly };
enum class UIFlexMode : std::uint32_t { None, Grow, Shrink, Fill, Custom };
enum class ItemLineAlignment : std::uint32_t { Automatic, Start, Center, End, Stretch };
enum class StrokeSizingMode : std::uint32_t { FixedSize, ScaledSize };
enum class BorderStrokePosition : std::uint32_t { Outer, Center, Inner };
enum class ScreenInsets : std::uint32_t { None, DeviceSafeInsets, CoreUISafeInsets, TopbarSafeInsets };
enum class ApplyShadowMode : std::uint32_t { Shape, Text };
enum class LightingStyle : std::uint32_t { Realistic, Soft };
enum class ScreenOrientation : std::uint32_t { LandscapeLeft, LandscapeRight, LandscapeSensor, Portrait, Sensor };
enum class ChatVersion : std::uint32_t { LegacyChatService, TextChatService };
// Where a chat message stands: Sending until the server answers, then
// Success or why it was refused.
enum class TextChatMessageStatus : std::uint32_t {
    Unknown = 1, Success, Sending, TextFilterFailed, Floodchecked, InvalidPrivacySettings,
    InvalidTextChannelPermissions, MessageTooLong, ModerationTimeout
};
enum class GameAvatarType : std::uint32_t { R6, R15, PlayerChoice };
enum class R15CollisionType : std::uint32_t { OuterBox, InnerBox };

// User interface.
enum class ZIndexBehavior : std::uint32_t { Global, Sibling };
enum class AutomaticSize : std::uint32_t { None, X, Y, XY };
enum class BorderMode : std::uint32_t { Outline, Middle, Inset };
enum class SizeConstraint : std::uint32_t { RelativeXY, RelativeXX, RelativeYY };
enum class FrameStyle : std::uint32_t { Custom, ChatBlue, RobloxSquare, RobloxRound, ChatGreen, ChatRed, DropShadow };
enum class ButtonStyle : std::uint32_t {
    Custom, RobloxButtonDefault, RobloxButton, RobloxRoundButton, RobloxRoundDefaultButton, RobloxRoundDropdownButton,
};
enum class ScaleType : std::uint32_t { Stretch, Slice, Tile, Fit, Crop };
enum class ResamplerMode : std::uint32_t { Default, Pixelated };
enum class TextXAlignment : std::uint32_t { Left, Right, Center };
enum class TextYAlignment : std::uint32_t { Top, Center, Bottom };
enum class TextTruncate : std::uint32_t { None, AtEnd, SplitWord };
enum class ElasticBehavior : std::uint32_t { WhenScrollable, Always, Never };
enum class ScrollBarInset : std::uint32_t { None, ScrollBar, Always };
enum class ScrollingDirection : std::uint32_t { X = 1, Y = 2, XY = 4 };
enum class VerticalScrollBarPosition : std::uint32_t { Right, Left };
enum class SurfaceGuiSizingMode : std::uint32_t { FixedSize, PixelsPerStud };
enum class SelectionBehavior : std::uint32_t { Escape, Stop };
enum class FillDirection : std::uint32_t { Horizontal, Vertical };
enum class HorizontalAlignment : std::uint32_t { Center, Left, Right };
enum class VerticalAlignment : std::uint32_t { Center, Top, Bottom };
enum class SortOrder : std::uint32_t { Name, Custom, LayoutOrder };
enum class StartCorner : std::uint32_t { TopLeft, TopRight, BottomLeft, BottomRight };
enum class AspectType : std::uint32_t { FitWithinMaxSize, ScaleWithParentSize };
enum class DominantAxis : std::uint32_t { Width, Height };
enum class EasingStyle : std::uint32_t {
    Linear,
    Sine,
    Back,
    Quad,
    Quart,
    Quint,
    Bounce,
    Elastic,
    Exponential,
    Circular,
    Cubic
};
enum class EasingDirection : std::uint32_t { In, Out, InOut };
enum class ApplyStrokeMode : std::uint32_t { Contextual, Border };
enum class LineJoinMode : std::uint32_t { Round, Bevel, Miter };
enum class AccessoryType : std::uint32_t {
    Unknown, Hat, Hair, Face, Neck, Shoulder, Front, Back, Waist, TShirt, Shirt, Pants, Jacket, Sweater, Shorts,
    LeftShoe, RightShoe, DressSkirt, Eyebrow, Eyelash,
};
enum class Font : std::uint32_t {
    Legacy = 0, Arial = 1, ArialBold = 2, SourceSans = 3, SourceSansBold = 4, SourceSansLight = 5,
    SourceSansItalic = 6, Bodoni = 7, Garamond = 8, Cartoon = 9, Code = 10, Highway = 11, SciFi = 12, Arcade = 13,
    Fantasy = 14, Antique = 15, SourceSansSemibold = 16, Gotham = 17, GothamMedium = 18, GothamBold = 19,
    GothamBlack = 20, AmaticSC = 21, Bangers = 22, Creepster = 23, DenkOne = 24, Fondamento = 25, FredokaOne = 26,
    GrenzeGotisch = 27, IndieFlower = 28, JosefinSans = 29, Jura = 30, Kalam = 31, LuckiestGuy = 32,
    Merriweather = 33, Michroma = 34, Nunito = 35, Oswald = 36, PatrickHand = 37, PermanentMarker = 38, Roboto = 39,
    RobotoCondensed = 40, RobotoMono = 41, Sarpanch = 42, SpecialElite = 43, TitilliumWeb = 44, Ubuntu = 45,
    BuilderSans = 46, BuilderSansMedium = 47, BuilderSansBold = 48, BuilderSansExtraBold = 49, Arimo = 50,
    ArimoBold = 51, Unknown = 100,
};

// Input.
enum class MouseBehavior : std::uint32_t { Default, LockCenter, LockCurrentPosition };
enum class RotationType : std::uint32_t { MovementRelative, CameraRelative };
enum class PreferredInput : std::uint32_t { KeyboardAndMouse, Gamepad, Touch, MicroGamepad };
enum class InputActionType : std::uint32_t { Bool, Direction1D, Direction2D, Direction3D, ViewportPosition };
enum class InputBindingType : std::uint32_t { Automatic, Scriptable };
enum class UserInputType : std::uint32_t {
    MouseButton1, MouseButton2, MouseButton3, MouseWheel, MouseMovement, Touch = 7, Keyboard, Focus, Accelerometer,
    Gyro, Gamepad1, Gamepad2, Gamepad3, Gamepad4, Gamepad5, Gamepad6, Gamepad7, Gamepad8, TextInput, InputMethod,
    None,
};
enum class UserInputState : std::uint32_t { Begin, Change, End, Cancel, None };
enum class KeyCode : std::uint32_t {
    None = 0, Backspace = 8, Tab = 9, Clear = 12, Return = 13, Pause = 19, Escape = 27, Space = 32,
    QuotedDouble = 34, Hash = 35, Dollar = 36, Percent = 37, Ampersand = 38, Quote = 39, LeftParenthesis = 40,
    RightParenthesis = 41, Asterisk = 42, Plus = 43, Comma = 44, Minus = 45, Period = 46, Slash = 47, Zero = 48,
    One = 49, Two = 50, Three = 51, Four = 52, Five = 53, Six = 54, Seven = 55, Eight = 56, Nine = 57, Colon = 58,
    Semicolon = 59, LessThan = 60, Equals = 61, GreaterThan = 62, Question = 63, At = 64, LeftBracket = 91,
    BackSlash = 92, RightBracket = 93, Caret = 94, Underscore = 95, Backquote = 96, A = 97, B = 98, C = 99, D = 100,
    E = 101, F = 102, G = 103, H = 104, I = 105, J = 106, K = 107, L = 108, M = 109, N = 110, O = 111, P = 112,
    Q = 113, R = 114, S = 115, T = 116, U = 117, V = 118, W = 119, X = 120, Y = 121, Z = 122, LeftCurly = 123,
    Pipe = 124, RightCurly = 125, Tilde = 126, Delete = 127, World0 = 160, World1 = 161, World2 = 162, World3 = 163,
    World4 = 164, World5 = 165, World6 = 166, World7 = 167, World8 = 168, World9 = 169, World10 = 170,
    World11 = 171, World12 = 172, World13 = 173, World14 = 174, World15 = 175, World16 = 176, World17 = 177,
    World18 = 178, World19 = 179, World20 = 180, World21 = 181, World22 = 182, World23 = 183, World24 = 184,
    World25 = 185, World26 = 186, World27 = 187, World28 = 188, World29 = 189, World30 = 190, World31 = 191,
    World32 = 192, World33 = 193, World34 = 194, World35 = 195, World36 = 196, World37 = 197, World38 = 198,
    World39 = 199, World40 = 200, World41 = 201, World42 = 202, World43 = 203, World44 = 204, World45 = 205,
    World46 = 206, World47 = 207, World48 = 208, World49 = 209, World50 = 210, World51 = 211, World52 = 212,
    World53 = 213, World54 = 214, World55 = 215, World56 = 216, World57 = 217, World58 = 218, World59 = 219,
    World60 = 220, World61 = 221, World62 = 222, World63 = 223, World64 = 224, World65 = 225, World66 = 226,
    World67 = 227, World68 = 228, World69 = 229, World70 = 230, World71 = 231, World72 = 232, World73 = 233,
    World74 = 234, World75 = 235, World76 = 236, World77 = 237, World78 = 238, World79 = 239, World80 = 240,
    World81 = 241, World82 = 242, World83 = 243, World84 = 244, World85 = 245, World86 = 246, World87 = 247,
    World88 = 248, World89 = 249, World90 = 250, World91 = 251, World92 = 252, World93 = 253, World94 = 254,
    World95 = 255, KeypadZero = 256, KeypadOne = 257, KeypadTwo = 258, KeypadThree = 259, KeypadFour = 260,
    KeypadFive = 261, KeypadSix = 262, KeypadSeven = 263, KeypadEight = 264, KeypadNine = 265, KeypadPeriod = 266,
    KeypadDivide = 267, KeypadMultiply = 268, KeypadMinus = 269, KeypadPlus = 270, KeypadEnter = 271,
    KeypadEquals = 272, Up = 273, Down = 274, Right = 275, Left = 276, Insert = 277, Home = 278, End = 279,
    PageUp = 280, PageDown = 281, F1 = 282, F2 = 283, F3 = 284, F4 = 285, F5 = 286, F6 = 287, F7 = 288, F8 = 289,
    F9 = 290, F10 = 291, F11 = 292, F12 = 293, F13 = 294, F14 = 295, F15 = 296, NumLock = 300, CapsLock = 301,
    ScrollLock = 302, RightShift = 303, LeftShift = 304, RightControl = 305, LeftControl = 306, RightAlt = 307,
    LeftAlt = 308, RightMeta = 309, LeftMeta = 310, LeftSuper = 311, RightSuper = 312, Mode = 313, Compose = 314,
    Help = 315, Print = 316, SysReq = 317, Break = 318, Menu = 319, Power = 320, Euro = 321, Undo = 322,
    ButtonX = 1000, ButtonY = 1001, ButtonA = 1002, ButtonB = 1003, ButtonR1 = 1004, ButtonL1 = 1005,
    ButtonR2 = 1006, ButtonL2 = 1007, ButtonR3 = 1008, ButtonL3 = 1009, ButtonStart = 1010, ButtonSelect = 1011,
    DPadLeft = 1012, DPadRight = 1013, DPadUp = 1014, DPadDown = 1015, Thumbstick1 = 1016, Thumbstick2 = 1017,
    Thumbstick1Up = 1018, Thumbstick1Down = 1019, Thumbstick1Left = 1020, Thumbstick1Right = 1021,
    Thumbstick2Up = 1022, Thumbstick2Down = 1023, Thumbstick2Left = 1024, Thumbstick2Right = 1025,
    MouseLeftButton = 1026, MouseRightButton = 1027, MouseMiddleButton = 1028, MouseBackButton = 1029,
    MouseNoButton = 1030, MouseX = 1031, MouseY = 1032, MousePosition = 1033, TouchPosition = 1034,
    MouseWheel = 1035, TrackpadPan = 1040, TrackpadPinch = 1045, MouseDelta = 1048, TouchDelta = 1049,
    TouchPinch = 1050, ButtonCenter = 1051, ButtonBack = 1052, ButtonUp = 1053, ButtonDown = 1054,
    ButtonLeft = 1055, ButtonRight = 1056,
};

// Tools and the core GUI.
enum class BinType : std::uint32_t { Script, GameTool, Grab, Clone, Hammer };
enum class CoreGuiType : std::uint32_t {
    PlayerList, Health, Backpack, Chat, All, EmotesMenu, SelfView, Captures, AvatarSwitcher, ExperienceShop,
};
// clang-format on
// NOLINTEND(performance-enum-size)

} // namespace Enum

namespace enums {

// Lookup by Roblox name ("Material"); null if the engine doesn't know the enum.
const EnumInfo* find(std::string_view name);
// Any of the client's enums by name, the engine's or not ("EasingStyle"):
// find, then the full catalog someone set (the script library sets the
// API dump's when a script context starts). For decoding values the engine
// doesn't model, such as a remote's arguments.
const EnumInfo* findAny(std::string_view name);
void setCatalog(const EnumInfo* (*lookup)(std::string_view name));
// Every enum in lunar::Enum, sorted by name.
std::span<const EnumInfo* const> all();

#if __cpp_impl_reflection

template <class E>
const EnumInfo& info()
{
    static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
    struct Table {
        EnumInfo info;
        std::array<EnumItem, enumerators.size()> items;
    };
    // The items point back at the table they live in.
    static const Table table = [] {
        Table t{{std::meta::identifier_of(^^E), {}}, {}};
        t.info.items = table.items;
        std::size_t i = 0;
        // clang-format doesn't know expansion statements yet.
        // clang-format off
        template for (constexpr std::meta::info enumerator : enumerators)
            t.items[i++] = EnumItem{&table.info, std::meta::identifier_of(enumerator), static_cast<std::uint32_t>([:enumerator:])};
        // clang-format on
        return t;
    }();
    return table.info;
}

#else

// No reflection (clangd): an empty enum with a usable identity.
template <class E>
const EnumInfo& info()
{
    static const EnumInfo empty{"Enum", {}};
    return empty;
}

#endif

// The EnumItem for a C++ enum value. An unnamed value keeps its number and gets an empty name.
template <class E>
EnumItem item(E e)
{
    const EnumInfo& enumInfo = info<E>();
    if (const EnumItem* found = enumInfo.find(static_cast<std::uint32_t>(e)))
        return *found;
    return EnumItem{&enumInfo, {}, static_cast<std::uint32_t>(e)};
}

} // namespace enums

} // namespace lunar
