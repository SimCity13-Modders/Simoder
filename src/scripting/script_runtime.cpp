#include "scripting/script_runtime.hpp"
#include <lua.h>
#include <lualib.h>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <filesystem>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>

namespace sc13::scripting {
namespace {
/** @summary Bounds combined declared VM budgets in the game's finite x86 address space. */
std::atomic<std::size_t> reservedScriptMemory{};
constexpr std::size_t maximumScriptMemory = 128U * 1024U * 1024U;
/** @summary Accepts the authoring project's full instance spelling only when the verified runtime width is sufficient. */
std::optional<core::Tgi> ParseScriptTgi(std::string_view text) {
    if (const auto compact = core::ParseTgi(text)) return compact;
    const auto separator = text.rfind(':');
    if (separator == text.npos) return {};
    auto instance = text.substr(separator + 1);
    if (instance.starts_with("0x") || instance.starts_with("0X")) instance.remove_prefix(2);
    if (instance.empty() || instance.size() > 16) return {};
    std::uint64_t value{};
    const auto parsed = std::from_chars(instance.data(), instance.data() + instance.size(), value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != instance.data() + instance.size() || value > UINT32_MAX) return {};
    auto prefix = core::ParseTgi(std::string(text.substr(0, separator + 1)) + "0");
    if (prefix) prefix->instance = static_cast<std::uint32_t>(value);
    return prefix;
}
}
void* ScriptRuntime::Allocate(void* context, void* pointer, std::size_t oldSize, std::size_t newSize) {
    auto& self = *static_cast<ScriptRuntime*>(context);
    if (!pointer) oldSize = 0;
    if (!newSize) {
        self.used_ -= oldSize;
        std::free(pointer);
        return nullptr;
    }
    if (newSize > self.maximum_ - (self.used_ - oldSize)) return nullptr;
    void* result = std::realloc(pointer, newSize);
    if (result) self.used_ = self.used_ - oldSize + newSize;
    return result;
}

void ScriptRuntime::Interrupt(lua_State* state, int gc) {
    if (gc >= 0) return;
    const auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    if (std::chrono::steady_clock::now() >= self.deadline_)
        luaL_error(state, "Simoder callback execution budget exceeded");
}

int ScriptRuntime::Log(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    std::size_t length{};
    const char* message = luaL_checklstring(state, 1, &length);
    if (length > 2048) luaL_error(state, "Simoder log message exceeds 2048 bytes");
    if (self.logs_.size() < 128) self.logs_.emplace_back(message, length);
    return 0;
}

int ScriptRuntime::HasCapability(lua_State* state) {
    const auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    const std::string_view name(luaL_checkstring(state, 1));
    lua_pushboolean(state, name == "script.luau" || name == "script.lifecycle" || name == "script.log" ||
        (name == "game.props.float" && bool(self.host_.readFloat)));
    return 1;
}

ScriptPatchCommand ScriptRuntime::PatchTarget(lua_State* state) {
    const auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    std::size_t length{};
    const char* raw = luaL_checklstring(state, 1, &length);
    if (length > 128) luaL_error(state, "PROP resource reference exceeds 128 bytes");
    const std::string_view resource(raw, length);
    const auto alias = self.resourceAliases_.find(resource);
    const auto target = alias == self.resourceAliases_.end() ? ParseScriptTgi(resource) : std::optional(alias->second);
    if (!target || target->type != 0x00B1B104U)
        luaL_error(state, "Expected PROP TYPE:GROUP:INSTANCE; nonzero high instance bits are unsupported");
    ScriptPatchCommand command; command.target = *target;
    std::uint32_t property{};
    if (lua_type(state, 2) == LUA_TNUMBER) {
        const double id = lua_tonumber(state, 2);
        if (!std::isfinite(id) || id < 0 || id > std::numeric_limits<std::uint32_t>::max() || std::floor(id) != id)
            luaL_error(state, "Property ID must be an exact uint32");
        property = static_cast<std::uint32_t>(id);
    } else {
        raw = luaL_checklstring(state, 2, &length);
        std::string_view id(raw, length);
        if (const auto named = self.propertyAliases_.find(id); named != self.propertyAliases_.end()) {
            command.property = named->second.propertyId;
            command.propertyName = named->second.name;
            command.source = named->second.source;
            return command;
        }
        if (id.starts_with("0x")) id.remove_prefix(2);
        if (id.empty() || id.size() > 8) luaL_error(state, "Expected hexadecimal property ID");
        const auto parsed = std::from_chars(id.data(), id.data() + id.size(), property, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size()) luaL_error(state, "Invalid hexadecimal property ID");
    }
    command.property = property;
    return command;
}

int ScriptRuntime::GetFloat(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    if (!self.host_.readFloat) luaL_error(state, "Game PROP API is unavailable in this host");
    const auto target = PatchTarget(state);
    std::optional<FloatObservation> observed;
    try { observed = self.host_.readFloat(target.target, target.property); }
    catch (const std::exception& error) { luaL_error(state, "PROP observation rejected: %s", error.what()); }
    if (!observed) { lua_pushnil(state); return 1; }
    lua_newtable(state);
    lua_pushnumber(state, observed->value); lua_setfield(state, -2, "value");
    lua_pushnumber(state, observed->vanilla); lua_setfield(state, -2, "vanilla");
    lua_pushnumber(state, static_cast<double>(observed->observedAtUnixMs)); lua_setfield(state, -2, "observedAtUnixMs");
    lua_pushnumber(state, static_cast<double>(observed->generation)); lua_setfield(state, -2, "generation");
    lua_pushnumber(state, static_cast<double>(observed->instances)); lua_setfield(state, -2, "instances");
    lua_pushlstring(state, observed->status.data(), observed->status.size()); lua_setfield(state, -2, "status");
    lua_pushboolean(state, true); lua_setfield(state, -2, "historical");
    lua_setreadonly(state, -1, true);
    return 1;
}

int ScriptRuntime::SetFloat(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    if (!self.host_.readFloat || self.stopping_) luaL_error(state, "PROP writes require an attached host outside onDisable");
    auto command = PatchTarget(state);
    const double value = luaL_checknumber(state, 3);
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) luaL_error(state, "Expected finite float32 value");
    command.value = static_cast<float>(value);
    if (self.commands_.size() >= 256) luaL_error(state, "At most 256 PROP commands per callback");
    self.commands_.push_back(command); return 0;
}

