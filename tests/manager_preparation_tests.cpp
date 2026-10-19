#include "mods/manager_preparation.hpp"
#include "mods/owned_publication.hpp"
#include "scripting/source_project.hpp"
#include "scripting/scheduler_publication.hpp"
#include "scripting/script_runtime.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "mod_fixture.hpp"
#include "test_harness.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool failAllocation{};
unsigned checks{};
using Access=sc13::mods::ManagerPreparationTestAccess;
using Lifetime=sc13::mods::ManagerLifetimeTestAccess;
using Plan=sc13::mods::ManagerPreparation::Plan;
using sc13::runtime::ScopeResult;
using namespace std::chrono_literals;
/** @summary Require actual bounded behavior. @param value Actual observation. @param message Fixed failure reason. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Require actual native rejection. @param action Attempted preparation or bypass. @param message Failure reason. */
template<class F> void Reject(F action,const char* message){bool denied{};try{action();}catch(const std::exception&){denied=true;}Check(denied,message);}
/** @summary Report a fixed test stage without exporting source or changing runtime budgets. @param label Native scenario identity. */
void Progress(const char* label){std::cerr<<"manager preparation begin "<<label<<std::endl;}
/** @summary Report only fixed copied first-bootstrap evidence after the actual constructor's lane join.
 * @param value Immutable terminal record or explicit not-reached/rejected state. @param requested Whether actual Enable requested a capture.
 * @details CPU values are raw 100 ns units without precision guarantees; cycles are raw counts, never elapsed time. Reporting cannot replace the original test result. */
void ReportColdBudget(const sc13::scripting::CallbackBudgetTestAccess::Observation& value,bool requested) noexcept{
    try{
        std::cerr<<"base cold diagnostic v=1 requested="<<requested<<" arm="<<static_cast<unsigned>(value.arm)
            <<" completion="<<static_cast<unsigned>(value.completion)<<" phase="<<static_cast<unsigned>(value.phase)
            <<" generation="<<value.generation<<" terminal="<<value.terminal<<" borrowCleared="<<value.borrowCleared
            <<" callbackStarted="<<value.callbackStarted<<" budgetMs="<<value.budgetMs<<" luaStatus="<<value.luaStatus
            <<" exceeded="<<value.exceeded<<" detectedElapsedValid="<<value.detectedElapsedValid<<" detectedElapsedUs="<<value.detectedElapsedUs
            <<" preSamplingNs="<<value.preSamplingNs<<" callbackStartNs="<<value.callbackStartNs<<" deadlineNs="<<value.deadlineNs
            <<" callbackEndNs="<<value.callbackEndNs<<" postSamplingNs="<<value.postSamplingNs<<" detectedDeadlineNs="<<value.detectedDeadlineNs
            <<" beforeThread="<<value.before.thread<<" afterThread="<<value.after.thread<<" sameThread="<<value.sameThread
            <<" platformBefore="<<value.before.platformAvailable<<" platformAfter="<<value.after.platformAvailable
            <<" cpuBeforeAvailable="<<value.before.cpuAvailable<<" cpuAfterAvailable="<<value.after.cpuAvailable
            <<" cpuBeforeError="<<value.before.cpuError<<" cpuAfterError="<<value.after.cpuError
            <<" kernelBefore100ns="<<value.before.kernel100ns<<" kernelAfter100ns="<<value.after.kernel100ns
            <<" userBefore100ns="<<value.before.user100ns<<" userAfter100ns="<<value.after.user100ns
            <<" cyclesBeforeAvailable="<<value.before.cyclesAvailable<<" cyclesAfterAvailable="<<value.after.cyclesAvailable
            <<" cyclesBeforeError="<<value.before.cycleError<<" cyclesAfterError="<<value.after.cycleError
            <<" rawCyclesBefore="<<value.before.cycles<<" rawCyclesAfter="<<value.after.cycles
            <<" wallOrderValid="<<value.wallOrderValid<<" cpuOrderValid="<<value.cpuOrderValid<<" cycleOrderValid="<<value.cycleOrderValid
            <<" luaAllocatorCalls="<<value.allocationCalls<<" luaFreeCalls="<<value.freeCalls<<" luaReallocationCalls="<<value.reallocationCalls
            <<" luaAllocationFailures="<<value.allocationFailures<<" luaRequestedBytes="<<value.requestedBytes
            <<" luaUsedBefore="<<value.usedBefore<<" luaUsedAfter="<<value.usedAfter<<" luaPeakUsed="<<value.peakUsed
            <<" gcInterruptEvents="<<value.gcInterruptEvents<<" instructionInterrupts="<<value.instructionInterrupts
            <<" maximumDepth="<<value.maximumDepth<<" nested="<<value.nested<<" overflow="<<value.overflow<<std::endl;
    }catch(...){/* @summary Missing diagnostic output does not change the original callback or test outcome. */}
}
/** @summary Retry only explicit precommit Busy for a positive immutable-capture fixture.
 * @param manager Original manager. @param capture One already successful immutable capture.
 * @param base Original pinned Base. @param observer Native generation evidence, possibly called for multiple distinct attempts.
 * @return One uncommitted successful preparation; all other failures propagate without retry. */
