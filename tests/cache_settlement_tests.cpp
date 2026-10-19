#include "runtime/cache_settlement.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local unsigned allocationCountdown{};
thread_local unsigned allocationFailures{};
}
/** @summary Fail one actual selected native allocation without fabricating cache outcomes. @param size Actual request. @return Owned native storage. */
void* operator new(std::size_t size){if(allocationCountdown&&!--allocationCountdown){++allocationFailures;throw std::bad_alloc();}if(void* result=std::malloc(size?size:1))return result;throw std::bad_alloc();}
/** @summary Preserve native array allocation ownership. @param size Actual request. @return Owned storage. */
void* operator new[](std::size_t size){return ::operator new(size);}
/** @summary Release actual native allocation. @param pointer Original allocation. */
void operator delete(void* pointer) noexcept{std::free(pointer);}
/** @summary Release array allocation. @param pointer Original allocation. */
void operator delete[](void* pointer) noexcept{::operator delete(pointer);}
/** @summary Match sized native delete. @param pointer Original allocation. @param size Size hint. */
void operator delete(void* pointer,std::size_t size) noexcept{static_cast<void>(size);::operator delete(pointer);}
/** @summary Match sized array delete. @param pointer Original allocation. @param size Size hint. */
void operator delete[](void* pointer,std::size_t size) noexcept{static_cast<void>(size);::operator delete(pointer);}

