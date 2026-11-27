// The default player scripts. When a place doesn't bring its own, the client
// puts Roblox's PlayerModule (with its CameraModule and ControlModule),
// PlayerScriptsLoader and RbxCharacterSounds into StarterPlayerScripts, so
// every player gets a copy; scripts reach for them with
// PlayerScripts:WaitForChild("PlayerModule") and require(...):GetControls().
//
// From the binary: StarterPlayer.CreateDefaultPlayerModule, the
// InstallDefaultScripts step and its "oldPlayerModulePresent",
// "oldPlayerScriptsPresent" (CameraScript, ControlScript) and
// "oldRbxCharacterSoundsPresent" checks, and the scripts' home,
// rbxasset://scripts/PlayerScripts ("PlayerScripts.rbxp"), which the macOS
// 0.742 player doesn't ship: its default scripts come from the server.
//
// Lunarstone's own camera and controls (physics::PlayerCamera, the
// HumanoidController the app drives) do what the PlayerModule's Lua does, so
// these are stand-ins: instances with the real names whose require() gives
// the PlayerModule's public API, backed by the native systems, rather than a
// second set of controls fighting the first. ControlModule:Disable() stops
// the native controls (PlaySession::controlsEnabled).

#include "Runtime.hpp"
#include "Services.hpp"
#include "Userdata.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace lunar::lua {

void Shared::setNativeModule(Instance& module, NativeModule push)
{
    std::erase_if(nativeModules_, [](const auto& item) { return item.second.first.expired(); });
    nativeModules_.insert_or_assign(&module, std::pair{module.weak_from_this(), push});
}

Shared::NativeModule Shared::nativeModule(const Instance& module) const
{
    auto it = nativeModules_.find(&module);
    if (it == nativeModules_.end() || it->second.first.lock().get() != &module)
        return nullptr;
    return it->second.second;
}

namespace {

// The keyboard's move vector, the ControlModule's Keyboard controller's:
// W/Up forward (-Z), S/Down back, A left, D right. Zero while disabled.
Vector3 moveVector(lua_State* L)
{
    Shared& shared = Runtime::of(L).shared();
    if (!shared.controlsEnabled)
        return {};
    const InputState& input = shared.state->input;
    auto down = [&](Enum::KeyCode key) { return input.keyDown(key) ? 1.0f : 0.0f; };
    const float forward = std::max(down(Enum::KeyCode::W), down(Enum::KeyCode::Up));
    const float backward = std::max(down(Enum::KeyCode::S), down(Enum::KeyCode::Down));
    return {down(Enum::KeyCode::D) - down(Enum::KeyCode::A), 0.0f, backward - forward};
}

// ControlModule:Enable(enable) (true when left out) and :Disable(); the
// active controller's Enable(enable) does the same for the keyboard.
int controlsEnable(lua_State* L)
{
    Runtime::of(L).shared().controlsEnabled = lua_isnoneornil(L, 2) || lua_toboolean(L, 2) != 0;
    return 0;
}

int controlsDisable(lua_State* L)
{
    Runtime::of(L).shared().controlsEnabled = false;
    return 0;
}

int controlsGetMoveVector(lua_State* L)
{
    pushVector3(L, moveVector(L));
    return 1;
}

int controllerIsJumping(lua_State* L)
{
    Shared& shared = Runtime::of(L).shared();
    lua_pushboolean(L, shared.controlsEnabled && shared.state->input.keyDown(Enum::KeyCode::Space));
    return 1;
}

int pushTrue(lua_State* L)
{
    lua_pushboolean(L, 1);
    return 1;
}

// GUESS: no click-to-move here; the real one makes its controller on demand.
int pushNil(lua_State* L)
{
    lua_pushnil(L);
    return 1;
}

// The ControlModule's active controller, upvalue 1 of GetActiveController.
int controlsActiveController(lua_State* L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

void setMethod(lua_State* L, const char* name, lua_CFunction function)
{
    lua_pushcfunction(L, function, name);
    lua_setfield(L, -2, name);
}

void pushControlModule(lua_State* L, Instance&)
{
    lua_newtable(L);
    setMethod(L, "Enable", controlsEnable);
    setMethod(L, "Disable", controlsDisable);
    setMethod(L, "GetMoveVector", controlsGetMoveVector);
    setMethod(L, "GetClickToMoveController", pushNil);
    // The keyboard controller, the one active on a computer.
    lua_newtable(L);
    setMethod(L, "Enable", controlsEnable);
    setMethod(L, "GetMoveVector", controlsGetMoveVector);
    setMethod(L, "GetIsJumping", controllerIsJumping);
    setMethod(L, "IsMoveVectorCameraRelative", pushTrue);
    lua_pushcclosure(L, controlsActiveController, "GetActiveController", 1);
    lua_setfield(L, -2, "GetActiveController");
}

int mouseLockEvent(lua_State* L)
{
    pushHubSignal(L, *Runtime::of(L).game().getService<UserInputService>(), "LunarstoneMouseLockToggled");
    return 1;
}

int mouseLockToggle(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "isMouseLocked");
    const bool locked = !lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_pushboolean(L, locked);
    lua_setfield(L, 1, "isMouseLocked");
    Runtime& rt = Runtime::of(L);
    auto* input = rt.game().getService<UserInputService>();
    input->setMouseBehavior(locked ? Enum::MouseBehavior::LockCenter : Enum::MouseBehavior::Default);
    if (const auto& settings = rt.shared().userSettings)
        if (auto* game = instanceCast<UserGameSettings>(settings->findFirstChildOfClass("UserGameSettings")))
            game->setRotationType(locked ? Enum::RotationType::CameraRelative : Enum::RotationType::MovementRelative);
    input->fireEvent("LunarstoneMouseLockToggled");
    return 0;
}

int mouseLockState(lua_State* L)
{
    lua_getfield(L, 1, "isMouseLocked");
    return 1;
}

// What the CameraModule's own Lua does each frame that a script can see: it
// reads the camera through the instance API at RenderPriority.Camera, from a
// thread that is the CameraModule's, so a hooked __index sees the access and
// getcallingscript names the module [sUNC's getcallingscript test waits in
// its __index hook for tostring(getcallingscript()) == "CameraModule"]. The
// binding's name is the real module's; the native camera does the moving.
constexpr std::string_view cameraUpdate = R"lua(
game:GetService("RunService"):BindToRenderStep("cameraRenderUpdate", Enum.RenderPriority.Camera.Value, function()
    local camera = workspace.CurrentCamera
    if camera then
        local _ = camera.CFrame
    end
end)
)lua";