Plan Positive(sc13::mods::ModManager& manager,const std::shared_ptr<const sc13::mods::CapturedCandidate>& capture,
    const std::shared_ptr<sc13::scripting::BaseHost>& base,std::function<void(sc13::scripting::ScriptPhase,std::uint64_t)> observer={}){
    const auto deadline=std::chrono::steady_clock::now()+5s;
    for(unsigned attempt=0;attempt!=4096;++attempt){
        if(std::chrono::steady_clock::now()>=deadline)break;
        try{return Access::Prepare(manager,capture,base,observer);}
        catch(const std::runtime_error& error){if(std::string_view(error.what())!="preparation.busy")throw;std::this_thread::yield();}
    }
    throw std::runtime_error("positive preparation Busy admission exhausted");
}
/** @summary Read reviewed local Base inputs as a complete test pin. @param root Actual checkout. @return Full exact pin. */
sc13::scripting::BasePackagePin Pin(const std::filesystem::path& root){
    using namespace sc13::scripting;
    BasePackagePin pin{"0.1.0-alpha",{}};
    pin.files.emplace("mod.toml",HashText(ReadBounded(root/"mod.toml",4096)));
    for(const auto& item:std::filesystem::recursive_directory_iterator(root/"scripts"))if(item.is_regular_file())
        pin.files.emplace(item.path().lexically_relative(root).generic_string(),HashText(ReadBounded(item.path(),1024U*1024U)));
    return pin;
}
/** @summary Own an actual manager and its ordinary isolated filesystem/resources. */
struct Fixture final {
    std::filesystem::path root=sc13::tests::CreateTemporaryDirectory();
    sc13::mods::PatchRegistry registry;
    sc13::runtime::RuntimeResourceCache cache;
    std::shared_ptr<sc13::runtime::RuntimeCommitDomain> domain=std::make_shared<sc13::runtime::RuntimeCommitDomain>();
    std::unique_ptr<sc13::mods::ModManager> manager;
    /** @summary Discover one truly inactive canonical owner through the unchanged legacy reader. */
    Fixture(){
        Write("return {}");
        manager=std::make_unique<sc13::mods::ModManager>(root/"mods",root/"state.toon",registry,cache,
            sc13::mods::RuntimeResourceRefreshCallback{},sc13::scripting::ScriptScheduler::LogSink{},nullptr,domain);
        std::string error;Check(manager->Initialize(error),"manager discovery");Check(manager->Snapshot().size()==1,"actual entry");
    }
    /** @summary Close manager before borrowed resources and delete owned fixture inputs. */
    ~Fixture(){manager.reset();std::error_code error;std::filesystem::remove_all(root,error);}
    /** @summary Write a complete candidate source generation. @param source Entry bytes. @param memory VM budget in bytes. */
    void Write(std::string_view source,std::size_t memory=8U*1024U*1024U){
        std::filesystem::create_directories(root/"mods/one/scripts");
        sc13::tests::WriteTextFile(root/"mods/one/mod.toml",sc13::tests::FixtureHeader("fixture.owner","Preparation")+
            "[scripts]\nlanguage='luau'\nentry='scripts/init.luau'\nmemoryMiB="+std::to_string(memory/(1024U*1024U))+"\nbudgetMs=20\n");
        sc13::tests::WriteTextFile(root/"mods/one/scripts/init.luau",source);
    }
    /** @summary Capture with the actual entry's process-local installation. @param pin Expected Base. @return Immutable candidate. */
    std::shared_ptr<const sc13::mods::CapturedCandidate> Capture(const sc13::scripting::BasePackagePin& pin){
        return Access::Capture(*manager,"fixture.owner",root/"mods/one",pin);
    }
};
/** @summary Prove successful local staging, immutable consumed sources and no activation/effects.
 * @param base Actual Base host. @param pin Original pin. */