namespace {
using namespace sc13::runtime;
using Tgi=sc13::core::Tgi;
using Record=sc13::core::RuntimePropertyRecord;
using S=CacheSettlement;using A=CacheSettlementTestAccess;using C=RuntimeResourceCacheTestAccess;
using namespace std::chrono_literals;
unsigned checks{};
constexpr Tgi targetA{0x00B1B104,0x61EFC000,1},targetB{0x00B1B104,0x61EFC000,2};
constexpr std::uint32_t property=0x09AE19D7;
/** @summary Require actual observed facts. @param value Condition. @param message Fixed diagnostic. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Copy a valid property table without registry/game dependencies. @return Owned actual native records. */
std::array<Record,1> Vanilla(){return {Record{property,std::bit_cast<std::uint32_t>(300.0f),{},0x000D0000}};}
/** @summary Prepare an actual cache plan with no registry effects. @param cache Original cache. @param target Exact TGI. @param address Synthetic identity only, never dereferenced. @return Actual plan. */
RuntimeResourceBuildPlan Plan(RuntimeResourceCache& cache,Tgi target=targetA,std::uintptr_t address=0x1000){RuntimeResourceBuildPlan plan;std::string error;const auto current=Vanilla();Check(cache.Prepare({target,address},current,7,{},plan,error),"actual cache Prepare");return plan;}
/** @summary Commit a previously prepared native plan. @param cache Original cache. @param plan Actual plan. */
void Commit(RuntimeResourceCache& cache,const RuntimeResourceBuildPlan& plan){std::string error;Check(cache.Commit(plan,error),"actual cache Commit");}
/** @summary Issue one original native owner, not a textual or caller-integer surrogate. @param cache Live original cache. @return Actual token. */
S::OwnerToken Owner(RuntimeResourceCache& cache){S::OwnerToken token;Check(A::IssueOwner(cache,token)==S::Result::Ok,"actual native owner issuance");return token;}
/** @summary Reserve exact unique original target storage. @param owner Original token. @param targets Actual unique union. @return Original move-only view. */
S::Batch Reserve(const S::OwnerToken& owner,const std::set<Tgi>& targets={targetA}){S::Batch batch;Check(S::Reserve(owner,targets,batch)==S::Result::Ok,"actual bounded original reservation");return batch;}
/** @summary Copy only genuine noncontended scalar evidence. @param batch Original view. @return Actual observation. */
S::Observation Observe(const S::Batch& batch){const auto result=S::Observe(batch);Check(result.result==S::Result::Ok,"actual scalar observation");return result;}
/** @summary Copy actual global accounting, never infer free capacity from an invalid observation. @return Native store metrics. */
A::Metrics Metrics(){A::Metrics result;Check(A::Inspect(result)==S::Result::Ok,"actual store metrics available");return result;}
/** @summary Explicitly cancel an untouched fixture reservation. @param batch Original view. */
void Cancel(S::Batch& batch){Check(S::CancelPrepared(batch)==S::Result::Ok,"actual Prepared cancellation");batch.Reset();}
/** @summary Require complete typed eviction, retaining its true count. @param cache Live original cache. @param batch Original acknowledged record. @return Actual completed result. */
S::Observation Evict(RuntimeResourceCache& cache,const S::Batch& batch){const auto result=S::Evict(cache,batch);Check(result.result==S::Result::Ok&&result.phase==S::Phase::Settled&&(result.eviction==CacheEvictionStatus::Evicted||result.eviction==CacheEvictionStatus::EvictedDiagnosticPending),"actual complete original eviction");return result;}
/** @summary Bounded actual lock-holder handshake with release before every outer assertion. */
struct Gate final {
    std::promise<void> entered,released;std::future<void> arrival=entered.get_future();std::shared_future<void> release=released.get_future().share();std::atomic_bool timedOut{};bool opened{};
    /** @summary Signal actual lock acquisition and wait within the unchanged five-second fixture bound. */
    void Hold(){entered.set_value();if(release.wait_for(5s)!=std::future_status::ready)timedOut=true;}
    /** @summary Release exactly once. */
    void Open(){if(!opened){opened=true;released.set_value();}}
    /** @summary Ensure release even after ordinary fixture failure. */
    ~Gate(){Open();}
};
/** @summary Await an actual accepted action without acquiring the record turn being observed.
 * @param batch Retained original view, not reset/moved concurrently. @param status Last actual accessor status.
 * @param count Last actual atomic count. @return True only for count1 before the unchanged five-second deadline.
 * @details Only this fixture's sole eviction can own an action here; its actual cache mutex remains held. */
bool ActionBusy(const S::Batch& batch,S::Result& status,std::uint32_t& count){
    status=S::Result::Invalid;count=0;const auto until=std::chrono::steady_clock::now()+5s;
    while(std::chrono::steady_clock::now()<until){status=A::CopyActionCount(batch,count);if(status!=S::Result::Ok||count!=0)return status==S::Result::Ok&&count==1;std::this_thread::yield();}
    return false;
}

/** @summary Reject cross-owner/reset/address ABA through actual Prepare/Commit. */
void Identity(){
    RuntimeResourceCache first,second;auto original=Plan(first);std::string error;
    Check(!second.Commit(original,error)&&second.size()==0,"foreign cache rejects same numeric epoch0 plan");first.Clear();Check(!first.Commit(original,error)&&first.size()==0,"Clear epoch0 ABA rejected");auto fresh=Plan(first);Commit(first,fresh);auto reused=Plan(first);Check(reused.status==RuntimeResourceBuildStatus::Reused,"actual Reused branch");first.Clear();Check(!first.Commit(reused,error),"Reused plan also carries original reset witness");Commit(first,Plan(first));
    alignas(RuntimeResourceCache) std::byte storage[sizeof(RuntimeResourceCache)];auto* old=std::construct_at(reinterpret_cast<RuntimeResourceCache*>(storage));auto oldPlan=Plan(*old);std::destroy_at(old);auto* successor=std::construct_at(reinterpret_cast<RuntimeResourceCache*>(storage));const bool rejected=!successor->Commit(oldPlan,error);const auto size=successor->size();std::destroy_at(successor);Check(rejected&&size==0,"actual same-address reconstruction cannot revive old weak lifetime witness");
    allocationFailures=0;allocationCountdown=1;bool threw{};try{RuntimeResourceCache failedCache;}catch(const std::bad_alloc&){threw=true;}allocationCountdown=0;Check(threw&&allocationFailures==1,"actual control allocation failure exposes no usable cache");
}
/** @summary Exercise immutable acknowledgement/cancellation and independent original owners without production marker claims. */
void Protocol(){
    RuntimeResourceCache cache;Commit(cache,Plan(cache));auto token=Owner(cache);auto batch=Reserve(token);const auto initial=Observe(batch);
    Check(initial.phase==S::Phase::Prepared&&initial.targets==1&&initial.payloadBytes==sizeof(Tgi)&&initial.committedRevision==0,"actual reserved payload before any acknowledgement");
    Check(S::Evict(cache,batch).result==S::Result::Conflict&&Plan(cache).status==RuntimeResourceBuildStatus::Reused,"unacknowledged record cannot evict");
    S::Batch duplicate;Check(S::Reserve(token,{targetB},duplicate)==S::Result::Conflict,"same original owner cannot replace pending target set");
    Check(A::IssueOwner(cache,token)==S::Result::Invalid&&S::Reserve(token,{targetB},batch)==S::Result::Invalid&&S::FindPending(token,batch)==S::Result::Invalid&&Observe(batch).incarnation==initial.incarnation&&Observe(batch).targets==1,"nonempty native outputs preserve original identity and payload");
    Check(S::MarkCommitted(batch,0)==S::Result::Invalid,"zero acknowledgement denied");Check(S::MarkCommitted(batch,19)==S::Result::Ok&&S::MarkCommitted(batch,19)==S::Result::Ok,"exact first revision idempotent");Check(S::MarkCommitted(batch,20)==S::Result::Conflict&&Observe(batch).committedRevision==19,"different revision cannot supersede original acknowledgement");Check(S::CancelPrepared(batch)==S::Result::Conflict,"acknowledged obligation cannot cancel");
    RuntimeResourceCache foreign;Commit(foreign,Plan(foreign));const auto refused=S::Evict(foreign,batch);Check(refused.result==S::Result::Ok&&refused.phase==S::Phase::Unqualified&&refused.eviction==CacheEvictionStatus::PendingResetMismatch&&refused.invalidated==0&&Plan(foreign).status==RuntimeResourceBuildStatus::Reused&&Plan(cache).status==RuntimeResourceBuildStatus::Reused,"live foreign cache cannot settle or mutate original obligation despite matching epoch and target");
    const auto complete=Evict(cache,batch);Check(complete.invalidated==1&&complete.committedRevision==19,"native eviction count joins original acknowledgement");auto after=Plan(cache);Commit(cache,after);const auto epoch=C::Epoch(cache,targetA);const auto repeat=Evict(cache,batch);Check(repeat.incarnation==complete.incarnation&&repeat.invalidated==complete.invalidated&&C::Epoch(cache,targetA)==epoch&&Plan(cache).status==RuntimeResourceBuildStatus::Reused,"already settled action never evicts again");
    S::Batch notFound;Check(S::FindPending(token,notFound)==S::Result::NotFound,"terminal retained batch no longer rediscoverable as pending");batch.Reset();
    auto cancelled=Reserve(Owner(cache));Check(S::CancelPrepared(cancelled)==S::Result::Ok&&S::MarkCommitted(cancelled,1)==S::Result::Conflict,"actual cancelled batch cannot resurrect");cancelled.Reset();
    auto other=Reserve(Owner(cache),{targetB});Check(S::MarkCommitted(other,27)==S::Result::Ok,"independent exact original owner acknowledgement");auto empty=Reserve(Owner(cache),{});Check(S::MarkCommitted(empty,29)==S::Result::Ok&&Evict(cache,empty).invalidated==0,"empty exact union may validly complete zero");Check(Observe(other).phase==S::Phase::CommittedPending&&Observe(other).committedRevision==27,"unrelated zero completion cannot settle another original owner");Evict(cache,other);empty.Reset();other.Reset();
}
/** @summary Drop every caller view and reacquire the exact retained native record, never an emergency replacement. */
void Rediscovery(){
    RuntimeResourceCache cache;Commit(cache,Plan(cache));auto token=Owner(cache);auto batch=Reserve(token,{targetA,targetB});const auto original=Observe(batch);Check(S::MarkCommitted(batch,23)==S::Result::Ok,"fixture-only native acknowledgement, not real D-marker evidence");batch.Reset();
    S::Batch wrong;auto foreign=Owner(cache);Check(S::FindPending(foreign,wrong)==S::Result::NotFound,"different native owner cannot find original pending");S::Batch recovered;Check(S::FindPending(token,recovered)==S::Result::Ok,"drop-all-views exact pending rediscovery");const auto retained=Observe(recovered);Check(retained.incarnation==original.incarnation&&retained.ownerSequence==original.ownerSequence&&retained.targets==2&&retained.payloadBytes==2*sizeof(Tgi)&&retained.committedRevision==23,"same original full payload and identity retained");
    S::Batch another;Check(S::FindPending(token,another)==S::Result::Ok,"actual second checked view");const auto movedId=Observe(another).incarnation;S::Batch moved=std::move(another);Check(S::Observe(another).result==S::Result::Invalid&&Observe(moved).incarnation==movedId,"view move transfers exact existing reference");Evict(cache,recovered);
    auto next=Reserve(token,{targetB});S::Batch found;Check(S::FindPending(token,found)==S::Result::Ok&&Observe(found).targets==1&&Observe(found).committedRevision==0,"retained terminal same-owner prefix cannot hide later genuine pending record");std::array<Tgi,1> nextTarget{};std::size_t copied{};Check(A::CopyTargets(found,nextTarget,copied)==S::Result::Ok&&copied==1&&nextTarget[0]==targetB,"rediscovery finds actual new original payload rather than old settled targets");Check(S::CancelPrepared(next)==S::Result::Ok,"new same-owner Prepared record cancels explicitly");found.Reset();next.Reset();moved.Reset();recovered.Reset();
}
/** @summary Actual later-node allocation failure preserves full batch and unchanged semantic epochs/validity. */
void Allocation(){
    RuntimeResourceCache cache;Commit(cache,Plan(cache,targetA,0x1000));Commit(cache,Plan(cache,targetB,0x2000));auto beforeA=Plan(cache,targetA,0x1000),beforeB=Plan(cache,targetB,0x2000);auto owner=Owner(cache);auto batch=Reserve(owner,{targetA,targetB});Check(S::MarkCommitted(batch,31)==S::Result::Ok,"original allocation fixture acknowledgement");
    allocationFailures=0;allocationCountdown=2;const auto pending=S::Evict(cache,batch);allocationCountdown=0;
    Check(allocationFailures==1&&pending.result==S::Result::Ok&&pending.eviction==CacheEvictionStatus::PendingAllocation&&pending.phase==S::Phase::CommittedPending&&pending.invalidated==0,"genuine second native allocation retains pending completion");
    Check(C::Epoch(cache,targetA)==std::optional<std::uint64_t>{0}&&!C::Epoch(cache,targetB),"real first zero bookkeeping node remains after later insertion failure");
    const auto retained=Observe(batch);Check(retained.targets==2&&retained.payloadBytes==2*sizeof(Tgi)&&retained.committedRevision==31,"allocation failure keeps complete original target charge and revision");
    std::array<Tgi,2> retainedTargets{};std::size_t copied{};Check(A::CopyTargets(batch,retainedTargets,copied)==S::Result::Ok&&copied==2&&retainedTargets[0]==targetA&&retainedTargets[1]==targetB,"actual immutable original payload retained through allocation failure, not only its count");
    Commit(cache,beforeA);Commit(cache,beforeB);Check(Plan(cache,targetA,0x1000).status==RuntimeResourceBuildStatus::Reused&&Plan(cache,targetB,0x2000).status==RuntimeResourceBuildStatus::Reused,"no semantic epoch increment or entry invalidation before full metadata availability");
    Check(Evict(cache,batch).invalidated==2,"same original retained batch eventually completes all targets");std::string error;Check(!cache.Commit(beforeA,error)&&!cache.Commit(beforeB,error),"every old affected plan denied after actual complete eviction");batch.Reset();
    RuntimeResourceCache legacy;Commit(legacy,Plan(legacy,targetA,0x1000));Commit(legacy,Plan(legacy,targetB,0x2000));std::size_t invalidated=99;const std::set<Tgi> both{targetA,targetB};allocationFailures=0;allocationCountdown=2;const bool success=legacy.InvalidateChecked(both,invalidated);allocationCountdown=0;Check(!success&&invalidated==0&&allocationFailures==1&&C::Epoch(legacy,targetA)==std::optional<std::uint64_t>{0}&&!C::Epoch(legacy,targetB),"legacy false/count0 maps actual incomplete metadata allocation");Check(legacy.InvalidateChecked(both,invalidated)&&invalidated==2,"legacy exact completed count after genuine retry");Check(legacy.InvalidateChecked({},invalidated)&&invalidated==0,"legacy valid zero is complete, not failure");
}
/** @summary Genuine cache/store locks cannot erase action completion or make passive observations block on cache. */
void Contention(){
    RuntimeResourceCache cache;Commit(cache,Plan(cache));auto owner=Owner(cache);auto batch=Reserve(owner);Check(S::MarkCommitted(batch,41)==S::Result::Ok,"actual contention acknowledgement");S::Batch extra;Check(S::FindPending(owner,extra)==S::Result::Ok,"independent checked view for real contended release");
    S::Batch empty,duplicate;std::uint32_t initialCount=99;
    Check(A::CopyActionCount(empty,initialCount)==S::Result::Invalid&&initialCount==0,"empty checked view cannot expose an action count");
    Check(A::CopyActionCount(batch,initialCount)==S::Result::Ok&&initialCount==0,"original record has no action before launch");
    Gate cacheGate,storeGate;std::future<void> holder,admission;std::future<S::Observation> action;
    bool cacheHeld{},storeHeld{},busy{},finishedWithoutAdmission{};S::Observation passive,result;
    auto countStatus=S::Result::Invalid,contendedObservation=S::Result::Invalid,competing=S::Result::Invalid,find=S::Result::Invalid;
    std::uint32_t acceptedCount{};
    try{
        holder=std::async(std::launch::async,[&]{C::HoldCache(cache,[&]{cacheGate.Hold();});});cacheHeld=cacheGate.arrival.wait_for(5s)==std::future_status::ready;
        if(cacheHeld){
            passive=S::Observe(batch);action=std::async(std::launch::async,[&]{return S::Evict(cache,batch);});busy=ActionBusy(batch,countStatus,acceptedCount);
            if(busy){
                contendedObservation=S::Observe(batch).result;competing=S::MarkCommitted(batch,41);find=S::FindPending(owner,duplicate);
                admission=std::async(std::launch::async,[&]{A::HoldAdmission([&]{storeGate.Hold();});});storeHeld=storeGate.arrival.wait_for(5s)==std::future_status::ready;
                extra.Reset();
            }
        }
        cacheGate.Open();if(holder.valid())holder.get();
        if(action.valid())finishedWithoutAdmission=action.wait_for(2s)==std::future_status::ready;
        storeGate.Open();if(admission.valid())admission.get();if(action.valid())result=action.get();
    }catch(...){
        cacheGate.Open();storeGate.Open();
        if(holder.valid())holder.wait();if(admission.valid())admission.wait();if(action.valid())action.wait();throw;
    }
    std::uint32_t finalCount=99;const auto finalStatus=A::CopyActionCount(batch,finalCount);
    std::cerr<<"cache contention accepted="<<busy<<" countStatus="<<static_cast<int>(countStatus)<<" acceptedCount="<<acceptedCount
        <<" observe="<<static_cast<int>(contendedObservation)<<" commit="<<static_cast<int>(competing)<<" find="<<static_cast<int>(find)
        <<" cacheHeld="<<cacheHeld<<" storeHeld="<<storeHeld<<" cacheTimedOut="<<cacheGate.timedOut.load()<<" storeTimedOut="<<storeGate.timedOut.load()
        <<" finishedWithoutAdmission="<<finishedWithoutAdmission<<" result="<<static_cast<int>(result.result)<<" phase="<<static_cast<int>(result.phase)
        <<" invalidated="<<result.invalidated<<" finalCountStatus="<<static_cast<int>(finalStatus)<<" finalCount="<<finalCount<<std::endl;
    Check(cacheHeld&&!cacheGate.timedOut,"actual joined cache lock cut reached");
    Check(passive.result==S::Result::Ok&&passive.phase==S::Phase::CommittedPending,"passive scalar observation does not borrow blocked cache");
    Check(busy&&countStatus==S::Result::Ok&&acceptedCount==1,"single actual eviction accepted before competing observations");
    Check(storeHeld&&!storeGate.timedOut,"actual joined admission lock cut reached");
    Check(contendedObservation==S::Result::Busy,"actual accepted eviction excludes scalar observation");
    Check(competing==S::Result::Busy,"actual accepted eviction excludes competing acknowledgement");
    Check(find==S::Result::Busy,"actual accepted eviction excludes pending lookup");
    Check(finishedWithoutAdmission&&result.result==S::Result::Ok&&result.phase==S::Phase::Settled&&result.invalidated==1,"completion records directly despite actual store admission held");
    Check(finalStatus==S::Result::Ok&&finalCount==0,"joined eviction releases its actual action count");
    Check(Metrics().externalViews==1,"independent view release cannot drop reference decrement while actual action and admission are busy");batch.Reset();
}
/** @summary Real flat payload/local owner/terminal-view capacity and incarnation recycling stay within exact bounds. */
void Capacity(){
    static_assert(sizeof(Tgi)==12);RuntimeResourceCache cache;std::set<Tgi> maximum;for(std::uint32_t i=0;i<8192;++i)maximum.insert({1,2,i});auto oversized=maximum;oversized.insert({1,2,8192});auto invalidOwner=Owner(cache);S::Batch denied;Check(S::Reserve(invalidOwner,oversized,denied)==S::Result::Capacity,"8193 actual unique targets rejected before slot publication");
    std::vector<S::OwnerToken> owners;std::vector<S::Batch> batches;owners.reserve(128);batches.reserve(128);std::uint64_t originalIncarnation{};
    for(unsigned i=0;i<128;++i){owners.push_back(Owner(cache));batches.push_back(Reserve(owners.back(),maximum));if(i==0)originalIncarnation=Observe(batches.back()).incarnation;}
    const auto full=Metrics();Check(full.occupied==128&&full.payloadBytes==12U*1024U*1024U&&full.externalViews==128,"actual128x8192x12 flat target budget and views");Check(sizeof(CacheWitness)<=64&&full.ownerBytes<=96&&full.viewBytes<=32&&full.recordBytes<=384&&full.scratchBytes<=64U*1024U&&full.storeBytes<=12U*1024U*1024U+128U*384U+4096U,"actual separate native metadata and offguard scratch bounds, not a total cache/process memory claim");auto excessOwner=Owner(cache);Check(S::Reserve(excessOwner,{targetA},denied)==S::Result::Capacity,"129th native record unavailable without evicting pending");
    for(std::size_t i=0;i<batches.size();++i){if(i%2)Check(S::CancelPrepared(batches[i])==S::Result::Ok,"exact unacknowledged capacity fixture explicitly cancelled");else{Check(S::MarkCommitted(batches[i],101+i)==S::Result::Ok,"exact native capacity acknowledgement");Check(Evict(cache,batches[i]).invalidated==0,"genuine target epoch fencing can settle with zero cached entries");}}Check(S::Reserve(excessOwner,{targetA},denied)==S::Result::Capacity&&Metrics().payloadBytes==full.payloadBytes,"both Cancelled and Settled externally retained views remain fully charged");
    batches.clear();auto replacement=Reserve(excessOwner);const auto recycled=Observe(replacement);Check(recycled.incarnation!=originalIncarnation,"actual reused slot receives nonreused incarnation");const auto reclaimed=Metrics();Check(reclaimed.occupied==1&&reclaimed.payloadBytes==sizeof(Tgi),"Reserve finite scan reclaims actual terminal zero-reference records");Cancel(replacement);
    std::set<Tgi> unique{targetA,targetA,targetB};auto deduplicated=Reserve(Owner(cache),unique);Check(Observe(deduplicated).targets==2&&Observe(deduplicated).payloadBytes==2*sizeof(Tgi),"actual input set union deduplicated before charging");Cancel(deduplicated);
    std::cout<<"cache native metadata owner="<<full.ownerBytes<<" view="<<full.viewBytes<<" record="<<full.recordBytes<<" store="<<full.storeBytes<<" scratch="<<full.scratchBytes<<" payload="<<full.payloadBytes<<'\n';
}
/** @summary Permanent mismatch cases run last; no reset/abandon API clears their retained process-lifetime obligations. */
void PermanentPending(){
    RuntimeResourceCache cache;auto owner=Owner(cache);auto resetBatch=Reserve(owner,{targetA,targetB});Check(S::MarkCommitted(resetBatch,51)==S::Result::Ok,"original reset fixture acknowledged");const auto original=Observe(resetBatch);cache.Clear();const auto mismatch=S::Evict(cache,resetBatch);Check(mismatch.result==S::Result::Ok&&mismatch.phase==S::Phase::Unqualified&&mismatch.eviction==CacheEvictionStatus::PendingResetMismatch&&mismatch.targets==2&&mismatch.committedRevision==51,"actual Clear cannot silently settle original pending targets");auto successorOwner=Owner(cache);auto successor=Reserve(successorOwner);Cancel(successor);Check(S::Evict(cache,resetBatch).eviction==CacheEvictionStatus::PendingResetMismatch,"same-cache successor reset never repairs old obligation");resetBatch.Reset();S::Batch recovered;Check(S::FindPending(owner,recovered)==S::Result::Ok&&Observe(recovered).incarnation==original.incarnation,"old reset identity remains rediscoverable after all views drop");recovered.Reset();
    auto dying=std::make_unique<RuntimeResourceCache>();auto expiredOwner=Owner(*dying);auto expiredBatch=Reserve(expiredOwner);Check(S::MarkCommitted(expiredBatch,61)==S::Result::Ok,"original lifetime fixture acknowledged");dying.reset();RuntimeResourceCache foreign;const auto unavailable=S::Evict(foreign,expiredBatch);Check(unavailable.result==S::Result::Ok&&unavailable.phase==S::Phase::Unqualified&&unavailable.eviction==CacheEvictionStatus::PendingUnavailable&&unavailable.targets==1,"expired weak original owner never dereferences dead cache or borrows replacement");expiredBatch.Reset();S::Batch retained;Check(S::FindPending(expiredOwner,retained)==S::Result::Ok&&Observe(retained).committedRevision==61,"expired original identity preserves native pending record");retained.Reset();
}
}
/** @summary Execute actual isolated cache bookkeeping tests, without claiming registry-marker/game/close integration. @return Zero only after all actual oracles pass. */
int main(){try{
    /** @summary Print fixed scenario identity before any native action. @param name Fixed label. @param body Actual case. */
    const auto run=[](const char* name,const auto& body){std::cerr<<"cache settlement begin "<<name<<std::endl;body();};
    run("cache/reset/address identity",Identity);run("native protocol",Protocol);run("pending rediscovery",Rediscovery);run("actual allocation",Allocation);run("native contention",Contention);run("bounded capacity",Capacity);run("permanent pending last",PermanentPending);
    std::cout<<"SOURCE_ONLY: uint64 issuer/reset/epoch/incarnation exhaustion; diagnostic allocation branch where actual pending status uses nonallocating SSO; no fabricated failure or production D marker\n";
    std::cout<<checks<<" cache settlement checks passed\n";return 0;
}catch(const std::exception& error){allocationCountdown=0;std::cerr<<"cache settlement: "<<error.what()<<'\n';return 1;}}
