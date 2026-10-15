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
#include <array>
#include <exception>
#include <type_traits>
#include <algorithm>
#include <cstring>

namespace sc13::scripting {
/** @summary Shares one private interpreter lane across managed and standalone runtimes. */
class ScriptLane final {
public:
    /** @summary A caller-owned command uses the VM's permanently reserved control record. */
    struct Work final { void (*run)(void*); void* context; std::exception_ptr error; bool done{}; };
    /** @summary Starts the sole serial interpreter lane. */
    ScriptLane() : worker_([this] { Run(); }) {}
    /** @summary Joins after runtime-owned references have drained and closed every Lua VM. */
    ~ScriptLane() {
        { const std::scoped_lock lock(mutex_); stopping_ = true; }
        wake_.notify_all(); worker_.join();
    }
    /** @summary Reserves a VM identity and control/retirement record before allocating Lua. */
    std::size_t Reserve(ScriptRuntime* runtime) {
        const std::scoped_lock lock(mutex_);
        for (std::size_t i = 0; i < slots_.size(); ++i) if (!slots_[i].runtime) {
            auto generation = nextGeneration_.load();
            do { if (generation == UINT64_MAX) throw std::runtime_error("Script generation space exhausted"); }
            while (!nextGeneration_.compare_exchange_weak(generation, generation + 1));
            slots_[i].runtime = runtime; slots_[i].generation = generation + 1;
            runtime->vmGeneration_ = slots_[i].generation; return i;
        }
        throw std::runtime_error("Script VM/control capacity exhausted (128)");
    }
    /** @summary Releases a closed VM without allocating a teardown request. */
    void Release(std::size_t index) noexcept {
        const std::scoped_lock lock(mutex_); slots_[index] = {};
    }
    /** @summary Binds the real published owner before scheduling; borrowed aliases never extend VM lifetime. */
    void Bind(std::size_t index, const std::shared_ptr<ScriptRuntime>& runtime) {
        const std::scoped_lock lock(mutex_); slots_[index].published = runtime;
    }
    /** @summary Fences current dispatch destruction before the last off-lane scheduler anchor can disappear. */
    void WaitIdle() {
        std::unique_lock lock(mutex_); wake_.wait(lock, [this] { return activeTicks_ == 0; });
    }
    /** @summary Dispatches through the VM's reserved record; same-lane native control does not queue.
     * @param index Private slot. @param run Owned native control operation. */
    template<class Function>
    void Invoke(std::size_t index, Function&& run) {
        if (OnLane()) { run(); return; }
        Work work{[](void* context) { (*static_cast<std::remove_reference_t<Function>*>(context))(); }, std::addressof(run)};
        std::unique_lock lock(mutex_);
        if (stopping_ || !slots_[index].runtime || slots_[index].work)
            throw std::runtime_error("Script control record unavailable");
        slots_[index].work = &work; wake_.notify_all();
        wake_.wait(lock, [&] { return work.done; });
        if (work.error) std::rethrow_exception(work.error);
    }
    /** @summary Returns native lane ownership without exposing a Lua pointer. */
    static bool OnLane() noexcept { return current_ != nullptr; }
private:
    /** @summary Keeps private VM identity valid until synchronous close and release finish. */
    struct Slot final { ScriptRuntime* runtime{}; Work* work{}; std::uint64_t generation{}; std::weak_ptr<ScriptRuntime> published; };
    /** @summary Rotates bounded VM visits without retaining the lane mutex during user code. */
    void Run() noexcept {
        current_ = this;
        std::size_t controlCursor{};
        std::size_t visitCursor{};
        bool controlTurn = true;
        auto nextTick = std::chrono::steady_clock::now();
        while (true) {
            Work* work{}; std::shared_ptr<ScriptRuntime> runtime;
            {
                std::unique_lock lock(mutex_);
                for (std::size_t n = 0; (controlTurn || std::chrono::steady_clock::now() < nextTick) && n < slots_.size(); ++n) {
                    auto& slot = slots_[(controlCursor + n) % slots_.size()];
                    if (slot.work) { work = slot.work; slot.work = nullptr; controlCursor = (controlCursor + n + 1) % slots_.size(); break; }
                }
                if (!work && stopping_) break;
                if (!work && std::chrono::steady_clock::now() < nextTick) {
                    wake_.wait_until(lock, nextTick); continue;
                }
                if (!work) {
                    for (std::size_t n = 0; n < slots_.size(); ++n) {
                        auto& slot = slots_[visitCursor++ % slots_.size()];
                        if (slot.runtime && slot.runtime->scheduled_.load()) {
                            runtime = slot.published.lock();
                            if (runtime) { ++activeTicks_; break; }
                        }
                    }
                    nextTick = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
                }
            }
            if (work) {
                try { work->run(work->context); } catch (...) { work->error = std::current_exception(); }
                { const std::scoped_lock lock(mutex_); work->done = true; }
                wake_.notify_all();
                controlTurn = false;
            } else if (runtime) {
                try { runtime->ScheduledTick(); } catch (...) { /* @summary Isolate host sink failures from the lane. */ }
                runtime.reset();
                { const std::scoped_lock lock(mutex_); --activeTicks_; }
                wake_.notify_all();
                controlTurn = true;
            }
        }
        current_ = nullptr;
    }
    inline static thread_local ScriptLane* current_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::array<Slot, 128> slots_{};
    inline static std::atomic_uint64_t nextGeneration_{};
    bool stopping_{};
    std::size_t activeTicks_{};
    std::thread worker_;
};
/** @summary Retains copied sinks independently of scheduler destruction. */
struct ScriptSchedulerHost final {
    ScriptScheduler::LogSink log;
    ScriptScheduler::CommandSink commands;
};
namespace {
/** @summary Reuses one live lane; every VM retains it through final Lua close. */
std::shared_ptr<ScriptLane> AcquireScriptLane() {
    static std::mutex mutex;
    static std::weak_ptr<ScriptLane> current;
    const std::scoped_lock lock(mutex);
    auto lane = current.lock();
    if (!lane) { lane = std::make_shared<ScriptLane>(); current = lane; }
    return lane;
}
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
    else if (self.droppedLogs_ != UINT64_MAX) ++self.droppedLogs_;
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
    self.Observe(ScriptPhase::Require);
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
    : ScriptRuntime(std::move(modId), definition, std::move(host), false) {}

ScriptRuntime::ScriptRuntime(BaseBootstrap, const ScriptDefinition& definition, ScriptHost host)
    : ScriptRuntime("simoder.base", definition, std::move(host), true) {}

ScriptRuntime::ScriptRuntime(std::string modId, const ScriptDefinition& definition, ScriptHost host, bool bootstrap)
    : bootstrap_(bootstrap), modId_(std::move(modId)), maximum_(definition.memoryBytes), budgetMs_(definition.budgetMs),
      modules_(definition.modules), resourceAliases_(definition.resourceAliases),
      propertyAliases_(definition.propertyAliases), host_(std::move(host)) {
    if (modId_ == "simoder.base" && !bootstrap_) throw std::runtime_error("Reserved base identity");
    lane_ = AcquireScriptLane();
    laneSlot_ = lane_->Reserve(this);
    try { lane_->Invoke(laneSlot_, [this, &definition] { Initialize(definition); }); }
    catch (...) { lane_->Release(laneSlot_); throw; }
}

bool ScriptRuntime::OnScriptLane() noexcept { return ScriptLane::OnLane(); }

void ScriptRuntime::Initialize(const ScriptDefinition& definition) {
    Observe(ScriptPhase::Create);
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
        if (!bootstrap_) {
        lua_newtable(state_);
        lua_pushcfunction(state_, &GetFloat, "simoder.props.getFloat"); lua_setfield(state_, -2, "getFloat");
        lua_pushcfunction(state_, &SetFloat, "simoder.props.setFloat"); lua_setfield(state_, -2, "setFloat");
        lua_pushcfunction(state_, &ClearFloat, "simoder.props.clearFloat"); lua_setfield(state_, -2, "clearFloat");
        lua_setreadonly(state_, -1, true); lua_setfield(state_, -2, "props");
        }
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
        Observe(ScriptPhase::Entry); Call(0, 1);
        if (!lua_istable(state_, -1)) throw std::runtime_error("Script must return a lifecycle table");
        tickRef_ = Callback("onTick");
        disableRef_ = Callback("onDisable");
        const int enable = Callback("onEnable");
        lua_pop(state_, 1);
        if (enable != -1) { Observe(ScriptPhase::Enable); lua_getref(state_, enable); Call(0, 0); lua_unref(state_, enable); }
        running_ = true;
        PublishStatus();
    } catch (...) { lua_close(state_); state_ = nullptr; reservedScriptMemory.fetch_sub(maximum_); reservedMemory_ = false; Observe(ScriptPhase::Close); throw; }
}

ScriptRuntime::~ScriptRuntime() {
    scheduled_.store(false);
    const std::scoped_lock control(controlMutex_);
    lane_->Invoke(laneSlot_, [this] { Close(); });
    lane_->Release(laneSlot_);
}
void ScriptRuntime::Close() noexcept {
    try { Stop(); }
    catch (...) { CaptureDiagnostic("Script retirement failed; forcing VM close"); }
    bool closed{};
    {
        const std::scoped_lock lock(mutex_);
        committed_ = false; running_ = false; stopping_ = true; commands_.clear();
        if (state_) { lua_close(state_); state_ = nullptr; closed = true; }
        if (reservedMemory_) { reservedScriptMemory.fetch_sub(maximum_); reservedMemory_ = false; }
        PublishStatus();
    }
    if (closed) Observe(ScriptPhase::Close);
}
void ScriptRuntime::Commit() {
    const std::scoped_lock lock(mutex_);
    committed_ = true;
    const std::scoped_lock statusLock(statusMutex_);
    publishedStatus_.running = running_;
}

void ScriptRuntime::Tick(double deltaSeconds) {
    if (!OnScriptLane()) {
        const std::scoped_lock control(controlMutex_);
        lane_->Invoke(laneSlot_, [this, deltaSeconds] { Tick(deltaSeconds); }); return;
    }
    const std::scoped_lock lock(mutex_);
    if (!running_ || !committed_ || tickRef_ == -1) return;
    const auto now = std::chrono::steady_clock::now();
    if (deltaSeconds < 0) deltaSeconds = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_ = now;
    try {
        Observe(ScriptPhase::Tick); lua_getref(state_, tickRef_); lua_pushnumber(state_, deltaSeconds); Call(1, 0);
    } catch (const std::exception& error) { running_ = false; CaptureDiagnostic(error.what()); }
    catch (...) { running_ = false; CaptureDiagnostic("Script callback failed with a nonstandard exception"); }
    PublishStatus();
}

void ScriptRuntime::Stop() {
    scheduled_.store(false);
    if (!OnScriptLane()) {
        const std::scoped_lock control(controlMutex_);
        lane_->Invoke(laneSlot_, [this] { Stop(); }); return;
    }
    const std::scoped_lock lock(mutex_);
    if (!committed_) return;
    committed_ = false;
    running_ = false;
    stopping_ = true;
    commands_.clear();
    if (disableRef_ != -1) {
        try { Observe(ScriptPhase::Disable); lua_getref(state_, disableRef_); Call(0, 0); }
        catch (const std::exception& error) { CaptureDiagnostic(error.what()); }
        catch (...) { CaptureDiagnostic("Script onDisable failed with a nonstandard exception"); }
    }
    PublishStatus();
}

ScriptStatus ScriptRuntime::Status() const {
    const std::scoped_lock lock(statusMutex_);
    return {publishedStatus_.running, publishedStatus_.memoryBytes,
        std::string(publishedStatus_.diagnostic.data(), publishedStatus_.diagnosticLength),
        publishedStatus_.generation, publishedStatus_.droppedLogs};
}
void ScriptRuntime::CaptureDiagnostic(std::string_view diagnostic) noexcept {
    diagnosticLength_ = std::min(diagnostic.size(), diagnostic_.size());
    if (diagnosticLength_) std::memcpy(diagnostic_.data(), diagnostic.data(), diagnosticLength_);
}
void ScriptRuntime::PublishStatus() noexcept {
    try {
        const std::scoped_lock lock(statusMutex_);
        publishedStatus_.running = running_ && committed_;
        publishedStatus_.memoryBytes = used_;
        publishedStatus_.diagnostic = diagnostic_;
        publishedStatus_.diagnosticLength = diagnosticLength_;
        publishedStatus_.generation = vmGeneration_;
        publishedStatus_.droppedLogs = droppedLogs_;
    } catch (...) { /* @summary Diagnostic publication cannot prevent retirement or VM release. */ }
}
void ScriptRuntime::Observe(ScriptPhase phase) noexcept {
    try { if (host_.lifecycle) host_.lifecycle(phase, vmGeneration_); }
    catch (...) { /* @summary Diagnostic observers cannot change committed runtime semantics. */ }
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
    const std::scoped_lock lock(mutex_); running_ = false; commands_.clear(); CaptureDiagnostic(diagnostic);
    PublishStatus();
}

void ScriptRuntime::ScheduledTick() {
    if (!scheduled_.load() || !schedulerHost_) return;
    if (std::chrono::steady_clock::now() - lastTick_ < std::chrono::milliseconds(50)) return;
    Tick(-1);
    const auto commands = TakePatchCommands();
    if (!commands.empty()) {
        std::string error;
        const auto borrowed = shared_from_this();
        if (!schedulerHost_->commands || !schedulerHost_->commands(modId_, borrowed, commands, error))
            Fail("PROP transaction rejected: " + error);
    }
    for (const auto& message : DrainLogs()) if (schedulerHost_->log) schedulerHost_->log(modId_, message);
}

ScriptScheduler::ScriptScheduler(LogSink sink, CommandSink commands)
    : lane_(AcquireScriptLane()), host_(std::make_shared<ScriptSchedulerHost>(ScriptSchedulerHost{std::move(sink), std::move(commands)})) {}
ScriptScheduler::~ScriptScheduler() { Shutdown(); }
void ScriptScheduler::Prepare(const std::shared_ptr<ScriptRuntime>& runtime) {
    if (ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler mutation from a script callback is unavailable");
    if (runtime) {
        const std::scoped_lock lock(runtime->mutex_);
        if (runtime->schedulerHost_ || runtime->scheduled_.load() || runtime->committed_)
            throw std::runtime_error("Script runtime is already bound or committed");
        runtime->schedulerHost_ = host_; lane_->Bind(runtime->laneSlot_, runtime);
    }
}
void ScriptScheduler::Shutdown() {
    if (ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler shutdown from a script callback is unavailable");
    std::map<std::string, std::shared_ptr<ScriptRuntime>> current;
    {
        const std::scoped_lock lock(mutex_);
        if (stopped_) return;
        stopped_ = true; current.swap(runtimes_);
        for (auto& [owner, runtime] : current) if (runtime) runtime->scheduled_.store(false);
    }
    lane_->WaitIdle();
    for (auto& [owner, runtime] : current) if (runtime) {
        runtime->Stop(); Flush(owner, runtime);
        const std::scoped_lock control(runtime->controlMutex_);
        runtime->lane_->Invoke(runtime->laneSlot_, [&runtime] { runtime->Close(); });
    }
}
void ScriptScheduler::Flush(const std::string& owner, const std::shared_ptr<ScriptRuntime>& runtime) {
    if (ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler flush from a script callback is unavailable");
    if (!runtime) return;
    try {
        const std::scoped_lock control(runtime->controlMutex_);
        runtime->lane_->Invoke(runtime->laneSlot_, [&] {
            for (const auto& message : runtime->DrainLogs()) if (host_->log) host_->log(owner, message);
        });
    }
    catch (...) { /* @summary Sink failure cannot prevent retirement. */ }
}
void ScriptScheduler::Put(const std::string& owner, std::shared_ptr<ScriptRuntime> runtime) {
    if (ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler mutation from a script callback is unavailable");
    if (runtime && !runtime->schedulerHost_) Prepare(runtime);
    std::shared_ptr<ScriptRuntime> replaced;
    {
        const std::scoped_lock lock(mutex_);
        if (stopped_) throw std::runtime_error("Script scheduler is stopping");
        replaced = std::move(runtimes_.at(owner));
        if (replaced) replaced->scheduled_.store(false);
        runtimes_.at(owner) = std::move(runtime);
        if (runtimes_.at(owner)) runtimes_.at(owner)->scheduled_.store(true);
    }
}
void ScriptScheduler::Reserve(const std::string& owner) {
    if (ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler mutation from a script callback is unavailable");
    const std::scoped_lock lock(mutex_);
    if (stopped_) throw std::runtime_error("Script scheduler is stopping");
    if (!runtimes_.contains(owner) && runtimes_.size() >= 128) throw std::runtime_error("Script scheduler owner capacity exhausted");
    runtimes_.try_emplace(owner);
}
void ScriptScheduler::ReleaseReservation(const std::string& owner) {
    const std::scoped_lock lock(mutex_);
    const auto found = runtimes_.find(owner);
    if (found != runtimes_.end() && !found->second) runtimes_.erase(found);
}
void ScriptScheduler::Remove(const std::string& owner, bool retainReservation) {
    if (ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler mutation from a script callback is unavailable");
    std::shared_ptr<ScriptRuntime> removed;
    {
        const std::scoped_lock lock(mutex_);
        if (const auto found = runtimes_.find(owner); found != runtimes_.end()) {
            if (found->second) found->second->scheduled_.store(false);
            removed = std::move(found->second);
            if (!retainReservation) runtimes_.erase(found);
        }
    }
}
}
