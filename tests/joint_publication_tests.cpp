#include "mods/owned_publication.hpp"
#include "scripting/base_host.hpp"
#include "scripting/luau_compiler.hpp"
#include "scripting/source_project.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
std::atomic_size_t fenceAllocations{},fenceFrees{},fenceCallbacks{};
}
/** @summary Counts actual C++ allocations inside the typed joint reservation. @param bytes Requested capacity. @return Allocated storage. */
void* operator new(std::size_t bytes) {
    if(sc13::scripting::SchedulerPublicationTestAccess::InsideFence()) ++fenceAllocations;
    if(auto pointer=std::malloc(bytes ? bytes : 1)) return pointer;throw std::bad_alloc();
}
/** @summary Applies identical accounting to arrays. @param bytes Capacity. @return Allocated storage. */
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
/** @summary Counts final C++ frees while actual publication participants are held. @param pointer Released allocation. */
void operator delete(void* pointer) noexcept {
    if(pointer && sc13::scripting::SchedulerPublicationTestAccess::InsideFence()) ++fenceFrees;std::free(pointer);
}
/** @summary Matches array allocation. @param pointer Allocation. */
void operator delete[](void* pointer) noexcept {::operator delete(pointer);}
/** @summary Matches sized scalar release. @param pointer Allocation. */
void operator delete(void* pointer,std::size_t) noexcept {::operator delete(pointer);}
/** @summary Matches sized array release. @param pointer Allocation. */
void operator delete[](void* pointer,std::size_t) noexcept {::operator delete(pointer);}