void Staging(const std::shared_ptr<sc13::scripting::BaseHost>& base,const sc13::scripting::BasePackagePin& pin){
    Fixture fixture;
    fixture.Write(R"(
local helper=require('./helper')
assert(helper==31)
assert(io==nil and (os==nil or os.execute==nil) and package==nil and loadfile==nil and dofile==nil)
assert(simoder.hasCapability('game.props.float'))
local p=simoder.props
p.setFloat('00B1B104:61EFC000:719436BD','89ABCDEF',31)
p.clearFloat('00B1B104:61EFC000:719436BD','89ABCDEF')
p.setFloat('00B1B104:61EFC000:719436BD','89ABCDEE',32)
simoder.log('entry-original')
return {onEnable=function() simoder.log('enable-original') end,
 onTick=function() error('unexpected tick') end,onDisable=function() error('unexpected disable') end}
)");
    sc13::tests::WriteTextFile(fixture.root/"mods/one/scripts/helper.luau","return 31");
    auto capture=fixture.Capture(pin);
    fixture.Write("error('reopened entry')");
    sc13::tests::WriteTextFile(fixture.root/"mods/one/scripts/helper.luau","error('reopened module')");
    std::filesystem::remove_all(fixture.root/"mods/one/scripts");
    const auto generation=fixture.registry.Current()->generation();const auto revision=fixture.domain->Revision();
    std::atomic_uint disable{},close{},tick{};
    /** @summary Keep actual per-generation evidence rather than hiding legitimate rejected-attempt replay. */
    struct Events final{unsigned create{},entry{},enable{},disable{},close{},tick{};};
    std::mutex eventsMutex;std::map<std::uint64_t,Events> events;
    auto plan=Positive(*fixture.manager,capture,base,[&](auto phase,auto generation){
        {std::lock_guard lock(eventsMutex);auto& record=events[generation];
            if(phase==sc13::scripting::ScriptPhase::Create)++record.create;
            if(phase==sc13::scripting::ScriptPhase::Entry)++record.entry;
            if(phase==sc13::scripting::ScriptPhase::Enable)++record.enable;
            if(phase==sc13::scripting::ScriptPhase::Disable)++record.disable;
            if(phase==sc13::scripting::ScriptPhase::Close)++record.close;
            if(phase==sc13::scripting::ScriptPhase::Tick)++record.tick;}
        if(phase==sc13::scripting::ScriptPhase::Disable)++disable;
        if(phase==sc13::scripting::ScriptPhase::Close)++close;
        if(phase==sc13::scripting::ScriptPhase::Tick)++tick;
    });
    Check(Access::Current(plan)==ScopeResult::Ok,"exact entry and Base retained");
    const auto& declarations=Access::Declarations(plan);
    Check(declarations.size()==1 && declarations[0].properties.size()==1,"set clear reducer parity");
    Check(declarations[0].properties[0].propertyId==0x89ABCDEEU && declarations[0].properties[0].value==32,"exact staged result");
    auto vm=Access::Vm(plan);
    const auto winningGeneration=Access::Endpoint(plan)->generation;
    Check(!vm->Status().running,"prepared status never operational");
    Check(vm->DrainLogs().empty(),"public log drain keeps preparation private");
    const auto logs=Access::StagedLogs(plan);Check(logs.size()==2 && logs[0]=="entry-original" && logs[1]=="enable-original","captured bytes used");
    Check(vm->TakePatchCommands().empty(),"commands consumed only into retained declarations");
    Reject([&]{vm->Commit();},"direct commit denied");vm->Tick(1);Check(tick==0,"prepared tick denied");
    sc13::scripting::ScriptScheduler scheduler({});
    Reject([&]{scheduler.Put("fixture.owner",vm);},"legacy Put denied");
    sc13::scripting::SchedulerPublicationTestAccess::Enroll(scheduler,fixture.domain);
    sc13::scripting::SchedulerPublicationPlan publication;
    Check(sc13::scripting::SchedulerPublicationTestAccess::Prepare(scheduler,"fixture.owner",vm,
        sc13::scripting::SchedulerPublicationPurpose::Activate,publication)!=ScopeResult::Ok,"typed fixture admission denied");
    auto origin=sc13::mods::OwnedPublicationTestAccess::Issue(vm,Access::Endpoint(plan),capture->ContentDigest());
    Check(static_cast<bool>(origin),"actual uncommitted origin fixture");
    auto jointRegistry=std::make_shared<sc13::mods::PatchRegistry>();
    auto record=sc13::mods::OwnerLifecycleTestAccess::IssueCandidate(origin,jointRegistry,fixture.domain);
    Check(static_cast<bool>(record),"isolated owner fixture before enrollment");
    sc13::mods::OwnerLifecycle::Plan joint;std::string error;
    Check(sc13::mods::OwnerLifecycleTestAccess::Joint(scheduler,record,{},origin,{},{},{},{},joint,error)!=ScopeResult::Ok,
        "joint fixture cannot enroll prepared VM");
    Check(Access::Current(plan)==ScopeResult::Ok,"rejected joint enrollment changes no native bindings");
    origin.reset();record.reset();
    Check(fixture.registry.Current()->generation()==generation && fixture.domain->Revision()==revision,"no publication effects");
    Check(fixture.manager->EnabledOrder().empty(),"no enabled order mutation");
    const auto scope=Access::Scope(plan)->Current();Check(!scope.activationCommitted,"preparation issued no activation");
    vm.reset();plan=Plan{};
    std::lock_guard lock(eventsMutex);Check(events.contains(winningGeneration),"winning actual native generation recorded");
    Check(events.at(winningGeneration).close==1&&disable==0&&tick==0,"successful uncommitted generation closes once without disable/tick");
    for(const auto& [nativeGeneration,observedRecord]:events){Check(nativeGeneration!=0&&observedRecord.create==1&&observedRecord.entry==1&&observedRecord.enable==1,"every actual initialized attempt executes its own lifecycle once");Check(observedRecord.close==1&&observedRecord.disable==0&&observedRecord.tick==0,"each rejected or winning generation closes exactly once without operational callbacks");}
    Check(close==events.size(),"aggregate close count matches actual distinct native generations");
}
/** @summary Reject every live semantic boundary while allowing bounded staging and diagnostics.
 * @param base Actual package. @param pin Original pin. */
