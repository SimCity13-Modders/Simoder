#include "mods/manager_preparation.hpp"
#include "scripting/script_patch_reducer.hpp"
#include <stdexcept>
#include "mods/fixture_observation.hpp"

namespace sc13::mods {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
ManagerPreparation::Plan::~Plan(){fixture::Destroying(this,fixture::ObjectKind::Preparation);}
/** @summary Fixture-thread cut borrows the initialized actual VM only until the hook returns. */
thread_local std::function<void(const std::shared_ptr<scripting::ScriptRuntime>&)> beforePreparationBind;
#endif
ManagerPreparation::Plan& ManagerPreparation::Plan::operator=(Plan&& other) noexcept {
    if(this==&other)return *this;
    // @summary Retire the previous VM before releasing the exact host/capture lifetime it consumed.
    vm_.reset();
    qualified_=other.qualified_;promotion_=std::move(other.promotion_);state_=std::move(other.state_);identity_=std::move(other.identity_);stamp_=std::move(other.stamp_);
    capture_=std::move(other.capture_);base_=std::move(other.base_);endpoint_=std::move(other.endpoint_);scope_=std::move(other.scope_);
    fingerprint_=other.fingerprint_;owner_=std::move(other.owner_);baseEpoch_=other.baseEpoch_;baseGeneration_=other.baseGeneration_;
    vmGeneration_=other.vmGeneration_;
    domain_=std::move(other.domain_);domainRevision_=other.domainRevision_;scopeSnapshot_=other.scopeSnapshot_;
    contextSnapshot_=other.contextSnapshot_;declarations_=std::move(other.declarations_);loaded_=std::move(other.loaded_);vm_=std::move(other.vm_);
    return *this;
}
bool ManagerPreparation::OriginalBase(const Plan& plan) noexcept {
    return plan.capture_ && plan.base_ && plan.endpoint_ && plan.endpoint_->bridge==plan.base_->bridge_ &&
        plan.capture_->BasePin().version==plan.base_->pin_.version && plan.capture_->BasePin().files==plan.base_->pin_.files;
}
runtime::ScopeResult ManagerPreparation::Current(const Plan& plan) {
    using runtime::ScopeResult;
    if(!plan.state_ || !plan.capture_ || !plan.base_ || !plan.endpoint_ || !plan.scope_ || !plan.vm_)return ScopeResult::Invalid;
    const ModManager::Lease lease(plan.state_);if(!lease)return ScopeResult::Cancelled;
    auto* manager=lease.Manager();
    if(manager->RuntimeStopping())return ScopeResult::Cancelled;
    if(!plan.domain_ || plan.domain_->IsStopping() || plan.domain_->Revision()!=plan.domainRevision_)return ScopeResult::Stale;
    std::unique_lock lock(plan.state_->mutex,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
    const auto found=plan.state_->records.find(plan.owner_);
    std::shared_ptr<const void> identity,stamp;
    if(found==plan.state_->records.end() || !ModManager::PreparationRecord(found->second,identity,stamp,plan.qualified_) ||
        identity!=plan.identity_ || stamp!=plan.stamp_ || found->second.fingerprint!=plan.fingerprint_ ||
        !plan.capture_->SameInstallation(found->second.installation))return ScopeResult::Stale;
    lock.unlock();
    const auto& pin=plan.capture_->BasePin();
    if(pin.version!=plan.base_->pin_.version || pin.files!=plan.base_->pin_.files ||
        plan.endpoint_->closed || plan.endpoint_->scope!=plan.scope_ || plan.endpoint_->bridge!=plan.base_->bridge_ ||
        plan.endpoint_->bridge->Epoch()!=plan.baseEpoch_ || plan.base_->bridge_->ReadyGeneration()!=plan.baseGeneration_ ||
        !plan.baseGeneration_ || !plan.base_->Ready())return ScopeResult::Stale;
    {
        std::unique_lock vm(plan.vm_->mutex_,std::try_to_lock);if(!vm.owns_lock())return ScopeResult::Busy;
        if(plan.vm_->vmGeneration_!=plan.vmGeneration_ || plan.endpoint_->ScriptGeneration()!=plan.vmGeneration_ ||
            plan.vm_->host_.semantic!=plan.endpoint_ || !plan.vm_->preparation_ || plan.vm_->committed_ ||
            plan.vm_->sealed_ || !plan.vm_->running_ || plan.vm_->stopRequested_ || !plan.vm_->state_)return ScopeResult::Stale;
    }
    const auto scope=plan.scope_->Current();
    if(scope.generation!=plan.scopeSnapshot_.generation || scope.epoch!=plan.scopeSnapshot_.epoch ||
        scope.revision!=plan.scopeSnapshot_.revision || scope.stopping || scope.activationCommitted)return ScopeResult::Stale;
    const auto context=plan.endpoint_->bridge->CurrentPublication();
    if(context.contexts!=plan.contextSnapshot_.contexts || context.singlePlayerKnown!=plan.contextSnapshot_.singlePlayerKnown)
        return ScopeResult::Stale;
    return ScopeResult::Ok;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
std::vector<std::string> ManagerPreparation::StagedLogs(const Plan& plan) {
    const std::scoped_lock lock(plan.vm_->mutex_);return plan.vm_->logs_;
}
ManagerPreparation::Plan ManagerPreparation::Prepare(ModManager& manager,
    std::shared_ptr<const CapturedCandidate> capture,std::shared_ptr<scripting::BaseHost> base,
    std::function<void(scripting::ScriptPhase,std::uint64_t)> observer,bool qualified) {
    if(scripting::ScriptRuntime::OnScriptLane())throw std::runtime_error("preparation.lane");
    const ModManager::Lease lease(manager.state_);
    if(!lease || manager.RuntimeStopping() || !capture || !base)throw std::runtime_error("preparation.closed");
    if(manager.runtimeDomain_->WriterActiveOnCurrentThread())throw std::runtime_error("preparation.writer");
    const auto& definition=capture->Definition();
    if(!definition.script || definition.script->language!="luau" || definition.services || !definition.resources.empty())
        throw std::runtime_error("preparation.eligibility");
    if(capture->BasePin().version!=base->pin_.version || capture->BasePin().files!=base->pin_.files || !base->Ready())
        throw std::runtime_error("preparation.base");
    Plan plan;plan.qualified_=qualified;plan.state_=manager.state_;plan.owner_=definition.manifest.id;
    {
        std::unique_lock lock(manager.mutex_,std::try_to_lock);if(!lock.owns_lock())throw std::runtime_error("preparation.busy");
        const auto found=manager.records_.find(plan.owner_);
        if(found==manager.records_.end() || !ModManager::PreparationRecord(found->second,plan.identity_,plan.stamp_,qualified) ||
            found->second.definition.directory!=definition.directory || !capture->SameInstallation(found->second.installation))
            throw std::runtime_error("preparation.entry");
        plan.fingerprint_=found->second.fingerprint;
    }
    plan.capture_=std::move(capture);plan.base_=std::move(base);
    std::shared_ptr<runtime::RuntimeScope> scope=qualified ?
        std::shared_ptr<runtime::RuntimeScope>(runtime::RuntimeScopeTestAccess::Create(manager.runtimeDomain_)) :
        std::make_shared<runtime::RuntimeScope>(nullptr,manager.runtimeDomain_);plan.scope_=scope;
    auto host=plan.base_->Bind(scope);host.lifecycle=std::move(observer);
    plan.endpoint_=host.semantic;plan.baseEpoch_=plan.endpoint_->bridge->Epoch();
    plan.baseGeneration_=plan.endpoint_->bridge->ReadyGeneration();
    plan.domain_=manager.runtimeDomain_;plan.domainRevision_=plan.domain_->Revision();
    plan.scopeSnapshot_=scope->Current();plan.contextSnapshot_=plan.endpoint_->bridge->CurrentPublication();
    if(!plan.baseGeneration_)throw std::runtime_error("preparation.base-closed");
    plan.vm_=std::shared_ptr<scripting::ScriptRuntime>(new scripting::ScriptRuntime(scripting::ScriptRuntime::Preparation{},
        plan.owner_,*definition.script,std::move(host)));
    plan.vmGeneration_=plan.vm_->vmGeneration_;
    plan.loaded_=plan.vm_->loadedDefinition_;
    plan.declarations_=scripting::ReduceScriptPatches({},plan.vm_->TakePatchCommands());
    // @summary Preserve try-only contention as Busy; an unexposed rejected candidate never acquires publication authority.
    const auto current=Current(plan);
    if(current!=runtime::ScopeResult::Ok)throw std::runtime_error(current==runtime::ScopeResult::Busy?"preparation.busy":"preparation.stale");
    if(beforePreparationBind)beforePreparationBind(plan.vm_);
    const auto bound=manager.scriptScheduler_->BindPreparation(plan.vm_);
    if(bound!=runtime::ScopeResult::Ok)throw std::runtime_error(bound==runtime::ScopeResult::Busy?"preparation.busy":
        bound==runtime::ScopeResult::Cancelled?"preparation.closed":"preparation.stale");
    // @summary Keep original snapshots; a second observation rejects races without refreshing authority or retrying inside Core.
    const auto finalCurrent=Current(plan);
    if(finalCurrent!=runtime::ScopeResult::Ok)throw std::runtime_error(finalCurrent==runtime::ScopeResult::Busy?"preparation.busy":"preparation.stale");
    return plan;
}
void ManagerPreparationTestAccess::BeforeBind(std::function<void(const std::shared_ptr<scripting::ScriptRuntime>&)> callback){beforePreparationBind=std::move(callback);}
runtime::ScopeResult ManagerPreparationTestAccess::Bind(ModManager& manager,const std::shared_ptr<scripting::ScriptRuntime>& vm){return manager.scriptScheduler_->BindPreparation(vm);}
void ManagerPreparationTestAccess::HoldLane(ModManager& manager,const std::function<void()>& callback){manager.scriptScheduler_->HoldPreparationLane(callback);}
runtime::ScopeResult ManagerPreparationTestAccess::Binding(ModManager& manager,const std::shared_ptr<scripting::ScriptRuntime>& vm){return manager.scriptScheduler_->PreparationBinding(vm);}
std::shared_ptr<const CapturedCandidate> ManagerPreparationTestAccess::Capture(ModManager& manager,const ModId& id,
    const std::filesystem::path& directory,scripting::BasePackagePin pin) {
    const ModManager::Lease lease(manager.state_);if(!lease)throw std::runtime_error("preparation.closed");
    std::shared_ptr<const InstallationBinding> installation;
    {
        std::unique_lock lock(manager.mutex_,std::try_to_lock);if(!lock.owns_lock())throw std::runtime_error("preparation.busy");
        const auto found=manager.records_.find(id);
        if(found==manager.records_.end() || found->second.definition.directory!=std::filesystem::absolute(directory).lexically_normal())
            throw std::runtime_error("preparation.entry");
        installation=found->second.installation;
    }
    return CapturedCandidate::Capture(directory,std::move(installation),std::move(pin));
}
#endif
}