namespace {
using namespace sc13::scripting;
using namespace sc13::runtime;
using namespace sc13::mods;
using Access=OwnerLifecycleTestAccess;
using SchedulerAccess=SchedulerPublicationTestAccess;
using namespace std::chrono_literals;
std::atomic_uint checks{};
/** @summary Requires a behavioral result. @param value Actual fact. @param message Fixed failure diagnostic. */
void Check(bool value,const char* message) {++checks;if(!value) throw std::runtime_error(message);}
/** @summary Pins real local Base bytes before any VM executes. @param path Verified checkout. @return Exact local pin. */
BasePackagePin Pin(const std::filesystem::path& path) {
    BasePackagePin result{"0.1.0-alpha",{}};
    result.files.emplace("mod.toml",HashText(ReadBounded(path/"mod.toml",4096)));
    for(const auto& file:std::filesystem::recursive_directory_iterator(path/"scripts")) if(file.is_regular_file())
        result.files.emplace(file.path().lexically_relative(path).generic_string(),HashText(ReadBounded(file.path(),1024U*1024U)));
    return result;
}
/** @summary Retains one actual candidate and its existing fixture authority/factual reservations. */
struct Candidate final {
    std::shared_ptr<RuntimeScope> scope;
    std::shared_ptr<SemanticEndpoint> endpoint;
    std::shared_ptr<ScriptRuntime> vm;
    std::shared_ptr<PublicationOrigin> origin;
    OperationContext decision;
    SemanticContextLease lease;
};
/** @summary Owns isolated native objects only; no game, OS VM or production ingress exists. */
struct Fixture final {
    std::shared_ptr<RuntimeCommitDomain> domain=std::make_shared<RuntimeCommitDomain>();
    std::shared_ptr<PatchRegistry> registry=std::make_shared<PatchRegistry>();
    std::shared_ptr<SemanticBridge> bridge=std::make_shared<SemanticBridge>();
    std::unique_ptr<BaseHost> base;
    ScriptScheduler scheduler;
    ResourcePatchDefinition patch;
    std::atomic_uint disables{},closes{},legacySinks{};
    /** @summary Starts the verified Base graph and fixture-only enrolled scheduler. @param package Actual checkout. */
    explicit Fixture(const std::filesystem::path& package) : scheduler({},[this](const std::string&,const std::shared_ptr<ScriptRuntime>&,
        const std::vector<ScriptPatchCommand>&,std::string&){++legacySinks;return true;}) {
        SemanticMethod method{"fixture.echo","test.echo@1","script","city","fixture-only","echo-confirmed",1024,
            [](const copied::Envelope& input,std::span<char> output,std::size_t& size) {
                const auto wire=input.Encode();if(wire.size()>output.size()) return false;
                std::memcpy(output.data(),wire.data(),wire.size());size=wire.size();return true;
            }};
        method.registryPublication=true;SemanticBridgeTestAccess::Configure(*bridge,{std::move(method)});
        SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::Runtime,1,1);
        SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,7,3);
        base=std::make_unique<BaseHost>(package,Pin(package),std::function<void(ScriptPhase,std::uint64_t)>{},bridge);
        Check(base->Ready(),"Base critical graph not qualified");
        Check(SchedulerAccess::Enroll(scheduler,domain)==ScopeResult::Ok,"Scheduler enrollment");
        patch.target={1,2,3};patch.properties.push_back({4,PropertyType::Float,PatchOperation::Set,1.0F});
        std::set<sc13::core::Tgi> affected;std::string error;
        Check(registry->Activate("foreign",{&patch,1},affected,error),"Foreign owner setup");
    }
    /** @summary Joins native callbacks while captured counters and Base still have valid lifetimes. */
    ~Fixture() {scheduler.Shutdown();}
    /** @summary Creates a real uncommitted VM with no operational scope activation.
     * @param number Distinguishing actual patch value. @param owner Exact native owner key. @return Retained candidate. */
    Candidate Make(float number,const std::string& owner="fixture.joint") {
        Candidate result;result.scope=std::shared_ptr<RuntimeScope>(RuntimeScopeTestAccess::Create(domain));
        result.scope->SetReadiness(true);auto host=base->Bind(result.scope);result.endpoint=host.semantic;
        host.lifecycle=[this](ScriptPhase phase,std::uint64_t) {
            if(SchedulerAccess::InsideFence())++fenceCallbacks;
            if(phase==ScriptPhase::Disable)++disables;if(phase==ScriptPhase::Close)++closes;
        };
        result.decision=RuntimeScopeTestAccess::Issue(*result.scope);
        SemanticBridgeTestAccess::Issue(*result.endpoint,result.decision);
        ScriptDefinition definition;definition.entry="scripts/init.luau";definition.language="luau";
        definition.memoryBytes=2U*1024U*1024U;definition.budgetMs=20;
        definition.bytecode=CompileLuauSource("return {onTick=function() end,onDisable=function() end}");
        result.vm=std::make_shared<ScriptRuntime>(owner,definition,std::move(host));
        auto changed=patch;changed.properties.front().value=number;
        result.origin=OwnedPublicationTestAccess::Issue(result.vm,result.endpoint,"fixture-joint",{changed});
        Facts(result);return result;
    }
    /** @summary Captures actual current city/Base facts from the same existing endpoint.
     * @param candidate Exact retained origin. */
    void Facts(Candidate& candidate) {
        copied::Envelope value;
        Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]})",
            "test.echo@1",candidate.endpoint->storageOwner,value)==copied::Status::Ok,"Lease input decode");
        Check(bridge->BindReferences(*candidate.endpoint,value,candidate.lease)==ScopeResult::Ok,"Actual factual lease");
    }
    /** @summary Refreshes only an operation reservation under the existing grant. @param candidate Current owner. */
    void Refresh(Candidate& candidate) {
        candidate.decision=RuntimeScopeTestAccess::Reserve(*candidate.scope,candidate.decision);
        Check(candidate.decision.owner!=0,"Existing grant reservation refresh");
        Facts(candidate);
    }
    /** @summary Prepares one real atomic native operation. @param record Exact slot. @param old Old origin or null.
     * @param next Candidate or null. @param plan Empty output. @return Typed result. */
    ScopeResult Prepare(const Access::Record& record,Candidate* old,Candidate* next,OwnerLifecycle::Plan& plan) {
        std::string error;return Access::Joint(scheduler,record,old ? old->origin : nullptr,next ? next->origin : nullptr,
            old ? old->decision : OperationContext{},next ? next->decision : OperationContext{},
            old ? old->lease : SemanticContextLease{},next ? next->lease : SemanticContextLease{},plan,error);
    }
};
/** @summary Retries only native Busy contention in bounded positive fixture checks, never a rejected capability.
 * @param record Exact slot. @param plan Original retained plan. @return Final native outcome. */
