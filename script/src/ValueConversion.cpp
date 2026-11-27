// Engine Value <-> Luau. The one place to extend when Value gains alternatives:
// add an overload to pushValue's visitor and a case to toValue.

#include "Userdata.hpp"

#include <cstring>
#include <string>
#include <variant>

namespace lunar::lua {

namespace {

template <class... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

template <class T>
std::optional<Value> userdataValue(lua_State* L, int idx)
{
    return Value(check<T>(L, idx));
}

} // namespace

void pushValue(lua_State* L, const Value& value)
{
    std::visit(Overloaded{
                   [&](std::monostate) { lua_pushnil(L); },
                   [&](bool b) { lua_pushboolean(L, b); },
                   [&](double d) { lua_pushnumber(L, d); },
                   [&](const std::string& s) { lua_pushlstring(L, s.data(), s.size()); },
                   [&](const Vector3& v) { pushVector3(L, v); },
                   [&](const Color3& c) { pushColor3(L, c); },
                   [&](const CFrame& cf) { pushCFrame(L, cf); },
                   [&](const InstancePtr& instance) { pushInstance(L, instance.get()); },
                   [&](const EnumItem& item) { pushEnumItem(L, item); },
                   [&](const Buffer& b) {
                       void* bytes = lua_newbuffer(L, b.bytes.size());
                       if (!b.bytes.empty())
                           std::memcpy(bytes, b.bytes.data(), b.bytes.size());
                   },
                   [&]<Userdata T>(const T& v) { push<T>(L, v); },
               },
               value);
}

std::optional<Value> toValue(lua_State* L, int idx)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
    case LUA_TNONE:
        return Value();
    case LUA_TBOOLEAN:
        return Value(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER:
        return Value(lua_tonumber(L, idx));
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, idx, &length);
        return Value(std::string(text, length));
    }
    case LUA_TBUFFER: {
        std::size_t length = 0;
        const auto* bytes = static_cast<const char*>(lua_tobuffer(L, idx, &length));
        return Value(Buffer{std::string(bytes, length)});
    }
    case LUA_TVECTOR:
        if (auto v = toVector3(L, idx))
            return Value(*v);
        return std::nullopt;
    case LUA_TUSERDATA:
        switch (lua_userdatatag(L, idx)) {
        case InstanceTag:
            return Value(toInstance(L, idx)->shared_from_this());
        case Color3Tag:
            return Value(*toColor3(L, idx));
        case CFrameTag:
            return Value(*toCFrame(L, idx));
        case Vector2Tag:
            return userdataValue<Vector2>(L, idx);
        case UDimTag:
            return userdataValue<UDim>(L, idx);
        case UDim2Tag:
            return userdataValue<UDim2>(L, idx);
        case RectTag:
            return userdataValue<Rect>(L, idx);
        case NumberRangeTag:
            return userdataValue<NumberRange>(L, idx);
        case NumberSequenceTag:
            return userdataValue<NumberSequence>(L, idx);
        case ColorSequenceTag:
            return userdataValue<ColorSequence>(L, idx);
        case BrickColorTag:
            return userdataValue<BrickColor>(L, idx);
        case EnumItemTag:
            return userdataValue<EnumItem>(L, idx);
        case FacesTag:
            return userdataValue<Faces>(L, idx);
        case AxesTag:
            return userdataValue<Axes>(L, idx);
        case PhysicalPropertiesTag:
            return userdataValue<PhysicalProperties>(L, idx);
        case Vector3int16Tag:
            return userdataValue<Vector3int16>(L, idx);
        case FontTag:
            return userdataValue<Font>(L, idx);
        default:
            return std::nullopt;
        }
    default:
        return std::nullopt;
    }
}

} // namespace lunar::lua
