#pragma once

#include <chrono>
#include "runtime/copied_value.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace sc13::platform { class RuntimeOwner; }
namespace sc13::runtime {
/** @summary Explicit native stop causes; none asserts graceful game termination. */
enum class StopReason { ExplicitShutdown, WindowClosing, OwnerLost, ActivationFailed };
/** @summary Bounded operation outcomes; a committed failure is never reported as undone. */
enum class ScopeResult { Ok, Denied, Stale, NotReady, Busy, Conflict, Cancelled, Indeterminate, BufferSmall, Invalid };
/** @summary Independent factual requirements for an existing operation. */
enum Readiness : std::uint32_t { RuntimeReady = 1, MenuReady = 2, CityReady = 4 };
/** @summary Host-issued value identity; possession does not bypass current authority validation. */
struct ScopeHandle final {
    std::uint64_t owner{};
    std::uint64_t serial{};
    bool operator==(const ScopeHandle&) const = default;
};
/** @summary Copied operation context; future providers must supply and revalidate these host-owned values. */
struct OperationContext final {
    std::uint64_t owner{};
    std::uint64_t epoch{};
    std::uint64_t grantGeneration{};
    std::uint64_t principal{};
    std::uint64_t session{};
    std::uint64_t entity{};
    std::uint64_t revision{};
    std::uint32_t opcode{};
    std::uint32_t readiness{RuntimeReady};
    std::chrono::steady_clock::time_point deadline{};
    bool operator==(const OperationContext&) const = default;
};
/** @summary Copied current scope facts, not an authorization token. */
struct ScopeSnapshot final {
    std::uint64_t generation{};
    std::uint64_t epoch{};
    std::uint64_t revision{};
    std::size_t running{};
    std::size_t retainedOperations{};
    bool activationCommitted{};
    bool stopping{};
    bool drained{};
};
/** @summary Task phase is independent of its irreversible commit marker. */
enum class TaskState { Queued, Running, Succeeded, Failed, Cancelled, Indeterminate };
/** @summary Copied delivery view; denial clears the view without changing retained outcome. */
struct TaskSnapshot final {
    TaskState state{TaskState::Queued};
    bool committed{};
    ScopeResult outcome{ScopeResult::Busy};
    copied::Envelope value;
};

/** @summary One core-owned writer lane/revision shared across independently revocable module scopes. */
class RuntimeCommitDomain final {
public:
    /** @summary Creates an initial writer domain; no authority or service is admitted. */
    RuntimeCommitDomain() = default;
    /** @summary Closes all child operation admission without locks, callbacks or joining. */
    void RequestStop() noexcept { stopping_.store(true); }
    /** @summary Observes the sticky shared admission gate. @return True after shared stop. */
    bool IsStopping() const noexcept { return stopping_.load(); }
    RuntimeCommitDomain(const RuntimeCommitDomain&) = delete;
    RuntimeCommitDomain& operator=(const RuntimeCommitDomain&) = delete;
private:
    std::mutex lane_;
    std::atomic<std::uint64_t> revision_{};
    std::atomic<std::size_t> pending_{};
    std::atomic_bool stopping_{};
    friend class RuntimeScope;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct RuntimeScopeTestAccess;
#endif
};

/** @summary Owns generic commit/lifetime state; production admission is closed until a qualified authority exists. */
class RuntimeScope final {
public:
    using CommitCallback = std::function<std::vector<std::byte>(std::span<const std::byte>)>;
    /** @summary Retains one actual freshly acquired owner; no authentication provider is installed.
     * @param owner Core-owned process-lifetime guard, or null for fail-closed construction.
     * @param domain Shared core writer lane; standalone instances get a private domain. */
    explicit RuntimeScope(std::shared_ptr<platform::RuntimeOwner> owner = {},
                          std::shared_ptr<RuntimeCommitDomain> domain = {});
    /** @summary Invalidates queued work; executing callbacks retain their dependencies to completion. */
    ~RuntimeScope();
    RuntimeScope(const RuntimeScope&) = delete;
    RuntimeScope& operator=(const RuntimeScope&) = delete;
    /** @summary Records manager postcommit only; never mints client authority. @return False after stop or missing owner. */
    bool CommitActivation();
    /** @summary Records verified observations supplied by core, never facts inferred by a client.
     * @param runtimePrepared Runtime preparation observed. @param menuObserved Verified menu observation.
     * @param cityObserved Verified city observation. */
    void SetReadiness(bool runtimePrepared, bool menuObserved = false, bool cityObserved = false);
    /** @summary Invalidates epochs and queued work without waiting or calling external code.
     * @param reason Diagnostic native stop cause. */
    void RequestStop(StopReason reason = StopReason::ExplicitShutdown) noexcept;
    /** @summary Waits boundedly outside game callbacks; timeout retains all dependency references.
     * @param timeout Nonnegative wait budget. @return True only after stop and safe resource drain. */
    bool DrainFor(std::chrono::milliseconds timeout);
    /** @summary Retains a dependency until safe drain. @param resource Core-owned resource. @return False at capacity or after stop. */
    bool RetainResource(std::shared_ptr<void> resource);
    /** @summary Reserve one continuation slot in the same generation budget as retained native dependencies.
     * @return False after stop or when the shared 64-resource ceiling is reached. */
    bool ReserveRegistration();
    /** @summary Return one reserved continuation slot without disclosing any task result. */
    void ReleaseRegistration() noexcept;
    /** @summary Copies current state. @return Snapshot without principal or credential secrets. */
    ScopeSnapshot Current() const;
    /** @summary Copies bounded input only after all current mandatory and permission gates.
     * @param context Host-issued context. @param input Copied input, maximum 1 MiB; consumer-specific limits still apply.
     * @param output Receives owner-bound operation on success, cleared on failure. @return Admission status. */
    ScopeResult Queue(const OperationContext& context, std::span<const std::byte> input, ScopeHandle& output);
    /** @summary Serializes one commit and rechecks current authority/revision immediately before invocation.
     * @param context Original issuing context. @param operation Owner-bound queued operation.
     * @param callback Trusted existing operation, never invoked under authority lock. @return Commit outcome; never automatically replays. */
    ScopeResult Commit(const OperationContext& context, ScopeHandle operation, CommitCallback callback);
    /** @summary Copies completed result only for the current original principal/session/context.
     * @param context Current host-issued context. @param operation Issued operation.
     * @param capacity Maximum bytes accepted. @param output Cleared on denial/short capacity. @return Completion or denial status. */
    ScopeResult Poll(const OperationContext& context, ScopeHandle operation, std::size_t capacity, std::vector<std::byte>& output) const;
    /** @summary Cancels only queued work; running/committed work is never represented as undone.
     * @param context Current issuing context. @param operation Issued operation. @return Cancellation outcome. */
    ScopeResult Cancel(const OperationContext& context, ScopeHandle operation);
    /** @summary Releases owned retained data even after revocation, without revealing it.
     * @param owner Issuing scope generation. @param operation Issued operation. @return Stale/denied/busy or successful cleanup. */
    ScopeResult Release(std::uint64_t owner, ScopeHandle operation);
    /** @summary Trusted bounded effect writes JSON and verifies its postcondition. */
    using ValueCallback = std::function<bool(const copied::Envelope&, std::span<char>, std::size_t&)>;
    /** @summary Reserves authority-bound task and native capacity before effects. @param context Host context. @param input Retained value. @param storage Shared pool. @param maxResultWire Reviewed limit. @param output Issued handle. @return Admission result. */
    ScopeResult QueueValue(const OperationContext& context, const copied::Envelope& input, const std::shared_ptr<copied::Storage>& storage, std::size_t maxResultWire, ScopeHandle& output);
    /** @summary Starts cancelable preparation. @param operation Native handle. @return Current gate. */
    ScopeResult BeginValue(ScopeHandle operation);
    /** @summary Revalidates then commits once on the existing writer domain. @param operation Native handle. @param callback Trusted effect.
     * @param guard Optional private atomic context fence; must not allocate, enter Lua or acquire locks.
     * @return Terminal outcome or precommit rejection without effects. */
    ScopeResult CommitValue(ScopeHandle operation, const ValueCallback& callback,
        const std::function<ScopeResult()>& guard = {});
    /** @summary Settles native owner-bound uncommitted work even after revocation; neither executes effects nor discloses payloads.
     * @param operation Native handle. @param reason Failure status. @return Completion status. */
    ScopeResult FinishValue(ScopeHandle operation, ScopeResult reason);
    /** @summary Revalidates delivery and shares a charged immutable result. @param operation Native handle. @param output Cleared on denial. @return Delivery status. */
    ScopeResult PollValue(ScopeHandle operation, TaskSnapshot& output) const;
    /** @summary Cancels queued/running-uncommitted task. @param operation Native handle. @return Cancellation status. */
    ScopeResult CancelValue(ScopeHandle operation);
private:
    struct State;
    std::shared_ptr<State> state_;
    /** @summary Validates authoritative state while its mutex is held.
     * @param state Current owned state. @param context Original host-issued context.
     * @param checkRevision Commit/submit require exact revision; result polling does not replay writers.
     * @return Current admission outcome. */
    static ScopeResult Validate(const State& state, const OperationContext& context, bool checkRevision);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct RuntimeScopeTestAccess;
#endif
};

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Synthetic authority exists only in separately compiled test targets, never runtime/Core. */
struct RuntimeScopeTestAccess final {
    /** @summary Inspect fixture cleanup without reopening a stopped generation. @param scope Fixture owner. @return Retained continuation slots. */
    static std::size_t Registrations(const RuntimeScope& scope);
    /** @summary Constructs a closed synthetic owner, never touching the production Global object.
     * @param domain Optional shared test writer lane. @return Test scope with deliberately local authority store. */
    static std::unique_ptr<RuntimeScope> Create(std::shared_ptr<RuntimeCommitDomain> domain = {});
    /** @summary Installs one local test grant and returns its bound context.
     * @param scope Test-only scope. @param sessionAllowed Full-session permission predicate.
     * @param entityAllowed Entity permission predicate. @param mandatoryAllowed All independent mandatory gates.
     * @param opcode Existing synthetic operation identity. @param lease Test-only lease lifetime.
     * @param readiness Exact test operation readiness mask. @return Issued test context. */
    static OperationContext Issue(RuntimeScope& scope, bool sessionAllowed = false, bool entityAllowed = true,
                                  bool mandatoryAllowed = true, std::uint32_t opcode = 1,
                                  std::chrono::milliseconds lease = std::chrono::minutes(1),
                                  std::uint32_t readiness = RuntimeReady);
    /** @summary Invalidates an existing grant without altering queued result ownership.
     * @param scope Test-only scope. @param session Bound session to revoke. */
    static void Revoke(RuntimeScope& scope, std::uint64_t session);
};
#endif
}