int ScriptRuntime::ClearFloat(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    if (!self.host_.readFloat || self.stopping_) luaL_error(state, "PROP writes require an attached host outside onDisable");
    const auto command = PatchTarget(state);
    if (self.commands_.size() >= 256) luaL_error(state, "At most 256 PROP commands per callback");
    self.commands_.push_back(command); return 0;
}

int ScriptRuntime::Time(lua_State* state) {
    const auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    lua_pushnumber(state, std::chrono::duration<double>(std::chrono::steady_clock::now() - self.start_).count());
    return 1;
}

int ScriptRuntime::Require(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    const std::string_view requested(luaL_checkstring(state, 1));
    if ((!requested.starts_with("./") && !requested.starts_with("../")) ||
        requested.find(':') != requested.npos || requested.find('\\') != requested.npos)
        luaL_error(state, "require expects a relative local Luau module");
    const std::string_view caller(lua_tostring(state, lua_upvalueindex(1)));
    const auto currentPath = std::filesystem::path(std::u8string(caller.begin(), caller.end()));
    auto path = (currentPath.parent_path() / std::filesystem::path(std::u8string(requested.begin(), requested.end()))).lexically_normal();
    if (path.extension().empty()) path += ".luau";
    const auto utf8 = path.generic_u8string();
    const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    if (!name.starts_with("scripts/") || !self.modules_.contains(name))
        luaL_error(state, "Local Luau module not found in compiled generation: %s (from %s)", name.c_str(), caller.data());
    if (const auto cached = self.moduleRefs_.find(name); cached != self.moduleRefs_.end()) {
        lua_getref(state, cached->second); return 1;
    }
    if (!self.loadingModules_.insert(name).second) luaL_error(state, "Cyclic Luau module dependency");
    const auto& bytecode = self.modules_.at(name);
    /** @summary Bind require to this module's environment so deferred callbacks retain their source directory. */
    lua_newtable(state);
    const int environment = lua_gettop(state);
    lua_pushlstring(state, name.data(), name.size());
    lua_pushcclosure(state, &Require, "require", 1);
    lua_setfield(state, environment, "require");
    lua_newtable(state);
    lua_pushvalue(state, LUA_GLOBALSINDEX);
    lua_setfield(state, -2, "__index");
    lua_setreadonly(state, -1, true);
    lua_setmetatable(state, environment);
    /** @summary Module globals differ from thread globals; bypass eagerly cached imports from the pinned VM. */
    lua_setsafeenv(state, environment, false);
    const auto chunk = "@" + name;
    int status = luau_load(state, chunk.c_str(), bytecode.data(), bytecode.size(), environment);
    lua_remove(state, environment);
    if (!status) status = lua_pcall(state, 0, 1, 0);
    self.loadingModules_.erase(name);
    if (status) lua_error(state);
    if (lua_isnil(state, -1)) luaL_error(state, "Luau module must return a value");
    self.moduleRefs_[name] = lua_ref(state, -1);
    return 1;
}

