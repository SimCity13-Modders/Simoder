#include "mods/manager_publication.hpp"
#include "mods/manager_cache.hpp"
#include <stdexcept>

namespace sc13::mods {
namespace {
/** @summary Reject same-thread persistence recursion through file cuts and arbitrary synchronous Plan teardown. */
thread_local bool persistenceAction{};
/** @summary Covers action lifetime through final borrower release without retaining a runtime owner. */
struct PersistenceAction final {
    /** @summary Enter only after the caller has rejected existing recursion. */
    PersistenceAction() noexcept{persistenceAction=true;}
    /** @summary Leave after every action-scoped operational owner is gone. */
    ~PersistenceAction(){persistenceAction=false;}
};
}
runtime::ScopeResult ManagerPublication::ValidatePersistence(ModManager& manager,Plan& original,const ModManager::Lease& lifetime) {
    using S=runtime::ScopeResult;auto& p=original.lifecycle_;const auto& binding=p.binding_;
    if(!lifetime||lifetime.Manager()!=&manager)return S::Cancelled;
    if(!binding)return S::Invalid;
    if(!binding->state||binding->state!=manager.state_||binding->registry!=&manager.registry_||binding->domain!=manager.runtimeDomain_)return S::Conflict;
    if(manager.RuntimeStopping())return S::Cancelled;
    if(p.consumed_||!p.joint_||!p.schedulerPrepared_||!p.expected_||!p.next_||!p.failed_||!p.scheduler_.state_||!p.scheduler_.node_||!p.scheduler_.jointBinding_)return S::Invalid;
    const bool activation=p.purpose_==OwnerLifecycle::Purpose::Activation;
    const bool handoff=p.purpose_==OwnerLifecycle::Purpose::Handoff;
    const bool removal=p.purpose_==OwnerLifecycle::Purpose::Removal;
    if((activation&&(!p.successor_||p.old_||p.absent_))||(handoff&&(!p.old_||!p.successor_||p.absent_))||(removal&&(!p.old_||p.successor_))||(!activation&&!handoff&&!removal))return S::Invalid;
    const auto expectedPurpose=activation?scripting::SchedulerPublicationPurpose::Activate:handoff?scripting::SchedulerPublicationPurpose::Handoff:scripting::SchedulerPublicationPurpose::Remove;
    if(p.scheduler_.consumed_||p.scheduler_.state_!=manager.scriptScheduler_->state_||p.scheduler_.domain_!=binding->domain||p.scheduler_.domainRevision_!=p.domainRevision_||p.scheduler_.owner_!=binding->owner||p.scheduler_.purpose_!=expectedPurpose||p.scheduler_.absent_!=p.absent_||p.scheduler_.old_!=(!p.absent_&&p.old_?p.old_->runtime_:nullptr)||p.scheduler_.candidate_!=(p.successor_?p.successor_->runtime_:nullptr))return S::Conflict;
    if(p.successor_&&(!p.successor_->preparation_||!p.successor_->preparation_->promotion_||p.scheduler_.preparationProof_!=p.successor_->preparation_->promotion_))return S::Invalid;
    // @summary Reuse genuine ordered reservations for early complete-shape admission, never a substitute for the later final transaction.
    OwnerLifecycle::Access access(binding);if(access.result!=S::Ok)return access.result;
    if(!access.actual||access.participant->stamp!=p.expected_||binding->domain->Revision()!=p.domainRevision_||manager.enabledOrder_!=p.expectedOrder_||access.participant->jointBinding!=p.scheduler_.jointBinding_)return S::Stale;
    scripting::SchedulerPublicationReservation scheduler(p.scheduler_,access.participant->jointBinding);if(scheduler.result_!=S::Ok)return scheduler.result_;
    auto publication=binding->registry->TryReservePublication();if(!publication.owns_lock())return S::Busy;
    if(!publication.CanPublish(p.publication_))return S::Conflict;
    return S::Ok;
}
void ManagerPublication::SettleFile(StatePathWriter::Turn& turn,FileOutcome& output) noexcept {
    using W=StatePathWriter;auto& view=output.observation_;const auto& disk=view.disk;
    if(disk.operation==W::OperationIdentity{}){view.settlementPending=false;return;}
    if(disk.settled){view.settlementPending=false;return;}
    if(disk.phase==W::Phase::Prepared){view.disk=W::CancelPrepared(turn,disk.operation);}
    else if(disk.kind!=W::Kind::Unknown&&disk.kind!=W::Kind::Conflict&&
        (disk.phase==W::Phase::Published||disk.phase==W::Phase::Compensated||disk.phase==W::Phase::Rejected||disk.phase==W::Phase::CompensationFailed)){
        view.disk=W::Settle(turn,disk.operation);
    }
    view.settlementPending=!view.disk.settled;
}
runtime::ScopeResult ManagerPublication::AttemptPersistence(ModManager& manager,Plan&& original,FileOutcome& output) {
    using S=runtime::ScopeResult;using W=StatePathWriter;
    if(persistenceAction)return S::Busy;
    if(scripting::ScriptRuntime::OnScriptLane())return S::Denied;
    if(output.observation_.admitted)return S::Conflict;
    PersistenceAction actionLifetime;
    // @summary Lease is the first owning operational local and outlives path, file turn, cache action and whole Plan disposal.
    const ModManager::Lease lifetime(manager.state_);
    const auto valid=ValidatePersistence(manager,original,lifetime);if(valid!=S::Ok)return valid;
    std::filesystem::path path;
    {std::unique_lock guard(manager.mutex_,std::try_to_lock);if(!guard.owns_lock())return S::Busy;if(manager.RuntimeStopping())return S::Cancelled;path=manager.statePath_;}
    W::Owner owner;W::Turn turn;ManagerCacheAction cache;
    const auto reserved=ManagerCache::Prepare(lifetime,original.lifecycle_.binding_,original.lifecycle_.publication_,cache);if(reserved!=S::Ok)return reserved;
    const auto opened=W::Open(path,owner);if(opened.kind!=W::Kind::Ok)return opened.kind==W::Kind::Busy?S::Busy:S::NotReady;
    const auto acquired=W::TryTurn(owner,turn);if(acquired.kind!=W::Kind::Ok)return acquired.kind==W::Kind::Busy?S::Busy:S::NotReady;
    output.owner_=owner;auto& view=output.observation_;view.admitted=true;
    {
        // @summary Inner whole-object lifetime keeps both file Turn and original Lease through arbitrary final native VM callbacks.
        Plan whole(std::move(original));
        const auto perform=[&]{
            view.disk=W::Prepare(turn,whole.lifecycle_.desiredOrder_,output.intent_);
            if(view.disk.kind!=W::Kind::Ok){SettleFile(turn,output);return;}
            view.disk=W::Publish(turn,output.intent_,output.receipt_);
            if(!view.disk.publicationConfirmed){SettleFile(turn,output);return;}
            const auto observed=W::Observe(turn,output.receipt_);view.disk=observed.result;
            if(!view.disk.publicationConfirmed||view.disk.current!=W::Current::MatchesCandidate||!observed.candidateBytesMatch||!observed.defaultStreamOnly||!observed.owned.valid||!observed.destination.valid||observed.owned!=observed.destination){view.settlementPending=true;return;}
            OwnerLifecycle::CommitEvidence evidence;view.memoryAttempts=1;
            try{view.memoryResult=OwnerLifecycle::CommitBound(whole.lifecycle_.binding_,whole.lifecycle_,true,&cache,&evidence);}
            catch(...){view.memoryResult=evidence.crossed?S::Indeterminate:S::NotReady;}
            view.memory=evidence.memory;view.crossed=evidence.crossed;view.revision=evidence.revision;
            if(view.memory==MemoryDisposition::RejectedBeforeMarker&&!view.crossed){view.compensationRequested=true;view.disk=W::Compensate(turn,output.receipt_);}
            SettleFile(turn,output);
        };
        perform();
    }
    view.finalized=true;
    if(view.settlementPending||view.disk.kind==W::Kind::Unknown)return S::Indeterminate;
    if(view.memoryAttempts)return view.memoryResult;
    return view.disk.publicationConfirmed?S::Indeterminate:S::NotReady;
}
StatePathWriter::Result ManagerPublication::ReconcileFile(FileOutcome& output) {
    using W=StatePathWriter;W::Result denied;
    if(persistenceAction){denied.kind=W::Kind::Busy;return denied;}
    if(scripting::ScriptRuntime::OnScriptLane()||!output.observation_.admitted||!output.observation_.finalized||!output.owner_)return denied;
    PersistenceAction actionLifetime;W::Turn turn;
    const auto admitted=W::TryTurn(output.owner_,turn);if(admitted.kind!=W::Kind::Ok)return admitted;
    auto& view=output.observation_;if(!view.settlementPending)return view.disk;
    view.disk=W::Reconcile(turn,view.disk.operation);SettleFile(turn,output);return view.disk;
}


runtime::ScopeResult ManagerPublication::Commit(Plan& plan) {
    return OwnerLifecycle::CommitBound(plan.lifecycle_.binding_,plan.lifecycle_,true);
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
ManagerPublicationTestAccess::FileObservation ManagerPublicationTestAccess::ObserveFile(const FileOutcome& output) noexcept{return ManagerPublication::FileObservation(output.observation_);}
ManagerPublication::PlanFacts ManagerPublication::ReadPlanFacts(const Plan& plan) noexcept {
    const auto& p=plan.lifecycle_;PlanFacts result;result.state=p.binding_?p.binding_->state:nullptr;result.stamp=p.expected_;result.joint=p.scheduler_.jointBinding_;result.node=p.scheduler_.node_;
    result.consumed=p.consumed_;result.jointPrepared=p.joint_;result.schedulerPrepared=p.schedulerPrepared_;result.absent=p.absent_;result.purpose=static_cast<unsigned>(p.purpose_);return result;
}
/** @summary Fixture-thread passive interleaving only; it is never copied or invoked under native publication guards. */
thread_local std::function<void()> beforePromotion;
void ManagerPublicationTestAccess::BeforePromotion(std::function<void()> callback){beforePromotion=std::move(callback);}
std::shared_ptr<OwnerLifecycle::Binding> ManagerPublication::Binding(ModManager& manager,const ModId& id) {
    const ModManager::Lease lifetime(manager.state_);if(!lifetime)return {};
    auto binding=std::make_shared<OwnerLifecycle::Binding>();
    // @summary Preparation observation uses only M; no D acquisition or lane wait occurs while it is held.
    std::unique_lock lock(manager.mutex_,std::try_to_lock);if(!lock.owns_lock())return {};
    const auto found=manager.records_.find(id);if(found==manager.records_.end())return {};
    binding->state=manager.state_;binding->identity=found->second.participant.identity;
    binding->installation=found->second.installation;binding->fingerprint=found->second.fingerprint;
    binding->domain=manager.runtimeDomain_;binding->registry=&manager.registry_;binding->owner=id;
    binding->expectedOrder=manager.enabledOrder_;return binding;
}
runtime::ScopeResult ManagerPublication::Prepare(ModManager& manager,ManagerPreparation::Plan&& preparation,
    const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
    const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
    Plan& output,std::string& error,const LifecycleDecision* oldOwned,const LifecycleDecision* newOwned) {
    using runtime::ScopeResult;
    if(output.lifecycle_.binding_ || !preparation.qualified_ || !preparation.vm_ || preparation.promotion_ ||
        preparation.state_!=manager.state_)return ScopeResult::Invalid;
    const ModManager::Lease lifetime(manager.state_);if(!lifetime)return ScopeResult::Cancelled;
    if(!ManagerPreparation::OriginalBase(preparation) || preparation.endpoint_->scope!=preparation.scope_ ||
        preparation.endpoint_->generation!=preparation.vmGeneration_ || preparation.domain_!=manager.runtimeDomain_)
        return ScopeResult::Stale;
    auto binding=Binding(manager,preparation.owner_);if(!binding)return ScopeResult::Busy;
    const auto cacheBound=ManagerCache::Ensure(manager,preparation.owner_);if(cacheBound!=ScopeResult::Ok)return cacheBound;
    // @summary Preserve exact original immutable stamp and domain before the active-proof cycle-breaking resets below.
    const OwnerLifecycle::PreparationSnapshot original{preparation.stamp_,preparation.domainRevision_};
    std::shared_ptr<PublicationOrigin> old;
    {
        OwnerLifecycle::Access access(binding);if(access.result!=ScopeResult::Ok)return access.result;
        if(binding->domain->Revision()!=preparation.domainRevision_)return ScopeResult::Stale;
        if(access.participant->identity!=preparation.identity_ || access.participant->stamp!=preparation.stamp_ ||
            binding->fingerprint!=preparation.fingerprint_ || !preparation.capture_->SameInstallation(binding->installation))return ScopeResult::Stale;
        old=access.participant->stamp->current;
        if(!old && !access.participant->CanonicalEmpty())return ScopeResult::Denied;
        if(!old && (access.actual->state!=ModState::Inactive || access.actual->scriptRuntime))return ScopeResult::Denied;
    }
    if(beforePromotion)beforePromotion();
    if((oldOwned || newOwned) && (!oldOwned || !newOwned ||
        !OwnerLifecycle::DecisionMatches(*oldOwned,old?old->scope_:nullptr,oldDecision) ||
        !OwnerLifecycle::DecisionMatches(*newOwned,preparation.scope_,newDecision)))return ScopeResult::Conflict;
    auto origin=std::shared_ptr<PublicationOrigin>(new PublicationOrigin(preparation.vm_,preparation.endpoint_,preparation.capture_->ContentDigest()));
    origin->declarative_=preparation.capture_->Definition().patches;
    origin->scripted_=std::make_shared<const std::vector<ResourcePatchDefinition>>(preparation.declarations_);
    preparation.promotion_=std::make_shared<const std::byte>();
    // @summary Remove manager and predecessor stamp ownership before active storage can reference this proof.
    preparation.state_.reset();preparation.stamp_.reset();preparation.identity_.reset();
    origin->preparation_=std::make_shared<ManagerPreparation::Plan>(std::move(preparation));
    {
        std::unique_lock vm(origin->runtime_->mutex_,std::try_to_lock);if(!vm.owns_lock())return ScopeResult::Busy;
        if(!origin->runtime_->preparation_ || origin->runtime_->preparationProof_)return ScopeResult::Denied;
        origin->runtime_->preparationProof_=origin->preparation_->promotion_;
    }
    ScopeResult prepared;
    try{prepared=OwnerLifecycle::PrepareJointBound(*manager.scriptScheduler_,binding,old,origin,
        oldDecision,newDecision,oldContext,newContext,output.lifecycle_,error,original);}
    catch(...){
        // @summary Raw helper guards have unwound; even a genuine partially constructed output keeps its original fresh owners.
        if(oldOwned && newOwned)static_cast<void>(OwnerLifecycle::AttachDecisions(output.lifecycle_,*oldOwned,*newOwned));
        throw;
    }
    if(oldOwned && newOwned){const auto attached=OwnerLifecycle::AttachDecisions(output.lifecycle_,*oldOwned,*newOwned);if(attached!=ScopeResult::Ok)return attached;}
    return prepared;
}
runtime::ScopeResult ManagerPublication::Remove(ModManager& manager,const std::shared_ptr<PublicationOrigin>& origin,
    Plan& output,std::string& error) {
    if(!origin || output.lifecycle_.binding_)return runtime::ScopeResult::Invalid;
    const ModManager::Lease lifetime(manager.state_);if(!lifetime)return runtime::ScopeResult::Cancelled;
    auto binding=Binding(manager,origin->owner_);if(!binding)return runtime::ScopeResult::Busy;
    const auto cacheBound=ManagerCache::Ensure(manager,origin->owner_);if(cacheBound!=runtime::ScopeResult::Ok)return cacheBound;
    return OwnerLifecycle::PrepareJointBound(*manager.scriptScheduler_,binding,origin,{}, {},{}, {},{},output.lifecycle_,error,{});
}
runtime::ScopeResult ManagerPublication::ReadLineage(const Plan& plan,Lineage& output){
    using runtime::ScopeResult;output={};const auto& binding=plan.lifecycle_.binding_;if(!binding||!binding->state)return ScopeResult::Invalid;
    OwnerLifecycle::Access access(binding);if(access.result!=ScopeResult::Ok)return access.result;
    auto& participant=*access.participant;std::shared_ptr<scripting::ScriptRuntime> vm;std::unique_lock<std::mutex> schedulerGuard,vmGuard;
    const auto inspected=scripting::SchedulerPublicationReservation::Inspect(*access.lease->Manager()->scriptScheduler_,binding->owner,participant.jointBinding,vm,schedulerGuard,vmGuard);
    if(inspected!=ScopeResult::Ok)return inspected;
    output.state=binding->state;output.participant=participant.identity;output.installation=binding->installation;output.joint=participant.jointBinding;output.stamp=participant.stamp;
    output.domainRevision=binding->domain->Revision();output.recordRevision=participant.stamp->revision;output.active=static_cast<bool>(participant.stamp->current);
    if(vm){output.node=vm->publicationNode_;output.registration=vm->registration_.load();output.generation=vm->vmGeneration_;}
    else output.node=plan.lifecycle_.scheduler_.node_;
    if(participant.stamp->current)output.incarnation=participant.stamp->current->incarnation_;
    else output.incarnation=participant.stamp->tombstone;
    return ScopeResult::Ok;
}
ManagerPreparation::Plan ManagerPublicationTestAccess::Candidate(ModManager& manager,std::shared_ptr<const CapturedCandidate> capture,
    std::shared_ptr<scripting::BaseHost> base,std::function<void(scripting::ScriptPhase,std::uint64_t)> observer) {
    return ManagerPreparation::Prepare(manager,std::move(capture),std::move(base),std::move(observer),true);
}
runtime::ScopeResult ManagerPublicationTestAccess::Enroll(ModManager& manager) {
    const ModManager::Lease lifetime(manager.state_);if(!lifetime)return runtime::ScopeResult::Cancelled;
    return scripting::SchedulerPublicationTestAccess::Enroll(*manager.scriptScheduler_,manager.runtimeDomain_);
}
runtime::ScopeResult ManagerPublicationTestAccess::Reclaim(ManagerPublication::Plan& plan) {
    return OwnerLifecycleTestAccess::Reclaim(plan.lifecycle_);
}
std::shared_ptr<PublicationOrigin> ManagerPublicationTestAccess::Origin(const ManagerPublication::Plan& plan) {
    return plan.lifecycle_.successor_ ? plan.lifecycle_.successor_ : plan.lifecycle_.old_;
}
runtime::ScopeResult ManagerPublicationTestAccess::Observe(const ManagerPublication::Plan& plan,Observation& output) {
    using runtime::ScopeResult;output={};const auto binding=plan.lifecycle_.binding_;if(!binding)return ScopeResult::Invalid;
    const ModManager::Lease lifetime(binding->state);if(!lifetime)return ScopeResult::Cancelled;
    return OwnerLifecycle::ObserveBound(binding,*lifetime.Manager()->scriptScheduler_,output,&plan.lifecycle_);
}
#endif
}
