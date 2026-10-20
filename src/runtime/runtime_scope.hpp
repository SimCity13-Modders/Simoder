#pragma once

#include <chrono>
#include "runtime/copied_value.hpp"
#include "runtime/runtime_authority_source.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include <variant>
#include "mods/patch_registry.hpp"

namespace sc13::platform { class RuntimeOwner; }
namespace sc13::scripting { class SemanticBridge; }
namespace sc13::mods { class OwnerLifecycle; class ManagerCommands; class ManagerClose; class LifecycleDecisionLease; struct LifecycleDecisionTestAccess; struct ManagerCommandTicket; struct ManagerCommandsTestAccess; struct OwnerLifecycleTestAccess; }
namespace sc13::mods::native { class NativeProvider; }
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
/** @summary Reviewed decision identity is distinct from effective authority and operation reservations. */
struct DecisionReference final {
    std::uint64_t serial{}, revision{}, action{};
    bool operator==(const DecisionReference&) const = default;
};
/** @summary Copied operation reservation; identities are checked against the sole native grant store. */
struct OperationContext final {
    std::uint64_t owner{};
    std::uint64_t epoch{};
    std::uint64_t grantGeneration{};
    std::uint64_t grantRevision{};
    OperationOrigin origin;
    DecisionReference decision;
    std::uint64_t reservation{};
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
    /** @summary Original task publication identity; polling never advances it and denial clears it. */
    std::uint64_t sequence{};
    /** @summary Original admission or irreversible writer revision, never the current revision at poll. */
    std::uint64_t revision{};
};

