#pragma once
#include "mods/owned_publication.hpp"
#include "mods/state_path_writer.hpp"

namespace sc13::mods {
/** @summary Closed canonical coordinator; production has no issuer or enrollment route. */
class ManagerPublication final {
public:
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    /** @summary Nonowning fixture identities from actual canonical reservations; no token or writable native object escapes. */
    struct Lineage final {
        std::weak_ptr<const void> state,participant,installation,joint,node,incarnation,stamp;
        std::uint64_t domainRevision{},recordRevision{},registration{},generation{};
        bool active{};
    };
#endif
    /** @summary One-shot actual-manager transaction and retained off-fence retirement storage. */
    class Plan final {
        friend class ManagerPublication;
        friend class ManagerClose;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        friend struct ManagerPublicationTestAccess;
#endif
        OwnerLifecycle::Plan lifecycle_;
    };
private:
    /** @summary Native fixed memory evidence; not an activation certificate. */
    using MemoryDisposition=OwnerLifecycle::MemoryDisposition;
    /** @summary File protocol history/current evidence and original one-shot memory result, without any runtime owner. */
    struct FileObservation final {
        StatePathWriter::Result disk;
        MemoryDisposition memory{MemoryDisposition::NotAttempted};
        runtime::ScopeResult memoryResult{runtime::ScopeResult::Invalid};
        std::uint64_t revision{};std::uint32_t memoryAttempts{};
        bool admitted{},crossed{},compensationRequested{},settlementPending{},finalized{};
    };
    /** @summary File-only continuation; native caller owns it exclusively and cannot replace retained pending ownership. */
    class FileOutcome final {
        friend class ManagerPublication;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        friend struct ManagerPublicationTestAccess;
#endif
        StatePathWriter::Owner owner_;
        StatePathWriter::Intent intent_;
        StatePathWriter::Receipt receipt_;
        FileObservation observation_;
    public:
        /** @summary Construct an empty original output. */
        FileOutcome()=default;
        FileOutcome(const FileOutcome&)=delete;FileOutcome& operator=(const FileOutcome&)=delete;
        FileOutcome(FileOutcome&&)=delete;FileOutcome& operator=(FileOutcome&&)=delete;
    };
    static_assert(sizeof(FileOutcome)<=512);
    static_assert(sizeof(FileObservation)<=512);
    /** @summary Admit exact same-State complete preparation through existing native reservations before path/effects.
     * @param manager Supplied original manager. @param original Unconsumed caller plan. @param lifetime Original admitted action lease. @return Early shape/identity result only. */
    static runtime::ScopeResult ValidatePersistence(ModManager& manager,Plan& original,const ModManager::Lease& lifetime);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    /** @summary Nonowning immutable input facts for original pre-effect rejection checks. */
    struct PlanFacts final {std::weak_ptr<const void> state,stamp,joint,node;bool consumed{},jointPrepared{},schedulerPrepared{},absent{};unsigned purpose{};};
    /** @summary Read native immutable plan facts without authority or mutation. @param plan Caller-exclusive original. @return Nonowning facts. */
    static PlanFacts ReadPlanFacts(const Plan& plan) noexcept;
#endif
    /** @summary Synchronously consume one genuine same-State plan after admission, retaining no runtime borrower on return.
     * @pre Caller is off lane and outside all publication/State guards, and exclusively owns input/output through teardown.
     * @param manager Original owner. @param original Genuine unchanged caller plan. @param output Empty file-only output.
     * @return Admission, exact memory rejection, or explicit committed/unresolved disposition. */
    static runtime::ScopeResult AttemptPersistence(ModManager& manager,Plan&& original,FileOutcome& output);
    /** @summary Reconcile only the already admitted original native operation; never attempt memory or initiate compensation.
     * @pre Exclusive off-lane/no-guard action ownership. @param output Original file-only continuation. @return Native protocol result. */
    static StatePathWriter::Result ReconcileFile(FileOutcome& output);
    /** @summary Settle only positively terminal original native bookkeeping, retaining mismatch/Unknown obligations.
     * @param turn Original exclusive file turn. @param output Original file-only outcome. */
    static void SettleFile(StatePathWriter::Turn& turn,FileOutcome& output) noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerPublicationTestAccess;
    friend struct ManagerCommandsTestAccess;
    /** @summary Build one actual binding before publication reservations. @param manager Actual owner. @param id Exact installed key. @return Native adapter. */
    static std::shared_ptr<OwnerLifecycle::Binding> Binding(ModManager& manager,const ModId& id);
    /** @summary Read exact canonical identity and actual active VM node under original D/M/S/V; inactive node is the genuine retained plan identity.
     * @param plan Exact canonical plan. @param output Cleared nonowning facts. @return Coherent result or actual bounded rejection. */
    static runtime::ScopeResult ReadLineage(const Plan& plan,Lineage& output);
    /** @summary Consume exact restricted preparation through the shared transaction. @param manager Original owner.
     * @param preparation Exact reviewed candidate. @param oldDecision Original current reservation.
     * @param newDecision Reviewed candidate reservation. @param oldContext Current facts. @param newContext Candidate facts.
     * @param output Empty retained plan. @param error Diagnostic. @return Preparation outcome without activation. */
    static runtime::ScopeResult Prepare(ModManager& manager,ManagerPreparation::Plan&& preparation,
        const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
        const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
        Plan& output,std::string& error,const LifecycleDecision* oldOwned=nullptr,const LifecycleDecision* newOwned=nullptr);
    /** @summary Prepare exact release-only removal. @param manager Owner. @param origin Exact retired/current identity.
     * @param output Empty plan. @param error Diagnostic. @return Preparation result. */
    static runtime::ScopeResult Remove(ModManager& manager,const std::shared_ptr<PublicationOrigin>& origin,Plan& output,std::string& error);
#endif
    /** @summary Commit via shared actual lifetime/ordered participants. @param plan Exact one-shot owner. @return Outcome. */
    static runtime::ScopeResult Commit(Plan& plan);
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fixture-only producer uses existing decision evaluator; absent from production. */
struct ManagerPublicationTestAccess final {
    /** @summary Fixed test-only observations of native persistence, never writable authorities. */
    using MemoryDisposition=ManagerPublication::MemoryDisposition;
    using FileOutcome=ManagerPublication::FileOutcome;
    using FileObservation=ManagerPublication::FileObservation;
    /** @summary Nonowning immutable input facts for pre-effect rejection/consumption checks. */
    using PlanFacts=ManagerPublication::PlanFacts;
    /** @summary Run the closed native action with existing fixture authority. @param manager Original manager. @param plan Genuine plan. @param output Empty original output. @return Actual result. */
    static runtime::ScopeResult AttemptPersistence(ModManager& manager,ManagerPublication::Plan&& plan,FileOutcome& output){return ManagerPublication::AttemptPersistence(manager,std::move(plan),output);}
    /** @summary Reconcile only original file bookkeeping. @param output Original output. @return Actual native result. */
    static StatePathWriter::Result ReconcileFile(FileOutcome& output){return ManagerPublication::ReconcileFile(output);}
    /** @summary Read fixed evidence under caller-exclusive ownership, never during concurrent action writes. @param output Original output. @return Scalar copy. */
    static FileObservation ObserveFile(const FileOutcome& output) noexcept;
    /** @summary Report complete native outcome storage. @return Actual bytes. */
    static std::size_t FileOutcomeBytes() noexcept{return sizeof(FileOutcome);}
    /** @summary Read original immutable plan identity under caller-exclusive ownership. @param plan Genuine plan. @return Nonowning facts. */
    static PlanFacts ReadPlanFacts(const ManagerPublication::Plan& plan) noexcept{return ManagerPublication::ReadPlanFacts(plan);}

