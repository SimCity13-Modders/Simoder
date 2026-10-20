#include "bootstrap/runtime_context.hpp"
#include "mods/mod_manager.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "mod_fixture.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace sc13;
using namespace runtime;
using namespace bootstrap;
using namespace std::chrono_literals;
std::atomic_uint checks{},ownerSerial{};
std::atomic_bool expectationFailed{};
/** @summary Fail an actual behavioral expectation. @param value Observed result. @param message Diagnostic. */
void Check(bool value,const char* message){++checks;if(!value){expectationFailed=true;throw std::runtime_error(message);}}
/** @summary Create only an actual restricted TEST owner through the existing real acquisition algorithm.
 * @return Validated retained original owner. */
std::shared_ptr<platform::RuntimeOwner> Owner(){
    auto owner=platform::RuntimeOwnerTestAccess::Acquire(L"Global\\Simoder.TEST.U1.C1R."+
        std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++ownerSerial));
    Check(owner&&owner->IsAcquired(),"actual TEST owner acquisition");return owner;
}
/** @summary Retain the one original producer/writer until explicit cooperative release and real join. */
struct OriginalThread final {
    std::promise<void> entered,release;
    std::shared_future<void> signal=release.get_future().share();
    std::thread worker;
    std::exception_ptr failure;
    bool released{};
    /** @summary Start one original thread. @param function Actual original algorithm, not a simulated fence. */
    template<class F> explicit OriginalThread(F function){
        worker=std::thread([this,function=std::move(function)]{
            try{function(*this);}catch(...){failure=std::current_exception();try{entered.set_exception(failure);}catch(...){}}
        });
    }
    /** @summary Signal actual boundary then retain the same original thread until caller releases it. */
    void Pause(){entered.set_value();signal.wait();}
    /** @summary Require entry to the actual original boundary, retaining failed children for join. */
    void Wait(){auto ready=entered.get_future();Check(ready.wait_for(5s)==std::future_status::ready,"original boundary entry");ready.get();}
    /** @summary Release exactly once and join original producer; preserve its exception. */
    void Join(){Release();if(worker.joinable())worker.join();if(failure)std::rethrow_exception(failure);}
    /** @summary Release original barrier without replacing or canceling its thread. */
    void Release() noexcept {if(!released){released=true;try{release.set_value();}catch(...){}}}
    /** @summary Exception paths still release/join the same original participant. */
    ~OriginalThread(){Release();if(worker.joinable())worker.join();}
};