/** @summary One core-owned writer lane/revision shared across independently revocable module scopes. */
class RuntimeCommitDomain final {
public:
    /** @summary Creates an initial writer domain; no authority or service is admitted. */
    RuntimeCommitDomain() = default;
    /** @summary Core-only ordered writer reservation, never an authority token. */
    class Writer final {
    public:
        /** @summary Releases the lane without calling external code. */
        ~Writer();
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        /** @summary Reports successful lane ownership. @return False for contention or same-thread reentry. */
        bool Acquired() const noexcept { return lock_.owns_lock(); }
        /** @summary Checks revision capacity and sticky stop before a nonfallible publication. */
        bool CanAdvance() const noexcept;
        /** @summary Advances at the prepared publication fence. @return Exact new revision, or zero if unavailable. */
        std::uint64_t Advance() noexcept;
        /** @summary Advances only an already committed lifecycle rollback after stop; never admits new work.
         * @return Exact rollback revision, or zero without capacity/ownership. */
        std::uint64_t AdvanceRollback() noexcept;
    private:
        /** @summary Acquires before any manager/registry locks. @param wait Existing legacy drains may wait; new task admission may not. */
        Writer(RuntimeCommitDomain& domain, bool wait);
        RuntimeCommitDomain& domain_;
        std::unique_lock<std::mutex> lock_;
        Writer* previous_{};
        friend class RuntimeCommitDomain;
        friend class RuntimeScope;
    };
    /** @summary Reserves one core writer. @param wait True only for existing synchronous legacy writers outside all other locks.
     * @return Stack reservation; recursive same-domain admission fails rather than deadlocking. */
    Writer ReserveWriter(bool wait = false) { return Writer(*this, wait); }
    /** @summary Detects same-thread native writer reentry before lifecycle preparation or Lua execution. */
    bool WriterActiveOnCurrentThread() const noexcept;
    /** @summary Observes the current writer revision without granting publication rights. */
    std::uint64_t Revision() const noexcept { return revision_.load(); }
    /** @summary Closes all child operation admission without locks, callbacks or joining. */
    void RequestStop() noexcept { stopping_.store(true); }
    /** @summary Observes the sticky shared admission gate. @return True after shared stop. */
    bool IsStopping() const noexcept { return stopping_.load(); }
    RuntimeCommitDomain(const RuntimeCommitDomain&) = delete;
    RuntimeCommitDomain& operator=(const RuntimeCommitDomain&) = delete;
private:
    static thread_local Writer* activeWriter_;
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
    /** @summary Reserves the existing shared native writer lane without granting authority. */
    RuntimeCommitDomain::Writer ReserveWriter() { return stateDomain().ReserveWriter(); }
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
    using AuthorityReview=RuntimeAuthoritySource::Review;
    using AuthorityChallenge=RuntimeAuthoritySource::Challenge;
    using AuthorityPermission=RuntimeAuthoritySource::PermissionView;
    /** @summary Match the original privately installed source, never a scalar role assertion.
     * @param state Original Q-held scope. @param origin Captured provenance. @return Qualified source/category/content. */
    static bool SourceMatches(const State& state,const OperationOrigin& origin) noexcept;
    /** @summary Prepare one bounded original review without issuing authority.
     * @param review Source-qualified immutable request. @return Original pending reference, or empty rejection. */
    AuthorityChallenge ChallengeAuthority(const AuthorityReview& review);
    /** @summary Consume one exact typed source/local-action proof into the original grant store once.
     * @param decision Actual source-bound proof; fixture drivers exist only in test builds. @return Original context, empty on rejection. */
    OperationContext DecideAuthority(const RuntimeAuthoritySource::Decision& decision);
    /** @summary Join through the actual source-bound participant proof, preserving original grant causality.
     * @param context Existing reviewed original reservation. @param participant Verified original source/actor/channel.
     * @return Separate original reservation, never a renewed decision. */
    OperationContext JoinAuthority(const OperationContext& context,const RuntimeAuthoritySource::Participant& participant);
    /** @summary Revoke one actual source-bound consent session under original D/Q.
     * @param control Exact original source and session. @return Actual revoke outcome. */
    ScopeResult RevokeAuthoritySession(const RuntimeAuthoritySource::Control& control);
    /** @summary Close an original authenticated channel without inheritance.
     * @param control Actual source and channel. @return Actual disconnect outcome. */
    ScopeResult DisconnectAuthority(const RuntimeAuthoritySource::Control& control);
    /** @summary Revoke a retained original grant without replay or result mutation.
     * @param control Actual source and grant. @return Actual revoke outcome. */
    ScopeResult RevokeAuthority(const RuntimeAuthoritySource::Control& control);
    /** @summary Inspect only through the same original evaluator.
     * @param context Original reservation. @return Sanitized effective projection. */
    AuthorityPermission InspectAuthority(const OperationContext& context) const;
    friend class SemanticTasks;
    friend class mods::native::NativeProvider;
    friend struct mods::ManagerCommandTicket;
    friend class mods::OwnerLifecycle;
    friend class mods::ManagerCommands;
    friend class mods::ManagerClose;
    friend class mods::LifecycleDecisionLease;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct mods::LifecycleDecisionTestAccess;
    /** @summary Only the separately compiled fixture bridge may reuse existing atomic LifecycleGuard validation. */
    friend class scripting::SemanticBridge;
#endif
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct mods::ManagerCommandsTestAccess;
#endif
    /** @summary Exact native ticket is release-only bookkeeping, never operational authority. */
    class CommandTicket final {
        friend class RuntimeScope;
        friend struct ScopeRetirementTestAccess;
        std::uint64_t owner_{},serial_{};
        std::size_t slot_{};
    };
    /** @summary Independent scalar scope certificate; never manager/VM/final loader readiness. */
    struct RetirementObservation final {
        ScopeResult result{ScopeResult::NotReady};
        std::uint64_t generation{},disposalSequence{},disposalAcknowledged{};
        /** @summary commandTickets is the historical field name for combined exact command and lifecycle tracked tickets. */
        std::size_t running{},registrations{},operations{},resources{},unknownResources{},commandTickets{},rootReservations{},nonCommandFreshReservations{};
        std::size_t mailboxWriting{},mailboxReady{},mailboxDraining{},ticketBytes{},disposalBytes{},metadataBytes{};
        bool stopping{},scopeReady{},disposalPending{},invariant{};
    };
    /** @summary Retire one exact ticket immediately or through the existing bounded mailbox.
     * @param ticket Original native incarnation. @return Ok acknowledged, NotReady transferred, or bounded rejection. */
    ScopeResult ReleaseCommandTicket(const CommandTicket& ticket) noexcept;
    /** @summary Scalar-only try-Q observation; no payload extraction or arbitrary destructor.
     * @return Original scope certificate with explicit pending ownership. */
    RetirementObservation ObserveRetirement() noexcept;
    /** @summary Explicit off-lane disposal of at most one node and resource outside all guards.
     * @pre Native caller owns no publication/State/VM/Q/status guard or current borrowed callback.
     * @return Bounded admission outcome; accepted native destructor duration is unbounded. */
    ScopeResult DrainRetirementOne() noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ScopeRetirementTestAccess;
#endif
    /** @summary Distinguishes release-only lifecycle bookkeeping from committed command admission; neither role issues authority. */
    enum class ReservationRole { Command, LifecycleDecision };
    /** @summary Share exact fresh reservation insertion while the caller owns original Q.
     * @param context Original reviewed reservation. @param role Native bookkeeping role selecting actual scope phase.
     * @return Fresh original-scope context or empty admission/capacity rejection. */
    OperationContext ReserveFreshLocked(const OperationContext& context,ReservationRole role);
    /** @summary Atomically reserve existing reviewed provenance and exact ticket under the same try-Q.
     * @param context Original reviewed grant. @param ticket Empty exact native output. @return Fresh context or empty denial. */
    OperationContext TryReserveCommand(const OperationContext& context,CommandTicket& ticket);
    /** @summary Shares the exact original fresh producer without creating another grant or reservation budget.
     * @param context Reviewed original context. @param ticket Empty exact output. @param role Explicit native bookkeeping role, never inferred from status output.
     * @param status Optional precise admission result.
     * @return Fresh context coupled to the existing native ticket under one Q. */
    OperationContext TryReserveTracked(const OperationContext& context,CommandTicket& ticket,ReservationRole role,ScopeResult* status=nullptr);
    /** @summary Validates exact lifecycle root bookkeeping in the actual original scope phase under try Q without activation, reservation or release.
     * @param context Original reviewed root. @return Native root lineage/admission result. */
    ScopeResult CheckRootDecision(const OperationContext& context) noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct mods::OwnerLifecycleTestAccess;
#endif
    /** @summary Closed typed lifecycle fence; never exposes grants or runs caller callbacks. */
    class LifecycleGuard final {
    public:
        /** @summary Try to reserve the actual scope. @param scope Exact retained scope. */
        explicit LifecycleGuard(RuntimeScope& scope) noexcept;
        /** @summary Reports nonblocking reservation success. @return Whether the scope mutex is held. */
        bool Acquired() const noexcept { return lock_.owns_lock(); }
        /** @summary Reuse the sole grant evaluator for a current or uncommitted candidate owner.
         * @param context Exact original reservation. @param candidate Require uncommitted candidate state.
         * @return Current authority without issuing or copying any grant. */
        ScopeResult Check(const OperationContext& context, bool candidate) const;
        /** @summary Confirms exact scope/domain identity before any marker.
         * @param domain Original writer domain. @return Whether this guard belongs to it. */
        bool InDomain(const RuntimeCommitDomain& domain) const noexcept;
        /** @summary Copies factual scalars while the actual try-only Q reservation remains held.
         * @return Current scope snapshot, or a closed empty snapshot when not acquired. */
        ScopeSnapshot Snapshot() const noexcept;
        /** @summary Retire epochs, grants and queued operations without releasing retained buffers. */
        void Retire() noexcept;
        /** @summary Expose only the candidate that passed Check under this same guard. */
        void Activate() noexcept;
    private:
        RuntimeScope& scope_;
        std::unique_lock<std::mutex> lock_;
    };
    /** @summary Returns the retained native domain; no authority is issued. */
    RuntimeCommitDomain& stateDomain() noexcept;
    /** @summary Publishes a typed prepared registry plan on the original task under actual context reservation.
     * @param operation Original value task. @param writer Actual shared domain reservation.
     * @param publication Guard validated before the scope fence. @param plan Retained prepared plan.
     * @param scopeGeneration Captured original scope identity. @param runtimeEpoch Captured original epoch.
     * @param input Original copied request, checked against the retained task input.
     * @return Precommit rejection or immutable registry-only outcome. */
    ScopeResult CommitPreparedValue(ScopeHandle operation, RuntimeCommitDomain::Writer& writer,
        mods::PatchRegistry::PublicationGuard& publication, mods::PatchRegistry::PreparedPublication& plan,
        std::uint64_t scopeGeneration, std::uint64_t runtimeEpoch, const copied::Envelope& input);
    /** @summary Drain exact native command release requests only outside compound publication guards.
     * @param state Original Q-locked state. */
    static void DrainCommandReleasesLocked(State& state) noexcept;
    /** @summary Ack only the exact tracked nonroot reservation under Q. @param state Original state.
     * @param slot Original ticket record. @param serial Exact incarnation. @return Acknowledgement or invariant failure. */
    static ScopeResult AcknowledgeCommandLocked(State& state,std::size_t slot,std::uint64_t serial) noexcept;
    /** @summary Reuse scalar stop/cancellation without destroying payloads. @param state Original Q-held state. @param reason Native reason. */
    static void RetireLocked(State& state,StopReason reason) noexcept;
    /** @summary Shares the one scalar snapshot implementation between legacy and typed guarded readers.
     * @param state Actual state whose mutex is held. @return Copied native facts without authority. */
    static ScopeSnapshot SnapshotLocked(const State& state) noexcept;
    std::shared_ptr<State> state_;
    /** @summary Validates authoritative state while its mutex is held.
     * @param state Current owned state. @param context Original host-issued context.
     * @param checkRevision Commit/submit require exact revision; result polling does not replay writers.
     * @param checkGrantRevision False only when issuing a fresh fixture reservation from still-valid prior provenance.
     * @param checkCapability False only for non-authorizing effective-capability inspection.
     * @param candidate True only for private lifecycle evaluation of an uncommitted native candidate.
     * @return Current admission outcome. */
    static ScopeResult Validate(const State& state, const OperationContext& context, bool checkRevision,
        bool checkGrantRevision = true, bool checkCapability = true, bool candidate = false);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct RuntimeScopeTestAccess;
#endif
};

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Synthetic authority exists only in separately compiled test targets, never runtime/Core. */
struct RuntimeScopeTestAccess final {
    /** @summary Negative allocation cuts exist only in fixture builds, never as authority setters. */
    enum class AllocationCut { None, Challenge, RootReservation, Grant, FreshReservation, JoinedReservation, Disconnect };
    /** @summary Scalar accounting from the original Q-held store; contains no source/proof or payload. */
    struct AuthorityObservation final {
        std::size_t challenges{},grants{},reservations{};
        std::uint64_t nextChallenge{},nextGrant{},nextReservation{};
        bool source{},admissionClosed{};
    };
    /** @summary Fail the next matching original allocation on this fixture thread once.
     * @param cut Exact negative cut; None clears a previously armed cut. */
    static void FailAllocation(AllocationCut cut) noexcept;
    /** @summary Read original source/store accounting without acquiring any owner.
     * @param scope Original fixture or source-absent production-constructed scope. @return Copied scalars only. */
    static AuthorityObservation ObserveAuthority(const RuntimeScope& scope);
    /** @summary Seed only a retained nonterminal fixture task near sequence exhaustion, without authority changes.
     * @param scope Actual isolated scope. @param task Original value-task handle.
     * @param sequence UINT64_MAX-2 or UINT64_MAX-1, retaining at least one terminal publication.
     * @return False for wrong kind/owner/phase, committed/reserved terminal or impossible exhausted input. */
    static bool SeedTaskSequence(RuntimeScope& scope,ScopeHandle task,std::uint64_t sequence);
    friend class RuntimeScope;
    /** @summary Try-reserve the actual fixture scope mutex for bounded contention/reclamation checks.
     * @param scope Exact fixture scope. @return Nonblocking native lock; never grants authority. */
    static std::unique_lock<std::mutex> ReserveState(RuntimeScope& scope);
    /** @summary Exact reviewed fixture request; never exported or available to production ingress. */
    using Review=RuntimeAuthoritySource::Review;
    /** @summary Non-authoritative bounded pending challenge identity. */
    using Challenge=RuntimeAuthoritySource::Challenge;
    /** @summary Truthful effective state, independent of feature/transport availability. */
    using PermissionView=RuntimeAuthoritySource::PermissionView;
    /** @summary Creates one expiring fixture-only pending human-action challenge. @param scope Native fixture. @param review Exact prompt. @return Zero on denial/capacity. */
    static Challenge ChallengeDecision(RuntimeScope& scope, const Review& review);
    /** @summary Consumes an exact fixture human-action proof once into the actual grant store. @param scope Native fixture. @param challenge Original prompt. @param review Unchanged reviewed request. @return Reservation or closed context. */
    static OperationContext Decide(RuntimeScope& scope, Challenge challenge, const Review& review);
    /** @summary Creates a new operation reservation without renewing or rewriting the original decision/task. @param scope Native fixture. @param context Existing grant reservation. @return Fresh domain revision bounded by original consent. */
    static OperationContext Reserve(RuntimeScope& scope, const OperationContext& context);
    /** @summary Try-only variant of the existing fixture reservation producer, outside publication guards.
     * @param scope Original fixture scope. @param context Existing reviewed provenance. @return Fresh reservation or empty on contention. */
    static OperationContext TryReserve(RuntimeScope& scope,const OperationContext& context);
    /** @summary Authenticates a fixture participant in an explicitly reviewed all-participants session. @param scope Fixture. @param context Governing grant. @param actor Joining actor. @param channel Actual channel. @return Separate bound participant reservation. */
    static OperationContext Join(RuntimeScope& scope, const OperationContext& context, std::uint64_t actor, std::uint64_t channel);
    /** @summary Closes consent across every matching agent grant while preserving reviewed entity fallback.
     * @param scope Fixture. @param consentSession Actual consent-session identity, never a grant identity.
     * @return Ok after serialized closure, Stale when absent or Busy without a change. */
    static ScopeResult RevokeSession(RuntimeScope& scope, std::uint64_t consentSession);
    /** @summary Closes one authenticated channel without replacement-session inheritance.
     * @param scope Fixture. @param channel Actual channel. @return Ok after closure, Invalid for zero or Busy without effects. */
    static ScopeResult Disconnect(RuntimeScope& scope, std::uint64_t channel);
    /** @summary Inspects current effective capabilities without issuing permission. @param scope Fixture. @param context Exact reservation. @return Cleared view on denial. */
    static PermissionView Inspect(RuntimeScope& scope, const OperationContext& context);
    /** @summary Inspect fixture cleanup without reopening a stopped generation. @param scope Fixture owner. @return Retained continuation slots. */
    static std::size_t Registrations(const RuntimeScope& scope);
    /** @summary Observe actual retained reservation capacity for isolated cleanup probes. @param scope Original fixture scope. @return Count. */
    static std::size_t Reservations(const RuntimeScope& scope);
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
    /** @summary Invalidates an exact grant and advances its effective revision without altering retained task outcomes.
     * @param scope Test-only scope. @param grantIdentity Exact grant identity, never an agent consent-session ID.
     * @return Ok after serialized revocation, Stale when absent or Busy without effects. */
    static ScopeResult Revoke(RuntimeScope& scope, std::uint64_t grantIdentity);
private:
    /** @summary Share the sole fixture reservation implementation while the caller owns Q outside compound fences.
     * @param scope Original fixture. @param context Existing reviewed provenance. @return Fresh exact bounded reservation. */
    static OperationContext ReserveLocked(RuntimeScope& scope,const OperationContext& context);
};
/** @summary Fixture-only exact retirement producer/observations; absent from production/Core. */
struct ScopeRetirementTestAccess final {
    using Ticket=RuntimeScope::CommandTicket;
    using Observation=RuntimeScope::RetirementObservation;
    /** @summary Coupled fresh context and exact release handle. */
    struct Reservation final {OperationContext context;Ticket ticket;};
    /** @summary Reserve one exact fresh ticket under actual Q. @param scope Original scope. @param context Existing reviewed provenance. @return Empty on denial. */
    static Reservation Reserve(RuntimeScope& scope,const OperationContext& context);
    /** @summary Release only the original native ticket. @param scope Original scope. @param ticket Original handle. @return Actual acknowledgement/transfer state. */
    static ScopeResult Release(RuntimeScope& scope,const Ticket& ticket);
    /** @summary Observe original scope without arbitrary destruction. @param scope Original scope. @return Current partial certificate. */
    static Observation Observe(RuntimeScope& scope);
    /** @summary Explicit disposal action outside lane/publication ownership. @param scope Original scope. @return Actual action outcome. */
    static ScopeResult DrainOne(RuntimeScope& scope);
    /** @summary Enroll a fixture-native manager-independent resource in the existing 64-holder budget.
     * @param scope Original scope. @param resource Trusted fixture object. @return Whether retained before stop. */
    static bool RetainSafe(RuntimeScope& scope,std::shared_ptr<void> resource);
    /** @summary Install deterministic actual reserve/mailbox transfer cuts, outside Q.
     * @param reserved After atomic successful reserve. @param writing After actual mailbox claim and before Ready publication. */
    static void Barriers(std::function<void()> reserved,std::function<void()> writing);
};
#endif
}
