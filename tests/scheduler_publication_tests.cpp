#include "scripting/scheduler_publication.hpp"
#include <Luau/Compiler.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <cstdlib>
#include <new>

namespace {
std::atomic_size_t fenceAllocations{},fenceFrees{},fenceCallbacks{};
thread_local bool rejectPreparationAllocation{};
}
/** @summary Detects real host allocation during a typed reservation attempt without allocating instrumentation. */
void* operator new(std::size_t bytes) {
    if(sc13::scripting::SchedulerPublicationTestAccess::InsideFence()) ++fenceAllocations;
    if(rejectPreparationAllocation) throw std::bad_alloc();
    if(auto pointer=std::malloc(bytes?bytes:1)) return pointer;
    throw std::bad_alloc();
}
/** @summary Applies the identical allocation probe to arrays. */
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
/** @summary Detects final reclamation while publication guards are retained. */
void operator delete(void* pointer) noexcept {if(pointer && sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceFrees;std::free(pointer);}
/** @summary Matches array release instrumentation. */
void operator delete[](void* pointer) noexcept {::operator delete(pointer);}
/** @summary Matches compiler-emitted sized scalar release. */
void operator delete(void* pointer,std::size_t) noexcept {::operator delete(pointer);}
/** @summary Matches compiler-emitted sized array release. */
void operator delete[](void* pointer,std::size_t) noexcept {::operator delete(pointer);}

namespace {
using namespace sc13::scripting;
using sc13::runtime::ScopeResult;
using sc13::runtime::RuntimeCommitDomain;
using Access=SchedulerPublicationTestAccess;
using Purpose=SchedulerPublicationPurpose;
using namespace std::chrono_literals;
std::atomic_uint checks{};
/** @summary Checks a behavioral oracle rather than implementation wording. @param value Actual condition. @param detail Failure message. */
void Check(bool value,const char* detail) {++checks;if(!value) throw std::runtime_error(detail);}
/** @summary Compiles a bounded real isolated Luau VM with optional native lifecycle evidence.
 * @param owner Native fixture identity. @param host Native callbacks. @param source Synthetic Lua source. @return Actual initialized uncommitted VM. */
std::shared_ptr<ScriptRuntime> Vm(const std::string& owner,ScriptHost host={},const std::string& source="return {onTick=function() end,onDisable=function() end}") {
    ScriptDefinition definition;definition.entry="scripts/main.luau";definition.language="luau";
    definition.memoryBytes=2U*1024U*1024U;definition.budgetMs=20;definition.bytecode=Luau::compile(source);
    return std::make_shared<ScriptRuntime>(owner,definition,std::move(host));
}
/** @summary Holds a real dispatch boundary until the test releases it; times out without an unbounded hang. */
struct Barrier final {
    std::mutex mutex;std::condition_variable changed;bool entered{},released{};
    /** @summary Pauses once; subsequent visits pass without blocking. */
    void Pause() {
        std::unique_lock lock(mutex);if(entered) return;entered=true;changed.notify_all();
        if(!changed.wait_for(lock,5s,[&]{return released;})) throw std::runtime_error("Barrier release timeout");
    }
    /** @summary Confirms the actual lane reached this boundary. */
    void Wait() {std::unique_lock lock(mutex);Check(changed.wait_for(lock,5s,[&]{return entered;}),"Actual dispatch barrier not reached");}
    /** @summary Releases native dispatch outside every publication lock. */
    void Release() {const std::scoped_lock lock(mutex);released=true;changed.notify_all();}
};
/** @summary Checks typed initial activation, exact handoff, once-only retirement and legacy bypass denial. */
void Lifecycle() {
    auto domain=std::make_shared<RuntimeCommitDomain>();ScriptScheduler scheduler;
    Check(Access::Enroll(scheduler,domain)==ScopeResult::Ok,"Enroll failed");
    Check(Access::Enroll(scheduler,std::make_shared<RuntimeCommitDomain>())==ScopeResult::Denied,"Original domain replaced");
    std::atomic_uint disables{},closes{};ScriptHost host;
    host.lifecycle=[&](ScriptPhase phase,std::uint64_t){if(Access::InsideFence())++fenceCallbacks;if(phase==ScriptPhase::Disable)++disables;if(phase==ScriptPhase::Close)++closes;};
    auto old=Vm("mod",host);SchedulerPublicationPlan initial;
    Check(Access::Prepare(scheduler,"mod",old,Purpose::Activate,initial)==ScopeResult::Ok,"Initial prepare failed");
    Check(!old->Status().running && !Access::Current(scheduler,"mod"),"Prepared candidate became operational");
    bool rejected{};try{old->Commit();}catch(...){rejected=true;}Check(rejected,"Direct Commit bypassed typed route");
    Check(Access::Publish(initial)==ScopeResult::Ok && domain->Revision()==1,"Initial marker missing");
    Check(old->Status().running && Access::Current(scheduler,"mod")==old,"Published owner absent");
    Check(Access::Publish(initial)==ScopeResult::Invalid,"Consumed initial plan replayed");
    rejected=false;try{scheduler.Remove("mod");}catch(...){rejected=true;}Check(rejected,"Text Remove bypassed typed node");
    rejected=false;try{scheduler.Put("mod",old);}catch(...){rejected=true;}Check(rejected,"Text Put bypassed typed node");
    rejected=false;try{scheduler.ReleaseReservation("mod");}catch(...){rejected=true;}Check(rejected,"Text release bypassed typed node");
    auto next=Vm("mod",host);SchedulerPublicationPlan handoff;
    Check(Access::Prepare(scheduler,"mod",next,Purpose::Handoff,handoff)==ScopeResult::Ok,"Handoff prepare failed");
    Check(Access::Publish(handoff)==ScopeResult::Ok && domain->Revision()==2,"Handoff failed");
    Check(!old->Status().running && next->Status().running && Access::Current(scheduler,"mod")==next,"Handoff status incoherent");
    Check(!Access::Closed(old),"Retired VM slot released before deferred reclamation");
    Check(disables==0 && closes==0,"Publication entered Lua/close");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok && Access::Closed(old),"Retired owner not fully closed");
    Check(disables==1 && closes==1,"Retirement onDisable/Close count incorrect");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok && disables==1,"Retirement replayed onDisable");
    old->Tick(0.05);old->Stop();Check(disables==1,"Closed old owner reentered lifecycle");
    next->Fail("fixture native failure");Check(!next->Status().running,"Fail did not close typed gate");
    SchedulerPublicationPlan removal;Check(Access::Prepare(scheduler,"mod",{},Purpose::Remove,removal)==ScopeResult::Ok,"Failed old removal prepare denied");
    Check(Access::Publish(removal)==ScopeResult::Ok && !Access::Current(scheduler,"mod"),"Failed owner exact cleanup denied");
    Check(Access::Reclaim(removal)==ScopeResult::Ok && disables==2 && closes==2,"Fail skipped owed onDisable");
}
/** @summary Rejects identity conflicts, empty-node ABA, stale plans, stop and closed candidates without effects. */
void IdentityAndAbort() {
    auto domain=std::make_shared<RuntimeCommitDomain>();ScriptScheduler scheduler;Access::Enroll(scheduler,domain);
    std::atomic_uint abortDisable{},abortClose{};ScriptHost abortHost;
    abortHost.lifecycle=[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Disable)++abortDisable;if(phase==ScriptPhase::Close)++abortClose;};
    auto candidate=Vm("mod",abortHost);SchedulerPublicationPlan stale;
    Check(Access::Prepare(scheduler,"mod",candidate,Purpose::Activate,stale)==ScopeResult::Ok,"ABA setup failed");
    Check(Access::ReleaseEmpty(scheduler,"mod")==ScopeResult::Ok,"Empty release failed");
    auto replacement=Vm("mod");SchedulerPublicationPlan fresh;
    Check(Access::Prepare(scheduler,"mod",replacement,Purpose::Activate,fresh)==ScopeResult::Ok,"Fresh empty node failed");
    Check(Access::Publish(stale)==ScopeResult::Stale && domain->Revision()==0,"Empty-node ABA accepted");
    Check(Access::Reclaim(stale)==ScopeResult::Ok && Access::Closed(candidate),"Aborted candidate retained lane slot");
    Check(abortDisable==0 && abortClose==1,"Never-committed candidate ran onDisable or skipped Close");
    Check(Access::Publish(fresh)==ScopeResult::Ok,"Fresh initial node denied");
    auto a=Vm("mod"),b=Vm("mod");SchedulerPublicationPlan pa,pb;
    Check(Access::Prepare(scheduler,"mod",a,Purpose::Handoff,pa)==ScopeResult::Ok,"First handoff prepare denied");
    Check(Access::Prepare(scheduler,"mod",b,Purpose::Handoff,pb)==ScopeResult::Ok,"Second handoff prepare denied");
    Check(Access::Publish(pa)==ScopeResult::Ok,"First handoff denied");
    Check(Access::Publish(pb)==ScopeResult::Stale,"Stale native node plan accepted");
    Check(Access::Reclaim(pb)==ScopeResult::Ok && Access::Closed(b),"Stale candidate abort failed");
    Access::Reclaim(pa);
    auto bad=Vm("other");SchedulerPublicationPlan invalid;
    Check(Access::Prepare(scheduler,"mod",bad,Purpose::Handoff,invalid)==ScopeResult::Denied,"Foreign owner accepted");
    Check(Access::Prepare(scheduler,"mod",{},Purpose::Handoff,invalid)==ScopeResult::Invalid,"Purpose/candidate mismatch accepted");
    Check(Access::Prepare(scheduler,"mod",bad,static_cast<Purpose>(99),invalid)==ScopeResult::Invalid,"Unknown native purpose accepted");
    ScriptScheduler foreign;Access::Enroll(foreign,std::make_shared<RuntimeCommitDomain>());
    Check(Access::Prepare(foreign,"mod",a,Purpose::Activate,invalid)==ScopeResult::Denied,"Foreign scheduler/host accepted");
    auto stopped=Vm("mod");SchedulerPublicationPlan stoppedPlan;
    Check(Access::Prepare(scheduler,"mod",stopped,Purpose::Handoff,stoppedPlan)==ScopeResult::Ok,"Stop setup failed");
    stopped->Stop();Check(Access::Publish(stoppedPlan)==ScopeResult::Denied,"Stopped candidate activated");
    Check(Access::Current(scheduler,"mod")==a,"Candidate failure replaced old owner");Access::Reclaim(stoppedPlan);
    SchedulerPublicationPlan removal;Check(Access::Prepare(scheduler,"mod",{},Purpose::Remove,removal)==ScopeResult::Ok,"Domain stop prepare failed");
    const auto revision=domain->Revision();domain->RequestStop();
    Check(Access::Publish(removal)==ScopeResult::Cancelled && domain->Revision()==revision,"Domain stop bypassed by removal");
}
/** @summary Exercises the actual lane selection gap, old-VM contention and canonical status guard contention. */
void SelectionAndContention() {
    auto domain=std::make_shared<RuntimeCommitDomain>();ScriptScheduler scheduler;Access::Enroll(scheduler,domain);
    Barrier selected;std::atomic_uint oldTicks{};ScriptHost host;
    host.lifecycle=[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++oldTicks;};
    auto old=Vm("mod",host),next=Vm("mod");Access::Barriers(old,[&]{selected.Pause();},{});
    SchedulerPublicationPlan activate;Access::Prepare(scheduler,"mod",old,Purpose::Activate,activate);Check(Access::Publish(activate)==ScopeResult::Ok,"Selection setup failed");
    selected.Wait();SchedulerPublicationPlan handoff;
    // @summary Candidate construction completed before the selection barrier on the sole interpreter lane.
    selected.Release();
    Check(Access::Prepare(scheduler,"mod",next,Purpose::Handoff,handoff)==ScopeResult::Ok,"Selection handoff prepare failed");
    Barrier held;auto holder=std::async(std::launch::async,[&]{Access::Hold(old,false,[&]{held.Pause();});});held.Wait();
    const auto revision=domain->Revision();Check(Access::Publish(handoff)==ScopeResult::Busy && domain->Revision()==revision,"VM contention waited or published");held.Release();holder.get();
    Barrier status;holder=std::async(std::launch::async,[&]{Access::Hold(next,true,[&]{status.Pause();});});status.Wait();
    Check(Access::Publish(handoff)==ScopeResult::Busy && domain->Revision()==revision,"Status contention partially published");status.Release();holder.get();
    Check(Access::Publish(handoff)==ScopeResult::Ok,"Publication after contention failed");Access::Reclaim(handoff);
}
/** @summary Proves selected old VM runs no Lua after handoff and sealed extracted batches never invoke legacy sinks. */
void ActualGaps() {
    auto domain=std::make_shared<RuntimeCommitDomain>();std::atomic_uint sinks{},oldTicks{};
    ScriptScheduler scheduler({},[&](const std::string&,const std::shared_ptr<ScriptRuntime>&,const std::vector<ScriptPatchCommand>&,std::string&){++sinks;return true;});
    Access::Enroll(scheduler,domain);Barrier selected;ScriptHost host;
    host.lifecycle=[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++oldTicks;};
    auto old=Vm("mod",host),next=Vm("mod");Access::Barriers(old,[&]{selected.Pause();},{});
    SchedulerPublicationPlan initial;Access::Prepare(scheduler,"mod",old,Purpose::Activate,initial);
    SchedulerPublicationPlan handoff;
    Check(Access::Publish(initial)==ScopeResult::Ok,"Gap initial failed");selected.Wait();
    Check(Access::Prepare(scheduler,"mod",next,Purpose::Handoff,handoff)==ScopeResult::Ok,"Gap handoff prepare failed");
    Check(Access::Publish(handoff)==ScopeResult::Ok,"Selected old VM blocked handoff");selected.Release();
    Access::Reclaim(handoff);Check(oldTicks==0,"Stale selected VM entered Lua");Check(sinks==0,"Sealed VM reached legacy sink");
    Barrier extracted;ScriptHost producerHost;
    // @summary The real legacy staging API requires a native read adapter; this fixture returns no observation and never touches game memory.
    producerHost.readFloat=[](const sc13::core::Tgi&,std::uint32_t){return std::optional<FloatObservation>{};};
    auto producer=Vm("mod",producerHost,"return {onTick=function() simoder.props.setFloat('00B1B104:00000002:00000003',4,5) end}");
    auto successor=Vm("mod");Access::Barriers(producer,{},[&]{extracted.Pause();});
    SchedulerPublicationPlan p;Access::Prepare(scheduler,"mod",producer,Purpose::Handoff,p);Check(Access::Publish(p)==ScopeResult::Ok,"Batch producer activation failed");Access::Reclaim(p);
    extracted.Wait();SchedulerPublicationPlan successorPlan;Check(Access::Prepare(scheduler,"mod",successor,Purpose::Handoff,successorPlan)==ScopeResult::Ok,"Batch successor prepare failed");
    Check(Access::Publish(successorPlan)==ScopeResult::Ok,"Extracted old batch prevented handoff");extracted.Release();Access::Reclaim(successorPlan);
    Check(sinks==0,"Stale sealed batch was relabeled through legacy sink");
}
/** @summary Retained plans safely deny after scheduler destruction and abort with full deferred candidate close. */
void ShutdownAndLifetime() {
    auto domain=std::make_shared<RuntimeCommitDomain>();auto candidate=Vm("mod");SchedulerPublicationPlan plan;
    {ScriptScheduler scheduler;Access::Enroll(scheduler,domain);Check(Access::Prepare(scheduler,"mod",candidate,Purpose::Activate,plan)==ScopeResult::Ok,"Lifetime setup failed");}
    Check(Access::Publish(plan)==ScopeResult::Cancelled,"Plan survived destroyed scheduler as active authority");
    Check(Access::Reclaim(plan)==ScopeResult::Ok && Access::Closed(candidate),"Destroyed scheduler candidate abort unsafe");
    ScriptScheduler scheduler;auto other=std::make_shared<RuntimeCommitDomain>();Access::Enroll(scheduler,other);
    auto old=Vm("mod"),next=Vm("mod");SchedulerPublicationPlan initial,handoff;
    Access::Prepare(scheduler,"mod",old,Purpose::Activate,initial);Check(Access::Publish(initial)==ScopeResult::Ok,"Shutdown activation failed");
    Access::Prepare(scheduler,"mod",next,Purpose::Handoff,handoff);scheduler.Shutdown();
    Check(Access::Closed(old) && !old->Status().running,"Shutdown failed to close active typed VM");
    Check(Access::Publish(handoff)==ScopeResult::Cancelled,"Shutdown stale handoff reactivated candidate");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok && Access::Closed(next),"Shutdown candidate abort leaked slot");
}
/** @summary Tests the actual in-flight Lua callback's VM lock plus independently occupied original D and S participants. */
void InflightAndOtherLocks() {
    auto domain=std::make_shared<RuntimeCommitDomain>();ScriptScheduler scheduler;Access::Enroll(scheduler,domain);
    Barrier callback;ScriptHost host;host.lifecycle=[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)callback.Pause();};
    auto old=Vm("mod",host),next=Vm("mod");SchedulerPublicationPlan initial,handoff;
    Access::Prepare(scheduler,"mod",old,Purpose::Activate,initial);Check(Access::Publish(initial)==ScopeResult::Ok,"Inflight initial failed");
    callback.Wait();Check(Access::Prepare(scheduler,"mod",next,Purpose::Handoff,handoff)==ScopeResult::Ok,"Inflight prepare failed");
    const auto revision=domain->Revision();
    Check(Access::Publish(handoff)==ScopeResult::Busy && domain->Revision()==revision,"Actual in-flight callback did not return bounded Busy");
    callback.Release();
    Barrier writer;auto holder=std::async(std::launch::async,[&]{auto guard=domain->ReserveWriter();Check(guard.Acquired(),"Writer fixture failed");writer.Pause();});writer.Wait();
    Check(Access::Publish(handoff)==ScopeResult::Busy,"Occupied D did not deny before S/V");writer.Release();holder.get();
    Barrier schedulerLock;holder=std::async(std::launch::async,[&]{Access::HoldScheduler(scheduler,[&]{schedulerLock.Pause();});});schedulerLock.Wait();
    Check(Access::Publish(handoff)==ScopeResult::Busy,"Occupied S did not deny before V");schedulerLock.Release();holder.get();
    // @summary Preserve the original once-only final attempt before any potentially blocking post-result diagnostics.
    const auto settled=Access::Publish(handoff);const auto settledRevision=domain->Revision();
    const bool oldRunning=old->Status().running,nextRunning=next->Status().running;
    std::cerr<<"inflight settled diagnostic result="<<static_cast<int>(settled)<<" postResultDomainRevision="<<settledRevision<<" postResultOldRunning="<<oldRunning<<" postResultNextRunning="<<nextRunning<<std::endl;
    Check(settled==ScopeResult::Ok,"Inflight settled handoff denied");Access::Reclaim(handoff);
}
/** @summary Proves a pending native stop and a waiting shutdown close admission before lane teardown completes. */
void PendingClosures() {
    auto domain=std::make_shared<RuntimeCommitDomain>();ScriptScheduler scheduler;Access::Enroll(scheduler,domain);
    Barrier callback;ScriptHost host;host.lifecycle=[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)callback.Pause();};
    auto old=Vm("mod",host),candidate=Vm("other"),successor=Vm("mod");SchedulerPublicationPlan initial,plan;
    Access::Prepare(scheduler,"mod",old,Purpose::Activate,initial);Check(Access::Publish(initial)==ScopeResult::Ok,"Pending closure initial failed");
    callback.Wait();Check(Access::Prepare(scheduler,"other",candidate,Purpose::Activate,plan)==ScopeResult::Ok,"Pending closure prepare failed");
    auto stop=std::async(std::launch::async,[&]{candidate->Stop();});
    auto deadline=std::chrono::steady_clock::now()+2s;
    while(!Access::StopRequested(candidate) && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    Check(Access::StopRequested(candidate),"Native stop request not visible before lane return");
    // @summary This initial plan has no old VM participant: only the queued candidate stop can deny it while the other VM occupies the lane.
    Check(Access::Publish(plan)==ScopeResult::Denied && domain->Revision()==1,"Pending native stop candidate activated");
    callback.Release();stop.get();
    Check(Access::Publish(plan)==ScopeResult::Denied,"Completed native stop candidate revived");Access::Reclaim(plan);
    SchedulerPublicationPlan shutdownPlan;Check(Access::Prepare(scheduler,"mod",successor,Purpose::Handoff,shutdownPlan)==ScopeResult::Ok,"Shutdown preparation failed");
    // @summary This native VM lock simulates a still-executing callback while Shutdown must release S before waiting.
    Barrier vmHeld;auto holder=std::async(std::launch::async,[&]{Access::Hold(old,false,[&]{vmHeld.Pause();});});vmHeld.Wait();
    auto shutdown=std::async(std::launch::async,[&]{scheduler.Shutdown();});deadline=std::chrono::steady_clock::now()+2s;
    while(!Access::Stopped(scheduler) && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    Check(Access::Stopped(scheduler),"Shutdown retained S while waiting for VM");
    Check(Access::Publish(shutdownPlan)==ScopeResult::Cancelled,"Waiting shutdown permitted publication");
    vmHeld.Release();holder.get();shutdown.get();
    Check(Access::Reclaim(shutdownPlan)==ScopeResult::Ok && Access::Closed(successor),"Shutdown deferred candidate close failed");
}
/** @summary Failing preparation allocation has no operational marker and can be safely prepared again outside fences. */
void PreparationFailure() {
    auto domain=std::make_shared<RuntimeCommitDomain>();ScriptScheduler scheduler;Access::Enroll(scheduler,domain);
    auto candidate=Vm("mod");SchedulerPublicationPlan plan;bool failed{};
    rejectPreparationAllocation=true;
    try{Access::Prepare(scheduler,"mod",candidate,Purpose::Activate,plan);}catch(const std::bad_alloc&){failed=true;}
    rejectPreparationAllocation=false;
    Check(failed && domain->Revision()==0 && !candidate->Status().running && !Access::Current(scheduler,"mod"),"Preparation allocation failure published effects");
    Check(Access::Prepare(scheduler,"mod",candidate,Purpose::Activate,plan)==ScopeResult::Ok,"Preparation failure could not recover bound candidate");
    Check(Access::Reclaim(plan)==ScopeResult::Ok && Access::Closed(candidate),"Preparation recovery abort retained VM slot");
}
/** @summary Exercises the original128owner-map capacity while fully closing each unpublished VM and recovering one exact empty node. */
void CapacityRecovery() {
    auto domain=std::make_shared<RuntimeCommitDomain>();ScriptScheduler scheduler;Access::Enroll(scheduler,domain);
    for(unsigned i=0;i<128;++i) {
        const auto owner="capacity."+std::to_string(i);auto candidate=Vm(owner);SchedulerPublicationPlan plan;
        Check(Access::Prepare(scheduler,owner,candidate,Purpose::Activate,plan)==ScopeResult::Ok,"Owner reservation capacity below128");
        Check(Access::Reclaim(plan)==ScopeResult::Ok && Access::Closed(candidate),"Abort did not return real slot/memory before next candidate");
    }
    auto candidate=Vm("capacity.excess");SchedulerPublicationPlan plan;
    Check(Access::Prepare(scheduler,"capacity.excess",candidate,Purpose::Activate,plan)==ScopeResult::Denied,"Owner reservations exceeded128");
    Check(domain->Revision()==0 && !candidate->Status().running,"Capacity failure published a candidate");
    Check(Access::ReleaseEmpty(scheduler,"capacity.0")==ScopeResult::Ok,"Empty exact node recovery failed");
    Check(Access::Prepare(scheduler,"capacity.excess",candidate,Purpose::Activate,plan)==ScopeResult::Ok,"Recovered owner capacity remained unavailable");
    Check(Access::Reclaim(plan)==ScopeResult::Ok && Access::Closed(candidate),"Recovered candidate did not close");
}
/** @summary A real lane-selected owner cannot enter Lua after shutdown's sole retained host latch closes. */
void ShutdownSelection() {
    auto domain=std::make_shared<RuntimeCommitDomain>();ScriptScheduler scheduler;Access::Enroll(scheduler,domain);
    Barrier selected;std::atomic_uint ticks{},closes{};ScriptHost host;
    host.lifecycle=[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++ticks;if(phase==ScriptPhase::Close)++closes;};
    auto old=Vm("mod",host),candidate=Vm("mod");Access::Barriers(old,[&]{selected.Pause();},{});
    SchedulerPublicationPlan initial,handoff;Access::Prepare(scheduler,"mod",old,Purpose::Activate,initial);
    Check(Access::Publish(initial)==ScopeResult::Ok,"Shutdown selection initial failed");selected.Wait();
    Check(Access::Prepare(scheduler,"mod",candidate,Purpose::Handoff,handoff)==ScopeResult::Ok,"Shutdown selection handoff prepare failed");
    auto shutdown=std::async(std::launch::async,[&]{scheduler.Shutdown();});
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(!Access::Stopped(scheduler) && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    Check(Access::Stopped(scheduler),"Shutdown latch inaccessible while old selection retained");
    Check(Access::Publish(handoff)==ScopeResult::Cancelled,"Shutdown selection stale plan activated");
    selected.Release();shutdown.get();
    Check(ticks==0 && closes==1 && Access::Closed(old),"Shutdown selected old VM ran Lua or skipped full close");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok && Access::Closed(candidate),"Shutdown selection candidate abort failed");
}
}
/** @summary Executes isolated native scheduler acceptance without game, operating-system VM or production permission sources. */
int main() {
    try{
        std::cout<<"Lifecycle\n"<<std::flush;Lifecycle();
        std::cout<<"IdentityAndAbort\n"<<std::flush;IdentityAndAbort();
        std::cout<<"SelectionAndContention\n"<<std::flush;SelectionAndContention();
        std::cout<<"ActualGaps\n"<<std::flush;ActualGaps();
        std::cout<<"ShutdownAndLifetime\n"<<std::flush;ShutdownAndLifetime();
        std::cout<<"InflightAndOtherLocks\n"<<std::flush;InflightAndOtherLocks();
        std::cout<<"PendingClosures\n"<<std::flush;PendingClosures();
        std::cout<<"PreparationFailure\n"<<std::flush;PreparationFailure();
        std::cout<<"CapacityRecovery\n"<<std::flush;CapacityRecovery();
        std::cout<<"ShutdownSelection\n"<<std::flush;ShutdownSelection();
        Check(fenceAllocations==0 && fenceFrees==0 && fenceCallbacks==0,"Allocation, final reclamation or Lua observer occurred at publication fence");
        std::cout<<checks.load()<<" scheduler publication checks passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