/** @summary Exercise the exact production preparation and retry algorithm on actual original objects. */
void Prepare(){
    bool rejected{};
    try {RuntimeContext invalid({});}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"missing owner rejected before composing original domain and publisher");
    RuntimeContext context(Owner());RootInitializationAttempt root;
    Check(root.Enter(false,nullptr).start,"first root selects actual startup");
    Check(!root.Enter(false,nullptr).start&&root.Enter(false,nullptr).result==ERROR_BUSY,"entered root is not false success");
    Check(root.Finish(context.Prepare())==ERROR_SUCCESS,"actual original preparation accepted");
    Check(context.Scope()->Current().activationCommitted,"actual original scope activation");
    Check((context.Scope()->Current().readiness&RuntimeReady)!=0,"actual original runtime readiness observed after setter");
    Check(context.Lifecycle()->Current().sequence==1,"one actual prepared event");
    for(int index=0;index<3;++index)Check(root.Enter(false,&context).result==ERROR_SUCCESS,"actual prepared retry");
    Check(context.Lifecycle()->Current().sequence==1,"no replayed publication on retry");
    RuntimeContext::CloseObservation close;
    Check(RetainRuntimeShutdown(context,nullptr,close)==ERROR_BUSY,"local close never finalizes root");
    Check(close.receiptStatus==LifecycleObservationStatus::Ok&&close.receipt.phase==LifecycleClosePhase::Closed,
        "actual original source close acknowledged");
    Check(close.scopeDrained&&close.managerScopesDrained,"independent original scope diagnostics");
    Check(context.Owner()->IsAcquired()&&context.Scope()&&context.Lifecycle(),"original context retained after Closed");
    Check(root.Enter(context.Domain()->IsStopping(),&context).result==ERROR_SHUTDOWN_IN_PROGRESS,"retired public root precedence");
    Check(context.Prepare()==ERROR_BUSY,"prepared helper revalidates actual stop");
    RuntimeContext unready(Owner());
    Check(unready.Prepare()==ERROR_SUCCESS,"fresh original scope can prepare before its readiness setter");
    unready.Scope()->SetReadiness(false);
    Check(unready.Prepare()==ERROR_BUSY&&unready.Scope()->Current().stopping,
        "repeated preparation rejects actual original readiness removal without republishing");
    Check(unready.Lifecycle()->Current().sequence==1,"readiness rejection never replays prepared publication");
}
/** @summary Earlier real startup errors cannot turn into successful Initialize on repetition. */
void RootFailures(){
    for(DWORD error:{10ul,11ul,12ul,13ul,14ul,15ul,16ul,static_cast<DWORD>(ERROR_BUSY)}){
        RootInitializationAttempt root;Check(root.Enter(false,nullptr).start,"one startup attempt");
        Check(root.Finish(error)==error,"first exact original error");
        Check(root.Enter(false,nullptr).result==error,"earlier no-context failure cached");
        Check(!root.Enter(false,nullptr).start,"failed startup never reentered");
        Check(root.Enter(true,nullptr).result==ERROR_SHUTDOWN_IN_PROGRESS,"actual retired routing precedence");
    }
    RuntimeContext context(Owner());context.Domain()->RequestStop();
    Check(context.Prepare()==ERROR_BUSY&&context.Prepare()==ERROR_BUSY,"actual activation rejection terminal");
    Check(context.Lifecycle()->Current().sequence==0,"failed activation publishes no prepared event");
}
/** @summary Same/non-top D rejection precedes L, scope operations, owner staging and output mutation. */
void Reentry(){
    RuntimeContext context(Owner());auto other=std::make_shared<RuntimeCommitDomain>();
    RuntimeContext::CloseObservation sentinel;sentinel.scopeDrained=true;sentinel.receipt.notificationFailed=true;
    {
        auto original=context.Domain()->ReserveWriter();Check(original.Acquired(),"actual original D");
        auto top=other->ReserveWriter();Check(top.Acquired(),"actual non-top original D");
        Check(context.Prepare()==ERROR_BUSY,"preparation rejects actual non-top D");
        Check(context.CloseRetained(sentinel)==ERROR_BUSY,"close rejects actual non-top D");
        Check(sentinel.scopeDrained&&sentinel.receipt.notificationFailed,"same-D output unchanged");
        Check(!context.Domain()->IsStopping(),"same-D rejection did not dispose or stop scope");
    }
    Check(context.Prepare()==ERROR_SUCCESS,"rejected entry did not consume once-attempt");
    RuntimeContext reentrant(Owner());DWORD nested=ERROR_SUCCESS;
    const auto token=reentrant.Lifecycle()->SubscribeCurrent([&](const auto& state){if(state.sequence==1)nested=reentrant.Prepare();});
    Check(token!=0&&reentrant.Prepare()==ERROR_SUCCESS&&nested==ERROR_BUSY,"actual Publishing recursion does not replay");
    Check(reentrant.Lifecycle()->Current().sequence==1&&reentrant.Lifecycle()->Unsubscribe(token),"original recursion event once");
    RuntimeContext stopped(Owner());
    const auto stopToken=stopped.Lifecycle()->SubscribeCurrent([&](const auto& state){
        if(state.sequence==1){RuntimeContext::CloseObservation close;Check(stopped.CloseRetained(close)==ERROR_BUSY,"reentrant actual close retained");}
    });
    Check(stopToken!=0&&stopped.Prepare()==ERROR_BUSY,"actual callback stop prevents false prepared success");
    Check(stopped.Scope()->Current().stopping&&stopped.Prepare()==ERROR_BUSY,"callback failure terminal");
    Check(stopped.Lifecycle()->Unsubscribe(stopToken),"original reentrant receiver disposed off guards");
}
/** @summary Foreign original D produces retained Pending and the same object closes only after original release. */
void ForeignWriter(){
    RuntimeContext context(Owner());Check(context.Prepare()==ERROR_SUCCESS,"prepare foreign writer fixture");
    OriginalThread writer([&](OriginalThread& thread){auto actual=context.Domain()->ReserveWriter();Check(actual.Acquired(),"foreign original writer acquired");thread.Pause();});
    writer.Wait();RuntimeContext::CloseObservation pending;
    Check(context.CloseRetained(pending)==ERROR_BUSY&&pending.receipt.phase==LifecycleClosePhase::Closing,"foreign original D leaves Pending");
    Check(context.Owner()->IsAcquired()&&context.Lifecycle()->Current().sequence==1,"pending retains original owner and historical event");
    writer.Join();RuntimeContext::CloseObservation closed;
    Check(context.CloseRetained(closed)==ERROR_BUSY&&closed.receipt.phase==LifecycleClosePhase::Closed,"same original close after actual D join");
    Check(closed.receipt.notificationPublished&&!closed.receipt.notificationFailed,"original pending stop finalized once");
    const auto sequence=context.Lifecycle()->Current().sequence;
    Check(context.CloseRetained(closed)==ERROR_BUSY&&context.Lifecycle()->Current().sequence==sequence,"repeated close cannot invent stop event");
}
/** @summary Actual synchronous delivery remains live despite Closed and locally drained core scope. */
void Delivery(){
    RuntimeContext context(Owner());std::atomic<OriginalThread*> original{};
    const auto token=context.Lifecycle()->SubscribeCurrent([&](const auto& state){if(state.sequence==1)original.load()->Pause();});
    Check(token!=0,"actual delivery subscription");DWORD prepared=ERROR_SUCCESS;
    OriginalThread publisher([&](OriginalThread& thread){original=&thread;prepared=context.Prepare();});
    publisher.Wait();RuntimeContext::CloseObservation close;
    Check(RetainRuntimeShutdown(context,nullptr,close)==ERROR_BUSY,"active delivery retained root route");
    Check(close.receipt.phase==LifecycleClosePhase::Closed&&close.scopeDrained,"Closed and scope drain are distinct from callback");
    Check(close.deliveryStatus==LifecycleObservationStatus::Ok&&close.delivery.delivering&&close.delivery.queued==1,"actual callback and original queued stop observed");
    Check(!context.Lifecycle()->Unsubscribe(token),"original active receiver still retained");
    publisher.Join();Check(prepared==ERROR_BUSY,"returned callback cannot restore stopped preparation");
    Check(context.Lifecycle()->Unsubscribe(token),"same original receiver disposed after actual return");
    Check(context.CloseRetained(close)==ERROR_BUSY&&!close.delivery.delivering&&close.delivery.queued==0,"idle still never authorizes global destruction");
}
/** @summary Full original bounded queue fails stop notification without fabricating a later replacement. */
void Capacity(){
    RuntimeContext context(Owner());unsigned stops{};RuntimeContext::CloseObservation atCapacity;
    const auto token=context.Lifecycle()->SubscribeCurrent([&](const auto& state){
        if(state.stopping)++stops;
        if(state.sequence!=1)return;
        for(unsigned index=0;index<128;++index)Check(context.Lifecycle()->Publish({0,true}),"actual full queue historical publication");
        Check(context.CloseRetained(atCapacity)==ERROR_BUSY,"actual full queue close retained");
    });
    Check(token!=0&&context.Prepare()==ERROR_BUSY,"full original queue prevents false prepared success");
    Check(atCapacity.receipt.phase==LifecycleClosePhase::Closed&&atCapacity.receipt.notificationFailed&&
        !atCapacity.receipt.notificationPublished,"Closed preserves failed stop notification");
    Check(atCapacity.delivery.delivering&&atCapacity.delivery.queued==128,"actual full original queue diagnostic");
    RuntimeContext::CloseObservation after;
    Check(context.CloseRetained(after)==ERROR_BUSY&&after.receipt.notificationFailed&&!after.receipt.notificationPublished,
        "empty queue never replaces original failed notification");
    Check(context.Lifecycle()->Current().sequence==129&&stops==0,"historical events preserved without invented stop");
    Check(context.Lifecycle()->Unsubscribe(token),"full-queue original receiver release");
}
/** @summary Actual allocation cuts retain originals and preserve first throwing preparation code15. */
void Allocation(){
    for(auto cut:{RuntimeLifecycleTestAccess::AllocationCut::Snapshot,RuntimeLifecycleTestAccess::AllocationCut::Event}){
        RuntimeContext context(Owner());RootInitializationAttempt root;
        Check(root.Enter(false,nullptr).start,"allocation root initial attempt");
        RuntimeLifecycleTestAccess::FailAllocation(cut);
        Check(root.Finish(context.Prepare())==15,"actual original allocation exception maps15");
        Check(context.Prepare()==15,"same helper failure code terminal");
        Check(root.Enter(context.Domain()->IsStopping(),&context).result==ERROR_SHUTDOWN_IN_PROGRESS,"retired root overrides cached exception");
        RuntimeContext::CloseObservation close;Check(context.CloseRetained(close)==ERROR_BUSY,"failed allocation close retained");
        Check(close.receipt.phase==LifecycleClosePhase::Closed&&close.receipt.notificationFailed&&!close.receipt.notificationPublished,
            "actual allocation failure does not invent notification");
        Check(context.Owner()->IsAcquired()&&context.Scope()->Current().stopping,"allocation retains actual owner and stopped scope");
    }
}
/** @summary Destructor witness uses original checked observer and verifies capture disposal outside D. */
struct Capture final {
    RuntimeContext* context{};bool* destroyed{};bool* safe{};
    /** @summary Original capture destruction may observe actual publisher only off guards. */
    ~Capture(){RuntimeLifecycle::DeliveryStatus delivery;*safe=!context->Domain()->WriterActiveOnCurrentThread()&&
        context->Lifecycle()->TryDeliveryStatus(delivery)==LifecycleObservationStatus::Ok;*destroyed=true;}
};
/** @summary Idle/Closed do not release a real subscription capture; only original successful unsubscribe does. */
void Captures(){
    RuntimeContext context(Owner());bool destroyed{},safe{};
    auto capture=std::make_shared<Capture>();capture->context=&context;capture->destroyed=&destroyed;capture->safe=&safe;
    std::weak_ptr<Capture> witness=capture;
    RuntimeLifecycle::Subscription token{};
    OriginalThread subscriber([&](OriginalThread& thread){
        RuntimeLifecycle::Callback receiver=[owned=std::move(capture),&thread](const auto& state){
            if(state.sequence==0)thread.Pause();
        };
        token=context.Lifecycle()->SubscribeCurrent(std::move(receiver));
    });
    subscriber.Wait();RuntimeContext::CloseObservation close;
    Check(!witness.expired()&&!destroyed,"original Subscribe producer retains actual owning capture");
    Check(context.CloseRetained(close)==ERROR_BUSY&&close.delivery.delivering,
        "active original Subscribe producer survives retained close");
    subscriber.Join();
    Check(token!=0&&!witness.expired()&&!destroyed,"original returned subscription still owns capture");
    Check(context.CloseRetained(close)==ERROR_BUSY&&close.receipt.phase==LifecycleClosePhase::Closed&&
        !close.delivery.delivering&&close.delivery.queued==0,"actual Closed idle diagnostic");
    Check(!witness.expired()&&!destroyed,"Closed idle is not capture disposal");
    Check(context.Lifecycle()->Unsubscribe(token)&&witness.expired()&&destroyed&&safe,"actual original unsubscribe disposes capture off guards");
    Check(context.Owner()->IsAcquired(),"borrowed context retained after capture disposal");
}
/** @summary An actual manager-owned callback Lease survives the same production retained root shutdown route.
 * @param root Isolated prequalified test writer directory; never a game directory. */
