// LUNAR_PROFILE: where simulated frames spend their time, by frame phase and
// by script, the way Studio's Script Performance window splits it.

#include "Runtime.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace lunar::lua {

namespace {

double clockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

const Instance* resumedScript(lua_State* thread, int nargs)
{
    // A thread that yielded continues its outermost function; a fresh one
    // starts the function under its arguments.
    if (lua_status(thread) == LUA_YIELD) {
        const int depth = lua_stackdepth(thread);
        lua_Debug ar{};
        if (depth <= 0 || !lua_getinfo(thread, depth - 1, "f", &ar))
            return nullptr;
    } else {
        const int function = lua_gettop(thread) - nargs;
        if (function < 1)
            return nullptr;
        lua_pushvalue(thread, function);
    }
    if (!lua_isfunction(thread, -1)) {
        lua_pop(thread, 1);
        return nullptr;
    }
    lua_getfenv(thread, -1);
    const Instance* script = nullptr;
    if (lua_istable(thread, -1)) {
        lua_rawgetfield(thread, -1, "script");
        script = toInstance(thread, -1);
        lua_pop(thread, 1);
    }
    lua_pop(thread, 2);
    return script;
}

Profiler::Scope::Scope(Profiler* profiler, const Instance* script, ScriptRole role, std::string_view what)
    : profiler_(profiler), script_(script), role_(role), what_(what), start_(clockSeconds())
{
    profiler_->nested_.push_back(0.0);
}

void Profiler::Scope::finish()
{
    const double spent = clockSeconds() - start_;
    const double exclusive = std::max(spent - profiler_->nested_.back(), 0.0);
    profiler_->nested_.pop_back();
    if (!profiler_->nested_.empty())
        profiler_->nested_.back() += spent;
    profiler_->add(script_, role_, what_, exclusive);
}

void Profiler::add(const Instance* script, ScriptRole role, std::string_view what, double seconds)
{
    std::string key = std::format("{}|{}|{}", static_cast<int>(role), static_cast<const void*>(script), what);
    auto [it, added] = entries_.try_emplace(std::move(key));
    Entry& entry = it->second;
    if (added || entry.script.lock().get() != script) {
        entry = Entry{};
        if (script) {
            entry.script = std::const_pointer_cast<Instance>(script->shared_from_this());
            entry.name = script->fullName();
        } else {
            entry.name = "?";
        }
        entry.what = what;
        entry.role = role;
    }
    entry.seconds += seconds;
    ++entry.resumes;
    profile_.luau[static_cast<std::size_t>(role)] += seconds;
    luauTotal_ += seconds;
}

void Profiler::beginFrame()
{
    ++profile_.frames;
    phaseMark_ = clockSeconds();
    luauMark_ = luauTotal_;
}

void Profiler::endPhase(SimulationProfile::Phase phase)
{
    const double now = clockSeconds();
    profile_.phases[phase] += now - phaseMark_;
    profile_.phaseLuau[phase] += luauTotal_ - luauMark_;
    phaseMark_ = now;
    luauMark_ = luauTotal_;
}

SimulationProfile Profiler::take()
{
    SimulationProfile profile = std::move(profile_);
    profile_ = {};
    for (auto& [key, entry] : entries_)
        if (entry.resumes > 0)
            profile.activity.push_back(
                ScriptActivity{entry.name, entry.what, entry.role, entry.seconds, entry.resumes});
    std::erase_if(entries_, [](const auto& item) { return item.second.script.expired(); });
    for (auto& [key, entry] : entries_) {
        entry.seconds = 0.0;
        entry.resumes = 0;
    }
    std::ranges::sort(profile.activity, std::ranges::greater{}, &ScriptActivity::seconds);
    return profile;
}

} // namespace lunar::lua