void ScriptRuntime::Call(int arguments, int results) {
    const auto commandCount = commands_.size();
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs_);
    int status{};
    try { status = lua_pcall(state_, arguments, results, 0); }
    catch (...) { commands_.resize(commandCount); lua_settop(state_, 0); throw; }
    if (status != LUA_OK) {
        commands_.resize(commandCount);
        const char* text = lua_tostring(state_, -1);
        const std::string error = text ? text : "Luau callback failed without a string diagnostic";
        lua_settop(state_, 0);
        throw std::runtime_error(error);
    }
}

int ScriptRuntime::Callback(const char* name) {
    lua_getfield(state_, -1, name);
    if (lua_isnil(state_, -1)) { lua_pop(state_, 1); return -1; }
    if (!lua_isfunction(state_, -1)) throw std::runtime_error(std::string(name) + " must be a function");
    const int reference = lua_ref(state_, -1);
    lua_pop(state_, 1);
    return reference;
}

ScriptRuntime::ScriptRuntime(std::string modId, const ScriptDefinition& definition, ScriptHost host)
    : modId_(std::move(modId)), maximum_(definition.memoryBytes), budgetMs_(definition.budgetMs),
      modules_(definition.modules), resourceAliases_(definition.resourceAliases),
      propertyAliases_(definition.propertyAliases), host_(std::move(host)) {
    if (maximum_ < 2U * 1024U * 1024U || maximum_ > 64U * 1024U * 1024U ||
        budgetMs_ < 1 || budgetMs_ > 20 || definition.bytecode.empty() ||
        definition.bytecode.size() > 8U * 1024U * 1024U)
        throw std::runtime_error("Invalid script generation or limits");
    const auto previous = reservedScriptMemory.fetch_add(maximum_);
    if (previous > maximumScriptMemory - maximum_) {
        reservedScriptMemory.fetch_sub(maximum_);
        throw std::runtime_error("Combined script memory budgets exceed the 128 MiB host limit");
    }
    reservedMemory_ = true;
    state_ = lua_newstate(&Allocate, this);
    if (!state_) { reservedScriptMemory.fetch_sub(maximum_); reservedMemory_ = false; throw std::runtime_error("Could not allocate Luau VM"); }
    try {
        lua_callbacks(state_)->userdata = this;
        lua_callbacks(state_)->interrupt = &Interrupt;
        luaL_openlibs(state_);
        for (const auto* name : {"getfenv", "setfenv", "debug", "require", "loadstring"}) {
            lua_pushnil(state_); lua_setglobal(state_, name);
        }
        lua_newtable(state_);
        lua_pushinteger(state_, 1); lua_setfield(state_, -2, "apiVersion");
        lua_pushlstring(state_, modId_.data(), modId_.size()); lua_setfield(state_, -2, "modId");
        lua_pushcfunction(state_, &Log, "simoder.log"); lua_setfield(state_, -2, "log");
        lua_pushcfunction(state_, &Time, "simoder.time"); lua_setfield(state_, -2, "time");
        lua_pushcfunction(state_, &HasCapability, "simoder.hasCapability"); lua_setfield(state_, -2, "hasCapability");
        lua_newtable(state_);
        lua_pushcfunction(state_, &GetFloat, "simoder.props.getFloat"); lua_setfield(state_, -2, "getFloat");
        lua_pushcfunction(state_, &SetFloat, "simoder.props.setFloat"); lua_setfield(state_, -2, "setFloat");
        lua_pushcfunction(state_, &ClearFloat, "simoder.props.clearFloat"); lua_setfield(state_, -2, "clearFloat");
        lua_setreadonly(state_, -1, true); lua_setfield(state_, -2, "props");
        lua_setreadonly(state_, -1, true); lua_setglobal(state_, "simoder");
        lua_pushcfunction(state_, &Log, "print"); lua_setglobal(state_, "print");
        lua_pushlstring(state_, definition.entry.data(), definition.entry.size());
        lua_pushcclosure(state_, &Require, "require", 1); lua_setglobal(state_, "require");
        luaL_sandbox(state_);
        luaL_sandboxthread(state_);
        const auto chunk = "@" + definition.entry;
        if (luau_load(state_, chunk.c_str(), definition.bytecode.data(), definition.bytecode.size(), 0)) {
            const char* error = lua_tostring(state_, -1);
            throw std::runtime_error(error ? error : "Invalid Luau bytecode");
        }
        Call(0, 1);
        if (!lua_istable(state_, -1)) throw std::runtime_error("Script must return a lifecycle table");
        tickRef_ = Callback("onTick");
        disableRef_ = Callback("onDisable");
        const int enable = Callback("onEnable");
        lua_pop(state_, 1);
        if (enable != -1) { lua_getref(state_, enable); Call(0, 0); lua_unref(state_, enable); }
        running_ = true;
    } catch (...) { lua_close(state_); state_ = nullptr; reservedScriptMemory.fetch_sub(maximum_); reservedMemory_ = false; throw; }
}

