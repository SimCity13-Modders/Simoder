#include "bootstrap/runtime_context.hpp"
#include "bootstrap/selection_data_session.hpp"
#include "scripting/script_runtime.hpp"
#include "mods/mod_manager.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace sc13::bootstrap {
namespace {
/** @summary Reject missing original ownership before constructing any domain, scope or publisher.
 * @param owner Original acquired process owner. @return Same retained owner.
 * @throws std::invalid_argument The supplied owner is absent or not acquired.
 * @pre Caller constructs ownership dependencies outside all publication guards. */
std::shared_ptr<platform::RuntimeOwner> RequireAcquiredOwner(std::shared_ptr<platform::RuntimeOwner> owner) {
    if(!owner||!owner->IsAcquired())throw std::invalid_argument("Runtime context requires an acquired process owner");
    return owner;
}
}
RuntimeContext::RuntimeContext(std::shared_ptr<platform::RuntimeOwner> owner)
    :owner_(RequireAcquiredOwner(std::move(owner))),domain_(std::make_shared<runtime::RuntimeCommitDomain>()),
     scope_(std::make_shared<runtime::RuntimeScope>(owner_,domain_)),
     lifecycle_(std::make_shared<runtime::RuntimeLifecycle>(domain_)) {}

RuntimeContext::~RuntimeContext()=default;
platform::component_store::SelectionReadStatus RuntimeContext::CaptureSelectionData(std::string_view root,
    const platform::component_store::metadata::Identifier& installation,
    std::shared_ptr<const platform::component_store::NativeAdmission> admission,
    const platform::base_artifact::TrustedArtifact& trusted,const std::map<std::string,std::string>& pins,
    std::string_view version,const platform::component_store::metadata::Compatibility& compatibility,
    std::shared_ptr<const platform::component_store::CapturedSelectionData>& output)noexcept{
    using Status=platform::component_store::SelectionReadStatus;
    if(GetCurrentThreadId()!=originalThread_||selectionAttempted_||output||!admission||domain_->IsStopping()||
        runtime::RuntimeCommitDomain::AnyWriterActiveOnCurrentThread()||scripting::ScriptRuntime::OnScriptLane())return Status::Refused;
    selectionAttempted_=true;try{
        selectionData_.reset(new SelectionDataSession(owner_,domain_,scope_,std::move(admission),originalThread_));
        return selectionData_->Capture(root,installation,trusted,pins,version,compatibility,output);
    }catch(...){return Status::Refused;}
}

bool RuntimeContext::PreparedFacts(std::uint32_t requiredReadiness) const {
    runtime::LifecycleSnapshot current;
    runtime::RuntimeLifecycle::CloseReceipt receipt;
    if(domain_->IsStopping()||!owner_||!owner_->IsAcquired()||
       lifecycle_->TryCurrent(current)!=runtime::LifecycleObservationStatus::Ok||
       lifecycle_->TryCloseStatus(receipt)!=runtime::LifecycleObservationStatus::Ok||
       !current.runtimePrepared||current.stopping||receipt.phase!=runtime::LifecycleClosePhase::Open)return false;
    const auto scope=scope_->Current();
    return scope.activationCommitted&&!scope.stopping&&
        (scope.readiness&requiredReadiness)==requiredReadiness&&!domain_->IsStopping();
}
DWORD RuntimeContext::Fail(DWORD error) noexcept {
    attempt_=Attempt::Failed;failure_=error;
    RequestStopIntent(runtime::StopReason::ActivationFailed);return failure_;
}
DWORD RuntimeContext::Prepare() noexcept {
    // @summary Raw original D inspection precedes owner copies, L, formatting and scope disposal.
    if(domain_->WriterActiveOnCurrentThread())return ERROR_BUSY;
    if(attempt_==Attempt::Failed)return failure_;
    if(attempt_==Attempt::Publishing)return ERROR_BUSY;
    try {
        if(attempt_==Attempt::Prepared)return PreparedFacts(runtime::RuntimeReady)?ERROR_SUCCESS:Fail(ERROR_BUSY);
        attempt_=Attempt::Publishing;
        if(!scope_->CommitActivation())return Fail(ERROR_BUSY);
        if(!lifecycle_->Publish({0,true,false,false,false})||!PreparedFacts(0))return Fail(ERROR_BUSY);
        scope_->SetReadiness(true);
        if(!PreparedFacts(runtime::RuntimeReady))return Fail(ERROR_BUSY);
        attempt_=Attempt::Prepared;return ERROR_SUCCESS;
    }catch(...){return Fail(15);}
}
void RuntimeContext::RequestStopIntent(runtime::StopReason reason) noexcept {
    domain_->RequestStop();scope_->RequestStop(reason);
}
DWORD RuntimeContext::CloseRetained(CloseObservation& output) noexcept {
    if(domain_->WriterActiveOnCurrentThread())return ERROR_BUSY;
    CloseObservation observed;
    RequestStopIntent(runtime::StopReason::ExplicitShutdown);
    try {static_cast<void>(lifecycle_->Publish({0,false,false,false,true}));}
    catch(...){observed.publicationThrew=true;}
    observed.receiptStatus=lifecycle_->TryCloseStatus(observed.receipt);
    observed.deliveryStatus=lifecycle_->TryDeliveryStatus(observed.delivery);
    try {observed.scopeDrained=scope_->DrainFor(std::chrono::milliseconds(0));}
    catch(...){observed.scopeDrained=false;}
    output=observed;return ERROR_BUSY;
}
RootInitializationAttempt::Route RootInitializationAttempt::Enter(bool retired,RuntimeContext* context) noexcept {
    if(retired)return {false,ERROR_SHUTDOWN_IN_PROGRESS};
    if(!entered_){entered_=true;return {true,ERROR_BUSY};}
    if(!finished_)return {false,ERROR_BUSY};
    if(result_!=ERROR_SUCCESS)return {false,result_};
    return {false,context?context->Prepare():ERROR_BUSY};
}
DWORD RootInitializationAttempt::Finish(DWORD result) noexcept {
    if(!finished_){result_=result;finished_=true;}
    return result_;
}
DWORD RetainRuntimeShutdown(RuntimeContext& context,mods::ModManager* manager,
    RuntimeContext::CloseObservation& output) noexcept {
    if(context.Domain()->WriterActiveOnCurrentThread())return ERROR_BUSY;
    static_cast<void>(context.CloseRetained(output));
    if(manager){manager->RequestRuntimeStop();output.managerScopesDrained=manager->RuntimeScopesDrained();}
    else output.managerScopesDrained=true;
    // @summary Delivery idle and drained scopes cannot authorize destruction of manager leases/captures.
    return ERROR_BUSY;
}
}
