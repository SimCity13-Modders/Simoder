#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <condition_variable>
#include <functional>
#include <map>
#include <thread>
#include <set>
#include "mods/mod_types.hpp"
#include "scripting/script_patch_command.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include <optional>
#include <atomic>
#include <array>

struct lua_State;
namespace sc13::scripting {
/** @summary Carries an owned historical float observation without game addresses or borrowed storage. */
using FloatObservation = runtime::RuntimeFloatObservation;
/** @summary Identifies host-authored lifecycle evidence without Lua pointers or mod-owned text. */
enum class ScriptPhase { Create, Require, Entry, Enable, Tick, Disable, Close };
/** @summary Connects a sandbox to copied resource evidence; mutations are drained outside the VM lock. */
struct ScriptHost final {
    std::function<std::optional<FloatObservation>(const core::Tgi&, std::uint32_t)> readFloat;
    /** @summary Optional nonreentrant native diagnostics observer; receives only a host phase and private generation ID. */
    std::function<void(ScriptPhase, std::uint64_t)> lifecycle;
};
/** @summary Keeps bytecode, source identity and limits in one immutable, compiler-produced generation. */
struct ScriptDefinition final {
    std::string bytecode;
    std::string entry;
    std::string language;
    std::string revision;
    std::size_t memoryBytes{8U * 1024U * 1024U};
    unsigned budgetMs{2};
    /** @summary Holds compiler-produced local Luau modules; no runtime filesystem require is exposed. */
    std::map<std::string, std::string> modules;
    /** @summary Resolves names only from a validated per-project catalog with explicit provenance. */
    std::map<std::string, core::Tgi, std::less<>> resourceAliases;
    std::map<std::string, mods::PropertyPatch, std::less<>> propertyAliases;
};
/** @summary Reports host-owned script evidence without exposing VM pointers to mods. */
struct ScriptStatus final {
    bool running{};
    std::size_t memoryBytes{};
    std::string diagnostic;
    std::uint64_t generation{};
    std::uint64_t droppedLogs{};
};
/** @summary Owns one isolated, bounded Luau VM and transactional staged lifecycle callbacks. */
class ScriptLane;
struct ScriptSchedulerHost;
class ScriptRuntime final : public std::enable_shared_from_this<ScriptRuntime> {
public:
    /** @summary Reports whether the caller owns the sole script lane; synchronous writer reentry is rejected. */
    [[nodiscard]] static bool OnScriptLane() noexcept;
    /** @summary Creates a sandbox and executes initialization with buffered effects; throws on failure. */
    ScriptRuntime(std::string modId, const ScriptDefinition& definition, ScriptHost host = {});
    /** @summary Closes the VM after the service stops scheduling it. */
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;
    /** @summary Commits buffered initialization messages after mod activation succeeds. */
    void Commit();
    /** @summary Runs a bounded background tick; a failed callback stops only this script. */
    void Tick(double deltaSeconds);
    /** @summary Calls onDisable once and closes scheduling, even when the callback fails. */
    void Stop();
    /** @summary Returns an immutable status snapshot under the VM lock. */
    [[nodiscard]] ScriptStatus Status() const;
    /** @summary Drains bounded messages; logging happens outside the VM lock. */
    [[nodiscard]] std::vector<std::string> DrainLogs();
    /** @summary Transfers staged or committed requests after a successful callback, without publishing effects. */
    [[nodiscard]] std::vector<ScriptPatchCommand> TakePatchCommands();
    /** @summary Stops callbacks and records a host transaction failure while retaining the last committed overrides. */
    void Fail(std::string diagnostic);
private:
    friend class ScriptLane;
    friend class ScriptScheduler;
    /** @summary Initializes Lua only on the reserved script lane. */
    void Initialize(const ScriptDefinition& definition);
    /** @summary Closes Lua on the reserved lane after exactly-once committed retirement. */
    void Close() noexcept;
    /** @summary Visits one published VM on the sole lane, then dispatches copied effects outside its lock. */
    void ScheduledTick();
    /** @summary Publishes a copied diagnostic snapshot without retaining a VM lock in readers. */
    void PublishStatus() noexcept;
    /** @summary Retains a bounded diagnostic without allocation, including host-allocation failure paths. */
    void CaptureDiagnostic(std::string_view diagnostic) noexcept;
    /** @summary Emits native lifecycle evidence; observer failures cannot alter Lua behavior. */
    void Observe(ScriptPhase phase) noexcept;
    std::shared_ptr<ScriptLane> lane_;
    std::size_t laneSlot_{};
    std::uint64_t vmGeneration_{};
    std::shared_ptr<ScriptSchedulerHost> schedulerHost_;
    std::atomic_bool scheduled_{};
    std::mutex controlMutex_;
    /** @summary Enforces the per-mod allocator cap on every VM allocation. */
    static void* Allocate(void* context, void* pointer, std::size_t oldSize, std::size_t newSize);
    /** @summary Interrupts loops and calls when the callback execution deadline expires. */
    static void Interrupt(lua_State* state, int gc);
    /** @summary Appends one bounded mod-owned message to the host buffer. */
    static int Log(lua_State* state);
    /** @summary Reports only implemented script capabilities. */
    static int HasCapability(lua_State* state);
    /** @summary Returns elapsed monotonic seconds without game memory access. */
    static int Time(lua_State* state);
    /** @summary Resolves relative local modules from the immutable generation with cycle detection. */
    static int Require(lua_State* state);
    /** @summary Returns copied historical PROP float evidence, or nil when nothing has been observed. */
    static int GetFloat(lua_State* state);
    /** @summary Buffers a finite float override; no native mutation happens inside Lua. */
    static int SetFloat(lua_State* state);
    /** @summary Removes only the calling mod's runtime override after successful callback completion. */
    static int ClearFloat(lua_State* state);
    /** @summary Parses full textual identities without silently truncating unsupported instance bits. */
    static ScriptPatchCommand PatchTarget(lua_State* state);
    /** @summary Executes an already-pushed callable with a fresh deadline and converts VM errors to C++ errors. */
    void Call(int arguments, int results);
    /** @summary Captures a supported optional lifecycle callback from the returned module table. */
    int Callback(const char* name);
    lua_State* state_{};
    std::string modId_;
    std::array<char, 2048> diagnostic_{};
    std::size_t diagnosticLength_{};
    std::size_t used_{};
    std::size_t maximum_{};
    unsigned budgetMs_{};
    int tickRef_{-1};
    int disableRef_{-1};
    bool running_{};
    bool committed_{};
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point lastTick_{start_};
    bool reservedMemory_{};
    std::vector<std::string> logs_;
    std::uint64_t droppedLogs_{};
    std::map<std::string, std::string> modules_;
    std::map<std::string, int> moduleRefs_;
    std::set<std::string> loadingModules_;
    std::map<std::string, core::Tgi, std::less<>> resourceAliases_;
    std::map<std::string, mods::PropertyPatch, std::less<>> propertyAliases_;
    ScriptHost host_;
    std::vector<ScriptPatchCommand> commands_;
    bool stopping_{};
    mutable std::mutex mutex_;
    mutable std::mutex statusMutex_;
    /** @summary Keeps teardown publication independent of allocating the external reader's string copy. */
    struct PublishedStatus final {
        bool running{};
        std::size_t memoryBytes{};
        std::array<char, 2048> diagnostic{};
        std::size_t diagnosticLength{};
        std::uint64_t generation{};
        std::uint64_t droppedLogs{};
    } publishedStatus_;
};
/** @summary Schedules mod scripts on one background thread, independent of render and resource hooks. */
class ScriptScheduler final {
public:
    using LogSink = std::function<void(const std::string&, const std::string&)>;
    /** @summary Publishes a callback batch outside all VM locks, checking active VM identity in the host. */
    using CommandSink = std::function<bool(const std::string&, const std::shared_ptr<ScriptRuntime>&,
        const std::vector<ScriptPatchCommand>&, std::string&)>;
    /** @summary Starts a bounded 20 Hz scheduler with an optional attributed log sink. */
    explicit ScriptScheduler(LogSink sink = {}, CommandSink commands = {});
    /** @summary Joins the worker before any VM or sink is destroyed. */
    ~ScriptScheduler();
    /** @summary Prebinds copied sinks before committing publication; performs no Lua entry. */
    void Prepare(const std::shared_ptr<ScriptRuntime>& runtime);
    /** @summary Atomically installs a committed VM, retaining in-flight references until callbacks finish. */
    void Put(const std::string& owner, std::shared_ptr<ScriptRuntime> runtime);
    /** @summary Allocates a scheduler slot before registry publication so later swaps cannot allocate. */
    void Reserve(const std::string& owner);
    /** @summary Releases an unpublished empty slot after failed preparation without touching an active VM. */
    void ReleaseReservation(const std::string& owner);
    /** @summary Drains lifecycle logs after a VM is removed from scheduling. */
    void Flush(const std::string& owner, const std::shared_ptr<ScriptRuntime>& runtime);
    /** @summary Stops and joins callback scheduling while retaining the log sink for final lifecycle callbacks. */
    void Shutdown();
    /** @summary Removes one scheduled VM without closing an in-flight callback's state. */
    void Remove(const std::string& owner, bool retainReservation = false);
private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<ScriptRuntime>> runtimes_;
    std::shared_ptr<ScriptLane> lane_;
    std::shared_ptr<ScriptSchedulerHost> host_;
    bool stopped_{};
};
}
