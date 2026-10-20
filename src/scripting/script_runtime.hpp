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
#include "scripting/script_command_batch.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "runtime/runtime_scope.hpp"
#include <optional>
#include <atomic>
#include <array>
#include "mods/action_worker.hpp"

struct lua_State;
namespace sc13::mods::native { class NativeProvider; }
namespace sc13::mods { class ModManager; class ManagerClose; struct ManagerCloseTestAccess; struct ManagerPreparationTestAccess; class ManagerPreparation; class ManagerPublication; class PublicationOrigin; class OwnedPublication; class OwnerLifecycle; struct OwnerLifecycleTestAccess; }
namespace sc13::scripting {
/** @summary Native scheduler close witnesses; not a VM retirement or resource release certificate. */
struct SchedulerCloseObservation final {
    runtime::ScopeResult result{runtime::ScopeResult::NotReady};
    std::uint64_t revision{},generation{};
    std::size_t checked{},holders{},selected{},queued{},executing{};
};
class SemanticLua;
class SemanticBridge;
struct ScriptSchedulerState;
class SchedulerPublicationPlan;
class SchedulerPublicationReservation;
enum class SchedulerPublicationPurpose;
struct SchedulerPublicationTestAccess;
struct SemanticEndpoint;
/** @summary Carries an owned historical float observation without game addresses or borrowed storage. */
using FloatObservation = runtime::RuntimeFloatObservation;
/** @summary Identifies host-authored lifecycle evidence without Lua pointers or mod-owned text. */
enum class ScriptPhase { Create, Require, Entry, Enable, Tick, Disable, Close };
/** @summary Host-authored semantic phases; codes never include arbitrary mod paths, payloads or error text. */
enum class SemanticPhase { Admit,Dispatch,Delivery,Callback,Await,Stop,Fault };
/** @summary Bounded local correlation record; a build source fingerprint is not a crash symbol-match assertion. */
struct SemanticDiagnostic final {
    std::array<char,65> buildIdentity{};
    std::uint64_t sequence{},monotonicNs{},vmGeneration{},scopeGeneration{},runtimeEpoch{},baseEpoch{},operation{},dropped{};
    std::array<std::uint64_t,4> contextEpochs{};
    SemanticPhase phase{};
    runtime::ScopeResult reason{runtime::ScopeResult::Ok};
};
/** @summary Connects a sandbox to copied resource evidence; mutations are drained outside the VM lock. */
struct ScriptHost final {
    /** @summary Native host selects whole-mod isolation by default; development isolates only the failed semantic function. */
    enum class ErrorMode { Regular, Development };
    std::function<std::optional<FloatObservation>(const core::Tgi&, std::uint32_t)> readFloat;
    /** @summary Optional nonreentrant native diagnostics observer; receives only a host phase and private generation ID. */
    std::function<void(ScriptPhase, std::uint64_t)> lifecycle;
    /** @summary Native-only origin binding, never accepted from a mod manifest or public mod ID. */
    std::shared_ptr<SemanticEndpoint> semantic;
    /** @summary Compiler-produced, pin-verified base presentation modules copied into an independent mod VM. */
    std::map<std::string, std::string> semanticModules;
    ErrorMode errorMode{ErrorMode::Regular};
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
    /** @summary Copies bounded local correlation records through the native host; performs no export or logging file I/O. */
    [[nodiscard]] std::vector<SemanticDiagnostic> SemanticDiagnostics() const;
    /** @summary Transfers staged or committed requests after a successful callback, without publishing effects. */
    [[nodiscard]] std::vector<ScriptPatchCommand> TakePatchCommands();
    /** @summary Stops callbacks and records a host transaction failure while retaining the last committed overrides. */
    void Fail(std::string diagnostic);
private:
    friend class SemanticParticipant;
    friend class SemanticBridge;
    friend class mods::native::NativeProvider;
    friend class mods::ManagerCommands;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct mods::ManagerCommandsTestAccess;
#endif
    /** @summary Native closed route has weak owner references and never selects itself from manifest content. */
    std::shared_ptr<mods::ManagerCommandRoute> commandRoute_;
    /** @summary Single-assignment original worker lifetime anchor; read/published under V and never retargeted. */
    std::shared_ptr<mods::ActionWorker::Control> commandControl_;
    friend class mods::ManagerPreparation;
    friend class mods::ManagerClose;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct mods::ManagerCloseTestAccess;
#endif
    friend class mods::ManagerPublication;
    friend class mods::OwnerLifecycle;
    /** @summary Exact consumed native preparation identity; never sufficient for standalone admission. */
    std::shared_ptr<const void> preparationProof_;
    /** @summary Native-only initializer role cannot be requested by manifests or promoted through legacy publication. */
    struct Preparation final {};
    /** @summary Execute exact captured bytecode in restricted preparation mode.
     * @param modId Exact native owner. @param definition Immutable captured generation. @param host Actual retained Base endpoint. */
    ScriptRuntime(Preparation, std::string modId, const ScriptDefinition& definition, ScriptHost host);
    friend class mods::PublicationOrigin;
    friend class mods::OwnedPublication;
    /** @summary Deep immutable copy of the exact loaded definition, never a caller's later mutable object. */
    std::shared_ptr<const ScriptDefinition> loadedDefinition_;
    /** @summary Charges semantic host-side snapshot storage independently of the VM allocator budget. */
    runtime::copied::Reservation loadedDefinitionAccounting_;
    friend class SemanticLua;
    std::unique_ptr<SemanticLua> semantics_;
    bool semanticQualified_{};
    friend class BaseHost;
    /** @summary Native-only verified bootstrap role, inaccessible to ordinary callers. */
    struct BaseBootstrap final {};
    /** @summary Construct verified base using the shared lane and allocator. */
    ScriptRuntime(BaseBootstrap, const ScriptDefinition& definition, ScriptHost host);
    /** @summary Share lifecycle mechanics while enforcing private role identity. */
    ScriptRuntime(std::string modId, const ScriptDefinition& definition, ScriptHost host, bool bootstrap, bool preparation = false);
    bool bootstrap_{};
    bool preparation_{};
    friend class ScriptLane;
    friend class ScriptScheduler;
    friend struct ScriptSchedulerState;
    friend class SchedulerPublicationPlan;
    friend class SchedulerPublicationReservation;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct SchedulerPublicationTestAccess;
    friend struct CallbackBudgetTestAccess;
#endif
    /** @summary Initializes Lua only on the reserved script lane. */
    void Initialize(const ScriptDefinition& definition);
    /** @summary Closes Lua on the reserved lane after exactly-once committed retirement. */
    void Close() noexcept;
    /** @summary Reuses exact existing Close/Invoke/ack/slot release while actual controlMutex is held.
     * @param requireInactive Reject a still committed/scheduled VM. @return Exact release outcome; Invoke exceptions propagate. */
    runtime::ScopeResult CloseAndReleaseLocked(bool requireInactive);
    /** @summary Tick an observed routeless VM under V; a concurrently installed native route defers to its worker. @param selectedRegistration Original lane selection. */
    void ScheduledTick(std::uint64_t selectedRegistration);
    /** @summary Checks the exact retained selection under the existing VM lock, without issuing permissions.
     * @param selectedRegistration Original native lane selection revision. @return True only for executable current ownership. */
    bool ExecutionEligible(std::uint64_t selectedRegistration) const noexcept;
    /** @summary Executes only the original lane-selected registration under the VM guard.
     * @param deltaSeconds Explicit interval, or negative for elapsed monotonic time. @param selectedRegistration Exact retained dispatch revision. */
    void TickSelected(double deltaSeconds,std::uint64_t selectedRegistration);
    /** @summary Execute existing Pump/onTick mechanics while caller holds V. @param deltaSeconds Original interval. @param selectedRegistration Exact registration. */
    void TickSelectedLocked(double deltaSeconds,std::uint64_t selectedRegistration);
    /** @summary Worker-stack transfer isolates exact automatic output without a retained mailbox. */
    struct CapturedTick final {
        std::vector<ScriptPatchCommand> prior,output;
        bool executed{};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        /** @summary Borrowed worker-owned fixture callback, invoked after V release before Work.done. */
        const std::function<void()>* beforeAcknowledgment{};
#endif
    };
    /** @summary Marshal exact original automatic tick through existing reserved Work. @param registration Original selection. @param generation Original VM generation. @param route Original route identity. @param transfer Empty worker-owned transfer through acknowledgment. */
    void InvokeSelectedTick(std::uint64_t registration,std::uint64_t generation,const mods::ManagerCommandRoute* route,CapturedTick& transfer);
    /** @summary Isolate Pump plus this tick output under one V before Work.done. @param registration Original selection. @param generation Original generation. @param route Original route. @param transfer Worker-stack storage. */
    void CaptureSelectedTick(std::uint64_t registration,std::uint64_t generation,const mods::ManagerCommandRoute* route,CapturedTick& transfer);
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
    /** @summary Native registration revision sampled by lane selection and rechecked under the VM guard. */
    std::atomic_uint64_t registration_{};
    std::shared_ptr<runtime::RuntimeCommitDomain> publicationDomain_;
    std::shared_ptr<const std::byte> publicationNode_;
    bool sealed_{}, retired_{}, disableOwed_{};
    std::atomic_bool laneReleased_{};
    /** @summary Monotonic native closure request is visible before a queued lane teardown can execute. */
    std::atomic_bool stopRequested_{};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    std::function<void()> selectedBarrier_, extractedBarrier_;
#endif
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
    /** @summary Temporary exact-callback allowance retains the combined pending-plus-output 256 bound. */
    std::size_t commandLimit_{256};
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
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary One fixture-only cold callback observation; no runtime owner, timer exemption or production issuer. */
struct CallbackBudgetTestAccess final {
    /** @summary Distinguish an absent request from native arm rejection without throwing through a swallowed observer. */
    enum class ArmResult : std::uint8_t {NotRequested,Armed,Busy,WrongLane,InvalidGeneration,InvalidPhase,Fenced,AlreadyUsed,MismatchedRuntime};
    /** @summary Distinguish unentered/cancelled capture from a real protected-call result. */
    enum class Completion : std::uint8_t {NotReached,Cancelled,LuaReturn,NativeException};
    /** @summary Raw enclosing native CPU samples; 100 ns is a unit, not precision, and cycles are never elapsed time. */
    struct NativeSample final {
        std::uint64_t kernel100ns{},user100ns{},cycles{};
        std::uint32_t thread{},cpuError{},cycleError{};
        bool platformAvailable{},cpuAvailable{},cyclesAvailable{};
    };
    /** @summary Fixed scalar evidence read only after the existing synchronous initialization join. */
    struct Observation final {
        NativeSample before,after;
        std::uint64_t generation{},allocationCalls{},freeCalls{},reallocationCalls{},allocationFailures{},requestedBytes{};
        std::uint64_t usedBefore{},usedAfter{},peakUsed{},gcInterruptEvents{},instructionInterrupts{},detectedElapsedUs{};
        std::int64_t preSamplingNs{},callbackStartNs{},deadlineNs{},callbackEndNs{},postSamplingNs{},detectedDeadlineNs{};
        std::uint32_t budgetMs{},maximumDepth{};
        std::int32_t luaStatus{-1};
        ScriptPhase phase{ScriptPhase::Enable};
        ArmResult arm{ArmResult::NotRequested};Completion completion{Completion::NotReached};
        bool terminal{},borrowCleared{true},callbackStarted{},exceeded{},detectedElapsedValid{},nested{},overflow{},sameThread{},wallOrderValid{},cpuOrderValid{},cycleOrderValid{};
    };
    static_assert(sizeof(Observation)<=384);
    static_assert(sizeof(Observation)<=512);
    /** @summary Arm once from the real first Enable observer on the lane, retaining only a borrowed fixed destination.
     * @param generation Actual observer generation. @param phase Actual Enable label. @param output Test-owned untouched destination.
     * @return Explicit admission; output must outlive joined construction and the retained observer. */
    static ArmResult Arm(std::uint64_t generation,ScriptPhase phase,Observation& output) noexcept;
    /** @summary Arm only from the authentic native preparation Entry observer of this exact generation.
     * @param generation Actual observer generation; never a caller-created runtime identity.
     * @param output Untouched fixed destination retained through joined construction and any retained observer.
     * @return Explicit diagnostic admission; grants no operation or runtime permission. */
    static ArmResult ArmPreparationEntry(std::uint64_t generation,Observation& output) noexcept;
    /** @summary Cancel only an unconsumed matching arm during handled initialization Close; never an active/foreign frame.
     * @param generation Actual Close generation. @return Whether the original borrowed slot was cleared. */
    static bool Cancel(std::uint64_t generation) noexcept;
private:
    friend class ScriptRuntime;
    /** @summary Fixed outer/native-nested classification without a second sample or runtime owner. */
    enum class Role : std::uint8_t {None,Outer,Nested,NestedOverflow};
    /** @summary Clears the original borrowed capture before any protected-call return or native exception cleanup. */
    class Frame final {
        ScriptRuntime* runtime_{};Role role_{Role::None};bool finished_{};
    public:
        /** @summary Install no-throw cleanup before sampling a consumed slot. @param runtime Exact actual caller. */
        explicit Frame(ScriptRuntime& runtime) noexcept;
        /** @summary Preserve clear/finalization during native unwinding without replacing an exception. */
        ~Frame() noexcept;
        Frame(const Frame&)=delete;Frame& operator=(const Frame&)=delete;
        /** @summary Copy the existing exact deadline after its unchanged assignment, with no new clock query. */
        void Deadline() noexcept;
        /** @summary Finish before original stack/command/string cleanup. @param status Protected Lua status or -1. @param nativeException Native unwind. */
        void Finish(int status,bool nativeException) noexcept;
    };
    static_assert(sizeof(Frame)<=64);
    /** @summary Admit exact bootstrap/generation and sample before its original watchdog starts. @param runtime Actual caller. @return Native frame role. */
    static Role Begin(ScriptRuntime& runtime) noexcept;
    /** @summary Copy only original watchdog scalars. @param runtime Exact outer runtime. */
    static void Deadline(ScriptRuntime& runtime) noexcept;
    /** @summary Publish one terminal sample and clear its borrowed TLS pointer. @param runtime Exact caller. @param role Original frame role. @param status Lua status. @param nativeException Native unwind. */
    static void End(ScriptRuntime& runtime,Role role,int status,bool nativeException) noexcept;
    /** @summary Count actual Lua allocator attempts only for the original active runtime. @param runtime Exact allocator owner. @param hasPointer Reallocation source exists. @param size Actual requested bytes. */
    static void Allocating(ScriptRuntime& runtime,bool hasPointer,std::size_t size) noexcept;
    /** @summary Observe unchanged allocator return/state after the real operation. @param runtime Original owner. @param success Real allocation/free result. */
    static void Allocated(ScriptRuntime& runtime,bool success) noexcept;
    /** @summary Count an existing VM interrupt without querying CPU or changing its policy. @param runtime Exact owner. @param gc Original GC label. */
    static void Interrupting(const ScriptRuntime& runtime,int gc) noexcept;
    /** @summary Copy the existing watchdog failure's exact now/deadline-derived evidence. @param runtime Original owner. @param elapsed Original elapsed microseconds. */
    static void Exceeded(const ScriptRuntime& runtime,std::int64_t elapsed) noexcept;
};
#endif
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
    friend class mods::ModManager;
    friend class mods::ManagerClose;
    friend class mods::ManagerPreparation;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct mods::ManagerCloseTestAccess;
    friend struct mods::ManagerPreparationTestAccess;
#endif
    /** @summary Associate an initialized uncommitted preparation with its original host without activation.
     * @param vm Actual candidate. @return Exact role/identity/closed-state qualification or try-only contention. */
    runtime::ScopeResult BindPreparation(const std::shared_ptr<ScriptRuntime>& vm) noexcept;
    /** @summary Retains the original atomic ingress latch without borrowing the scheduler.
     * @return Fixed exact host latch. */
    std::shared_ptr<std::atomic_bool> CloseAdmission() const noexcept;
    /** @summary Tries a bounded scan of actual lane slots for this exact host.
     * @param cursor Original scan position. @param revision Original host activity revision. @return Actual bounded witnesses. */
    SchedulerCloseObservation PollClose(std::size_t& cursor,std::uint64_t& revision) noexcept;
    /** @summary Tries current original-host activity under actual L without a slot traversal. @return Native counter snapshot. */
    SchedulerCloseObservation ObserveCloseActivity() noexcept;
    /** @summary Explicit off-lane closure of one exact original current VM, using existing lane controls.
     * @param anchor Original scheduler State. @param vm Current record VM. @param generation Exact original slot identity.
     * @param closedNow Reports a newly acknowledged release only. @return Exact result; exception leaves VM retained. */
    static runtime::ScopeResult CloseCurrent(const std::shared_ptr<ScriptSchedulerState>& anchor,
        const std::shared_ptr<ScriptRuntime>& vm,std::uint64_t generation,bool& closedNow) noexcept;
    /** @summary Tries original Plan target provenance without closing or activating a VM.
     * @param anchor Original scheduler State. @param vm Genuine retained VM. @param generation Exact identity.
     * @param preparation Require original uncommitted preparation role. @return Native qualification. */
    static runtime::ScopeResult InspectCloseTarget(const std::shared_ptr<ScriptSchedulerState>& anchor,
        const std::shared_ptr<ScriptRuntime>& vm,std::uint64_t generation,bool preparation) noexcept;
    /** @summary Shares exact slot/host/role checks with control and V already held.
     * @param anchor Original scheduler. @param vm Original target. @param generation Exact identity.
     * @param preparation Require uncommitted role. @return Try-L result. */
    static runtime::ScopeResult QualifyCloseLocked(const std::shared_ptr<ScriptSchedulerState>& anchor,
        const std::shared_ptr<ScriptRuntime>& vm,std::uint64_t generation,bool preparation) noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    /** @summary Executes a real ordinary control using its existing permanent slot. @param vm Exact VM. @param callback Native body. */
    static void InvokeCloseControl(const std::shared_ptr<ScriptRuntime>& vm,const std::function<void()>& callback);
    /** @summary Copies actual slot work under L. @param vm Exact VM. @return Native witnesses. */
    static SchedulerCloseObservation ObserveCloseControl(const std::shared_ptr<ScriptRuntime>& vm);
    /** @summary Hold the actual lane mutex for a bounded native fixture interleaving, never Lua.
     * @param callback Native gate; its owner must release/join before returning the borrowed candidate. */
    void HoldPreparationLane(const std::function<void()>& callback);
    /** @summary Observe exact VM/slot/original host association without creating another owner.
     * @param vm Original retained fixture VM. @return Ok, Stale, Denied or actual contention. */
    runtime::ScopeResult PreparationBinding(const std::shared_ptr<ScriptRuntime>& vm) noexcept;
#endif
    friend class mods::ManagerCommands;
    friend class SchedulerPublicationPlan;
    friend class SchedulerPublicationReservation;
    friend class mods::OwnerLifecycle;
    friend class mods::ManagerPublication;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct SchedulerPublicationTestAccess;
#endif
    /** @summary Retains the actual sole scheduler map/lock after stack-owner destruction invalidates plans. */
    std::shared_ptr<ScriptSchedulerState> state_;
    std::shared_ptr<ScriptLane> lane_;
    std::shared_ptr<ScriptSchedulerHost> host_;
    /** @summary Prepares a typed change through native identities; no production caller is enrolled.
     * @param owner Native fixture node name. @param candidate Actual initialized uncommitted VM, null for removal.
     * @param purpose Closed native change kind. @param output Receives a one-shot retained plan only on success.
     * @param preparationProof Exact consumed canonical preparation, empty for standalone fixture admission.
     * @return Ok, or a rejection with no operational publication. */
    runtime::ScopeResult PreparePublication(const std::string& owner, const std::shared_ptr<ScriptRuntime>& candidate,
        SchedulerPublicationPurpose purpose, SchedulerPublicationPlan& output,
        const std::shared_ptr<const void>& preparationProof = {});
    /** @summary Publishes a prepared isolated scheduler change with a nonblocking typed fence.
     * @param plan Exact native preparation, consumed only by successful publication. @return Ok or a pre-marker rejection. */
    static runtime::ScopeResult PublishPublication(SchedulerPublicationPlan& plan);
};
}
