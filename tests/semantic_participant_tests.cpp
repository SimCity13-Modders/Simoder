/** @file @summary Genuine script transport and separately admitted actual native closed-lifetime qualifications. */
#include "scripting/semantic_bridge.hpp"
#include "scripting/base_host.hpp"
#include "scripting/luau_compiler.hpp"
#include "scripting/source_project.hpp"
#include "mods/native/native_provider.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#if defined(SIMODER_NATIVE_TEST_HOST)
#include "platform/runtime_owner.hpp"
#include "core/sha256.hpp"
#include <Windows.h>
#endif

namespace {
using namespace sc13;
using namespace scripting;
using runtime::ScopeResult;
using Key=SemanticParticipantKey;
int checks{};
static_assert(std::is_trivially_copyable_v<Key>);
static_assert(!std::is_constructible_v<Key,std::uint64_t>);
static_assert(!std::is_constructible_v<SemanticParticipant,std::uint64_t>);
static_assert(!std::is_constructible_v<SemanticParticipant,SdHandle>);
static_assert(!std::is_constructible_v<mods::native::ProviderParticipant,std::shared_ptr<void>>);
/** @summary Require observed original implementation behavior. @param value Condition. @param message Fixed diagnostic. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Pin the actual authored Base checkout in the isolated trusted fixture.
 * @param path Exact Base directory. @return Complete source identity. */
BasePackagePin Pin(const std::filesystem::path& path){
    BasePackagePin pin{"0.1.0-alpha",{}};
    pin.files.emplace("mod.toml",HashText(ReadBounded(path/"mod.toml",4096)));
    for(const auto& file:std::filesystem::recursive_directory_iterator(path/"scripts"))if(file.is_regular_file())
        pin.files.emplace(file.path().lexically_relative(path).generic_string(),HashText(ReadBounded(file.path(),1024U*1024U)));
    return pin;
}
/** @summary Compile a real VM definition, never an endpoint generation factory. @return Original executable source. */
ScriptDefinition Definition(){
    ScriptDefinition result;result.entry="scripts/init.luau";result.language="luau";result.budgetMs=20;
    result.memoryBytes=2U*1024U*1024U;result.bytecode=CompileLuauSource("return {}");return result;
}
/** @summary Copy the immutable input using the original reserved result storage.
 * @param input Original envelope. @param output Reserved buffer. @param size Produced bytes. @return Exact copy result. */
bool Echo(const runtime::copied::Envelope& input,std::span<char> output,std::size_t& size){
    const auto wire=input.Encode();if(wire.size()>output.size())return false;
    std::memcpy(output.data(),wire.data(),wire.size());size=wire.size();return true;
}
/** @summary Configure only fixture facts and the existing sole interpreter/task route.
 * @param bridge Original transport. */
void Configure(SemanticBridge& bridge){
    SemanticBridgeTestAccess::Configure(bridge,{{"fixture.echo","test.echo@1","script","runtime","fixture","echo",1024,Echo}});
    SemanticBridgeTestAccess::ObserveContext(bridge,SemanticContextKind::Runtime,1,1);
}
/** @summary Account actual fixed representations rather than inventing a separate participant allowance. */
void Sizes(){
    std::cout<<"sizeof key="<<sizeof(Key)<<" participant="<<sizeof(SemanticParticipant)<<" endpoint="<<sizeof(SemanticEndpoint)
        <<" binding="<<sizeof(SemanticEndpointBinding)<<" invocation="<<sizeof(SemanticInvocation)
        <<" context="<<sizeof(SemanticContextLease)<<" bridge="<<sizeof(SemanticBridge)
        <<" queue-capacity=128 endpoint-capacity=128 pool-global="<<runtime::copied::Storage::GlobalLimit
        <<" pool-owner="<<runtime::copied::Storage::OwnerLimit<<'\n';
}
/** @summary Prove exact table/pool rejection rolls unpublished ownership back under unchanged ceilings. */
void Capacities(){
    auto bridge=std::make_shared<SemanticBridge>();
    auto scope=std::make_shared<runtime::RuntimeScope>();const auto owner=scope->Current().generation;
    std::vector<std::shared_ptr<SemanticEndpoint>> endpoints;endpoints.reserve(128);
    for(unsigned i=0;i!=128;++i)endpoints.push_back(bridge->Bind(scope));
    const auto full=bridge->Values()->Used(owner);bool rejected{};
    try{static_cast<void>(bridge->Bind(scope));}catch(const std::runtime_error&){rejected=true;}
    Check(rejected&&bridge->Values()->Used(owner)==full,"129th endpoint rejects and rolls back original pool charge");
    endpoints.clear();
    for(unsigned i=0;i!=128;++i)endpoints.push_back(bridge->Bind(scope));
    Check(bridge->Values()->Used(owner)==full,"expired weak slots recycle without extra participant registry or quota");
    endpoints.clear();
    const auto before=bridge->Values()->Used(owner);
    runtime::copied::Reservation exhausted;
    Check(bridge->Values()->Account(owner,runtime::copied::Storage::OwnerLimit-before,exhausted)==runtime::copied::Status::Ok,
        "exact unchanged original owner ceiling can be reserved");
    rejected=false;try{static_cast<void>(bridge->Bind(scope));}catch(const std::runtime_error&){rejected=true;}
    Check(rejected&&bridge->Values()->Used(owner)==runtime::copied::Storage::OwnerLimit,"pool exhausted bind exposes no endpoint and leaks no charge");
    exhausted={};Check(bridge->Values()->Used(owner)==before,"failed unpublished bind returns original accounting");
}
/** @summary Test only genuine script keys and destructor transfers on the original queue.
 * @param package Actual Base checkout. */
void Scripts(const std::filesystem::path& package){
    auto bridge=std::make_shared<SemanticBridge>();Configure(*bridge);
    auto base=std::make_shared<BaseHost>(package,Pin(package),std::function<void(ScriptPhase,std::uint64_t)>{},bridge);
    Check(base->Ready(),"actual Base VM ready");
    auto pending=base->Bind(std::make_shared<runtime::RuntimeScope>());
    Check(pending.semantic->Participant().Key().Type()==Key::Kind::Unbound && !pending.semantic->ScriptGeneration(),"pending endpoint has no VM alias");
    SemanticEndpointBinding unavailable;
    Check(SemanticBridgeTestAccess::CaptureBinding(*bridge,pending.semantic,unavailable)==ScopeResult::Stale && !unavailable.endpoint,
        "unbound capture denies before genuine VM initialization");
    const auto beforeFailed=bridge->Values()->Used(pending.semantic->storageOwner);
    auto bad=Definition();bad.bytecode.clear();bool failed{};
    try{ScriptRuntime invalid("fixture.invalid",bad,pending);}catch(const std::runtime_error&){failed=true;}
    Check(failed&&!pending.semantic->ScriptGeneration()&&bridge->Values()->Used(pending.semantic->storageOwner)==beforeFailed,
        "failed VM allocation cannot publish a false participant key");
    bool oldScopeDestroyed{},oldScopeDestroyedOffB{};
    auto* raw=runtime::RuntimeScopeTestAccess::Create().release();
    auto firstScope=std::shared_ptr<runtime::RuntimeScope>(raw,[&](auto* scope){
        auto observation=SemanticBridgeTestAccess::ReserveObservation(*bridge);
        oldScopeDestroyedOffB=observation.owns_lock();oldScopeDestroyed=true;delete scope;
    });
    Check(firstScope->CommitActivation(),"original first scope activates");firstScope->SetReadiness(true);
    auto firstHost=base->Bind(firstScope);auto firstEndpoint=firstHost.semantic;
    SemanticBridgeTestAccess::Issue(*firstEndpoint,runtime::RuntimeScopeTestAccess::Issue(*firstScope));
    auto first=std::make_shared<ScriptRuntime>("fixture.same",Definition(),std::move(firstHost));first->Commit();
    const auto firstKey=firstEndpoint->Participant().Key();
    Check(firstKey.Type()==Key::Kind::ActualScriptVm && firstKey.ScriptGeneration()==first->Status().generation,
        "script key is the actual reserved VM generation");
    auto secondScope=std::shared_ptr<runtime::RuntimeScope>(runtime::RuntimeScopeTestAccess::Create());
    Check(secondScope->CommitActivation(),"original second scope activates");secondScope->SetReadiness(true);
    auto secondHost=base->Bind(secondScope);auto secondEndpoint=secondHost.semantic;
    SemanticBridgeTestAccess::Issue(*secondEndpoint,runtime::RuntimeScopeTestAccess::Issue(*secondScope));
    auto second=std::make_shared<ScriptRuntime>("fixture.same",Definition(),std::move(secondHost));second->Commit();
    Check(firstKey!=secondEndpoint->Participant().Key()&&firstKey.ScriptGeneration()!=secondEndpoint->ScriptGeneration(),
        "same manifest text does not merge distinct genuine VM identities");
    ScriptHost repeated;repeated.semantic=firstEndpoint;failed=false;
    const auto beforeRepeat=bridge->Values()->Used(firstEndpoint->storageOwner);
    try{ScriptRuntime duplicate("fixture.same",Definition(),std::move(repeated));}catch(const std::runtime_error&){failed=true;}
    Check(failed&&firstEndpoint->Participant().Key()==firstKey&&!firstEndpoint->closed&&
        bridge->Values()->Used(firstEndpoint->storageOwner)==beforeRepeat,"second VM cannot overwrite initialized participant or leak accounting");
    runtime::copied::Envelope firstValue,secondValue;
    constexpr auto wire=R"({"v":1,"schema":"test.echo@1","value":["null"]})";
    Check(bridge->Values()->Decode(wire,"test.echo@1",firstEndpoint->storageOwner,firstValue)==runtime::copied::Status::Ok&&
        bridge->Values()->Decode(wire,"test.echo@1",secondEndpoint->storageOwner,secondValue)==runtime::copied::Status::Ok,"original owned values decoded");
    SemanticEndpointBinding originalBinding;
    Check(SemanticBridgeTestAccess::CaptureBinding(*bridge,firstEndpoint,originalBinding)==ScopeResult::Ok,"original genuine script binding");
    Check(SemanticBridgeTestAccess::CaptureBinding(*bridge,originalBinding.endpoint,originalBinding)==ScopeResult::Ok&&
        originalBinding.participant==firstKey,"capture retains source borrowed from previous caller output before replacing it off B");
    auto substituted=originalBinding;substituted.participant=secondEndpoint->Participant().Key();
    const auto beforeSubstitution=SemanticBridgeTestAccess::ObserveQueue(*bridge);
    runtime::TaskHandle rejectedTask;SemanticContextLease rejectedContext;
    Check(bridge->Request(firstEndpoint,"fixture.echo",firstValue,rejectedTask,rejectedContext,nullptr,&substituted)==ScopeResult::Stale&&
        !rejectedTask&&beforeSubstitution==SemanticBridgeTestAccess::ObserveQueue(*bridge),"another genuine script key cannot substitute under same original B");
    originalBinding={};substituted={};
    runtime::TaskHandle firstTask,secondTask;SemanticContextLease firstContext,secondContext;
    Check(bridge->Request(firstEndpoint,"fixture.echo",firstValue,firstTask,firstContext)==ScopeResult::Ok&&
        bridge->Request(secondEndpoint,"fixture.echo",secondValue,secondTask,secondContext)==ScopeResult::Ok,"both genuine script routes preserve original admission");
    Check(firstContext.participant==firstKey&&secondContext.participant==secondEndpoint->Participant().Key(),"contexts retain exact tagged identities");
    auto foreignContext=firstContext;foreignContext.participant=secondContext.participant;
    Check(bridge->Request(firstEndpoint,"fixture.echo",firstValue,rejectedTask,rejectedContext,&foreignContext)==ScopeResult::Stale&&!rejectedTask,
        "foreign genuine VM context key cannot rebind original facts");
    SemanticInvocation output;
    Check(bridge->Take(output)&&output.participant==firstKey,"original queue retains actual first participant");
    Check(bridge->Dispatch(output,"fixture.echo",firstValue)==ScopeResult::Ok,"original script dispatch commits through same evaluator");
    Check(firstEndpoint->tasks->Release(firstTask)==ScopeResult::Ok,"original completed task released");
    first->Stop();first.reset();firstEndpoint.reset();firstScope.reset();firstValue={};
    Check(!oldScopeDestroyed,"populated output retains its original endpoint before replacement");
    Check(bridge->Take(output)&&output.participant==secondContext.participant,"second original queue item replaces populated output");
    Check(oldScopeDestroyed&&oldScopeDestroyedOffB,"actual final original scope destructor runs after B unlock");
    Check(bridge->Dispatch(output,"fixture.echo",secondValue)==ScopeResult::Ok&&secondEndpoint->tasks->Release(secondTask)==ScopeResult::Ok,
        "replacement preserves second original task and copied value");
    second->Stop();
    Sizes();Capacities();
}
#if defined(SIMODER_NATIVE_TEST_HOST)
/** @summary Hash only the independently admitted synthetic image, never issue production trust.
 * @param path Frozen fixture copy. @return Isolated reviewed registration. */
mods::native::ReviewedModule Review(const std::filesystem::path& path){
    mods::native::ReviewedModule result;result.moduleId="fixture.typed-participant";result.absolutePath=path;result.nativeConsentApproved=true;
    core::Sha256Digest digest{};std::string error;Check(core::Sha256File(path,digest,error),"actual fixture hash");
    for(std::size_t i=0;i<digest.size();++i)result.sha256[i]=std::to_integer<unsigned char>(digest[i]);return result;
}
/** @summary Qualify a genuine retained native endpoint while execution remains closed without another caller VM.
 * @param package Real Base. @param parent Fresh explicit owned root. @param image Approved actual fixture DLL.
 * @param mode Independently reviewed stop scenario. */
void Native(const std::filesystem::path& package,const std::filesystem::path& parent,const std::filesystem::path& image,std::string_view mode){
    using namespace mods::native;
    Check(parent.is_absolute()&&std::filesystem::is_directory(parent),"explicit existing owned native root");
    const auto root=parent/("typed-"+std::string(mode)+"-"+std::to_string(GetCurrentProcessId()));
    Check(std::filesystem::create_directory(root),"fresh never overwritten native case");
    const auto stage=root/"stage";Check(std::filesystem::create_directory(stage),"plain original loader stage");
    const auto source=root/"fixture.dll";Check(std::filesystem::copy_file(image,source),"copy exact admitted native image");
    auto owner=platform::RuntimeOwnerTestAccess::Acquire(L"Global\\Simoder.TEST.U1.TypedParticipant."+std::to_wstring(GetCurrentProcessId()));
    Check(owner&&owner->IsAcquired(),"genuine isolated runtime owner");
    auto domain=std::make_shared<runtime::RuntimeCommitDomain>();auto lifecycle=std::make_shared<runtime::RuntimeLifecycle>();
    NativeModuleLoader loader(stage,owner,domain,lifecycle);std::shared_ptr<NativeModule> module;
    const auto registration=Review(source);Check(loader.Prepare(registration,module)==SD_OK&&module,"actual original native preparation");
    std::optional<NativeParticipant> participant,other;
    Check(module->CaptureParticipant(participant)==SD_OK&&module->CaptureParticipant(other)==SD_OK&&participant->SameIdentity(*other),"two retained views name same actual State");
    auto bridge=std::make_shared<SemanticBridge>();Configure(*bridge);
    auto base=std::make_shared<BaseHost>(package,Pin(package),std::function<void(ScriptPhase,std::uint64_t)>{},bridge);
    const auto baseGeneration=base->Status().generation;
    auto provider=std::make_unique<NativeProvider>(base,*participant);
    auto endpoint=NativeProviderTestAccess::Endpoint(*provider);
    const auto key=endpoint->Participant().Key();const auto identity=participant->ObserveLocalFence().identity;
    Check(key.Type()==Key::Kind::RetainedNativeModule&&!key.ScriptGeneration()&&!endpoint->ScriptGeneration()&&
        endpoint->Participant().Matches(*other)&&endpoint->storageOwner==identity.scopeGeneration,"native endpoint retains actual State and scope without VM alias");
    SemanticEndpointBinding binding;
    Check(SemanticBridgeTestAccess::CaptureBinding(*bridge,endpoint,binding)==ScopeResult::Ok&&binding.participant==key&&
        binding.scope==endpoint->scope&&std::holds_alternative<runtime::UnqualifiedOrigin>(binding.authority.origin)&&!binding.authority.reservation,
        "native binding is historical identity only, never an ordinary script grant");
    const auto counters=SemanticBridgeTestAccess::ObserveQueue(*bridge);const auto operations=endpoint->scope->Current().retainedOperations;
    runtime::copied::Envelope value;Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["null"]})","test.echo@1",endpoint->storageOwner,value)==runtime::copied::Status::Ok,"original native-owned copied value");
    const auto charge=bridge->Values()->Used(endpoint->storageOwner);
    for(unsigned i=0;i!=32;++i){
        runtime::TaskHandle task;SemanticContextLease context;context.operation=999;
        Check(bridge->Request(endpoint,"fixture.echo",value,task,context)==ScopeResult::NotReady&&!task&&!context.operation&&!context.mask,
            "native Request closes before operation serial or reservation");
        SdHandle output{99,99};Check(provider->Bind(identity.owner,{},41,{1,1},0,&output)==SD_NOT_READY&&!output.slot&&!output.generation,
            "native C adapter cannot select ordinary role");
        output={99,99};Check(provider->Invoke(identity.owner,{1,1},41,{},&output)==SD_NOT_READY&&!output.slot&&!output.generation,"native Invoke remains unqualified before task storage");
    }
    Check(counters==SemanticBridgeTestAccess::ObserveQueue(*bridge)&&operations==endpoint->scope->Current().retainedOperations&&
        charge==bridge->Values()->Used(endpoint->storageOwner)&&base->Status().generation==baseGeneration,
        "closed native attempts preserve original counters, pool, tasks and sole Base generation");
    bool rejected{};try{NativeProvider wrong({identity.owner.slot+1,identity.owner.generation},base,endpoint,*participant,lifecycle,{},{});}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"wrong native owner cannot substitute into actual endpoint");
    rejected=false;try{NativeProvider agent(identity.owner,base,endpoint,*participant,lifecycle,{1,1},{});}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"unverified session cannot upgrade native lifetime");
    rejected=false;try{NativeProvider arbitrary(identity.owner,base,endpoint,*participant,std::make_shared<runtime::RuntimeLifecycle>(),{},{});}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"substitute lifecycle cannot replace original State publisher");
    SemanticContextLease closedContext;closedContext.participant=key;closedContext.baseEpoch=bridge->Epoch();closedContext.mask=1;
    Check(bridge->ContextFence(*endpoint,closedContext)==ScopeResult::NotReady,"native atomic fence never qualifies operations");
    if(mode=="native-domain")domain->RequestStop();
    else if(mode=="native-lifecycle")lifecycle->Publish({0,false,false,false,true});
    else if(mode=="native-scope")endpoint->scope->RequestStop();
    else Check(mode=="native","known actual native scenario");
    if(mode!="native"){
        const bool local=participant->ObserveLocalFence().LocallyLive();
        Check(local==(mode!="native-domain"),"native local word honestly excludes external scope/lifecycle stop");
        Check(SemanticBridgeTestAccess::CaptureBinding(*bridge,endpoint,binding)==ScopeResult::Stale&&!binding.endpoint,
            "off-guard original scope/lifecycle stop denies new historical binding");
    }
    if(mode=="native-scope"){
        bridge->Close();
        Check(participant->ObserveLocalFence().LocallyLive()&&bridge->ContextFence(*endpoint,closedContext)==ScopeResult::Stale,
            "closed actual Base bridge denies a locally live native identity without scope mutex in fence");
    }
    provider.reset();Check(endpoint->scope->Current().stopping,"provider cleanup stops original scope without pretending native quiescence");
    Check(!participant->ObserveLocalFence().retired,"scope cleanup is not original local native retirement");
    Check(module->Quiesce()==SD_OK&&module->Drain()==SD_OK,"only original native quiesce and drain complete retirement");
    Check(!endpoint->Participant().NativeLocallyLive()&&bridge->ContextFence(*endpoint,closedContext)==ScopeResult::Stale,"original retirement is sticky in retained endpoint");
    if(mode=="native"){
        std::shared_ptr<NativeModule> successor;Check(loader.Prepare(registration,successor)==SD_OK&&successor,"same image admits separate actual State");
        std::optional<NativeParticipant> next;Check(successor->CaptureParticipant(next)==SD_OK&&!next->SameIdentity(*participant),"same image successor has distinct retained identity");
        NativeProvider nextProvider(base,*next);const auto nextEndpoint=NativeProviderTestAccess::Endpoint(nextProvider);
        Check(key!=nextEndpoint->Participant().Key()&&!nextEndpoint->Participant().Matches(*participant),"native incarnation and scope cannot renew old key");
        Check(successor->Quiesce()==SD_OK&&successor->Drain()==SD_OK,"same original successor cleanup");
    }
    Sizes();
}
#endif
}
/** @summary Run only the explicitly selected qualification; native modes require separate actual artifact/run approval.
 * @param argc Exact mode and frozen local inputs. @param argv Script/Base or native/Base/root/image.
 * @return Zero only for actual observed success; no retries or fallback participant models. */
int main(int argc,char** argv){
    try{
        Check(argc>=3,"mode and actual Base package required");const std::string mode=argv[1];
        if(mode=="script"){Check(argc==3,"script mode exact arguments");Scripts(argv[2]);}
#if defined(SIMODER_NATIVE_TEST_HOST)
        else{Check(argc==5,"native mode exact arguments");Native(argv[2],argv[3],argv[4],mode);}
#else
        else throw std::runtime_error("native execution is not compiled into ordinary script qualification");
#endif
        std::cout<<"PASS "<<mode<<" checks="<<checks<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
