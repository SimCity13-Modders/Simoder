#include "runtime/runtime_lifecycle.hpp"
#include "runtime/runtime_scope.hpp"

#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace sc13::runtime {
namespace {
/** @summary Original bounded capacities include the single pending stop event. */
constexpr std::size_t MaxSubscriptions=64,MaxEvents=128;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Negative fixture cut has no production representation. */
thread_local RuntimeLifecycleTestAccess::AllocationCut allocationCut{};
/** @summary Fail an existing allocation exactly once. @param cut Original boundary. */
void AllocationBoundary(RuntimeLifecycleTestAccess::AllocationCut cut){
    if(allocationCut!=cut)return;
    allocationCut=RuntimeLifecycleTestAccess::AllocationCut::None;throw std::bad_alloc{};
}
#endif
}
/** @summary Actual publisher state; retained delivery never calls through a destroyed publisher. */
struct RuntimeLifecycle::State final {
    /** @summary Original receiver independently retained to callback completion. */
    struct Receiver final {Callback callback;bool active{true};std::size_t running{};};
    /** @summary Pending stop stays in this original event deque and receiver snapshot. */
    struct Event final {LifecycleSnapshot snapshot;std::vector<std::shared_ptr<Receiver>> receivers;bool pendingStop{};};
    /** @summary Notification state is independent from authority acknowledgement. */
    enum class Notification {None,Pending,Published,Failed};
    std::mutex mutex;
    LifecycleSnapshot current;
    std::map<Subscription,std::shared_ptr<Receiver>> subscriptions;
    std::deque<Event> events;
    Subscription next{1};
    bool delivering{},closed{};
    std::shared_ptr<RuntimeCommitDomain> domain;
    std::shared_ptr<LifecycleAuthoritySource> source;
    Notification notification{Notification::None};
};

