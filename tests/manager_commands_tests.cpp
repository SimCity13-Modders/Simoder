#include "mods/manager_commands.hpp"
#include "mods/manager_publication.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include "mod_fixture.hpp"
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <array>
#include <algorithm>
#include <memory>
#include <malloc.h>
#include "mods/fixture_observation.hpp"
#include <set>
#include <vector>
#include "mods/manager_close.hpp"

namespace {
std::atomic_size_t fenceAllocations{},fenceFrees{};
thread_local bool failAllocation{};
thread_local const sc13::runtime::RuntimeCommitDomain* failAfterDomain{};
thread_local std::uint64_t failAfterRevision{};
thread_local bool postcommitFault{},postcommitFaultInsideFence{};
/** @summary Default zero preserves the existing first-allocation postmarker failure; new cases may skip one real allocation. */
thread_local unsigned postmarkerSkip{},postmarkerOrdinal{};
std::atomic_uint observedPostmarkerOrdinal{},observedPostmarkerFailures{};
std::atomic_bool observedFaultOnLane{},observedFaultInFence{};
}
/** @summary Preserve genuine failure/fence probes and count exact native admission allocation attempts; excludes Lua/C allocation. */
void BeforeNativeAllocation(){sc13::mods::fixture::Allocating();if(failAllocation){failAllocation=false;throw std::bad_alloc();}if(failAfterDomain&&failAfterDomain->Revision()>=failAfterRevision){++postmarkerOrdinal;if(postmarkerSkip)--postmarkerSkip;else{failAfterDomain=nullptr;postcommitFault=true;postcommitFaultInsideFence=sc13::scripting::SchedulerPublicationTestAccess::InsideFence();observedPostmarkerOrdinal.store(postmarkerOrdinal);++observedPostmarkerFailures;observedFaultOnLane.store(sc13::scripting::ScriptRuntime::OnScriptLane());observedFaultInFence.store(postcommitFaultInsideFence);throw std::bad_alloc();}}if(sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceAllocations;}
/** @summary Allocate actual unaligned scalar storage after the unchanged failure/fence probes and precise admission probe. @param size Actual bytes. @return Owned storage. */
void* operator new(std::size_t size){BeforeNativeAllocation();if(auto p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
/** @summary Match array ownership. @param size Requested bytes. @return Owned allocation. */
void* operator new[](std::size_t size){return ::operator new(size);}
/** @summary Count native final-free evidence. @param p Owned allocation. */
void operator delete(void* p) noexcept{if(p){sc13::mods::fixture::Freeing();if(sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceFrees;}std::free(p);}
/** @summary Match array free. @param p Owned allocation. */
void operator delete[](void* p) noexcept{::operator delete(p);}
/** @summary Match sized free. @param p Owned allocation. @param size Unused allocator hint. */
void operator delete(void* p,std::size_t size) noexcept{static_cast<void>(size);::operator delete(p);}
/** @summary Match sized array free. @param p Owned allocation. @param size Unused allocator hint. */
void operator delete[](void* p,std::size_t size) noexcept{static_cast<void>(size);::operator delete(p);}
/** @summary Cover aligned scalar native allocation through the same genuine probes. @param size Actual bytes. @param alignment Required alignment. @return Aligned owned storage. */
void* operator new(std::size_t size,std::align_val_t alignment){BeforeNativeAllocation();if(auto p=_aligned_malloc(size?size:1,static_cast<std::size_t>(alignment)))return p;throw std::bad_alloc();}
/** @summary Cover aligned array allocation once. @param size Actual bytes. @param alignment Required alignment. @return Aligned storage. */
void* operator new[](std::size_t size,std::align_val_t alignment){return ::operator new(size,alignment);}
/** @summary Free matching actual aligned storage and count it once. @param p Actual allocation. @param alignment Matching alignment. */
void operator delete(void* p,std::align_val_t alignment)noexcept{static_cast<void>(alignment);if(p){sc13::mods::fixture::Freeing();if(sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceFrees;}_aligned_free(p);}
/** @summary Match aligned array destruction. @param p Actual allocation. @param alignment Matching alignment. */
void operator delete[](void* p,std::align_val_t alignment)noexcept{::operator delete(p,alignment);}
/** @summary Match sized aligned scalar destruction. @param p Actual allocation. @param size Unused hint. @param alignment Matching alignment. */
void operator delete(void* p,std::size_t size,std::align_val_t alignment)noexcept{static_cast<void>(size);::operator delete(p,alignment);}
/** @summary Match sized aligned array destruction. @param p Actual allocation. @param size Unused hint. @param alignment Matching alignment. */
void operator delete[](void* p,std::size_t size,std::align_val_t alignment)noexcept{static_cast<void>(size);::operator delete(p,alignment);}
/** @summary Cover nonthrowing native scalar allocation. @param size Actual bytes. @return Owned storage or null. */
void* operator new(std::size_t size,const std::nothrow_t&)noexcept{try{return ::operator new(size);}catch(...){return nullptr;}}
/** @summary Cover nonthrowing native array allocation. @param size Actual bytes. @return Owned storage or null. */
void* operator new[](std::size_t size,const std::nothrow_t&)noexcept{try{return ::operator new[](size);}catch(...){return nullptr;}}
/** @summary Match failed nonthrowing scalar construction. @param p Actual storage. */
void operator delete(void* p,const std::nothrow_t&)noexcept{::operator delete(p);}
/** @summary Match failed nonthrowing array construction. @param p Actual storage. */
void operator delete[](void* p,const std::nothrow_t&)noexcept{::operator delete[](p);}
/** @summary Cover aligned nonthrowing scalar allocation. @param size Actual bytes. @param alignment Required alignment. @return Storage or null. */
void* operator new(std::size_t size,std::align_val_t alignment,const std::nothrow_t&)noexcept{try{return ::operator new(size,alignment);}catch(...){return nullptr;}}
/** @summary Cover aligned nonthrowing array allocation. @param size Actual bytes. @param alignment Required alignment. @return Storage or null. */
void* operator new[](std::size_t size,std::align_val_t alignment,const std::nothrow_t&)noexcept{try{return ::operator new[](size,alignment);}catch(...){return nullptr;}}
/** @summary Match failed aligned nonthrowing scalar construction. @param p Actual storage. @param alignment Matching alignment. */
void operator delete(void* p,std::align_val_t alignment,const std::nothrow_t&)noexcept{::operator delete(p,alignment);}
/** @summary Match failed aligned nonthrowing array construction. @param p Actual storage. @param alignment Matching alignment. */
void operator delete[](void* p,std::align_val_t alignment,const std::nothrow_t&)noexcept{::operator delete[](p,alignment);}

namespace {
using namespace sc13::mods;
using namespace sc13::runtime;
using namespace sc13::scripting;
using Access=ManagerPublicationTestAccess;
using Commands=ManagerCommandsTestAccess;
using Preparation=ManagerPreparationTestAccess;
using Scheduler=SchedulerPublicationTestAccess;
using Lifetime=ManagerLifetimeTestAccess;
using namespace std::chrono_literals;
std::atomic_uint checks{};
const char* scenarioLabel="startup";
/** @summary Flush fixed native progress markers so a hung process remains diagnosable without changing timing budgets. @param label Current bounded scenario. */
void Progress(const char* label){scenarioLabel=label;std::cerr<<"manager commands begin "<<label<<std::endl;}
const sc13::core::Tgi target{0x00B1B104,0x61EFC000,0x719436BD};
/** @summary Require actual behavior. @param value Observation. @param message Fixed diagnostic. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Poll a bounded eventual predicate, retaining its exact first success without reevaluation. @param predicate Observation or checked acquisition; failed acquisition attempts preserve output. @param message Failure diagnostic. @param budget Existing finite wait budget. */
template<class F> void Await(F predicate,const char* message,std::chrono::seconds budget=5s){const auto end=std::chrono::steady_clock::now()+budget;bool ready{};while(!(ready=predicate())&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);Check(ready,message);}
/** @summary Bound one native pause; destruction releases a waiter even when a test throws. */
struct Gate final{
    std::mutex mutex;std::condition_variable cv;bool entered{},released{};
    /** @summary Release any paused native callback. */
    ~Gate(){Release();}
    /** @summary Pause outside every production guard for at most five seconds. */
    void Pause(){std::unique_lock lock(mutex);entered=true;cv.notify_all();cv.wait_for(lock,5s,[&]{return released;});}
    /** @summary Observe callback entry without unbounded waiting. */
    void Wait(){std::unique_lock lock(mutex);Check(cv.wait_for(lock,5s,[&]{return entered;}),"native pause reached");}
    /** @summary Resume a callback exactly once. */
    void Release(){std::lock_guard lock(mutex);released=true;cv.notify_all();}
};
/** @summary Pin every executable Base input locally. @param root Verified checkout. @return Complete pin. */
BasePackagePin Pin(const std::filesystem::path& root){BasePackagePin pin{"0.1.0-alpha",{}};pin.files.emplace("mod.toml",HashText(ReadBounded(root/"mod.toml",4096)));for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"scripts"))if(entry.is_regular_file())pin.files.emplace(entry.path().lexically_relative(root).generic_string(),HashText(ReadBounded(entry.path(),1024U*1024U)));return pin;}
/** @summary Own exact native candidate and retained lifecycle witnesses. */
struct Candidate final{ManagerPreparation::Plan preparation;ManagerPublication::Plan publication;std::shared_ptr<RuntimeScope> scope;std::shared_ptr<SemanticEndpoint> endpoint;std::shared_ptr<ScriptRuntime> vm;OperationContext decision;SemanticContextLease facts;};
/** @summary Own actual native participants and synthetic local mod files, never game data. */
struct Fixture final{
    std::filesystem::path root=sc13::tests::CreateTemporaryDirectory();PatchRegistry registry;RuntimeResourceCache cache;
    std::shared_ptr<RuntimeCommitDomain> domain=std::make_shared<RuntimeCommitDomain>();
    std::shared_ptr<SemanticBridge> bridge=std::make_shared<SemanticBridge>();BasePackagePin pin;std::shared_ptr<BaseHost> base;std::unique_ptr<ModManager> manager;
    /** @summary Start actual pinned Base and manager, or share the already live singleton for an independent manager. @param package Verified checkout. @param sharedBase Existing exact singleton or absent for original behavior. @param sharedBridge Its exact bridge; required together with sharedBase. */
    explicit Fixture(const std::filesystem::path& package,std::shared_ptr<BaseHost> sharedBase={},std::shared_ptr<SemanticBridge> sharedBridge={}):pin(Pin(package)){
        Check(static_cast<bool>(sharedBase)==static_cast<bool>(sharedBridge),"shared singleton Base and bridge supplied together");if(sharedBridge)bridge=std::move(sharedBridge);
        if(!sharedBase){SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::Runtime,1,1);SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,7,3);}
        base=sharedBase?std::move(sharedBase):std::make_shared<BaseHost>(package,pin,std::function<void(ScriptPhase,std::uint64_t)>{},bridge);Check(base->Ready(),"actual Base ready");Write("simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)");
        manager=std::make_unique<ModManager>(root/"mods",root/"state.toon",registry,cache,RuntimeResourceRefreshCallback{},ScriptScheduler::LogSink{},nullptr,domain);std::string error;Check(manager->Initialize(error),"actual manager initialized");Check(Access::Enroll(*manager)==ScopeResult::Ok,"actual scheduler enrolled");
    }
    /** @summary Close borrowers before cache/registry and remove only synthetic inputs. */
    ~Fixture(){manager.reset();base.reset();std::error_code error;std::filesystem::remove_all(root,error);}
    /** @summary Write bounded fixture source with original default declarative baseline. @param tick Lua callback body. @param initial Lua initialization body. @param owner Actual authored fixture ID, original default unchanged. @param declarations Authored manifest patches within actual capture limits; original default unchanged. */
    void Write(const std::string& tick,const std::string& initial="simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)",const std::string& owner="fixture.owner",const std::string& declarations=sc13::tests::FixturePatch(3)){
        const auto directory=root/"mods"/owner;std::filesystem::create_directories(directory/"scripts");
        sc13::tests::WriteTextFile(directory/"mod.toml",sc13::tests::FixtureHeader(owner,"Commands")+"[scripts]\nlanguage='luau'\nentry='scripts/init.luau'\nmemoryMiB=2\nbudgetMs=20\n"+declarations);
        sc13::tests::WriteTextFile(directory/"scripts/init.luau",initial+"\nreturn {onTick=function() "+tick+" end,onDisable=function() end}");
    }
    /** @summary Capture original factual context without issuing permission. @param candidate Original endpoint. */
    void Facts(Candidate& candidate){copied::Envelope value;Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]})","test.echo@1",candidate.endpoint->storageOwner,value)==copied::Status::Ok,"facts decode");Check(bridge->BindReferences(*candidate.endpoint,value,candidate.facts)==ScopeResult::Ok,"original facts");}
    /** @summary Prepare one immutable source capture with bounded explicit precommit Busy retry, then issue the existing grant. @param tick Callback body. @param observer Original native initialization evidence. @param initial Actual Lua upvalues/initialization, original default unchanged. @param owner Actual fixture ID already discovered by manager, original default unchanged. @param declarations Exact authored declarative payload, original default unchanged. @return Native candidate. */
    Candidate Make(const std::string& tick,std::function<void(ScriptPhase,std::uint64_t)> observer={},const std::string& initial="simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)",const std::string& owner="fixture.owner",const std::string& declarations=sc13::tests::FixturePatch(3)){
        Write(tick,initial,owner,declarations);Candidate result;std::shared_ptr<const CapturedCandidate> capture;
        const auto deadline=std::chrono::steady_clock::now()+5s;unsigned attempts{};
        while(!capture){if(attempts++>=4096||std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("preparation.busy bounded capture exhausted");try{capture=Preparation::Capture(*manager,owner,root/"mods"/owner,pin);}catch(const std::runtime_error& error){if(std::string_view(error.what())!="preparation.busy")throw;std::this_thread::yield();}}
        for(;;){if(attempts++>=4096||std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("preparation.busy bounded candidate exhausted");try{result.preparation=Access::Candidate(*manager,capture,base,observer);break;}catch(const std::runtime_error& error){if(std::string_view(error.what())!="preparation.busy")throw;std::this_thread::yield();}}
        result.scope=Preparation::Scope(result.preparation);result.endpoint=Preparation::Endpoint(result.preparation);result.vm=Preparation::Vm(result.preparation);result.scope->SetReadiness(true);result.decision=RuntimeScopeTestAccess::Issue(*result.scope);Check(result.decision.owner!=0,"reviewed fixture grant");Facts(result);return result;
    }
    /** @summary Prepare exact original lifecycle once; report bounded native failure facts outside its guards. @param old Original active candidate or absent for activation. @param next Actual successor candidate. */
    void Prepare(Candidate* old,Candidate& next){
        if(old){old->decision=RuntimeScopeTestAccess::Reserve(*old->scope,old->decision);Facts(*old);}
        std::string error;const auto result=Access::Prepare(*manager,std::move(next.preparation),old?old->decision:OperationContext{},next.decision,old?old->facts:SemanticContextLease{},next.facts,next.publication,error);
        if(result!=ScopeResult::Ok)std::cerr<<"lifecycle prepare purpose="<<(old?"handoff":"activation")<<" ScopeResult="<<static_cast<unsigned>(result)<<" nativeError="<<error<<'\n';
        Check(result==ScopeResult::Ok,"actual lifecycle prepared");
    }
    /** @summary Commit only bounded transient lifecycle Busy. @param candidate Exact retained plan. */
    void Activate(Candidate& candidate){ScopeResult result=ScopeResult::Busy;for(unsigned n=0;n<4096&&result==ScopeResult::Busy;++n){result=Access::Commit(candidate.publication);if(result==ScopeResult::Busy)std::this_thread::yield();}Check(result==ScopeResult::Ok,"actual activation committed");}
};
const std::string set9="simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',9)";
const std::string clear="simoder.props.clearFloat('00B1B104:61EFC000:719436BD','09AE19D7')";
/** @summary Distinguish actual final-M contention from true stale preparation without changing authority or initialization budgets. @param package Verified pinned Base. */
void PreparationContention(const std::filesystem::path& package){
    Fixture fixture(package);fixture.Write("");auto capture=Preparation::Capture(*fixture.manager,"fixture.owner",fixture.root/"mods"/"fixture.owner",fixture.pin);const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();auto original=Lifetime::Capture(*fixture.manager,"fixture.owner");Gate entry;
    auto candidate=std::async(std::launch::async,[&]{try{auto result=Access::Candidate(*fixture.manager,capture,fixture.base,[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Entry)entry.Pause();});return std::string("unexpected candidate success");}catch(const std::runtime_error& error){return std::string(error.what());}});
    entry.Wait();{auto manager=Lifetime::HoldManager(*fixture.manager);entry.Release();Check(candidate.wait_for(2s)==std::future_status::ready,"actual final-M Busy returns without waiting");Check(candidate.get()=="preparation.busy","actual final try-M contention is classified Busy");}
    Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"Busy candidate has no registry or D effect");Check(Lifetime::Validate(original)==ScopeResult::Ok,"Busy candidate preserves actual canonical stamp");Check(!fixture.manager->Snapshot().front().scriptRunning,"Busy candidate never activates");
    auto recovered=Access::Candidate(*fixture.manager,capture,fixture.base);Check(Preparation::Current(recovered)==ScopeResult::Ok,"same immutable capture succeeds after actual M contention ends");Check(!Preparation::Scope(recovered)->Current().activationCommitted&&fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"recovered candidate remains unactivated");
    unsigned entries{};const auto staleRevision=fixture.domain->Revision();bool stale{};try{static_cast<void>(fixture.Make("",[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Entry){++entries;auto writer=fixture.domain->ReserveWriter();Check(writer.Acquired(),"actual native stale witness writer");Check(writer.Advance()==staleRevision+1,"intentional native domain invalidation");}}));}catch(const std::runtime_error& error){stale=std::string_view(error.what())=="preparation.stale";}
    Check(stale&&entries==1,"true Stale propagates once without candidate retry");Check(fixture.domain->Revision()==staleRevision+1&&fixture.registry.Current()==snapshot,"only explicit stale-test D advance occurred");Check(Lifetime::Validate(original)==ScopeResult::Ok&&!fixture.manager->Snapshot().front().scriptRunning,"true Stale never publishes canonical state or activates VM");
}
/** @summary Retain the first actual extracted envelope and discard later ones without renewal or execution. */
struct Capture final{
    /** @summary Retain callback storage independently of the test stack lifetime. */
    struct Storage final{std::mutex mutex;std::condition_variable cv;ScriptCommandBatch batch;bool captured{};};
    std::shared_ptr<Storage> storage=std::make_shared<Storage>();
    ScriptCommandBatch& batch=storage->batch;
    std::weak_ptr<ScriptRuntime> vm;
    std::weak_ptr<RuntimeScope> scope;
    /** @summary Move original scheduler output into one owned slot. @param value Exact extracted envelope. */
    static void Take(const std::shared_ptr<Storage>& data,ScriptCommandBatch& value){std::lock_guard lock(data->mutex);if(!data->captured){data->batch=std::move(value);data->captured=true;data->cv.notify_all();}else value=ScriptCommandBatch{};}
    /** @summary Wait a bounded interval for actual extraction. */
    void Wait(){std::unique_lock lock(storage->mutex);const bool ready=storage->cv.wait_for(lock,5s,[&]{return storage->captured;});lock.unlock();if(!ready){std::cerr<<"capture timeout scenario="<<scenarioLabel;if(const auto actual=vm.lock()){const auto status=actual->Status();const auto route=Commands::Observe(actual);std::cerr<<" running="<<status.running<<" diagnostic="<<status.diagnostic<<" generation="<<status.generation<<" routeOutcome="<<static_cast<unsigned>(route.lastOutcome)<<" attempts="<<route.attempts<<" commits="<<route.commits<<" selected="<<route.selectedRegistration<<" committedRegistration="<<route.commitRegistration;}if(const auto owner=scope.lock()){const auto status=owner->Current();std::cerr<<" scopeGeneration="<<status.generation<<" scopeEpoch="<<status.epoch<<" scopeRevision="<<status.revision<<" activation="<<status.activationCommitted<<" stopping="<<status.stopping<<" reservations="<<Commands::Reservations(*owner);}std::cerr<<'\n';}Check(ready,"actual extracted batch captured");}
};
/** @summary Install dormant route and interception before scheduling. @param fixture Actual owner. @param candidate Prepared lifecycle plan. @param capture Native retained slot. */
void Install(Fixture& fixture,Candidate& candidate,Capture& capture){Check(Commands::Install(*fixture.manager,Access::Origin(candidate.publication),candidate.decision)==ScopeResult::Ok,"dormant exact route installed");capture.vm=candidate.vm;capture.scope=candidate.scope;Commands::Intercept(candidate.vm,[weak=std::weak_ptr<Capture::Storage>(capture.storage)](ScriptCommandBatch& batch){if(const auto data=weak.lock())Capture::Take(data,batch);else batch=ScriptCommandBatch{};});}
/** @summary Copy coherent actual canonical state with bounded transient contention. @param candidate Retained lifecycle binding. @return Coherent observation. */
Access::Observation Observe(const Candidate& candidate){Access::Observation output;ScopeResult result=ScopeResult::Busy;for(unsigned n=0;n<4096&&result==ScopeResult::Busy;++n){result=Access::Observe(candidate.publication,output);if(result==ScopeResult::Busy)std::this_thread::yield();}Check(result==ScopeResult::Ok,"canonical observation");Check(output.projectionMatches&&output.orderMatches&&output.ownerInOrder&&output.orderSize==1,"whole actual projection and order agree");Check(output.runtime==candidate.vm&&output.running,"same actual current VM");return output;}
/** @summary Require exact effective float bits and declarative baseline parity. @param candidate Actual binding. @param scripted Expected staged patch presence. @param value Expected bits. */
void Value(const Candidate& candidate,bool scripted,float value){const auto observed=Observe(candidate);const auto patches=observed.registry->Find(target);Check(patches&&patches->size()==(scripted?2U:1U),"declarative and scripted count");Check(patches->front().patch.properties.front().value==3,"declarative baseline preserved");Check(std::bit_cast<std::uint32_t>(patches->back().patch.properties.front().value)==std::bit_cast<std::uint32_t>(value),"effective exact float bits");}
/** @summary Qualify actual Set/Clear, one-shot, unchanged ownership and capacity retirement. @param package Verified Base. */
void Publication(const std::filesystem::path& package){
    for(const auto& body:std::vector<std::string>{set9,clear,"simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',-0.0)"}){
        Fixture fixture(package);auto candidate=fixture.Make(body);fixture.Prepare(nullptr,candidate);Capture capture;Install(fixture,candidate,capture);fixture.Activate(candidate);capture.Wait();const auto before=Observe(candidate);const auto count=Commands::Reservations(*candidate.scope);Check(count>=2,"ticket owns exact additional reservation");
        Check(Commands::Consume(capture.batch)==ScopeResult::Ok,"actual delayed command committed");Check(fixture.domain->Revision()==before.domainRevision+1,"one command D marker");Check(Commands::Consume(capture.batch)==ScopeResult::Conflict,"one-shot replay denied");const auto after=Observe(candidate);Check(after.recordRevision==before.recordRevision+1&&after.origin==before.origin&&after.runtime==before.runtime,"same VM and origin with fresh sole stamp");Value(candidate,body==clear?false:true,body==clear?3.0F:body==set9?9.0F:-0.0F);
        const auto status=Commands::Observe(candidate.vm);Check(status.commits==1&&status.selectedRegistration!=0&&status.selectedRegistration==status.commitRegistration,"original registration retained");Commands::Intercept(candidate.vm,{});Await([&]{return Commands::Reservations(*candidate.scope)==1;},"successful reservation retired");
    }
}
/** @summary Qualify exact denial and cleanup after post-extraction authority/context/lifetime changes. @param package Verified Base. */
void Denials(const std::filesystem::path& package){
    for(unsigned scenario=0;scenario!=8;++scenario){Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);Capture capture;Install(fixture,candidate,capture);fixture.Activate(candidate);capture.Wait();const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();
        if(scenario==0)Check(RuntimeScopeTestAccess::Revoke(*candidate.scope,candidate.decision.grantGeneration)==ScopeResult::Ok,"exact permission revoke");
        if(scenario==1)candidate.scope->RequestStop();if(scenario==2)fixture.domain->RequestStop();
        if(scenario==3)SemanticBridgeTestAccess::ObserveContext(*fixture.bridge,SemanticContextKind::City,7,4);
        if(scenario==4)fixture.bridge->Close();if(scenario==5)candidate.vm->Fail("isolated command failure");
        if(scenario==6)SemanticBridgeTestAccess::ObserveContext(*fixture.bridge,SemanticContextKind::City,7,3,false);
        if(scenario==7)fixture.manager.reset();
        Check(Commands::Consume(capture.batch)!=ScopeResult::Ok,"changed original authority/context denies");Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"denial preserves registry and D");Check(Commands::Consume(capture.batch)==ScopeResult::Conflict,"denied envelope consumed");capture.batch=ScriptCommandBatch{};Await([&]{return Commands::Reservations(*candidate.scope)<=1;},"denied reservation retired");
    }
}
/** @summary Contend each actual publication participant with an already extracted envelope. @param package Verified Base. */
void Contention(const std::filesystem::path& package){
    for(unsigned participant=0;participant!=7;++participant){Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);Capture capture;Install(fixture,candidate,capture);fixture.Activate(candidate);capture.Wait();const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();ScopeResult outcome=ScopeResult::Invalid;
        std::cerr<<"manager commands contention participant="<<participant<<std::endl;
        const auto attempt=[&]{auto work=std::async(std::launch::async,[&]{return Commands::Consume(capture.batch);});Check(work.wait_for(2s)==std::future_status::ready,"command contention nonblocking");return work.get();};
        if(participant==0){auto lock=fixture.domain->ReserveWriter();outcome=attempt();}if(participant==1){auto lock=Lifetime::HoldManager(*fixture.manager);outcome=attempt();}
        if(participant==2)Scheduler::HoldScheduler(Access::Scheduler(*fixture.manager),[&]{outcome=attempt();});if(participant==3)Scheduler::Hold(candidate.vm,false,[&]{outcome=attempt();});
        if(participant==4){auto lock=fixture.registry.TryReservePublication();outcome=attempt();}if(participant==5){auto lock=SemanticBridgeTestAccess::ReserveObservation(*fixture.bridge);outcome=attempt();}
        if(participant==6){auto lock=RuntimeScopeTestAccess::ReserveState(*candidate.scope);outcome=attempt();}
        Check(outcome==ScopeResult::Busy,"actual contender returns Busy");Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"Busy no marker");Check(Commands::Consume(capture.batch)==ScopeResult::Conflict,"Busy consumes original ticket");Value(candidate,true,1);capture.batch=ScriptCommandBatch{};Await([&]{return Commands::Reservations(*candidate.scope)==1;},"Busy ticket exact retirement");
    }
}
/** @summary Old extracted work never retargets same textual ID or successor diagnostics. @param package Verified Base. */
void Handoff(const std::filesystem::path& package){Fixture fixture(package);auto old=fixture.Make(set9);fixture.Prepare(nullptr,old);Capture capture;Install(fixture,old,capture);fixture.Activate(old);capture.Wait();auto next=fixture.Make("");fixture.Prepare(&old,next);fixture.Activate(next);const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();const auto status=next.vm->Status();Check(Commands::Consume(capture.batch)!=ScopeResult::Ok,"old extracted ticket denies after handoff");Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"old ticket cannot mutate successor");Check(next.vm->Status().diagnostic==status.diagnostic&&next.vm->Status().generation==status.generation,"old result cannot publish successor status");Value(next,true,1);Check(Access::Reclaim(next.publication)==ScopeResult::Ok,"retired original VM reclaimed");}
/** @summary Delay actually selected or extracted scheduler work while publishing a same-ID successor. @param package Verified Base. */
void SelectionDelay(const std::filesystem::path& package){for(unsigned extracted=0;extracted!=2;++extracted){
    Fixture fixture(package);auto old=fixture.Make(set9);fixture.Prepare(nullptr,old);Capture capture;Install(fixture,old,capture);
    auto gate=std::make_shared<Gate>();auto armed=std::make_shared<std::atomic_bool>();
    const auto pause=[gate,armed]{if(armed->exchange(false))gate->Pause();};
    Scheduler::Barriers(old.vm,extracted?std::function<void()>{}:pause,extracted?pause:std::function<void()>{});
    fixture.Activate(old);capture.Wait();auto next=fixture.Make("");fixture.Prepare(&old,next);*armed=true;gate->Wait();
    fixture.Activate(next);const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();const auto diagnostic=next.vm->Status().diagnostic;
    gate->Release();Check(Access::Reclaim(next.publication)==ScopeResult::Ok,"delayed selected VM retired after resume");
    Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"delayed scheduler selection never retargets successor");Check(next.vm->Status().diagnostic==diagnostic,"delayed original result cannot disclose to successor");Value(next,true,1);
}}
/** @summary A delayed command invalidates an earlier handoff stamp while keeping GetFloat closed. @param package Verified Base. */
void StaleHandoffAndRead(const std::filesystem::path& package){Fixture fixture(package);auto old=fixture.Make("assert(not pcall(function() simoder.props.getFloat('00B1B104:61EFC000:719436BD','09AE19D7') end)) "+set9);fixture.Prepare(nullptr,old);Capture capture;Install(fixture,old,capture);fixture.Activate(old);capture.Wait();auto next=fixture.Make("");fixture.Prepare(&old,next);Check(Commands::Consume(capture.batch)==ScopeResult::Ok,"staging works while live GetFloat remains closed");Value(old,true,9);const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry.Current();Check(Access::Commit(next.publication)!=ScopeResult::Ok,"command stamp invalidates previously prepared handoff");Check(fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot,"stale handoff cannot replace committed command baseline");Check(Access::Reclaim(next.publication)==ScopeResult::Ok,"stale candidate cleanup outside fence");}
/** @summary Exact no-change cannot bypass revoked permission, and missing route never gains authority. @param package Verified Base. */
void NoChangeAndMissing(const std::filesystem::path& package){
    {Fixture fixture(package);auto candidate=fixture.Make("simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)");fixture.Prepare(nullptr,candidate);Capture capture;Install(fixture,candidate,capture);fixture.Activate(candidate);capture.Wait();const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry.Current();Check(RuntimeScopeTestAccess::Revoke(*candidate.scope,candidate.decision.grantGeneration)==ScopeResult::Ok,"no-change grant revoked");Check(Commands::Consume(capture.batch)!=ScopeResult::Ok,"no-change checks exact authority");Check(fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot,"no-change denial has no effect");}
    {Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);fixture.Activate(candidate);const auto snapshot=fixture.registry.Current();Await([&]{return !candidate.vm->Status().running;},"missing producer isolates readerless write callback");const auto status=candidate.vm->Status();Check(status.diagnostic.find("PROP writes require an attached host")!=std::string::npos,"missing producer reports closed native write admission");
        Access::Observation observed;ScopeResult result=ScopeResult::Busy;for(unsigned n=0;n<4096&&result==ScopeResult::Busy;++n){result=Access::Observe(candidate.publication,observed);if(result==ScopeResult::Busy)std::this_thread::yield();}
        Check(result==ScopeResult::Ok&&observed.runtime==candidate.vm&&observed.origin==Access::Origin(candidate.publication)&&!observed.running,"missing producer preserves exact canonical identity while stopping original VM");Check(observed.projectionMatches&&observed.orderMatches&&observed.ownerInOrder&&observed.orderSize==1,"missing producer preserves canonical projection and full owner order");Check(observed.recordRevision==1&&observed.domainRevision==1&&fixture.registry.Current()==snapshot,"missing producer preserves original stamp and registry without command marker");Check(Scheduler::Current(Access::Scheduler(*fixture.manager),"fixture.owner")==candidate.vm,"missing producer does not replace scheduler registration owner");
        const auto patches=snapshot->Find(target);Check(patches&&patches->size()==2&&patches->back().patch.properties.front().value==1,"missing producer leaves initial script and declarative patches unchanged");Check(fixture.domain->Revision()==1&&Commands::Observe(candidate.vm).commits==0,"missing producer creates no command authority");}
}
/** @summary Original sink lease denies manager closure and waits safely for an acquired user. @param package Verified Base. */
void Lease(const std::filesystem::path& package){for(unsigned acquired=0;acquired!=2;++acquired){Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);Capture capture;Install(fixture,candidate,capture);fixture.Activate(candidate);capture.Wait();Gate gate;Commands::Barriers(candidate.vm,acquired?std::function<void()>{}:[&]{gate.Pause();},acquired?[&]{gate.Pause();}:std::function<void()>{});const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();auto consume=std::async(std::launch::async,[&]{return Commands::Consume(capture.batch);});gate.Wait();auto close=std::async(std::launch::async,[&]{fixture.manager.reset();});if(acquired)Check(close.wait_for(40ms)==std::future_status::timeout,"manager waits for actual acquired sink lease");else Check(close.wait_for(2s)==std::future_status::ready,"manager closes before lease admission");gate.Release();Check(consume.wait_for(2s)==std::future_status::ready,"closed sink returns");Check(consume.get()!=ScopeResult::Ok,"closed sink denied");Check(close.wait_for(2s)==std::future_status::ready,"manager drains without fences");close.get();Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"closure no borrowed publication");Commands::Barriers(candidate.vm,{},{});}}
/** @summary Exercise reservation retirement after explicit abort and native preparation allocation failure. @param package Verified Base. */
void AbortAndException(const std::filesystem::path& package){for(unsigned mode=0;mode!=2;++mode){Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);Capture capture;Install(fixture,candidate,capture);fixture.Activate(candidate);capture.Wait();const auto snapshot=fixture.registry.Current();if(mode){failAllocation=true;ScopeResult outcome=ScopeResult::Invalid;try{outcome=Commands::Consume(capture.batch);}catch(const std::bad_alloc&){}failAllocation=false;Check(outcome!=ScopeResult::Ok,"preparation allocation fault cannot commit");}capture.batch=ScriptCommandBatch{};Await([&]{return Commands::Reservations(*candidate.scope)==1;},"aborted or exceptional ticket retired");Check(fixture.registry.Current()==snapshot,"abort/fault preserves publication");}}
/** @summary Invalid callback payloads cannot cross the existing finite/count boundary. @param package Verified Base. */
void InvalidPayload(const std::filesystem::path& package){for(const auto& body:std::vector<std::string>{"simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',0/0)","for i=1,257 do "+set9+" end"}){Fixture fixture(package);auto candidate=fixture.Make(body);fixture.Prepare(nullptr,candidate);Check(Commands::Install(*fixture.manager,Access::Origin(candidate.publication),candidate.decision)==ScopeResult::Ok,"invalid payload route installed");fixture.Activate(candidate);Await([&]{return !candidate.vm->Status().running;},"invalid callback isolated by existing runtime limits");Check(fixture.domain->Revision()==1,"invalid payload has no command marker");const auto patches=fixture.registry.Current()->Find(target);Check(patches&&patches->size()==2&&patches->back().patch.properties.front().value==1,"invalid payload preserves active baseline");Await([&]{return Commands::Reservations(*candidate.scope)==1;},"failed callback ticket retired");}}
/** @summary An actual postmarker cache insertion failure reports committed/pending without rolling back registry state. @param package Verified Base. */
void CachePending(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make("simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',9)");fixture.Prepare(nullptr,candidate);Capture capture;Install(fixture,candidate,capture);fixture.Activate(candidate);capture.Wait();const auto before=Observe(candidate);
    postcommitFault=false;postcommitFaultInsideFence=false;failAfterRevision=before.domainRevision+1;failAfterDomain=fixture.domain.get();
    const auto outcome=Commands::Consume(capture.batch);failAfterDomain=nullptr;
    Check(postcommitFault&&!postcommitFaultInsideFence,"native allocation fault occurs only after actual D marker outside fence");Check(outcome==ScopeResult::Indeterminate,"cache eviction failure reports committed pending");
    const auto after=Observe(candidate);Check(after.domainRevision==before.domainRevision+1&&after.recordRevision==before.recordRevision+1,"cache failure retains single committed D and stamp");
    const auto added=after.registry->Find(sc13::core::Tgi{0x00B1B104,0x61EFC000,0x719436BE});Check(added&&added->size()==1&&added->front().patch.properties.front().value==9,"cache failure never rolls back newly committed logical patch");
    Check(Commands::Observe(candidate.vm).commits==1&&candidate.vm->Status().running,"committed pending counted without retiring original VM");Check(Commands::Consume(capture.batch)==ScopeResult::Conflict,"committed pending envelope remains one-shot");
    Await([&]{return Commands::Reservations(*candidate.scope)==1;},"committed pending reservation retired");Value(candidate,true,1);
}
/** @summary Fill actual reservation capacity and prove exact deferred Q-busy retirement preserves the governing grant. @param package Verified Base. */
void DeferredCapacity(const std::filesystem::path& package){
    /** @summary Retain 127 real one-shot envelopes independently of callback stack lifetime. */
    struct Queue final{std::mutex mutex;std::condition_variable cv;std::vector<ScriptCommandBatch> batches;};
    Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);Check(Commands::Install(*fixture.manager,Access::Origin(candidate.publication),candidate.decision)==ScopeResult::Ok,"capacity route installed");
    auto queue=std::make_shared<Queue>();queue->batches.reserve(127);
    Commands::Intercept(candidate.vm,[weak=std::weak_ptr<Queue>(queue)](ScriptCommandBatch& batch){if(const auto data=weak.lock()){std::lock_guard lock(data->mutex);if(data->batches.size()<127){data->batches.push_back(std::move(batch));data->cv.notify_all();}else batch=ScriptCommandBatch{};}else batch=ScriptCommandBatch{};});
    fixture.Activate(candidate);
    {std::unique_lock lock(queue->mutex);Check(queue->cv.wait_for(lock,15s,[&]{return queue->batches.size()==127;}),"127 actual extracted tickets fill remaining native capacity");}
    Check(Commands::Reservations(*candidate.scope)==128,"root grant plus127 exact reservations fills capacity");
    const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();
    {
        auto lock=RuntimeScopeTestAccess::ReserveState(*candidate.scope);
        for(auto& batch:queue->batches){auto consume=std::async(std::launch::async,[&batch]{return Commands::Consume(batch);});Check(consume.wait_for(2s)==std::future_status::ready,"Q-busy deferred release never blocks consumed batch");Check(consume.get()==ScopeResult::Busy,"each full-capacity ticket denies Q contention");Check(Commands::Consume(batch)==ScopeResult::Conflict,"deferred exact envelope cannot replay");}
        Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"full deferred queue changes no registry or D marker");
    }
    Await([&]{return Commands::Reservations(*candidate.scope)==1;},"all127 deferred exact reservations drain to governing grant");
    Commands::Intercept(candidate.vm,{});Await([&]{return Commands::Observe(candidate.vm).commits>=1;},"governing grant remains usable after full deferred retirement");
    Await([&]{return Commands::Reservations(*candidate.scope)==1;},"postdrain actual tick reservation retires");Value(candidate,true,9);
}
/** @summary Tick beyond native reservation capacity without leaking one-shot tickets. @param package Verified Base. */
void Repeated(const std::filesystem::path& package){Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);Check(Commands::Install(*fixture.manager,Access::Origin(candidate.publication),candidate.decision)==ScopeResult::Ok,"repeated route");auto intercepted=std::make_shared<std::atomic_uint>();auto failures=std::make_shared<std::atomic_uint>();Commands::Intercept(candidate.vm,[intercepted,failures](ScriptCommandBatch& batch){if(Commands::Consume(batch)!=ScopeResult::Ok)++*failures;++*intercepted;});fixture.Activate(candidate);Await([&]{return *intercepted>=140;},"actual ticks beyond reservation capacity",15s);Commands::Intercept(candidate.vm,{});Await([&]{return Commands::Reservations(*candidate.scope)==1;},"repeated ticks bounded exact reservation count");Check(*failures==0,"all repeated native batches admitted without slot exhaustion");Value(candidate,true,9);const auto observed=Observe(candidate);Check(fixture.domain->Revision()==2&&observed.domainRevision==2&&observed.recordRevision==2,"140 validated repeated batches advance D and sole stamp only for first changed Set");}
}
namespace {
using Settlement=CacheSettlement;
using Store=CacheSettlementTestAccess;
using Cache=RuntimeResourceCacheTestAccess;
using Close=ManagerCloseTestAccess;
namespace NativeObservation=sc13::mods::fixture;
using AdmissionReason=NativeObservation::AdmissionReason;
/** @summary Retain the exact VM through joined removal of allocation observation, never its origin/Plan/State. */
struct AdmissionMeter final {
    std::shared_ptr<ScriptRuntime> vm,laneWitness;NativeObservation::Handle handle;bool joined{};
    /** @summary Arm the fixed original target and retain an optional independent barrier witness through exceptional cleanup. @param value Actual observed route VM. @param witness Live independent VM sharing the actual singleton script lane; never the counter target. */
    explicit AdmissionMeter(std::shared_ptr<ScriptRuntime> value,std::shared_ptr<ScriptRuntime> witness={}):vm(std::move(value)),laneWitness(std::move(witness)),handle(NativeObservation::ArmAdmission(vm.get())){Check(static_cast<bool>(handle),"fixed admission observation admitted");}
    /** @summary Stop original matching and join the actual preceding lane visit using the retained open witness when original ingress may be closed. */
    void Join(){if(joined)return;NativeObservation::StopAdmission(handle);if(laneWitness)Close::InvokeControl(laneWitness,[]{});else if(!Scheduler::Closed(vm))Close::InvokeControl(vm,[]{});joined=true;}
    /** @summary Require real complete intervals and no native C++ allocation/free or wrong-lane observation. @return Exact joined counters. */
    NativeObservation::Admission Finish(){Join();const auto actual=NativeObservation::ObserveAdmission(handle);Check(actual.started!=0&&actual.started==actual.finished&&actual.allocations==0&&actual.frees==0&&actual.nested==0&&actual.offLane==0&&actual.reasons[static_cast<unsigned>(AdmissionReason::Unset)]==0,"exact full native admission intervals close without allocations, frees, nesting or off-lane substitution");return actual;}
    /** @summary Read an actual completed branch count without manufacturing a visit. @param reason Required native branch. @return Whether that branch actually completed. */
    bool Has(AdmissionReason reason)const{return NativeObservation::ObserveAdmission(handle).reasons[static_cast<unsigned>(reason)]!=0;}
    /** @summary Preserve observation/VM lifetime through cleanup on every test exception. */
    ~AdmissionMeter()noexcept{try{Join();}catch(...){std::terminate();}}
};
const sc13::core::Tgi targetB{0x00B1B104,0x61EFC000,0x719436BE},targetC{0x00B1B104,0x61EFC000,0x719436BF};
const std::string initialCounter="local ticks=0; simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)";
/** @summary Wait for a real gate without throwing while another operation owns a blocking future. @param gate Original bounded gate. @return Actual entry before deadline. */
bool Entered(const std::shared_ptr<Gate>& gate){std::unique_lock lock(gate->mutex);return gate->cv.wait_for(lock,5s,[&]{return gate->entered;});}
/** @summary Pause just one genuine callback; captured owners outlive worker invocation. @param gate Shared joined gate. @return Actual single-shot callback. */
std::shared_ptr<const std::function<void()>> Once(const std::shared_ptr<Gate>& gate){auto armed=std::make_shared<std::atomic_bool>(true);return std::make_shared<const std::function<void()>>([gate,armed]{if(armed->exchange(false))gate->Pause();});}
/** @summary Install the genuine automatic route without interception or replacement Consume. @param fixture Original manager. @param candidate Genuine plan. */
void Automatic(Fixture& fixture,Candidate& candidate){Check(Commands::Install(*fixture.manager,Access::Origin(candidate.publication),candidate.decision)==ScopeResult::Ok,"automatic original route installed without interception");}
/** @summary Observe actual write-gate completion after worker releases the marker turn. @param fixture Original manager. @return Exact copied pending observation. */
Settlement::Observation Pending(Fixture& fixture){Settlement::Observation result;Await([&]{result=Commands::ObserveCache(*fixture.manager,"fixture.owner");return result.result!=Settlement::Result::Busy;},"actual original cache observation responsive");return result;}
/** @summary Require that exact original lineage has no outstanding record. @param fixture Original owner. */
void Settled(Fixture& fixture){Await([&]{return Commands::ObserveCache(*fixture.manager,"fixture.owner").result==Settlement::Result::NotFound;},"original cache obligation settled, not a latest-route diagnostic");}
/** @summary Inspect actual store accounting with bounded try-only retry. @return Genuine current metrics. */
Store::Metrics StoreMetrics(){Store::Metrics result;Await([&]{return Store::Inspect(result)==Settlement::Result::Ok;},"actual fixed store metrics");return result;}
/** @summary Prepare real vanilla-derived cache evidence; synthetic key is never dereferenced. @param fixture Original cache/registry. @param tgi Exact target. @param address Stable synthetic cache identity. @return Actual cache plan. */
RuntimeResourceBuildPlan CachePlan(Fixture& fixture,sc13::core::Tgi tgi=target,std::uintptr_t address=0x1000){
    const std::array<sc13::core::RuntimePropertyRecord,1> vanilla{{{0x09AE19D7,std::bit_cast<std::uint32_t>(300.0F),{},0x000D0000}}};
    RuntimeResourceBuildPlan plan;std::string error;const auto snapshot=fixture.registry.Current();const auto patches=snapshot->Find(tgi);
    Check(fixture.cache.Prepare({tgi,address},vanilla,snapshot->generation(),patches?std::span<const RegisteredPatch>(*patches):std::span<const RegisteredPatch>{},plan,error),"genuine vanilla-derived cache plan");return plan;
}
/** @summary Check exact newest registry output without attributing ordinary manual pending commands to it. @param fixture Original registry. @param tgi Exact target. @param value Expected scalar. */
void RegistryValue(Fixture& fixture,sc13::core::Tgi tgi,float value){const auto snapshot=fixture.registry.Current();const auto patches=snapshot->Find(tgi);Check(patches&&!patches->empty()&&patches->back().patch.properties.front().value==value,"exact automatic target and value");}
/** @summary Observe genuine automatic Set/Clear, noop, original cache invalidation and x86 fixed layouts. @param package Verified Base. */
void AutomaticPublication(const std::filesystem::path& package){
    const auto layout=Commands::HandoffLayout();std::cout<<"handoff sizeof job="<<layout.scheduledJobBytes<<" Control="<<layout.controlBytes<<" prior="<<layout.priorControlBytes<<" anchor="<<layout.vmAnchorBytes<<" transfer="<<layout.transferBytes<<'\n';
    Check(sizeof(void*)==4&&layout.scheduledJobBytes<=128&&layout.controlBytes<=layout.priorControlBytes+256,"actual Win32 fixed handoff storage bounds");
    for(const auto& body:std::array<std::string,2>{set9,clear}){
        Fixture fixture(package);auto laneThread=std::make_shared<std::thread::id>();auto workerThread=std::make_shared<std::thread::id>();auto recorded=std::make_shared<std::atomic_bool>();
        auto candidate=fixture.Make(body,[laneThread,recorded](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick&&!recorded->exchange(true))*laneThread=std::this_thread::get_id();});fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);
        auto gate=std::make_shared<Gate>();auto once=std::make_shared<std::atomic_bool>(true);
        Commands::HandoffBarriers(candidate.vm,{},std::make_shared<const std::function<void()>>([gate,once,workerThread]{if(once->exchange(false)){*workerThread=std::this_thread::get_id();gate->Pause();}}));
        fixture.Activate(candidate);const bool reached=Entered(gate);const auto before=Observe(candidate);const auto epoch=Cache::Epoch(fixture.cache,target);auto oldPlan=CachePlan(fixture);std::string error;const bool stored=fixture.cache.Commit(oldPlan,error);
        gate->Release();Check(reached&&stored,"actual automatic callback reached worker with original cached resource");
        Await([&]{return fixture.domain->Revision()==before.domainRevision+1&&Commands::Observe(candidate.vm).commits>=1;},"unintercepted scheduler reaches native publication");Settled(fixture);
        const auto after=Observe(candidate);Check(after.origin==before.origin&&after.runtime==before.runtime&&after.recordRevision==before.recordRevision+1,"actual automatic path preserves original stamp lineage and VM");
        Check(Cache::Epoch(fixture.cache,target)==std::optional<std::uint64_t>{epoch.value_or(0)+1},"actual original cache epoch advanced exactly once");
        Check(!fixture.cache.Commit(oldPlan,error),"actual old vanilla-derived plan invalidated by automatic command");
        auto fresh=CachePlan(fixture);Check(fixture.cache.Commit(fresh,error),"fresh actual vanilla-derived preparation remains usable");Value(candidate,body!=clear,body==clear?3.0F:9.0F);
        const auto fixedEpoch=Cache::Epoch(fixture.cache,target);Await([&]{return Commands::Observe(candidate.vm).attempts>=3;},"real later SamePatches cycles run");
        Check(fixture.domain->Revision()==after.domainRevision&&Cache::Epoch(fixture.cache,target)==fixedEpoch,"automatic noops neither Advance nor re-evict");
        Check(*laneThread!=std::thread::id{}&&*workerThread!=std::thread::id{}&&*laneThread!=*workerThread&&*workerThread!=std::this_thread::get_id(),"real Lua lane and native worker have different thread identities");
        std::cout<<"automatic threads lane="<<*laneThread<<" worker="<<*workerThread<<'\n';
        Commands::HandoffBarriers(candidate.vm,{},{});
    }
}
/** @summary Preserve prior and later manual commands across the actual preack/preconsume automatic cut. @param package Verified Base. */
void AutomaticOutputIsolation(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make("ticks+=1; if ticks==1 then simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',11) elseif ticks==2 then simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',22) elseif ticks==3 then simoder.props.setFloat('00B1B104:61EFC000:719436BF','09AE19D7',33) end",{},initialCounter);
    fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);auto worker=std::make_shared<Gate>(),ack=std::make_shared<Gate>(),consume=std::make_shared<Gate>();
    auto holder=std::async(std::launch::async,[&]{Commands::HoldWorker(*fixture.manager,[worker]{worker->Pause();});});const bool held=Entered(worker);
    Commands::HandoffBarriers(candidate.vm,Once(ack),Once(consume));fixture.Activate(candidate);candidate.vm->Tick(0);worker->Release();holder.get();
    const bool captured=Entered(ack);const auto revision=fixture.domain->Revision();auto manual=std::async(std::launch::async,[&]{candidate.vm->Tick(0);});
    const bool waitsForAck=manual.wait_for(20ms)==std::future_status::timeout;ack->Release();const bool beforeConsume=Entered(consume);const bool manualReady=manual.wait_for(2s)==std::future_status::ready;
    std::vector<ScriptPatchCommand> ordinary;if(manualReady){manual.get();ordinary=candidate.vm->TakePatchCommands();}
    const bool noEarlyMarker=fixture.domain->Revision()==revision;consume->Release();if(!manualReady)manual.get();
    Check(held&&captured&&waitsForAck&&beforeConsume&&manualReady&&noEarlyMarker,"actual ack and worker cuts exclude competing control until capture completes");
    Check(ordinary.size()==2&&ordinary[0].target==target&&ordinary[0].value==11&&ordinary[1].target==targetC&&ordinary[1].value==33,"preexisting and later public Tick output stays caller-drainable in order");
    Await([&]{return fixture.domain->Revision()==revision+1;},"only captured automatic output commits");Settled(fixture);RegistryValue(fixture,targetB,22);RegistryValue(fixture,target,1);
    Check(!fixture.registry.Current()->Find(targetC),"later same-generation Tick cannot lend its commands to earlier automatic ticket");Commands::HandoffBarriers(candidate.vm,{},{});
}
/** @summary Combined prior/manual plus automatic callback quota is 256 and failure rolls back only its own output. @param package Verified Base. */
void AutomaticQuota(const std::filesystem::path& package){
    for(const unsigned prior:std::array<unsigned,2>{255,256}){
        Fixture fixture(package);const auto tick="ticks+=1; if ticks==1 then for i=1,"+std::to_string(prior)+" do simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',11) end else "+set9+" end";
        auto candidate=fixture.Make(tick,{},initialCounter);fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);auto gate=std::make_shared<Gate>();
        auto holder=std::async(std::launch::async,[&]{Commands::HoldWorker(*fixture.manager,[gate]{gate->Pause();});});const bool held=Entered(gate);fixture.Activate(candidate);candidate.vm->Tick(0);const auto revision=fixture.domain->Revision();gate->Release();holder.get();Check(held,"actual W excludes automatic quota race");
        if(prior==255){Await([&]{return Commands::Observe(candidate.vm).commits>=1;},"one remaining callback command admitted");Check(fixture.domain->Revision()==revision+1,"255 prior plus one callback command commits once");}
        else {Await([&]{return !candidate.vm->Status().running;},"zero callback allowance rejects actual push");Check(fixture.domain->Revision()==revision,"quota rejection cannot publish command marker");Check(candidate.vm->Status().diagnostic.find("At most 256")!=std::string::npos,"actual setter quota diagnostic");}
        const auto ordinary=candidate.vm->TakePatchCommands();Check(ordinary.size()==prior&&std::all_of(ordinary.begin(),ordinary.end(),[](const auto& command){return command.target==target&&command.value==11;}),"successful capture or callback rollback preserves every original manual command");if(prior==255)candidate.vm->Stop();
        Await([&]{return Commands::Reservations(*candidate.scope)<=1;},"quota/error ticket retired without replay");
    }
}
/** @summary Both semantic Pump and onTick contribute to the same exact automatic output. @param package Verified Base. */
void AutomaticSemanticOutput(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make("if not subscribed then assert(base.subscribe(function(record,denial) assert(record and not denial); observed=true; simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',21) end)); subscribed=true elseif observed and not done then done=true; simoder.props.setFloat('00B1B104:61EFC000:719436BF','09AE19D7',22) end",{},"local subscribed,observed,done=false,false,false; simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)");
    fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);fixture.Activate(candidate);Await([&]{return Commands::Observe(candidate.vm).commits>=1;},"real semantic subscription and onTick output consumed automatically");Settled(fixture);
    RegistryValue(fixture,targetB,21);RegistryValue(fixture,targetC,22);Check(fixture.domain->Revision()==2&&Observe(candidate).recordRevision==2,"semantic Pump and onTick share exactly one changed publication");
}
/** @summary Real W/UI occupancy defers Lua before ticket selection while independent lane controls progress. @param package Verified Base. */
void AutomaticOccupancy(const std::filesystem::path& package){
    for(unsigned ui=0;ui!=2;++ui){
        Fixture fixture(package);auto ticks=std::make_shared<std::atomic_uint>();auto candidate=fixture.Make(set9,[ticks](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++*ticks;});fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);AdmissionMeter admission(candidate.vm);auto gate=std::make_shared<Gate>();std::future<void> holder;
        if(ui){Close::ActionBarrier(*fixture.manager,[gate]{gate->Pause();});Check(fixture.manager->RequestAction("refresh"),"real original UI occupancy accepted");}
        else holder=std::async(std::launch::async,[&]{Commands::HoldWorker(*fixture.manager,[gate]{gate->Pause();});});
        const bool held=Entered(gate);fixture.Activate(candidate);const auto reservationCount=Commands::Reservations(*candidate.scope);const auto revision=fixture.domain->Revision();
        std::atomic_bool controlOnLane{};auto control=std::async(std::launch::async,[&]{Close::InvokeControl(candidate.vm,[&]{controlOnLane=ScriptRuntime::OnScriptLane();});});const bool progressed=control.wait_for(2s)==std::future_status::ready;
        std::this_thread::sleep_for(65ms);const bool deferred=*ticks==0&&Commands::Reservations(*candidate.scope)==reservationCount&&fixture.domain->Revision()==revision;
        const bool busy=ui?fixture.manager->OperationBusy():true;gate->Release();if(holder.valid())holder.get();control.get();if(ui)Close::ActionBarrier(*fixture.manager,{});
        Check(held&&progressed&&controlOnLane&&deferred&&busy,"same worker occupancy rejects before Lua/reservation and does not block lane control");
        Await([&]{return Commands::Observe(candidate.vm).commits>=1;},"deferred actual automatic cycle eventually executes");Check(fixture.domain->Revision()==revision+1,"deferred cycle executes once without hidden replay");Value(candidate,true,9);Check(admission.Has(ui?AdmissionReason::Busy:AdmissionReason::WorkerLock)&&admission.Has(AdmissionReason::Accepted),"actual W-lock or busy rejection and later original acceptance have exact intervals");static_cast<void>(admission.Finish());
    }
}
/** @summary Native V contention never prevents another real VM's lane control or creates a fresh ticket. @param package Verified Base. */
void AutomaticVmContention(const std::filesystem::path& package){
    Fixture fixture(package);auto ticks=std::make_shared<std::atomic_uint>();auto candidate=fixture.Make(set9,[ticks](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++*ticks;});fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);
    AdmissionMeter admission(candidate.vm);
    Fixture other(package,fixture.base,fixture.bridge);auto independent=other.Make("");other.Prepare(nullptr,independent);other.Activate(independent);
    auto gate=std::make_shared<Gate>();auto holder=std::async(std::launch::async,[&]{Scheduler::Hold(candidate.vm,false,[gate]{gate->Pause();});});const bool held=Entered(gate);
    // @summary Activation itself needs V, so hold contention only after activation while worker admission is deferred by W.
    gate->Release();holder.get();Check(held,"genuine V lock observed");
    auto worker=std::make_shared<Gate>();auto workerHeld=std::async(std::launch::async,[&]{Commands::HoldWorker(*fixture.manager,[worker]{worker->Pause();});});const bool w=Entered(worker);fixture.Activate(candidate);
    auto actualV=std::make_shared<Gate>();auto contender=std::async(std::launch::async,[&]{Scheduler::Hold(candidate.vm,false,[actualV]{actualV->Pause();});});const bool v=Entered(actualV);worker->Release();workerHeld.get();
    auto control=std::async(std::launch::async,[&]{independent.vm->Tick(0);});const bool progress=control.wait_for(2s)==std::future_status::ready;std::this_thread::sleep_for(65ms);const bool noTick=*ticks==0&&Commands::Reservations(*candidate.scope)==1;
    actualV->Release();contender.get();control.get();Check(w&&v&&progress&&noTick,"contended actual V allows other VM control progress without ticket/Lua");Await([&]{return Commands::Observe(candidate.vm).commits>=1;},"original VM executes after actual V release");Check(admission.Has(AdmissionReason::InitialV)&&admission.Has(AdmissionReason::Accepted),"genuine initial-V rejection and accepted promotion counted in exact admission interval");static_cast<void>(admission.Finish());
}
}
namespace {
/** @summary Arm one genuine second epoch-node allocation failure only on the actual executing worker after its original D marker. @param fixture Original cache/domain. @param candidate Real automatic route. @return First real preconsume synchronization gate. */
std::shared_ptr<Gate> ArmPartialFailure(Fixture& fixture,Candidate& candidate){
    observedPostmarkerOrdinal=0;observedPostmarkerFailures=0;observedFaultOnLane=false;observedFaultInFence=false;
    auto armed=std::make_shared<std::atomic_bool>(true);auto gate=std::make_shared<Gate>();
    Commands::HandoffBarriers(candidate.vm,{},std::make_shared<const std::function<void()>>([armed,gate,domain=fixture.domain]{if(armed->exchange(false)){postmarkerSkip=1;postmarkerOrdinal=0;postcommitFault=false;postcommitFaultInsideFence=false;failAfterRevision=domain->Revision()+1;failAfterDomain=domain.get();gate->Pause();}}));return gate;
}
/** @summary Finish activation's own cache action before the exact first automatic faulting Consume. @param fixture Original owner. @param candidate Actual pending-failure route. */
void ActivatePartial(Fixture& fixture,Candidate& candidate){
    const auto gate=ArmPartialFailure(fixture,candidate);fixture.Activate(candidate);const bool entered=Entered(gate);
    // @summary Prevent incidental later admission visits from contending the first real final-V while proving the actual allocator branch.
    auto laneGate=std::make_shared<Gate>();auto lane=std::async(std::launch::async,[&]{Preparation::HoldLane(*fixture.manager,[laneGate]{laneGate->Pause();});});const bool laneHeld=Entered(laneGate);gate->Release();
    const auto deadline=std::chrono::steady_clock::now()+2s;while(observedPostmarkerFailures.load()!=1&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();const bool faulted=observedPostmarkerFailures==1;
    laneGate->Release();lane.get();Check(entered&&laneHeld&&faulted,"first genuine automatic marker/allocation executes after activation with actual lane admission excluded, without replacing native work");
}
/** @summary Require actual partial insertion and complete original obligation after the real automatic marker. @param fixture Original cache. @param candidate Original active owner. @return Copied immutable obligation identity. */
Settlement::Observation PartialFailure(Fixture& fixture,Candidate& candidate){
    Await([&]{return observedPostmarkerFailures.load()==1&&Commands::Observe(candidate.vm).attempts>=1;},"actual worker postmarker allocation failure observed");
    Settlement::Observation pending;Await([&]{pending=Commands::ObserveCache(*fixture.manager,"fixture.owner");return pending.result==Settlement::Result::Ok&&pending.phase==Settlement::Phase::CommittedPending;},"original automatic record remains committed pending");
    Check(observedPostmarkerOrdinal==2&&!observedFaultOnLane&&!observedFaultInFence,"second actual postmarker allocation fails off lane and outside publication fences");
    Check(Cache::Epoch(fixture.cache,targetB)==std::optional<std::uint64_t>{0}&&!Cache::Epoch(fixture.cache,targetC),"first actual epoch node stays zero and second remains absent");
    Check(pending.targets==3&&pending.payloadBytes==3*sizeof(sc13::core::Tgi)&&pending.committedRevision==2&&pending.invalidated==0&&pending.eviction==CacheEvictionStatus::PendingAllocation,"original full affected union and actual committed revision retained");
    RegistryValue(fixture,targetB,9);RegistryValue(fixture,targetC,10);Check(fixture.domain->Revision()==2&&Observe(candidate).recordRevision==2,"partial cache bookkeeping never rolls back actual command publication");
    Settlement::Batch view;Await([&]{return Commands::PendingCache(*fixture.manager,"fixture.owner",view)==Settlement::Result::Ok;},"fresh original Lease rediscovers actual pending record");
    std::array<sc13::core::Tgi,3> payload{};std::size_t copied{};const auto exact=Store::CopyTargets(view,payload,copied);const auto retained=Settlement::Observe(view);
    Check(exact==Settlement::Result::Ok&&copied==3&&payload==std::array<sc13::core::Tgi,3>{target,targetB,targetC}&&retained.incarnation==pending.incarnation&&retained.ownerSequence==pending.ownerSequence&&retained.committedRevision==pending.committedRevision,"actual immutable original full affected payload survives real partial insertion");
    view.Reset();const auto dropped=Pending(fixture);Check(dropped.incarnation==pending.incarnation&&dropped.targets==3,"all original envelope/native views may drop without erasing pending record");
    std::cout<<"automatic partial allocation ordinal="<<observedPostmarkerOrdinal<<" revision="<<pending.committedRevision<<" targets="<<pending.targets<<'\n';return pending;
}
const std::string bothTargets="simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',9); simoder.props.setFloat('00B1B104:61EFC000:719436BF','09AE19D7',10)";
/** @summary Pending blocks actual noop/new command and lifecycle writes, then original eviction repairs after revoke/stopped D without store reacquisition. @param package Verified Base. */
void AutomaticPendingRepair(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make("ticks+=1; if ticks<=2 then "+bothTargets+" elseif ticks==3 then simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',19) end",{},initialCounter);
    fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);ActivatePartial(fixture,candidate);const auto original=PartialFailure(fixture,candidate);
    Await([&]{return Commands::Observe(candidate.vm).attempts>=3;},"real later no-op and changed-patch command attempted while pending");
    const auto snapshot=fixture.registry.Current();const auto epoch=Cache::Epoch(fixture.cache,target);Check(fixture.domain->Revision()==original.committedRevision&&Commands::Observe(candidate.vm).lastOutcome==ScopeResult::Busy,"pending gate denies noop and changed output before marker");RegistryValue(fixture,targetB,9);
    auto next=fixture.Make("");fixture.Prepare(&candidate,next);const auto handoff=Access::Commit(next.publication);ManagerPublication::Plan removal;std::string error;const auto preparedRemoval=Access::Remove(*fixture.manager,Access::Origin(candidate.publication),removal,error);const auto removed=preparedRemoval==ScopeResult::Ok?Access::Commit(removal):preparedRemoval;
    const auto retained=Pending(fixture);Check(handoff==ScopeResult::Busy&&preparedRemoval==ScopeResult::Ok&&removed==ScopeResult::Busy,"actual lifecycle handoff and removal are gated by original pending record");
    Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==original.committedRevision&&retained.ownerSequence==original.ownerSequence&&retained.incarnation==original.incarnation&&retained.targets==original.targets,"all denied writes preserve exact original pending lineage and registry");
    Check(RuntimeScopeTestAccess::Revoke(*candidate.scope,candidate.decision.grantGeneration)==ScopeResult::Ok,"legitimate original command grant revoked before native repair");fixture.domain->RequestStop();
    // @summary Retain and verify the exact original record before launching its sole accepted repair action.
    Settlement::Batch repairView;Await([&]{return Commands::PendingCache(*fixture.manager,"fixture.owner",repairView)==Settlement::Result::Ok;},"original repair view retained before cache contention");
    std::array<sc13::core::Tgi,3> repairTargets{};std::size_t repairCopied{};const auto copiedRepair=Store::CopyTargets(repairView,repairTargets,repairCopied);const auto repairIdentity=Settlement::Observe(repairView);
    Check(copiedRepair==Settlement::Result::Ok&&repairCopied==3&&repairTargets==std::array<sc13::core::Tgi,3>{target,targetB,targetC}&&repairIdentity.result==Settlement::Result::Ok&&repairIdentity.phase==Settlement::Phase::CommittedPending&&repairIdentity.ownerSequence==original.ownerSequence&&repairIdentity.incarnation==original.incarnation&&repairIdentity.committedRevision==original.committedRevision,"repair view pins exact original pending lineage and complete affected payload");
    std::uint32_t initialActions{};Check(Store::CopyActionCount(repairView,initialActions)==Settlement::Result::Ok&&initialActions==0,"original repair record has no accepted action before launch");
    auto cacheGate=std::make_shared<Gate>(),storeGate=std::make_shared<Gate>();std::atomic_bool cacheExited{},storeExited{};
    std::future<void> holder,admission;std::future<Settlement::Observation> repair;
    bool cacheHeld{},actualTurn{},storeHeld{},cacheStillHeld{},storeStillHeld{},finished{};auto countStatus=Settlement::Result::Invalid;std::uint32_t acceptedActions{};Settlement::Observation passive,repaired;
    try{
        holder=std::async(std::launch::async,[&]{Cache::HoldCache(fixture.cache,[&]{cacheGate->Pause();cacheExited.store(true,std::memory_order_release);});});cacheHeld=Entered(cacheGate);
        passive=Commands::ObserveCache(*fixture.manager,"fixture.owner");repair=std::async(std::launch::async,[&]{return Commands::RepairCache(*fixture.manager,"fixture.owner");});
        // @summary Atomic observation cannot steal the store/record admission needed by this one-shot repair.
        const auto deadline=std::chrono::steady_clock::now()+2s;while(std::chrono::steady_clock::now()<deadline){countStatus=Store::CopyActionCount(repairView,acceptedActions);if(countStatus!=Settlement::Result::Ok||acceptedActions!=0){actualTurn=countStatus==Settlement::Result::Ok&&acceptedActions==1;break;}std::this_thread::yield();}
        if(actualTurn){admission=std::async(std::launch::async,[&]{Store::HoldAdmission([&]{storeGate->Pause();storeExited.store(true,std::memory_order_release);});});storeHeld=Entered(storeGate);}
        cacheStillHeld=cacheHeld&&!cacheExited.load(std::memory_order_acquire);cacheGate->Release();if(holder.valid())holder.get();finished=repair.wait_for(2s)==std::future_status::ready;
        storeStillHeld=storeHeld&&!storeExited.load(std::memory_order_acquire);
        std::cerr<<"automatic pending repair cacheHeld="<<cacheHeld<<" passiveResult="<<static_cast<unsigned>(passive.result)<<" passivePhase="<<static_cast<unsigned>(passive.phase)<<" countStatus="<<static_cast<unsigned>(countStatus)<<" actions="<<acceptedActions<<" actualTurn="<<actualTurn<<" storeHeld="<<storeHeld<<" cacheStillHeld="<<cacheStillHeld<<" finished="<<finished<<" storeStillHeld="<<storeStillHeld<<std::endl;
        storeGate->Release();if(admission.valid())admission.get();repaired=repair.get();
    }catch(...){
        // @summary Release both borrowed pauses before any future joins, preserving the original exception.
        cacheGate->Release();storeGate->Release();if(holder.valid())holder.wait();if(admission.valid())admission.wait();if(repair.valid())repair.wait();throw;
    }
    repairView.Reset();std::cerr<<"automatic pending repair result="<<static_cast<unsigned>(repaired.result)<<" phase="<<static_cast<unsigned>(repaired.phase)<<std::endl;
    Check(cacheHeld&&passive.result==Settlement::Result::Ok&&passive.phase==Settlement::Phase::CommittedPending&&actualTurn&&storeHeld&&cacheStillHeld&&finished&&storeStillHeld,"actual cache wait preserves responsive observation and repair completes while store admission stays held");
    Check(repaired.result==Settlement::Result::Ok&&repaired.phase==Settlement::Phase::Settled&&repaired.ownerSequence==original.ownerSequence&&repaired.incarnation==original.incarnation&&repaired.committedRevision==original.committedRevision,"only original native eviction settles exact retained obligation");
    Check(fixture.domain->Revision()==original.committedRevision&&fixture.registry.Current()==snapshot&&Cache::Epoch(fixture.cache,target)==std::optional<std::uint64_t>{epoch.value_or(0)+1}&&Cache::Epoch(fixture.cache,targetB)==std::optional<std::uint64_t>{1}&&Cache::Epoch(fixture.cache,targetC)==std::optional<std::uint64_t>{1},"revoked/stopped-D repair evicts full original union once without registry or grant replay");
    Check(Commands::RepairCache(*fixture.manager,"fixture.owner").result==Settlement::Result::NotFound,"already settled dropped-view obligation cannot evict again");
    Check(Access::Reclaim(next.publication)==ScopeResult::Ok,"denied handoff candidate reclaimed off guards");Commands::HandoffBarriers(candidate.vm,{},{});
}
/** @summary Real fixed-store capacity rejects automatic commands before marker, retained terminal views charge capacity until actual last release. @param package Verified Base. */
void AutomaticCacheCapacity(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);auto gate=std::make_shared<Gate>();auto holder=std::async(std::launch::async,[&]{Commands::HoldWorker(*fixture.manager,[gate]{gate->Pause();});});const bool held=Entered(gate);fixture.Activate(candidate);
    std::vector<Settlement::Batch> views;views.reserve(128);std::vector<Settlement::OwnerToken> owners;owners.reserve(128);Settlement::Result full=Settlement::Result::Ok;
    for(unsigned i=0;i<129;++i){Settlement::OwnerToken owner;Check(Store::IssueOwner(fixture.cache,owner)==Settlement::Result::Ok,"actual native capacity fixture token");Settlement::Batch batch;const auto result=Settlement::Reserve(owner,{targetB},batch);if(result==Settlement::Result::Capacity){full=result;break;}Check(result==Settlement::Result::Ok,"real capacity reservation");Check(Settlement::CancelPrepared(batch)==Settlement::Result::Ok,"capacity record becomes genuinely terminal but externally retained");owners.push_back(std::move(owner));views.push_back(std::move(batch));}
    const auto charged=StoreMetrics();const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry.Current();gate->Release();holder.get();Check(held&&full==Settlement::Result::Capacity&&charged.occupied==128&&!views.empty(),"real process ledger full including original pending and retained terminal views");
    Await([&]{return Commands::Observe(candidate.vm).attempts>=2;},"automatic original command attempts under actual capacity exhaustion");Check(fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot&&Commands::Observe(candidate.vm).commits==0,"capacity gate precedes actual D/stamp/registry publication");
    const auto released=views.size();views.clear();owners.clear();Await([&]{return Commands::Observe(candidate.vm).commits>=1;},"same original command recovers only after genuine terminal last-view release");Settled(fixture);Check(fixture.domain->Revision()==revision+1,"capacity recovery runs later real tick once");Value(candidate,true,9);Check(StoreMetrics().occupied<128,"finite actual reserve scan reclaimed released terminal records");std::cout<<"actual terminal capacity views released="<<released<<'\n';
}
/** @summary Cache reset preserves original Unqualified lineage and denies same participant publication/re-enrollment. @param package Verified Base. */
void AutomaticResetPending(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make("ticks+=1; if ticks==1 then "+bothTargets+" end",{},initialCounter);fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);ActivatePartial(fixture,candidate);const auto original=PartialFailure(fixture,candidate);
    fixture.cache.Clear();const auto mismatch=Commands::RepairCache(*fixture.manager,"fixture.owner");Check(mismatch.result==Settlement::Result::Ok&&mismatch.phase==Settlement::Phase::Unqualified&&mismatch.eviction==CacheEvictionStatus::PendingResetMismatch&&mismatch.ownerSequence==original.ownerSequence&&mismatch.incarnation==original.incarnation&&mismatch.committedRevision==original.committedRevision,"real cache reset cannot retoken or settle original committed obligation");
    auto next=fixture.Make("");fixture.Prepare(&candidate,next);const auto snapshot=fixture.registry.Current();const auto outcome=Access::Commit(next.publication);Check(outcome==ScopeResult::NotReady&&fixture.registry.Current()==snapshot&&fixture.domain->Revision()==2,"same original participant remains closed after reset rather than obtaining a replacement token");
    const auto still=Pending(fixture);Check(still.phase==Settlement::Phase::Unqualified&&still.ownerSequence==original.ownerSequence&&still.targets==3,"original full reset-mismatched payload remains rediscoverable after all command envelopes drop");Check(Access::Reclaim(next.publication)==ScopeResult::Ok,"reset-denied successor cleanup");Commands::HandoffBarriers(candidate.vm,{},{});
}
/** @summary Actual callback owner disposal occurs off lane and outside publication fences when closed Control is retained by VM. */
struct DisposalWitness final {
    /** @summary Independent observation storage survives both normal and exceptional callback-owner disposal. */
    struct Observation final{std::atomic_bool destroyed{},onLane{},inFence{};};
    std::shared_ptr<Observation> observation;
    /** @summary Retain only evidence, never the observed callback owner. @param value Independent observation storage. */
    explicit DisposalWitness(std::shared_ptr<Observation> value):observation(std::move(value)){}
    /** @summary Record the real destructor thread classification, without injecting runtime state. */
    ~DisposalWitness(){observation->onLane=ScriptRuntime::OnScriptLane();observation->inFence=Scheduler::InsideFence();observation->destroyed=true;}
};
/** @summary Queued/executing automatic close cuts drain original worker and retain only inert Control; never self-certify manager closure. @param package Verified Base. */
void AutomaticCloseCuts(const std::filesystem::path& package){
    for(unsigned cut=0;cut!=2;++cut){
        Fixture fixture(package);auto candidate=fixture.Make(set9);fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);auto gate=std::make_shared<Gate>();auto disposal=std::make_shared<DisposalWitness::Observation>();
        auto witness=std::make_shared<DisposalWitness>(disposal);
        if(cut==0)Close::ActionBarrier(*fixture.manager,[gate,witness]{gate->Pause();});else Commands::HandoffBarriers(candidate.vm,Once(gate),{});
        fixture.Activate(candidate);const bool reached=Entered(gate);const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();const auto handle=Close::Capture(*fixture.manager);const auto request=ManagerClose::Request(handle);
        auto retainedVm=candidate.vm;std::weak_ptr<ScriptRuntime> weakVm=retainedVm;witness.reset();auto close=std::async(std::launch::async,[&]{fixture.manager.reset();});const bool waiting=close.wait_for(20ms)==std::future_status::timeout;
        gate->Release();const bool joined=close.wait_for(2s)==std::future_status::ready;close.get();
        Check(reached&&request==ScopeResult::Ok&&waiting&&joined,"actual pending/executing original worker drains through close without lane/worker deadlock");Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"close after actual accepted cycle cannot publish stale output");
        if(cut==0)Check(disposal->destroyed&&!disposal->onLane&&!disposal->inFence,"last actual Control barrier owner disposed off lane and guards before worker join");
        Check(!weakVm.expired()&&Scheduler::Closed(retainedVm),"retained original VM observes joined closed slot and inert original Control");
        const auto before=fixture.domain->Revision();retainedVm->Tick(0);Check(fixture.domain->Revision()==before,"retained closed original VM cannot schedule replacement work");Commands::HandoffBarriers(candidate.vm,{},{});
    }
}
}
namespace {
/** @summary Real ReserveMarker retains original turn, acknowledges actual Advance without store reacquisition and cancels rejected preparation exactly. @param package Verified Base. */
void AutomaticMarkerTurn(const std::filesystem::path& package){
    for(unsigned deny=0;deny!=2;++deny){
        Fixture fixture(package);auto candidate=fixture.Make("ticks+=1; if ticks==1 then "+set9+" end",{},initialCounter);fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);
        auto markerGate=std::make_shared<Gate>();Commands::CachePreparedBarrier(candidate.vm,Once(markerGate));fixture.Activate(candidate);const bool prepared=Entered(markerGate);const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry.Current();const auto epoch=Cache::Epoch(fixture.cache,target);
        Settlement::Batch competing;const auto lookup=Commands::PendingCache(*fixture.manager,"fixture.owner",competing);const auto observation=Commands::ObserveCache(*fixture.manager,"fixture.owner");
        bool completedWhileHeld{};
        if(!deny){
            auto storeGate=std::make_shared<Gate>();auto admission=std::async(std::launch::async,[storeGate]{Store::HoldAdmission([storeGate]{storeGate->Pause();});});const bool held=Entered(storeGate);
            markerGate->Release();const auto deadline=std::chrono::steady_clock::now()+2s;while(std::chrono::steady_clock::now()<deadline){if(fixture.domain->Revision()==revision+1&&!fixture.manager->OperationBusy()){completedWhileHeld=true;break;}std::this_thread::yield();}
            storeGate->Release();admission.get();Check(held&&completedWhileHeld,"actual Advance/ack and final disposal do not reacquire held store admission");Settled(fixture);
            Check(fixture.domain->Revision()==revision+1&&Cache::Epoch(fixture.cache,target)==std::optional<std::uint64_t>{epoch.value_or(0)+1},"same original marker acknowledges and evicts once under unrelated store contention");
        }else{
            {
                auto publication=fixture.registry.TryReservePublication();const bool held=publication.owns_lock();markerGate->Release();const auto deadline=std::chrono::steady_clock::now()+2s;
                while(std::chrono::steady_clock::now()<deadline){if(!fixture.manager->OperationBusy()){completedWhileHeld=true;break;}std::this_thread::yield();}
                if(!held)completedWhileHeld=false;
            }
            Check(completedWhileHeld,"actual premarker final-R denial completes without waiting or orphaned preparation");Settled(fixture);
            Check(fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot&&Cache::Epoch(fixture.cache,target)==epoch&&Commands::Observe(candidate.vm).lastOutcome==ScopeResult::Busy,"only own revision-zero Prepared cancelled after genuine final-fence rejection");
        }
        Check(prepared&&lookup==Settlement::Result::Busy&&observation.result==Settlement::Result::Busy&&Settlement::Observe(competing).result==Settlement::Result::Invalid,"actual original marker turn excludes pending lookup and exposes no partial new view");Commands::CachePreparedBarrier(candidate.vm,{});
    }
}
/** @summary Actual Take/Fail/Stop/close cuts before ack and before Consume cannot merge, replay or publish stopped output. @param package Verified Base. */
void AutomaticOutputControls(const std::filesystem::path& package){
    for(unsigned phase=0;phase!=2;++phase)for(unsigned control=0;control!=4;++control){
        Fixture fixture(package);auto candidate=fixture.Make("ticks+=1; if ticks==1 then "+set9+" end",{},initialCounter);fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);
        auto ack=std::make_shared<Gate>(),consume=std::make_shared<Gate>();Commands::HandoffBarriers(candidate.vm,Once(ack),Once(consume));fixture.Activate(candidate);const bool captured=Entered(ack);
        bool workerCut{};if(phase){ack->Release();workerCut=Entered(consume);}
        const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry.Current();std::future<void> stopped;std::vector<ScriptPatchCommand> ordinary;ScopeResult requested=ScopeResult::Invalid;
        if(control==0)ordinary=candidate.vm->TakePatchCommands();
        if(control==1)candidate.vm->Fail("actual captured-output failure cut");
        if(control==2)stopped=std::async(std::launch::async,[&]{candidate.vm->Stop();});
        if(control==3)requested=ManagerClose::Request(Close::Capture(*fixture.manager));
        if(!phase){ack->Release();workerCut=Entered(consume);}
        bool stopDone=true;if(stopped.valid())stopDone=stopped.wait_for(2s)==std::future_status::ready;
        consume->Release();if(stopped.valid())stopped.get();Check(captured&&workerCut&&stopDone,"all real output-control gates released and queued control joined before oracles");
        Await([&]{return Commands::Observe(candidate.vm).attempts>=1&&!fixture.manager->OperationBusy();},"captured exact cycle completed once");
        if(control==0){Check(ordinary.empty(),"ordinary Take cannot steal captured automatic output at either cut");Check(fixture.domain->Revision()==revision+1,"Take alone leaves exact automatic output eligible");Value(candidate,true,9);}
        else {Check(fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot,"real failed/stopped/closed original cannot publish captured output");if(control==3)Check(requested==ScopeResult::Ok,"close request applied to original admitted anchor");}
        Await([&]{return Commands::Reservations(*candidate.scope)<=1;},"each actual control denial releases only its fresh ticket");Commands::HandoffBarriers(candidate.vm,{},{});
    }
}
/** @summary Closed ordinary manager admission cannot repair or abandon an existing original pending record. @param package Verified Base. */
void AutomaticClosedPending(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make("ticks+=1; if ticks==1 then "+bothTargets+" end",{},initialCounter);fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);ActivatePartial(fixture,candidate);const auto original=PartialFailure(fixture,candidate);
    Settlement::Batch retained;Await([&]{return Commands::PendingCache(*fixture.manager,"fixture.owner",retained)==Settlement::Result::Ok;},"actual original pending view retained before close admission");const auto snapshot=fixture.registry.Current();const auto handle=Close::Capture(*fixture.manager);Check(ManagerClose::Request(handle)==ScopeResult::Ok,"original ordinary ingress closed legitimately");
    const auto denied=Commands::RepairCache(*fixture.manager,"fixture.owner");const auto record=Settlement::Observe(retained);Check(denied.result==Settlement::Result::Unavailable&&record.result==Settlement::Result::Ok&&record.phase==Settlement::Phase::CommittedPending&&record.incarnation==original.incarnation&&record.committedRevision==original.committedRevision&&record.targets==3,"closed manager refuses new ordinary action but preserves exact original obligation");
    fixture.manager.reset();const auto after=Settlement::Observe(retained);Check(after.phase==Settlement::Phase::CommittedPending&&after.committedRevision==original.committedRevision&&fixture.domain->Revision()==original.committedRevision&&fixture.registry.Current()==snapshot,"manager destruction is not cache abandonment, repair or a registry replay");retained.Reset();
}
}
namespace {
/** @summary Borrowed executable-lifetime admission callback with externally retained joined context. */
struct AdmissionContext final {
    std::shared_ptr<Gate> gate=std::make_shared<Gate>();std::atomic_bool armed{};std::atomic_uint visits{};
    /** @summary Pause only one actual visit after its initial V release and before trying original W. @param raw Externally retained context, never owned on lane. */
    static void Pause(void* raw){auto& context=*static_cast<AdmissionContext*>(raw);++context.visits;if(context.armed.exchange(false))context.gate->Pause();}
};
/** @summary Own borrowed admission context through gate release, native holder joins, removal under V and a real lane barrier on every exit. */
struct AdmissionVisit final {
    AdmissionContext context;
    std::shared_ptr<ScriptRuntime> vm,laneWitness;
    std::shared_ptr<Gate> workerGate=std::make_shared<Gate>(),vmGate=std::make_shared<Gate>();
    std::future<void> worker,holder,laneJoin;
    bool installed{};
    /** @summary Install while this already constructed owner can clean up a partially completed installation. @param target Original route VM. @param witness Live independent VM on the same actual scheduler lane. */
    void Install(std::shared_ptr<ScriptRuntime> target,std::shared_ptr<ScriptRuntime> witness){vm=std::move(target);laneWitness=std::move(witness);installed=true;Commands::AdmissionBarrier(vm,&AdmissionContext::Pause,&context);}
    /** @summary Release every gate before joining threads, then prevent new borrowed callbacks and join the lane's preceding visit. Exceptions propagate on the ordinary path. */
    void Clear(){
        context.armed=false;context.gate->Release();workerGate->Release();vmGate->Release();
        if(worker.valid())worker.wait();if(holder.valid())holder.wait();if(laneJoin.valid())laneJoin.wait();
        if(installed){Commands::AdmissionBarrier(vm,nullptr,nullptr);Close::InvokeControl(laneWitness,[]{});installed=false;}
    }
    /** @summary Repeat safe cleanup during exceptional unwinding; inability to join fails the test process instead of destroying still-borrowed memory. */
    ~AdmissionVisit() noexcept{try{Clear();}catch(...){std::terminate();}}
};
/** @summary Force genuine final-V rejection with proven eligible/open original single worker and joined lane visit. @param package Verified Base. */
void AutomaticFinalV(const std::filesystem::path& package){
    Fixture fixture(package);auto ticks=std::make_shared<std::atomic_uint>();auto candidate=fixture.Make(set9,[ticks](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++*ticks;});fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);
    AdmissionMeter admission(candidate.vm);
    // @summary A real accepted UI operation proves the original worker is open; wait for its actual occupancy to drain.
    Check(fixture.manager->RequestAction("refresh"),"original worker genuinely accepts UI ingress before contention");Await([&]{return !fixture.manager->OperationBusy();},"actual UI occupancy drained before eligibility setup");
    Fixture independent(package,fixture.base,fixture.bridge);auto other=independent.Make("");independent.Prepare(nullptr,other);independent.Activate(other);
    AdmissionVisit visit;visit.Install(candidate.vm,other.vm);visit.worker=std::async(std::launch::async,[&]{Commands::HoldWorker(*fixture.manager,[gate=visit.workerGate]{gate->Pause();});});const bool wHeld=Entered(visit.workerGate);
    fixture.Activate(candidate);candidate.vm->Tick(0);std::this_thread::sleep_for(65ms);const auto beforeTicks=ticks->load();const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry.Current();const auto reservations=Commands::Reservations(*candidate.scope);
    visit.context.armed=true;const bool initialPassed=Entered(visit.context.gate);visit.workerGate->Release();visit.worker.get();bool wAvailable{},idleUnderW{};
    Commands::HoldWorker(*fixture.manager,[&]{wAvailable=true;idleUnderW=!fixture.manager->OperationBusy();});
    visit.holder=std::async(std::launch::async,[&]{Scheduler::Hold(candidate.vm,false,[gate=visit.vmGate]{gate->Pause();});});const bool finalHeld=Entered(visit.vmGate);
    visit.context.gate->Release();visit.laneJoin=std::async(std::launch::async,[&]{Close::InvokeControl(other.vm,[]{});});const bool joined=visit.laneJoin.wait_for(2s)==std::future_status::ready;
    const bool rejected=ticks->load()==beforeTicks&&fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot&&Commands::Reservations(*candidate.scope)==reservations&&!fixture.manager->OperationBusy();
    visit.vmGate->Release();visit.holder.get();visit.laneJoin.get();visit.Clear();
    Check(wHeld&&initialPassed&&wAvailable&&idleUnderW&&finalHeld&&joined&&rejected&&reservations==1,"eligible initial-V visit acquires genuinely open idle W but rejects actual held final-V without Lua/ticket/D/busy effect");
    Await([&]{return Commands::Observe(candidate.vm).commits>=1;},"a later ordinary eligible visit succeeds after final V release");Check(fixture.domain->Revision()==revision+1,"later visit is one real publication, not replay of rejected admission");
    Check(admission.Has(AdmissionReason::FinalV)&&admission.Has(AdmissionReason::Accepted),"genuine final-V rejection and accepted promotion counted through all local destruction");static_cast<void>(admission.Finish());
}
/** @summary Two genuine VMs share one original manager worker occupancy and both progress without a second queue. @param package Verified Base. */
void AutomaticSharedWorker(const std::filesystem::path& package){
    Fixture fixture(package);const std::string secondInitial="simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',1)";
    fixture.Write("simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',19)",secondInitial,"fixture.second");std::string error;Check(fixture.manager->Refresh(error),"second authored owner discovered by actual manager");
    auto firstTicks=std::make_shared<std::atomic_uint>(),secondTicks=std::make_shared<std::atomic_uint>();auto first=fixture.Make(set9,[firstTicks](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++*firstTicks;});fixture.Prepare(nullptr,first);Automatic(fixture,first);
    auto workerGate=std::make_shared<Gate>();auto hold=std::async(std::launch::async,[&]{Commands::HoldWorker(*fixture.manager,[workerGate]{workerGate->Pause();});});const bool held=Entered(workerGate);fixture.Activate(first);
    auto second=fixture.Make("simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',19)",[secondTicks](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++*secondTicks;},secondInitial,"fixture.second");fixture.Prepare(nullptr,second);
    Check(Commands::Install(*fixture.manager,Access::Origin(second.publication),second.decision)==ScopeResult::Ok,"second actual original route shares same worker");auto consumeGate=std::make_shared<Gate>();const auto once=Once(consumeGate);Commands::HandoffBarriers(first.vm,{},once);Commands::HandoffBarriers(second.vm,{},once);fixture.Activate(second);const auto revision=fixture.domain->Revision();workerGate->Release();hold.get();const bool occupied=Entered(consumeGate);
    const auto invocations=firstTicks->load()+secondTicks->load();const auto reservations=Commands::Reservations(*first.scope)+Commands::Reservations(*second.scope);const bool uiDenied=!fixture.manager->RequestAction("refresh");
    auto control=std::async(std::launch::async,[&]{Close::InvokeControl(first.vm,[]{});Close::InvokeControl(second.vm,[]{});});const bool controls=control.wait_for(2s)==std::future_status::ready;const bool truthful=fixture.manager->OperationBusy();consumeGate->Release();control.get();
    Check(held&&occupied&&invocations==1&&reservations==3&&uiDenied&&controls&&truthful,"one genuine shared occupancy excludes second Lua/ticket/UI job while both VM controls progress");
    Await([&]{return Commands::Observe(first.vm).commits>=1&&Commands::Observe(second.vm).commits>=1;},"both original VM visits eventually execute on same worker");Check(fixture.domain->Revision()==revision+2,"two original changed outputs each commit once");const auto current=fixture.registry.Current();const auto firstPatches=current->Find(target);Check(firstPatches&&std::any_of(firstPatches->begin(),firstPatches->end(),[](const auto& patch){return patch.owner=="fixture.owner"&&patch.patch.properties.front().value==9;}),"first exact owner script patch retained independently of second owner's declarative order");RegistryValue(fixture,targetB,19);
    Await([&]{return Commands::Reservations(*first.scope)==1&&Commands::Reservations(*second.scope)==1;},"both exact fresh command tickets retire to their own governing grants");Commands::HandoffBarriers(first.vm,{},{});Commands::HandoffBarriers(second.vm,{},{});
}
}
namespace {
/** @summary Actual selected original output rejects revoked/context/domain facts or same-ID successor without retargeting/replay. @param package Verified Base. */
void AutomaticOriginalFacts(const std::filesystem::path& package){
    for(unsigned mode=0;mode!=4;++mode){
        std::cerr<<"automatic original facts mode="<<mode<<std::endl;
        Fixture fixture(package);auto ticks=std::make_shared<std::atomic_uint>();auto candidate=fixture.Make(set9,[ticks](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++*ticks;});fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);auto gate=std::make_shared<Gate>();Commands::HandoffBarriers(candidate.vm,{},Once(gate));fixture.Activate(candidate);const bool captured=Entered(gate);
        if(mode==3){
            // @summary Keep the genuine fresh lifecycle lease and entire successor plan branch-local, disposed off guards before the reservation oracle.
            auto next=fixture.Make("");const auto originalRoot=candidate.decision;LifecycleDecision oldFresh,nextRoot;
            Check(LifecycleDecisionTestAccess::FreshDecision(candidate.scope,candidate.decision,oldFresh)==ScopeResult::Ok,"original facts handoff owns authentic fresh decision");
            Check(LifecycleDecisionTestAccess::RootDecision(next.scope,next.decision,nextRoot)==ScopeResult::Ok,"original facts successor retains existing root decision");
            fixture.Facts(candidate);fixture.Facts(next);std::string error;
            const auto prepared=Access::Prepare(*fixture.manager,std::move(next.preparation),oldFresh,nextRoot,candidate.facts,next.facts,next.publication,error);
            if(prepared!=ScopeResult::Ok)std::cerr<<"original facts mode=3 typed prepare ScopeResult="<<static_cast<unsigned>(prepared)<<" nativeError="<<error<<std::endl;
            Check(prepared==ScopeResult::Ok,"actual original facts typed lifecycle prepared");fixture.Activate(next);const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();const auto status=next.vm->Status();gate->Release();Await([&]{return !fixture.manager->OperationBusy()&&Commands::Observe(candidate.vm).attempts>=1;},"actual old automatic output consumed after successor publication");
            Check(captured&&fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision&&Commands::Observe(candidate.vm).commits==0,"captured original automatic ticket cannot adopt a same-ID successor");Check(next.vm->Status().diagnostic==status.diagnostic&&next.vm->Status().generation==status.generation&&next.vm->Status().generation!=candidate.vm->Status().generation,"original denial leaves successor diagnostics and distinct VM generation intact");Value(next,true,1);Check(Access::Reclaim(next.publication)==ScopeResult::Ok,"actual retired original cohort reclaimed after worker disposal");Check(candidate.decision==originalRoot,"original root decision is never renewed by typed handoff");
        }else{
            const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();const auto epoch=Cache::Epoch(fixture.cache,target);ScopeResult revoked=ScopeResult::Ok;
            if(mode==0)revoked=RuntimeScopeTestAccess::Revoke(*candidate.scope,candidate.decision.grantGeneration);if(mode==1)SemanticBridgeTestAccess::ObserveContext(*fixture.bridge,SemanticContextKind::City,7,4);if(mode==2)fixture.domain->RequestStop();gate->Release();
            Await([&]{return !fixture.manager->OperationBusy()&&Commands::Observe(candidate.vm).attempts>=1;},"actual original final Fence denies changed facts");std::this_thread::sleep_for(65ms);
            Check(captured&&revoked==ScopeResult::Ok&&fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision&&Cache::Epoch(fixture.cache,target)==epoch&&Commands::Observe(candidate.vm).commits==0&&ticks->load()==1,"genuine stale/revoked automatic output cannot Advance/evict or replay completed Lua");
        }
        Await([&]{return Commands::Reservations(*candidate.scope)<=1;},"original fact-denied ticket retires without root grant renewal");Commands::HandoffBarriers(candidate.vm,{},{});
    }
}
}
namespace {
/** @summary Observe coherent canonical identities using the existing later-defined helper. @param plan Genuine plan. @return Nonowning native facts. */
Access::Lineage Lineage(const ManagerPublication::Plan& plan);
/** @summary Compare nonreusable identity using the existing later-defined helper. @param a Original weak identity. @param b Current weak identity. @return Same nonempty control block. */
bool SameIdentity(const std::weak_ptr<const void>& a,const std::weak_ptr<const void>& b);
/** @summary Require original installed lineage using the existing later-defined helper. @param old Prior facts. @param next Current facts. */
void SameInstalled(const Access::Lineage& old,const Access::Lineage& next);
/** @summary Join a genuine worker pause before candidate/manager destruction, including assertion unwind. */
struct JoinedWorkerPause final {
    std::shared_ptr<Gate> gate=std::make_shared<Gate>();std::future<void> worker;
    /** @summary Hold actual W using the existing finite gate, never synthetic admission. @param fixture Live borrowed manager. */
    explicit JoinedWorkerPause(Fixture& fixture):worker(std::async(std::launch::async,[manager=fixture.manager.get(),pause=gate]{const auto end=std::chrono::steady_clock::now()+5s;while(std::chrono::steady_clock::now()<end){bool held{};Commands::HoldWorker(*manager,[&]{if(!manager->OperationBusy()){held=true;pause->Pause();}});if(held)return;std::this_thread::sleep_for(1ms);}})){
        const bool entered=Entered(gate);if(!entered)Release();Check(entered,"actual W entered for legal lifecycle capacity fixture");Held();
    }
    /** @summary Require that the original finite pause has not expired. */
    void Held(){Check(worker.valid()&&worker.wait_for(0ms)==std::future_status::timeout,"actual W remains held within original pause budget");}
    /** @summary Release before joining; propagate worker failures on ordinary completion. */
    void Release(){gate->Release();if(worker.valid())worker.get();}
    /** @summary Always release before waiting on unwind, while the borrowed manager is alive. */
    ~JoinedWorkerPause(){gate->Release();if(worker.valid())worker.wait();}
};
/** @summary Generate legal bounded authored manifest definitions. @param begin First actual numeric TGI instance. @param count Positive count within existing4096 capture bound. @return Unique authored float patches. */
std::string DeclarationRange(std::uint32_t begin,std::uint32_t count=4096){Check(count&&count<=4096,"declaration fixture respects original capture count");std::string result;result.reserve(800U*1024U);for(std::uint32_t i=0;i<count;++i)result+="[[patches]]\n[patches.target]\ntype=0x00B1B104\ngroup=0x61EFC000\ninstance="+std::to_string(begin+i)+"\n[[patches.properties]]\nid=0x09AE19D7\ntype='float'\noperation='set'\nvalue=3.0\n";return result;}
/** @summary Reach actual8193 affected targets through legal4096 lifecycle inputs and one genuine automatic extra target; reject before marker. @param package Verified Base. */
void ActualLifecycleTargetCapacity(const std::filesystem::path& package){
    Fixture fixture(package);const std::string initial="local extraAdded=false; simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)";
    auto old=fixture.Make("if not extraAdded then extraAdded=true; simoder.props.setFloat('00B1B104:61EFC000:719436BE','09AE19D7',9) end",{},initial,"fixture.owner",DeclarationRange(1,4095));fixture.Prepare(nullptr,old);Automatic(fixture,old);
    Access::Lineage activated;Settlement::Observation activationCache;
    {JoinedWorkerPause pause(fixture);fixture.Activate(old);pause.Held();activated=Lineage(old.publication);activationCache=Pending(fixture);Check(activated.active&&!fixture.registry.Current()->Find(targetB)&&!Cache::Epoch(fixture.cache,targetB)&&activationCache.result==Settlement::Result::NotFound,"legal4096 activation settled before actual extra script target");pause.Release();}
    Await([&]{return fixture.domain->Revision()==activated.domainRevision+1&&Commands::Observe(old.vm).commits>=1;},"genuine automatic extra target commits once");Settled(fixture);
    const auto grown=Lineage(old.publication);const auto grownCache=Pending(fixture);SameInstalled(activated,grown);
    Check(grown.active&&SameIdentity(activated.incarnation,grown.incarnation)&&!SameIdentity(activated.stamp,grown.stamp)&&grown.registration==activated.registration&&grown.generation==activated.generation&&grown.domainRevision==activated.domainRevision+1&&grown.recordRevision==activated.recordRevision+1,"actual automatic growth preserves original incarnation and advances exactly one original stamp");
    Check(grownCache.result==Settlement::Result::NotFound&&activationCache.ownerSequence&&grownCache.ownerSequence==activationCache.ownerSequence&&Cache::Epoch(fixture.cache,targetB)==std::optional<std::uint64_t>{1},"same native cache owner settles real extra target eviction");RegistryValue(fixture,target,1);RegistryValue(fixture,targetB,9);
    const auto grownSnapshot=fixture.registry.Current();std::size_t actualOldTargets=2;for(std::uint32_t i=1;i<=4095;++i){Check(grownSnapshot->Find(sc13::core::Tgi{0x00B1B104,0x61EFC000,i})!=nullptr,"every genuine old declarative target remains after automatic growth");++actualOldTargets;}Check(actualOldTargets==4097&&!grownSnapshot->Find(targetC),"actual old registry contains4095 declarations plus two real script targets");
    const std::string nextInitial="simoder.props.setFloat('00B1B104:61EFC000:719436BF','09AE19D7',1)";
    auto next=fixture.Make("",{},nextInitial,"fixture.owner",DeclarationRange(4096,4095));std::shared_ptr<const PatchRegistrySnapshot> before;std::uint64_t revision{};ScopeResult result=ScopeResult::Invalid;
    {JoinedWorkerPause pause(fixture);fixture.Prepare(&old,next);pause.Held();before=fixture.registry.Current();revision=fixture.domain->Revision();const auto retained=Pending(fixture);result=Access::Commit(next.publication);
        Check(retained.result==Settlement::Result::NotFound&&result==ScopeResult::NotReady&&fixture.registry.Current()==before&&fixture.domain->Revision()==revision,"actual lifecycle8193 union fails bounded target admission before irreversible marker");
        const auto unchanged=Lineage(old.publication);SameInstalled(grown,unchanged);Check(SameIdentity(grown.incarnation,unchanged.incarnation)&&SameIdentity(grown.stamp,unchanged.stamp)&&unchanged.domainRevision==grown.domainRevision&&unchanged.recordRevision==grown.recordRevision,"oversized rejected successor preserves exact grown native lineage and stamp");pause.Release();}
    const auto after=fixture.registry.Current();Check(after==before&&fixture.domain->Revision()==revision,"overflow rejection preserves registry and marker after worker resumes");
    Check(after->Find(sc13::core::Tgi{0x00B1B104,0x61EFC000,1})&&after->Find(sc13::core::Tgi{0x00B1B104,0x61EFC000,4095})&&!after->Find(sc13::core::Tgi{0x00B1B104,0x61EFC000,4096})&&!after->Find(sc13::core::Tgi{0x00B1B104,0x61EFC000,8190})&&after->Find(target)&&after->Find(targetB)&&!after->Find(targetC),"genuine old manifest and script targets remain and disjoint successor targets never publish");
    Check(Pending(fixture).result==Settlement::Result::NotFound,"oversized rejected lifecycle action exposes no partial Prepared record");Check(Access::Reclaim(next.publication)==ScopeResult::Ok,"genuine oversized successor candidate reclaimed without marker or limit override");
}
}
namespace {
/** @summary Same actual live cache rejects a second real manager State rather than replacing original canonical lineage. @param package Verified Base. */
void ActualCacheStateLatch(const std::filesystem::path& package){
    Fixture fixture(package);auto original=fixture.Make("");fixture.Prepare(nullptr,original);fixture.Activate(original);const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();PatchRegistry foreignRegistry;auto foreignDomain=std::make_shared<RuntimeCommitDomain>();
    /** @summary Reset foreign borrower before its borrowed cache/registry resources and restore original manager on every exit. */
    struct Restore final {
        Fixture& fixture;std::unique_ptr<ModManager> owner;
        /** @summary Move the original owner off the fixture without closing it. @param value Exact fixture. */
        explicit Restore(Fixture& value):fixture(value),owner(std::move(value.manager)){}
        /** @summary Destroy actual foreign manager before its registry and put original owner back. */
        ~Restore(){fixture.manager.reset();fixture.manager=std::move(owner);}
    } restore(fixture);
    fixture.manager=std::make_unique<ModManager>(fixture.root/"mods",fixture.root/"foreign-state.toon",foreignRegistry,fixture.cache,RuntimeResourceRefreshCallback{},ScriptScheduler::LogSink{},nullptr,foreignDomain);std::string error;Check(fixture.manager->Initialize(error)&&Access::Enroll(*fixture.manager)==ScopeResult::Ok,"second real State prepared against exact same actual cache");
    auto foreign=fixture.Make("");ManagerPublication::Plan denied;const auto result=Access::Prepare(*fixture.manager,std::move(foreign.preparation),OperationContext{},foreign.decision,SemanticContextLease{},foreign.facts,denied,error);
    Check(result==ScopeResult::Denied&&foreignDomain->Revision()==0&&fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot,"once-bound original State latch prevents a second native State from enrolling around original lineage");
    Check(Commands::ObserveCache(*restore.owner,"fixture.owner").result==Settlement::Result::NotFound,"denied foreign State does not create or erase original pending ownership");
}
}
/** @summary Execute only isolated delayed command qualification. @param argc Argument count. @param argv Verified Base path. @return Zero after all explicit oracles. */
namespace {
/** @summary Own a test-initiated ordinary native close and release its bounded gate before any future join on unwind. */
struct JoinedOwnerClose final {
    Fixture& fixture;std::shared_ptr<Gate> gate;std::future<void> closing;std::atomic_uint closeThread{};
    /** @summary Retain only fixture reference and independent gate through joined owner disposal. @param value Actual manager fixture. @param pause Real native pause. */
    JoinedOwnerClose(Fixture& value,std::shared_ptr<Gate> pause):fixture(value),gate(std::move(pause)){}
    /** @summary Start exactly one ordinary native manager destructor; no replacement retirement result is supplied. */
    void Begin(){closing=std::async(std::launch::async,[this]{closeThread=NativeObservation::Thread();fixture.manager.reset();});}
    /** @summary Resume actual operation and join original teardown before checking state. */
    void Join(){gate->Release();if(closing.valid())closing.get();}
    /** @summary Do not destroy borrowed gate/evidence before the actual native close has returned. */
    ~JoinedOwnerClose()noexcept{try{Join();}catch(...){std::terminate();}}
};
/** @summary Retain a complete actual removal plan through native reclamation, including exceptional paths after the lane pause has joined. */
struct JoinedRemovalReclaim final {
    ManagerPublication::Plan& plan;bool armed{};
    /** @summary Borrow the complete caller-owned plan; this owner must outlive the guard. @param value Genuine removal plan. */
    explicit JoinedRemovalReclaim(ManagerPublication::Plan& value):plan(value){}
    /** @summary Reclaim the exact prepared/consumed whole plan only after the later-declared pause owner has joined. */
    void Complete(){if(armed){const auto result=Access::Reclaim(plan);Check(result==ScopeResult::Ok,"whole original removal cleanup acknowledged");armed=false;}}
    /** @summary Never discard a required retired cohort after a test assertion throws. */
    ~JoinedRemovalReclaim()noexcept{try{Complete();}catch(...){std::terminate();}}
};
/** @summary Require actual native cohort registration, not wrapper or extra strong owner evidence. @param value Weak exact cohort. */
void CohortArmed(const Commands::Cohort& value){Check(value.preparationEntry&&value.stateEntry&&value.baseEntry&&!value.preparation.expired()&&!value.state.expired()&&!value.base.expired()&&!value.origin.expired(),"actual promoted Plan, real State and singleton Base observed without ownership");}
/** @summary Require joined actual last-owner expiry and actual destructor thread/guard records. @param value Exact original cohort. @param mainThread Actual test disposer. @param closeThread Actual joined native close owner, zero if unused. @param workerThread Actual original worker, zero if not observed. */
void CohortDestroyed(const Commands::Cohort& value,std::uint32_t mainThread,std::uint32_t closeThread=0,std::uint32_t workerThread=0){
    Check(value.preparation.expired()&&value.state.expired()&&value.base.expired()&&value.origin.expired(),"actual complete original Plan/State/Base/origin weak owners expire after joined cleanup");
    for(const auto handle:{value.preparationEntry,value.stateEntry,value.baseEntry}){
        const auto actual=NativeObservation::ObserveDestruction(handle);
        Check(actual.entries==1&&actual.thread!=0&&(actual.thread==mainThread||actual.thread==closeThread||actual.thread==workerThread)&&!actual.onLane&&!actual.inFence&&!actual.underWorker,"real owning type destructor executes once on a known off-lane disposer outside actual publication and W guards");
        NativeObservation::ForgetDestruction(handle);
    }
}
/** @summary Exercise genuinely accepted pending, detached and executed original cancellation without releasing the required whole cohort early. @param package Exact pinned Base. */
void AutomaticLifetimeCuts(const std::filesystem::path& package){
    for(unsigned cut=0;cut!=4;++cut){
        Fixture fixture(package);auto ticks=std::make_shared<std::atomic_uint>();auto candidate=fixture.Make(set9,[ticks](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++*ticks;});fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);
        const auto cohort=Commands::WatchCohort(*fixture.manager,Access::Origin(candidate.publication),fixture.base);CohortArmed(cohort);AdmissionMeter admission(candidate.vm);
        auto gate=std::make_shared<Gate>();auto queue=std::make_shared<Commands::QueueObservation>();auto workerThread=std::make_shared<std::atomic_uint>();
        if(cut<2)Commands::BeforeDetach(*fixture.manager,std::make_shared<const std::function<void(Commands::QueueObservation)>>([gate,queue,workerThread](Commands::QueueObservation actual){*queue=actual;*workerThread=NativeObservation::Thread();gate->Pause();}));
        else if(cut==2)Close::ActionBarrier(*fixture.manager,[gate,workerThread]{*workerThread=NativeObservation::Thread();gate->Pause();});
        else Commands::HandoffBarriers(candidate.vm,Once(gate),{});
        auto originalOwner=Lifetime::Capture(*fixture.manager,"fixture.owner");Check(!Lifetime::Closed(originalOwner),"valid original owner is live before genuine close");
        JoinedOwnerClose close(fixture,gate);const auto originalLatch=Commands::ActionClosed(*fixture.manager);Check(static_cast<bool>(originalLatch),"real original close-admission latch captured");fixture.Activate(candidate);const bool reached=Entered(gate);
        Check(reached,"genuine original pending/detached/captured cancellation cut reached");
        const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();auto retainedVm=candidate.vm;const auto mainThread=NativeObservation::Thread();
        if(cut<2)Check(queue->scheduled&&queue->busy&&!queue->stopping&&!queue->closed&&queue->thread==workerThread->load()&&ticks->load()==0&&Commands::Observe(candidate.vm).attempts==0&&Commands::Reservations(*candidate.scope)==1,"actual accepted job is still pending before detach, with no Lua/Select/ticket");
        if(cut==2)Check(ticks->load()==0&&Commands::Observe(candidate.vm).attempts==0,"actual detached predecessor has not selected or run Lua");
        if(cut==3)Check(ticks->load()==1&&ScriptRuntime::OnScriptLane()==false,"actual original Lua ran once before held acknowledgment");
        // @summary A preack pause is on the lane, so remove matching now but defer the real lane join until original close completes.
        NativeObservation::StopAdmission(admission.handle);
        if(cut==1)Check(ManagerClose::Request(Close::Capture(*fixture.manager))==ScopeResult::Ok&&originalLatch->load(),"real request closes original admission after actual promotion but before detach");
        close.Begin();
        if(cut<3)Await([&]{return originalLatch->load();},"ordinary teardown reached real original action close latch before gate release");
        else Await([&]{return Lifetime::Closed(originalOwner);},"original State close gate reached while genuine preack lease remains held");
        Check(close.closing.wait_for(20ms)==std::future_status::timeout,"original close waits for the held genuine native operation");
        close.Join();originalOwner=Lifetime::Plan{};static_cast<void>(admission.Finish());
        Check(Scheduler::Closed(retainedVm)&&fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision&&Commands::Reservations(*candidate.scope)<=1,"joined actual close releases original VM slot without stale publication or fresh ticket retention");
        Check(!cohort.preparation.expired()&&!cohort.state.expired(),"whole authentic activation Plan retains original cohort through native close");
        Commands::HandoffBarriers(retainedVm,{},{});candidate=Candidate{};
        Check(cohort.preparation.expired()&&cohort.state.expired()&&cohort.origin.expired()&&!cohort.base.expired(),"after joined native close complete Plan release reclaims actual preparation/State/origin while fixture Base remains");
        const auto before=fixture.domain->Revision();retainedVm->Tick(0);Check(Scheduler::Closed(retainedVm)&&fixture.domain->Revision()==before,"remaining VM retains only inert original Control and cannot schedule a new action");
        retainedVm.reset();admission.vm.reset();fixture.base.reset();CohortDestroyed(cohort,mainThread,close.closeThread.load(),workerThread->load());
    }
}
/** @summary Reject a genuinely retired original visit, then reclaim the whole exact Plan and observe actual expiry; separately cover real closed-W admission. @param package Exact pinned Base. */
void AutomaticRetirementAdmission(const std::filesystem::path& package){
    for(unsigned closed=0;closed!=2;++closed){
        std::cerr<<"automatic retirement admission closed="<<closed<<std::endl;
        Fixture fixture(package);auto ticks=std::make_shared<std::atomic_uint>();auto candidate=fixture.Make(set9,[ticks](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Tick)++*ticks;});fixture.Prepare(nullptr,candidate);Automatic(fixture,candidate);
        Commands::Cohort cohort;if(!closed){cohort=Commands::WatchCohort(*fixture.manager,Access::Origin(candidate.publication),fixture.base);CohortArmed(cohort);}
        Fixture independent(package,fixture.base,fixture.bridge);auto other=independent.Make("");independent.Prepare(nullptr,other);independent.Activate(other);AdmissionMeter admission(candidate.vm,other.vm);
        ManagerPublication::Plan removal;JoinedRemovalReclaim reclaim(removal);AdmissionVisit visit;visit.Install(candidate.vm,other.vm);
        visit.worker=std::async(std::launch::async,[&]{Commands::HoldWorker(*fixture.manager,[gate=visit.workerGate]{gate->Pause();});});Check(Entered(visit.workerGate),"original W held while actual removal is prepared");fixture.Activate(candidate);
        if(!closed){std::string error;const auto prepared=Access::Remove(*fixture.manager,Access::Origin(candidate.publication),removal,error);reclaim.armed=prepared==ScopeResult::Ok;Check(prepared==ScopeResult::Ok,"complete genuine original removal plan prepared before the admission cut");}
        std::this_thread::sleep_for(65ms);visit.context.armed=true;Check(Entered(visit.context.gate),"actual original scheduler visit paused after initial V");visit.workerGate->Release();visit.worker.get();
        const auto revision=fixture.domain->Revision();const auto ticksBefore=ticks->load();const auto snapshot=fixture.registry.Current();
        if(closed)Check(ManagerClose::Request(Close::Capture(*fixture.manager))==ScopeResult::Ok,"actual original ingress closed while the accepted initial-V visit remains paused");
        else Check(Access::Commit(removal)==ScopeResult::Ok,"native removal commits while lane is paused without synchronous VM reclaim");
        const auto removedRevision=fixture.domain->Revision();const auto removedSnapshot=fixture.registry.Current();
        if(!closed)Check(removedRevision==revision+1&&!cohort.origin.expired()&&!cohort.preparation.expired(),"actual removal marker retains whole original retirement cohort");
        visit.Clear();Check(admission.Has(closed?AdmissionReason::Closed:AdmissionReason::Ineligible),"resumed actual visit rejects real closed W or genuine retired final eligibility");static_cast<void>(admission.Finish());
        Check(ticks->load()==ticksBefore&&fixture.domain->Revision()==removedRevision&&fixture.registry.Current()==removedSnapshot&&Commands::Reservations(*candidate.scope)<=1,"resumed rejected original has no Lua, ticket, D, registry or cache replay");
        if(closed){Check(removedRevision==revision&&removedSnapshot==snapshot,"close request itself publishes no D or patch mutation");continue;}
        reclaim.Complete();Check(Scheduler::Closed(candidate.vm),"joined original visit precedes genuine whole-plan native Reclaim");
        auto retainedVm=candidate.vm;const auto mainThread=NativeObservation::Thread();removal=ManagerPublication::Plan{};candidate=Candidate{};
        Check(cohort.origin.expired()&&cohort.preparation.expired()&&!cohort.state.expired(),"real original origin/preparation expire after whole-plan Reclaim while live manager still owns State");
        fixture.manager.reset();Check(cohort.state.expired()&&Scheduler::Closed(retainedVm),"actual State expires while original VM retains inert Control");
        independent.manager.reset();other=Candidate{};independent.base.reset();visit.vm.reset();visit.laneWitness.reset();admission.vm.reset();admission.laneWitness.reset();retainedVm.reset();fixture.base.reset();CohortDestroyed(cohort,mainThread);
    }
}
/** @summary Preserve actual cache owner identity through repaired command output, successful successor, removal and reactivation. @param package Exact pinned Base. */
void AutomaticSuccessfulLineage(const std::filesystem::path& package){
    Fixture fixture(package);auto original=fixture.Make("ticks+=1; if ticks==1 then "+bothTargets+" end",{},initialCounter);fixture.Prepare(nullptr,original);Automatic(fixture,original);ActivatePartial(fixture,original);const auto pending=PartialFailure(fixture,original);
    Settlement::Batch retained;Await([&]{return Commands::PendingCache(*fixture.manager,"fixture.owner",retained)==Settlement::Result::Ok;},"original pending lineage view captured");
    const auto repaired=Commands::RepairCache(*fixture.manager,"fixture.owner");Check(repaired.result==Settlement::Result::Ok&&repaired.phase==Settlement::Phase::Settled&&repaired.ownerSequence==pending.ownerSequence&&repaired.incarnation==pending.incarnation&&repaired.committedRevision==pending.committedRevision,"exact original pending lineage genuinely repaired");
    Commands::HandoffBarriers(original.vm,{},{});const auto oldGeneration=original.vm->Status().generation;auto next=fixture.Make("");fixture.Prepare(&original,next);const auto beforeHandoff=fixture.domain->Revision();fixture.Activate(next);Settled(fixture);
    const auto afterHandoff=Commands::ObserveCache(*fixture.manager,"fixture.owner");Check(afterHandoff.result==Settlement::Result::NotFound&&afterHandoff.ownerSequence==pending.ownerSequence&&fixture.domain->Revision()==beforeHandoff+1&&next.vm!=original.vm&&next.vm->Status().generation!=oldGeneration,"successful actual successor preserves original cache owner with one marker and distinct VM");Value(next,true,1);Check(Access::Reclaim(next.publication)==ScopeResult::Ok,"whole successful handoff plan genuinely reclaims predecessor");
    ManagerPublication::Plan removal;std::string error;Check(Access::Remove(*fixture.manager,Access::Origin(next.publication),removal,error)==ScopeResult::Ok,"successful successor removal prepared");const auto beforeRemove=fixture.domain->Revision();Check(Access::Commit(removal)==ScopeResult::Ok,"successful genuine successor removal committed");Settled(fixture);
    const auto afterRemove=Commands::ObserveCache(*fixture.manager,"fixture.owner");Check(afterRemove.result==Settlement::Result::NotFound&&afterRemove.ownerSequence==pending.ownerSequence&&fixture.domain->Revision()==beforeRemove+1&&!fixture.registry.Current()->Find(target)&&!fixture.registry.Current()->Find(targetB)&&!fixture.registry.Current()->Find(targetC),"actual inactive installed participant preserves owner lineage and removes all old patches exactly once");Check(Access::Reclaim(removal)==ScopeResult::Ok,"whole genuine removal reclaim completes");
    auto active=fixture.Make("");fixture.Prepare(nullptr,active);const auto beforeActive=fixture.domain->Revision();fixture.Activate(active);Settled(fixture);const auto afterActive=Commands::ObserveCache(*fixture.manager,"fixture.owner");
    Check(afterActive.result==Settlement::Result::NotFound&&afterActive.ownerSequence==pending.ownerSequence&&fixture.domain->Revision()==beforeActive+1,"same installed participant reactivation preserves original cache owner lineage");Value(active,true,1);
    const auto terminal=Settlement::Observe(retained);Check(terminal.phase==Settlement::Phase::Settled&&terminal.ownerSequence==pending.ownerSequence&&terminal.incarnation==pending.incarnation&&terminal.committedRevision==pending.committedRevision,"retained original terminal record is immutable through later successful lifecycle markers");
}
}

