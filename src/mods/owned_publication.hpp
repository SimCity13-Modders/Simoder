#pragma once
#include "mods/owner_participant.hpp"
#include "mods/manager_preparation.hpp"
#include "scripting/semantic_bridge.hpp"
#include "scripting/script_runtime.hpp"
#include "scripting/scheduler_publication.hpp"

namespace sc13::mods {
class ManagerCache;
class ManagerCacheAction;
class ManagerCommands;
class ManagerCommandRoute;
struct ManagerCommandTicket;
class LifecycleDecisionLease;
/** @summary Immutable original lifecycle reservation view; replacement/disposal is an off-guard native operation, never a grant issuer. */
class LifecycleDecision final {
    friend class OwnerLifecycle;
    friend class ManagerPublication;
    friend struct LifecycleDecisionTestAccess;
public:
    /** @summary Constructs the explicit absent-side decision. */
    LifecycleDecision()=default;
    /** @summary Copies immutable native facts without exposing reservation ownership. @return Original context or empty absent view. */
    runtime::OperationContext Context() const noexcept{return context_;}
private:
    std::shared_ptr<runtime::RuntimeScope> scope_;
    runtime::OperationContext context_;
    std::shared_ptr<LifecycleDecisionLease> lease_;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Closed original decision producer; never compiles into a production permission issuer. */
struct LifecycleDecisionTestAccess final {
    /** @summary Wraps exact existing root without creating or owning a fresh reservation.
     * @param scope Original scope. @param context Reviewed root. @param output Empty native output. @return Exact admission. */
    static runtime::ScopeResult RootDecision(const std::shared_ptr<runtime::RuntimeScope>& scope,const runtime::OperationContext& context,LifecycleDecision& output);
    /** @summary Couples original fresh reservation and ticket under the same Q before exposure.
     * @param scope Original scope. @param context Reviewed original source. @param output Empty output. @return Exact native result. */
    static runtime::ScopeResult FreshDecision(const std::shared_ptr<runtime::RuntimeScope>& scope,const runtime::OperationContext& context,LifecycleDecision& output);
    /** @summary Reports actual fixed native lease storage, excluding existing shared ticket pool. @return Native bytes. */
    static std::size_t LeaseBytes() noexcept;
};
#endif
/** @summary Native incarnation pinned to the actual loaded VM; never a permission issuer. */
class PublicationOrigin final {
public:
    /** @summary Pure startup preparation and operational admission have distinct phases. */
    enum class Phase { Candidate, Active, Retired };
private:
    friend class ManagerCommands;
    friend struct ManagerCommandTicket;
    friend class OwnedPublication;
    friend class OwnerLifecycle;
    friend class ManagerPublication;
    friend class ManagerClose;
    friend class scripting::SemanticBridge;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct OwnedPublicationTestAccess;
    friend struct OwnerLifecycleTestAccess;
    friend struct ManagerCommandsTestAccess;
#endif
    /** @summary Capture actual immutable runtime inputs; construction has no production caller.
     * @param runtime Actual retained VM. @param endpoint VM's original endpoint.
     * @param fingerprint Native source correlation, not a durable consent identity. */
    PublicationOrigin(std::shared_ptr<scripting::ScriptRuntime> runtime,
        std::shared_ptr<scripting::SemanticEndpoint> endpoint, std::string fingerprint);
    /** @summary Revalidate retained loaded VM identity without interpreting text IDs as authority.
     * @return Whether actual VM, endpoint, scope and immutable definition still match. */
    bool Current() const noexcept;
    /** @summary Proof precedes VM ownership so VM destruction occurs while exact Base/capture remain alive. */
    std::shared_ptr<ManagerPreparation::Plan> preparation_;
    std::shared_ptr<scripting::ScriptRuntime> runtime_;
    std::shared_ptr<scripting::SemanticEndpoint> endpoint_;
    std::shared_ptr<runtime::RuntimeScope> scope_;
    std::shared_ptr<const scripting::ScriptDefinition> definition_;
    std::string owner_, fingerprint_;
    /** @summary Separate retained identity prevents same-ID VMs from replacing another incarnation's registry set. */
    std::shared_ptr<const void> incarnation_;
    /** @summary Copied native record baseline; no caller-controlled foreign owner is represented. */
    std::vector<ResourcePatchDefinition> declarative_;
    std::shared_ptr<const std::vector<ResourcePatchDefinition>> scripted_;
    std::uint64_t generation_{};
    Phase phase_{Phase::Candidate};
    std::shared_ptr<const void> jointBinding_;
};

/** @summary One closed lifecycle algorithm for fixture and actual canonical records; production issuance remains absent. */
class OwnerLifecycle final {
    friend class ManagerCache;
    friend class ManagerCommands;
    friend class ManagerCommandRoute;
    friend struct ManagerCommandTicket;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerCommandsTestAccess;
#endif
    /** @summary Shares the canonical entry stamp representation without attaching a second current record. */
    using Stamp = OwnerParticipant::Stamp;
    /** @summary Retains actual registry/domain lifetimes instead of reusable addresses. */
    struct Record final : OwnerParticipant {
        std::shared_ptr<PatchRegistry> registry;
        std::shared_ptr<runtime::RuntimeCommitDomain> domain;
        ModId owner;
    };
    /** @summary Closed adapter retains either the fixture slot or actual map anchor, never a second current record. */
    struct Binding final {
        std::shared_ptr<Record> fixture;
        std::shared_ptr<ModManager::State> state;
        std::shared_ptr<const void> identity;
        std::shared_ptr<const InstallationBinding> installation;
        ModDefinitionFingerprint fingerprint;
        std::shared_ptr<runtime::RuntimeCommitDomain> domain;
        PatchRegistry* registry{};
        ModId owner;
        /** @summary Captured before compound reservations, compared without allocation under actual M. */
        std::vector<ModId> expectedOrder;
    };
    /** @summary Per-call canonical reservation; lifetime lease precedes original D then actual try-M. */
    struct Access final {
        /** @summary Resolve exact membership under original locks. @param binding Closed native participant adapter. */
        explicit Access(const std::shared_ptr<Binding>& binding);
        std::optional<ModManager::Lease> lease;
        runtime::RuntimeCommitDomain::Writer writer;
        std::unique_lock<std::mutex> manager;
        OwnerParticipant* participant{};
        ModManager::Record* actual{};
        runtime::ScopeResult result{runtime::ScopeResult::Invalid};
    };
    /** @summary Private purpose prevents release-only reservations from becoming general writes. */
    enum class Purpose { Activation, Removal, Handoff };
    /** @summary Original canonical preparation snapshot, retained locally through promotion without an active-proof ownership cycle. */
    struct PreparationSnapshot final {
        std::shared_ptr<const void> stamp;
        std::uint64_t domainRevision{};
    };
public:
    /** @summary Shared coherent scalar facts for actual canonical and fixture participants; never authority. */
    struct Observation final {
        std::shared_ptr<PublicationOrigin> origin;
        std::shared_ptr<scripting::ScriptRuntime> runtime;
        std::shared_ptr<const PatchRegistrySnapshot> registry;
        runtime::ScopeSnapshot scope;
        std::uint64_t domainRevision{},recordRevision{};
        std::size_t orderSize{};
        bool running{},projectionMatches{},ownerInOrder{},orderMatches{};
    };
    /** @summary Retains prepared and retired storage beyond every writer reservation. */
    class Plan final {
    public:
        /** @summary Construct an empty non-authoritative holder. */
        Plan() = default;
        /** @summary Transfer retained storage outside all fences. @param other Sole prior holder. */
        Plan(Plan&& other) noexcept = default;
        /** @summary Replace retained storage outside all fences. @param other Sole prior holder. @return This holder. */
        Plan& operator=(Plan&& other) noexcept;
        Plan(const Plan&) = delete;
        Plan& operator=(const Plan&) = delete;
    private:
        friend class OwnerLifecycle;
        friend class ManagerPublication;
        friend class ManagerClose;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        friend struct ManagerPublicationTestAccess;
#endif
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        friend struct OwnerLifecycleTestAccess;
#endif
        /** @summary Decision owners precede every record/stamp/origin/VM owner so final release follows all VM disposal. */
        std::shared_ptr<LifecycleDecisionLease> oldDecisionOwner_,newDecisionOwner_;
        std::shared_ptr<Record> record_;
        std::shared_ptr<Binding> binding_;
        std::shared_ptr<Stamp> expected_, next_, failed_;
        std::shared_ptr<PublicationOrigin> old_, successor_;
        PatchRegistry::PreparedPublication publication_;
        runtime::OperationContext oldDecision_, newDecision_;
        bool typedDecisions_{};
        scripting::SemanticContextLease oldContext_, newContext_;
        std::uint64_t domainRevision_{};
        Purpose purpose_{Purpose::Removal};
        bool absent_{}, consumed_{};
        scripting::SchedulerPublicationPlan scheduler_;
        bool joint_{}, schedulerPrepared_{};
        /** @summary All projection storage is prepared outside fences and swapped at the same marker. */
        std::vector<ModId> expectedOrder_, nextOrder_, desiredOrder_;
        std::optional<ModDefinition> projectionDefinition_;
        std::vector<ResourcePatchDefinition> projectionPatches_;
        std::shared_ptr<scripting::ScriptRuntime> projectionVm_;
        std::shared_ptr<runtime::RuntimeScope> projectionScope_;
        std::string projectionDiagnostic_;
    };
private:
    friend class ManagerPublication;
    friend class ManagerClose;
    /** @summary Scalar memory disposition written only by the existing shared marker algorithm. */
    enum class MemoryDisposition {NotAttempted,RejectedBeforeMarker,NoChangeConsumed,MarkerCommitted,MarkerIndeterminate};
    /** @summary Original marker evidence; no pointer, authority or inferred rollback. */
    struct CommitEvidence final {MemoryDisposition memory{MemoryDisposition::RejectedBeforeMarker};std::uint64_t revision{};bool crossed{};};
    static_assert(sizeof(CommitEvidence)<=512);
    /** @summary Checks immutable original decision scope/context/lease binding, including explicit absent side.
     * @param decision Native view. @param scope Exact origin scope or null absent side. @param context Expected immutable facts.
     * @return Whether this original lineage is exact. */
    static bool DecisionMatches(const LifecycleDecision& decision,const std::shared_ptr<runtime::RuntimeScope>& scope,
        const runtime::OperationContext& context) noexcept;
    /** @summary Attaches exact original decision owners only after all preparation guards unwind.
     * @param plan Genuine partial or complete output. @param old Original predecessor view. @param next Original successor view.
     * @return Exact matching ownership or conflict, never posthoc raw adoption. */
    static runtime::ScopeResult AttachDecisions(Plan& plan,const LifecycleDecision& old,const LifecycleDecision& next) noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerPublicationTestAccess;
#endif
    /** @summary Read actual or fixture participants under the same ordered scalar observation guards.
     * @param record Exact native binding. @param scheduler Original scheduler. @param output Cleared coherent facts.
     * @param view Optional retained order witness. @return Coherent result or bounded rejection. */
    static runtime::ScopeResult ObserveBound(const std::shared_ptr<Binding>& record,scripting::ScriptScheduler& scheduler,Observation& output,
        const Plan* view = nullptr);
    /** @summary Adapt the retained fixture without copying its participant. @param record Exact fixture. @return Closed binding. */
    static std::shared_ptr<Binding> Bind(const std::shared_ptr<Record>& record);
    /** @summary Shared preparation core for actual and fixture participants; no permission issuance.
     * @param record Original binding. @param old Exact predecessor. @param successor Exact candidate.
     * @param oldDecision Current reviewed reservation. @param newDecision Candidate reviewed reservation.
     * @param oldContext Current factual lease. @param newContext Candidate factual lease.
     * @param output Empty retained owner. @param error Diagnostic. @param original Mandatory original pair for a canonical successor; empty only for detached fixtures or release-only removal. @return Preparation result. */
    static runtime::ScopeResult PrepareBound(const std::shared_ptr<Binding>& record,
        const std::shared_ptr<PublicationOrigin>& old,const std::shared_ptr<PublicationOrigin>& successor,
        const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
        const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
        Plan& output,std::string& error,const PreparationSnapshot& original);
    /** @summary Shared scheduler binding core; actual adapter participates in the same metadata fence.
     * @param scheduler Actual scheduler. @param record Original binding. @param old Exact predecessor.
     * @param successor Exact candidate. @param oldDecision Current reviewed reservation.
     * @param newDecision Candidate reviewed reservation. @param oldContext Current factual lease.
     * @param newContext Candidate factual lease. @param output Empty/retry retained preparation.
     * @param error Diagnostic. @param original Mandatory unchanged original pair for every canonical successor; empty only for detached fixtures or release-only removal. @return Joint binding outcome without activation. */
    static runtime::ScopeResult PrepareJointBound(scripting::ScriptScheduler& scheduler,const std::shared_ptr<Binding>& record,
        const std::shared_ptr<PublicationOrigin>& old,const std::shared_ptr<PublicationOrigin>& successor,
        const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
        const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
        Plan& output,std::string& error,const PreparationSnapshot& original);
    /** @summary Sole publication algorithm for fixture and actual participant adapters.
     * @param record Original adapter. @param plan Its exact preparation. @param joint Require scheduler participation.
     * @param staged Original already-prepared action, or null. @param evidence Optional scalar result initialized before every early return.
     * @return Atomic result or pre-marker denial. */
    static runtime::ScopeResult CommitBound(const std::shared_ptr<Binding>& record,Plan& plan,bool joint,ManagerCacheAction* staged=nullptr,CommitEvidence* evidence=nullptr);
    /** @summary Single guarded lifecycle algorithm with an already admitted original cache action. @param record Original binding. @param plan Genuine plan. @param joint Exact scheduler participation. @param action Original staged cache action or model-only null. @param evidence Optional original scalar marker output. @return Actual publication disposition. */
    static runtime::ScopeResult CommitReserved(const std::shared_ptr<Binding>& record,Plan& plan,bool joint,ManagerCacheAction* action,CommitEvidence* evidence=nullptr);
    /** @summary Validate captured native Base and exact preparation under already-held V/B; never locks recursively.
     * @param origin Exact consumed preparation owner. @param expected Private candidate or exact active phase.
     * @return Current native witness, never authority. */
    static runtime::ScopeResult ValidatePreparationLocked(const std::shared_ptr<PublicationOrigin>& origin,
        PublicationOrigin::Phase expected=PublicationOrigin::Phase::Candidate) noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct OwnerLifecycleTestAccess;
#endif
    /** @summary Capture a closed model record from retained native objects only.
     * @param origin Exact active VM. @param registry Exact registry. @param domain Original scope domain.
     * @return Bound record or null on contention/identity mismatch. */
    static std::shared_ptr<Record> Issue(const std::shared_ptr<PublicationOrigin>& origin,
        std::shared_ptr<PatchRegistry> registry, std::shared_ptr<runtime::RuntimeCommitDomain> domain);
    /** @summary Coherent model-only record read under its original domain.
     * @param record Exact native slot. @param output Current owner. @param revision Stamp revision.
     * @return Coherent read or closed/busy outcome. */
    static runtime::ScopeResult Inspect(const std::shared_ptr<Record>& record,
        std::shared_ptr<PublicationOrigin>& output, std::uint64_t& revision);
    /** @summary Capture a record-issued purpose-bound candidate without operational admission.
     * @param record Exact native model slot. @param old Exact predecessor.
     * @param successor Candidate for handoff, null for removal. @param oldDecision Current predecessor reservation.
     * @param newDecision Current candidate decision. @param oldContext Captured predecessor facts.
     * @param newContext Captured successor facts. @param output Empty retained holder. @param error Diagnostic.
     * @return Preparation result without effects. */
    static runtime::ScopeResult Prepare(const std::shared_ptr<Record>& record,
        const std::shared_ptr<PublicationOrigin>& old, const std::shared_ptr<PublicationOrigin>& successor,
        const runtime::OperationContext& oldDecision, const runtime::OperationContext& newDecision,
        const scripting::SemanticContextLease& oldContext, const scripting::SemanticContextLease& newContext,
        Plan& output, std::string& error);
    /** @summary Commit once on exact native identity with no caller callbacks or grant issuance.
     * @param record Exact issuing slot. @param plan Its retained candidate. @return Registry/model-only outcome. */
    static runtime::ScopeResult Commit(const std::shared_ptr<Record>& record, Plan& plan);
    /** @summary Captures an exact empty fixture slot for initial joint activation.
     * @param candidate Original uncommitted incarnation. @param registry Actual retained registry.
     * @param domain Original scope domain. @return Empty native slot or closed rejection. */
    static std::shared_ptr<Record> IssueCandidate(const std::shared_ptr<PublicationOrigin>& candidate,
        std::shared_ptr<PatchRegistry> registry, std::shared_ptr<runtime::RuntimeCommitDomain> domain);
    /** @summary Prepares and binds the actual scheduler and model record without activation.
     * @param scheduler Actual enrolled scheduler. @param record Exact model slot. @param old Current origin or null initially.
     * @param successor Candidate or null for removal. @param oldDecision Existing old authority reservation.
     * @param newDecision Existing candidate reservation. @param oldContext Original factual lease.
     * @param newContext Candidate factual lease. @param output Empty retained plan. @param error Diagnostic.
     * @return Prepared joint result; no grants or runtime integration. */
    static runtime::ScopeResult PrepareJoint(scripting::ScriptScheduler& scheduler, const std::shared_ptr<Record>& record,
        const std::shared_ptr<PublicationOrigin>& old, const std::shared_ptr<PublicationOrigin>& successor,
        const runtime::OperationContext& oldDecision, const runtime::OperationContext& newDecision,
        const scripting::SemanticContextLease& oldContext, const scripting::SemanticContextLease& newContext,
        Plan& output, std::string& error);
    /** @summary Runs the shared one-marker lifecycle algorithm with optional scheduler participants.
     * @param record Exact slot. @param plan One-shot preparation. @param joint Select exact joint-only route.
     * @return Atomic publication outcome. */
    static runtime::ScopeResult CommitPrepared(const std::shared_ptr<Record>& record, Plan& plan, bool joint);
};

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Isolated native record producer; entirely absent from production Core. */
struct OwnerLifecycleTestAccess final {
    /** @summary Opaque fixture record lifetime; cannot be constructed by mod or MCP input. */
    using Record = std::shared_ptr<OwnerLifecycle::Record>;
    /** @summary Installs a thread-local bounded fixture interleaving only outside every publication guard.
     * @param barrier Test synchronization after factual preparation and before scheduler ownership capture. */
    static void PreparationBarrier(std::function<void()> barrier);
    /** @summary Issues only an empty fixture record for exact initial candidate.
     * @param candidate Actual uncommitted origin. @param registry Exact registry. @param domain Original domain.
     * @return Retained slot without authority or activation. */
    static Record IssueCandidate(const std::shared_ptr<PublicationOrigin>& candidate,
        std::shared_ptr<PatchRegistry> registry, std::shared_ptr<runtime::RuntimeCommitDomain> domain);
    /** @summary Prepares initial activation, handoff or removal through shared native participants.
     * @param scheduler Actual fixture scheduler. @param record Exact native slot. @param old Existing owner or null.
     * @param successor Candidate or null for removal. @param oldDecision Existing reservation.
     * @param newDecision Candidate reservation. @param oldContext Original facts. @param newContext Candidate facts.
     * @param output Empty plan. @param error Diagnostic. @return Preparation status. */
    static runtime::ScopeResult Joint(scripting::ScriptScheduler& scheduler,const Record& record,
        const std::shared_ptr<PublicationOrigin>& old,const std::shared_ptr<PublicationOrigin>& successor,
        const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
        const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
        OwnerLifecycle::Plan& output,std::string& error);
    /** @summary Consumes exactly one joint preparation through the single original marker.
     * @param record Exact native slot. @param plan Retained joint preparation. @return Publication status. */
    static runtime::ScopeResult CommitJoint(const Record& record,OwnerLifecycle::Plan& plan);
    /** @summary Reclaims only actual retired or successfully prepared aborted VMs off-lane after all guards.
     * @param plan Retained joint preparation. @return Full close/slot-release status, or Ok without touching a rejected unowned VM. */
    static runtime::ScopeResult Reclaim(OwnerLifecycle::Plan& plan);
    /** @summary Copies actual participant state while one original D prevents publication interleaving. */
    using JointObservation=OwnerLifecycle::Observation;
    /** @summary Observes actual participants, never a shadow model or authorization.
     * @param record Exact slot. @param scheduler Original scheduler. @param output Cleared copied facts.
     * @return Coherent read or bounded contention/conflict. */
    static runtime::ScopeResult Observe(const Record& record,scripting::ScriptScheduler& scheduler,JointObservation& output);
    /** @summary Capture an existing exact active origin and its live domain/registry.
     * @param origin Actual isolated VM. @param registry Retained exact registry. @param domain Original domain.
     * @return Record, or null if actual identities do not match. */
    static Record Issue(const std::shared_ptr<PublicationOrigin>& origin,
        std::shared_ptr<PatchRegistry> registry, std::shared_ptr<runtime::RuntimeCommitDomain> domain);
    /** @summary Prepare exact release-only cleanup, including an idempotent absent tombstone.
     * @param record Native slot. @param old Exact old incarnation. @param output Empty output.
     * @param error Diagnostic. @return Preparation outcome. */
    static runtime::ScopeResult Remove(const Record& record, const std::shared_ptr<PublicationOrigin>& old,
        OwnerLifecycle::Plan& output, std::string& error);
    /** @summary Prepare the complete candidate with reviewed existing scope reservations and factual leases.
     * @param record Native slot. @param old Exact current incarnation. @param successor Exact candidate.
     * @param oldDecision Current old reservation. @param newDecision Current candidate reservation.
     * @param oldContext Exact old factual lease. @param newContext Exact candidate factual lease.
     * @param output Empty output. @param error Diagnostic. @return Preparation outcome. */
    static runtime::ScopeResult Handoff(const Record& record, const std::shared_ptr<PublicationOrigin>& old,
        const std::shared_ptr<PublicationOrigin>& successor, const runtime::OperationContext& oldDecision,
        const runtime::OperationContext& newDecision, const scripting::SemanticContextLease& oldContext,
        const scripting::SemanticContextLease& newContext, OwnerLifecycle::Plan& output, std::string& error);
    /** @summary Consume exactly one purpose-bound plan. @param record Exact issuing slot.
     * @param plan Retained candidate. @return Model-only outcome, never live-game success. */
    static runtime::ScopeResult Commit(const Record& record, OwnerLifecycle::Plan& plan);
    /** @summary Read coherently under the original domain. @param record Native slot.
     * @param output Cleared first; exact active origin on success. @param revision Receives record stamp.
     * @return Busy or coherent current slot; null current denotes a removal tombstone. */
    static runtime::ScopeResult Inspect(const Record& record, std::shared_ptr<PublicationOrigin>& output,
        std::uint64_t& revision);
};
#endif
/** @summary Sole owner of an internally prepared plan and exact originating request. No raw-plan sealing API exists. */
class OwnedPublication final {
public:
    /** @summary Creates an empty non-authoritative holder. */
    OwnedPublication() = default;
    /** @summary Transfer prepared and retired storage without publication. @param other Sole previous holder. */
    OwnedPublication(OwnedPublication&& other) noexcept = default;
    /** @summary Replace storage only outside reservations. @param other Sole previous holder. @return This holder. */
    OwnedPublication& operator=(OwnedPublication&& other) noexcept = default;
    OwnedPublication(const OwnedPublication&) = delete;
    OwnedPublication& operator=(const OwnedPublication&) = delete;
private:
    friend class ManagerCommands;
    /** @summary Shared non-authorizing reducer and registry preparation used by native producers.
     * @param origin Exact native incarnation. @param registry Registry owner. @param baseline Original immutable scripted set.
     * @param commands Bounded owned output. @param next Receives reduced set. @param plan Receives complete registry candidate.
     * @param error Diagnostic. @param busy Optional try-only active replacement, reporting writer contention.
     * @return Whether staging succeeded; no authority is issued. */
    static bool PreparePatches(const PublicationOrigin& origin,PatchRegistry& registry,
        const std::shared_ptr<const std::vector<ResourcePatchDefinition>>& baseline,
        const std::vector<scripting::ScriptPatchCommand>& commands,
        std::shared_ptr<const std::vector<ResourcePatchDefinition>>& next,PatchRegistry::PreparedPublication& plan,std::string& error,bool* busy=nullptr);
    /** @summary Compare complete scripted state, including metadata and exact float bits, without allocating.
     * @param left Original reduced baseline. @param right Next reduced baseline. @return Exact equality. */
    static bool SamePatches(const std::vector<ResourcePatchDefinition>& left,const std::vector<ResourcePatchDefinition>& right) noexcept;
    friend class scripting::SemanticBridge;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct OwnedPublicationTestAccess;
#endif
    /** @summary Validates exact native identity under the already reserved original domain.
     * @param invocation Original task request. @return Current owner/request admission, never a permission grant. */
    runtime::ScopeResult Validate(const scripting::SemanticInvocation& invocation) const noexcept;
    std::shared_ptr<PublicationOrigin> origin_;
    scripting::SemanticInvocation invocation_;
    std::string wire_;
    PatchRegistry::PreparedPublication plan_;
    std::shared_ptr<const std::vector<ResourcePatchDefinition>> baseline_, next_;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Isolated trusted native producer; compiled out of production Core. */
struct OwnedPublicationTestAccess final {
    /** @summary Returns a copied loaded snapshot for immutability checks in the isolated host only.
     * @param origin Actual native incarnation. @return Independent snapshot copy. */
    static scripting::ScriptDefinition Snapshot(const PublicationOrigin& origin);
    /** @summary Captures an actual VM and its native endpoint; fingerprint is correlation, not a security digest.
     * @param runtime Actual VM. @param endpoint Its actual binding. @param fingerprint Source correlation.
     * @param declarative Immutable native declarative baseline. @param scripted Initial native scripted baseline.
     * @return Candidate incarnation without grants or operational readiness. */
    static std::shared_ptr<PublicationOrigin> Issue(std::shared_ptr<scripting::ScriptRuntime> runtime,
        std::shared_ptr<scripting::SemanticEndpoint> endpoint, std::string fingerprint,
        std::vector<ResourcePatchDefinition> declarative = {}, std::vector<ResourcePatchDefinition> scripted = {});
    /** @summary Changes phase under the same publication domain; retirement is irreversible.
     * @param origin Original incarnation. @param phase Requested lifecycle phase.
     * @return False for contention, premature activation or retired incarnation. */
    static bool Transition(PublicationOrigin& origin, PublicationOrigin::Phase phase);
    /** @summary Reduces only this origin's bounded commands and internally prepares its exact registry plan.
     * @param origin Native issuer. @param registry Target registry. @param invocation Original retained request.
     * @param commands Trusted synthetic producer output, not a qualified public payload decoder.
     * @param output Empty output holder. @param error Failure diagnostic. @return Whether preparation succeeded. */
    static bool Prepare(const std::shared_ptr<PublicationOrigin>& origin, PatchRegistry& registry,
        const scripting::SemanticInvocation& invocation, const std::vector<scripting::ScriptPatchCommand>& commands,
        OwnedPublication& output, std::string& error);
};
#endif
}