void Admission(const std::shared_ptr<sc13::scripting::BaseHost>& base,const sc13::scripting::BasePackagePin& pin){
    Fixture fixture;fixture.Write(R"(
local function denied(f,...)
 local value,reason=f(...)
 assert(value==nil and reason=='NotReady')
end
denied(base.values.validate,'bad','bad')
denied(base.values.wire,nil)
denied(base.request,'missing',nil)
denied(base.run,function() error('ran') end)
denied(base.snapshot)
denied(base.subscribe,function() error('subscription') end)
denied(base.descriptors)
local okay=pcall(function() simoder.props.getFloat('00B1B104:61EFC000:719436BD','89ABCDEF') end)
assert(not okay)
simoder.props.setFloat('00B1B104:61EFC000:719436BD','89ABCDEF',7)
simoder.log('restricted-complete')
return {}
)");
    auto plan=Positive(*fixture.manager,fixture.Capture(pin),base);
    Check(Access::Declarations(plan).size()==1,"set without fake reader");
    auto vm=Access::Vm(plan);Check(vm->DrainLogs().empty(),"restricted logs remain unpublished");const auto logs=Access::StagedLogs(plan);
    Check(logs.size()==1 && logs[0]=="restricted-complete","all admission assertions executed");
    Check(vm->SemanticDiagnostics().empty(),"no operational semantic admission recorded");
}
/** @summary Exercise VM errors and bounded rejection followed by a fresh healthy preparation.
 * @param base Actual package. @param pin Full Base pin. */