namespace {
/** @summary Compare genuine ownership identities without taking a strong reference or depending on reusable raw addresses. @param a Original weak identity. @param b Current weak identity. @return Same nonempty control-block identity. */
bool SameIdentity(const std::weak_ptr<const void>& a,const std::weak_ptr<const void>& b){const std::weak_ptr<const void> empty;return (a.owner_before(empty)||empty.owner_before(a))&&!a.owner_before(b)&&!b.owner_before(a);}
/** @summary Read actual original canonical identities with finite transient contention only. @param plan Genuine retained plan. @return Coherent nonowning facts. */
Access::Lineage Lineage(const ManagerPublication::Plan& plan){Access::Lineage result;ScopeResult outcome=ScopeResult::Busy;Await([&]{outcome=Access::ReadLineage(plan,result);return outcome!=ScopeResult::Busy;},"actual lineage observation terminates");Check(outcome==ScopeResult::Ok,"actual canonical lineage available");return result;}
/** @summary Require persistent real installed membership and scheduler lineage across successful native lifecycle. @param old Original observation. @param next Successor observation. */
void SameInstalled(const Access::Lineage& old,const Access::Lineage& next){Check(SameIdentity(old.state,next.state)&&SameIdentity(old.participant,next.participant)&&SameIdentity(old.installation,next.installation)&&SameIdentity(old.joint,next.joint)&&SameIdentity(old.node,next.node),"same actual State, participant, installation, joint binding and scheduler node retained");}
/** @summary Perform a complete real release-only removal and joined whole-plan reclaim, retaining cleanup on assertion unwind. @param fixture Original manager. @param candidate Actual active original. @param removal Empty complete caller-owned holder. */
void RemoveJoined(Fixture& fixture,Candidate& candidate,ManagerPublication::Plan& removal){JoinedRemovalReclaim reclaim(removal);std::string error;const auto prepared=Access::Remove(*fixture.manager,Access::Origin(candidate.publication),removal,error);reclaim.armed=prepared==ScopeResult::Ok;Check(prepared==ScopeResult::Ok,"genuine same-participant removal prepared");Check(Access::Commit(removal)==ScopeResult::Ok,"genuine same-participant removal committed");reclaim.Complete();Check(Scheduler::Closed(candidate.vm),"whole original removal acknowledged slot release");}
/** @summary Own a passive wrapper cut on the actual preparation thread; clear the thread-local callback on every return and exception. */
struct PromotionHook final{
    /** @summary Install only independent finite gate state. @param gate Joined test pause. @param fail Throw a genuine fixture exception after gate release when requested. */
    PromotionHook(const std::shared_ptr<Gate>& gate,bool fail){Access::BeforePromotion([gate,fail]{gate->Pause();if(fail)throw std::runtime_error("fixture.early-promotion");});}
    /** @summary Retire the callback outside native guards on the same worker thread. */
    ~PromotionHook(){Access::BeforePromotion({});}
};
/** @summary Keep original candidate/manager and independent pause alive until the real preparation thread joins, also on assertion unwind. */
struct JoinedPromotion final{
    Fixture& fixture;Candidate& candidate;std::shared_ptr<Gate> gate=std::make_shared<Gate>();std::future<ScopeResult> operation;
    /** @summary Borrow actual owners, which must be declared before this join guard. @param owner Live original manager fixture. @param value Original candidate. */
    JoinedPromotion(Fixture& owner,Candidate& value):fixture(owner),candidate(value){}
    /** @summary Start the actual canonical wrapper with original decisions and facts. @param fail Select a real thrown callback exception without native state mutation. */
    void Start(bool fail=false){operation=std::async(std::launch::async,[this,fail]{PromotionHook hook(gate,fail);std::string error;return Access::Prepare(*fixture.manager,std::move(candidate.preparation),{},candidate.decision,{},candidate.facts,candidate.publication,error);});}
    /** @summary Release and obtain the real result after callback retirement. @return Actual wrapper outcome; original exceptions propagate. */
    ScopeResult Finish(){gate->Release();return operation.get();}
    /** @summary Never let borrowed candidate/manager or gate die while preparation is running. */
    ~JoinedPromotion(){gate->Release();if(operation.valid())operation.wait();}
};
/** @summary Prove all actual installed lineage identities, fresh incarnation and rejection of old routes/tickets/plans after reactivation. @param package Verified singleton Base checkout. */
void ReactivationIdentityAndReplay(const std::filesystem::path& package){
    Fixture fixture(package);auto old=fixture.Make(set9);fixture.Prepare(nullptr,old);Capture capture;Install(fixture,old,capture);fixture.Activate(old);capture.Wait();const auto initial=Lineage(old.publication);const auto cacheOwner=Commands::ObserveCache(*fixture.manager,"fixture.owner").ownerSequence;
    ManagerPublication::Plan removal;RemoveJoined(fixture,old,removal);const auto removed=Lineage(removal);SameInstalled(initial,removed);Check(!removed.active&&removed.recordRevision==initial.recordRevision+1&&removed.domainRevision==initial.domainRevision+1&&SameIdentity(initial.incarnation,removed.incarnation),"real removal preserves original incarnation tombstone with one marker");
    auto next=fixture.Make("");fixture.Prepare(nullptr,next);fixture.Activate(next);const auto active=Lineage(next.publication);SameInstalled(initial,active);Check(active.active&&!SameIdentity(initial.incarnation,active.incarnation)&&!SameIdentity(removed.stamp,active.stamp)&&next.vm!=old.vm&&next.scope!=old.scope&&active.generation!=initial.generation&&active.registration>initial.registration&&active.recordRevision==removed.recordRevision+1&&active.domainRevision==removed.domainRevision+1,"new same-slot activation changes genuine generation/incarnation/stamp and advances revisions");Value(next,true,1);
    Check(Commands::ObserveCache(*fixture.manager,"fixture.owner").ownerSequence==cacheOwner,"reactivated installed participant retains exact original cache owner");const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();
    Check(Commands::Consume(capture.batch)!=ScopeResult::Ok&&Commands::Consume(capture.batch)==ScopeResult::Conflict,"original delayed ticket rejects once and cannot retarget new incarnation");capture.batch=ScriptCommandBatch{};
    Check(Access::Commit(old.publication)!=ScopeResult::Ok&&Access::Commit(removal)!=ScopeResult::Ok,"consumed original activation/removal plans cannot replay");ManagerPublication::Plan staleRemoval;std::string error;Check(Access::Remove(*fixture.manager,Access::Origin(old.publication),staleRemoval,error)!=ScopeResult::Ok&&Access::Commit(staleRemoval)!=ScopeResult::Ok,"retired original origin cannot remove reactivated owner");Check(Access::Reclaim(staleRemoval)==ScopeResult::Ok,"rejected unowned plan retains ordinary cleanup disposition");
    old.vm->Tick(0);Check(Scheduler::Closed(old.vm)&&fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"retained retired original VM/route cannot execute or publish");Value(next,true,1);ManagerPublication::Plan secondRemoval;RemoveJoined(fixture,next,secondRemoval);SameInstalled(initial,Lineage(secondRemoval));
}
/** @summary Competing real candidates and foreign authentic sources cannot refresh a removed slot or cross its installed identity. @param package Verified Base. */
void ReactivationCompetingAndForeign(const std::filesystem::path& package){
    Fixture fixture(package);auto old=fixture.Make("");fixture.Prepare(nullptr,old);fixture.Activate(old);ManagerPublication::Plan removal;RemoveJoined(fixture,old,removal);
    auto first=fixture.Make("");auto second=fixture.Make("");fixture.Prepare(nullptr,first);fixture.Activate(first);const auto before=Lineage(first.publication);const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();std::string error;
    const auto stale=Access::Prepare(*fixture.manager,std::move(second.preparation),{},second.decision,{},second.facts,second.publication,error);Check(stale==ScopeResult::Stale&&Access::Commit(second.publication)!=ScopeResult::Ok,"second authentic preparation against the old removed stamp cannot refresh or expose executable output");Check(Access::Reclaim(second.publication)==ScopeResult::Ok,"rejected unowned candidate does not invent reclamation");
    Fixture foreign(package,fixture.base,fixture.bridge);auto foreignCandidate=foreign.Make("");const auto foreignRevision=foreign.domain->Revision();ManagerPublication::Plan wrong;Check(Access::Prepare(*fixture.manager,std::move(foreignCandidate.preparation),{},foreignCandidate.decision,{},foreignCandidate.facts,wrong,error)!=ScopeResult::Ok&&Access::Commit(wrong)!=ScopeResult::Ok,"authentic other manager with identical textual owner cannot cross original State/installation");Check(Access::Reclaim(wrong)==ScopeResult::Ok&&foreign.domain->Revision()==foreignRevision,"foreign rejection leaves exact original candidate untouched by reclamation");
    Check(OwnerLifecycleTestAccess::CommitJoint({},Access::Shared(first.publication))!=ScopeResult::Ok,"detached wrapper cannot reinterpret canonical installed binding");Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"competing/foreign/replayed attempts preserve actual patches and marker");const auto after=Lineage(first.publication);SameInstalled(before,after);Check(SameIdentity(before.stamp,after.stamp)&&SameIdentity(before.incarnation,after.incarnation)&&before.registration==after.registration,"failed authentic attempts preserve exact active stamp/incarnation/registration");Value(first,true,1);
}
/** @summary Exercise the actual original-snapshot gap with unchanged control, same-participant cycle, real second-participant domain change, and thrown-cut cleanup. @param package Verified Base. */
void ReactivationOriginalSnapshot(const std::filesystem::path& package){
    for(unsigned mode=0;mode!=4;++mode){
        Fixture fixture(package);std::string error;if(mode==2){fixture.Write("","simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)","fixture.second");Check(fixture.manager->Refresh(error),"actual second participant discovered before sealed lifecycle");}
        auto old=fixture.Make("");fixture.Prepare(nullptr,old);fixture.Activate(old);ManagerPublication::Plan removal;RemoveJoined(fixture,old,removal);const auto original=Lineage(removal);auto candidate=fixture.Make("");JoinedPromotion attempt(fixture,candidate);attempt.Start(mode==3);Check(Entered(attempt.gate),"actual wrapper paused off guards after original snapshot check");
        if(mode==1||mode==2){const auto owner=mode==1?"fixture.owner":"fixture.second";auto intervening=fixture.Make("",{},"simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',1)",owner);fixture.Prepare(nullptr,intervening);fixture.Activate(intervening);ManagerPublication::Plan interveningRemoval;RemoveJoined(fixture,intervening,interveningRemoval);}
        const auto beforeResume=Lineage(removal);SameInstalled(original,beforeResume);if(mode==1)Check(!SameIdentity(original.stamp,beforeResume.stamp)&&beforeResume.domainRevision==original.domainRevision+2,"genuine intervening activation/removal replaces original stamp and D despite identical final empty slot/order");if(mode==2)Check(SameIdentity(original.stamp,beforeResume.stamp)&&beforeResume.domainRevision==original.domainRevision+2,"real second participant changes D while original participant stamp remains exactly unchanged");
        const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();ScopeResult result=ScopeResult::Invalid;bool threw{};try{result=attempt.Finish();}catch(const std::runtime_error& exception){threw=std::string_view(exception.what())=="fixture.early-promotion";if(!threw)throw;}
        if(mode==0){Check(result==ScopeResult::Ok&&!threw,"unchanged original stamp/domain succeeds through real early cut");fixture.Activate(candidate);const auto current=Lineage(candidate.publication);SameInstalled(original,current);Check(current.active&&current.domainRevision==revision+1&&current.recordRevision==original.recordRevision+1,"unchanged-pair control has one genuine activation marker");Value(candidate,true,1);}
        else{Check(mode==3?threw:result==ScopeResult::Stale,"genuine original snapshot mismatch or actual callback exception is retained");Check(Access::Commit(candidate.publication)!=ScopeResult::Ok,"failed wrapper output remains noncommittable");Check(Access::Reclaim(candidate.publication)==ScopeResult::Ok,"failed output preserves established genuine unowned/partial reclamation path");Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision,"failed original candidate publishes no refreshed marker or registry state");const auto after=Lineage(removal);Check(SameIdentity(beforeResume.stamp,after.stamp)&&!after.active&&after.recordRevision==beforeResume.recordRevision,"failed original candidate preserves exact removed canonical stamp");auto fresh=fixture.Make("");fixture.Prepare(nullptr,fresh);fixture.Activate(fresh);Value(fresh,true,1);}
    }
}
}