RuntimeLifecycle::RuntimeLifecycle():RuntimeLifecycle(std::shared_ptr<RuntimeCommitDomain>{}){}
RuntimeLifecycle::RuntimeLifecycle(std::shared_ptr<RuntimeCommitDomain> domain):state_(std::make_shared<State>()){
    if(domain){
        state_->source=std::shared_ptr<LifecycleAuthoritySource>(new LifecycleAuthoritySource);
        state_->domain=std::move(domain);
        if(state_->domain->IsStopping()){
            RequestCloseIntent(*state_,LifecycleCloseReason::ExplicitStop);
        }
        static_cast<void>(AdvanceClose(state_));
    }
}
void RuntimeLifecycle::RequestCloseIntent(State& state,LifecycleCloseReason reason) noexcept{
    if(!state.source)return;
    state.source->Request(reason);
    state.domain->RequestStop();
}
void RuntimeLifecycle::AcknowledgeClose(const std::shared_ptr<State>& state) noexcept{
    if(!state->source)return;
    if(state->source->Phase()==LifecycleClosePhase::Closing){
        // @summary No L, callback, owner copy/disposal or receiver access under actual D.
        auto writer=state->domain->ReserveWriter();
        if(!writer.Acquired())return;
        state->source->Complete(LifecycleClosePhase::Closed);
    }
}
bool RuntimeLifecycle::AdvanceClose(const std::shared_ptr<State>& state) noexcept{
    AcknowledgeClose(state);
    if(!state->source)return false;
    if(state->source->Phase()!=LifecycleClosePhase::Closed)return false;
    const std::lock_guard lock(state->mutex);
    if(state->notification!=State::Notification::Pending)return false;
    State::Event* pending{};
    for(auto& event:state->events)if(event.pendingStop){
        if(pending){state->notification=State::Notification::Failed;return false;}pending=&event;
    }
    if(!pending||state->closed||state->current.sequence==std::numeric_limits<std::uint64_t>::max()){
        state->notification=State::Notification::Failed;return false;
    }
    pending->snapshot.sequence=state->current.sequence+1;pending->snapshot.stopping=true;
    state->current=pending->snapshot;pending->pendingStop=false;state->notification=State::Notification::Published;return true;
}
RuntimeLifecycle::~RuntimeLifecycle(){
    const auto state=state_;
    RequestCloseIntent(*state,LifecycleCloseReason::Destruction);
    // @summary Destruction cannot replace the normal owner's retained Pending retry or certify false drain.
    AcknowledgeClose(state);
    if(state->source){
        state->source->Complete(LifecycleClosePhase::AbandonedPending);
    }
    {
        const std::lock_guard lock(state->mutex);state->closed=true;
        for(const auto& [id,receiver]:state->subscriptions){(void)id;receiver->active=false;}
    }
    // @summary Extract only original bounded nodes/events; no allocating replacement deque in a destructor.
    for(std::size_t count=0;count<MaxSubscriptions;++count){
        decltype(state->subscriptions)::node_type retired;
        {const std::lock_guard lock(state->mutex);if(state->subscriptions.empty())break;retired=state->subscriptions.extract(state->subscriptions.begin());}
    }
    for(std::size_t count=0;count<MaxEvents;++count){
        State::Event cancelled;
        {const std::lock_guard lock(state->mutex);if(state->events.empty())break;cancelled=std::move(state->events.front());state->events.pop_front();}
    }
}
RuntimeLifecycle::Subscription RuntimeLifecycle::SubscribeCurrent(Callback callback){
    if(!callback)return 0;
    if(state_->domain&&state_->domain->WriterActiveOnCurrentThread()){
        RequestCloseIntent(*state_,LifecycleCloseReason::Invariant);return 0;
    }
    if(state_->source&&state_->source->Phase()!=LifecycleClosePhase::Open)return 0;
    const auto state=state_;
    std::shared_ptr<State::Receiver> receiver;State::Event event;
    std::exception_ptr failure;Subscription id{};
    try{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        AllocationBoundary(RuntimeLifecycleTestAccess::AllocationCut::Receiver);
#endif
        receiver=std::make_shared<State::Receiver>();receiver->callback=std::move(callback);event.receivers.push_back(receiver);
    }catch(...){failure=std::current_exception();}
    {
        const std::lock_guard lock(state->mutex);
        if(failure)RequestCloseIntent(*state,LifecycleCloseReason::Allocation);
        else if(!state->closed&&(!state->source||state->source->Phase()==LifecycleClosePhase::Open)&&state->subscriptions.size()<MaxSubscriptions){
            if(state->events.size()>=MaxEvents||state->next==std::numeric_limits<Subscription>::max())RequestCloseIntent(*state,LifecycleCloseReason::Capacity);
            else{
                event.snapshot=state->current;id=state->next;
                try{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
                    AllocationBoundary(RuntimeLifecycleTestAccess::AllocationCut::Event);
#endif
                    state->events.push_back(std::move(event));
                    try{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
                        AllocationBoundary(RuntimeLifecycleTestAccess::AllocationCut::Subscription);
#endif
                        state->subscriptions.emplace(id,receiver);
                    }
                    catch(...){event=std::move(state->events.back());state->events.pop_back();throw;}
                    ++state->next;
                }catch(...){id=0;failure=std::current_exception();RequestCloseIntent(*state,LifecycleCloseReason::Allocation);}
            }
        }
    }
    if(failure){static_cast<void>(AdvanceClose(state));std::rethrow_exception(failure);}
    static_cast<void>(AdvanceClose(state));Deliver(state);return id;
}
bool RuntimeLifecycle::Unsubscribe(Subscription subscription) noexcept{
    if(state_->domain&&state_->domain->WriterActiveOnCurrentThread()){
        RequestCloseIntent(*state_,LifecycleCloseReason::Invariant);return false;
    }
    const auto state=state_;std::shared_ptr<State::Receiver> retired;
    {
        const std::lock_guard lock(state->mutex);const auto found=state->subscriptions.find(subscription);
        if(found!=state->subscriptions.end()){
            found->second->active=false;if(found->second->running)return false;
            retired=std::move(found->second);state->subscriptions.erase(found);
        }
    }
    return true;
}
bool RuntimeLifecycle::Publish(LifecycleSnapshot observed){
    if(state_->domain&&state_->domain->WriterActiveOnCurrentThread()){
        RequestCloseIntent(*state_,observed.stopping?LifecycleCloseReason::ExplicitStop:LifecycleCloseReason::Invariant);return false;
    }
    const auto state=state_;State::Event event;std::exception_ptr failure;bool accepted{},continueClose{};
    {
        const std::lock_guard lock(state->mutex);if(state->closed)return false;
        if(state->source&&state->source->Phase()!=LifecycleClosePhase::Open)continueClose=observed.stopping;
        else if(state->events.size()>=MaxEvents||state->current.sequence==std::numeric_limits<std::uint64_t>::max()){
            RequestCloseIntent(*state,state->events.size()>=MaxEvents?LifecycleCloseReason::Capacity:LifecycleCloseReason::Sequence);continueClose=true;
            if(state->source)state->notification=State::Notification::Failed;
        }else{
            const bool pending=state->source&&observed.stopping;
            if(pending){RequestCloseIntent(*state,LifecycleCloseReason::ExplicitStop);continueClose=true;}
            observed.sequence=pending?0:state->current.sequence+1;observed.stopping=observed.stopping||state->current.stopping;
            event.snapshot=observed;event.pendingStop=pending;
            try{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
                AllocationBoundary(RuntimeLifecycleTestAccess::AllocationCut::Snapshot);
#endif
                event.receivers.reserve(state->subscriptions.size());
                for(const auto& [id,receiver]:state->subscriptions){(void)id;event.receivers.push_back(receiver);}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
                AllocationBoundary(RuntimeLifecycleTestAccess::AllocationCut::Event);
#endif
                state->events.push_back(std::move(event));
                if(pending)state->notification=State::Notification::Pending;else{state->current=observed;accepted=true;}
            }catch(...){
                failure=std::current_exception();RequestCloseIntent(*state,LifecycleCloseReason::Allocation);continueClose=true;
                if(state->source)state->notification=State::Notification::Failed;
            }
        }
    }
    const bool finalized=continueClose&&AdvanceClose(state);
    // @summary Failed publication has no event; preserve its original exception despite completed authority close.
    if(failure)std::rethrow_exception(failure);
    Deliver(state);return accepted||finalized;
}
LifecycleSnapshot RuntimeLifecycle::Current() const{const std::lock_guard lock(state_->mutex);return state_->current;}
RuntimeLifecycle::CloseReceipt RuntimeLifecycle::CloseStatus() const{
    const std::lock_guard lock(state_->mutex);return ReceiptLocked(*state_);
}
RuntimeLifecycle::CloseReceipt RuntimeLifecycle::ReceiptLocked(const State& state) noexcept{
    if(!state.source)return {};
    const auto observed=state.source->Observe();
    return {observed.phase,observed.reason,state.notification==State::Notification::Published,state.notification==State::Notification::Failed};
}
LifecycleObservationStatus RuntimeLifecycle::TryCurrent(LifecycleSnapshot& output) const noexcept{
    auto* const state=state_.get();
    if(state->domain&&state->domain->WriterActiveOnCurrentThread())return LifecycleObservationStatus::Busy;
    try{
        std::unique_lock lock(state->mutex,std::try_to_lock);
        if(!lock.owns_lock())return LifecycleObservationStatus::Busy;
        const auto observed=state->current;output=observed;return LifecycleObservationStatus::Ok;
    }catch(...){return LifecycleObservationStatus::Unavailable;}
}
LifecycleObservationStatus RuntimeLifecycle::TryCloseStatus(CloseReceipt& output) const noexcept{
    auto* const state=state_.get();
    if(state->domain&&state->domain->WriterActiveOnCurrentThread())return LifecycleObservationStatus::Busy;
    try{
        std::unique_lock lock(state->mutex,std::try_to_lock);
        if(!lock.owns_lock())return LifecycleObservationStatus::Busy;
        const auto observed=ReceiptLocked(*state);output=observed;return LifecycleObservationStatus::Ok;
    }catch(...){return LifecycleObservationStatus::Unavailable;}
}
LifecycleObservationStatus RuntimeLifecycle::TryDeliveryStatus(DeliveryStatus& output) const noexcept{
    auto* const state=state_.get();
    if(state->domain&&state->domain->WriterActiveOnCurrentThread())return LifecycleObservationStatus::Busy;
    try{
        std::unique_lock lock(state->mutex,std::try_to_lock);
        if(!lock.owns_lock())return LifecycleObservationStatus::Busy;
        const DeliveryStatus observed{state->events.size(),state->delivering};
        output=observed;return LifecycleObservationStatus::Ok;
    }catch(...){return LifecycleObservationStatus::Unavailable;}
}
std::shared_ptr<const LifecycleAuthoritySource> RuntimeLifecycle::StageAuthoritySource(const RuntimeCommitDomain& expected) const{
    if(state_->domain&&state_->domain->WriterActiveOnCurrentThread())return {};
    const auto state=state_;
    std::shared_ptr<const LifecycleAuthoritySource> source=state->source;
    const std::lock_guard lock(state->mutex);
    if(state->closed||state->domain.get()!=&expected||!source||source->Phase()!=LifecycleClosePhase::Open||state->domain->IsStopping())return {};
    return source;
}
void RuntimeLifecycle::Deliver(const std::shared_ptr<State>& state) noexcept{
    {const std::lock_guard lock(state->mutex);if(state->delivering)return;state->delivering=true;}
    for(;;){
        State::Event event;
        {
            const std::lock_guard lock(state->mutex);
            if(state->events.empty()||state->events.front().pendingStop){state->delivering=false;return;}
            event=std::move(state->events.front());state->events.pop_front();
        }
        for(const auto& receiver:event.receivers){
            {const std::lock_guard lock(state->mutex);if(state->closed||!receiver->active)continue;++receiver->running;}
            bool faulted{};try{receiver->callback(event.snapshot);}catch(...){faulted=true;}
            {
                const std::lock_guard lock(state->mutex);--receiver->running;if(faulted)receiver->active=false;
                if(!receiver->active)for(auto found=state->subscriptions.begin();found!=state->subscriptions.end();++found){
                    if(found->second==receiver){state->subscriptions.erase(found);break;}
                }
            }
        }
    }
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
void RuntimeLifecycleTestAccess::FailAllocation(AllocationCut cut) noexcept{allocationCut=cut;}
RuntimeLifecycleTestAccess::Observation RuntimeLifecycleTestAccess::Observe(const RuntimeLifecycle& lifecycle){
    const std::lock_guard lock(lifecycle.state_->mutex);Observation value;
    value.queued=lifecycle.state_->events.size();value.delivering=lifecycle.state_->delivering;
    value.stateBytes=sizeof(RuntimeLifecycle::State);value.eventBytes=sizeof(RuntimeLifecycle::State::Event);
    value.sourceBytes=sizeof(LifecycleAuthoritySource);value.receiptBytes=sizeof(RuntimeLifecycle::CloseReceipt);
    for(const auto& event:lifecycle.state_->events)if(event.pendingStop)++value.pending;
    return value;
}
std::shared_ptr<const LifecycleAuthoritySource> RuntimeLifecycleTestAccess::Source(const RuntimeLifecycle& lifecycle,const RuntimeCommitDomain& expected){return lifecycle.StageAuthoritySource(expected);}
bool RuntimeLifecycleTestAccess::ExhaustSequence(RuntimeLifecycle& lifecycle){
    const std::lock_guard lock(lifecycle.state_->mutex);auto& state=*lifecycle.state_;
    if(!state.source||state.source->Phase()!=LifecycleClosePhase::Open||!state.events.empty()||state.delivering)return false;
    state.current.sequence=std::numeric_limits<std::uint64_t>::max();return true;
}
std::unique_lock<std::mutex> RuntimeLifecycleTestAccess::ReserveState(RuntimeLifecycle& lifecycle){
    return std::unique_lock<std::mutex>(lifecycle.state_->mutex);
}
#endif
}
