#pragma once
#include "scripting/semantic_bridge.hpp"
#include "scripting/script_runtime.hpp"
#include <array>

struct lua_State;
namespace sc13::scripting {
class ScriptRuntime;
/** @summary One VM's private Lua presentation/continuation bindings; semantic state remains in base and RuntimeScope. */
class SemanticLua final {
public:
    /** @summary Binds to actual native VM identity on the script lane. @param runtime Owning VM. */
    explicit SemanticLua(ScriptRuntime& runtime,runtime::copied::Reservation accounting);
    /** @summary Releases registry references before the interpreter closes. */
    ~SemanticLua();
    /** @summary Injects immutable host capabilities before untrusted source executes. */
    void Install();
    /** @summary Captures actual critical service objects after successful initialization. @param tableRef Lifecycle module registry reference. @return True for qualified complete critical graph. */
    bool Qualify(int tableRef);
    /** @summary Executes queued cross-VM delivery only at a later script turn outside other VM locks. */
    void Pump();
    /** @summary Closes endpoint admission and drops stale continuations without resuming old code. */
    void Stop() noexcept;
    /** @summary Copies the bounded host-owned diagnostic ring for the existing native host pipeline. */
    std::vector<SemanticDiagnostic> Diagnostics() const;
private:
    /** @summary Tagged userdata retains immutable copied values rather than cross-VM Lua tables. */
    struct Value;
    struct Task;
    struct Invocation;
    struct TaskLease;
    struct LifecycleRecord final {
        std::uint64_t epoch{},baseGeneration{},generation{},scopeEpoch{},revision{},sequence{},monotonicNs{};
        bool ready{};
        std::array<SemanticContextFact,4> contexts{};
        bool singlePlayerKnown{};
        bool operator==(const LifecycleRecord&) const = default;
    };
    struct Subscription final {
        int reference{-1};
        std::size_t first{128},tail{128},count{};
        LifecycleRecord last{};
        std::uint64_t serial{};
        std::uint64_t cursor{};
        bool faulted{},executing{},cancelled{};
    };
    struct MailEvent final { LifecycleRecord record; std::size_t next{128}; bool used{}; };
    struct Callback final { std::shared_ptr<TaskLease> task; int reference{-1}; std::uint64_t turn{}; };
    struct Coroutine final { lua_State* state{}; int reference{-1}; std::shared_ptr<TaskLease> waiting; std::uint64_t turn{}; bool started{}; };
    ScriptRuntime& owner_;
    std::shared_ptr<SemanticEndpoint> endpoint_;
    std::array<Callback,64> callbacks_{};
    std::array<Coroutine,64> coroutines_{};
    std::array<Subscription,64> subscriptions_{};
    std::array<MailEvent,128> mailbox_{};
    std::size_t mailboxCount_{};
    runtime::copied::Reservation accounting_;
    std::array<SemanticDiagnostic,64> diagnostics_{};
    std::size_t diagnosticFirst_{},diagnosticCount_{};
    std::uint64_t diagnosticSequence_{},diagnosticDropped_{};
    std::size_t retained_{};
    std::uint64_t turn_{};
    int servicesRef_{-1}, dispatchRef_{-1};
    bool stopped_{};
    std::uint64_t lifecycleSequence_{};
    std::uint64_t nextSubscription_{1};
    unsigned workCategory_{};
    std::size_t callbackCursor_{},coroutineCursor_{},subscriptionCursor_{};
    /** @summary Resolves the actual callback VM host, never a script-provided identity. */
    static SemanticLua& Self(lua_State* state);
    /** @summary Decode exact tagged input through the single shared codec. @param state Actual owner VM. @return Lua result count. */
    static int Validate(lua_State* state);
    /** @summary Encode a private immutable wrapper without numeric conversion. @param state Actual owner VM. @return Lua result count. */
    static int Wire(lua_State* state);
    /** @summary Admit a mod invocation or consume one private base dispatch token. @param state Actual owner VM. @return Lua result count. */
    static int Request(lua_State* state);
    /** @summary Revalidate delivery rights without changing a task outcome. @param state Actual owner VM. @return Lua result count. */
    static int Poll(lua_State* state);
    /** @summary Cancel only uncommitted native work. @param state Actual owner VM. @return Lua result count. */
    static int Cancel(lua_State* state);
    /** @summary Reserve exactly one later task callback. @param state Actual owner VM. @return Lua result count. */
    static int Observe(lua_State* state);
    /** @summary Yield only a host-registered retained coroutine. @param state Actual owner coroutine. @return Native Luau yield marker. */
    static int Await(lua_State* state);
    /** @summary Queue a retained owner coroutine for a later lane turn. @param state Actual owner VM. @return Lua result count. */
    static int Run(lua_State* state);
    /** @summary Copy an exact current native publication. @param state Actual owner VM. @return Lua result count. */
    static int Snapshot(lua_State* state);
    /** @summary Atomically reserve current publication followed by its ordered source cursor. @param state Actual owner VM. @return Lua result count. */
    static int Subscribe(lua_State* state);
    /** @summary Retire a native-bound observer and report an in-flight callback as Busy. @param state Actual owner VM. @return Lua result count. */
    static int Unsubscribe(lua_State* state);
    /** @summary Copy reviewed descriptor metadata without authority. @param state Actual owner VM. @return Lua result count. */
    static int Descriptors(lua_State* state);
    /** @summary Returns an owner-bound task or rejects foreign-Vm handle use. */
    Task& CheckTask(lua_State* state, int index);
    /** @summary Pushes an immutable copied-value wrapper. */
    void PushValue(lua_State* state, const runtime::copied::Envelope& value,const SemanticContextLease* captured=nullptr);
    /** @summary Pushes a private task userdata. */
    void PushTask(lua_State* state, runtime::TaskHandle task,const SemanticContextLease& context);
    /** @summary Pushes one rights-checked task record or sanitized error. */
    int PushPoll(lua_State* state, const TaskLease& task);
    /** @summary Pushes factual lifecycle identity, never a permission. */
    void PushSnapshot(lua_State* state);
    /** @summary Captures one host-authored lifecycle publication without reading another VM. */
    LifecycleRecord CaptureSnapshot();
    /** @summary Copies the exact native publication while retaining per-owner descriptive scope facts. */
    LifecycleRecord CaptureSnapshot(const SemanticPublication& publication);
    /** @summary Pushes a copied exact-identity lifecycle publication. */
    void PushSnapshot(lua_State* state,const LifecycleRecord& record);
    /** @summary Appends without allocation under the shared per-generation event/byte ceilings. */
    bool Enqueue(Subscription& subscription,const LifecycleRecord& record) noexcept;
    /** @summary Removes a closed observer's private events without delivering them to another observer. */
    void Clear(Subscription& subscription) noexcept;
    /** @summary Records a bounded failure and enforces the native DEV/REGULAR isolation policy. */
    void Failure(const char* code) noexcept;
    /** @summary Shares the scope's 64 resource slots with native retained dependencies. */
    bool AcquireRegistration() noexcept;
    /** @summary Returns one continuation slot even after revocation or stop. */
    void ReleaseRegistration() noexcept;
    /** @summary Records sanitized phase metadata after the authority boundary, never from the allocator/interrupt. */
    void Record(SemanticPhase phase,runtime::ScopeResult reason,const SemanticContextLease* context=nullptr) noexcept;
};
}