void ManagerLease(const std::filesystem::path& root){
    RuntimeContext context(Owner());Check(context.Prepare()==ERROR_SUCCESS,"manager lease context prepared");
    mods::PatchRegistry registry;RuntimeResourceCache cache;
    tests::WriteModFixture(root/"mods"/"one","fixture.owner","C1R retained manager",5);
    auto manager=std::make_unique<mods::ModManager>(root/"mods",root/"state.toon",registry,cache,
        mods::RuntimeResourceRefreshCallback{},scripting::ScriptScheduler::LogSink{},context.Owner(),context.Domain());
    std::string error;Check(manager->Initialize(error),"actual canonical manager initialization");
    using Access=mods::ManagerLifetimeTestAccess;
    auto anchor=Access::Capture(*manager,"fixture.owner");auto weak=Access::Anchor(anchor);auto host=Access::Host(*manager);
    OriginalThread borrower([&](OriginalThread& thread){
        Access::CallbackBarrier([&]{thread.Pause();});
        static_cast<void>(host.readFloat({1,2,3},4));Access::CallbackBarrier({});
    });
    borrower.Wait();RuntimeContext::CloseObservation close;
    Check(RetainRuntimeShutdown(context,manager.get(),close)==ERROR_BUSY,"actual leased manager root route never waits on its destructor");
    Check(manager&&registry.Current()&&!weak.expired()&&!Access::Closed(anchor),"original manager and borrowed services retained with active Lease");
    Check(close.receipt.phase==LifecycleClosePhase::Closed&&close.scopeDrained,"actual local close is not manager finalization");
    borrower.Join();Check(RetainRuntimeShutdown(context,manager.get(),close)==ERROR_BUSY,"completed Lease still does not fabricate E5");
    Check(manager&&!Access::Closed(anchor)&&!weak.expired(),"root route never entered actual CloseAndDrain");
    host={};anchor={};
    // @summary Fixture-only final disposal follows original borrower join and capture release, outside guards.
    manager.reset();Check(weak.expired(),"actual manager owner released only at fixture teardown");
}
}
/** @summary Run one exact isolated case per fresh process, preserving generated evidence.
 * @param argc Three: executable, exact mode, prequalified writer root. @param argv Fixed finite case inputs.
 * @return Zero only after all actual original participants joined and expectations held. */
int main(int argc,char** argv){
    try{
        Check(argc==3,"usage: bootstrap-runtime-context-tests <mode> <qualified-root>");
        const std::string mode=argv[1];const std::filesystem::path root=argv[2];
        Check(root.is_absolute()&&std::filesystem::is_directory(root),"prequalified original test writer root exists");
        if(mode=="prepare")Prepare();else if(mode=="root-failures")RootFailures();
        else if(mode=="reentry")Reentry();else if(mode=="foreign-writer")ForeignWriter();
        else if(mode=="delivery")Delivery();else if(mode=="capacity")Capacity();
        else if(mode=="allocation")Allocation();else if(mode=="capture")Captures();
        else if(mode=="manager-lease")ManagerLease(root);else Check(false,"unknown finite original case");
        Check(!expectationFailed.load(),"no callback expectation swallowed by original receiver isolation");
        std::cout<<"PASS "<<mode<<" checks="<<checks.load()<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