namespace {
/** @summary Keep the detached fixture's candidate-specific tombstone and joint binding distinct from canonical successful-removal reactivation. @param package Verified Base. */
void ReactivationDetachedBoundary(const std::filesystem::path& package){
    Fixture fixture(package);auto domain=std::make_shared<RuntimeCommitDomain>();auto registry=std::make_shared<PatchRegistry>();ScriptScheduler scheduler;Check(Scheduler::Enroll(scheduler,domain)==ScopeResult::Ok,"real independent detached scheduler enrolled");
    /** @summary Own genuine detached native fixture inputs with no canonical preparation proof. */
    struct Detached final{Candidate candidate;std::shared_ptr<PublicationOrigin> origin;};
    /** @summary Construct an actual generic fixture VM and existing reviewed grant; never a canonical proof or issuer. @return Real distinct candidate. */
    const auto make=[&]{Detached value;auto& c=value.candidate;c.scope=std::shared_ptr<RuntimeScope>(RuntimeScopeTestAccess::Create(domain));c.scope->SetReadiness(true);auto host=fixture.base->Bind(c.scope);c.endpoint=host.semantic;c.decision=RuntimeScopeTestAccess::Issue(*c.scope);SemanticBridgeTestAccess::Issue(*c.endpoint,c.decision);ScriptDefinition definition;definition.entry="scripts/init.luau";definition.language="luau";definition.memoryBytes=2U*1024U*1024U;definition.budgetMs=20;definition.bytecode=CompileLuauSource("return {onTick=function() end,onDisable=function() end}");c.vm=std::make_shared<ScriptRuntime>("fixture.detached",definition,std::move(host));ResourcePatchDefinition patch;patch.target=target;patch.properties.push_back({0x09AE19D7,PropertyType::Float,PatchOperation::Set,1.0F});value.origin=OwnedPublicationTestAccess::Issue(c.vm,c.endpoint,"fixture-detached",{patch});fixture.Facts(c);return value;};
    auto first=make(),other=make();const auto record=OwnerLifecycleTestAccess::IssueCandidate(first.origin,registry,domain);const auto foreign=OwnerLifecycleTestAccess::IssueCandidate(other.origin,registry,domain);Check(record&&foreign,"two genuine candidate-specific detached slots issued before sealing");
    /** @summary Invoke the unchanged detached adapter with exact genuine candidate or original removal. @param slot Genuine detached record. @param old Original owner or null. @param next Candidate or null. @param output Empty complete plan. @return Actual native outcome. */
    const auto prepare=[&](const auto& slot,Detached* old,Detached* next,OwnerLifecycle::Plan& output){std::string error;return OwnerLifecycleTestAccess::Joint(scheduler,slot,old?old->origin:nullptr,next?next->origin:nullptr,old?old->candidate.decision:OperationContext{},next?next->candidate.decision:OperationContext{},old?old->candidate.facts:SemanticContextLease{},next?next->candidate.facts:SemanticContextLease{},output,error);};
    OwnerLifecycle::Plan mismatch;const auto initial=registry->Current();Check(prepare(record,nullptr,&other,mismatch)!=ScopeResult::Ok&&OwnerLifecycleTestAccess::CommitJoint(record,mismatch)!=ScopeResult::Ok&&OwnerLifecycleTestAccess::Reclaim(mismatch)==ScopeResult::Ok,"wrong candidate cannot use detached initial tombstone or gain cleanup authority");Check(!Scheduler::Closed(other.candidate.vm)&&registry->Current()==initial&&domain->Revision()==0,"mismatched detached origin leaves genuine candidate and publication intact");
    OwnerLifecycle::Plan activated;Check(prepare(record,nullptr,&first,activated)==ScopeResult::Ok&&OwnerLifecycleTestAccess::CommitJoint(record,activated)==ScopeResult::Ok,"original candidate-specific detached activation remains supported");const auto active=registry->Current();const auto activeRevision=domain->Revision();Check(OwnerLifecycleTestAccess::CommitJoint(foreign,activated)!=ScopeResult::Ok&&registry->Current()==active&&domain->Revision()==activeRevision,"real foreign detached record cannot reinterpret original joint plan");
    OwnerLifecycle::Plan removal;
    /** @summary Retain a whole genuine detached removal through acknowledged reclamation on every exit. */
    struct Reclaim final{OwnerLifecycle::Plan& plan;bool armed{};/** @summary Reclaim after all native reservations unwind. */void Complete(){if(armed){Check(OwnerLifecycleTestAccess::Reclaim(plan)==ScopeResult::Ok,"genuine whole detached removal reclaimed");armed=false;}}/** @summary Preserve reclamation on assertion unwind. */~Reclaim(){try{Complete();}catch(...){std::terminate();}}} reclaim{removal};
    const auto prepared=prepare(record,&first,nullptr,removal);reclaim.armed=prepared==ScopeResult::Ok;Check(prepared==ScopeResult::Ok&&OwnerLifecycleTestAccess::CommitJoint(record,removal)==ScopeResult::Ok,"original detached removal committed");reclaim.Complete();const auto removed=registry->Current();const auto removedRevision=domain->Revision();OwnerLifecycle::Plan replacement;
    Check(prepare(record,nullptr,&other,replacement)!=ScopeResult::Ok&&OwnerLifecycleTestAccess::CommitJoint(record,replacement)!=ScopeResult::Ok&&OwnerLifecycleTestAccess::Reclaim(replacement)==ScopeResult::Ok,"canonical removed-slot exception never widens detached predecessor tombstone into arbitrary new candidate authority");OwnerLifecycle::Plan wrongBinding;Check(prepare(foreign,nullptr,&other,wrongBinding)!=ScopeResult::Ok&&OwnerLifecycleTestAccess::CommitJoint(foreign,wrongBinding)!=ScopeResult::Ok&&OwnerLifecycleTestAccess::Reclaim(wrongBinding)==ScopeResult::Ok,"genuine empty scheduler node retains original joint binding against a distinct detached record");Check(!Scheduler::Closed(other.candidate.vm)&&registry->Current()==removed&&domain->Revision()==removedRevision,"both detached negative paths retain genuine unowned candidate without marker/patch changes");scheduler.Shutdown();
}
}