    /** @summary Install a thread-local passive cut after the wrapper's first Access and before promotion; caller must clear on that same thread after return or exception.
     * @param callback Independently owned finite gate only; no authority or state fabrication. */
    static void BeforePromotion(std::function<void()> callback);
    /** @summary Native nonowning identities and coherent revisions, never capabilities. */
    using Lineage=ManagerPublication::Lineage;
    /** @summary Read exact actual identities for a retained canonical plan. @param plan Real plan. @param output Cleared facts. @return Actual observation result. */
    static runtime::ScopeResult ReadLineage(const ManagerPublication::Plan& plan,Lineage& output){return ManagerPublication::ReadLineage(plan,output);}
    /** @summary Uses exact original typed decision owners without changing canonical transaction logic.
     * @param manager Original owner. @param candidate Original preparation. @param oldDecision Exact old view or absent.
     * @param newDecision Exact new view or absent. @param oldContext Original facts. @param newContext Original facts.
     * @param output Empty plan. @param error Native diagnostics. @return Canonical preparation outcome. */
    static runtime::ScopeResult Prepare(ModManager& manager,ManagerPreparation::Plan&& candidate,
        const LifecycleDecision& oldDecision,const LifecycleDecision& newDecision,
        const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
        ManagerPublication::Plan& output,std::string& error){
        const auto old=oldDecision,next=newDecision;
        return ManagerPublication::Prepare(manager,std::move(candidate),old.Context(),next.Context(),oldContext,newContext,output,error,&old,&next);
    }
    /** @summary Prepare with an existing fixture scope before original Base binding. @param manager Owner. @param capture Exact bytes.
     * @param base Retained Base. @param observer Native evidence. @return Restricted candidate. */
    static ManagerPreparation::Plan Candidate(ModManager& manager,std::shared_ptr<const CapturedCandidate> capture,
        std::shared_ptr<scripting::BaseHost> base,std::function<void(scripting::ScriptPhase,std::uint64_t)> observer={});
    /** @summary Enroll only the actual isolated scheduler. @param manager Fresh owner. @return Exact enrollment result. */
    static runtime::ScopeResult Enroll(ModManager& manager);
    /** @summary Consume exact candidate into retained joint storage. @return Preparation outcome; no authority issued. */
    static runtime::ScopeResult Prepare(ModManager& manager,ManagerPreparation::Plan&& candidate,
        const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
        const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
        ManagerPublication::Plan& output,std::string& error) {
        return ManagerPublication::Prepare(manager,std::move(candidate),oldDecision,newDecision,oldContext,newContext,output,error);
    }
    /** @summary Prepare release-only exact removal. @return Preparation outcome. */
    static runtime::ScopeResult Remove(ModManager& manager,const std::shared_ptr<PublicationOrigin>& origin,
        ManagerPublication::Plan& output,std::string& error){return ManagerPublication::Remove(manager,origin,output,error);}
    /** @summary Publish actual sole stamp and participants. @param plan Exact one-shot joint plan. @return Atomic outcome. */
    static runtime::ScopeResult Commit(ManagerPublication::Plan& plan){return ManagerPublication::Commit(plan);}
    /** @summary Close prepared abort/retired VM outside all guards. @param plan Retained cleanup. @return Result. */
    static runtime::ScopeResult Reclaim(ManagerPublication::Plan& plan);
    /** @summary Observe exact original or successor identity. @param plan Retained preparation. @return Exact origin. */
    static std::shared_ptr<PublicationOrigin> Origin(const ManagerPublication::Plan& plan);
    /** @summary Observe actual map/scheduler/registry/scope/status under original reservations. */
    using Observation=OwnerLifecycleTestAccess::JointObservation;
    /** @summary Observe actual canonical participants. @param plan Retained binding. @param output Cleared facts. @return Coherent result. */
    static runtime::ScopeResult Observe(const ManagerPublication::Plan& plan,Observation& output);
    /** @summary Reach actual scheduler for existing contention/bypass probes. @param manager Native owner. @return Original scheduler. */
    static scripting::ScriptScheduler& Scheduler(ModManager& manager){return *manager.scriptScheduler_;}
    /** @summary Expose shared plan solely for negative fixture-wrapper bypass tests. @param plan Actual plan. @return Retained shared plan. */
    static OwnerLifecycle::Plan& Shared(ManagerPublication::Plan& plan){return plan.lifecycle_;}
};
#endif
}
