#include "mods/manager_publication.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include "mod_fixture.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
std::atomic_size_t fenceAllocations{},fenceFrees{},fenceCallbacks{};
thread_local bool failAllocation{};
}
/** @summary Count native C++ allocations in the existing publication fence. @param size Requested bytes. @return Owned storage. */
void* operator new(std::size_t size){
    if(failAllocation){failAllocation=false;throw std::bad_alloc();}
    if(sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceAllocations;
    if(void* value=std::malloc(size?size:1))return value;throw std::bad_alloc();
}
/** @summary Account for native array allocations. @param size Requested bytes. @return Owned storage. */
void* operator new[](std::size_t size){return ::operator new(size);}
/** @summary Count native final frees while publication participants remain reserved. @param value Released storage. */
void operator delete(void* value) noexcept{if(value&&sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceFrees;std::free(value);}
/** @summary Match array ownership. @param value Released storage. */
void operator delete[](void* value) noexcept{::operator delete(value);}
/** @summary Match sized scalar ownership. @param value Released storage. @param size Allocator hint. */
void operator delete(void* value,std::size_t size) noexcept{static_cast<void>(size);::operator delete(value);}
/** @summary Match sized array ownership. @param value Released storage. @param size Allocator hint. */
void operator delete[](void* value,std::size_t size) noexcept{static_cast<void>(size);::operator delete(value);}

namespace {
using namespace sc13::mods;
using namespace sc13::runtime;
using namespace sc13::scripting;
using Access=ManagerPublicationTestAccess;
using Preparation=ManagerPreparationTestAccess;
using Lifetime=ManagerLifetimeTestAccess;
using Scheduler=SchedulerPublicationTestAccess;
using namespace std::chrono_literals;
std::atomic_uint checks{};
const char* scenarioLabel="startup";unsigned scenarioIndex{},makeOrdinal{};const char* preparationOperation="none";
/** @summary Flush passive fixed scenario evidence without changing native admission. @param label Group. @param index Existing subcase. */
void Progress(const char* label,unsigned index=0){scenarioLabel=label;scenarioIndex=index;std::cerr<<"manager publication begin "<<label<<" subcase="<<index<<std::endl;}
/** @summary Distinguish capture from candidate construction without catching or retrying Busy. @param operation Fixed operation. @param begin Advance real Make ordinal. */
void PreparationProgress(const char* operation,bool begin=false){if(begin)++makeOrdinal;preparationOperation=operation;std::cerr<<"manager publication preparation group="<<scenarioLabel<<" subcase="<<scenarioIndex<<" make="<<makeOrdinal<<" operation="<<operation<<std::endl;}
const sc13::core::Tgi target{0x00B1B104,0x61EFC000,0x719436BD};
/** @summary Require an observed behavior. @param actual Observation. @param message Fixed failure diagnostic. */
void Check(bool actual,const char* message){++checks;if(!actual)throw std::runtime_error(message);}
/** @summary Require a native exception without converting denial into authority. @param action Attempt. @param message Fixed diagnostic. */
template<class F> void Reject(F action,const char* message){bool rejected{};try{action();}catch(const std::exception&){rejected=true;}Check(rejected,message);}
/** @summary Pin every executable local Base input before constructing its real interpreter. @param root Checkout. @return Complete exact pin. */
BasePackagePin Pin(const std::filesystem::path& root){
    BasePackagePin pin{"0.1.0-alpha",{}};
    pin.files.emplace("mod.toml",HashText(ReadBounded(root/"mod.toml",4096)));
    for(const auto& item:std::filesystem::recursive_directory_iterator(root/"scripts"))if(item.is_regular_file())
        pin.files.emplace(item.path().lexically_relative(root).generic_string(),HashText(ReadBounded(item.path(),1024U*1024U)));
    return pin;
}
/** @summary Retain exact prepared candidate and independently issued fixture reservations. */
struct Candidate final{
    ManagerPreparation::Plan preparation;
    std::shared_ptr<RuntimeScope> scope;
    std::shared_ptr<SemanticEndpoint> endpoint;
    std::shared_ptr<ScriptRuntime> vm;
    OperationContext decision;
    SemanticContextLease facts;
};
/** @summary Own only synthetic files and actual native manager/Base participants, never a game or OS VM. */
struct Fixture final{
    std::filesystem::path root=sc13::tests::CreateTemporaryDirectory();
    PatchRegistry registry;
    RuntimeResourceCache cache;
    std::shared_ptr<RuntimeCommitDomain> domain=std::make_shared<RuntimeCommitDomain>();
    std::shared_ptr<SemanticBridge> bridge=std::make_shared<SemanticBridge>();
    BasePackagePin pin;
    std::shared_ptr<BaseHost> base;
    std::atomic_uint disables{},closes{},ticks{};
    std::unique_ptr<ModManager> manager;
    /** @summary Start real Base and discover actual inactive entries; synthetic authority remains fixture-only. @param package Verified checkout. */
    explicit Fixture(const std::filesystem::path& package,bool unrelated=false):pin(Pin(package)){
        SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::Runtime,1,1);
        SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,7,3);
        base=std::make_shared<BaseHost>(package,pin,std::function<void(ScriptPhase,std::uint64_t)>{},bridge);
        Check(base->Ready(),"actual Base ready");Write(1);if(unrelated)Write(5,"fixture.other");
        manager=std::make_unique<ModManager>(root/"mods",root/"state.toon",registry,cache,
            RuntimeResourceRefreshCallback{},ScriptScheduler::LogSink{},nullptr,domain);
        std::string error;Check(manager->Initialize(error),"actual manager discovered");
        Check(Access::Enroll(*manager)==ScopeResult::Ok,"actual scheduler enrollment");
    }
    /** @summary Destroy native borrowers before counters/cache/registry and remove only owned temporary inputs. */
    ~Fixture(){manager.reset();base.reset();std::error_code error;std::filesystem::remove_all(root,error);}
    /** @summary Replace captured candidate bytes; no active source migration is implied. @param value Script patch. @param owner Installed owner. */
    void Write(float value,const std::string& owner="fixture.owner"){
        const auto directory=root/"mods"/owner;std::filesystem::create_directories(directory/"scripts");
        sc13::tests::WriteTextFile(directory/"mod.toml",sc13::tests::FixtureHeader(owner,"Canonical")+
            "[scripts]\nlanguage='luau'\nentry='scripts/init.luau'\nmemoryMiB=2\nbudgetMs=20\n"+
            sc13::tests::FixturePatch(3));
        sc13::tests::WriteTextFile(directory/"scripts/init.luau",
            "simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',"+std::to_string(value)+")\n"
            "return {onTick=function() end,onDisable=function() end}");
    }
    /** @summary Obtain exact factual lease without issuing permission. @param candidate Original endpoint. */
    void Facts(Candidate& candidate){
        copied::Envelope value;
        Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]})",
            "test.echo@1",candidate.endpoint->storageOwner,value)==copied::Status::Ok,"factual reference decode");
        Check(bridge->BindReferences(*candidate.endpoint,value,candidate.facts)==ScopeResult::Ok,"original factual lease");
    }
    /** @summary Prepare restricted VM with exact installed capture and existing synthetic authority. @param value Candidate value. @param owner Actual key. @return Retained candidate. */
    Candidate Make(float value,const std::string& owner="fixture.owner"){
        Write(value,owner);Candidate candidate;
        PreparationProgress("Capture",true);auto capture=Preparation::Capture(*manager,owner,root/"mods"/owner,pin);
        PreparationProgress("Candidate");candidate.preparation=Access::Candidate(*manager,capture,base,[this](ScriptPhase phase,std::uint64_t){
            if(Scheduler::InsideFence())++fenceCallbacks;
            if(phase==ScriptPhase::Disable)++disables;
            if(phase==ScriptPhase::Close)++closes;
            if(phase==ScriptPhase::Tick)++ticks;
        });
        PreparationProgress("CandidateCompleted");candidate.scope=Preparation::Scope(candidate.preparation);candidate.endpoint=Preparation::Endpoint(candidate.preparation);
        candidate.vm=Preparation::Vm(candidate.preparation);
        candidate.scope->SetReadiness(true);
        candidate.decision=RuntimeScopeTestAccess::Issue(*candidate.scope);
        Check(candidate.decision.owner!=0,"existing sole fixture decision evaluator");
        Facts(candidate);return candidate;
    }
    /** @summary Refresh an existing grant reservation after an actual D marker. @param candidate Original grant owner. */
    void Refresh(Candidate& candidate){
        candidate.decision=RuntimeScopeTestAccess::Reserve(*candidate.scope,candidate.decision);
        Check(candidate.decision.owner!=0,"original grant refresh");Facts(candidate);
    }
    /** @summary Prepare through actual canonical manager and the one shared lifecycle algorithm. @param old Predecessor or null. @param next Successor. @param plan Empty output. @return Native outcome. */
    ScopeResult Prepare(Candidate* old,Candidate& next,ManagerPublication::Plan& plan){
        std::string error;return Access::Prepare(*manager,std::move(next.preparation),old?old->decision:OperationContext{},
            next.decision,old?old->facts:SemanticContextLease{},next.facts,plan,error);
    }
    /** @summary Prepare release-only exact current/tombstone removal. @param origin Native identity. @param plan Empty output. @return Native outcome. */
    ScopeResult Remove(const std::shared_ptr<PublicationOrigin>& origin,ManagerPublication::Plan& plan){
        std::string error;return Access::Remove(*manager,origin,plan,error);
    }
};
/** @summary Retry only bounded transient native Busy; never renew a rejected grant. @param plan Original one-shot plan. @return Publication outcome. */
ScopeResult Commit(ManagerPublication::Plan& plan){
    ScopeResult result=ScopeResult::Busy;for(unsigned i=0;i!=4096&&result==ScopeResult::Busy;++i){result=Access::Commit(plan);if(result==ScopeResult::Busy)std::this_thread::yield();}
    if(result!=ScopeResult::Ok)std::cerr<<"bounded Commit rejected with ScopeResult="<<static_cast<unsigned>(result)<<'\n';return result;
}
/** @summary Obtain a coherent copied observation with bounded Busy retries. @param plan Native binding. @return Jointly guarded facts. */
Access::Observation Observe(const ManagerPublication::Plan& plan){
    Access::Observation output;ScopeResult result=ScopeResult::Busy;
    for(unsigned i=0;i!=4096&&result==ScopeResult::Busy;++i){result=Access::Observe(plan,output);if(result==ScopeResult::Busy)std::this_thread::yield();}
    Check(result==ScopeResult::Ok,"actual guarded observation");return output;
}
/** @summary Require matching actual manager/scheduler/registry/scope generation. @param plan Native binding. @param candidate Active candidate or null. @param value Effective script value. */
void Coherent(const ManagerPublication::Plan& plan,const Candidate* candidate,float value=0){
    auto observed=Observe(plan);Check(observed.registry->domain_revision()==observed.domainRevision,"registry and original D agree");
    Check(observed.projectionMatches&&observed.orderMatches,"actual M guarded projection and entire order coherent");
    const auto patches=observed.registry->Find(target);
    if(candidate){
        Check(observed.origin==Access::Origin(plan)&&observed.runtime==candidate->vm&&observed.running,"canonical stamp/scheduler/status agree");
        Check(observed.scope.activationCommitted&&!observed.scope.stopping,"actual scope agrees");
        Check(observed.ownerInOrder&&observed.orderSize==1,"actual M guarded owner order agrees");
        Check(patches&&!patches->empty()&&patches->back().owner=="fixture.owner","actual canonical patch present");
        const auto& properties=patches->back().patch.properties;
        Check(properties.size()==1&&properties[0].value==value,"authored and staged patches use shared effective reducer");
        Check(patches->size()==2&&patches->front().owner=="fixture.owner"&&patches->front().patch.properties.front().value==3,"captured authored patch preserved ahead of script declaration");
    }else Check(!observed.origin&&!observed.runtime&&!patches&&!observed.ownerInOrder&&observed.orderSize==0,"removal exposes no active participant");
}
/** @summary Qualify initial activation, same-installation handoff and exact release-only tombstone removal. @param package Actual Base checkout. */
void Lifecycle(const std::filesystem::path& package){
    Fixture fixture(package);auto old=fixture.Make(1);ManagerPublication::Plan initial;
    const auto epoch=fixture.bridge->Epoch(),generation=fixture.bridge->ReadyGeneration();
    Check(fixture.Prepare(nullptr,old,initial)==ScopeResult::Ok,"initial canonical prepare");
    Check(!old.vm->Status().running&&!old.scope->Current().activationCommitted&&!fixture.registry.IsActive("fixture.owner"),"candidate not visible early");
    Reject([&]{old.vm->Commit();},"ordinary Commit cannot promote prepared candidate");
    Reject([&]{Access::Scheduler(*fixture.manager).Put("fixture.owner",old.vm);},"textual Put cannot promote prepared candidate");
    SchedulerPublicationPlan standalone;
    Check(Scheduler::Prepare(Access::Scheduler(*fixture.manager),"fixture.owner",old.vm,SchedulerPublicationPurpose::Activate,standalone)!=ScopeResult::Ok,"standalone typed candidate promotion denied");
    Check(OwnerLifecycleTestAccess::Commit({},Access::Shared(initial))!=ScopeResult::Ok&&
        OwnerLifecycleTestAccess::CommitJoint({},Access::Shared(initial))!=ScopeResult::Ok,"foreign fixture wrapper cannot commit actual canonical plan");
    const auto initialOutcome=Commit(initial);
    if(initialOutcome!=ScopeResult::Ok||fixture.domain->Revision()!=1)
        std::cerr<<"initial outcome="<<static_cast<unsigned>(initialOutcome)<<" D revision="<<fixture.domain->Revision()<<" expected outcome=0 revision=1\n";
    Check(initialOutcome==ScopeResult::Ok&&fixture.domain->Revision()==1,"initial single D marker");Coherent(initial,&old,1);
    Check(Access::Commit(initial)!=ScopeResult::Ok,"initial replay denied");
    Check(fixture.manager->EnabledOrder()==std::vector<ModId>{"fixture.owner"},"actual enabled order initial projection");
    fixture.Refresh(old);auto next=fixture.Make(9);ManagerPublication::Plan handoff;
    Check(fixture.Prepare(&old,next,handoff)==ScopeResult::Ok,"actual same ID handoff prepare");
    Check(Commit(handoff)==ScopeResult::Ok&&fixture.domain->Revision()==2,"handoff single D marker");Coherent(handoff,&next,9);
    Check(old.scope->Current().stopping&&!old.vm->Status().running,"predecessor execution revoked");
    Check(fixture.disables==0&&!Scheduler::Closed(old.vm),"retirement deferred beyond fence");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok&&fixture.disables==1&&Scheduler::Closed(old.vm),"old callback and close once");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok&&fixture.disables==1,"retirement reclaim idempotent");
    Check(fixture.bridge->Epoch()==epoch&&fixture.bridge->ReadyGeneration()==generation,"same actual Base generation preserved");
    Check(RuntimeScopeTestAccess::Revoke(*next.scope,next.decision.grantGeneration)==ScopeResult::Ok,"exact committed permission revoked");
    next.vm->Fail("isolated failure");next.scope->RequestStop();fixture.bridge->Close();
    ManagerPublication::Plan removal;Check(fixture.Remove(Access::Origin(handoff),removal)==ScopeResult::Ok,"release-only removal after Fail revoke Base close");
    Check(Commit(removal)==ScopeResult::Ok&&fixture.domain->Revision()==3,"removal single D marker");Coherent(removal,nullptr);
    Check(fixture.manager->EnabledOrder().empty(),"actual removal order projection");
    Check(Access::Reclaim(removal)==ScopeResult::Ok&&fixture.disables==2&&Scheduler::Closed(next.vm),"failed committed owner disable exactly once");
    const auto snapshot=fixture.registry.Current();ManagerPublication::Plan repeated;
    Check(fixture.Remove(Access::Origin(handoff),repeated)==ScopeResult::Ok,"exact tombstone removal preparation");
    Check(Commit(repeated)==ScopeResult::Ok&&fixture.domain->Revision()==3&&fixture.registry.Current()==snapshot,"repeated removal has no marker");
    Check(Scheduler::ReleaseEmpty(Access::Scheduler(*fixture.manager),"fixture.owner")==ScopeResult::Denied,"legacy empty node release denied");
}
/** @summary Reject every actual D/M/S/V/R/B/Q/T contender without partial publication. @param package Actual Base. */
void Contention(const std::filesystem::path& package){
    for(unsigned participant=0;participant!=8;++participant){Progress("Contention",participant);
        Fixture fixture(package);auto candidate=fixture.Make(1);ManagerPublication::Plan plan;
        Check(fixture.Prepare(nullptr,candidate,plan)==ScopeResult::Ok,"contention preparation");
        const auto snapshot=fixture.registry.Current();ScopeResult outcome=ScopeResult::Invalid;
        const auto attempt=[&]{auto pending=std::async(std::launch::async,[&]{return Access::Commit(plan);});
            Check(pending.wait_for(2s)==std::future_status::ready,"actual contention must not wait");return pending.get();};
        if(participant==0){auto lock=fixture.domain->ReserveWriter();outcome=attempt();}
        if(participant==1){auto lock=Lifetime::HoldManager(*fixture.manager);outcome=attempt();}
        if(participant==2)Scheduler::HoldScheduler(Access::Scheduler(*fixture.manager),[&]{outcome=attempt();});
        if(participant==3)Scheduler::Hold(candidate.vm,false,[&]{outcome=attempt();});
        if(participant==4){auto lock=fixture.registry.TryReservePublication();outcome=attempt();}
        if(participant==5){auto lock=SemanticBridgeTestAccess::ReserveObservation(*fixture.bridge);outcome=attempt();}
        if(participant==6){auto lock=RuntimeScopeTestAccess::ReserveState(*candidate.scope);outcome=attempt();}
        if(participant==7)Scheduler::Hold(candidate.vm,true,[&]{outcome=attempt();});
        Check(outcome==ScopeResult::Busy&&fixture.domain->Revision()==0&&fixture.registry.Current()==snapshot,"contender changed no participant");
        Check(!candidate.vm->Status().running&&!candidate.scope->Current().activationCommitted&&fixture.manager->EnabledOrder().empty(),"contender opened no admission or order");
        Check(Commit(plan)==ScopeResult::Ok&&fixture.domain->Revision()==1,"same plan survives transient contention");Coherent(plan,&candidate,1);
    }
}
/** @summary Ensure stale decisions/context/record/domain and closed dependencies cannot publish. @param package Actual Base. */
void Stale(const std::filesystem::path& package){
    for(unsigned scenario=0;scenario!=9;++scenario){Progress("Stale",scenario);
        Fixture fixture(package);auto candidate=fixture.Make(1);ManagerPublication::Plan plan;
        Check(fixture.Prepare(nullptr,candidate,plan)==ScopeResult::Ok,"stale case preparation");
        if(scenario==0)Check(Lifetime::ReplaceStamp(*fixture.manager,"fixture.owner"),"replace actual canonical stamp");
        if(scenario==1){auto writer=fixture.domain->ReserveWriter();Check(writer.Acquired()&&writer.Advance()!=0,"external domain marker");}
        if(scenario==2)candidate.scope->RequestStop();
        if(scenario==3)SemanticBridgeTestAccess::ObserveContext(*fixture.bridge,SemanticContextKind::City,7,3);
        if(scenario==4)fixture.bridge->Close();
        if(scenario==5)candidate.vm->Stop();
        if(scenario==6)Access::Scheduler(*fixture.manager).Shutdown();
        if(scenario==7)fixture.domain->RequestStop();
        if(scenario==8)Check(RuntimeScopeTestAccess::Revoke(*candidate.scope,candidate.decision.grantGeneration)==ScopeResult::Ok,"candidate exact grant revoke");
        const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry.Current();
        Check(Access::Commit(plan)!=ScopeResult::Ok,"stale actual plan denied");
        Check(fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot&&!candidate.scope->Current().activationCommitted,"stale candidate has no publication effects");
        Check(Access::Reclaim(plan)==ScopeResult::Ok&&Scheduler::Closed(candidate.vm),"stale candidate fully reclaimed");
        Check(fixture.disables==0,"never committed candidate has no disable");
    }
}
/** @summary Prove pre-conversion capture witnesses reject foreign installation, stale source and actual membership ABA. @param package Actual Base. */
void CaptureIdentity(const std::filesystem::path& package){
    Fixture fixture(package);
    auto foreign=CapturedCandidate::Capture(fixture.root/"mods/fixture.owner",InstallationBinding::Create(),fixture.pin);
    Reject([&]{PreparationProgress("CandidateForeign");auto candidate=Access::Candidate(*fixture.manager,foreign,fixture.base);},"same path and ID foreign installation denied");
    auto wrong=fixture.pin;wrong.files.begin()->second=std::string(64,'A');
    PreparationProgress("CaptureWrongPin");auto incorrect=Preparation::Capture(*fixture.manager,"fixture.owner",fixture.root/"mods/fixture.owner",wrong);
    Reject([&]{PreparationProgress("CandidateWrongPin");auto candidate=Access::Candidate(*fixture.manager,incorrect,fixture.base);},"wrong exact Base content denied");
    auto candidate=fixture.Make(1);fixture.Write(4);std::string error;Check(fixture.manager->Refresh(error),"actual inactive refresh");
    ManagerPublication::Plan stale;Check(fixture.Prepare(nullptr,candidate,stale)!=ScopeResult::Ok,"legacy fingerprint change denies old candidate");
    auto before=fixture.Make(5);std::filesystem::remove_all(fixture.root/"mods/fixture.owner");
    Check(fixture.manager->Refresh(error),"actual entry erase");fixture.Write(6);Check(fixture.manager->Refresh(error),"actual entry recreate");
    ManagerPublication::Plan aba;Check(fixture.Prepare(nullptr,before,aba)!=ScopeResult::Ok,"actual same ID membership ABA denied");
}
/** @summary Reject substitution of original endpoint scope by another legitimate same-domain fixture scope. @param package Actual Base. */
void ScopeSubstitution(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make(1);
    auto replacement=std::shared_ptr<RuntimeScope>(RuntimeScopeTestAccess::Create(fixture.domain));replacement->SetReadiness(true);
    candidate.endpoint->scope=replacement;ManagerPublication::Plan denied;
    const auto snapshot=fixture.registry.Current();const auto result=fixture.Prepare(nullptr,candidate,denied);
    candidate.endpoint->scope=candidate.scope;
    Check(result!=ScopeResult::Ok&&fixture.domain->Revision()==0&&fixture.registry.Current()==snapshot&&fixture.manager->EnabledOrder().empty(),"same domain legitimate scope cannot replace exact initialized scope");
}
/** @summary Prevent canonical metadata sealing during the real legacy Enable off-M initializer window. @param package Actual Base. */
void LegacyInitializationRace(const std::filesystem::path& package){
    Fixture fixture(package);const auto source=fixture.root/"mods/fixture.owner/scripts/init.luau";
    sc13::tests::WriteTextFile(source,"return {}");std::string error;
    Check(fixture.manager->Refresh(error),"real legacy empty initializer definition refreshed");
    Candidate candidate;
    PreparationProgress("Capture",true);const auto raceCapture=Preparation::Capture(*fixture.manager,"fixture.owner",fixture.root/"mods/fixture.owner",fixture.pin);
    PreparationProgress("Candidate");candidate.preparation=Access::Candidate(*fixture.manager,raceCapture,fixture.base);PreparationProgress("CandidateCompleted");
    candidate.scope=Preparation::Scope(candidate.preparation);candidate.vm=Preparation::Vm(candidate.preparation);
    candidate.endpoint=Preparation::Endpoint(candidate.preparation);
    candidate.scope->SetReadiness(true);
    candidate.decision=RuntimeScopeTestAccess::Issue(*candidate.scope);Check(candidate.decision.owner!=0,"race candidate existing decision evaluator");fixture.Facts(candidate);
    std::promise<void> entered,release;auto paused=entered.get_future();auto resume=release.get_future().share();
    auto legacy=std::async(std::launch::async,[&]{
        Lifetime::LegacyInitializationBarrier([&]{entered.set_value();if(resume.wait_for(5s)!=std::future_status::ready)throw std::runtime_error("legacy initialization barrier timeout");});
        std::string failure;const bool enabled=fixture.manager->SetEnabled("fixture.owner",true,failure);
        Lifetime::LegacyInitializationBarrier({});return enabled;
    });
    Check(paused.wait_for(2s)==std::future_status::ready,"actual legacy initializer paused outside M");
    {auto lock=Lifetime::HoldManager(*fixture.manager);Check(lock.owns_lock(),"actual M available while legacy initializer paused");}
    const auto snapshot=fixture.registry.Current();const auto revision=fixture.domain->Revision();const auto order=fixture.manager->EnabledOrder();
    const auto statePath=fixture.root/"state.toon";const bool stateExisted=std::filesystem::exists(statePath);
    const auto stateBytes=stateExisted?ReadBounded(statePath,1024U*1024U):std::string{};
    ManagerPublication::Plan denied;const auto outcome=fixture.Prepare(nullptr,candidate,denied);
    Check(outcome==ScopeResult::Busy,"real in-flight legacy lifecycle rejects canonical metadata sealing");
    Check(fixture.registry.Current()==snapshot&&fixture.domain->Revision()==revision&&fixture.manager->EnabledOrder()==order,"paused legacy canonical denial has no publication or order effect");
    Check(std::filesystem::exists(statePath)==stateExisted&&(!stateExisted||ReadBounded(statePath,1024U*1024U)==stateBytes),"paused canonical denial has no persistence write");
    release.set_value();Check(legacy.wait_for(2s)==std::future_status::ready&&legacy.get(),"real legacy initializer resumes and activates without poisoned seal");
    PreparationProgress("CaptureAfterLegacy");const auto capture=Preparation::Capture(*fixture.manager,"fixture.owner",fixture.root/"mods/fixture.owner",fixture.pin);
    Reject([&]{PreparationProgress("CandidateAfterLegacy");auto adoption=Access::Candidate(*fixture.manager,capture,fixture.base);},"quiescent active legacy owner cannot be adopted");
    Check(fixture.manager->SetEnabled("fixture.owner",false,error),"real legacy owner remains unsealed and can disable");
    auto fresh=fixture.Make(2);ManagerPublication::Plan accepted;
    Check(fixture.Prepare(nullptr,fresh,accepted)==ScopeResult::Ok&&Commit(accepted)==ScopeResult::Ok,"fresh canonical owner succeeds after legacy quiescence");
}
/** @summary Preserve a separate actual owner's entire activation order and registry contribution through handoff. @param package Actual Base. */
void UnrelatedOwner(const std::filesystem::path& package){
    Fixture fixture(package,true);auto unrelated=fixture.Make(5,"fixture.other");ManagerPublication::Plan other;
    Check(fixture.Prepare(nullptr,unrelated,other)==ScopeResult::Ok&&Commit(other)==ScopeResult::Ok,"unrelated actual owner initial");
    auto old=fixture.Make(1);ManagerPublication::Plan initial;
    Check(fixture.Prepare(nullptr,old,initial)==ScopeResult::Ok&&Commit(initial)==ScopeResult::Ok,"second actual owner initial");
    fixture.Refresh(old);auto next=fixture.Make(9);ManagerPublication::Plan handoff;
    Check(fixture.Prepare(&old,next,handoff)==ScopeResult::Ok&&Commit(handoff)==ScopeResult::Ok,"unrelated order handoff");
    const auto view=Observe(handoff);Check(view.orderMatches&&view.projectionMatches&&view.orderSize==2&&view.ownerInOrder,"guarded entire unrelated order preserved");
    const auto patches=view.registry->Find(target);
    Check(patches&&patches->size()==4&&(*patches)[0].owner=="fixture.other"&&(*patches)[1].owner=="fixture.other"&&
        (*patches)[1].patch.properties[0].value==5&&(*patches)[3].owner=="fixture.owner"&&(*patches)[3].patch.properties[0].value==9,"unrelated registry contribution and ordering preserved");
    Check(fixture.manager->EnabledOrder()==std::vector<ModId>{"fixture.other","fixture.owner"},"diagnostic exact unrelated IDs confirm guarded full order oracle");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok,"unrelated order retired cleanup");
}
/** @summary Exercise actual scheduler capacity and native preflight allocation failure without any publication marker. @param package Actual Base. */
void CapacityAndAllocation(const std::filesystem::path& package){
    {
        Fixture fixture(package);auto candidate=fixture.Make(1);ManagerPublication::Plan denied;bool failed{};
        failAllocation=true;try{static_cast<void>(fixture.Prepare(nullptr,candidate,denied));}catch(const std::bad_alloc&){failed=true;}failAllocation=false;
        Check(failed&&fixture.domain->Revision()==0&&!fixture.registry.IsActive("fixture.owner")&&!candidate.vm->Status().running,"native allocation rejection has no effects");
        ManagerPublication::Plan retry;Check(fixture.Prepare(nullptr,candidate,retry)==ScopeResult::Ok&&Commit(retry)==ScopeResult::Ok,"same untouched candidate recovers after preflight allocation failure");
    }
    Fixture fixture(package);auto& scheduler=Access::Scheduler(*fixture.manager);
    for(unsigned index=0;index!=128;++index){Progress("CapacityAndAllocation",index);
        const auto owner="capacity."+std::to_string(index);ScriptDefinition definition;
        definition.entry="scripts/init.luau";definition.language="luau";definition.memoryBytes=2U*1024U*1024U;definition.budgetMs=20;
        definition.bytecode=CompileLuauSource("return {}");auto vm=std::make_shared<ScriptRuntime>(owner,definition,ScriptHost{});
        SchedulerPublicationPlan reserved;Check(Scheduler::Prepare(scheduler,owner,vm,SchedulerPublicationPurpose::Activate,reserved)==ScopeResult::Ok,"real scheduler reservation below original 128 ceiling");
        Check(Scheduler::Reclaim(reserved)==ScopeResult::Ok&&Scheduler::Closed(vm),"capacity reservation releases VM but retains exact empty node");
    }
    auto candidate=fixture.Make(1);ManagerPublication::Plan denied;
    Check(fixture.Prepare(nullptr,candidate,denied)!=ScopeResult::Ok,"full actual scheduler rejects canonical publication preparation");
    Check(fixture.domain->Revision()==0&&fixture.manager->EnabledOrder().empty()&&!fixture.registry.IsActive("fixture.owner"),"capacity rejection precedes any marker");
    Check(Scheduler::ReleaseEmpty(scheduler,"capacity.0")==ScopeResult::Ok,"foreign exact empty node capacity recovered");
    auto retry=fixture.Make(2);ManagerPublication::Plan recovered;
    Check(fixture.Prepare(nullptr,retry,recovered)==ScopeResult::Ok&&Commit(recovered)==ScopeResult::Ok,"fresh canonical candidate succeeds after actual capacity recovery");
}
/** @summary Reject mutation/rollback/textual and duplicate-record paths for sealed actual owners. @param package Actual Base. */
void LegacyBoundaries(const std::filesystem::path& package){
    Fixture fixture(package);auto candidate=fixture.Make(1);ManagerPublication::Plan plan;
    Check(fixture.Prepare(nullptr,candidate,plan)==ScopeResult::Ok&&Commit(plan)==ScopeResult::Ok,"legacy fence owner active");
    const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry.Current();
    const auto origin=Access::Origin(plan);auto alias=std::shared_ptr<PatchRegistry>(&fixture.registry,[](PatchRegistry*){});
    Check(!OwnerLifecycleTestAccess::Issue(origin,alias,fixture.domain),"duplicate fixture record denied actual origin");
    Check(!OwnerLifecycleTestAccess::IssueCandidate(origin,alias,fixture.domain),"duplicate candidate record denied actual origin");
    auto foreign=OwnerLifecycleTestAccess::IssueCandidate(OwnedPublicationTestAccess::Issue(candidate.vm,candidate.endpoint,"foreign"),alias,fixture.domain);
    Check(!foreign,"same VM arbitrary textual origin cannot manufacture joint owner");
    std::string error;
    Check(!fixture.manager->SetEnabled("fixture.owner",false,error),"legacy Disable denied sealed owner");
    Check(!fixture.manager->SetEnabled("fixture.owner",true,error),"legacy Enable denied sealed owner");
    Check(!fixture.manager->Reload("fixture.owner",error),"legacy Reload denied sealed owner");
    const std::vector<ScriptPatchCommand> commands{{target,0x09AE19D7,6.0F,{},{}}};
    auto sink=Lifetime::Sink(*fixture.manager);Check(!sink("fixture.owner",candidate.vm,commands,error),"legacy command sink denied sealed owner");
    SchedulerPublicationPlan removal;
    const auto prepared=Scheduler::Prepare(Access::Scheduler(*fixture.manager),"fixture.owner",nullptr,SchedulerPublicationPurpose::Remove,removal);
    Check(prepared!=ScopeResult::Ok||Scheduler::Publish(removal)!=ScopeResult::Ok,"standalone typed removal denied actual seal");
    fixture.Write(7);static_cast<void>(fixture.manager->Refresh(error));
    Check(fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot,"legacy refreshed source cannot mutate actual publication");
    std::filesystem::remove_all(fixture.root/"mods/fixture.owner");static_cast<void>(fixture.manager->Refresh(error));
    Check(fixture.domain->Revision()==revision&&fixture.registry.Current()==snapshot,"legacy missing directory cannot remove sealed owner");
    Coherent(plan,&candidate,1);
}
/** @summary Qualify abort/retry semantics for both empty sealed actual entry and current predecessor. @param package Actual Base. */
void AbortRetry(const std::filesystem::path& package){
    Fixture fixture(package);auto abandoned=fixture.Make(1);ManagerPublication::Plan abort;
    Check(fixture.Prepare(nullptr,abandoned,abort)==ScopeResult::Ok,"initial abort preparation");
    Check(Access::Reclaim(abort)==ScopeResult::Ok&&Scheduler::Closed(abandoned.vm)&&fixture.disables==0,"initial abort closes without disable");
    Check(fixture.domain->Revision()==0&&!fixture.registry.IsActive("fixture.owner"),"abort changes no marker");
    std::string error;Check(!fixture.manager->SetEnabled("fixture.owner",true,error),"aborted sealed empty owner rejects legacy restart");
    auto retry=fixture.Make(2);ManagerPublication::Plan active;
    Check(fixture.Prepare(nullptr,retry,active)==ScopeResult::Ok&&Commit(active)==ScopeResult::Ok,"fresh typed candidate succeeds after sealed abort");
    fixture.Refresh(retry);auto failed=fixture.Make(3);ManagerPublication::Plan handoff;
    Check(fixture.Prepare(&retry,failed,handoff)==ScopeResult::Ok,"handoff abort preparation");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok&&Scheduler::Closed(failed.vm)&&fixture.disables==0,"handoff abort preserves predecessor and no disable");Coherent(active,&retry,2);
    auto successor=fixture.Make(4);ManagerPublication::Plan replacement;
    Check(fixture.Prepare(&retry,successor,replacement)==ScopeResult::Ok&&Commit(replacement)==ScopeResult::Ok,"fresh handoff succeeds after aborted candidate");
    Coherent(replacement,&successor,4);Check(Access::Reclaim(replacement)==ScopeResult::Ok&&fixture.disables==1,"successful replacement retires predecessor once");
}
/** @summary Retain plans beyond actual manager lifetime and prove no borrowed access or State/VM ownership cycle. @param package Actual Base. */
void LifetimeClosure(const std::filesystem::path& package){
    Fixture fixture(package);auto lifetime=Lifetime::Capture(*fixture.manager,"fixture.owner");auto anchor=Lifetime::Anchor(lifetime);
    auto candidate=fixture.Make(1);std::weak_ptr<ScriptRuntime> vm=candidate.vm;ManagerPublication::Plan plan;
    Check(fixture.Prepare(nullptr,candidate,plan)==ScopeResult::Ok,"retained plan preparation");
    fixture.manager.reset();Check(Access::Commit(plan)==ScopeResult::Cancelled,"closed manager denies retained commit");
    Access::Observation observation;Check(Access::Observe(plan,observation)==ScopeResult::Cancelled,"closed manager denies retained borrowed observation");
    Check(Access::Reclaim(plan)==ScopeResult::Ok&&Scheduler::Closed(candidate.vm),"retained aborted VM reclaimed after manager free");
    candidate=Candidate{};plan=ManagerPublication::Plan{};lifetime=Lifetime::Plan{};
    Check(anchor.expired()&&vm.expired(),"no retained State or VM cycle after plans released");
}
/** @summary Race actual guarded observations with one same-ID handoff and reject every mixed view. @param package Actual Base. */
void Concurrent(const std::filesystem::path& package){
    Fixture fixture(package);auto old=fixture.Make(1);ManagerPublication::Plan initial;
    Check(fixture.Prepare(nullptr,old,initial)==ScopeResult::Ok&&Commit(initial)==ScopeResult::Ok,"concurrent initial");
    fixture.Refresh(old);auto next=fixture.Make(9);ManagerPublication::Plan handoff;
    Check(fixture.Prepare(&old,next,handoff)==ScopeResult::Ok,"concurrent handoff preparation");
    const auto oldOrigin=Access::Origin(initial),newOrigin=Access::Origin(handoff);
    std::barrier start(2);std::atomic_uint observed{};
    auto reader=std::async(std::launch::async,[&]{start.arrive_and_wait();for(unsigned i=0;i!=256;++i){
        Access::Observation view;const auto status=Access::Observe(handoff,view);if(status==ScopeResult::Busy){std::this_thread::yield();continue;}
        Check(status==ScopeResult::Ok,"concurrent actual guarded read");const bool before=view.origin==oldOrigin,after=view.origin==newOrigin;
        Check((before||after)&&view.runtime==(before?old.vm:next.vm)&&view.running&&view.scope.activationCommitted&&!view.scope.stopping,"actual guarded old or new VM/scope/status");
        Check(view.projectionMatches&&view.orderMatches&&view.ownerInOrder&&view.orderSize==1,"actual guarded old or new projection/order");
        Check(view.registry->domain_revision()==view.domainRevision&&view.domainRevision==(before?1U:2U),"actual guarded old or new marker");
        const auto patches=view.registry->Find(target);Check(patches&&patches->back().patch.properties.front().value==(before?1.0F:9.0F),"actual guarded old or new patch");++observed;
    }});
    start.arrive_and_wait();auto result=Commit(handoff);reader.get();if(result==ScopeResult::Busy)result=Commit(handoff);
    Check(result==ScopeResult::Ok&&fixture.domain->Revision()==2&&observed>0,"concurrent one marker with actual observations");
    Coherent(handoff,&next,9);Check(Access::Reclaim(handoff)==ScopeResult::Ok,"concurrent retired cleanup");
}
}
/** @summary Execute isolated actual manager publication tests without gameplay or OS VM. @param argc Argument count. @param argv Actual Base checkout. @return Zero only after all behavioral checks pass. */
int main(int argc,char** argv){
    try{
        Check(argc==2,"verified Base checkout argument required");const std::filesystem::path package=argv[1];
        Progress("Lifecycle");Lifecycle(package);Progress("Contention");Contention(package);Progress("Stale");Stale(package);Progress("CaptureIdentity");CaptureIdentity(package);Progress("ScopeSubstitution");ScopeSubstitution(package);Progress("LegacyInitializationRace");LegacyInitializationRace(package);Progress("UnrelatedOwner");UnrelatedOwner(package);
        Progress("CapacityAndAllocation");CapacityAndAllocation(package);Progress("LegacyBoundaries");LegacyBoundaries(package);Progress("AbortRetry");AbortRetry(package);Progress("LifetimeClosure");LifetimeClosure(package);Progress("Concurrent");Concurrent(package);
        Check(fenceAllocations==0&&fenceFrees==0&&fenceCallbacks==0,"publication fence allocated freed or called lifecycle code");
        std::cout<<checks<<" manager publication checks passed\n";return 0;
    }catch(const std::exception& error){failAllocation=false;std::cerr<<"manager publication failure group="<<scenarioLabel<<" subcase="<<scenarioIndex<<" make="<<makeOrdinal<<" operation="<<preparationOperation<<std::endl;std::cerr<<"manager publication: "<<error.what()<<'\n';return 1;}
}
