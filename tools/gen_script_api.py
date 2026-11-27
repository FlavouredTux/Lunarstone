#!/usr/bin/env python3
"""Generates the scripting API from the 0.742 player reflection baseline and
the supported release's official API dump and player strings. Existing
player-only members survive; current API types and security replace the old
ones. Newly introduced classes/enums and missing members must occur in the
current player binary. Longstanding Studio-only classes stay excluded.

    tools/gen_script_api.py spelunk-dir --api-dump API-Dump.json \\
        --player-strings player-strings.txt [--output RobloxApiData.cpp]

The spelunk directory supplies enums.json, reflection.json and the baseline API dump
(apidump/API-Dump-0.742.0.7421053.json). The target version is read from
engine/include/lunar/RobloxVersion.hpp. Without explicit input paths, the new
dump and strings are apidump/API-Dump-<version>.json and strings-<version>.txt
under spelunk. Strings may be plain output from `strings -a` or spelunk's
address|text listing. The generated output is checked in; builds need none
of these inputs and never fetch API metadata at runtime.
"""

import argparse
import hashlib
import json
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
version = re.search(r'robloxVersion\[\] = "([^"]+)"',
                    (root / "engine/include/lunar/RobloxVersion.hpp").read_text())[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("spelunk", type=pathlib.Path)
parser.add_argument("--api-dump", type=pathlib.Path)
parser.add_argument("--player-strings", type=pathlib.Path)
parser.add_argument("--output", type=pathlib.Path, default=root / "script/src/RobloxApiData.cpp")
args = parser.parse_args()
spelunk = args.spelunk
out = args.output

enums = json.loads((spelunk / "enums.json").read_text())
classes = json.loads((spelunk / "reflection.json").read_text())
baseline_api = json.loads((spelunk / "apidump" / "API-Dump-0.742.0.7421053.json").read_text())
api_path = args.api_dump or spelunk / "apidump" / f"API-Dump-{version}.json"
api_dump = json.loads(api_path.read_text())

# Reflection's C++ property types -> the Luau type a script sees.
cpp_types = {
    "bool": "boolean",
    "float": "number",
    "double": "number",
    "int": "number",
    "long long": "number",
    "unsigned int": "number",
    "std::basic_string<char, std::char_traits<char>, std::allocator<char>>": "string",
    "Content": "Content",
    "ContentId": "string",
    "BinaryString": "string",
    "SharedString": "string",
    "ProtectedString": "string",
    "CoordinateFrame": "CFrame",
    "OptionalCoordinateFrame": "CFrame",
    "Rect2D": "Rect",
    "Text::FontInfo": "Font",
    "Color3uint8": "Color3",
}

member_kinds = {
    "PropDescriptor": "Property",
    "EnumPropDescriptor": "Property",
    "RefPropDescriptor": "Property",
    "NetAssetPropDescriptor": "Property",
    "BoundGetterDesc": "Property",
    "BoundFuncDesc": "Function",
    "CustomBoundFuncDesc": "Function",
    "BoundYieldFuncDesc": "YieldFunction",
    "BoundAsyncCallbackDesc": "Callback",
    "EventDesc": "Event",
}


# Members the dump misses (registered outside the reflection tables it walks)
# and parameter names it mistook for members.
extra_members = {
    "BasePart": {"Touched": "EventDesc", "TouchEnded": "EventDesc"},
    "BaseRemoteEvent": {"OnServerEvent": "EventDesc", "OnClientEvent": "EventDesc"},
    "RemoteFunction": {"OnServerInvoke": "BoundAsyncCallbackDesc", "OnClientInvoke": "BoundAsyncCallbackDesc"},
    "BindableFunction": {"OnInvoke": "BoundAsyncCallbackDesc"},
    "MarketplaceService": {"ProcessReceipt": "BoundAsyncCallbackDesc"},
    "TextChatService": {name: "BoundAsyncCallbackDesc" for name in (
        "OnIncomingMessage", "OnBubbleAdded", "OnChatWindowAdded")},
    "TextChannel": {"OnIncomingMessage": "BoundAsyncCallbackDesc", "ShouldDeliverCallback": "BoundAsyncCallbackDesc"},
    "SocialService": {"OnCallInviteInvoked": "BoundAsyncCallbackDesc"},
    "RunService": {"RenderStepped": "EventDesc", "Stepped": "EventDesc"},
    "DataModel": {"BindToClose": "BoundFuncDesc"},
    "UserInputService": {name: "EventDesc" for name in (
        "InputBegan", "InputChanged", "InputEnded", "TouchStarted", "TouchMoved", "TouchEnded")},
}
# Enum items newer than the 0.742 dumps that live games use: JJS skips
# unbound InputBindings by Enum.KeyCode.Unknown (missing from 0.743's API
# dump too). GUESS: its value, 0 like None, which an unbound binding reads.
extra_enum_items = {
    "KeyCode": {"Unknown": 0},
}
# The dump names some callbacks after their first argument ("message" for
# TextChatService.OnIncomingMessage); those lower-case callbacks go, and the
# real names come from extra_members.
bogus_members = {"arguments", "player", "receiptInfo"}
for cls, members in extra_members.items():
    if cls not in classes:
        continue
    for name, kind in members.items():
        classes[cls]["members"].setdefault(name, {"kind": kind, "type": "void"})


# Members the API dump has and the reflection dump missed, for the classes the
# player has. The Studio API alone is not evidence that a player has a member.
def dump_member_kind(m):
    t = m["MemberType"]
    if t == "Property":
        return "PropDescriptor"
    if t == "Function":
        return "BoundYieldFuncDesc" if "Yields" in m.get("Tags", []) else "BoundFuncDesc"
    if t == "Event":
        return "EventDesc"
    return "BoundAsyncCallbackDesc"


def dump_member_type(m):
    v = m.get("ValueType")
    if not v:
        return "void"
    if v["Category"] == "Enum":
        return v["Name"]
    primitive = {"bool": "bool", "int": "int", "int64": "long long", "float": "float", "double": "double",
                 "string": "std::basic_string<char, std::char_traits<char>, std::allocator<char>>"}
    return primitive.get(v["Name"], v["Name"])


player_strings = set()
strings_path = args.player_strings or spelunk / f"strings-{version}.txt"
for line in strings_path.read_text(errors="replace").splitlines():
    fields = line.split("|", 1)
    player_strings.add(fields[1] if len(fields) == 2 and re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", fields[0].strip())
                       else line)

baseline_classes = {c["Name"]: c for c in baseline_api["Classes"]}
dump_classes = {c["Name"]: c for c in api_dump["Classes"]}
# A known API class removed since the baseline must not be resurrected by the
# older reflection data. Private player classes absent from both dumps stay.
for name in baseline_classes.keys() - dump_classes.keys():
    classes.pop(name, None)
for name, info in dump_classes.items():
    if name not in classes and name not in baseline_classes and name in player_strings:
        tags = info.get("Tags", [])
        classes[name] = {"base": info["Superclass"], "members": {},
                         "creatable": "NotCreatable" not in tags, "service": "Service" in tags}

baseline_enums = {e["Name"] for e in baseline_api["Enums"]}
dump_enums = {e["Name"]: e for e in api_dump["Enums"]}
for name in baseline_enums - dump_enums.keys():
    enums.pop(name, None)
for name, info in dump_enums.items():
    if name in enums or (name not in baseline_enums and name in player_strings):
        enums[name] = {item["Name"]: item["Value"] for item in info["Items"]}
for name, items in extra_enum_items.items():
    for item, value in items.items():
        enums[name].setdefault(item, value)

for dump_class in api_dump["Classes"]:
    info = classes.get(dump_class["Name"])
    if info is None:
        continue
    current_names = {m["Name"] for m in dump_class["Members"]}
    old = baseline_classes.get(dump_class["Name"], {})
    for removed in {m["Name"] for m in old.get("Members", [])} - current_names:
        info["members"].pop(removed, None)
    for m in dump_class["Members"]:
        if m["Name"] not in info["members"] and m["Name"] not in player_strings:
            continue
        kind = dump_member_kind(m)
        if kind == "PropDescriptor" and m["ValueType"]["Category"] == "Enum":
            kind = "EnumPropDescriptor"
        elif kind == "PropDescriptor" and m["ValueType"]["Category"] == "Class":
            kind = "RefPropDescriptor"
        info["members"][m["Name"]] = {"kind": kind, "type": dump_member_type(m)}
for info in classes.values():
    for name in bogus_members:
        info["members"].pop(name, None)
    for name in [n for n, m in info["members"].items()
                 if m.get("kind") == "BoundAsyncCallbackDesc" and n[:1].islower()]:
        info["members"].pop(name)


# Enums whose C++ name in the reflection isn't their Luau name.
enum_names = {
    "RenderQualitySetting": "SavedQualitySetting",  # UserGameSettings.SavedQualityLevel
}


def luau_type(member):
    kind = member["kind"]
    if kind == "EnumPropDescriptor":
        name = member["type"].split("::")[-1]
        return "Enum." + enum_names.get(name, name)
    if kind == "RefPropDescriptor":
        return member["type"]
    if kind in ("PropDescriptor", "NetAssetPropDescriptor", "BoundGetterDesc"):
        t = member["type"]
        return cpp_types.get(t, t)
    return ""


# The API dump's security levels -> the capability a script needs for them,
# as the client numbers capabilities (bit 0 Plugin ... bit 5 NotAccessible,
# names at 0x1016fa07b), stored as bit + 1 with 0 for none.
security_capability = {
    "None": 0,
    "PluginSecurity": 1,         # Plugin
    "LocalUserSecurity": 2,      # LocalUser
    "WritePlayerSecurity": 3,    # WritePlayer
    "RobloxScriptSecurity": 4,   # RobloxScript
    "RobloxSecurity": 5,         # RobloxEngine (GUESS: the pairing)
    "RobloxEngineSecurity": 5,
    "NotAccessibleSecurity": 6,  # NotAccessible
}


def security(class_name, member_name):
    """(read, write) capabilities of a member; the class's bases are searched
    in case the dump declares it higher up."""
    seen = set()
    while class_name and class_name not in seen:
        seen.add(class_name)
        info = dump_classes.get(class_name)
        if info is None:
            break
        for m in info["Members"]:
            if m["Name"] == member_name:
                s = m.get("Security", "None")
                if isinstance(s, dict):
                    return security_capability[s["Read"]], security_capability[s["Write"]]
                return security_capability[s], security_capability[s]
        class_name = info.get("Superclass")
    return 0, 0


def scriptable(class_name, member_name):
    """Whether a script reaches the property by indexing: the dump tags the
    ones it doesn't NotScriptable, and leaves out the player's own hidden
    ones (BasePart.siz, Humanoid.Health_XML) altogether. A class the dump
    doesn't have is the player's own, and its members are left as they are."""
    if class_name not in dump_classes:
        return True
    seen = set()
    while class_name and class_name not in seen:
        seen.add(class_name)
        info = dump_classes.get(class_name)
        if info is None:
            break
        for m in info["Members"]:
            if m["Name"] == member_name:
                return "NotScriptable" not in (m.get("Tags") or [])
        class_name = info.get("Superclass")
    return False


def is_service(name):
    seen = set()
    while name and name not in seen:
        seen.add(name)
        info = classes.get(name)
        if info is None:
            return False
        if info.get("service") or info["base"] == "ServiceInstance":
            return True
        name = info["base"]
    return False


lines = [
    f"// Generated by tools/gen_script_api.py for Roblox {version}.",
    "// Sources: the 0.742 player reflection baseline, the supported release's",
    "// official API dump and player strings (types, members, enums and security).",
    f"// API dump SHA-256: {hashlib.sha256(api_path.read_bytes()).hexdigest()}",
    f"// Player strings SHA-256: {hashlib.sha256(strings_path.read_bytes()).hexdigest()}",
    "// Do not edit by hand; see tools/gen_script_api.py for regeneration.",
    "",
    '#include "RobloxApi.hpp"',
    "#include <lunar/RobloxVersion.hpp>",
    "",
    "namespace lunar::lua::api {",
    "",
    f'static_assert(std::string_view(lunar::robloxVersion) == "{version}",',
    '              "Regenerate RobloxApiData.cpp when updating the supported Roblox release");',
    "",
    "// clang-format off",
    "namespace {",
    "",
    "constexpr EnumItemData enumItemData[] = {",
]

enum_rows = []
offset = 0
for name in sorted(enums):
    items = sorted(enums[name].items(), key=lambda kv: (kv[1], kv[0]))
    for item, value in items:
        lines.append(f'    {{"{item}", {value}u}},')
    enum_rows.append(f'    {{"{name}", {offset}, {len(items)}}},')
    offset += len(items)
lines += ["};", "", "constexpr EnumData enumData[] = {"] + enum_rows + ["};", ""]

# Old names the client still answers to for renamed items (Font.GothamSemibold
# is GothamMedium): the API dump's LegacyNames, for the enums and items above.
legacy_rows = []
for dumped in api_dump["Enums"]:
    for dumped_item in dumped["Items"]:
        if dumped["Name"] in enums and dumped_item["Name"] in enums[dumped["Name"]]:
            for old in dumped_item.get("LegacyNames", []):
                legacy_rows.append((dumped["Name"], old, dumped_item["Name"]))
lines.append("constexpr EnumLegacyName enumLegacyNames[] = {")
lines += [f'    {{"{e}", "{old}", "{current}"}},' for e, old, current in sorted(legacy_rows)]
lines += ["};", ""]

lines.append("constexpr MemberData memberData[] = {")
class_rows = []
offset = 0
for name in sorted(classes):
    info = classes[name]
    members = sorted(info["members"].items())
    count = 0
    for member, desc in members:
        kind = member_kinds.get(desc["kind"])
        if kind is None:
            continue
        read, write = security(name, member)
        hidden = kind == "Property" and not scriptable(name, member)
        restricted = f", {read}, {write}" if read or write or hidden else ""
        if hidden:
            restricted += ", false"
        lines.append(f'    {{"{member}", MemberKind::{kind}, "{luau_type(desc)}"{restricted}}},')
        count += 1
    flags = []
    if info.get("creatable"):
        flags.append("ClassCreatable")
    if is_service(name):
        flags.append("ClassService")
    base = info["base"] if info["base"] in classes else ""
    class_rows.append(f'    {{"{name}", "{base}", {offset}, {count}, {" | ".join(flags) or "0"}}},')
    offset += count
lines += ["};", "", "constexpr ClassData classData[] = {"] + class_rows + ["};", ""]

lines += [
    "} // namespace",
    "// clang-format on",
    "",
    "std::span<const EnumData> allEnums()",
    "{",
    "    return enumData;",
    "}",
    "",
    "std::span<const EnumItemData> itemsOf(const EnumData& data)",
    "{",
    "    return std::span(enumItemData).subspan(data.first, data.count);",
    "}",
    "",
    "std::span<const EnumLegacyName> legacyEnumNames()",
    "{",
    "    return enumLegacyNames;",
    "}",
    "",
    "std::span<const ClassData> allClasses()",
    "{",
    "    return classData;",
    "}",
    "",
    "std::span<const MemberData> membersOf(const ClassData& data)",
    "{",
    "    return std::span(memberData).subspan(data.first, data.count);",
    "}",
    "",
    "} // namespace lunar::lua::api",
    "",
]

out.write_text("\n".join(lines))
print(f"{out}: {len(enums)} enums, {len(classes)} classes")