void startCameraUpdate(lua_State* L, Instance& module)
{
    Runtime& rt = Runtime::of(L);
    const std::string chunkName = module.fullName();
    auto bytecode = rt.codeOf(cameraUpdate, chunkName);
    if (!bytecode)
        return;
    auto thread = rt.loadThread(*bytecode, chunkName, &module);
    if (!thread)
        return;
    const int ref = lua_ref(rt.main(), -1);
    lua_pop(rt.main(), 1);
    if (auto error = rt.resume(*thread, 0, "require"))
        rt.report(*error);
    lua_unref(rt.main(), ref);
}

// The renderer owns the camera. Expose the controller identity and mouse-lock
// signal used by games' custom controls alongside the native ControlModule.
void pushCameraModule(lua_State* L, Instance& module)
{
    startCameraUpdate(L, module);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "isMouseLocked");
    setMethod(L, "GetBindableToggleEvent", mouseLockEvent);
    setMethod(L, "GetIsMouseLocked", mouseLockState);
    setMethod(L, "OnMouseLockToggled", mouseLockToggle);
    lua_setfield(L, -2, "activeMouseLockController");
}

// PlayerModule:GetControls() / :GetCameras(): require of the child module
// (upvalue 1), so they're the same objects scripts get by requiring it.
int playerModuleRequire(lua_State* L)
{
    Instance* child = toInstance(L, lua_upvalueindex(1));
    if (!child) {
        lua_pushnil(L);
        return 1;
    }
    return Runtime::of(L).require(L, *child);
}

int playerModuleClickToMove(lua_State* L)
{
    lua_pushnil(L);
    return 1;
}

void pushPlayerModule(lua_State* L, Instance& module)
{
    lua_newtable(L);
    pushInstance(L, module.findFirstChild("ControlModule"));
    lua_pushcclosure(L, playerModuleRequire, "GetControls", 1);
    lua_setfield(L, -2, "GetControls");
    pushInstance(L, module.findFirstChild("CameraModule"));
    lua_pushcclosure(L, playerModuleRequire, "GetCameras", 1);
    lua_setfield(L, -2, "GetCameras");
    for (const auto& [childName, field] :
         {std::pair{"ControlModule", "controls"}, std::pair{"CameraModule", "cameras"}})
        if (Instance* child = module.findFirstChild(childName)) {
            Runtime::of(L).require(L, *child);
            lua_setfield(L, -2, field);
        }
    setMethod(L, "GetClickToMoveController", playerModuleClickToMove);
}

