#include "mods/manager_commands.hpp"
#include <array>
#include "mods/manager_close.hpp"
#include "mods/manager_publication.hpp"
#include "scripting/source_project.hpp"
#include "scripting/scheduler_publication.hpp"
#include "mod_fixture.hpp"
#include "test_harness.hpp"
#include <Luau/Compiler.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <utility>

namespace {
using namespace sc13::mods;using namespace sc13::runtime;using namespace sc13::scripting;
using Close=ManagerClose;using Access=ManagerCloseTestAccess;using Preparation=ManagerPreparationTestAccess;using Publication=ManagerPublicationTestAccess;using Scope=ScopeRetirementTestAccess;using Decisions=LifecycleDecisionTestAccess;using namespace std::chrono_literals;
std::atomic_uint checks{};
/** @summary Require observed native behavior. @param value Actual condition. @param message Fixed failure evidence. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Bound each stateful fixture observation once per iteration. @param predicate Actual observation. @param message Fixed failure evidence. */
template<class F> void Await(F predicate,const char* message){const auto end=std::chrono::steady_clock::now()+5s;bool done=predicate();while(!done&&std::chrono::steady_clock::now()<end){std::this_thread::sleep_for(1ms);done=predicate();}Check(done,message);}
/** @summary Own one finite real producer/control pause. */
struct Gate final{std::mutex mutex;std::condition_variable changed;bool entered{},released{};
    /** @summary Resume any original paused work during unwinding. */
    ~Gate(){Release();}
    /** @summary Pause first actual visit outside guards. */
    void Pause(){std::unique_lock lock(mutex);if(entered)return;entered=true;changed.notify_all();if(!changed.wait_for(lock,5s,[&]{return released;}))throw std::runtime_error("prepared retirement pause timeout");}
    /** @summary Require actual native entry. */
    void Wait(){std::unique_lock lock(mutex);Check(changed.wait_for(lock,5s,[&]{return entered;}),"actual prepared retirement pause reached");}
    /** @summary Resume once. */
    void Release(){std::lock_guard lock(mutex);released=true;changed.notify_all();}
};
/** @summary Pin every actual Base source. @param root Verified local package. @return Complete native pin. */
BasePackagePin Pin(const std::filesystem::path& root){BasePackagePin pin{"0.1.0-alpha",{}};pin.files.emplace("mod.toml",HashText(ReadBounded(root/"mod.toml",4096)));for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"scripts"))if(entry.is_regular_file())pin.files.emplace(entry.path().lexically_relative(root).generic_string(),HashText(ReadBounded(entry.path(),1024U*1024U)));return pin;}
/** @summary Own original actual canonical manager and trusted Base fixture. */
struct Fixture final{
    std::filesystem::path root=sc13::tests::CreateTemporaryDirectory();std::unique_ptr<PatchRegistry> registry=std::make_unique<PatchRegistry>();std::unique_ptr<RuntimeResourceCache> cache=std::make_unique<RuntimeResourceCache>();std::shared_ptr<RuntimeCommitDomain> domain=std::make_shared<RuntimeCommitDomain>();std::shared_ptr<SemanticBridge> bridge=std::make_shared<SemanticBridge>();BasePackagePin pin;std::shared_ptr<BaseHost> base;std::unique_ptr<ModManager> manager;
    /** @summary Initialize real Base/native manager without game effects. @param package Actual Base checkout. */
    explicit Fixture(const std::filesystem::path& package):pin(Pin(package)){SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::Runtime,1,1);SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,7,3);base=std::make_shared<BaseHost>(package,pin,std::function<void(ScriptPhase,std::uint64_t)>{},bridge);Check(base->Ready(),"actual Base ready");const auto path=root/"mods"/"fixture.owner";std::filesystem::create_directories(path/"scripts");sc13::tests::WriteTextFile(path/"mod.toml",sc13::tests::FixtureHeader("fixture.owner","Prepared retirement")+"[scripts]\nlanguage='luau'\nentry='scripts/init.luau'\nmemoryMiB=2\nbudgetMs=20\n"+sc13::tests::FixturePatch(3));sc13::tests::WriteTextFile(path/"scripts/init.luau","return {onTick=function() end,onDisable=function() end}");manager=std::make_unique<ModManager>(root/"mods",root/"state.toon",*registry,*cache,RuntimeResourceRefreshCallback{},ScriptScheduler::LogSink{},nullptr,domain);std::string error;Check(manager->Initialize(error),"actual manager initialized");Check(Publication::Enroll(*manager)==ScopeResult::Ok,"actual scheduler enrolled");}
    /** @summary Close native owners before real borrowed resources. */
    ~Fixture(){manager.reset();base.reset();cache.reset();registry.reset();std::error_code error;std::filesystem::remove_all(root,error);}
    /** @summary Capture immutable original source once. @return Native source owner. */
    auto Capture(){return Preparation::Capture(*manager,"fixture.owner",root/"mods"/"fixture.owner",pin);}
    /** @summary Prepare genuine original Plan without authority or activation. @param capture Optional already-captured immutable source, retained unchanged across Busy. @return Move-only original Plan. */
    ManagerPreparation::Plan Candidate(std::shared_ptr<const CapturedCandidate> capture={}){if(!capture)capture=Capture();const auto end=std::chrono::steady_clock::now()+5s;for(unsigned i=0;i!=4096;++i){try{return Publication::Candidate(*manager,capture,base);}catch(const std::runtime_error& error){if(std::string(error.what())!="preparation.busy")throw;}if(std::chrono::steady_clock::now()>=end)break;std::this_thread::yield();}throw std::runtime_error("bounded positive candidate remains Busy");}
};
/** @summary Own original requested State and exclusive native token. */
struct Closing final{Close::Handle handle;Close::Token token;
    /** @summary Request actual close without ordinary borrower count. @param manager Original owner. */
    explicit Closing(ModManager& manager):handle(Access::Capture(manager)){Check(Close::Request(handle)==ScopeResult::Ok&&Close::Acquire(handle,token)==ScopeResult::Ok,"original close owner");}
};
/** @summary Passively observe opaque holder without closing or releasing it. @param holder Persistent original Plan owner. @return Scalar native facts. */
auto Observe(const Close::PreparedRetirement& holder){auto value=Close::ObservePrepared(holder);Check(value.targetCount<=2&&value.holderBytes<=4096,"fixed holder and target bounds");Check(value.remainingRetirement&&!value.finalizable,"prepared completion is not manager finalization");return value;}
/** @summary Explicitly drain one persistent original holder to acknowledged target completion. @param token Original close owner. @param holder Persistent whole Plan. */
void Drain(Close::Token& token,Close::PreparedRetirement& holder){Await([&]{const auto action=Close::DrainPrepared(token,holder);Check(action.remainingRetirement&&!action.finalizable,"explicit Plan action stays partial");const auto view=Observe(holder);return view.targetCount!=0&&view.completedTargets==view.targetCount;},"genuine prepared targets acknowledged");}
/** @summary Bind actual factual references through the original native bridge. @param fixture Original host. @param endpoint Exact origin. @return Owned factual lease. */
SemanticContextLease Facts(Fixture& fixture,const std::shared_ptr<SemanticEndpoint>& endpoint){copied::Envelope input;Check(fixture.bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]})","test.echo@1",endpoint->storageOwner,input)==copied::Status::Ok,"actual factual reference payload");SemanticContextLease facts;Check(fixture.bridge->BindReferences(*endpoint,input,facts)==ScopeResult::Ok,"actual original factual lease");return facts;}
/** @summary Retain each explicitly listed origin without manufacturing a universal owner census. */
struct Origin final{ManagerPreparation::Plan preparation;std::shared_ptr<RuntimeScope> scope;std::shared_ptr<ScriptRuntime> vm;std::shared_ptr<SemanticEndpoint> endpoint;std::weak_ptr<const CapturedCandidate> source;OperationContext root;LifecycleDecision decision;SemanticContextLease facts;
    /** @summary Release ordinary native owners off all component guards after acknowledgements. */
    void Drop(){preparation=ManagerPreparation::Plan{};vm.reset();endpoint.reset();facts={};decision={};scope.reset();}
};
/** @summary Create one genuine original prepared origin and reviewed lifecycle root. @param fixture Real native host. @return Explicit origin owners. */
Origin Make(Fixture& fixture){Origin result;auto capture=fixture.Capture();result.source=capture;result.preparation=fixture.Candidate(capture);result.scope=Preparation::Scope(result.preparation);result.vm=Preparation::Vm(result.preparation);result.endpoint=Preparation::Endpoint(result.preparation);result.scope->SetReadiness(true);result.root=RuntimeScopeTestAccess::Issue(*result.scope);Check(result.root.owner!=0,"actual original reviewed root");Check(Decisions::RootDecision(result.scope,result.root,result.decision)==ScopeResult::Ok,"actual original lifecycle presentation without forced activation");result.facts=Facts(fixture,result.endpoint);return result;}
/** @summary Commit an original native Plan with existing finite Busy-only admission bound. @param plan Actual one-shot Plan. */
void Commit(ManagerPublication::Plan& plan){const auto end=std::chrono::steady_clock::now()+5s;ScopeResult result=ScopeResult::Busy;for(unsigned i=0;i!=4096&&result==ScopeResult::Busy&&std::chrono::steady_clock::now()<end;++i){result=Publication::Commit(plan);if(result==ScopeResult::Busy)std::this_thread::yield();}Check(result==ScopeResult::Ok,"actual canonical publication committed");}
/** @summary Prepare a genuine initial/handoff Plan retaining exact original owners. @param fixture Actual manager. @param old Current origin or null. @param next Real candidate. @param oldDecision Original old lease or empty. @param output Empty whole Plan. */
void Publish(Fixture& fixture,Origin* old,Origin& next,const LifecycleDecision& oldDecision,ManagerPublication::Plan& output){std::string error;Check(Publication::Prepare(*fixture.manager,std::move(next.preparation),oldDecision,next.decision,old?Facts(fixture,old->endpoint):SemanticContextLease{},next.facts,output,error)==ScopeResult::Ok,"actual whole original publication prepared");}
/** @summary Own a genuine extracted envelope independently of the original callback's stack. */
struct Extracted final{std::mutex mutex;ScriptCommandBatch batch;bool captured{};};
/** @summary Retain native Close evidence without borrowed stack lifetimes. */
struct CloseEvidence final{std::atomic_uint closes{};std::atomic_bool dependencies{true};std::weak_ptr<BaseHost> base;};
/** @summary Observe exact original VM Close and original dependencies through a native callback. @param vm Actual VM. @param source Exact original weak capture. @param evidence Owned fixed observer facts. */
void Track(const std::shared_ptr<ScriptRuntime>& vm,std::weak_ptr<const CapturedCandidate> source,const std::shared_ptr<CloseEvidence>& evidence){Access::Lifecycle(vm,[evidence,source](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Close){++evidence->closes;if(evidence->base.expired()||source.expired())evidence->dependencies=false;}});}
/** @summary Own the actual resource destructor pause independently of test stack unwinding. */
struct ResourceEvidence final{Gate gate;std::atomic_uint destroyed{};std::atomic_bool offQ{},busy{},failed{};std::weak_ptr<RuntimeScope> scope;};
/** @summary Copy only separately guarded scalar observations; no cross-component atomic-snapshot claim. */
struct ListedObservation final{Close::Observation manager;std::array<Scope::Observation,3> scopes;bool listedOwnersSettled{};};
/** @summary Sample only the explicit deduplicated cohort after external actions/producers join. @param token Original State token. @param scopes Three exact distinct scopes. @param slots Actual joined slot acknowledgements. @param prepared Actual whole-Plan target completion. @return Test-only conjunction without cleanup authority. */
ListedObservation Listed(Close::Token& token,const std::array<std::shared_ptr<RuntimeScope>,3>& scopes,bool slots,bool prepared){ListedObservation result;result.manager=Close::Poll(token);Check(result.manager.checked<=32&&result.manager.remainingRetirement&&!result.manager.finalizable,"native32 manager observation remains partial");bool ready=true;for(std::size_t i=0;i<scopes.size();++i){result.scopes[i]=Scope::Observe(*scopes[i]);ready=ready&&result.scopes[i].result==ScopeResult::Ok&&result.scopes[i].scopeReady;}result.listedOwnersSettled=slots&&prepared&&ready&&result.manager.result==ScopeResult::Ok&&result.manager.phase==Close::Phase::FoundationReady;return result;}
/** @summary Explicitly reduce/dispose one original scope off every publication guard. @param scope Exact original scope. */
void ScopeReady(RuntimeScope& scope){Await([&]{static_cast<void>(Scope::DrainOne(scope));const auto result=Scope::Observe(scope);return result.result==ScopeResult::Ok&&result.scopeReady;},"actual original scope reaches readiness");const auto final=Scope::Observe(scope);Check(final.commandTickets==0&&final.mailboxWriting==0&&final.mailboxReady==0&&final.mailboxDraining==0&&final.nonCommandFreshReservations==0&&final.unknownResources==0&&final.disposalSequence==final.disposalAcknowledged,"scope requires all original transfer/disposal acknowledgements");}
/** @summary Qualify only a genuine explicitly owned cohort through VM, scope, lease, mailbox and resource interleavings. @param package Actual pinned Base. */
void Cohort(const std::filesystem::path& package){
    Fixture fixture(package);
    // @summary Change only the fresh fixture source before its first capture; all later captures see these exact same bytes.
    sc13::tests::WriteTextFile(fixture.root/"mods"/"fixture.owner"/"scripts"/"init.luau","return {onTick=function() simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',9) end,onDisable=function() end}");
    auto old=Make(fixture);auto stored=std::make_shared<Extracted>();ManagerPublication::Plan initial;Publish(fixture,nullptr,old,{},initial);
    Check(ManagerCommandsTestAccess::Install(*fixture.manager,Publication::Origin(initial),old.root)==ScopeResult::Ok,"actual original dormant command route");
    ManagerCommandsTestAccess::Intercept(old.vm,[stored](ScriptCommandBatch& batch){std::lock_guard lock(stored->mutex);if(!stored->captured){stored->batch=std::move(batch);stored->captured=true;}else batch=ScriptCommandBatch{};});Commit(initial);
    Await([&]{std::lock_guard lock(stored->mutex);return stored->captured;},"actual original selected/extracted command batch retained");
    LifecycleDecision fresh;Check(Decisions::FreshDecision(old.scope,old.root,fresh)==ScopeResult::Ok,"actual old typed fresh lifecycle lease");auto next=Make(fixture);ManagerPublication::Plan handoff;Publish(fixture,&old,next,fresh,handoff);
    Check(ManagerCommandsTestAccess::Install(*fixture.manager,Publication::Origin(handoff),next.root)==ScopeResult::Ok,"actual successor command route");ManagerCommandsTestAccess::Intercept(next.vm,[](ScriptCommandBatch& batch){batch=ScriptCommandBatch{};});Commit(handoff);
    auto candidate=Make(fixture);const std::array<std::shared_ptr<RuntimeScope>,3> scopes{old.scope,next.scope,candidate.scope};Check(scopes[0]!=scopes[1]&&scopes[0]!=scopes[2]&&scopes[1]!=scopes[2],"cohort counts each original scope exactly once");Check(Access::CurrentVM(*fixture.manager,"fixture.owner")==next.vm,"actual successor is current record");
    auto evidence=std::make_shared<CloseEvidence>();evidence->base=fixture.base;Track(old.vm,old.source,evidence);Track(next.vm,next.source,evidence);Track(candidate.vm,candidate.source,evidence);
    auto resource=std::make_shared<ResourceEvidence>();resource->scope=candidate.scope;
    auto ownedResource=std::shared_ptr<void>(new int(7),[resource](void* pointer){++resource->destroyed;try{if(auto scope=resource->scope.lock()){{auto q=RuntimeScopeTestAccess::ReserveState(*scope);resource->offQ=q.owns_lock();}resource->busy=Scope::Observe(*scope).result==ScopeResult::Busy&&Scope::DrainOne(*scope)==ScopeResult::Busy;}resource->gate.Pause();}catch(...){resource->failed=true;}delete static_cast<int*>(pointer);});
    Check(Scope::RetainSafe(*candidate.scope,ownedResource),"actual qualified native resource retained");ownedResource.reset();
    Gate executing;auto control=std::async(std::launch::async,[&]{Access::InvokeControl(next.vm,[&]{executing.Pause();});});executing.Wait();Closing closing(*fixture.manager);const auto before=Close::Poll(closing.token);const auto scopeBefore=Scope::Observe(*next.scope);
    Close::PreparedRetirement deniedCandidate,deniedPredecessor;const std::weak_ptr<PublicationOrigin> originalOrigin=Publication::Origin(handoff);const auto candidateDenied=Close::CapturePreparation(closing.token,std::move(candidate.preparation),deniedCandidate);const auto predecessorDenied=Close::CapturePublication(closing.token,std::move(handoff),deniedPredecessor);
    executing.Release();control.get();Check(before.result==ScopeResult::NotReady||before.result==ScopeResult::Busy,"actual executing control prevents foundation readiness");Check(before.phase!=Close::Phase::FoundationReady&&scopeBefore.result==ScopeResult::NotReady&&!scopeBefore.scopeReady&&scopeBefore.running==0,"actual scope running0 cannot certify actual native control completion");Check((candidateDenied==ScopeResult::Busy||candidateDenied==ScopeResult::NotReady)&&(predecessorDenied==ScopeResult::Busy||predecessorDenied==ScopeResult::NotReady)&&Preparation::Vm(candidate.preparation)==candidate.vm&&Publication::Origin(handoff)==originalOrigin.lock()&&evidence->closes==0,"actual preaccepted control denies both captures without consuming genuine whole Plans or closing a VM");
    Close::PreparedRetirement predecessor,unused;Check(Close::CapturePublication(closing.token,std::move(handoff),predecessor)==ScopeResult::Ok&&Close::CapturePreparation(closing.token,std::move(candidate.preparation),unused)==ScopeResult::Ok,"actual whole predecessor and unused candidate captured after control acknowledgement");
    Check(Observe(predecessor).targetCount==1&&Observe(unused).targetCount==1,"original roles expose exact eligible targets only");Drain(closing.token,predecessor);Check(Access::LaneReleased(old.vm)&&!Access::LaneReleased(next.vm),"predecessor retirement excludes actual current successor");
    auto disposing=std::async(std::launch::async,[&]{return Close::DrainPrepared(closing.token,unused);});resource->gate.Wait();const auto resourcePending=Scope::Observe(*candidate.scope);const auto holderPending=Close::ObservePrepared(unused);resource->gate.Release();static_cast<void>(disposing.get());Check(resourcePending.result==ScopeResult::Busy&&!resourcePending.scopeReady&&(holderPending.result==ScopeResult::Busy||holderPending.actionInFlight),"actual native destructor blocks listed readiness until off-Q acknowledgement");Check(resource->offQ&&resource->busy&&resource->destroyed==1&&!resource->failed,"actual resource destructor runs outside Q with retained exclusive disposal turn");Drain(closing.token,unused);
    Await([&]{const auto action=Close::DrainOne(closing.token);Check(action.checked<=32&&action.remainingRetirement&&!action.finalizable,"actual current-record action bounded scan stays partial");return Access::LaneReleased(next.vm);},"actual successor current-record VM retired separately");
    const auto held=Scope::Observe(*old.scope);Check(held.result==ScopeResult::NotReady&&!held.scopeReady&&held.commandTickets>=2&&held.mailboxWriting+held.mailboxReady+held.mailboxDraining==0,"released VM slots do not qualify held original extracted batch and typed fresh lease");
    const auto predecessorDone=Observe(predecessor),candidateDone=Observe(unused);const bool prepared=predecessorDone.targetCount==1&&predecessorDone.completedTargets==1&&candidateDone.targetCount==1&&candidateDone.completedTargets==1;
    predecessor.Reset();fresh={};const auto envelopeOnly=Scope::Observe(*old.scope);Check(envelopeOnly.result==ScopeResult::NotReady&&envelopeOnly.commandTickets==1&&!envelopeOnly.scopeReady&&envelopeOnly.mailboxWriting+envelopeOnly.mailboxReady+envelopeOnly.mailboxDraining==0,"last typed lifecycle Plan/producer drop leaves genuine future envelope release pending");
    {auto q=RuntimeScopeTestAccess::ReserveState(*old.scope);Check(q.owns_lock(),"actual original scope Q held");auto observation=std::async(std::launch::async,[&]{return Scope::Observe(*old.scope);});const bool immediate=observation.wait_for(1s)==std::future_status::ready;q.unlock();const auto value=observation.get();Check(immediate&&value.result==ScopeResult::Busy,"genuine Q contention observation is try-only, not inferred readiness");}
    Gate writing;std::future<void> release;{auto q=RuntimeScopeTestAccess::ReserveState(*old.scope);Check(q.owns_lock(),"actual envelope release cannot borrow Q");release=std::async(std::launch::async,[&]{Scope::Barriers({},[&]{writing.Pause();});{std::lock_guard lock(stored->mutex);stored->batch=ScriptCommandBatch{};}Scope::Barriers({},{});});writing.Wait();}
    const auto writer=Scope::Observe(*old.scope);static_cast<void>(Scope::DrainOne(*old.scope));const auto undrained=Scope::Observe(*old.scope);writing.Release();release.get();Check(writer.mailboxWriting==1&&writer.commandTickets==1&&!undrained.scopeReady&&undrained.mailboxWriting==1,"real Writing release is never mistaken for Ready acknowledgement");const auto ready=Scope::Observe(*old.scope);Check(ready.mailboxReady==1&&ready.commandTickets==1&&!ready.scopeReady,"passive observation does not reduce genuine Ready mailbox");
    unused.Reset();initial=ManagerPublication::Plan{};old.decision={};next.decision={};candidate.decision={};for(const auto& scope:scopes)ScopeReady(*scope);
    const bool slots=Access::LaneReleased(old.vm)&&Access::LaneReleased(next.vm)&&Access::LaneReleased(candidate.vm);ListedObservation final;Await([&]{final=Listed(closing.token,scopes,slots,prepared);return final.listedOwnersSettled;},"explicit listed owners settle after every joined action and producer");Check(final.manager.remainingRetirement&&!final.manager.finalizable&&evidence->closes==3&&evidence->dependencies,"listed conjunction is partial; all three actual Close calls preserve original Base/capture owners");
    ManagerCommandsTestAccess::Intercept(old.vm,{});ManagerCommandsTestAccess::Intercept(next.vm,{});old.Drop();next.Drop();candidate.Drop();fixture.base.reset();fixture.manager.reset();Check(evidence->base.expired()&&old.source.expired()&&next.source.expired()&&candidate.source.expired(),"all actual native Plans/current owners released before Base/capture weak expiry assertions");
}
/** @summary Unknown generic resources/raw fresh reservations remain unqualified in a separate genuine negative cohort. */
void UnknownOwners(){auto domain=std::make_shared<RuntimeCommitDomain>();auto scope=RuntimeScopeTestAccess::Create(domain);Check(scope->CommitActivation(),"negative scope activated by native fixture");scope->SetReadiness(true);const auto root=RuntimeScopeTestAccess::Issue(*scope,false,true,true,1);Check(root.owner!=0&&root.reservation!=0,"actual original root issued before raw reservation");const auto raw=RuntimeScopeTestAccess::Reserve(*scope,root);Check(raw.owner!=0&&raw.reservation!=root.reservation,"actual raw noncommand fresh owner retained");auto resource=std::make_shared<int>(9);std::weak_ptr<int> weak=resource;Check(scope->RetainResource(resource),"actual unknown opaque resource retained");resource.reset();static_cast<void>(Scope::DrainOne(*scope));const auto value=Scope::Observe(*scope);Check(value.result==ScopeResult::NotReady&&!value.scopeReady&&value.nonCommandFreshReservations==1&&value.unknownResources==1&&value.resources==1&&!weak.expired(),"unknown and raw owners never join positive listed completion");}
}
/** @summary Run only the explicitly listed native cohort, not a manager-wide closure certificate. @param argc Actual Base argument count. @param argv Pinned Base root. @return Zero only after real required branches pass. */
int main(int argc,char** argv){try{Check(argc==2,"actual Base checkout required");Cohort(std::filesystem::path(argv[1]));UnknownOwners();Check(sc13::tests::FailureCount()==0,"original shared fixture setup");std::cout<<checks<<" known-owner inventory checks passed; universal b2 PARTIAL; finalizable=false\n";return 0;}catch(const std::exception& error){std::cerr<<"known-owner inventory: "<<error.what()<<'\n';return 1;}}