void Failures(const std::shared_ptr<sc13::scripting::BaseHost>& base,const sc13::scripting::BasePackagePin& pin){
    Fixture fixture;
    const std::vector<std::string> failures{
        "error('entry failure')",
        "return {onEnable=function() error('enable failure') end}",
        "while true do end",
        "local a={} while true do a[#a+1]=string.rep('x',4096) end",
        "for i=1,257 do simoder.props.setFloat('00B1B104:61EFC000:719436BD',i,1) end return {}",
        "simoder.props.setFloat('00B1B104:61EFC000:719436BD',1,0/0) return {}",
        "simoder.props.setFloat('00B1B104:61EFC000:719436BD',1,math.huge) return {}",
        "simoder.props.setFloat('bad',1,2) return {}",
        "simoder.props.getFloat('00B1B104:61EFC000:719436BD',1) return {}",
        "return require('./missing')"
    };
    const auto generation=fixture.registry.Current()->generation();const auto revision=fixture.domain->Revision();
    for(const auto& source:failures){
        fixture.Write(source,2U*1024U*1024U);auto capture=fixture.Capture(pin);
        Reject([&]{auto plan=Access::Prepare(*fixture.manager,capture,base);},"initializer rejects actual error/budget");
        fixture.Write("simoder.log('recovered') return {}");
        auto recovered=Positive(*fixture.manager,fixture.Capture(pin),base);
        Check(Access::Current(recovered)==ScopeResult::Ok,"failed initializer releases slot and memory");
    }
    fixture.Write("return {}");auto capture=fixture.Capture(pin);
    failAllocation=true;
    Reject([&]{auto plan=Access::Prepare(*fixture.manager,capture,base);},"host allocation failure propagates");
    failAllocation=false;
    auto recovered=Positive(*fixture.manager,capture,base);Check(Access::Current(recovered)==ScopeResult::Ok,"host allocation recovery");
    Check(fixture.registry.Current()->generation()==generation && fixture.domain->Revision()==revision,"failures publish nothing");
    recovered=Plan{};
    fixture.Write("for i=1,140 do simoder.log('bounded') end return {}");
    auto logged=Positive(*fixture.manager,fixture.Capture(pin),base);
    auto logger=Access::Vm(logged);Check(logger->DrainLogs().empty(),"bounded diagnostics stay unpublished");Check(Access::StagedLogs(logged).size()==128,"diagnostics bounded to128");
    Check(logger->Status().droppedLogs==12,"excess diagnostics counted");
}
/** @summary Bind actual installation/stamp and Base pins rather than caller-authored expected text.
 * @param base Actual Base. @param pin Original pin. */