ScopeResult Commit(const Access::Record& record,OwnerLifecycle::Plan& plan) {
    auto result=ScopeResult::Busy;
    for(unsigned retry=0;retry!=4096 && result==ScopeResult::Busy;++retry) {
        result=Access::CommitJoint(record,plan);if(result==ScopeResult::Busy)std::this_thread::yield();
    }
    return result;
}
/** @summary Reads actual jointly guarded participants and verifies complete incarnation correspondence.
 * @param fixture Actual objects. @param record Original slot. @param expected Expected active candidate or null.
 * @param value Actual patch payload. */
void Coherent(Fixture& fixture,const Access::Record& record,Candidate* expected,float value=0) {
    Access::JointObservation observation;
    ScopeResult status=ScopeResult::Busy;
    for(unsigned retry=0;retry!=4096 && status==ScopeResult::Busy;++retry) {
        status=Access::Observe(record,fixture.scheduler,observation);if(status==ScopeResult::Busy)std::this_thread::yield();
    }
    Check(status==ScopeResult::Ok,"Coherent native observation");
    Check(observation.registry->domain_revision()==observation.domainRevision,"Registry/domain marker mismatch");
    const auto patches=observation.registry->Find(fixture.patch.target);Check(patches && !patches->empty(),"Foreign patches lost");
    Check(patches->front().owner=="foreign","Unrelated activation ordering changed");
    if(expected) {
        Check(observation.origin==expected->origin && observation.runtime==expected->vm && observation.running,"Record/scheduler/VM split");
        Check(observation.scope.activationCommitted && !observation.scope.stopping,"Scope activation split");
        Check(patches->size()==2 && patches->back().owner=="fixture.joint" &&
            patches->back().patch.properties.front().value==value,"Actual registry incarnation mismatch");
    } else Check(!observation.origin && !observation.runtime && patches->size()==1,"Removal left a published participant");
}
/** @summary Qualifies complete initial activation, handoff, removal and no-change cleanup. @param package Base checkout. */
void Lifecycle(const std::filesystem::path& package) {
    Fixture fixture(package);auto old=fixture.Make(1);const auto baseEpoch=fixture.bridge->Epoch();
    auto record=Access::IssueCandidate(old.origin,fixture.registry,fixture.domain);Check(bool(record),"Empty record issuance");
    OwnerLifecycle::Plan initial;Check(fixture.Prepare(record,nullptr,&old,initial)==ScopeResult::Ok,"Initial joint preparation");
    Check(!old.vm->Status().running && !old.scope->Current().activationCommitted && !fixture.registry->IsActive("fixture.joint"),"Initial candidate visible early");
    Check(Access::Commit(record,initial)==ScopeResult::Conflict,"Standalone lifecycle bypass");
    Check(!OwnedPublicationTestAccess::Transition(*old.origin,PublicationOrigin::Phase::Active),"Legacy phase bypass");
    Check(Commit(record,initial)==ScopeResult::Ok && fixture.domain->Revision()==1,"Initial one marker");Coherent(fixture,record,&old,1);
    Check(Access::CommitJoint(record,initial)==ScopeResult::Conflict,"Initial replay");
    Check(!Access::Issue(old.origin,fixture.registry,fixture.domain),"Second legacy record bypass");
    SchedulerPublicationPlan standalone;
    Check(SchedulerAccess::Prepare(fixture.scheduler,"fixture.joint",nullptr,SchedulerPublicationPurpose::Remove,standalone)==ScopeResult::Ok,"Standalone attempt preparation");
    Check(SchedulerAccess::Publish(standalone)!=ScopeResult::Ok && fixture.domain->Revision()==1,"Standalone scheduler bypass");
    fixture.Refresh(old);auto next=fixture.Make(9);OwnerLifecycle::Plan handoff;
    Check(fixture.Prepare(record,&old,&next,handoff)==ScopeResult::Ok,"Joint handoff preparation");
    Check(Commit(record,handoff)==ScopeResult::Ok && fixture.domain->Revision()==2,"Handoff one marker");Coherent(fixture,record,&next,9);
    Check(old.scope->Current().stopping && !old.vm->Status().running,"Old admission remained live");
    Check(!SchedulerAccess::Closed(old.vm) && fixture.disables==0,"Premature retirement destruction");
    Check(Access::Reclaim(handoff)==ScopeResult::Ok && SchedulerAccess::Closed(old.vm) && fixture.disables==1,"Deferred old close");
    next.vm->Fail("synthetic failed owner");next.scope->RequestStop();fixture.bridge->Close();
    OwnerLifecycle::Plan removal;Check(fixture.Prepare(record,&next,nullptr,removal)==ScopeResult::Ok,"Release-only removal after failure/Base close");
    Check(Commit(record,removal)==ScopeResult::Ok && fixture.domain->Revision()==3,"Removal marker");Coherent(fixture,record,nullptr);
    Check(Access::Reclaim(removal)==ScopeResult::Ok && SchedulerAccess::Closed(next.vm) && fixture.disables==2,"Failed committed owner disable accounting");
    const auto snapshot=fixture.registry->Current();OwnerLifecycle::Plan repeated;
    Check(fixture.Prepare(record,&next,nullptr,repeated)==ScopeResult::Ok,"Exact tombstone preparation");
    Check(Commit(record,repeated)==ScopeResult::Ok && fixture.domain->Revision()==3 && fixture.registry->Current()==snapshot,"Repeated removal advanced publication");
    Check(SchedulerAccess::ReleaseEmpty(fixture.scheduler,"fixture.joint")==ScopeResult::Denied,"Standalone empty-node release bypassed joint seal");
    Check(fixture.bridge->Epoch()==baseEpoch && fixture.legacySinks==0,"Base epoch or legacy sink changed");
}
/** @summary Requires zero participant changes when any actual publication lock is contended. @param package Base checkout. */
void Contention(const std::filesystem::path& package) {
    for(int participant=0;participant!=7;++participant) {
        Fixture fixture(package);auto candidate=fixture.Make(1);auto record=Access::IssueCandidate(candidate.origin,fixture.registry,fixture.domain);
        OwnerLifecycle::Plan plan;Check(fixture.Prepare(record,nullptr,&candidate,plan)==ScopeResult::Ok,"Contention prepare");
        const auto snapshot=fixture.registry->Current();ScopeResult result=ScopeResult::Invalid;
        const auto attempt=[&] {auto future=std::async(std::launch::async,[&]{return Access::CommitJoint(record,plan);});
            Check(future.wait_for(2s)==std::future_status::ready,"Participant reservation blocked");return future.get();};
        if(participant==0) {auto writer=fixture.domain->ReserveWriter();result=attempt();}
        if(participant==1) SchedulerAccess::HoldScheduler(fixture.scheduler,[&]{result=attempt();});
        if(participant==2) SchedulerAccess::Hold(candidate.vm,false,[&]{result=attempt();});
        if(participant==3) {auto registry=fixture.registry->TryReservePublication();result=attempt();}
        if(participant==4) {auto observation=SemanticBridgeTestAccess::ReserveObservation(*fixture.bridge);result=attempt();}
        if(participant==5) {auto scope=RuntimeScopeTestAccess::ReserveState(*candidate.scope);result=attempt();}
        if(participant==6) SchedulerAccess::Hold(candidate.vm,true,[&]{result=attempt();});
        Check(result==ScopeResult::Busy && fixture.domain->Revision()==0 && fixture.registry->Current()==snapshot,"Contention partially published");
        Check(!candidate.vm->Status().running && !candidate.scope->Current().activationCommitted,"Contention opened admission");
        Check(Commit(record,plan)==ScopeResult::Ok && fixture.domain->Revision()==1,"Contention retry failed");Coherent(fixture,record,&candidate,1);
    }
}
/** @summary Denies reclamation or origin poisoning when a candidate belongs to another existing preparation.
 * @param package Actual Base checkout. */