template <class T>
std::shared_ptr<T> makeChild(Instance& parent, std::string_view name)
{
    auto child = std::make_shared<T>();
    child->setName(std::string(name));
    child->setParent(&parent);
    return child;
}

// A stand-in: no Source of its own.
bool isStandIn(const Instance* instance)
{
    const auto* module = dynamic_cast<const ModuleScript*>(instance);
    return module && module->source().empty();
}

} // namespace

bool installDefaultPlayerScripts(Shared& shared, DataModel& game)
{
    auto* starterPlayer = dynamic_cast<StarterPlayer*>(game.getService("StarterPlayer"));
    if (!starterPlayer)
        return false;
    Instance* scripts = starterPlayer->findFirstChildOfClass("StarterPlayerScripts");
    if (!scripts)
        scripts = makeChild<StarterPlayerScripts>(*starterPlayer, "StarterPlayerScripts").get();
    bool added = false;
    // A place with a PlayerModule of its own (or the old CameraScript and
    // ControlScript) keeps it.
    const bool own = scripts->findFirstChild("PlayerModule") || scripts->findFirstChild("CameraScript")
                     || scripts->findFirstChild("ControlScript");
    if (starterPlayer->createDefaultPlayerModule() && !own) {
        auto module = makeChild<ModuleScript>(*scripts, "PlayerModule");
        shared.setNativeModule(*module, pushPlayerModule);
        shared.setNativeModule(*makeChild<ModuleScript>(*module, "CameraModule"), pushCameraModule);
        shared.setNativeModule(*makeChild<ModuleScript>(*module, "ControlModule"), pushControlModule);
        // GUESS: the loader only requires the PlayerModule, which the stand-in doesn't need.
        makeChild<LocalScript>(*scripts, "PlayerScriptsLoader");
        added = true;
    }
    // GUESS: footstep and jump sounds aren't played natively yet; the
    // stand-in is there for scripts that look for it.
    if (!scripts->findFirstChild("RbxCharacterSounds"))
        makeChild<LocalScript>(*scripts, "RbxCharacterSounds");
    return added;
}

void registerPlayerModule(Shared& shared, Instance& playerScripts)
{
    Instance* module = playerScripts.findFirstChild("PlayerModule");
    // Roblox's shipped module is non-archivable on live servers. Clone skips
    // it, but game scripts still require PlayerScripts.PlayerModule. Supply
    // the same native facade as the locally generated default, without
    // starting a second camera/controller beside the native ones.
    if (!module) {
        Instance* root = &playerScripts;
        while (root->parent())
            root = root->parent();
        auto* game = instanceCast<DataModel>(root);
        auto* starter = game ? instanceCast<StarterPlayer>(game->findService("StarterPlayer")) : nullptr;
        Instance* defaults = starter ? starter->findFirstChildOfClass("StarterPlayerScripts") : nullptr;
        Instance* shipped = defaults ? defaults->findFirstChild("PlayerModule") : nullptr;
        if (starter && starter->createDefaultPlayerModule() && shipped && !shipped->archivable()
            && shipped->findFirstChild("CameraModule") && shipped->findFirstChild("ControlModule")) {
            if (InstancePtr facade = shipped->cloneForClient()) {
                // Keep submodules and configuration values such as
                // CameraModule.MouseLockController.BoundKeys. Only the three
                // native entry points are facades; nested modules retain code.
                for (std::string_view name : {"", "CameraModule", "ControlModule"}) {
                    auto* entry
                        = instanceCast<ModuleScript>(name.empty() ? facade.get() : facade->findFirstChild(name));
                    if (entry)
                        entry->setSource("");
                }
                facade->setParent(&playerScripts);
                module = facade.get();
            }
        }
    }
    if (!isStandIn(module))
        return;
    shared.setNativeModule(*module, pushPlayerModule);
    if (Instance* camera = module->findFirstChild("CameraModule"); isStandIn(camera))
        shared.setNativeModule(*camera, pushCameraModule);
    if (Instance* controls = module->findFirstChild("ControlModule"); isStandIn(controls))
        shared.setNativeModule(*controls, pushControlModule);
}

} // namespace lunar::lua