void Identities(const std::shared_ptr<sc13::scripting::BaseHost>& base,const sc13::scripting::BasePackagePin& pin){
    Fixture fixture;auto capture=fixture.Capture(pin);
    auto foreign=sc13::mods::CapturedCandidate::Capture(fixture.root/"mods/one",sc13::mods::InstallationBinding::Create(),pin);
    unsigned created{};
    Reject([&]{auto plan=Access::Prepare(*fixture.manager,foreign,base,[&](auto,auto){++created;});},"foreign native installation rejected");
    Check(created==0,"installation rejected before VM");
    auto wrong=pin;wrong.version="different";
    auto wrongCapture=fixture.Capture(wrong);
    Reject([&]{auto plan=Access::Prepare(*fixture.manager,wrongCapture,base);},"actual Base version mismatch");
    wrong=pin;wrong.files.begin()->second=std::string(64,'A');wrongCapture=fixture.Capture(wrong);
    Reject([&]{auto plan=Access::Prepare(*fixture.manager,wrongCapture,base);},"actual Base content pin mismatch");
    auto plan=Positive(*fixture.manager,capture,base);
    Check(Lifetime::ReplaceStamp(*fixture.manager,"fixture.owner"),"replace actual current stamp");
    Check(Access::Current(plan)==ScopeResult::Stale,"prepared stamp cannot follow successor");
    auto observed=Positive(*fixture.manager,capture,base);
    std::filesystem::remove_all(fixture.root/"mods/one");std::string error;
    Check(fixture.manager->Refresh(error),"erase actual entry");fixture.Write("return {}");
    Check(fixture.manager->Refresh(error),"recreate actual entry");
    Check(Access::Current(observed)==ScopeResult::Stale,"entry ABA rejected");
    Reject([&]{auto invalid=Access::Prepare(*fixture.manager,capture,base);},"old capture installation cannot bind recreated ID");
    auto retained=Positive(*fixture.manager,fixture.Capture(pin),base);
    fixture.manager.reset();Check(Access::Current(retained)==ScopeResult::Cancelled,"closed manager denies without borrowed access");
    Check(!Access::Vm(retained)->Status().running,"retained preparation never running after manager destruction");
}
/** @summary Verify preparation lease drains on shutdown and detects concurrent canonical changes.
 * @param base Actual retained package. @param pin Exact package pin. */
void Interleavings(const std::shared_ptr<sc13::scripting::BaseHost>& base,const sc13::scripting::BasePackagePin& pin){
    Fixture fixture;auto capture=fixture.Capture(pin);
    std::promise<void> entered,release;auto ready=entered.get_future();auto resume=release.get_future().share();
    auto* manager=fixture.manager.get();
    auto prepared=std::async(std::launch::async,[&]{
        try{auto plan=Access::Prepare(*manager,capture,base,[&](auto phase,auto){
            if(phase==sc13::scripting::ScriptPhase::Entry){entered.set_value();if(resume.wait_for(5s)!=std::future_status::ready)throw std::runtime_error("barrier timeout");}
        });return false;}catch(const std::exception&){return true;}
    });
    Check(ready.wait_for(2s)==std::future_status::ready,"actual initializer pause");
    auto lifetime=Lifetime::Capture(*manager,"fixture.owner");
    auto closing=std::async(std::launch::async,[&]{fixture.manager.reset();});
    for(unsigned i=0;i<200 && !Lifetime::Closed(lifetime);++i)std::this_thread::sleep_for(1ms);
    Check(Lifetime::Closed(lifetime),"closure visible during leased initializer");
    Check(closing.wait_for(20ms)!=std::future_status::ready,"destructor drains active initializer lease");
    Check(Lifetime::ManagerAvailable(lifetime),"no M held during initializer");
    {auto writer=fixture.domain->ReserveWriter();Check(writer.Acquired(),"no D held during initializer");}
    release.set_value();Check(prepared.get(),"closure rejects post-init candidate");closing.get();
}
/** @summary Reject same-entry refresh and actual M contention at the post-initialization check.
 * @param base Actual Base. @param pin Exact pin. @param contention Select manager-lock conflict instead of refreshed content. */
void ChangedDuringInitialization(const std::shared_ptr<sc13::scripting::BaseHost>& base,
    const sc13::scripting::BasePackagePin& pin,bool contention){
    Fixture fixture;auto capture=fixture.Capture(pin);
    std::promise<void> entered,release;auto ready=entered.get_future();auto resume=release.get_future().share();
    auto preparation=std::async(std::launch::async,[&]{
        try{auto plan=Access::Prepare(*fixture.manager,capture,base,[&](auto phase,auto){
            if(phase==sc13::scripting::ScriptPhase::Entry){entered.set_value();static_cast<void>(resume.wait_for(5s));}
        });return false;}catch(const std::exception&){return true;}
    });
    Check(ready.wait_for(2s)==std::future_status::ready,"source-stale initializer pause");
    if(contention){
        auto lock=Lifetime::HoldManager(*fixture.manager);release.set_value();
        Check(preparation.wait_for(2s)==std::future_status::ready,"post-init try-M does not wait");
        Check(preparation.get(),"M conflict rejects preparation");
    }else{
        fixture.Write("simoder.log('new canonical generation') return {}");std::string error;
        Check(fixture.manager->Refresh(error),"actual same-entry Refresh during initialization");
        release.set_value();Check(preparation.get(),"changed legacy fingerprint snapshot rejects preparation");
    }
    Check(fixture.manager->EnabledOrder().empty(),"interleaved preparation remains inactive");
}
/** @summary Reject stopped scopes/domains and changed native context without interpreting them as consent.
 * @param base Actual Base. @param pin Exact pin. @param bridge Original actual Base bridge. */