int main(int argc,char** argv){try{Check(argc==2,"verified Base checkout required");const std::filesystem::path package=argv[1];Progress("PreparationContention");PreparationContention(package);Progress("Publication");Publication(package);Progress("Denials");Denials(package);Progress("Contention");Contention(package);Progress("Handoff");Handoff(package);Progress("SelectionDelay");SelectionDelay(package);Progress("StaleHandoffAndRead");StaleHandoffAndRead(package);Progress("NoChangeAndMissing");NoChangeAndMissing(package);Progress("Lease");Lease(package);Progress("AbortAndException");AbortAndException(package);Progress("InvalidPayload");InvalidPayload(package);Progress("CachePending");CachePending(package);Progress("DeferredCapacity");DeferredCapacity(package);Progress("Repeated");Repeated(package);Progress("AutomaticPublication");AutomaticPublication(package);Progress("AutomaticOutputIsolation");AutomaticOutputIsolation(package);Progress("AutomaticQuota");AutomaticQuota(package);Progress("AutomaticSemanticOutput");AutomaticSemanticOutput(package);Progress("AutomaticOccupancy");AutomaticOccupancy(package);Progress("AutomaticVmContention");AutomaticVmContention(package);Progress("AutomaticFinalV");AutomaticFinalV(package);Progress("AutomaticSharedWorker");AutomaticSharedWorker(package);Progress("AutomaticPendingRepair");AutomaticPendingRepair(package);Progress("AutomaticCacheCapacity");AutomaticCacheCapacity(package);Progress("ActualLifecycleTargetCapacity");ActualLifecycleTargetCapacity(package);Progress("ActualCacheStateLatch");ActualCacheStateLatch(package);Progress("AutomaticMarkerTurn");AutomaticMarkerTurn(package);Progress("AutomaticResetPending");AutomaticResetPending(package);Progress("AutomaticOutputControls");AutomaticOutputControls(package);Progress("AutomaticClosedPending");AutomaticClosedPending(package);Progress("AutomaticOriginalFacts");AutomaticOriginalFacts(package);Progress("AutomaticCloseCuts");AutomaticCloseCuts(package);Progress("AutomaticLifetimeCuts");AutomaticLifetimeCuts(package);Progress("AutomaticRetirementAdmission");AutomaticRetirementAdmission(package);Progress("AutomaticSuccessfulLineage");AutomaticSuccessfulLineage(package);Progress("ReactivationIdentityAndReplay");ReactivationIdentityAndReplay(package);Progress("ReactivationCompetingAndForeign");ReactivationCompetingAndForeign(package);Progress("ReactivationOriginalSnapshot");ReactivationOriginalSnapshot(package);Progress("ReactivationDetachedBoundary");ReactivationDetachedBoundary(package);Check(fenceAllocations==0&&fenceFrees==0,"native C++ fence allocation/free probes remain zero");std::cout<<checks<<" manager command checks passed\n";return 0;}catch(const std::exception& error){failAllocation=false;failAfterDomain=nullptr;std::cerr<<"manager commands: "<<error.what()<<'\n';return 1;}}