void ForeignPreparation(const std::filesystem::path& package) {
    for(bool foreignScheduler:{false,true}) {
        Fixture fixture(package);ScriptScheduler other;
        Check(SchedulerAccess::Enroll(other,fixture.domain)==ScopeResult::Ok,"Second isolated scheduler enrollment");
        auto candidate=fixture.Make(1);SchedulerPublicationPlan original;
        auto& owner=foreignScheduler ? other : fixture.scheduler;
        Check(SchedulerAccess::Prepare(owner,"fixture.joint",candidate.vm,SchedulerPublicationPurpose::Activate,original)==ScopeResult::Ok,"Original independent candidate preparation");
        auto record=Access::IssueCandidate(candidate.origin,fixture.registry,fixture.domain);Check(bool(record),"Foreign-preparation candidate record");
        OwnerLifecycle::Plan denied;
        Check(fixture.Prepare(record,nullptr,&candidate,denied)!=ScopeResult::Ok,"Already prepared candidate adopted");
        Check(Access::Reclaim(denied)==ScopeResult::Ok && !SchedulerAccess::Closed(candidate.vm),"Rejected preparation acquired foreign close authority");
        Check(SchedulerAccess::Publish(original)==ScopeResult::Ok && candidate.vm->Status().running,"Original candidate owner lost publication");
        SchedulerPublicationPlan removal;Check(SchedulerAccess::Prepare(owner,"fixture.joint",nullptr,SchedulerPublicationPurpose::Remove,removal)==ScopeResult::Ok,"Independent owner removalprepare");
        Check(SchedulerAccess::Publish(removal)==ScopeResult::Ok && SchedulerAccess::Reclaim(removal)==ScopeResult::Ok,"Independent owner cleanup");
    }
}
/** @summary Exercises a real competing candidate claim between prequalification and scheduler preparation.
 * @param package Actual Base checkout. */
