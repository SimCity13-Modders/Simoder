#include "runtime/runtime_lifecycle.hpp"
#include "runtime/runtime_scope.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <type_traits>
#include <array>
#include <cstring>

using namespace sc13::runtime;
using namespace std::chrono_literals;
namespace {
std::atomic_bool fixtureFailure{};
/** @summary Fails an exact behavioral expectation. @param value Observed condition. @param message Failure diagnostic. */
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
/** @summary Retain/join the actual foreign lock holder; all capture destruction follows release of its lock. */
struct OriginalHold final {
    std::promise<void> entered,release;
    std::shared_future<void> signal=release.get_future().share();
    std::future<void> worker;
    /** @summary Acquire the actual original guard and pause without a replacement model.
     * @param body Own the real guard around the supplied bounded pause. */
    template<class F> explicit OriginalHold(F body){
        auto ready=entered.get_future();
        worker=std::async(std::launch::async,[this,body=std::move(body)]{
            try{
                const auto pause=[this]{entered.set_value();Require(signal.wait_for(10s)==std::future_status::ready,"original guard release timeout");};
                body(pause);
            }catch(...){fixtureFailure=true;try{entered.set_exception(std::current_exception());}catch(...){};}
        });
        if(ready.wait_for(5s)!=std::future_status::ready){release.set_value();worker.wait();Require(false,"original guard did not enter");}
        ready.get();
    }
    /** @summary Release and join the same original holder before disposing captures. */
    ~OriginalHold(){release.set_value();worker.wait();}
};
/** @summary Verifies current-before-events ordering, sticky stop and reentrant unsubscribe. */
void Ordering() {
    RuntimeLifecycle lifecycle;
    Require(lifecycle.Publish({0, true, false, false, false}), "runtime publication");
    std::vector<std::uint64_t> sequence;
    const auto first = lifecycle.SubscribeCurrent([&](const auto& state) { sequence.push_back(state.sequence); });
    Require(first != 0 && sequence == std::vector<std::uint64_t>{1}, "late current state missing");
    bool selfBusy{};
    RuntimeLifecycle::Subscription self{};
    self = lifecycle.SubscribeCurrent([&](const auto& state) {
        if (state.sequence == 2) selfBusy = !lifecycle.Unsubscribe(self);
    });
    Require(lifecycle.Publish({0, true, true, false, false}), "menu publication");
    Require(selfBusy && lifecycle.Unsubscribe(self), "self unsubscribe did not expose retained callback");
    Require(lifecycle.Publish({0, true, false, true, false}), "city publication");
    Require(sequence == std::vector<std::uint64_t>({1, 2, 3}), "ordered events violated");
    Require(lifecycle.Publish({0, false, false, false, true}), "stop publication");
    Require(lifecycle.Publish({0, true, true, true, false}) && lifecycle.Current().stopping, "stop unstuck");
    Require(lifecycle.Unsubscribe(first), "idle receiver not drained");
}
/** @summary Verifies concurrent unsubscribe closes admission and reports retained current callback. */
void ConcurrentUnsubscribe() {
    RuntimeLifecycle lifecycle;
    std::promise<void> entered, release;
    auto releaseSignal = release.get_future().share();
    std::atomic_int calls{};
    const auto id = lifecycle.SubscribeCurrent([&](const auto& state) {
        ++calls;
        if (state.sequence == 1) { entered.set_value(); Require(releaseSignal.wait_for(10s) == std::future_status::ready, "receiver release timeout"); }
    });
    auto writer = std::async(std::launch::async, [&] { return lifecycle.Publish({0, true}); });
    Require(entered.get_future().wait_for(5s) == std::future_status::ready, "receiver never entered");
    const bool firstBusy = !lifecycle.Unsubscribe(id);
    const bool stillBusy = !lifecycle.Unsubscribe(id);
    const bool queued = lifecycle.Publish({0, true, true});
    release.set_value();
    Require(writer.get(), "writer failed");
    Require(firstBusy && stillBusy && queued && lifecycle.Unsubscribe(id) && calls == 2, "unsubscribe admission/drain race");
}
/** @summary Verifies destruction retires queued delivery but retains reserved callbacks safely. */
void Destruction() {
    auto lifecycle = std::make_unique<RuntimeLifecycle>();
    std::promise<void> entered, release;
    auto releaseSignal = release.get_future().share();
    std::atomic_int calls{};
    lifecycle->SubscribeCurrent([&](const auto& state) {
        ++calls;
        if (state.sequence == 1) { entered.set_value(); Require(releaseSignal.wait_for(10s) == std::future_status::ready, "destruction release timeout"); }
    });
    RuntimeLifecycle* const borrowed = lifecycle.get();
    auto writer = std::async(std::launch::async, [&] { return borrowed->Publish({0, true}); });
    Require(entered.get_future().wait_for(5s) == std::future_status::ready, "destruction receiver never entered");
    Require(lifecycle->Publish({0, true, true}), "queued destruction event");
    lifecycle.reset();
    release.set_value();
    Require(writer.get() && calls == 2, "destruction delivered retired callback");
}
/** @summary Destruction of captured resources can reenter the same live publisher. */
struct ReentrantResource final {
    RuntimeLifecycle* lifecycle;
    bool* destroyed;
    /** @summary Proves capture destruction happens without the publisher state lock held. */
    ~ReentrantResource() { static_cast<void>(lifecycle->Current()); *destroyed = true; }
};
/** @summary Verifies receiver destruction and bounded overload paths avoid locks/reentrant deadlock. */
void ResourceAndBounds() {
    RuntimeLifecycle lifecycle;
    bool destroyed{};
    auto resource = std::make_shared<ReentrantResource>();
    resource->lifecycle = &lifecycle; resource->destroyed = &destroyed;
    const auto id = lifecycle.SubscribeCurrent([resource](const auto&) {});
    resource.reset();
    Require(lifecycle.Unsubscribe(id) && destroyed, "callback capture released under lock");
    std::vector<RuntimeLifecycle::Subscription> subscriptions;
    for (int index = 0; index != 64; ++index) subscriptions.push_back(lifecycle.SubscribeCurrent([](const auto&) {}));
    Require(lifecycle.SubscribeCurrent([](const auto&) {}) == 0, "subscription capacity exceeded");
    for (const auto subscription : subscriptions) Require(subscription != 0 && lifecycle.Unsubscribe(subscription), "bounded subscription cleanup");
    int rejected{};
    const auto flood = lifecycle.SubscribeCurrent([&](const auto& state) {
        if (state.sequence != 1) return;
        for (int index = 0; index != 130; ++index) if (!lifecycle.Publish({0, true})) ++rejected;
    });
    Require(lifecycle.Publish({0, true}) && rejected == 2, "event queue not bounded");
    Require(lifecycle.Unsubscribe(flood), "flood receiver cleanup");
    const auto throwing = lifecycle.SubscribeCurrent([](const auto&) { throw std::runtime_error("receiver failure"); });
    Require(throwing != 0 && lifecycle.Unsubscribe(throwing), "faulted callback not retired");
}
/** @summary Actual same-domain reentry is Pending; a stopped delivery lane restarts exactly once after checkpoint. */
void BoundCheckpoint(){
    using A=RuntimeLifecycleTestAccess;
    static_assert(!std::is_default_constructible_v<LifecycleAuthoritySource>);
    RuntimeLifecycle unbound;auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeCommitDomain foreign;
    Require(unbound.CloseStatus().phase==LifecycleClosePhase::Unbound&&!A::Source(unbound,*domain),"unbound factual publisher minted source");
    RuntimeLifecycle lifecycle(domain);auto source=A::Source(lifecycle,*domain);
    Require(source&&source->Phase()==LifecycleClosePhase::Open&&!A::Source(lifecycle,foreign),"source accepted foreign domain");
    std::vector<LifecycleSnapshot> events;
    lifecycle.SubscribeCurrent([&](const auto& event){events.push_back(event);});
    Require(lifecycle.Publish({0,true}),"bound original ready fact");
    const auto revision=domain->Revision();
    {
        OriginalHold holder([domain](const auto& pause){auto writer=domain->ReserveWriter();Require(writer.Acquired(),"actual D checkpoint fixture");pause();});
        Require(!lifecycle.Publish({0,false,false,false,true}),"same-D reentry falsely closed");
        const auto status=lifecycle.CloseStatus();const auto queue=A::Observe(lifecycle);
        Require(status.phase==LifecycleClosePhase::Closing&&!status.notificationPublished&&domain->IsStopping()&&
            !lifecycle.Current().stopping&&lifecycle.Current().sequence==1&&queue.queued==1&&queue.pending==1&&!queue.delivering,
            "pending close changed original Current or lost pending front");
        Require(!lifecycle.Publish({0,true})&&!lifecycle.SubscribeCurrent([](const auto&){}),"Closing admitted new facts/subscription");
        Require(!lifecycle.Publish({0,false,false,false,true})&&A::Observe(lifecycle).queued==1,"repeated pending stop duplicated event");
    }
    Require(lifecycle.Publish({0,false,false,false,true}),"cooperative checkpoint did not finalize original stop");
    Require(source->Phase()==LifecycleClosePhase::Closed&&lifecycle.CloseStatus().notificationPublished&&
        lifecycle.Current().stopping&&lifecycle.Current().sequence==2&&events.size()==3&&events.back().sequence==2,
        "actual close omitted or duplicated original stop publication");
    Require(!lifecycle.Publish({0,false,false,false,true})&&events.size()==3&&domain->Revision()==revision,
        "closed source published duplicate or advanced revision");
    {auto writer=domain->ReserveWriter();Require(writer.Acquired()&&!writer.CanAdvance()&&!writer.Advance(),"acknowledged source close allowed new marker");}
    const auto bytes=A::Observe(lifecycle);
    std::cout<<"lifecycle bytes state="<<bytes.stateBytes<<" event="<<bytes.eventBytes<<" source="<<bytes.sourceBytes<<" receipt="<<bytes.receiptBytes<<'\n';
}
/** @summary Finalizing while the original delivery lane is busy resumes that lane without duplicate or lost stop. */
void BoundActiveDelivery(){
    auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);
    std::promise<void> entered,release;const auto signal=release.get_future().share();std::atomic_int calls{},stops{};
    lifecycle.SubscribeCurrent([&](const auto& snapshot){++calls;if(snapshot.stopping)++stops;
        if(snapshot.sequence==1){entered.set_value();Require(signal.wait_for(10s)==std::future_status::ready,"bound delivery release timeout");}});
    auto original=std::async(std::launch::async,[&]{return lifecycle.Publish({0,true});});
    Require(entered.get_future().wait_for(5s)==std::future_status::ready,"actual original bound receiver did not enter");
    {
        OriginalHold holder([domain](const auto& pause){auto writer=domain->ReserveWriter();Require(writer.Acquired(),"active lane actual D");pause();});
        Require(!lifecycle.Publish({0,false,false,false,true})&&RuntimeLifecycleTestAccess::Observe(lifecycle).delivering,
            "pending close replaced active lane");
    }
    const bool finalized=lifecycle.Publish({0,false,false,false,true});
    const bool retained=stops==0&&calls==2&&RuntimeLifecycleTestAccess::Observe(lifecycle).queued==1;
    release.set_value();const bool accepted=original.get();
    Require(finalized&&retained&&accepted&&stops==1&&calls==3&&RuntimeLifecycleTestAccess::Observe(lifecycle).queued==0,
        "original active lane failed exactly-once pending delivery");
}
/** @summary Explicit stop against the full original queue preserves every accepted event and never invents a notification. */
void ExplicitStopFullQueue(){
    using A=RuntimeLifecycleTestAccess;
    auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);
    auto source=A::Source(lifecycle,*domain);
    std::promise<void> entered,release;const auto signal=release.get_future().share();
    std::atomic_int calls{},stops{};
    lifecycle.SubscribeCurrent([&](const auto& snapshot){
        ++calls;if(snapshot.stopping)++stops;
        if(snapshot.sequence==1){entered.set_value();Require(signal.wait_for(10s)==std::future_status::ready,"full queue original receiver release timeout");}
    });
    auto original=std::async(std::launch::async,[&]{return lifecycle.Publish({0,true});});
    Require(entered.get_future().wait_for(5s)==std::future_status::ready,"full queue original receiver did not enter");
    for(int index=0;index!=128;++index)Require(lifecycle.Publish({0,true,true}),"original full queue fill rejected accepted fact");
    const auto before=lifecycle.Current();const auto queued=A::Observe(lifecycle);const auto revision=domain->Revision();
    Require(before.sequence==129&&!before.stopping&&queued.queued==128&&queued.pending==0&&queued.delivering&&calls==2,
        "explicit stop did not start with full128 and original blocked receiver");
    {
        OriginalHold holder([domain](const auto& pause){auto writer=domain->ReserveWriter();Require(writer.Acquired(),"full queue original D holder");pause();});
        Require(!lifecycle.Publish({0,false,false,false,true}),"explicit stop falsely accepted a129th original event");
        const auto receipt=lifecycle.CloseStatus();const auto pending=A::Observe(lifecycle);const auto current=lifecycle.Current();
        Require(receipt.phase==LifecycleClosePhase::Closing&&receipt.reason==LifecycleCloseReason::Capacity&&
            !receipt.notificationPublished&&receipt.notificationFailed&&domain->IsStopping()&&
            pending.queued==128&&pending.pending==0&&pending.delivering&&current.sequence==before.sequence&&
            current.runtimePrepared==before.runtimePrepared&&current.menuObserved==before.menuObserved&&
            current.cityObserved==before.cityObserved&&current.stopping==before.stopping&&calls==2&&stops==0,
            "full explicit stop invented event/current/sequence or acknowledged before actual D");
        Require(!lifecycle.Publish({0,false,false,false,true})&&!lifecycle.Publish({0,true}),"full pending source accepted replacement publication");
        const auto repeated=lifecycle.CloseStatus();
        Require(repeated.phase==receipt.phase&&repeated.reason==receipt.reason&&repeated.notificationFailed&&
            !repeated.notificationPublished&&A::Observe(lifecycle).queued==128&&source->Phase()==LifecycleClosePhase::Closing,
            "full explicit stop retry replaced original close receipt or queued event");
    }
    Require(!lifecycle.Publish({0,false,false,false,true}),"full no-event close retry fabricated successful stop notification");
    const auto closed=lifecycle.CloseStatus();const auto retained=A::Observe(lifecycle);
    const bool exact=closed.phase==LifecycleClosePhase::Closed&&closed.reason==LifecycleCloseReason::Capacity&&
        closed.notificationFailed&&!closed.notificationPublished&&retained.queued==128&&retained.pending==0&&
        retained.delivering&&lifecycle.Current().sequence==before.sequence&&!lifecycle.Current().stopping&&
        domain->Revision()==revision&&source->Phase()==LifecycleClosePhase::Closed&&calls==2&&stops==0;
    release.set_value();const bool completed=original.get();
    const auto drained=A::Observe(lifecycle);
    Require(exact&&completed&&calls==130&&stops==0&&drained.queued==0&&!drained.delivering&&
        lifecycle.Current().sequence==before.sequence&&!lifecycle.Current().stopping,
        "full explicit stop lost historical wakeup or delivered fabricated stop callback");
    Require(!lifecycle.Publish({0,false,false,false,true})&&calls==130&&A::Observe(lifecycle).queued==0,
        "closed full-queue source admitted a late replacement stop");
}
/** @summary Original full queue, overflow and allocation errors close authority without inventing notification/current. */
void BoundFailures(){
    using A=RuntimeLifecycleTestAccess;
    {
        auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);int accepted{},rejected{},calls{};
        lifecycle.SubscribeCurrent([&](const auto& snapshot){++calls;if(snapshot.sequence!=1)return;
            for(int n=0;n!=128;++n)if(lifecycle.Publish({0,true}))++accepted;
            if(!lifecycle.Publish({0,true}))++rejected;});
        Require(lifecycle.Publish({0,true})&&accepted==128&&rejected==1&&calls==130,"original bounded full queue changed historical callbacks");
        Require(lifecycle.CloseStatus().phase==LifecycleClosePhase::Closed&&lifecycle.CloseStatus().notificationFailed&&
            lifecycle.Current().sequence==129&&!lifecycle.Current().stopping&&!lifecycle.Publish({0,false,false,false,true}),
            "queue failure fabricated stopped Current/event");
    }
    {
        auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);
        Require(A::ExhaustSequence(lifecycle),"original sequence exhaustion fixture rejected safe cut");const auto before=lifecycle.Current();
        Require(!lifecycle.Publish({0,false,false,false,true})&&lifecycle.Current().sequence==before.sequence&&
            !lifecycle.Current().stopping&&lifecycle.CloseStatus().phase==LifecycleClosePhase::Closed&&A::Observe(lifecycle).queued==0,
            "sequence wrapped or fabricated event");
    }
    for(const auto cut:{A::AllocationCut::Snapshot,A::AllocationCut::Event}){
        auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);int calls{};
        lifecycle.SubscribeCurrent([&](const auto&){++calls;});Require(lifecycle.Publish({0,true}),"allocation fixture original ready event");
        const auto before=lifecycle.Current();bool preserved{};
        {
            OriginalHold holder([domain](const auto& pause){auto writer=domain->ReserveWriter();Require(writer.Acquired(),"allocation original D");pause();});A::FailAllocation(cut);
            try{static_cast<void>(lifecycle.Publish({0,true,true}));}catch(const std::bad_alloc&){preserved=true;}
            Require(preserved&&calls==2&&lifecycle.Current().sequence==before.sequence&&lifecycle.Current().menuObserved==before.menuObserved&&
                A::Observe(lifecycle).queued==0&&lifecycle.CloseStatus().phase==LifecycleClosePhase::Closing,
                "allocation exception lost or half-published original event");
        }
        Require(!lifecycle.Publish({0,false,false,false,true})&&lifecycle.CloseStatus().phase==LifecycleClosePhase::Closed&&
            lifecycle.CloseStatus().notificationFailed&&calls==2&&!lifecycle.Current().stopping,"failed publication invented later stop event");
    }
    for(const auto cut:{A::AllocationCut::Receiver,A::AllocationCut::Subscription}){
        auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);bool threw{};
        A::FailAllocation(cut);
        try{static_cast<void>(lifecycle.SubscribeCurrent([](const auto&){}));}catch(const std::bad_alloc&){threw=true;}
        Require(threw&&lifecycle.CloseStatus().phase==LifecycleClosePhase::Closed&&lifecycle.Current().sequence==0&&A::Observe(lifecycle).queued==0,
            "subscription allocation did not preserve exception and closed source");
    }
    A::FailAllocation(A::AllocationCut::None);
}
/** @summary Abnormal destruction with actual D held cannot certify Closed; no source/publisher ownership cycle. */
void BoundDestruction(){
    using A=RuntimeLifecycleTestAccess;
    auto domain=std::make_shared<RuntimeCommitDomain>();auto lifecycle=std::make_unique<RuntimeLifecycle>(domain);
    auto source=A::Source(*lifecycle,*domain);Require(source!=nullptr,"actual source retained before destruction");
    {
        OriginalHold holder([domain](const auto& pause){auto writer=domain->ReserveWriter();Require(writer.Acquired(),"destructor original D");pause();});
        Require(!lifecycle->Publish({0,false,false,false,true}),"destructor fixture expected actual Pending");
        const auto current=lifecycle->Current();Require(current.sequence==0&&!current.stopping,"Pending falsely published before destruction");
        lifecycle.reset();Require(source->Phase()==LifecycleClosePhase::AbandonedPending&&domain->IsStopping(),"destructor pretended completed close");
    }
    Require(source->Phase()==LifecycleClosePhase::AbandonedPending,"abandoned cell reopened after domain release");
    std::weak_ptr<RuntimeCommitDomain> weak=domain;domain.reset();Require(weak.expired(),"retained source owns domain/publisher cycle");
    auto normalDomain=std::make_shared<RuntimeCommitDomain>();auto normal=std::make_unique<RuntimeLifecycle>(normalDomain);
    auto closed=A::Source(*normal,*normalDomain);normal.reset();Require(closed->Phase()==LifecycleClosePhase::Closed,"uncontended destructor missed real checkpoint");
}
/** @summary Checked same-D and non-top-D reads reject without touching even padding/output or requesting stop. */
void CheckedObservations(){
    using A=RuntimeLifecycleTestAccess;using Status=LifecycleObservationStatus;
    auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);RuntimeCommitDomain inner;
    auto source=A::Source(lifecycle,*domain);Require(lifecycle.Publish({0,true,true}),"checked original facts");
    LifecycleSnapshot snapshot{987,true,false,true,true};
    RuntimeLifecycle::CloseReceipt receipt{LifecycleClosePhase::AbandonedPending,LifecycleCloseReason::Invariant,true,true};
    std::array<unsigned char,sizeof(snapshot)> originalSnapshot{};std::array<unsigned char,sizeof(receipt)> originalReceipt{};
    std::memcpy(originalSnapshot.data(),&snapshot,sizeof(snapshot));std::memcpy(originalReceipt.data(),&receipt,sizeof(receipt));
    const auto reject=[&]{
        Require(lifecycle.TryCurrent(snapshot)==Status::Busy&&lifecycle.TryCloseStatus(receipt)==Status::Busy,"same original D observation did not reject");
        Require(std::memcmp(originalSnapshot.data(),&snapshot,sizeof(snapshot))==0&&std::memcmp(originalReceipt.data(),&receipt,sizeof(receipt))==0,
            "checked rejection changed original output bytes");
        Require(source->Phase()==LifecycleClosePhase::Open&&!A::Source(lifecycle,*domain),"checked query changed source or staged owner under D");
    };
    {auto outer=domain->ReserveWriter();Require(outer.Acquired(),"original D for checked rejection");reject();
        {auto nested=inner.ReserveWriter();Require(nested.Acquired(),"actual nested different D");reject();}}
    Require(lifecycle.Current().sequence==1&&A::Observe(lifecycle).queued==0,"checked rejection mutated actual factual state");
    {
        OriginalHold held([&](const auto& pause){auto lock=A::ReserveState(lifecycle);pause();});
        Require(lifecycle.TryCurrent(snapshot)==Status::Busy&&lifecycle.TryCloseStatus(receipt)==Status::Busy,"actual foreign L contention not Busy");
        Require(std::memcmp(originalSnapshot.data(),&snapshot,sizeof(snapshot))==0&&std::memcmp(originalReceipt.data(),&receipt,sizeof(receipt))==0,
            "contended original L changed output");
    }
    Require(lifecycle.TryCurrent(snapshot)==Status::Ok&&snapshot.sequence==1&&snapshot.runtimePrepared&&snapshot.menuObserved&&!snapshot.stopping,
        "off-D checked Current did not copy original facts");
    Require(lifecycle.TryCloseStatus(receipt)==Status::Ok&&receipt.phase==LifecycleClosePhase::Open&&receipt.reason==LifecycleCloseReason::None,
        "off-D coherent Open receipt");
    {auto writer=domain->ReserveWriter();Require(writer.Acquired()&&!lifecycle.Publish({0,false,false,false,true}),"same-D stop unexpectedly acknowledged");}
    Require(lifecycle.TryCloseStatus(receipt)==Status::Ok&&receipt.phase==LifecycleClosePhase::Closing&&receipt.reason==LifecycleCloseReason::ExplicitStop&&
        !receipt.notificationPublished&&!receipt.notificationFailed,"same-D intent fabricated notification");
    Require(!lifecycle.Publish({0,false,false,false,true})&&lifecycle.TryCloseStatus(receipt)==Status::Ok&&receipt.phase==LifecycleClosePhase::Closed&&
        !receipt.notificationPublished&&lifecycle.Current().sequence==1&&!lifecycle.Current().stopping,"no-event retry fabricated stopped Current");
    RuntimeLifecycle unbound;Require(unbound.TryCloseStatus(receipt)==Status::Ok&&receipt.phase==LifecycleClosePhase::Unbound,"unbound checked receipt fabricated source");
}
/** @summary Non-owning callback target for guarded negative-entry tests; it retains no captures. @param snapshot Ignored fact. */
void NoCapture(const LifecycleSnapshot& snapshot){static_cast<void>(snapshot);}
/** @summary Guarded mutation rejects without publishing an event; callbacks are staged with no owning captures. */
void ReentrantEntries(){
    using A=RuntimeLifecycleTestAccess;
    for(int mode=0;mode!=3;++mode){
        auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);const auto id=lifecycle.SubscribeCurrent(NoCapture);
        RuntimeLifecycle::Callback callback=NoCapture;
        {
            auto writer=domain->ReserveWriter();Require(writer.Acquired(),"actual guarded mutation D");
            if(mode==0)Require(!lifecycle.Publish({0,true}),"guarded ordinary Publish accepted");
            if(mode==1)Require(lifecycle.SubscribeCurrent(std::move(callback))==0,"guarded subscription accepted");
            if(mode==2)Require(!lifecycle.Unsubscribe(id),"guarded unsubscribe claimed drain");
        }
        Require(lifecycle.CloseStatus().phase==LifecycleClosePhase::Closing&&lifecycle.Current().sequence==0&&A::Observe(lifecycle).queued==0,
            "guarded entry acquired facts/event instead of intent");
        Require(!lifecycle.Publish({0,false,false,false,true})&&lifecycle.CloseStatus().phase==LifecycleClosePhase::Closed&&
            !lifecycle.CloseStatus().notificationPublished&&lifecycle.Unsubscribe(id),"original off-D retry/cleanup not honest");
    }
}
/** @summary Retained capture can diagnose its own off-D/off-L destruction against the original publisher. */
struct CheckedCapture final {
    RuntimeLifecycle* lifecycle{};std::shared_ptr<RuntimeCommitDomain> domain;bool* destroyed{};bool* outside{};
    /** @summary Last capture disposal happens while publisher is alive and original D/L are not held here. */
    ~CheckedCapture(){LifecycleSnapshot observed;*outside=!domain->WriterActiveOnCurrentThread()&&
        lifecycle->TryCurrent(observed)==LifecycleObservationStatus::Ok;*destroyed=true;}
};
/** @summary Owning callbacks are transferred/rejected/disposed only off guards, with real external retention when needed. */
void CaptureOwnership(){
    auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);bool destroyed{},outside{};
    auto capture=std::make_shared<CheckedCapture>();capture->lifecycle=&lifecycle;capture->domain=domain;capture->destroyed=&destroyed;capture->outside=&outside;
    auto id=lifecycle.SubscribeCurrent([capture](const auto&){});capture.reset();
    Require(lifecycle.Unsubscribe(id)&&destroyed&&outside,"accepted owning callback capture disposed under D/L");
    destroyed=false;outside=false;capture=std::make_shared<CheckedCapture>();capture->lifecycle=&lifecycle;capture->domain=domain;
    capture->destroyed=&destroyed;capture->outside=&outside;
    Require(lifecycle.Publish({0,false,false,false,true}),"capture rejection fixture actual close");
    RuntimeLifecycle::Callback callback=[capture](const auto&){};
    Require(lifecycle.SubscribeCurrent(std::move(callback))==0&&!destroyed,"rejection lost original external capture owner");
    capture.reset();Require(destroyed&&outside,"rejected capture final disposal was not off actual D/L");
}
/** @summary Same-D rejection preserves a concurrently accepted original pending stop and its first reason. */
void PendingReentry(){
    using A=RuntimeLifecycleTestAccess;
    auto domain=std::make_shared<RuntimeCommitDomain>();RuntimeLifecycle lifecycle(domain);std::atomic_int stops{};
    lifecycle.SubscribeCurrent([&](const auto& observed){if(observed.stopping)++stops;});
    auto source=A::Source(lifecycle,*domain);
    std::promise<void> entered,reenter,reentered,release;
    auto action=reenter.get_future().share();auto leave=release.get_future().share();
    auto original=std::async(std::launch::async,[&]{
        auto writer=domain->ReserveWriter();if(!writer.Acquired())return false;
        entered.set_value();if(action.wait_for(10s)!=std::future_status::ready)return false;
        const bool rejected=!lifecycle.Publish({0,true});reentered.set_value();
        return leave.wait_for(10s)==std::future_status::ready&&rejected;
    });
    const bool ready=entered.get_future().wait_for(5s)==std::future_status::ready;
    bool pending{},retained{};
    if(ready){pending=!lifecycle.Publish({0,false,false,false,true});reenter.set_value();
        const bool actual=reentered.get_future().wait_for(5s)==std::future_status::ready;
        if(actual){const auto observed=A::Observe(lifecycle);const auto cell=source->Observe();
            retained=observed.queued==1&&observed.pending==1&&cell.phase==LifecycleClosePhase::Closing&&
                cell.reason==LifecycleCloseReason::ExplicitStop&&!lifecycle.Current().stopping&&stops==0;}}
    else reenter.set_value();
    release.set_value();const bool completed=original.get();
    Require(ready&&pending&&retained&&completed,"same-D concurrent stop changed original pending event/reason");
    Require(lifecycle.Publish({0,false,false,false,true})&&stops==1&&lifecycle.Current().sequence==1&&A::Observe(lifecycle).queued==0,
        "original pending stop did not finalize once after same-D rejection");
}
}
/** @summary Executes isolated factual lifecycle ordering and callback-lifetime tests. */
int main() {
    try { Ordering(); ConcurrentUnsubscribe(); Destruction(); ResourceAndBounds(); BoundCheckpoint(); BoundActiveDelivery(); ExplicitStopFullQueue(); BoundFailures(); BoundDestruction(); CheckedObservations(); ReentrantEntries(); CaptureOwnership(); PendingReentry(); Require(!fixtureFailure,"original fixture holder failed"); std::cout << "Lifecycle ordering, bounded delivery and callback drain passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
