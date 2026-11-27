#pragma once

// Script containers. The engine only stores them; running them is the
// scripting layer's job. Included through <lunar/Classes.hpp>.

#include <lunar/Enums.hpp>
#include <lunar/Instance.hpp>

#include <string>

namespace lunar {

class LuaSourceContainer : public InstanceImpl<LuaSourceContainer, Instance, ClassKind::Abstract> {
public:
    LUNAR_FIELD(std::string, source, Source)
    // Deprecated: an asset id the source was linked from.
    LUNAR_FIELD(std::string, linkedSource, LinkedSource)
};

class BaseScript : public InstanceImpl<BaseScript, LuaSourceContainer, ClassKind::Abstract> {
public:
    LUNAR_FIELD(bool, enabled, Enabled, true)
    LUNAR_FIELD(Enum::RunContext, runContext, RunContext, Enum::RunContext::Legacy)

    // The old, inverted name for Enabled; files still store this one.
    LUNAR_PROPERTY() bool disabled() const { return !enabled(); }
    void setDisabled(bool disabled)
    {
        if (disabled == this->disabled())
            return;
        setEnabled(!disabled);
        changed.fire("Disabled");
    }
};

class Script : public InstanceImpl<Script, BaseScript> {};
class LocalScript final : public InstanceImpl<LocalScript, Script> {};
class ModuleScript final : public InstanceImpl<ModuleScript, LuaSourceContainer> {};

} // namespace lunar