void CompetingPreparation(const std::filesystem::path& package) {
    Fixture fixture(package);auto candidate=fixture.Make(1);auto record=Access::IssueCandidate(candidate.origin,fixture.registry,fixture.domain);
    SchedulerPublicationPlan independent;std::atomic_uint visits{};
    Access::PreparationBarrier([&] {
        Check(!SchedulerAccess::InsideFence(),"Preparation barrier entered a publication reservation");++visits;
        Check(SchedulerAccess::Prepare(fixture.scheduler,"fixture.joint",candidate.vm,SchedulerPublicationPurpose::Activate,independent)==ScopeResult::Ok,
            "Competing exact candidate claim failed");
    });
    OwnerLifecycle::Plan rejected;const auto outcome=fixture.Prepare(record,nullptr,&candidate,rejected);
    Access::PreparationBarrier({});
    Check(visits==1 && outcome==ScopeResult::Denied,"Competing claim was not rejected");
    Check(Access::Reclaim(rejected)==ScopeResult::Ok && !SchedulerAccess::Closed(candidate.vm),"Competing owner lost closure authority");
    Check(SchedulerAccess::Publish(independent)==ScopeResult::Ok && fixture.domain->Revision()==1,"Competing standalone plan was poisoned by failed metadata bind");
    Check(OwnedPublicationTestAccess::Transition(*candidate.origin,PublicationOrigin::Phase::Retired),"Failed joint preparation sealed a foreign incarnation");
    SchedulerPublicationPlan removal;
    Check(SchedulerAccess::Prepare(fixture.scheduler,"fixture.joint",nullptr,SchedulerPublicationPurpose::Remove,removal)==ScopeResult::Ok &&
        SchedulerAccess::Publish(removal)==ScopeResult::Ok && SchedulerAccess::Reclaim(removal)==ScopeResult::Ok,"Competing owner cleanup");
}
/** @summary Invalidates standalone plans captured before the exact record and node were jointly bound.
 * @param package Actual Base checkout. */