void Freshness(const std::shared_ptr<sc13::scripting::BaseHost>& base,const sc13::scripting::BasePackagePin& pin,
    sc13::scripting::SemanticBridge& bridge){
    Fixture fixture;auto capture=fixture.Capture(pin);
    auto plan=Positive(*fixture.manager,capture,base);
    {
        auto lock=Lifetime::HoldManager(*fixture.manager);
        auto current=std::async(std::launch::async,[&]{return Access::Current(plan);});
        Check(current.wait_for(1s)==std::future_status::ready && current.get()==ScopeResult::Busy,"Current uses actual try-M");
    }
    Access::Scope(plan)->RequestStop(sc13::runtime::StopReason::ExplicitShutdown);
    Check(Access::Current(plan)==ScopeResult::Stale,"stopped original scope rejected");
    plan=Positive(*fixture.manager,capture,base);
    {auto writer=fixture.domain->ReserveWriter();Check(writer.Acquired() && writer.Advance()!=0,"fixture domain revision change");}
    Check(Access::Current(plan)==ScopeResult::Stale,"original domain revision retained");
    plan=Positive(*fixture.manager,capture,base);
    sc13::scripting::SemanticBridgeTestAccess::ObserveContext(bridge,sc13::scripting::SemanticContextKind::City,10,2,true);
    Check(Access::Current(plan)==ScopeResult::Stale,"native context facts cannot silently rebind");
    plan=Positive(*fixture.manager,capture,base);
    sc13::scripting::SemanticBridgeTestAccess::ObserveContext(bridge,sc13::scripting::SemanticContextKind::City,10,2,false);
    Check(Access::Current(plan)==ScopeResult::Stale,"single-player factual witness retained");
    plan=Positive(*fixture.manager,capture,base);
    fixture.domain->RequestStop();Check(Access::Current(plan)==ScopeResult::Cancelled,"stopped original domain rejected by manager close precedence");
    Reject([&]{auto next=Access::Prepare(*fixture.manager,capture,base);},"stopped manager domain cannot prepare");
}
/** @summary Deny unsupported captured kind before VM startup. @param base Actual package. @param pin Exact pin. */
void Eligibility(const std::shared_ptr<sc13::scripting::BaseHost>& base,const sc13::scripting::BasePackagePin& pin){
    Fixture fixture;
    auto ordinary=fixture.Capture(pin);std::string error;unsigned activeCreated{};
    Check(fixture.manager->SetEnabled("fixture.owner",true,error),"real legacy owner active");
    Reject([&]{auto invalid=Access::Prepare(*fixture.manager,ordinary,base,[&](auto,auto){++activeCreated;});},"active legacy cannot be adopted");
    Check(activeCreated==0,"active legacy rejected before VM");
    Check(fixture.manager->SetEnabled("fixture.owner",false,error),"legacy owner returned inactive");
    {auto writer=fixture.domain->ReserveWriter();Check(writer.Acquired(),"writer fixture acquired");
        Reject([&]{auto invalid=Access::Prepare(*fixture.manager,ordinary,base);},"initialization forbidden under D");}
    std::filesystem::remove_all(fixture.root/"mods/one/scripts");
    sc13::tests::WriteTextFile(fixture.root/"mods/one/mod.toml",sc13::tests::FixtureHeader("fixture.owner","Declarative")+sc13::tests::FixturePatch(2));
    auto capture=fixture.Capture(pin);unsigned created{};
    Reject([&]{auto plan=Access::Prepare(*fixture.manager,capture,base,[&](auto,auto){++created;});},"declarative-only requires separate identity unit");
    Check(created==0,"unsupported before VM creation");
    for(const auto& field:{std::string("[services]\ncommand='forbidden'\n"),std::string("[[resources]]\nid='forbidden'\n"),std::string("dependencies=['other']\n")}){
        sc13::tests::WriteTextFile(fixture.root/"mods/one/mod.toml",sc13::tests::FixtureHeader("fixture.owner","Unsupported")+field);
        Reject([&]{auto invalid=fixture.Capture(pin);},"unsupported manifest rejected before preparer");
    }
}
}
/** @summary Inject one native allocation failure on the calling test thread. @param size Requested bytes. @return Owned memory. */
void* operator new(std::size_t size){if(failAllocation){failAllocation=false;throw std::bad_alloc();}if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
/** @summary Release test-owned ordinary allocation. @param pointer Owned block. */
void operator delete(void* pointer) noexcept{std::free(pointer);}
/** @summary Release sized test-owned allocation. @param pointer Owned block. @param size Unused allocator size. */
void operator delete(void* pointer,std::size_t size) noexcept{static_cast<void>(size);std::free(pointer);}
/** @summary Run isolated actual manager/Base/VM qualification; never launches a game or operating-system VM.
 * @param argc Argument count. @param argv Actual Base checkout. @return Zero only on behavioral success. */
int main(int argc,char** argv){
    sc13::scripting::CallbackBudgetTestAccess::Observation coldBudget;
    bool coldRequested{},coldReported{};
    try{
        Check(argc==2,"Base checkout argument required");auto pin=Pin(argv[1]);
        auto bridge=std::make_shared<sc13::scripting::SemanticBridge>();
        /** @summary Arm on the actual first Enable lane event, cancelling only an unconsumed matching arm on handled Close.
         * @param phase Actual native lifecycle phase. @param generation Original runtime generation emitted by that event. */
        const auto observeCold=[&](sc13::scripting::ScriptPhase phase,std::uint64_t generation) noexcept{
            if(phase==sc13::scripting::ScriptPhase::Enable&&!coldRequested){coldRequested=true;static_cast<void>(sc13::scripting::CallbackBudgetTestAccess::Arm(generation,phase,coldBudget));}
            if(phase==sc13::scripting::ScriptPhase::Close)static_cast<void>(sc13::scripting::CallbackBudgetTestAccess::Cancel(generation));
        };
        auto base=std::make_shared<sc13::scripting::BaseHost>(argv[1],pin,observeCold,bridge);
        ReportColdBudget(coldBudget,coldRequested);coldReported=true;
        Check(base->Ready(),"actual Base qualified");
        Progress("Staging");Staging(base,pin);Progress("Admission");Admission(base,pin);Progress("Failures");Failures(base,pin);Progress("Identities");Identities(base,pin);Progress("Interleavings");Interleavings(base,pin);
        Progress("ChangedDuringInitialization Refresh");ChangedDuringInitialization(base,pin,false);Progress("ChangedDuringInitialization M");ChangedDuringInitialization(base,pin,true);Progress("Freshness");Freshness(base,pin,*bridge);Progress("Eligibility");Eligibility(base,pin);
        Progress("FinalBaseClose");Fixture fixture;auto plan=Positive(*fixture.manager,fixture.Capture(pin),base);
        bridge->Close();Check(Access::Current(plan)==ScopeResult::Stale,"retained Base close invalidates endpoint witness");
        Reject([&]{auto next=Access::Prepare(*fixture.manager,fixture.Capture(pin),base);},"closed Base rejects before initialization");
        std::cout<<checks<<" manager preparation checks passed\n";return 0;
    }catch(const std::exception& error){failAllocation=false;if(!coldReported)ReportColdBudget(coldBudget,coldRequested);std::cerr<<"manager preparation: "<<error.what()<<'\n';return 1;}
}
