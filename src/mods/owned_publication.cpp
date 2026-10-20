#include "mods/owned_publication.hpp"
#include "mods/manager_cache.hpp"
#include "scripting/script_patch_reducer.hpp"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <bit>
#include <new>

namespace sc13::mods {
/** @summary One fixed native fresh-reservation owner shared by genuine lifecycle Plans. */
class LifecycleDecisionLease final {
    friend class OwnerLifecycle;
    friend struct LifecycleDecisionTestAccess;
    std::shared_ptr<runtime::RuntimeScope> scope_;
    runtime::RuntimeScope::CommandTicket ticket_;
    runtime::OperationContext context_;
public:
    /** @summary Allocates empty release bookkeeping before any fresh reservation exists. */
    LifecycleDecisionLease()=default;
    /** @summary Releases only the original fresh ticket, immediately or through the existing bounded mailbox. */
    ~LifecycleDecisionLease(){if(scope_ && context_.owner)static_cast<void>(scope_->ReleaseCommandTicket(ticket_));}
};
static_assert(sizeof(LifecycleDecisionLease)<=512);
OwnerLifecycle::Plan& OwnerLifecycle::Plan::operator=(Plan&& other) noexcept {
    if(this!=&other){this->~Plan();::new(static_cast<void*>(this)) Plan(std::move(other));}return *this;
}
bool OwnerLifecycle::DecisionMatches(const LifecycleDecision& decision,const std::shared_ptr<runtime::RuntimeScope>& scope,
    const runtime::OperationContext& context) noexcept {
    if(!scope)return !decision.scope_ && !decision.context_.owner && !decision.lease_ && !context.owner;
    if(decision.scope_!=scope || !context.owner || decision.context_!=context)return false;
    return !decision.lease_ || (decision.lease_->scope_==scope && decision.lease_->context_==context);
}
runtime::ScopeResult OwnerLifecycle::AttachDecisions(Plan& plan,const LifecycleDecision& old,const LifecycleDecision& next) noexcept {
    using runtime::ScopeResult;
    if(!plan.binding_)return ScopeResult::Ok;
    if(!DecisionMatches(old,plan.old_?plan.old_->scope_:nullptr,plan.oldDecision_) ||
        !DecisionMatches(next,plan.successor_?plan.successor_->scope_:nullptr,plan.newDecision_))return ScopeResult::Conflict;
    if(plan.typedDecisions_ && (plan.oldDecisionOwner_!=old.lease_ || plan.newDecisionOwner_!=next.lease_))return ScopeResult::Conflict;
    plan.oldDecisionOwner_=old.lease_;plan.newDecisionOwner_=next.lease_;plan.typedDecisions_=true;return ScopeResult::Ok;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
runtime::ScopeResult LifecycleDecisionTestAccess::RootDecision(const std::shared_ptr<runtime::RuntimeScope>& scope,
    const runtime::OperationContext& context,LifecycleDecision& output){
    using runtime::ScopeResult;if(!scope || output.scope_)return ScopeResult::Invalid;
    const auto result=scope->CheckRootDecision(context);if(result!=ScopeResult::Ok)return result;
    output.scope_=scope;output.context_=context;return ScopeResult::Ok;
}
runtime::ScopeResult LifecycleDecisionTestAccess::FreshDecision(const std::shared_ptr<runtime::RuntimeScope>& scope,
    const runtime::OperationContext& context,LifecycleDecision& output){
    using runtime::ScopeResult;if(!scope || output.scope_)return ScopeResult::Invalid;
    try{
        auto owner=std::make_shared<LifecycleDecisionLease>();owner->scope_=scope;
        ScopeResult result;owner->context_=scope->TryReserveTracked(context,owner->ticket_,runtime::RuntimeScope::ReservationRole::LifecycleDecision,&result);
        if(result!=ScopeResult::Ok)return result;
        output.scope_=scope;output.context_=owner->context_;output.lease_=std::move(owner);return ScopeResult::Ok;
    }catch(...){return ScopeResult::NotReady;}
}
std::size_t LifecycleDecisionTestAccess::LeaseBytes() noexcept{return sizeof(LifecycleDecisionLease);}
namespace {
/** @summary Fixture-only interleaving point outside D/S/V/R/B/Q/T; absent from production Core. */
thread_local std::function<void()> preparationBarrier;
}
#endif
PublicationOrigin::PublicationOrigin(std::shared_ptr<scripting::ScriptRuntime> runtime,
    std::shared_ptr<scripting::SemanticEndpoint> endpoint, std::string fingerprint)
    : runtime_(std::move(runtime)), endpoint_(std::move(endpoint)), fingerprint_(std::move(fingerprint)) {
    if (!runtime_ || !endpoint_ || !endpoint_->scope || !endpoint_->tasks ||
        runtime_->host_.semantic != endpoint_ || runtime_->vmGeneration_ != endpoint_->ScriptGeneration() || fingerprint_.empty())
        throw std::invalid_argument("Publication origin does not match the loaded VM");
    scope_ = endpoint_->scope; definition_ = runtime_->loadedDefinition_;
    if (!definition_) throw std::invalid_argument("Publication origin has no loaded semantic snapshot");
    owner_ = runtime_->modId_; generation_ = runtime_->vmGeneration_;
    incarnation_=std::make_shared<const std::byte>(std::byte{});
}
bool PublicationOrigin::Current() const noexcept {
    return runtime_ && endpoint_ && scope_ && definition_ && incarnation_ &&
        runtime_->vmGeneration_ == generation_ && runtime_->loadedDefinition_ == definition_ &&
        runtime_->host_.semantic == endpoint_ && endpoint_->scope == scope_ && endpoint_->ScriptGeneration() == generation_;
}
bool OwnedPublication::PreparePatches(const PublicationOrigin& origin,PatchRegistry& registry,
    const std::shared_ptr<const std::vector<ResourcePatchDefinition>>& baseline,
    const std::vector<scripting::ScriptPatchCommand>& commands,
    std::shared_ptr<const std::vector<ResourcePatchDefinition>>& next,PatchRegistry::PreparedPublication& plan,std::string& error,bool* busy) {
    if(busy)*busy=false;
    if(!baseline || commands.size()>256) {error="Invalid bounded PROP batch";return false;}
    for(const auto& command:commands)if(command.value && !std::isfinite(*command.value)) {
        error="Nonfinite PROP command";return false;
    }
    next=std::make_shared<const std::vector<ResourcePatchDefinition>>(scripting::ReduceScriptPatches(*baseline,commands));
    auto patches=origin.declarative_;
    patches.insert(patches.end(),next->begin(),next->end());
    const auto kind=busy ? PatchRegistry::ChangeKind::Replace : registry.IsActive(origin.owner_) ?
        PatchRegistry::ChangeKind::Replace : PatchRegistry::ChangeKind::Activate;
    return registry.PrepareOwned(kind,origin.owner_,patches,origin.incarnation_,plan,error,busy);
}
bool OwnedPublication::SamePatches(const std::vector<ResourcePatchDefinition>& left,const std::vector<ResourcePatchDefinition>& right) noexcept {
    if(left.size()!=right.size())return false;
    for(std::size_t i=0;i<left.size();++i) {
        const auto& a=left[i];const auto& b=right[i];
        if(a.target!=b.target || a.name!=b.name || a.source!=b.source || a.properties.size()!=b.properties.size())return false;
        for(std::size_t j=0;j<a.properties.size();++j) {
            const auto& x=a.properties[j];const auto& y=b.properties[j];
            if(x.propertyId!=y.propertyId || x.type!=y.type || x.operation!=y.operation ||
                std::bit_cast<std::uint32_t>(x.value)!=std::bit_cast<std::uint32_t>(y.value) ||
                x.name!=y.name || x.source!=y.source || x.description!=y.description || x.unit!=y.unit || x.origin!=y.origin)return false;
        }
    }
    return true;
}
runtime::ScopeResult OwnedPublication::Validate(const scripting::SemanticInvocation& invocation) const noexcept {
    using runtime::ScopeResult;
    if (!origin_) return ScopeResult::NotReady;
    // @summary Establish the exact original domain before reading its writer-serialized phase or baseline.
    if(invocation.origin!=origin_->endpoint_ || !invocation.origin || invocation.origin->scope!=origin_->scope_)
        return ScopeResult::Conflict;
    if (origin_->phase_ != PublicationOrigin::Phase::Active) return ScopeResult::NotReady;
    if (origin_->scripted_ != baseline_) return ScopeResult::Conflict;
    if (!origin_->Current())
        return ScopeResult::Stale;
    const auto& captured = invocation_;
    if (invocation.origin != origin_->endpoint_ || invocation.origin != captured.origin ||
        invocation.task != captured.task || invocation.participant != captured.participant ||
        invocation.epoch != captured.epoch || invocation.method != captured.method ||
        invocation.context.facts != captured.context.facts || invocation.context.mask != captured.context.mask ||
        invocation.context.operation != captured.context.operation || invocation.context.participant != captured.context.participant ||
        invocation.context.baseEpoch != captured.context.baseEpoch || invocation.context.scopeGeneration != captured.context.scopeGeneration ||
        invocation.context.runtimeEpoch != captured.context.runtimeEpoch || !invocation.value ||
        invocation.value.Schema()!=captured.value.Schema() || invocation.value.Encode() != wire_)
        return ScopeResult::Conflict;
    return ScopeResult::Ok;
}

std::shared_ptr<OwnerLifecycle::Record> OwnerLifecycle::Issue(const std::shared_ptr<PublicationOrigin>& origin,
    std::shared_ptr<PatchRegistry> registry, std::shared_ptr<runtime::RuntimeCommitDomain> domain) {
    if (!origin || !registry || !domain || &origin->scope_->stateDomain() != domain.get()) return {};
    // @summary Allocate every retained record object before reserving the writer.
    auto record = std::make_shared<Record>();
    auto stamp = std::make_shared<Stamp>(); stamp->current = origin; stamp->revision = 1;
    record->registry = std::move(registry); record->domain = std::move(domain);
    record->owner = origin->owner_; record->stamp = std::move(stamp);
    auto writer = record->domain->ReserveWriter();
    if (!writer.Acquired() || !origin->Current() || origin->jointBinding_ || origin->phase_ != PublicationOrigin::Phase::Active) return {};
    auto publication = record->registry->TryReservePublication();
    if (!publication.owns_lock()) return {};
    const auto sealed = record->registry->active_.find(record->owner);
    if (sealed == record->registry->active_.end() || sealed->second.incarnation != origin->incarnation_) return {};
    return record;
}

runtime::ScopeResult OwnerLifecycle::Inspect(const std::shared_ptr<Record>& record,
    std::shared_ptr<PublicationOrigin>& output, std::uint64_t& revision) {
    using runtime::ScopeResult;
    output.reset(); revision = 0;
    if (!record) return ScopeResult::Invalid;
    auto writer = record->domain->ReserveWriter();
    if (!writer.Acquired()) return ScopeResult::Busy;
    output = record->stamp->current; revision = record->stamp->revision;
    return record->stamp->cleanupPending ? ScopeResult::Indeterminate : ScopeResult::Ok;
}

std::shared_ptr<OwnerLifecycle::Binding> OwnerLifecycle::Bind(const std::shared_ptr<Record>& record) {
    if(!record)return {};
    auto binding=std::make_shared<Binding>();binding->fixture=record;binding->owner=record->owner;
    binding->domain=record->domain;binding->registry=record->registry.get();return binding;
}
OwnerLifecycle::Access::Access(const std::shared_ptr<Binding>& binding)
    : writer([&]() -> runtime::RuntimeCommitDomain::Writer {
        // @summary Lease member is already constructed; admit it before reserving D without moving its counted ownership.
        if(binding->state)lease.emplace(binding->state);
        return binding->domain->ReserveWriter();
    }()) {
    using runtime::ScopeResult;
    if(binding->state && (!lease || !*lease)){result=ScopeResult::Cancelled;return;}
    if(!writer.Acquired()){result=ScopeResult::Busy;return;}
    if(binding->state) {
        manager=std::unique_lock(binding->state->mutex,std::try_to_lock);
        if(!manager.owns_lock()){result=ScopeResult::Busy;return;}
        auto* owner=lease->Manager();if(owner->RuntimeStopping()){result=ScopeResult::Cancelled;return;}
        // @summary Existing lifecycle reservation spans legacy unlock/init/relock work; never seal its in-flight entry.
        if(owner->lifecycleBusy_.load()){result=ScopeResult::Busy;return;}
        const auto found=binding->state->records.find(binding->owner);
        if(found==binding->state->records.end() || found->second.participant.identity!=binding->identity ||
            found->second.installation!=binding->installation || found->second.fingerprint!=binding->fingerprint){result=ScopeResult::Stale;return;}
        actual=&found->second;participant=&actual->participant;
    } else participant=binding->fixture.get();
    result=participant ? ScopeResult::Ok : ScopeResult::Invalid;
}
runtime::ScopeResult OwnerLifecycle::Prepare(const std::shared_ptr<Record>& record,
    const std::shared_ptr<PublicationOrigin>& old,const std::shared_ptr<PublicationOrigin>& successor,
    const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
    const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
    Plan& output,std::string& error) {
    if(!record || (output.binding_ && output.binding_->state))return runtime::ScopeResult::Conflict;
    auto binding=Bind(record);const auto result=PrepareBound(binding,old,successor,oldDecision,newDecision,oldContext,newContext,output,error,{});
    if(result==runtime::ScopeResult::Ok)output.record_=record;
    return result;
}
runtime::ScopeResult OwnerLifecycle::PrepareJoint(scripting::ScriptScheduler& scheduler,const std::shared_ptr<Record>& record,
    const std::shared_ptr<PublicationOrigin>& old,const std::shared_ptr<PublicationOrigin>& successor,
    const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
    const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
    Plan& output,std::string& error) {
    if(!record || (output.binding_ && output.binding_->state) || (output.record_ && output.record_!=record))return runtime::ScopeResult::Conflict;
    auto binding=output.binding_ ? output.binding_ : Bind(record);
    const auto result=PrepareJointBound(scheduler,binding,old,successor,oldDecision,newDecision,oldContext,newContext,output,error,{});
    if(output.binding_)output.record_=record;
    return result;
}
runtime::ScopeResult OwnerLifecycle::CommitPrepared(const std::shared_ptr<Record>& record,Plan& plan,bool joint) {
    if(!record || !plan.binding_ || plan.binding_->state || plan.record_!=record)return runtime::ScopeResult::Conflict;
    return CommitBound(plan.binding_,plan,joint);
}
runtime::ScopeResult OwnerLifecycle::ValidatePreparationLocked(const std::shared_ptr<PublicationOrigin>& origin,
    PublicationOrigin::Phase expected) noexcept {
    using runtime::ScopeResult;if(!origin || !origin->preparation_)return ScopeResult::Ok;
    const auto& proof=*origin->preparation_;const auto& bridge=*origin->endpoint_->bridge;
    if((expected!=PublicationOrigin::Phase::Candidate && expected!=PublicationOrigin::Phase::Active) ||
        origin->phase_!=expected || origin->runtime_->preparation_!=(expected==PublicationOrigin::Phase::Candidate) ||
        !ManagerPreparation::OriginalBase(proof) || origin->definition_!=proof.loaded_ || origin->scope_!=proof.scope_ ||
        origin->endpoint_->scope!=proof.scope_ || &origin->scope_->stateDomain()!=proof.domain_.get() ||
        origin->endpoint_->ScriptGeneration()!=proof.vmGeneration_ ||
        origin->runtime_->preparationProof_!=proof.promotion_ ||
        origin->runtime_->vmGeneration_!=proof.vmGeneration_ || origin->endpoint_!=proof.endpoint_ ||
        bridge.closed_ || bridge.readyGeneration_!=proof.baseGeneration_ || bridge.epoch_!=proof.baseEpoch_ ||
        bridge.contexts_!=proof.contextSnapshot_.contexts || bridge.singlePlayerKnown_!=proof.contextSnapshot_.singlePlayerKnown)
        return ScopeResult::Stale;
    return ScopeResult::Ok;
}
runtime::ScopeResult OwnerLifecycle::PrepareBound(const std::shared_ptr<Binding>& record,
    const std::shared_ptr<PublicationOrigin>& old, const std::shared_ptr<PublicationOrigin>& successor,
    const runtime::OperationContext& oldDecision, const runtime::OperationContext& newDecision,
    const scripting::SemanticContextLease& oldContext, const scripting::SemanticContextLease& newContext,
    Plan& output, std::string& error,const PreparationSnapshot& original) {
    using runtime::ScopeResult;
    std::optional<ModManager::Lease> lifetime;
    if(record && record->state){lifetime.emplace(record->state);if(!*lifetime)return ScopeResult::Cancelled;}
    const bool activation=!old && successor;
    if(record && record->state && successor && (!original.stamp || !successor->preparation_ || !successor->preparation_->qualified_))return ScopeResult::Denied;
    if (output.binding_ || !record || (!old && !activation) || (old && (!old->Current() || old->owner_ != record->owner ||
        &old->scope_->stateDomain() != record->domain.get()))) return ScopeResult::Conflict;
    if (successor && (!successor->Current() || successor == old || (old && successor->scope_ == old->scope_) ||
        successor->owner_ != record->owner || (old && successor->incarnation_ == old->incarnation_) ||
        (old && successor->endpoint_->bridge != old->endpoint_->bridge) ||
        &successor->scope_->stateDomain() != record->domain.get())) return ScopeResult::Conflict;
    Plan candidate;candidate.expectedOrder_=record->expectedOrder;
    candidate.binding_ = record; candidate.old_ = old; candidate.successor_ = successor;
    candidate.oldDecision_ = oldDecision; candidate.newDecision_ = newDecision;
    candidate.oldContext_ = oldContext; candidate.newContext_ = newContext;
    candidate.purpose_ = activation ? Purpose::Activation : successor ? Purpose::Handoff : Purpose::Removal;
    {
        Access access(record); if(access.result!=ScopeResult::Ok)return access.result;
        auto* participant=access.participant;
        // @summary Canonical successors cannot refresh the original preparation pair; reject before any executable output is exposed.
        if(record->state && successor && (participant->stamp!=original.stamp || record->domain->Revision()!=original.domainRevision))return ScopeResult::Stale;
        candidate.expected_ = participant->stamp;
        candidate.domainRevision_ = record->domain->Revision();
        if(access.actual && access.lease->Manager()->enabledOrder_!=record->expectedOrder)return ScopeResult::Stale;
        const bool current = activation ? !candidate.expected_->current && (record->state ?
            participant->CanonicalEmpty() && access.actual && access.actual->state==ModState::Inactive && !access.actual->scriptRuntime && !access.actual->serviceSupervisor && candidate.expected_->tombstone!=successor->incarnation_ :
            candidate.expected_->tombstone==successor->incarnation_) : candidate.expected_->current == old;
        const bool tombstone = !successor && !candidate.expected_->current &&
            candidate.expected_->tombstone == old->incarnation_;
        if (candidate.expected_->cleanupPending || (!current && !tombstone) || (successor && ((!activation && old->phase_ != PublicationOrigin::Phase::Active) ||
            successor->phase_ != PublicationOrigin::Phase::Candidate)) || candidate.expected_->revision == UINT64_MAX)
            return ScopeResult::Conflict;
    }
    // @summary Budget copied owner input before building any publication; no effects or admission changes occur here.
    std::vector<ResourcePatchDefinition> patches;
    if (successor) {
        constexpr std::size_t maximum = 4096;
        if (!successor->scripted_ || successor->declarative_.size() > maximum ||
            successor->scripted_->size() > maximum - successor->declarative_.size()) return ScopeResult::Busy;
        std::size_t properties{};
        const auto bounded = [&](const auto& definitions) {
            for (const auto& patch : definitions) {
                if (patch.properties.size() > maximum - properties) return false;
                properties += patch.properties.size();
            }
            return true;
        };
        if (!bounded(successor->declarative_) || !bounded(*successor->scripted_)) return ScopeResult::Busy;
        patches = successor->declarative_;
        patches.insert(patches.end(), successor->scripted_->begin(), successor->scripted_->end());
    }
    candidate.next_ = std::make_shared<Stamp>();
    candidate.next_->current = successor;
    candidate.next_->tombstone = successor ? std::shared_ptr<const void>{} : old->incarnation_;
    candidate.next_->revision = candidate.expected_->revision + 1;
    candidate.failed_ = std::make_shared<Stamp>();
    candidate.failed_->tombstone = old ? old->incarnation_ : successor->incarnation_;
    candidate.failed_->revision = candidate.next_->revision;
    candidate.failed_->cleanupPending = true;
    if (activation) {
        if(!record->registry->PrepareOwned(PatchRegistry::ChangeKind::Activate,record->owner,patches,
            successor->incarnation_,candidate.publication_,error)) return ScopeResult::Conflict;
    } else if (!record->registry->PrepareMaintenance(record->owner, old->incarnation_,
        successor ? successor->incarnation_ : std::shared_ptr<const void>{}, patches,
        candidate.publication_, candidate.absent_, error)) return ScopeResult::Conflict;
    if(record->state) {
        candidate.nextOrder_=candidate.expectedOrder_;
        if(activation)candidate.nextOrder_.push_back(record->owner);
        if(!successor)std::erase(candidate.nextOrder_,record->owner);
        candidate.desiredOrder_=candidate.nextOrder_;
        if(successor) {
            candidate.projectionDefinition_=successor->preparation_->capture_->Definition();
            candidate.projectionPatches_=*successor->scripted_;
            candidate.projectionVm_=successor->runtime_;candidate.projectionScope_=successor->scope_;
        }
    }
    output = std::move(candidate); return ScopeResult::Ok;
}

runtime::ScopeResult OwnerLifecycle::Commit(const std::shared_ptr<Record>& record, Plan& plan) {
    return CommitPrepared(record,plan,false);
}
std::shared_ptr<OwnerLifecycle::Record> OwnerLifecycle::IssueCandidate(const std::shared_ptr<PublicationOrigin>& candidate,
    std::shared_ptr<PatchRegistry> registry,std::shared_ptr<runtime::RuntimeCommitDomain> domain) {
    if(!candidate || !registry || !domain || &candidate->scope_->stateDomain()!=domain.get()) return {};
    auto record=std::make_shared<Record>();record->registry=std::move(registry);record->domain=std::move(domain);
    record->owner=candidate->owner_;record->stamp=std::make_shared<Stamp>();
    record->stamp->tombstone=candidate->incarnation_;
    record->jointBinding=std::make_shared<const std::byte>();
    auto writer=record->domain->ReserveWriter();
    if(!writer.Acquired() || !candidate->Current() || candidate->jointBinding_ || candidate->phase_!=PublicationOrigin::Phase::Candidate) return {};
    auto publication=record->registry->TryReservePublication();
    if(!publication.owns_lock() || record->registry->active_.contains(record->owner)) return {};
    return record;
}
runtime::ScopeResult OwnerLifecycle::PrepareJointBound(scripting::ScriptScheduler& scheduler,const std::shared_ptr<Binding>& record,
    const std::shared_ptr<PublicationOrigin>& old,const std::shared_ptr<PublicationOrigin>& successor,
    const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
    const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
    Plan& output,std::string& error,const PreparationSnapshot& original) {
    using runtime::ScopeResult;
    std::optional<ModManager::Lease> lifetime;
    if(record && record->state){lifetime.emplace(record->state);if(!*lifetime)return ScopeResult::Cancelled;}
    if(!record || (!old && !successor)) return ScopeResult::Invalid;
    if(record->state && successor && (!original.stamp || !successor->preparation_ || !successor->preparation_->qualified_))return ScopeResult::Denied;
    const auto exact=[&](const std::shared_ptr<PublicationOrigin>& origin) {
        return !origin || (origin->Current() && origin->owner_==record->owner && &origin->scope_->stateDomain()==record->domain.get());
    };
    if(!exact(old) || !exact(successor) || (old && successor && (old==successor || old->scope_==successor->scope_ ||
        old->endpoint_->bridge!=successor->endpoint_->bridge))) return ScopeResult::Conflict;
    if(output.binding_ && (output.binding_!=record || !output.joint_ || output.old_!=old || output.successor_!=successor || output.consumed_ ||
        (record->state && successor && (output.expected_!=original.stamp || output.domainRevision_!=original.domainRevision)))) return ScopeResult::Invalid;
    std::shared_ptr<const void> observedBinding;
    { Access access(record);if(access.result!=ScopeResult::Ok)return access.result;
      if(record->state && successor && (access.participant->stamp!=original.stamp || record->domain->Revision()!=original.domainRevision))return ScopeResult::Stale;
      observedBinding=access.participant->jointBinding; }
    const auto enrollment=scripting::SchedulerPublicationReservation::ValidateEnrollment(scheduler,record->owner,
        old ? old->runtime_ : nullptr,successor ? successor->runtime_ : nullptr,record->domain,observedBinding,output.schedulerPrepared_,successor && successor->preparation_ ? successor->runtime_->preparationProof_ : std::shared_ptr<const void>{});
    if(enrollment!=ScopeResult::Ok)return enrollment;
    auto token=std::make_shared<const std::byte>();
    std::shared_ptr<const void> binding;
    {
        Access access(record);if(access.result!=ScopeResult::Ok)return access.result;auto* participant=access.participant;
        if(record->state && successor && (participant->stamp!=original.stamp || record->domain->Revision()!=original.domainRevision))return ScopeResult::Stale;
        const bool exactCurrent=old ? participant->stamp->current==old || (!successor && !participant->stamp->current && participant->stamp->tombstone==old->incarnation_) :
            !participant->stamp->current && (record->state ?
                participant->CanonicalEmpty() && access.actual && access.actual->state==ModState::Inactive && !access.actual->scriptRuntime && !access.actual->serviceSupervisor && participant->stamp->tombstone!=successor->incarnation_ :
                participant->stamp->tombstone==successor->incarnation_);
        if(!exactCurrent || participant->stamp->cleanupPending || (successor && successor->phase_!=PublicationOrigin::Phase::Candidate) ||
            (old && successor && old->phase_!=PublicationOrigin::Phase::Active)) return ScopeResult::Conflict;
        binding=participant->jointBinding ? participant->jointBinding : token;
        if((old && old->jointBinding_ && old->jointBinding_!=binding) ||
            (successor && successor->jointBinding_ && successor->jointBinding_!=binding)) return ScopeResult::Conflict;
    }
    // @summary Only successful scheduler preparation issues reclamation ownership; rejected foreign VMs remain caller-owned.
    auto outcome=ScopeResult::Ok;
    if(!output.binding_) {
        outcome=PrepareBound(record,old,successor,oldDecision,newDecision,oldContext,newContext,output,error,original);
        if(outcome!=ScopeResult::Ok) return outcome;
    }
    output.joint_=true;
    const auto purpose=!old ? scripting::SchedulerPublicationPurpose::Activate : successor ?
        scripting::SchedulerPublicationPurpose::Handoff : scripting::SchedulerPublicationPurpose::Remove;
    if(!output.schedulerPrepared_) {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        if(preparationBarrier)preparationBarrier();
#endif
        scripting::SchedulerPublicationPlan prepared;
        outcome=output.absent_ ? scripting::SchedulerPublicationReservation::PrepareRemoved(scheduler,record->owner,observedBinding,prepared) :
            scheduler.PreparePublication(record->owner,successor ? successor->runtime_ : nullptr,purpose,prepared,successor && successor->preparation_ ? successor->runtime_->preparationProof_ : std::shared_ptr<const void>{});
        if(outcome!=ScopeResult::Ok) {
            return outcome;
        }
        output.scheduler_=std::move(prepared);output.schedulerPrepared_=true;
    }
    if(output.scheduler_.domain_!=record->domain || output.scheduler_.old_!=(!output.absent_ && old ? old->runtime_ : nullptr) ||
        output.scheduler_.candidate_!=(successor ? successor->runtime_ : nullptr) ||
        output.scheduler_.domainRevision_!=output.domainRevision_) return ScopeResult::Conflict;
    Access access(record);if(access.result!=ScopeResult::Ok)return access.result;auto* participant=access.participant;
    scripting::SchedulerPublicationReservation reservation(output.scheduler_,output.scheduler_.jointBinding_);
    if(reservation.result_!=ScopeResult::Ok)return reservation.result_;
    if(participant->stamp!=output.expected_ || record->domain->Revision()!=output.domainRevision_ ||
        (participant->jointBinding && participant->jointBinding!=binding) ||
        (old && (!old->Current() || (old->jointBinding_ && old->jointBinding_!=binding))) ||
        (successor && (!successor->Current() || successor->phase_!=PublicationOrigin::Phase::Candidate ||
            (successor->jointBinding_ && successor->jointBinding_!=binding)))) return ScopeResult::Conflict;
    // @summary One metadata fence seals every actual participant together, without a publication revision or authority change.
    const auto bound=reservation.BindToken(binding);if(bound!=ScopeResult::Ok)return bound;
    if(!participant->jointBinding)participant->jointBinding=binding;
    if(old)old->jointBinding_=binding;
    if(successor)successor->jointBinding_=binding;
    return ScopeResult::Ok;
}
runtime::ScopeResult OwnerLifecycle::CommitBound(const std::shared_ptr<Binding>& record,Plan& plan,bool joint,ManagerCacheAction* staged,CommitEvidence* evidence) {
    using S=runtime::ScopeResult;if(evidence)*evidence={};
    if(!record||plan.binding_!=record||plan.consumed_||joint!=plan.joint_||(joint&&!plan.schedulerPrepared_))return S::Conflict;
    if(!record->state)return CommitReserved(record,plan,joint,nullptr,evidence);
    const ModManager::Lease lifetime(record->state);if(!lifetime)return S::Cancelled;
    ManagerCacheAction local;auto* action=staged?staged:&local;
    if(!staged){const auto admitted=ManagerCache::Prepare(lifetime,record,plan.publication_,local);if(admitted!=S::Ok)return admitted;}
    else if(!action->prepared_||action->revision_)return S::Invalid;
    try{auto result=CommitReserved(record,plan,joint,action,evidence);const auto cache=action->Finish();if(action->revision_&&cache!=S::Ok)result=S::Indeterminate;return result;}
    catch(...){if(action->revision_)return S::Indeterminate;throw;}
}
runtime::ScopeResult OwnerLifecycle::CommitReserved(const std::shared_ptr<Binding>& record,Plan& plan,bool joint,ManagerCacheAction* action,CommitEvidence* evidence) {
    using runtime::ScopeResult;if(evidence)*evidence={};
    if (!record || plan.binding_ != record || plan.consumed_ || joint!=plan.joint_ || (joint && !plan.schedulerPrepared_)) return ScopeResult::Conflict;
    Access access(record);if(access.result!=ScopeResult::Ok)return access.result;
    auto& writer=access.writer;auto* participant=access.participant;
    if(action&&!action->Matches(*participant))return ScopeResult::Conflict;
    if (!writer.CanAdvance()) return ScopeResult::NotReady;
    const bool activation=plan.purpose_==Purpose::Activation;
    if(access.actual && access.lease->Manager()->enabledOrder_!=plan.expectedOrder_)return ScopeResult::Stale;
    if ((!joint && (participant->jointBinding || activation)) || (joint && !participant->jointBinding) ||
        (plan.old_ && (plan.old_->jointBinding_!=participant->jointBinding || !plan.old_->Current())) ||
        (plan.successor_ && plan.successor_->jointBinding_!=participant->jointBinding) ||
        participant->stamp != plan.expected_ || record->domain->Revision() != plan.domainRevision_ ||
        (!activation && !plan.old_)) return ScopeResult::Conflict;
    std::optional<scripting::SchedulerPublicationReservation> scheduler;
    if(joint) {
        // @summary Stack construction has no allocation; friendship remains within this exact native coordinator.
        scheduler.emplace(plan.scheduler_,participant->jointBinding);
        if(scheduler->result_!=ScopeResult::Ok) return scheduler->result_;
    }
    auto publication = record->registry->TryReservePublication();
    if (!publication.owns_lock()) return ScopeResult::Busy;
    if (!publication.CanPublish(plan.publication_)) return ScopeResult::Conflict;
    const bool handoff = plan.purpose_ == Purpose::Handoff;
    const bool admitting=handoff || activation;
    if (admitting != static_cast<bool>(plan.successor_)) return ScopeResult::Invalid;
    auto* old = plan.old_.get();
    auto* next = plan.successor_.get();
    if (admitting && (!next->Current() || (!activation && old->phase_ != PublicationOrigin::Phase::Active) ||
        next->phase_ != PublicationOrigin::Phase::Candidate || plan.absent_)) return ScopeResult::Conflict;
    std::unique_lock<std::mutex> context;
    if (admitting) {
        auto& bridge = *next->endpoint_->bridge;
        context = std::unique_lock<std::mutex>(bridge.mutex_, std::try_to_lock);
        if (!context.owns_lock()) return ScopeResult::Busy;
        const auto oldFacts = old ? bridge.ValidateContextLocked(*old->endpoint_, plan.oldContext_) : ScopeResult::Ok;
        const auto newFacts = bridge.ValidateContextLocked(*next->endpoint_, plan.newContext_);
        if (oldFacts != ScopeResult::Ok) return oldFacts;
        if (newFacts != ScopeResult::Ok) return newFacts;
        const auto preparation=ValidatePreparationLocked(plan.successor_);
        if(preparation!=ScopeResult::Ok)return preparation;
    }
    auto* first = old ? old->scope_.get() : next->scope_.get(); auto* second = old && next ? next->scope_.get() : nullptr;
    if (second && std::less<runtime::RuntimeScope*>{}(second, first)) std::swap(first, second);
    runtime::RuntimeScope::LifecycleGuard firstGuard(*first);
    if (!firstGuard.Acquired()) return ScopeResult::Busy;
    std::optional<runtime::RuntimeScope::LifecycleGuard> secondGuard;
    if (second) secondGuard.emplace(*second);
    if (secondGuard && !secondGuard->Acquired()) return ScopeResult::Busy;
    auto* oldGuard = old ? (first == old->scope_.get() ? &firstGuard : &*secondGuard) : nullptr;
    auto* newGuard = next ? (first == next->scope_.get() ? &firstGuard : &*secondGuard) : nullptr;
    if ((oldGuard && !oldGuard->InDomain(*record->domain)) || (newGuard && !newGuard->InDomain(*record->domain)))
        return ScopeResult::Conflict;
    if (admitting) {
        const auto oldAuthority = oldGuard ? oldGuard->Check(plan.oldDecision_, false) : ScopeResult::Ok;
        const auto newAuthority = newGuard->Check(plan.newDecision_, true);
        if (oldAuthority != ScopeResult::Ok) return oldAuthority;
        if (newAuthority != ScopeResult::Ok) return newAuthority;
        if ((old && (plan.oldContext_.scopeGeneration != plan.oldDecision_.owner ||
            plan.oldContext_.runtimeEpoch != plan.oldDecision_.epoch)) ||
            plan.newContext_.scopeGeneration != plan.newDecision_.owner ||
            plan.newContext_.runtimeEpoch != plan.newDecision_.epoch) return ScopeResult::Stale;
    }
    if (plan.absent_ && !participant->stamp->current) {
        // @summary Exact repeated removal is a consumed no-change result, not another domain or registry publication.
        if(evidence)evidence->memory=MemoryDisposition::NoChangeConsumed;
        plan.consumed_ = true;if(scheduler) scheduler->Apply(); return ScopeResult::Ok;
    }
    scripting::SchedulerPublicationReservation::StatusReservation status(scheduler ? &*scheduler : nullptr);
    if(status.Result()!=ScopeResult::Ok) return status.Result();
    // @summary Last fallible gate. Everything below only swaps prebuilt state and mutates retained native scalars.
    const auto revision = writer.Advance();
    if (!revision) return ScopeResult::NotReady;
    if(evidence){evidence->crossed=true;evidence->revision=revision;evidence->memory=MemoryDisposition::MarkerCommitted;}
    if(action)action->Acknowledge(revision);
    plan.consumed_ = true;
    if(old) {oldGuard->Retire(); old->phase_ = PublicationOrigin::Phase::Retired;}
    // @summary CanPublish was checked under this exact still-held registry guard; no registry mutator runs before Publish.
    // The defensive impossible-failure branch exposes only a preallocated closed/pending stamp, never a live successor.
    if (!plan.absent_ && !publication.Publish(plan.publication_, revision)) {
        if(evidence)evidence->memory=MemoryDisposition::MarkerIndeterminate;
        if(scheduler) scheduler->CloseIndeterminate();
        participant->stamp.swap(plan.failed_); return ScopeResult::Indeterminate;
    }
    participant->stamp.swap(plan.next_);
    if (admitting) { next->phase_ = PublicationOrigin::Phase::Active; newGuard->Activate(); }
    if(scheduler) scheduler->Apply();
    if(access.actual) {
        auto& actual=*access.actual;
        actual.scriptRuntime.swap(plan.projectionVm_);actual.runtimeScope.swap(plan.projectionScope_);
        actual.scriptPatches.swap(plan.projectionPatches_);actual.diagnostic.swap(plan.projectionDiagnostic_);
        if(plan.projectionDefinition_)std::swap(actual.definition,*plan.projectionDefinition_);
        actual.state=admitting ? ModState::Active : ModState::Inactive;
        access.lease->Manager()->enabledOrder_.swap(plan.nextOrder_);
    }
    return ScopeResult::Ok;
}
runtime::ScopeResult OwnerLifecycle::ObserveBound(const std::shared_ptr<Binding>& record,scripting::ScriptScheduler& scheduler,Observation& output,const Plan* view) {
    using runtime::ScopeResult;output={};if(!record)return ScopeResult::Invalid;
    Access access(record);if(access.result!=ScopeResult::Ok)return access.result;auto* participant=access.participant;
    if(!participant->jointBinding)return ScopeResult::NotReady;
    std::unique_lock<std::mutex> schedulerGuard,vmGuard;
    const auto observed=scripting::SchedulerPublicationReservation::Inspect(scheduler,record->owner,participant->jointBinding,output.runtime,schedulerGuard,vmGuard);
    if(observed!=ScopeResult::Ok) return observed;
    auto registry=record->registry->TryReservePublication();if(!registry.owns_lock())return ScopeResult::Busy;
    std::optional<runtime::RuntimeScope::LifecycleGuard> scope;
    if(participant->stamp->current) {
        scope.emplace(*participant->stamp->current->scope_);if(!scope->Acquired())return ScopeResult::Busy;
    }
    output.origin=participant->stamp->current;output.recordRevision=participant->stamp->revision;
    output.registry=record->registry->Current();output.domainRevision=record->domain->Revision();
    if(scope)output.scope=scope->Snapshot();
    if(access.actual) {
        const auto& order=access.lease->Manager()->enabledOrder_;
        output.orderSize=order.size();output.ownerInOrder=std::find(order.begin(),order.end(),record->owner)!=order.end();
        if(view) {
            if(output.origin==view->old_)output.orderMatches=order==view->expectedOrder_;
            if(output.origin==view->successor_)output.orderMatches=order==view->desiredOrder_;
        }
        output.projectionMatches=access.actual->scriptRuntime==output.runtime &&
            access.actual->runtimeScope==(output.origin ? output.origin->scope_ : nullptr) &&
            access.actual->state==(output.origin ? ModState::Active : ModState::Inactive);
    }
    const auto status=scripting::SchedulerPublicationReservation::ReadRunning(output.runtime,output.running);
    if(status!=ScopeResult::Ok)return status;
    return participant->stamp->cleanupPending ? ScopeResult::Indeterminate : ScopeResult::Ok;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
void OwnerLifecycleTestAccess::PreparationBarrier(std::function<void()> barrier) {preparationBarrier=std::move(barrier);}
OwnerLifecycleTestAccess::Record OwnerLifecycleTestAccess::IssueCandidate(const std::shared_ptr<PublicationOrigin>& candidate,
    std::shared_ptr<PatchRegistry> registry,std::shared_ptr<runtime::RuntimeCommitDomain> domain) {
    return OwnerLifecycle::IssueCandidate(candidate,std::move(registry),std::move(domain));
}
runtime::ScopeResult OwnerLifecycleTestAccess::Joint(scripting::ScriptScheduler& scheduler,const Record& record,
    const std::shared_ptr<PublicationOrigin>& old,const std::shared_ptr<PublicationOrigin>& successor,
    const runtime::OperationContext& oldDecision,const runtime::OperationContext& newDecision,
    const scripting::SemanticContextLease& oldContext,const scripting::SemanticContextLease& newContext,
    OwnerLifecycle::Plan& output,std::string& error) {
    return OwnerLifecycle::PrepareJoint(scheduler,record,old,successor,oldDecision,newDecision,oldContext,newContext,output,error);
}
runtime::ScopeResult OwnerLifecycleTestAccess::CommitJoint(const Record& record,OwnerLifecycle::Plan& plan) {
    return OwnerLifecycle::CommitPrepared(record,plan,true);
}
runtime::ScopeResult OwnerLifecycleTestAccess::Reclaim(OwnerLifecycle::Plan& plan) {
    if(!plan.schedulerPrepared_)return runtime::ScopeResult::Ok;
    return scripting::SchedulerPublicationTestAccess::Reclaim(plan.scheduler_);
}
runtime::ScopeResult OwnerLifecycleTestAccess::Observe(const Record& record,scripting::ScriptScheduler& scheduler,JointObservation& output) {
    return OwnerLifecycle::ObserveBound(OwnerLifecycle::Bind(record),scheduler,output);
}
OwnerLifecycleTestAccess::Record OwnerLifecycleTestAccess::Issue(const std::shared_ptr<PublicationOrigin>& origin,
    std::shared_ptr<PatchRegistry> registry, std::shared_ptr<runtime::RuntimeCommitDomain> domain) {
    return OwnerLifecycle::Issue(origin, std::move(registry), std::move(domain));
}
runtime::ScopeResult OwnerLifecycleTestAccess::Remove(const Record& record,
    const std::shared_ptr<PublicationOrigin>& old, OwnerLifecycle::Plan& output, std::string& error) {
    return OwnerLifecycle::Prepare(record, old, {}, {}, {}, {}, {}, output, error);
}
runtime::ScopeResult OwnerLifecycleTestAccess::Handoff(const Record& record,
    const std::shared_ptr<PublicationOrigin>& old, const std::shared_ptr<PublicationOrigin>& successor,
    const runtime::OperationContext& oldDecision, const runtime::OperationContext& newDecision,
    const scripting::SemanticContextLease& oldContext, const scripting::SemanticContextLease& newContext,
    OwnerLifecycle::Plan& output, std::string& error) {
    return OwnerLifecycle::Prepare(record, old, successor, oldDecision, newDecision, oldContext, newContext, output, error);
}
runtime::ScopeResult OwnerLifecycleTestAccess::Commit(const Record& record, OwnerLifecycle::Plan& plan) {
    return OwnerLifecycle::Commit(record, plan);
}
runtime::ScopeResult OwnerLifecycleTestAccess::Inspect(const Record& record,
    std::shared_ptr<PublicationOrigin>& output, std::uint64_t& revision) {
    return OwnerLifecycle::Inspect(record, output, revision);
}
scripting::ScriptDefinition OwnedPublicationTestAccess::Snapshot(const PublicationOrigin& origin) {
    return *origin.definition_;
}
std::shared_ptr<PublicationOrigin> OwnedPublicationTestAccess::Issue(std::shared_ptr<scripting::ScriptRuntime> runtime,
    std::shared_ptr<scripting::SemanticEndpoint> endpoint, std::string fingerprint,
    std::vector<ResourcePatchDefinition> declarative, std::vector<ResourcePatchDefinition> scripted) {
    auto origin=std::shared_ptr<PublicationOrigin>(new PublicationOrigin(std::move(runtime),std::move(endpoint),std::move(fingerprint)));
    origin->declarative_=std::move(declarative);
    origin->scripted_=std::make_shared<const std::vector<ResourcePatchDefinition>>(std::move(scripted)); return origin;
}
bool OwnedPublicationTestAccess::Transition(PublicationOrigin& origin, PublicationOrigin::Phase phase) {
    if (phase == PublicationOrigin::Phase::Active && !origin.runtime_->Status().running) return false;
    auto writer = origin.scope_->ReserveWriter();
    if (!writer.Acquired() || origin.jointBinding_ || origin.phase_ == PublicationOrigin::Phase::Retired || phase == PublicationOrigin::Phase::Candidate)
        return false;
    if (phase == PublicationOrigin::Phase::Active) {
        const auto scope=origin.scope_->Current();
        if (!scope.activationCommitted || scope.stopping || origin.endpoint_->closed) return false;
    }
    origin.phase_ = phase; return true;
}
bool OwnedPublicationTestAccess::Prepare(const std::shared_ptr<PublicationOrigin>& origin, PatchRegistry& registry,
    const scripting::SemanticInvocation& invocation, const std::vector<scripting::ScriptPatchCommand>& commands,
    OwnedPublication& output, std::string& error) {
    if (!origin || invocation.origin != origin->endpoint_) {error="Foreign publication origin";return false;}
    OwnedPublication candidate;
    {
        auto writer=origin->scope_->ReserveWriter();
        if(!writer.Acquired() || origin->phase_==PublicationOrigin::Phase::Retired) {error="Unavailable owner baseline";return false;}
        candidate.baseline_=origin->scripted_;
    }
    if(!OwnedPublication::PreparePatches(*origin,registry,candidate.baseline_,commands,candidate.next_,candidate.plan_,error))return false;
    candidate.origin_=origin; candidate.invocation_=invocation; candidate.wire_=std::string(invocation.value.Encode());
    output=std::move(candidate); return true;
}
#endif
}