void EarlierPlans(const std::filesystem::path& package) {
    Fixture fixture(package);auto old=fixture.Make(1);
    Check(old.scope->CommitActivation(),"Independent scope setup");SchedulerPublicationPlan activation;
    Check(SchedulerAccess::Prepare(fixture.scheduler,"fixture.joint",old.vm,SchedulerPublicationPurpose::Activate,activation)==ScopeResult::Ok &&
        SchedulerAccess::Publish(activation)==ScopeResult::Ok,"Independent typed scheduler setup");
    Check(OwnedPublicationTestAccess::Transition(*old.origin,PublicationOrigin::Phase::Active),"Independent lifecycle setup");
    fixture.Refresh(old);old.endpoint->context=old.decision;
    copied::Envelope value;Check(fixture.bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]})",
        "test.echo@1",old.endpoint->storageOwner,value)==copied::Status::Ok,"Independent registry input");
    TaskHandle task;SemanticContextLease lease;Check(fixture.bridge->Request(old.endpoint,"fixture.echo",value,task,lease)==ScopeResult::Ok,"Independent registry request");
    SemanticInvocation invocation;Check(fixture.bridge->Take(invocation),"Independent registry dequeue");
    OwnedPublication publication;std::string error;
    Check(OwnedPublicationTestAccess::Prepare(old.origin,*fixture.registry,invocation,{},publication,error) &&
        fixture.bridge->DispatchPrepared(invocation,*fixture.registry,publication)==ScopeResult::Ok,"Independent registry seal");
    Check(old.endpoint->tasks->Release(task)==ScopeResult::Ok,"Independent task cleanup");fixture.Refresh(old);
    auto record=Access::Issue(old.origin,fixture.registry,fixture.domain);
    auto duplicate=Access::Issue(old.origin,fixture.registry,fixture.domain);Check(record && duplicate,"Preexisting exact model records");
    OwnerLifecycle::Plan previous,duplicatePlan;
    Check(Access::Remove(record,old.origin,previous,error)==ScopeResult::Ok &&
        Access::Remove(duplicate,old.origin,duplicatePlan,error)==ScopeResult::Ok,"Preexisting lifecycle plans");
    SchedulerPublicationPlan schedule;
    Check(SchedulerAccess::Prepare(fixture.scheduler,"fixture.joint",nullptr,SchedulerPublicationPurpose::Remove,schedule)==ScopeResult::Ok,"Preexisting scheduler plan");
    auto oversized=fixture.Make(7);
    oversized.origin=OwnedPublicationTestAccess::Issue(oversized.vm,oversized.endpoint,"oversized-native-candidate",
        std::vector<ResourcePatchDefinition>(4097,fixture.patch));
    OwnerLifecycle::Plan denied;
    Check(fixture.Prepare(record,&old,&oversized,denied)==ScopeResult::Busy,"Oversized prepared input was not rejected");
    Check(OwnedPublicationTestAccess::Transition(*old.origin,PublicationOrigin::Phase::Active) &&
        OwnedPublicationTestAccess::Transition(*oversized.origin,PublicationOrigin::Phase::Retired),"Failed preparation partially sealed old or candidate metadata");
    Check(Access::Reclaim(denied)==ScopeResult::Ok && !SchedulerAccess::Closed(oversized.vm),"Rejected unowned candidate acquired close authority");
    auto next=fixture.Make(9);OwnerLifecycle::Plan joint;Check(fixture.Prepare(record,&old,&next,joint)==ScopeResult::Ok,"Joint binding of exact existing typed owner");
    const auto revision=fixture.domain->Revision();const auto snapshot=fixture.registry->Current();
    Check(Access::Commit(record,previous)==ScopeResult::Conflict && Access::Commit(duplicate,duplicatePlan)==ScopeResult::Conflict &&
        SchedulerAccess::Publish(schedule)!=ScopeResult::Ok,"Prebinding standalone capability survived joint seal");
    Check(fixture.domain->Revision()==revision && fixture.registry->Current()==snapshot,"Rejected old plan changed participants");
    Check(Commit(record,joint)==ScopeResult::Ok && fixture.domain->Revision()==revision+1,"Actual joint successor failed");Coherent(fixture,record,&next,9);
    Check(Access::Reclaim(joint)==ScopeResult::Ok,"Preexisting owner deferred cleanup");
}
/** @summary Requires actual try-only Q/T observations under the same coherent domain read.
 * @param package Actual Base checkout. */
