#include "mods/manager_close.hpp"
#include "mods/owned_publication.hpp"
#include "scripting/semantic_bridge.hpp"
#include <utility>
#include <limits>
#include <exception>

namespace sc13::mods {
namespace {
/** @summary One try-only turn serializes scalar observations with explicit unbounded actions. */
struct RetirementTurn final {
    /** @summary Tries original native turn without waiting. @param flag Original State turn. */
    explicit RetirementTurn(std::atomic_bool& flag):flag_(flag){bool expected=false;owned=flag_.compare_exchange_strong(expected,true);}
    /** @summary Publishes completed native scalar state without reacquiring any publication gate. */
    ~RetirementTurn(){if(owned)flag_.store(false);}
    std::atomic_bool& flag_;
    bool owned{};
};
}
/** @summary Shared exact State pin and target holders for record and persistent-Plan retirement. */
struct ManagerClose::Action final {
    std::shared_ptr<ModManager::State> state;
    std::shared_ptr<scripting::ScriptSchedulerState> scheduler;
    std::shared_ptr<scripting::ScriptRuntime> vm;
    std::shared_ptr<PublicationOrigin> origin;
    std::shared_ptr<runtime::RuntimeScope> scope;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    std::shared_ptr<const std::function<void()>> barrier;
#endif
    PreparedRetirement* prepared{};
    std::size_t target{};
    std::uint64_t nonce{},sequence{};
    runtime::ScopeResult code{runtime::ScopeResult::NotReady};
    bool pinned{},closedNow{};
    /** @summary Drops every copied owner off guards, acknowledges exact phase, then releases native pin without gate/Q. */
    ~Action(){
        vm.reset();origin.reset();scope.reset();scheduler.reset();
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        barrier.reset();
#endif
        if(pinned){auto& progress=state->retirement;
            if(prepared){if(code==runtime::ScopeResult::Ok)prepared->targets_[target].completed=true;}
            else {progress.completed=code;progress.completedOwner=nonce;progress.completedSequence=sequence;}
            if(closedNow)++progress.retired;
            progress.pins.fetch_sub(1);progress.pins.notify_all();}
    }
};
runtime::ScopeResult ManagerClose::BeginAction(Action& action,std::unique_lock<std::mutex>& gate,std::unique_lock<std::mutex>& records) noexcept {
    using runtime::ScopeResult;const auto& state=action.state;
    gate=std::unique_lock(state->gate,std::try_to_lock);if(!gate.owns_lock())return ScopeResult::Busy;
    if(state->closeOwner.load()!=action.nonce || !state->closeRequested || state->closed || !state->manager)return ScopeResult::Denied;
    auto* manager=state->manager;if(manager->runtimeDomain_->WriterActiveOnCurrentThread())return ScopeResult::Denied;
    if(state->users || state->retirement.pins.load())return ScopeResult::NotReady;
    bool busy{};if(!manager->actionWorker_->TryCloseObservation(busy))return ScopeResult::Busy;if(busy)return ScopeResult::NotReady;
    const auto activity=manager->scriptScheduler_->ObserveCloseActivity();if(activity.result!=ScopeResult::Ok)return activity.result;
    records=std::unique_lock(state->mutex,std::try_to_lock);if(!records.owns_lock())return ScopeResult::Busy;
    action.scheduler=manager->scriptScheduler_->state_;return ScopeResult::Ok;
}
runtime::ScopeResult ManagerClose::PinAction(Action& action) noexcept {
    auto& progress=action.state->retirement;if(progress.sequence==UINT64_MAX)return runtime::ScopeResult::Denied;
    action.sequence=++progress.sequence;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    action.barrier=action.state->retirementBarrier;
#endif
    progress.pins.store(1);action.pinned=true;return runtime::ScopeResult::Ok;
}
runtime::ScopeResult ManagerClose::RunAction(Action& action) noexcept {
    using runtime::ScopeResult;
    try{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        if(action.barrier)(*action.barrier)();
#endif
        if(action.scope){
            const auto drained=action.scope->DrainRetirementOne();
            if(drained!=ScopeResult::Ok && drained!=ScopeResult::NotReady)return action.code=drained;
            const auto observed=action.scope->ObserveRetirement();
            if(observed.result==ScopeResult::Busy || observed.invariant || !observed.stopping || observed.running)
                return action.code=observed.invariant?ScopeResult::Indeterminate:(observed.result==ScopeResult::Busy?ScopeResult::Busy:ScopeResult::NotReady);
        }
        return action.code=scripting::ScriptScheduler::CloseCurrent(action.scheduler,action.vm,action.vm->vmGeneration_,action.closedNow);
    }catch(...){return action.code=ScopeResult::Indeterminate;}
}
ManagerClose::Token::Token(Token&& other) noexcept :state_(std::move(other.state_)),nonce_(std::exchange(other.nonce_,0)) {}
ManagerClose::Token& ManagerClose::Token::operator=(Token&& other) noexcept {
    if(this!=&other){Release();state_=std::move(other.state_);nonce_=std::exchange(other.nonce_,0);}return *this;
}
ManagerClose::Token::~Token(){Release();}
void ManagerClose::Token::Release() noexcept {
    if(state_ && nonce_){auto expected=nonce_;static_cast<void>(state_->closeOwner.compare_exchange_strong(expected,0));}
    nonce_=0;state_.reset();
}
runtime::ScopeResult ManagerClose::Request(const Handle& handle) noexcept {
    if(!handle.state_) return runtime::ScopeResult::Invalid;
    const auto& state=*handle.state_;
    handle.state_->closeRequested.store(true);
    if(state.schedulerClose)state.schedulerClose->store(true);
    if(state.actionClose)state.actionClose->store(true);
    return runtime::ScopeResult::Ok;
}
runtime::ScopeResult ManagerClose::Acquire(const Handle& handle,Token& token) noexcept {
    using runtime::ScopeResult;
    if(!handle.state_ || token.state_ || scripting::ScriptRuntime::OnScriptLane())return ScopeResult::Invalid;
    const auto& state=handle.state_;
    std::unique_lock lock(state->gate,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
    if(!state->closeRequested || state->closed || !state->manager)return ScopeResult::Denied;
    if(state->retirement.turn.load() || state->closeOwner.load())return ScopeResult::Busy;
    if(state->closeSequence==std::numeric_limits<std::uint64_t>::max())return ScopeResult::Denied;
    const auto nonce=++state->closeSequence;
    state->closeOwner.store(nonce);state->closeCursor=0;state->closeHolders=0;state->closeRevision=0;
    token.state_=state;token.nonce_=nonce;return ScopeResult::Ok;
}
ManagerClose::Observation ManagerClose::Poll(Token& token) noexcept {
    using runtime::ScopeResult;
    Observation result;
    if(!token.state_ || !token.nonce_ || scripting::ScriptRuntime::OnScriptLane())return result;
    const auto& state=token.state_;
    RetirementTurn turn(state->retirement.turn);if(!turn.owned){result.result=ScopeResult::Busy;result.actionInFlight=true;return result;}
    std::unique_lock lock(state->gate,std::try_to_lock);if(!lock.owns_lock()){result.result=ScopeResult::Busy;return result;}
    if(state->closeOwner.load()!=token.nonce_ || !state->closeRequested || state->closed || !state->manager){result.result=ScopeResult::Denied;return result;}
    result.generation=token.nonce_;result.admissionsClosed=true;result.phase=Phase::Draining;
    result.acknowledgements=state->foundationAcknowledgements;result.users=state->users;
    if(result.users){result.result=ScopeResult::NotReady;return result;}
    auto* manager=state->manager;
    if(!manager->actionWorker_->TryCloseObservation(result.actionBusy)){result.result=ScopeResult::Busy;return result;}
    if(result.actionBusy){result.result=ScopeResult::NotReady;return result;}
    {
        std::unique_lock records(state->mutex,std::try_to_lock);if(!records.owns_lock()){result.result=ScopeResult::Busy;return result;}
        JoinRetirement(*state,token.nonce_);result.retired=state->retirement.retired;result.recordScanComplete=state->retirement.complete;
    }
    const auto previousRevision=state->closeRevision;const auto previousCursor=state->closeCursor;
    const auto lane=manager->scriptScheduler_->PollClose(state->closeCursor,state->closeRevision);
    if(lane.result!=ScopeResult::Busy){if(previousRevision!=lane.revision || previousCursor>=128)state->closeHolders=0;state->closeHolders+=lane.holders;}
    result.result=lane.result;result.activityRevision=lane.revision;result.checked=lane.checked;
    result.cursor=state->closeCursor;result.holders=state->closeHolders;result.selected=lane.selected;result.queued=lane.queued;result.executing=lane.executing;
    if(lane.result==ScopeResult::Ok){result.phase=Phase::FoundationReady;state->foundationAcknowledgements=1;result.acknowledgements=1;}
    return result;
}
void ManagerClose::JoinRetirement(ModManager::State& state,std::uint64_t nonce) noexcept {
    auto& progress=state.retirement;
    if(!progress.initialized || progress.cursorOwner!=nonce){
        progress.cursor=state.records.begin();progress.initialized=true;progress.complete=progress.cursor==state.records.end();
        progress.cursorOwner=nonce;progress.cursorGeneration=0;progress.joinedSequence=progress.completedSequence;return;
    }
    if(progress.completedSequence==progress.joinedSequence)return;
    if(progress.completedOwner==nonce && progress.completed==runtime::ScopeResult::Ok && progress.cursor!=state.records.end() &&
        progress.cursor->second.scriptRuntime && progress.cursor->second.scriptRuntime->vmGeneration_==progress.cursorGeneration){
        ++progress.cursor;progress.complete=progress.cursor==state.records.end();
    }
    progress.joinedSequence=progress.completedSequence;
}
ManagerClose::RetirementObservation ManagerClose::DrainOne(Token& token) noexcept {
    using runtime::ScopeResult;
    RetirementObservation result;
    if(!token.state_ || !token.nonce_ || scripting::ScriptRuntime::OnScriptLane())return result;
    const auto state=token.state_;const auto nonce=token.nonce_;
    RetirementTurn turn(state->retirement.turn);
    if(!turn.owned){result.result=ScopeResult::Busy;result.actionInFlight=true;return result;}
    Action action;
    static_assert(sizeof(ModManager::State::Retirement)<=128);
    static_assert(sizeof(Action)<=256);
    result.metadataBytes=sizeof(ModManager::State::Retirement);result.holderBytes=sizeof(Action);
    action.state=state;action.nonce=nonce;
    try{
        std::unique_lock<std::mutex> gate,records;
        const auto admitted=BeginAction(action,gate,records);if(admitted!=ScopeResult::Ok){result.result=admitted;return result;}
        result.generation=nonce;result.retired=state->retirement.retired;
        JoinRetirement(*state,nonce);auto& progress=state->retirement;
        for(;progress.cursor!=state->records.end() && result.checked<32;){
            ++result.checked;auto& record=progress.cursor->second;
            if(!record.scriptRuntime){++progress.cursor;++result.skipped;continue;}
            const auto& current=record.participant.stamp->current;
            if(record.participant.jointBinding){
                if(!current || current->phase_!=PublicationOrigin::Phase::Active || current->runtime_!=record.scriptRuntime ||
                    current->scope_!=record.runtimeScope || !current->preparation_ || !current->Current()){
                    result.result=ScopeResult::Denied;result.pending=1;return result;
                }
                action.origin=current;action.scope=current->scope_;
            }else if(current){result.result=ScopeResult::Conflict;result.pending=1;return result;}
            else {
                // @summary An unsealed current record cannot borrow a semantic preparation's external dependency lifetime.
                if(record.scriptRuntime->host_.semantic){result.result=ScopeResult::Denied;result.pending=1;return result;}
            }
            action.vm=record.scriptRuntime;break;
        }
        progress.complete=progress.cursor==state->records.end();result.recordScanComplete=progress.complete;
        if(!action.vm){result.result=ScopeResult::NotReady;return result;}
        const auto pinned=PinAction(action);if(pinned!=ScopeResult::Ok){result.result=pinned;result.pending=1;return result;}
        progress.cursorGeneration=action.vm->vmGeneration_;
        result.vmGeneration=progress.cursorGeneration;result.completion=action.sequence;
        records.unlock();gate.unlock();
        RunAction(action);
        result.result=action.code;result.pending=action.code==ScopeResult::Ok?0:1;
        if(action.closedNow)++result.retired;
        return result;
    }catch(...){action.code=ScopeResult::Indeterminate;result.result=ScopeResult::Indeterminate;result.pending=1;return result;}
}
ManagerClose::PreparedRetirement::PreparedRetirement(PreparedRetirement&& other) noexcept { *this=std::move(other); }
ManagerClose::PreparedRetirement& ManagerClose::PreparedRetirement::operator=(PreparedRetirement&& other) noexcept {
    if(this==&other)return *this;
    RetirementTurn turn(busy_),source(other.busy_);if(!turn.owned || !source.owned)std::terminate();
    DestroyStorageLocked();state_=std::move(other.state_);targets_=other.targets_;generation_=other.generation_;count_=other.count_;
    if(auto* plan=std::get_if<ManagerPreparation::Plan>(&other.storage_))storage_.emplace<ManagerPreparation::Plan>(std::move(*plan));
    else if(auto* publication=std::get_if<ManagerPublication::Plan>(&other.storage_))storage_.emplace<ManagerPublication::Plan>(std::move(*publication));
    other.DestroyStorageLocked();return *this;
}
ManagerClose::PreparedRetirement::~PreparedRetirement(){Reset();}
void ManagerClose::PreparedRetirement::Reset() noexcept {
    RetirementTurn turn(busy_);if(!turn.owned)std::terminate();DestroyStorageLocked();
}
void ManagerClose::PreparedRetirement::DestroyStorageLocked() noexcept {
    auto state=std::move(state_);targets_={};generation_=0;count_=0;
    static_cast<void>(state);
    // @summary Native reentry sees Busy/invalid identity throughout whole original Plan and final State disposal.
    storage_.emplace<std::monostate>();
}
ManagerClose::PreparedObservation ManagerClose::ObservePrepared(const PreparedRetirement& holder) noexcept {
    PreparedObservation result;result.holderBytes=sizeof(PreparedRetirement);
    RetirementTurn turn(holder.busy_);if(!turn.owned){result.result=runtime::ScopeResult::Busy;result.actionInFlight=true;return result;}
    if(!holder.state_ || !holder.generation_)return result;
    result.generation=holder.generation_;result.targetCount=holder.count_;
    for(std::size_t i=0;i<holder.count_;++i)if(holder.targets_[i].completed)++result.completedTargets;
    result.result=result.completedTargets==result.targetCount?runtime::ScopeResult::Ok:runtime::ScopeResult::NotReady;return result;
}
runtime::ScopeResult ManagerClose::CapturePreparation(Token& token,ManagerPreparation::Plan&& plan,PreparedRetirement& output) noexcept {
    using runtime::ScopeResult;
    if(!token.state_ || !token.nonce_ || scripting::ScriptRuntime::OnScriptLane())return ScopeResult::Invalid;
    const auto state=token.state_;const auto nonce=token.nonce_;
    RetirementTurn turn(state->retirement.turn);if(!turn.owned)return ScopeResult::Busy;
    RetirementTurn holder(output.busy_);if(!holder.owned)return ScopeResult::Busy;
    if(output.generation_ || output.storage_.index()!=0)return ScopeResult::Invalid;
    Action action;action.state=state;action.nonce=nonce;
    std::unique_lock<std::mutex> gate,records;
    auto result=BeginAction(action,gate,records);if(result!=ScopeResult::Ok)return result;
    if(plan.state_!=state || !plan.vm_ || !plan.scope_ || !plan.endpoint_ || !ManagerPreparation::OriginalBase(plan) ||
        plan.endpoint_->scope!=plan.scope_ || plan.endpoint_->ScriptGeneration()!=plan.vmGeneration_)return ScopeResult::Denied;
    result=scripting::ScriptScheduler::InspectCloseTarget(action.scheduler,plan.vm_,plan.vmGeneration_,true);if(result!=ScopeResult::Ok)return result;
    if(state->retirement.sequence==UINT64_MAX)return ScopeResult::Denied;
    const auto generation=++state->retirement.sequence;
    const PreparedRetirement::Target target{plan.vm_.get(),plan.vmGeneration_,false};
    records.unlock();gate.unlock();
    output.state_=state;output.targets_[0]=target;output.count_=1;output.generation_=generation;
    output.storage_.emplace<ManagerPreparation::Plan>(std::move(plan));return ScopeResult::Ok;
}
runtime::ScopeResult ManagerClose::SelectPrepared(PreparedRetirement& holder,std::size_t index,Action& action) noexcept {
    using runtime::ScopeResult;if(index>=holder.count_)return ScopeResult::Invalid;
    const auto& target=holder.targets_[index];
    if(auto* plan=std::get_if<ManagerPreparation::Plan>(&holder.storage_)){
        if(plan->state_!=action.state || plan->vm_.get()!=target.vm || plan->vmGeneration_!=target.generation ||
            !ManagerPreparation::OriginalBase(*plan) || !plan->endpoint_ || plan->endpoint_->scope!=plan->scope_ ||
            plan->endpoint_->ScriptGeneration()!=target.generation)return ScopeResult::Denied;
        action.vm=plan->vm_;action.scope=plan->scope_;
        return scripting::ScriptScheduler::InspectCloseTarget(action.scheduler,action.vm,target.generation,true);
    }
    auto* publication=std::get_if<ManagerPublication::Plan>(&holder.storage_);if(!publication)return ScopeResult::Invalid;
    const auto& plan=publication->lifecycle_;
    if(!plan.binding_ || plan.binding_->state!=action.state || !plan.schedulerPrepared_)return ScopeResult::Denied;
    for(const auto& origin:{plan.old_,plan.successor_})if(origin && origin->runtime_.get()==target.vm && origin->generation_==target.generation){action.origin=origin;break;}
    if(!action.origin || !action.origin->preparation_ || !action.origin->Current() || !ManagerPreparation::OriginalBase(*action.origin->preparation_))return ScopeResult::Denied;
    const auto& scheduler=plan.scheduler_;
    const auto selected=plan.consumed_?scheduler.retired_:scheduler.candidate_;
    const bool defensive=plan.consumed_ && scheduler.candidate_==action.origin->runtime_ && scheduler.candidate_!=scheduler.retired_;
    if(selected!=action.origin->runtime_ && !defensive)return ScopeResult::Denied;
    const auto current=action.state->records.find(plan.binding_->owner);
    if(current!=action.state->records.end() && current->second.scriptRuntime==action.origin->runtime_)return ScopeResult::Denied;
    action.vm=action.origin->runtime_;action.scope=action.origin->scope_;
    {std::unique_lock lock(action.vm->mutex_,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
        if(action.vm->committed_ || action.vm->scheduled_.load())return ScopeResult::Denied;}
    return scripting::ScriptScheduler::InspectCloseTarget(action.scheduler,action.vm,target.generation,false);
}
runtime::ScopeResult ManagerClose::CapturePublication(Token& token,ManagerPublication::Plan&& publication,PreparedRetirement& output) noexcept {
    using runtime::ScopeResult;
    if(!token.state_ || !token.nonce_ || scripting::ScriptRuntime::OnScriptLane())return ScopeResult::Invalid;
    const auto state=token.state_;const auto nonce=token.nonce_;
    RetirementTurn turn(state->retirement.turn);if(!turn.owned)return ScopeResult::Busy;
    RetirementTurn holder(output.busy_);if(!holder.owned)return ScopeResult::Busy;
    if(output.generation_ || output.storage_.index()!=0)return ScopeResult::Invalid;
    Action action;action.state=state;action.nonce=nonce;
    std::unique_lock<std::mutex> gate,records;
    auto result=BeginAction(action,gate,records);if(result!=ScopeResult::Ok)return result;
    const auto& plan=publication.lifecycle_;
    if(!plan.binding_ || plan.binding_->state!=state || !plan.schedulerPrepared_ || !plan.joint_ ||
        plan.scheduler_.state_!=action.scheduler || plan.consumed_!=plan.scheduler_.consumed_)return ScopeResult::Denied;
    for(const auto& origin:{plan.old_,plan.successor_})if(origin && (!origin->preparation_ || !origin->Current() || !ManagerPreparation::OriginalBase(*origin->preparation_)))return ScopeResult::Denied;
    std::array<PreparedRetirement::Target,2> targets{};std::size_t count{};
    const auto add=[&](const std::shared_ptr<scripting::ScriptRuntime>& vm)->ScopeResult{
        if(!vm)return ScopeResult::Ok;
        for(std::size_t i=0;i<count;++i)if(targets[i].vm==vm.get() && targets[i].generation==vm->vmGeneration_)return ScopeResult::Ok;
        const auto origin=plan.old_ && plan.old_->runtime_==vm?plan.old_:plan.successor_ && plan.successor_->runtime_==vm?plan.successor_:nullptr;
        if(!origin || count==targets.size())return ScopeResult::Conflict;
        const auto current=state->records.find(plan.binding_->owner);
        if(current!=state->records.end() && current->second.scriptRuntime==vm)return ScopeResult::Denied;
        {std::unique_lock lock(vm->mutex_,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
            if(vm->committed_ || vm->scheduled_.load())return ScopeResult::Denied;}
        const auto qualified=scripting::ScriptScheduler::InspectCloseTarget(action.scheduler,vm,vm->vmGeneration_,false);if(qualified!=ScopeResult::Ok)return qualified;
        targets[count++]={vm.get(),vm->vmGeneration_,false};return ScopeResult::Ok;
    };
    result=add(plan.consumed_?plan.scheduler_.retired_:plan.scheduler_.candidate_);if(result!=ScopeResult::Ok)return result;
    if(plan.consumed_ && plan.scheduler_.candidate_ && plan.scheduler_.candidate_!=plan.scheduler_.retired_){
        const auto current=state->records.find(plan.binding_->owner);
        if(current==state->records.end() || current->second.scriptRuntime!=plan.scheduler_.candidate_){
            result=add(plan.scheduler_.candidate_);if(result!=ScopeResult::Ok)return result;
        }
    }
    if(state->retirement.sequence==UINT64_MAX)return ScopeResult::Denied;
    const auto generation=++state->retirement.sequence;records.unlock();gate.unlock();
    output.state_=state;output.targets_=targets;output.count_=count;output.generation_=generation;
    output.storage_.emplace<ManagerPublication::Plan>(std::move(publication));return ScopeResult::Ok;
}
ManagerClose::RetirementObservation ManagerClose::DrainPrepared(Token& token,PreparedRetirement& holder) noexcept {
    using runtime::ScopeResult;RetirementObservation result;result.holderBytes=sizeof(PreparedRetirement);
    static_assert(sizeof(PreparedRetirement)<=4096);
    if(!token.state_ || !token.nonce_ || scripting::ScriptRuntime::OnScriptLane())return result;
    const auto state=token.state_;const auto nonce=token.nonce_;
    RetirementTurn turn(state->retirement.turn);if(!turn.owned){result.result=ScopeResult::Busy;result.actionInFlight=true;return result;}
    RetirementTurn borrowed(holder.busy_);if(!borrowed.owned){result.result=ScopeResult::Busy;result.actionInFlight=true;return result;}
    if(holder.state_!=state || !holder.generation_){result.result=ScopeResult::Denied;return result;}
    Action action;action.state=state;action.nonce=nonce;action.prepared=&holder;
    std::unique_lock<std::mutex> gate,records;
    auto admitted=BeginAction(action,gate,records);if(admitted!=ScopeResult::Ok){result.result=admitted;return result;}
    result.generation=holder.generation_;result.retired=state->retirement.retired;
    std::size_t index{};for(;index<holder.count_ && holder.targets_[index].completed;++index)++result.skipped;
    if(index==holder.count_){result.result=ScopeResult::NotReady;return result;}
    action.target=index;result.checked=1;
    admitted=SelectPrepared(holder,index,action);if(admitted!=ScopeResult::Ok){result.result=admitted;result.pending=1;return result;}
    admitted=PinAction(action);if(admitted!=ScopeResult::Ok){result.result=admitted;result.pending=1;return result;}
    result.vmGeneration=holder.targets_[index].generation;result.completion=action.sequence;result.metadataBytes=sizeof(ModManager::State::Retirement);
    records.unlock();gate.unlock();RunAction(action);
    result.result=action.code;result.pending=action.code==ScopeResult::Ok?0:1;if(action.closedNow)++result.retired;return result;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
ManagerClose::Handle ManagerCloseTestAccess::Capture(ModManager& manager){ManagerClose::Handle handle;handle.state_=manager.state_;return handle;}
scripting::ScriptScheduler& ManagerCloseTestAccess::Scheduler(ModManager& manager){return *manager.scriptScheduler_;}
void ManagerCloseTestAccess::InvokeControl(const std::shared_ptr<scripting::ScriptRuntime>& vm,const std::function<void()>& callback){scripting::ScriptScheduler::InvokeCloseControl(vm,callback);}
scripting::SchedulerCloseObservation ManagerCloseTestAccess::Controls(const std::shared_ptr<scripting::ScriptRuntime>& vm){return scripting::ScriptScheduler::ObserveCloseControl(vm);}
void ManagerCloseTestAccess::ActionBarrier(ModManager& manager,std::function<void()> callback){
    auto barrier=callback?std::make_shared<const std::function<void()>>(std::move(callback)):nullptr;
    std::shared_ptr<const std::function<void()>> retired;
    {const std::scoped_lock lock(manager.actionWorker_->control_->mutex);
        retired.swap(manager.actionWorker_->control_->executionBarrier);
        manager.actionWorker_->control_->executionBarrier=std::move(barrier);}
}
std::weak_ptr<const void> ManagerCloseTestAccess::Anchor(const ManagerClose::Handle& handle){return handle.state_;}
std::shared_ptr<scripting::ScriptRuntime> ManagerCloseTestAccess::CurrentVM(ModManager& manager,const std::string& id){
    const std::scoped_lock lock(manager.mutex_);const auto found=manager.records_.find(id);return found==manager.records_.end()?nullptr:found->second.scriptRuntime;
}
std::shared_ptr<runtime::RuntimeScope> ManagerCloseTestAccess::CurrentScope(ModManager& manager,const std::string& id){
    const std::scoped_lock lock(manager.mutex_);const auto found=manager.records_.find(id);return found==manager.records_.end()?nullptr:found->second.runtimeScope;
}
void ManagerCloseTestAccess::Lifecycle(const std::shared_ptr<scripting::ScriptRuntime>& vm,std::function<void(scripting::ScriptPhase,std::uint64_t)> observer){
    std::function<void(scripting::ScriptPhase,std::uint64_t)> retired;
    {const std::scoped_lock lock(vm->mutex_);retired.swap(vm->host_.lifecycle);vm->host_.lifecycle.swap(observer);}
}
void ManagerCloseTestAccess::RetirementBarrier(ModManager& manager,std::function<void()> callback){
    auto barrier=callback?std::make_shared<const std::function<void()>>(std::move(callback)):nullptr;
    {const std::scoped_lock lock(manager.state_->gate);barrier.swap(manager.state_->retirementBarrier);}
}
void ManagerCloseTestAccess::HoldState(const ManagerClose::Handle& handle,const std::function<void()>& callback){const std::scoped_lock lock(handle.state_->gate);callback();}
bool ManagerCloseTestAccess::LaneReleased(const std::shared_ptr<scripting::ScriptRuntime>& vm){const std::scoped_lock lock(vm->controlMutex_);return vm->laneReleased_.load();}
#endif
}