ScriptRuntime::~ScriptRuntime() { if (state_) lua_close(state_); if (reservedMemory_) reservedScriptMemory.fetch_sub(maximum_); }
void ScriptRuntime::Commit() { const std::scoped_lock lock(mutex_); committed_ = true; }

void ScriptRuntime::Tick(double deltaSeconds) {
    const std::scoped_lock lock(mutex_);
    if (!running_ || !committed_ || tickRef_ == -1) return;
    const auto now = std::chrono::steady_clock::now();
    if (deltaSeconds < 0) deltaSeconds = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_ = now;
    try {
        lua_getref(state_, tickRef_); lua_pushnumber(state_, deltaSeconds); Call(1, 0);
    } catch (const std::exception& error) { running_ = false; diagnostic_ = error.what(); }
}

void ScriptRuntime::Stop() {
    const std::scoped_lock lock(mutex_);
    if (!committed_) return;
    committed_ = false;
    running_ = false;
    stopping_ = true;
    commands_.clear();
    if (disableRef_ != -1) {
        try { lua_getref(state_, disableRef_); Call(0, 0); }
        catch (const std::exception& error) { diagnostic_ = error.what(); }
    }
}

ScriptStatus ScriptRuntime::Status() const {
    const std::scoped_lock lock(mutex_);
    return {running_ && committed_, used_, diagnostic_};
}

std::vector<std::string> ScriptRuntime::DrainLogs() {
    const std::scoped_lock lock(mutex_);
    if (!committed_ && running_) return {};
    auto logs = std::move(logs_); logs_.clear(); return logs;
}

std::vector<ScriptPatchCommand> ScriptRuntime::TakePatchCommands() {
    const std::scoped_lock lock(mutex_);
    auto result = std::move(commands_); commands_.clear(); return result;
}
void ScriptRuntime::Fail(std::string diagnostic) {
    const std::scoped_lock lock(mutex_); running_ = false; commands_.clear(); diagnostic_ = std::move(diagnostic);
}

ScriptScheduler::ScriptScheduler(LogSink sink, CommandSink commands)
    : sink_(std::move(sink)), commands_(std::move(commands)), worker_([this](std::stop_token stop) {
    std::size_t cursor{};
    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        const auto cycleDeadline = now + std::chrono::milliseconds(5);
        std::vector<std::pair<std::string, std::shared_ptr<ScriptRuntime>>> current;
        try { const std::scoped_lock lock(mutex_); current.assign(runtimes_.begin(), runtimes_.end()); }
        catch (...) { /* @summary Under host allocation pressure, skip this cycle without terminating the process. */ }
        for (std::size_t visited = 0; visited < current.size(); ++visited) {
            const auto& [owner, runtime] = current[cursor++ % current.size()];
            if (!runtime) continue;
            if (stop.stop_requested()) break;
            try {
                runtime->Tick(-1);
                const auto commands = runtime->TakePatchCommands();
                if (!commands.empty()) {
                    std::string error;
                    if (!commands_ || !commands_(owner, runtime, commands, error))
                        runtime->Fail("PROP transaction rejected: " + error);
                }
                for (const auto& message : runtime->DrainLogs()) if (sink_) sink_(owner, message);
            } catch (...) { /* @summary A logging sink failure cannot escape the owned scheduler thread. */ }
            if (std::chrono::steady_clock::now() >= cycleDeadline) break;
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(50), [&stop] { return stop.stop_requested(); });
    }
}) {}
ScriptScheduler::~ScriptScheduler() {
    Shutdown();
}
void ScriptScheduler::Shutdown() {
    worker_.request_stop(); wake_.notify_all(); if (worker_.joinable()) worker_.join();
}
void ScriptScheduler::Flush(const std::string& owner, const std::shared_ptr<ScriptRuntime>& runtime) {
    if (!runtime) return;
    try { for (const auto& message : runtime->DrainLogs()) if (sink_) sink_(owner, message); }
    catch (...) { /* @summary Shutdown and disable remain effective if a logging sink throws. */ }
}
void ScriptScheduler::Put(const std::string& owner, std::shared_ptr<ScriptRuntime> runtime) {
    const std::scoped_lock lock(mutex_); runtimes_.at(owner) = std::move(runtime);
}
void ScriptScheduler::Reserve(const std::string& owner) {
    const std::scoped_lock lock(mutex_); runtimes_.try_emplace(owner);
}
void ScriptScheduler::Remove(const std::string& owner) {
    const std::scoped_lock lock(mutex_);
    if (const auto found = runtimes_.find(owner); found != runtimes_.end()) found->second.reset();
}
}