void ReaderContention(const std::filesystem::path& package) {
    Fixture fixture(package);auto candidate=fixture.Make(1);auto record=Access::IssueCandidate(candidate.origin,fixture.registry,fixture.domain);
    OwnerLifecycle::Plan plan;Check(fixture.Prepare(record,nullptr,&candidate,plan)==ScopeResult::Ok && Commit(record,plan)==ScopeResult::Ok,"Reader initial publication");
    const auto attempt=[&] {
        auto future=std::async(std::launch::async,[&]{Access::JointObservation view;return Access::Observe(record,fixture.scheduler,view);});
        Check(future.wait_for(2s)==std::future_status::ready,"Coherent reader blocked on an occupied participant");return future.get();
    };
    {auto scope=RuntimeScopeTestAccess::ReserveState(*candidate.scope);Check(attempt()==ScopeResult::Busy,"Reader ignored actual Q contention");}
    SchedulerAccess::Hold(candidate.vm,true,[&]{Check(attempt()==ScopeResult::Busy,"Reader ignored actual T contention");});
    Coherent(fixture,record,&candidate,1);
}
/** @summary Verifies stale facts, revoked permissions, wrong origins and abandoned preparation fail closed. @param package Base checkout. */
void Rejections(const std::filesystem::path& package) {
    for(int scenario=0;scenario!=6;++scenario) {
        Fixture fixture(package);auto candidate=fixture.Make(1);auto record=Access::IssueCandidate(candidate.origin,fixture.registry,fixture.domain);
        OwnerLifecycle::Plan plan;Check(fixture.Prepare(record,nullptr,&candidate,plan)==ScopeResult::Ok,"Rejection preparation");
        if(scenario==0) SemanticBridgeTestAccess::ObserveContext(*fixture.bridge,SemanticContextKind::City,7,3);
        if(scenario==1) candidate.scope->RequestStop();
        if(scenario==2) fixture.bridge->Close();
        if(scenario==3) candidate.vm->Stop();
        if(scenario==4) fixture.scheduler.Shutdown();
        if(scenario==5) {auto unrelated=fixture.Make(7,"different-owner");OwnerLifecycle::Plan wrong;
            Check(fixture.Prepare(record,nullptr,&unrelated,wrong)==ScopeResult::Conflict,"Foreign owner rebound record");
            Check(OwnedPublicationTestAccess::Transition(*unrelated.origin,PublicationOrigin::Phase::Retired),"Foreign origin poisoned by rejection");}
        const auto snapshot=fixture.registry->Current();
        if(scenario!=5) Check(Access::CommitJoint(record,plan)!=ScopeResult::Ok && fixture.domain->Revision()==0 &&
            fixture.registry->Current()==snapshot && !candidate.scope->Current().activationCommitted,"Rejected candidate changed participants");
        Check(Access::Reclaim(plan)==ScopeResult::Ok && SchedulerAccess::Closed(candidate.vm),"Aborted candidate not fully reclaimed");
        Check(fixture.disables==0,"Never committed candidate disabled");
    }
}
/** @summary Concurrently reads actual D-guarded participants while one handoff races them. @param package Base checkout. */
void Concurrent(const std::filesystem::path& package) {
    Fixture fixture(package);auto old=fixture.Make(1);auto record=Access::IssueCandidate(old.origin,fixture.registry,fixture.domain);
    OwnerLifecycle::Plan initial;Check(fixture.Prepare(record,nullptr,&old,initial)==ScopeResult::Ok,"Concurrent initialprepare");
    Check(Commit(record,initial)==ScopeResult::Ok,"Concurrent initialcommit");fixture.Refresh(old);
    auto next=fixture.Make(9);OwnerLifecycle::Plan plan;Check(fixture.Prepare(record,&old,&next,plan)==ScopeResult::Ok,"Concurrent handoffprepare");
    std::barrier start(2);std::atomic_uint observations{};
    auto reader=std::async(std::launch::async,[&] {
        start.arrive_and_wait();for(unsigned index=0;index!=128;++index) {
            Access::JointObservation observed;const auto status=Access::Observe(record,fixture.scheduler,observed);
            if(status==ScopeResult::Busy) {std::this_thread::yield();continue;}
            Check(status==ScopeResult::Ok,"Concurrent observation failed");
            const bool predecessor=observed.origin==old.origin;const bool successor=observed.origin==next.origin;
            Check((predecessor || successor) && observed.runtime==(predecessor ? old.vm : next.vm) && observed.running &&
                observed.scope.activationCommitted && !observed.scope.stopping,"Concurrent observation torn");
            Check(observed.registry->domain_revision()==observed.domainRevision && observed.domainRevision==(predecessor ? 1U : 2U),"Concurrent marker split");
            const auto patches=observed.registry->Find(fixture.patch.target);
            Check(patches && patches->size()==2 && patches->back().patch.properties.front().value==(predecessor ? 1.0F : 9.0F),"Concurrent registry split");++observations;
        }
    });
    start.arrive_and_wait();ScopeResult outcome=ScopeResult::Busy;
    for(unsigned retry=0;retry!=1024 && outcome==ScopeResult::Busy;++retry) {outcome=Access::CommitJoint(record,plan);if(outcome==ScopeResult::Busy)std::this_thread::yield();}
    reader.get();for(unsigned retry=0;retry!=4096 && outcome==ScopeResult::Busy;++retry) {
        outcome=Access::CommitJoint(record,plan);if(outcome==ScopeResult::Busy)std::this_thread::yield();
    }
    Check(outcome==ScopeResult::Ok && fixture.domain->Revision()==2 && observations>0,"Concurrent one-marker publication missing");Coherent(fixture,record,&next,9);
    Check(Access::Reclaim(plan)==ScopeResult::Ok,"Concurrent deferredclose");
}
}
/** @summary Runs isolated joint publication qualification. @param argc Argument count. @param argv Verified Base checkout.
 * @return Zero only after every behavioral and fence oracle passes. */
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("Expected separately checked-out Base path");
        const std::filesystem::path package=argv[1];Lifecycle(package);Contention(package);Rejections(package);
        ForeignPreparation(package);CompetingPreparation(package);EarlierPlans(package);ReaderContention(package);Concurrent(package);
        Check(fenceAllocations==0 && fenceFrees==0 && fenceCallbacks==0,"Joint reservation allocated, freed or entered a lifecycle callback");
        std::cout<<checks<<" joint publication checks passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
