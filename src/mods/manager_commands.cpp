#include "mods/manager_commands.hpp"
#include "mods/manager_publication.hpp"
#include "mods/manager_cache.hpp"
#include <atomic>
#include <utility>
#include <limits>

namespace sc13::mods {
/** @summary Weak route avoids permanent owner cycles; only accepted original job/ticket retains its operation cohort. */
class ManagerCommandRoute final {
public:
    std::weak_ptr<ModManager::State> state;
    std::weak_ptr<PublicationOrigin> origin;
    /** @summary Borrowed identity only; exact VM's once-bound anchor owns lifetime. */
    ActionWorker::Control* control{};
    std::shared_ptr<const void> identity;
    std::shared_ptr<const InstallationBinding> installation;
    ModDefinitionFingerprint fingerprint;
    ModId owner;
    std::shared_ptr<runtime::RuntimeCommitDomain> domain;
    PatchRegistry* registry{};
    std::shared_ptr<const std::byte> node;
    runtime::OperationContext grant;
    std::atomic<runtime::ScopeResult> outcome{runtime::ScopeResult::NotReady};
    std::atomic_uint64_t attempts{},commits{},selected{},committedRegistration{},generation{};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    std::mutex callbacks;
    std::function<void(scripting::ScriptCommandBatch&)> intercept;
    std::function<void()> beforeLease,afterLease;
    std::shared_ptr<const std::function<void()>> beforeAcknowledgment,beforeConsume;
    /** @summary Fixture-only joined interleaving after original marker admission, not an alternate result producer. */
    std::shared_ptr<const std::function<void()>> cachePrepared;
    /** @summary Borrowed fixture code/context scalars are synchronized by actual V and never own lane payload. */
    void (*admissionProbe)(void*){};
    void* admissionContext{};
#endif
};
/** @summary Owned preselection evidence and off-fence retirement storage; no caller can construct it through the public envelope. */
struct ManagerCommandTicket final {
    /** @summary Return only this ticket's fresh native reservation after all compound guards have gone. */
    ~ManagerCommandTicket(){ManagerCommands::Release(*this);}
    std::shared_ptr<ManagerCommandRoute> route;
    std::shared_ptr<OwnerLifecycle::Binding> binding;
    std::shared_ptr<PublicationOrigin> origin;
    std::shared_ptr<OwnerParticipant::Stamp> expected,next,failed;
    std::shared_ptr<const std::vector<ResourcePatchDefinition>> baseline,reduced;
    std::vector<ResourcePatchDefinition> projection;
    std::vector<scripting::ScriptPatchCommand> commands;
    PatchRegistry::PreparedPublication publication;
    std::shared_ptr<const void> joint;
    scripting::SemanticContextLease context;
    runtime::OperationContext decision;
    /** @summary Original coupled native release identity survives delayed envelopes without an ordinary Lease. */
    runtime::RuntimeScope::CommandTicket commandTicket;
    std::uint64_t registration{},domainRevision{},generation{};
    bool attached{},consumed{};
};
/** @summary Stack-only original participant reservation; every fallible gate precedes publication. */
struct ManagerCommands::Fence final {
    /** @summary Try original S/V/R/B/Q under the caller's original D/M.
     * @param ticket Exact captured operation. @param access Original lifetime and map reservation.
     * @param capture True only before the first delayed-selection barrier. */
    Fence(ManagerCommandTicket& ticket,OwnerLifecycle::Access& access,bool capture) {
        using runtime::ScopeResult;
        if(access.result!=ScopeResult::Ok){result=access.result;return;}
        auto& origin=*ticket.origin;auto& binding=*ticket.binding;
        auto* participant=access.participant;
        if(!access.actual || !participant->jointBinding || participant->stamp->cleanupPending ||
            participant->stamp->current!=ticket.origin || origin.phase_!=PublicationOrigin::Phase::Active ||
            origin.jointBinding_!=participant->jointBinding || !origin.Current() || !origin.preparation_ ||
            access.actual->scriptRuntime!=origin.runtime_ || access.actual->runtimeScope!=origin.scope_ ||
            access.actual->state!=ModState::Active){result=ScopeResult::Stale;return;}
        if(capture) {
            ticket.expected=participant->stamp;ticket.baseline=origin.scripted_;
            ticket.joint=participant->jointBinding;ticket.domainRevision=binding.domain->Revision();
        } else if(participant->stamp!=ticket.expected || participant->jointBinding!=ticket.joint ||
            origin.scripted_!=ticket.baseline || binding.domain->Revision()!=ticket.domainRevision){result=ScopeResult::Stale;return;}
        result=scripting::SchedulerPublicationReservation::InspectExact(*access.lease->Manager()->scriptScheduler_,binding.owner,
            ticket.joint,origin.runtime_,ticket.route->node,ticket.registration,scheduler,vm);
        if(result!=ScopeResult::Ok)return;
        if(origin.runtime_->commandRoute_!=ticket.route || origin.generation_!=ticket.generation ||
            ticket.route->grant.opcode!=1){result=ScopeResult::Denied;return;}
        publication.emplace(binding.registry->TryReservePublication());
        if(!publication->owns_lock()){result=ScopeResult::Busy;return;}
        auto& bridge=*origin.endpoint_->bridge;
        context=std::unique_lock(bridge.mutex_,std::try_to_lock);if(!context.owns_lock()){result=ScopeResult::Busy;return;}
        result=OwnerLifecycle::ValidatePreparationLocked(ticket.origin,PublicationOrigin::Phase::Active);if(result!=ScopeResult::Ok)return;
        scope.emplace(*origin.scope_);if(!scope->Acquired()){result=ScopeResult::Busy;return;}
        if(!scope->InDomain(*binding.domain)){result=ScopeResult::Conflict;return;}
        if(capture) {
            // @summary Native Runtime/City facts come from the actual original bridge, never from script-supplied names.
            constexpr std::uint8_t mask=(1U<<static_cast<unsigned>(scripting::SemanticContextKind::Runtime)) |
                (1U<<static_cast<unsigned>(scripting::SemanticContextKind::City));
            for(std::size_t i=0;i<bridge.contexts_.size();++i)if(mask&(1U<<i)) {
                const auto& fact=bridge.contexts_[i];
                if(!fact.slot || !fact.generation || !fact.epoch){result=ScopeResult::NotReady;return;}
            }
            const auto current=scope->Snapshot();
            ticket.context={bridge.contexts_,origin.generation_,bridge.epoch_,0,current.generation,current.epoch,mask};
        }
        result=bridge.ValidateContextLocked(*origin.endpoint_,ticket.context);if(result!=ScopeResult::Ok)return;
        const auto current=scope->Snapshot();
        if(current.generation!=ticket.context.scopeGeneration || current.epoch!=ticket.context.runtimeEpoch){result=ScopeResult::Stale;return;}
        if(!capture) {
            if(ticket.decision.opcode!=1 || ticket.decision.owner!=ticket.context.scopeGeneration ||
                ticket.decision.epoch!=ticket.context.runtimeEpoch){result=ScopeResult::Denied;return;}
            result=scope->Check(ticket.decision,false);
        }
    }
    // @summary Reverse member destruction releases Q/B/R/V/S before the outer M/D/native lease.
    std::unique_lock<std::mutex> scheduler,vm;
    std::optional<PatchRegistry::PublicationGuard> publication;
    std::unique_lock<std::mutex> context;
    std::optional<runtime::RuntimeScope::LifecycleGuard> scope;
    runtime::ScopeResult result{runtime::ScopeResult::Invalid};
};
void ManagerCommands::Release(ManagerCommandTicket& ticket) noexcept {
    if(ticket.origin && ticket.origin->scope_)
        static_cast<void>(ticket.origin->scope_->ReleaseCommandTicket(ticket.commandTicket));
    ticket.commandTicket={};
    ticket.decision={};
}
bool ManagerCommands::TrySchedule(scripting::ScriptRuntime& vm,std::uint64_t registration) noexcept {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    fixture::AdmissionInterval admission(&vm);
#define SIMODER_ADMISSION_REASON(value) admission.Reason(fixture::AdmissionReason::value)
#else
#define SIMODER_ADMISSION_REASON(value) static_cast<void>(0)
#endif
    ActionWorker::Control* control{};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    void (*admissionProbe)(void*){};
    void* admissionContext{};
#endif
    {
        std::unique_lock initial(vm.mutex_,std::try_to_lock);
        if(!initial.owns_lock()){SIMODER_ADMISSION_REASON(InitialV);return true;}
        if(!vm.commandRoute_){SIMODER_ADMISSION_REASON(Routeless);return false;}
        control=vm.commandControl_.get();
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        admissionProbe=vm.commandRoute_->admissionProbe;admissionContext=vm.commandRoute_->admissionContext;
#endif
    }
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    // @summary Neither V nor W is held; a throwing observation rejects only this still-unaccepted visit.
    try{if(admissionProbe)admissionProbe(admissionContext);}catch(...){SIMODER_ADMISSION_REASON(ProbeException);return true;}
#endif
    if(!control){SIMODER_ADMISSION_REASON(MissingControl);return true;}
    // @summary Selected shared VM pins its immutable anchor; no ownership copy precedes fallible admission.
    std::unique_lock worker(control->mutex,std::try_to_lock);if(!worker.owns_lock()){SIMODER_ADMISSION_REASON(WorkerLock);return true;}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    fixture::WorkerRegion workerRegion;
#endif
    if(control->stopping||control->closeAdmission->load()){SIMODER_ADMISSION_REASON(Closed);return true;}
    if(control->busy.load()){SIMODER_ADMISSION_REASON(Busy);return true;}
    std::unique_lock selected(vm.mutex_,std::try_to_lock);if(!selected.owns_lock()){SIMODER_ADMISSION_REASON(FinalV);return true;}
    if(vm.commandControl_.get()!=control||!vm.commandRoute_||vm.commandRoute_->control!=control||
        vm.commandRoute_->grant.opcode!=1||!vm.ExecutionEligible(registration)||!vm.scheduled_.load()||
        std::chrono::steady_clock::now()-vm.lastTick_<std::chrono::milliseconds(50)||
        !std::holds_alternative<std::monostate>(control->pending)){SIMODER_ADMISSION_REASON(Ineligible);return true;}
    static_assert(sizeof(ActionWorker::ScheduledJob)<=128);
    auto& job=control->pending.emplace<ActionWorker::ScheduledJob>();control->busy.store(true);
    auto origin=vm.commandRoute_->origin.lock();
    if(!origin){control->pending.emplace<std::monostate>();control->busy.store(false);SIMODER_ADMISSION_REASON(OriginExpired);return true;}
    // @summary No rejection, allocation, callback or fallible operation follows successful original promotion.
    job.origin=std::move(origin);job.route=vm.commandRoute_;
    job.registration=registration;job.generation=vm.vmGeneration_;
    control->wake.notify_one();SIMODER_ADMISSION_REASON(Accepted);return true;
#undef SIMODER_ADMISSION_REASON
}
void ManagerCommands::ExecuteScheduled(ActionWorker::ScheduledJob& job) {
    if(scripting::ScriptRuntime::OnScriptLane()||!job.route||!job.origin)return;
    const auto state=job.route->state.lock();
    const ModManager::Lease lifetime(state);if(!lifetime)return;
    auto& vm=*job.origin->runtime_;
    if(vm.vmGeneration_!=job.generation)return;
    auto batch=Select(vm,job.registration,job.route,job.origin);
    if(!batch.ticket_)return;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    if(vm.selectedBarrier_)vm.selectedBarrier_();
#endif
    scripting::ScriptRuntime::CapturedTick transfer;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    std::shared_ptr<const std::function<void()>> beforeAcknowledgment,beforeConsume;
    {const std::scoped_lock lock(job.route->callbacks);beforeAcknowledgment=job.route->beforeAcknowledgment;beforeConsume=job.route->beforeConsume;}
    transfer.beforeAcknowledgment=beforeAcknowledgment.get();
#endif
    try{vm.InvokeSelectedTick(job.registration,job.generation,job.route.get(),transfer);}
    catch(...){return;}
    if(!transfer.executed)return;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    if(beforeConsume)(*beforeConsume)();
#endif
    Attach(batch,std::move(transfer.output));
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    if(vm.extractedBarrier_&&batch.ticket_->attached)vm.extractedBarrier_();
#endif
    Dispatch(batch);
    // @summary Preserve original host logging via its existing release-only lane control, without compound guards.
    lifetime.Manager()->scriptScheduler_->Flush(vm.modId_,job.origin->runtime_);
}
scripting::ScriptCommandBatch ManagerCommands::Select(scripting::ScriptRuntime& vm,std::uint64_t registration,
    const std::shared_ptr<ManagerCommandRoute>& selectedRoute,const std::shared_ptr<PublicationOrigin>& selectedOrigin) {
    scripting::ScriptCommandBatch batch;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    const auto& route=selectedRoute;
    {std::unique_lock lock(vm.mutex_,std::try_to_lock);if(!lock.owns_lock()||vm.commandRoute_!=route)return batch;}
    if(!route || route->grant.opcode!=1)return batch;
    // @summary All storage is allocated before any original participant reservation.
    auto ticket=std::make_unique<ManagerCommandTicket>();ticket->route=route;ticket->origin=selectedOrigin;
    if(!ticket->origin)return batch;
    ticket->binding=std::make_shared<OwnerLifecycle::Binding>();
    auto& binding=*ticket->binding;binding.state=route->state.lock();if(!binding.state)return batch;
    binding.identity=route->identity;binding.installation=route->installation;binding.fingerprint=route->fingerprint;
    binding.owner=route->owner;binding.domain=route->domain;binding.registry=route->registry;
    ticket->registration=registration;ticket->generation=ticket->origin->generation_;
    {
        OwnerLifecycle::Access access(ticket->binding);Fence fence(*ticket,access,true);
        if(fence.result!=runtime::ScopeResult::Ok)return batch;
    }
    // @summary The fixture-only issuer creates a new bounded reservation from existing reviewed provenance, outside every guard.
    ticket->decision=ticket->origin->scope_->TryReserveCommand(route->grant,ticket->commandTicket);
    if(!ticket->decision.reservation || ticket->decision.reservation==route->grant.reservation)return batch;
    {
        OwnerLifecycle::Access access(ticket->binding);Fence fence(*ticket,access,false);
        if(fence.result!=runtime::ScopeResult::Ok)return batch;
    }
    route->selected.store(registration);route->generation.store(ticket->generation);
    batch.ticket_=std::move(ticket);
#else
    static_cast<void>(vm);static_cast<void>(registration);static_cast<void>(selectedRoute);static_cast<void>(selectedOrigin);
#endif
    return batch;
}
void ManagerCommands::Attach(scripting::ScriptCommandBatch& batch,std::vector<scripting::ScriptPatchCommand>&& commands) {
    if(!batch.ticket_ || batch.ticket_->attached || batch.ticket_->consumed || commands.empty())return;
    batch.ticket_->commands=std::move(commands);batch.ticket_->attached=true;
}
void ManagerCommands::Dispatch(scripting::ScriptCommandBatch& batch) {
    if(!batch.ticket_ || !batch.ticket_->attached)return;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    std::function<void(scripting::ScriptCommandBatch&)> intercept;
    {const std::scoped_lock lock(batch.ticket_->route->callbacks);intercept=batch.ticket_->route->intercept;}
    if(intercept)intercept(batch);
#endif
    if(batch.ticket_)static_cast<void>(Consume(batch));
}
runtime::ScopeResult ManagerCommands::Consume(scripting::ScriptCommandBatch& batch) {
    using runtime::ScopeResult;
    if(!batch.ticket_ || !batch.ticket_->attached || batch.ticket_->consumed)return ScopeResult::Conflict;
    auto& ticket=*batch.ticket_;ticket.consumed=true;auto route=ticket.route;
    ++route->attempts;
    auto outcome=ScopeResult::Invalid;bool committed{},closeStatus{},crossed{};
    try {
        // @summary Copy fixture callbacks before entry; callbacks never execute inside any compound publication guard.
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        std::function<void()> before,after;
        std::shared_ptr<const std::function<void()>> cachePrepared;
        {const std::scoped_lock lock(route->callbacks);before=route->beforeLease;after=route->afterLease;cachePrepared=route->cachePrepared;}
        if(before)before();
#endif
        const ModManager::Lease lifetime(ticket.binding->state);
        if(!lifetime)outcome=ScopeResult::Cancelled;
        else {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
            if(after)after();
#endif
            // @summary Staging and all fallible allocation happen while borrowed dependencies are leased, outside D/M/S/V/R/B/Q.
            ticket.next=std::make_shared<OwnerParticipant::Stamp>();ticket.failed=std::make_shared<OwnerParticipant::Stamp>();
            std::string error;bool busy{};
            if(!OwnedPublication::PreparePatches(*ticket.origin,*ticket.binding->registry,ticket.baseline,
                ticket.commands,ticket.reduced,ticket.publication,error,&busy))outcome=busy ? ScopeResult::Busy : ScopeResult::Invalid;
            else {
                ticket.projection=*ticket.reduced;
                ticket.next->current=ticket.origin;ticket.next->revision=ticket.expected->revision+1;
                ticket.failed->current=ticket.origin;ticket.failed->revision=ticket.next->revision;ticket.failed->cleanupPending=true;
                ManagerCacheAction cacheAction;
                outcome=ManagerCache::Prepare(lifetime,ticket.binding,ticket.publication,cacheAction);
                if(outcome==ScopeResult::Ok){
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
                // @summary Only original record turn is retained; no compound/store/cache mutex exists at this actual cut.
                if(cachePrepared)(*cachePrepared)();
#endif
                {
                scripting::SchedulerPublicationReservation::Fence instrumentation;
                OwnerLifecycle::Access access(ticket.binding);Fence fence(ticket,access,false);outcome=fence.result;
                if(outcome==ScopeResult::Ok) {
                    if(!cacheAction.Matches(*access.participant))outcome=ScopeResult::Conflict;
                    else if(!fence.publication->CanPublish(ticket.publication))outcome=ScopeResult::Conflict;
                    else if(OwnedPublication::SamePatches(*ticket.baseline,*ticket.reduced))outcome=ScopeResult::Ok;
                    else if(ticket.expected->revision==UINT64_MAX || !access.writer.CanAdvance())outcome=ScopeResult::NotReady;
                    else {
                        // @summary Last fallible gate; every object replaced below remains retained in this original ticket off-fence.
                        const auto revision=access.writer.Advance();
                        if(!revision)outcome=ScopeResult::NotReady;
                        else {
                        cacheAction.Acknowledge(revision);crossed=true;
                        if(!fence.publication->Publish(ticket.publication,revision)) {
                            auto& original=*ticket.origin->runtime_;
                            original.committed_=false;original.running_=false;original.retired_=true;
                            original.scheduled_.store(false);original.stopRequested_.store(true);
                            fence.scope->Retire();closeStatus=true;
                            access.participant->stamp.swap(ticket.failed);outcome=ScopeResult::Indeterminate;
                        } else {
                            ticket.origin->scripted_.swap(ticket.reduced);
                            access.actual->scriptPatches.swap(ticket.projection);
                            access.participant->stamp.swap(ticket.next);
                            outcome=ScopeResult::Ok;committed=true;
                        }
                        }
                    }
                }
                }
                // @summary Cache eviction is postcommit and leased; no live game refresh is asserted by logical publication.
                if(crossed&&cacheAction.Finish()!=ScopeResult::Ok)outcome=ScopeResult::Indeterminate;
                if(closeStatus) {
                    const std::scoped_lock original(ticket.origin->runtime_->mutex_);
                    ticket.origin->runtime_->PublishStatus();
                }
                }
            }
        }
    } catch(...) {outcome=(crossed || committed || closeStatus) ? ScopeResult::Indeterminate : ScopeResult::Invalid;}
    // @summary Exact reservation release may free native map storage, so it occurs only after every fence and lease guard has ended.
    Release(ticket);
    route->outcome.store(outcome);
    if(committed || outcome==ScopeResult::Ok){++route->commits;route->committedRegistration.store(ticket.registration);}
    return outcome;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
runtime::ScopeResult ManagerCommandsTestAccess::Install(ModManager& manager,const std::shared_ptr<PublicationOrigin>& origin,
    const runtime::OperationContext& grant) {
    using runtime::ScopeResult;
    if(!origin || grant.opcode!=1 || !grant.reservation)return ScopeResult::Denied;
    const ModManager::Lease lease(manager.state_);if(!lease)return ScopeResult::Cancelled;
    const auto originalControl=manager.actionWorker_->control_;
    auto binding=ManagerPublication::Binding(manager,origin->owner_);if(!binding)return ScopeResult::Busy;
    const auto cacheBound=ManagerCache::Ensure(manager,origin->owner_);if(cacheBound!=ScopeResult::Ok)return cacheBound;
    auto route=std::make_shared<ManagerCommandRoute>();
    route->state=binding->state;route->origin=origin;route->identity=binding->identity;
    route->installation=binding->installation;route->fingerprint=binding->fingerprint;route->owner=binding->owner;
    route->domain=binding->domain;route->registry=binding->registry;route->grant=grant;
    OwnerLifecycle::Access access(binding);if(access.result!=ScopeResult::Ok)return access.result;
    if(!origin->preparation_ || !origin->jointBinding_ || access.participant->jointBinding!=origin->jointBinding_ ||
        !origin->Current() || (origin->phase_!=PublicationOrigin::Phase::Candidate &&
        (origin->phase_!=PublicationOrigin::Phase::Active || access.participant->stamp->current!=origin)))return ScopeResult::Denied;
    auto& vm=*origin->runtime_;std::unique_lock lock(vm.mutex_,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
    runtime::RuntimeScope::LifecycleGuard scope(*origin->scope_);if(!scope.Acquired())return ScopeResult::Busy;
    if(vm.commandRoute_ || !vm.preparationProof_ || vm.host_.semantic!=origin->endpoint_ ||
        grant.owner!=scope.Snapshot().generation)return ScopeResult::Denied;
    route->node=vm.publicationNode_;if(!route->node)return ScopeResult::Denied;
    if(vm.commandControl_&&vm.commandControl_!=originalControl)return ScopeResult::Denied;
    route->control=originalControl.get();
    if(!vm.commandControl_)vm.commandControl_=originalControl;
    vm.commandRoute_=std::move(route);return ScopeResult::Ok;
}
void ManagerCommandsTestAccess::Intercept(const std::shared_ptr<scripting::ScriptRuntime>& vm,
    std::function<void(scripting::ScriptCommandBatch&)> callback) {
    std::shared_ptr<ManagerCommandRoute> route;{const std::scoped_lock lock(vm->mutex_);route=vm->commandRoute_;}
    if(route){const std::scoped_lock lock(route->callbacks);route->intercept=std::move(callback);}
}
void ManagerCommandsTestAccess::Barriers(const std::shared_ptr<scripting::ScriptRuntime>& vm,
    std::function<void()> beforeLease,std::function<void()> afterLease) {
    std::shared_ptr<ManagerCommandRoute> route;{const std::scoped_lock lock(vm->mutex_);route=vm->commandRoute_;}
    if(route){const std::scoped_lock lock(route->callbacks);route->beforeLease=std::move(beforeLease);route->afterLease=std::move(afterLease);}
}
ManagerCommandsTestAccess::Observation ManagerCommandsTestAccess::Observe(const std::shared_ptr<scripting::ScriptRuntime>& vm) {
    std::shared_ptr<ManagerCommandRoute> route;{const std::scoped_lock lock(vm->mutex_);route=vm->commandRoute_;}
    if(!route)return {};
    return {route->outcome.load(),route->attempts.load(),route->commits.load(),route->selected.load(),
        route->committedRegistration.load(),route->generation.load()};
}
std::size_t ManagerCommandsTestAccess::Reservations(const runtime::RuntimeScope& scope){return runtime::RuntimeScopeTestAccess::Reservations(scope);}
runtime::CacheSettlement::Observation ManagerCommandsTestAccess::ObserveCache(ModManager& manager,const ModId& id){return ManagerCache::Inspect(manager,id,false);}
runtime::CacheSettlement::Observation ManagerCommandsTestAccess::RepairCache(ModManager& manager,const ModId& id){return ManagerCache::Inspect(manager,id,true);}
runtime::CacheSettlement::Result ManagerCommandsTestAccess::PendingCache(ModManager& manager,const ModId& id,runtime::CacheSettlement::Batch& output){return ManagerCache::Pending(manager,id,output);}
void ManagerCommandsTestAccess::CachePreparedBarrier(const std::shared_ptr<scripting::ScriptRuntime>& vm,std::shared_ptr<const std::function<void()>> callback) {
    std::shared_ptr<ManagerCommandRoute> route;
    {const std::scoped_lock lock(vm->mutex_);route=vm->commandRoute_;}
    if(!route)return;
    {const std::scoped_lock lock(route->callbacks);route->cachePrepared.swap(callback);}
    // @summary Retired callback and route snapshot dispose after V and callback guard have both ended.
}
void ManagerCommandsTestAccess::AdmissionBarrier(const std::shared_ptr<scripting::ScriptRuntime>& vm,void (*callback)(void*),void* context) {
    const std::scoped_lock lock(vm->mutex_);
    if(auto* route=vm->commandRoute_.get()){route->admissionProbe=callback;route->admissionContext=context;}
}
ManagerCommandsTestAccess::Cohort ManagerCommandsTestAccess::WatchCohort(ModManager& manager,const std::shared_ptr<PublicationOrigin>& origin,const std::shared_ptr<scripting::BaseHost>& base){
    Cohort value;if(!origin||!origin->runtime_||!base||scripting::ScriptRuntime::OnScriptLane())return value;
    const void* stateAddress{};const void* preparationAddress{};
    {std::unique_lock lock(manager.mutex_,std::try_to_lock);if(!lock.owns_lock())return {};value.state=manager.state_;stateAddress=manager.state_.get();}
    {std::unique_lock lock(origin->runtime_->mutex_,std::try_to_lock);if(!lock.owns_lock()||!origin->preparation_||origin->preparation_->base_!=base)return {};value.preparation=origin->preparation_;preparationAddress=origin->preparation_.get();value.origin=origin;value.base=base;}
    value.preparationEntry=fixture::ArmDestruction(preparationAddress,fixture::ObjectKind::Preparation);value.stateEntry=fixture::ArmDestruction(stateAddress,fixture::ObjectKind::State);value.baseEntry=fixture::ArmDestruction(base.get(),fixture::ObjectKind::Base);return value;
}
void ManagerCommandsTestAccess::BeforeDetach(ModManager& manager,std::shared_ptr<const std::function<void(QueueObservation)>> callback){
    const ModManager::Lease lifetime(manager.state_);if(!lifetime)return;const auto control=manager.actionWorker_->control_;
    {const std::scoped_lock lock(control->mutex);fixture::WorkerRegion region;control->beforeDetach.swap(callback);}
}
std::shared_ptr<const std::atomic_bool> ManagerCommandsTestAccess::ActionClosed(ModManager& manager){
    const ModManager::Lease lifetime(manager.state_);if(!lifetime)return {};return manager.actionWorker_->control_->closeAdmission;
}
ManagerCommandsTestAccess::Layout ManagerCommandsTestAccess::HandoffLayout() noexcept {
    return {sizeof(ActionWorker::ScheduledJob),sizeof(ActionWorker::Control),sizeof(ActionWorker::PriorStorage),
        sizeof(std::shared_ptr<ActionWorker::Control>),sizeof(scripting::ScriptRuntime::CapturedTick)};
}
void ManagerCommandsTestAccess::HandoffBarriers(const std::shared_ptr<scripting::ScriptRuntime>& vm,
    std::shared_ptr<const std::function<void()>> beforeAcknowledgment,std::shared_ptr<const std::function<void()>> beforeConsume) {
    std::shared_ptr<ManagerCommandRoute> route;
    {const std::scoped_lock lock(vm->mutex_);route=vm->commandRoute_;}
    if(!route)return;
    {const std::scoped_lock lock(route->callbacks);route->beforeAcknowledgment.swap(beforeAcknowledgment);route->beforeConsume.swap(beforeConsume);}
}
void ManagerCommandsTestAccess::HoldWorker(ModManager& manager,const std::function<void()>& callback) {
    const ModManager::Lease lifetime(manager.state_);if(!lifetime)return;
    const auto control=manager.actionWorker_->control_;
    const std::scoped_lock lock(control->mutex);fixture::WorkerRegion region;callback();
}
#endif
}
namespace sc13::scripting {
ScriptCommandBatch::ScriptCommandBatch() noexcept=default;
ScriptCommandBatch::~ScriptCommandBatch()=default;
ScriptCommandBatch::ScriptCommandBatch(ScriptCommandBatch&& other) noexcept=default;
ScriptCommandBatch& ScriptCommandBatch::operator=(ScriptCommandBatch&& other) noexcept=default;
}
