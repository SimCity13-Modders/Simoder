#include "mods/native/native_provider.hpp"
#include "scripting/luau_compiler.hpp"
#include "scripting/source_project.hpp"
#include "mods/owned_publication.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace sc13;
using mods::native::NativeProvider;
using runtime::ScopeResult;
int checks{};
constexpr std::string_view Wire=R"({"v":1,"schema":"test.echo@1","value":["null"]})";
constexpr std::string_view RouteWire=R"({"v":1,"schema":"test.echo@1","value":["ref","runtime","1","1"]})";
constexpr std::string_view AllFacts=R"({"v":1,"schema":"test.echo@1","value":["list",[["ref","runtime","1","1"],["ref","region","2","1"],["ref","city","3","1"],["ref","entity","4","1"]]]})";
/** @summary Require actual original-source behavior. @param value Observed condition. @param message Fixed diagnostic. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Compare opaque identities without reinterpretation. @param a First identity. @param b Second identity. @return Equality. */
bool Same(SdHandle a,SdHandle b){return a.slot==b.slot&&a.generation==b.generation;}
/** @summary Verify denied metadata preserves only negotiation bytes. @param info Actual output. @return Sanitized body. */
bool Empty(const SdTaskInfo& info){return !info.task.slot&&!info.task.generation&&!info.context.slot&&!info.context.generation&&
    !info.phase&&!info.error&&!info.flags&&!info.revision&&!info.sequence;}
/** @summary Capture the actual separately checked-out authored Base package. @param path Package. @return Full file pin. */
scripting::BasePackagePin Pin(const std::filesystem::path& path){
    scripting::BasePackagePin pin{"0.1.0-alpha",{}};
    pin.files.emplace("mod.toml",scripting::HashText(scripting::ReadBounded(path/"mod.toml",4096)));
    for(const auto& file:std::filesystem::recursive_directory_iterator(path/"scripts"))if(file.is_regular_file())
        pin.files.emplace(file.path().lexically_relative(path).generic_string(),scripting::HashText(scripting::ReadBounded(file.path(),1024U*1024U)));
    return pin;
}
/** @summary Echo only original immutable envelope bytes. @param input Original value. @param output Reserved storage.
 * @param size Written bytes. @return Verified copy. */
bool Echo(const runtime::copied::Envelope& input,std::span<char> output,std::size_t& size){
    const auto wire=input.Encode();if(wire.size()>output.size())return false;
    std::memcpy(output.data(),wire.data(),wire.size());size=wire.size();return true;
}
/** @summary Retain actual original Base/Scope/Bridge/VM and fixture-only authority through every native observation. */
struct Fixture final {
    std::shared_ptr<scripting::SemanticBridge> bridge=std::make_shared<scripting::SemanticBridge>();
    std::shared_ptr<scripting::BaseHost> base;
    std::shared_ptr<runtime::RuntimeScope> scope{runtime::RuntimeScopeTestAccess::Create()};
    std::shared_ptr<scripting::SemanticEndpoint> endpoint;
    std::shared_ptr<scripting::ScriptRuntime> vm;
    std::shared_ptr<mods::PublicationOrigin> origin;
    std::shared_ptr<runtime::RuntimeLifecycle> lifecycle=std::make_shared<runtime::RuntimeLifecycle>();
    std::unique_ptr<NativeProvider> provider;
    runtime::OperationContext authority;
    scripting::SemanticContextLease route;
    SdHandle owner{91,1},session{92,1},invocation{};
    int effects{};
    /** @summary Create actual closed-provider composition, never a native VM or production authority.
     * @param package Real Base checkout. @param registry Select the existing prepared registry route.
     * @param construct Whether to construct the native wrapper immediately. @param lease Existing grant expiry budget. */
    Fixture(const std::filesystem::path& package,bool registry=false,bool construct=true,std::chrono::milliseconds lease=std::chrono::minutes(1)){
        scripting::SemanticMethod method{"fixture.echo","test.echo@1","script","runtime","fixture","echo",1024,
            [this](const auto& input,auto output,auto& size){++effects;return Echo(input,output,size);}};
        method.registryPublication=registry;scripting::SemanticBridgeTestAccess::Configure(*bridge,{method});
        for(unsigned i=0;i!=4;++i)scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,static_cast<scripting::SemanticContextKind>(i),i+1,1);
        base=std::make_shared<scripting::BaseHost>(package,Pin(package),std::function<void(scripting::ScriptPhase,std::uint64_t)>{},bridge);
        Check(scope->CommitActivation(),"actual scope activation");scope->SetReadiness(true);
        auto host=base->Bind(scope);endpoint=host.semantic;
        authority=runtime::RuntimeScopeTestAccess::Issue(*scope,false,true,true,1,lease);scripting::SemanticBridgeTestAccess::Issue(*endpoint,authority);
        scripting::ScriptDefinition definition;definition.entry="scripts/init.luau";definition.budgetMs=20;
        definition.bytecode=scripting::CompileLuauSource("return {}");
        vm=std::make_shared<scripting::ScriptRuntime>("fixture.native-poll",definition,std::move(host));
        origin=mods::OwnedPublicationTestAccess::Issue(vm,endpoint,"native-poll-original");vm->Commit();
        Check(origin&&mods::OwnedPublicationTestAccess::Transition(*origin,mods::PublicationOrigin::Phase::Active),"actual owner active");
        runtime::copied::Envelope input;
        Check(bridge->Values()->Decode(RouteWire,"test.echo@1",endpoint->storageOwner,input)==runtime::copied::Status::Ok,"actual route envelope");
        Check(bridge->BindReferences(*endpoint,input,route)==ScopeResult::Ok,"actual route facts");
        if(construct)Open();
    }
    /** @summary Construct with original captures and retain actual invocation. */
    void Open(){provider=std::make_unique<NativeProvider>(owner,base,endpoint,vm,lifecycle,session,
        std::vector<mods::native::ProviderRoute>{{41,"fixture.echo",{1,1},route,authority.opcode}});
        Check(provider->Bind(owner,session,41,{1,1},authority.revision,&invocation)==SD_OK,"actual invocation bound");}
    /** @summary Retire in original ownership order, including exception paths. */
    ~Fixture(){if(provider)provider->Close();if(vm)vm->Stop();origin.reset();provider.reset();}
    /** @summary Enqueue through actual native adapter. @param wire Immutable authored input. @return Native task. */
    SdHandle Invoke(std::string_view wire=Wire){SdHandle task{};
        Check(provider->Invoke(owner,invocation,41,{reinterpret_cast<const std::uint8_t*>(wire.data()),static_cast<std::uint32_t>(wire.size())},&task)==SD_OK,"original native request");return task;}
    /** @summary Take exactly the admitted original queue item without executing a replacement model. @return Original retained invocation. */
    scripting::SemanticInvocation Take(){scripting::SemanticInvocation request;Check(bridge->Take(request),"original queue item");return request;}
    /** @summary Observe empty original task state. @param task Native identity. @return Actual coherent publication. */
    SdTaskInfo Observe(SdHandle task){SdTaskInfo info{{sizeof(SdTaskInfo),1}};std::uint32_t required=99;
        Check(provider->Poll(owner,invocation,task,&info,nullptr,0,&required)==SD_OK&&required==0,"empty original observation");return info;}
    /** @summary Require complete denied output sanitation. @param task Original identity. @param expected Exact denial. */
    void Denied(SdHandle task,SdResult expected){SdTaskInfo info;std::memset(&info,0xAB,sizeof(info));info.header={sizeof(info),1};
        std::array<std::uint8_t,256> bytes;bytes.fill(0xAB);std::uint32_t required=99;
        Check(provider->Poll(owner,invocation,task,&info,bytes.data(),static_cast<std::uint32_t>(bytes.size()),&required)==expected&&Empty(info)&&!required&&
            std::all_of(bytes.begin(),bytes.end(),[](auto value){return value==0;}),"denial clears whole metadata buffer and length");}
    /** @summary Release actual terminal task without discarding retained invocation. @param task Exact original identity. */
    void Release(SdHandle task){Check(provider->Release(owner,task)==SD_OK,"original task release");}
};
/** @summary Test real enqueue/Base pump, metadata sizing/copy, immutable result and typed per-task identity. @param package Actual Base. */
void Delivery(const std::filesystem::path& package){
    Fixture f(package);auto task=f.Invoke();const auto queued=f.Observe(task);
    Check(queued.phase==SD_TASK_QUEUED&&queued.error==SD_BUSY&&!queued.flags&&queued.sequence==1&&queued.revision==f.authority.revision&&
        Same(queued.task,task)&&queued.context.slot&&queued.context.generation&&!Same(queued.context,task)&&!Same(queued.context,f.invocation),"queued factual tuple and distinct kinds");
    Check(f.provider->ReleaseInvocation(f.owner,f.invocation)==SD_BUSY,"original task retains invocation");
    auto second=f.Invoke();const auto other=f.Observe(second);Check(!Same(queued.context,other.context),"same facts different operations distinct execution observations");
    SdHandle rejected{};Check(f.provider->Bind(f.owner,f.session,41,queued.context,f.authority.revision,&rejected)==SD_STALE&&!rejected.slot,"own informational observation never target");
    SdTaskInfo confused{{sizeof(SdTaskInfo),1}};std::uint32_t required=99;
    Check(f.provider->Poll(f.owner,queued.context,task,&confused,nullptr,0,&required)==SD_STALE&&Empty(confused)&&!required,"context cannot be invocation");
    Check(f.provider->Poll(f.owner,f.invocation,queued.context,&confused,nullptr,0,&required)==SD_STALE&&Empty(confused)&&!required,"context cannot be task");
    f.base->Pump();f.base->Pump();Check(f.effects==1,"actual Base pump commits once and preserves the other original revision fence");
    SdTaskInfo info{{sizeof(SdTaskInfo),1}};std::array<std::uint8_t,8> small;small.fill(0xAA);
    Check(f.provider->Poll(f.owner,f.invocation,task,&info,small.data(),static_cast<std::uint32_t>(small.size()),&required)==SD_BUFFER_SMALL&&
        required==Wire.size()&&Empty(info)&&std::all_of(small.begin(),small.end(),[](auto byte){return byte==0;}),"buffer-small length only without partial disclosure");
    std::vector<std::uint8_t> bytes(required+8,0xAA);
    Check(f.provider->Poll(f.owner,f.invocation,task,&info,bytes.data(),static_cast<std::uint32_t>(bytes.size()),&required)==SD_OK&&
        required==Wire.size()&&info.phase==SD_TASK_SUCCEEDED&&info.error==SD_OK&&info.flags==SD_TASK_COMMITTED&&info.sequence==4&&
        Same(info.context,queued.context)&&std::string_view(reinterpret_cast<const char*>(bytes.data()),required)==Wire&&
        std::all_of(bytes.begin()+required,bytes.end(),[](auto byte){return byte==0;}),"whole terminal original payload and zero tail");
    const auto revision=info.revision;
    {auto writer=f.scope->ReserveWriter();Check(writer.Acquired()&&writer.Advance()>revision,"unrelated actual domain writer advances after terminal");}
    for(int i=0;i!=8;++i){SdTaskInfo current{{sizeof(SdTaskInfo),1}};
        Check(f.provider->Poll(f.owner,f.invocation,task,&current,bytes.data(),static_cast<std::uint32_t>(bytes.size()),&required)==SD_OK&&
            current.sequence==info.sequence&&current.revision==revision&&Same(current.context,queued.context)&&current.error==SD_OK,"terminal stable after other actual writer");}
    Check(f.provider->Cancel(f.owner,f.invocation,task)==SD_BUSY&&f.effects==1,"terminal poll never reruns or undoes");
    f.Release(task);f.Denied(task,SD_STALE);f.Release(second);
    Check(f.provider->ReleaseInvocation(f.owner,f.invocation)==SD_OK,"invocation releases after original tasks");
}
/** @summary Reuse only a legitimately released uncommitted slot without refreshing or adopting different authority.
 * @param package Actual Base. */
void KindReuse(const std::filesystem::path& package){
    Fixture f(package);const auto task=f.Invoke(),observation=f.Observe(task).context;
    Check(f.provider->Cancel(f.owner,f.invocation,task)==SD_OK,"original uncommitted cancel before reuse");f.base->Pump();f.Release(task);
    const auto reused=f.Invoke();const auto fresh=f.Observe(reused);
    Check(reused.slot==task.slot&&reused.generation!=task.generation&&!Same(fresh.context,observation),"slot reuse changes task and context generations");
    f.Denied(task,SD_STALE);Check(f.provider->Cancel(f.owner,f.invocation,reused)==SD_OK,"fresh queued cancellation");
    f.base->Pump();f.Release(reused);
}
/** @summary Bind distinct observations to the complete original Request-produced execution leases, not static route facts.
 * @param package Actual Base. */
void ExecutionLeases(const std::filesystem::path& package){
    Fixture f(package);const auto first=f.Invoke(AllFacts),second=f.Invoke(AllFacts);
    const auto firstInfo=f.Observe(first),secondInfo=f.Observe(second);auto a=f.Take(),b=f.Take();
    Check(a.context.operation&&b.context.operation&&a.context.operation!=b.context.operation&&a.context.mask==15&&b.context.mask==15&&
        a.context.facts==b.context.facts&&a.context.participant==b.context.participant&&a.context.baseEpoch==b.context.baseEpoch&&
        a.context.scopeGeneration==b.context.scopeGeneration&&a.context.runtimeEpoch==b.context.runtimeEpoch&&!Same(firstInfo.context,secondInfo.context),
        "complete equal facts with distinct original operations receive distinct observation identities");
    Check(f.bridge->Dispatch(a,"fixture.echo",a.value)==ScopeResult::Ok&&f.bridge->Dispatch(b,"fixture.echo",b.value)==ScopeResult::Conflict&&
        f.effects==1,"distinct original leases retain the actual revision conflict fence");f.Release(first);f.Release(second);
}
/** @summary Test coherent preparing/committed/terminal snapshots with original participants retained across a real callback barrier.
 * @param package Actual Base. */
void InFlight(const std::filesystem::path& package){
    Fixture f(package);const auto task=f.Invoke();const auto queued=f.Take();
    Check(f.endpoint->tasks->Begin(queued.task)==ScopeResult::Ok,"actual task preparing");const auto preparing=f.Observe(task);
    Check(preparing.phase==SD_TASK_RUNNING&&!preparing.flags&&preparing.sequence==2&&preparing.revision==f.authority.revision,"preparing original tuple");
    std::promise<void> entered,release;auto ready=entered.get_future();auto leave=release.get_future().share();
    auto execution=std::async(std::launch::async,[&]{return f.endpoint->tasks->Commit(queued.task,[&](const auto& input,auto output,auto& size){
        entered.set_value();leave.wait();return Echo(input,output,size);});});
    if(ready.wait_for(std::chrono::seconds(5))!=std::future_status::ready){release.set_value();static_cast<void>(execution.get());Check(false,"actual callback barrier reached");}
    bool valid=true;for(int i=0;i!=32;++i){SdTaskInfo current{{sizeof(SdTaskInfo),1}};std::uint32_t required=99;
        const auto observed=f.provider->Poll(f.owner,f.invocation,task,&current,nullptr,0,&required);
        valid=valid&&observed==SD_OK&&!required&&current.phase==SD_TASK_RUNNING&&current.flags==SD_TASK_COMMITTED&&
            current.error==SD_BUSY&&current.sequence==3&&current.revision==preparing.revision+1&&Same(current.context,preparing.context);}
    const auto retained=f.provider->Release(f.owner,task);release.set_value();const auto result=execution.get();
    Check(valid&&retained==SD_BUSY&&result==ScopeResult::Ok,"whole in-flight tuple and retained original participants");f.Release(task);
}
/** @summary Test factual precommit and uncertain terminal outcomes independently of successful API observation. @param package Actual Base. */
void Outcomes(const std::filesystem::path& package){
    for(int mode=0;mode!=4;++mode){Fixture f(package);auto task=f.Invoke();auto queued=f.Take();
        if(mode==0)Check(f.provider->Cancel(f.owner,f.invocation,task)==SD_OK,"actual queued cancel");
        else {Check(f.endpoint->tasks->Begin(queued.task)==ScopeResult::Ok,"actual preparing for failure");
            if(mode==1)Check(f.endpoint->tasks->FinishUncommitted(queued.task,ScopeResult::Conflict)==ScopeResult::Ok,"actual uncommitted failure");
            else Check(f.endpoint->tasks->Commit(queued.task,[mode](const auto&,auto,auto&)->bool{if(mode==3)throw std::runtime_error("fixture callback");return false;})==ScopeResult::Indeterminate,"actual uncertain marker");}
        const auto info=f.Observe(task);
        Check(info.phase==(mode==0?SD_TASK_CANCELLED:mode==1?SD_TASK_FAILED:SD_TASK_INDETERMINATE)&&
            info.error==static_cast<std::uint32_t>(mode==0?SD_CANCELLED:mode==1?SD_CONFLICT:SD_INDETERMINATE)&&info.flags==(mode<2?0:SD_TASK_COMMITTED)&&
            info.sequence==static_cast<std::uint64_t>(mode==0?2:mode==1?3:4),"API observation success differs from factual failure");f.Release(task);}
}
/** @summary Test all four input-added facts, authorization loss and original full binding replacement before delivery. @param package Actual Base. */
void Fences(const std::filesystem::path& package){
    for(unsigned kind=0;kind!=4;++kind){Fixture f(package);auto task=f.Invoke(AllFacts);auto queued=f.Take();
        Check(queued.context.mask==15&&queued.context.operation&&queued.context.participant.ScriptGeneration()==f.endpoint->ScriptGeneration()&&
            queued.context.baseEpoch==f.bridge->Epoch()&&queued.context.scopeGeneration==f.scope->Current().generation&&
            queued.context.runtimeEpoch==f.scope->Current().epoch,"full original request lease retains all identities");
        for(unsigned i=0;i!=4;++i)Check(queued.context.facts[i].slot==i+1&&queued.context.facts[i].generation==1&&queued.context.facts[i].epoch,"all original fact members retained");
        scripting::SemanticBridgeTestAccess::ObserveContext(*f.bridge,static_cast<scripting::SemanticContextKind>(kind),kind+1,1);
        f.Denied(task,SD_STALE);f.Release(task);}
    {Fixture f(package);auto task=f.Invoke();f.base->Pump();
        Check(runtime::RuntimeScopeTestAccess::Revoke(*f.scope,f.authority.grantGeneration)==ScopeResult::Ok,"actual original grant revocation");
        f.Denied(task,SD_DENIED);f.Release(task);}
    {Fixture f(package);auto task=f.Invoke();mods::LifecycleDecision replacement;
        Check(scripting::SemanticBridgeTestAccess::RefreshReservation(*f.bridge,f.origin,replacement)==ScopeResult::Ok,"actual original reservation refresh");
        f.Denied(task,SD_STALE);f.Release(task);}
    {Fixture f(package);auto task=f.Invoke();f.scope->RequestStop();f.Denied(task,SD_STALE);f.Release(task);}
    {Fixture f(package,false,true,std::chrono::seconds(1));auto task=f.Invoke();f.base->Pump();
        std::this_thread::sleep_until(f.authority.deadline);f.Denied(task,SD_DENIED);f.Release(task);}
    {Fixture f(package);auto task=f.Invoke();const SdHandle otherOwner{94,1};
        NativeProvider other(otherOwner,f.base,f.endpoint,f.vm,f.lifecycle,f.session,{{41,"fixture.echo",{1,1},f.route,f.authority.opcode}});
        SdHandle invocation{},another{};
        Check(other.Bind(otherOwner,f.session,41,{1,1},f.authority.revision,&invocation)==SD_OK&&
            other.Invoke(otherOwner,invocation,41,{reinterpret_cast<const std::uint8_t*>(Wire.data()),static_cast<std::uint32_t>(Wire.size())},&another)==SD_OK,
            "distinct provider retains same actual singleton Base participant");
        const auto first=f.Observe(task);SdTaskInfo second{{sizeof(SdTaskInfo),1}};std::uint32_t secondRequired=99;
        Check(other.Poll(otherOwner,invocation,another,&second,nullptr,0,&secondRequired)==SD_OK&&!secondRequired,"second provider original queued observation");
        Check(!Same(first.context,second.context)&&!Same(task,another),"provider blocks never alias original observations");
        f.Denied(another,SD_STALE);SdTaskInfo info{{sizeof(SdTaskInfo),1}};std::uint32_t required=99;
        Check(f.provider->Poll({92,1},f.invocation,task,&info,nullptr,0,&required)==SD_DENIED&&Empty(info)&&!required,"cross-owner metadata denial");
        Check(f.provider->Cancel(f.owner,f.invocation,task)==SD_OK&&other.Cancel(otherOwner,invocation,another)==SD_OK,"provider task cleanup cancel");
        f.base->Pump();f.base->Pump();f.Release(task);Check(other.Release(otherOwner,another)==SD_OK&&
            other.ReleaseInvocation(otherOwner,invocation)==SD_OK,"second provider original task and invocation cleanup");}
}
/** @summary Remove the fixture-thread observation cut before any original retained participant can be destroyed. */
struct ClearPollCut final {
    /** @summary No callback may outlive its referenced original fixture owners. */
    ~ClearPollCut(){mods::native::NativeProviderTestAccess::AfterPoll({});}
};
/** @summary Mutate actual facts/authority/full binding after original snapshot capture and require no stale disclosure.
 * @param package Actual Base. */
void DeliveryFences(const std::filesystem::path& package){
    for(int mode=0;mode!=4;++mode){Fixture f(package);const auto task=f.Invoke();f.base->Pump();
        mods::LifecycleDecision replacement;bool reached=false;ScopeResult mutation=ScopeResult::Ok;
        ClearPollCut cleanup;
        mods::native::NativeProviderTestAccess::AfterPoll([&]{reached=true;
            if(mode==0)scripting::SemanticBridgeTestAccess::ObserveContext(*f.bridge,scripting::SemanticContextKind::Runtime,1,1);
            if(mode==1)mutation=runtime::RuntimeScopeTestAccess::Revoke(*f.scope,f.authority.grantGeneration);
            if(mode==2)mutation=scripting::SemanticBridgeTestAccess::RefreshReservation(*f.bridge,f.origin,replacement);
            if(mode==3)throw std::runtime_error("controlled delivery cut failure");
        });
        f.Denied(task,mode==0||mode==2?SD_STALE:mode==1?SD_DENIED:SD_OPERATION_FAILED);
        Check(reached&&mutation==ScopeResult::Ok,"actual mutation after coherent original task capture");
        mods::native::NativeProviderTestAccess::AfterPoll({});f.Release(task);
    }
}
/** @summary Exercise the actual prepared registry writer, with no simulated native outcome. @param package Actual Base. */
void Prepared(const std::filesystem::path& package){
    Fixture f(package,true);auto task=f.Invoke();auto queued=f.Take();
    mods::PatchRegistry registry;mods::OwnedPublication plan;std::string error;
    Check(mods::OwnedPublicationTestAccess::Prepare(f.origin,registry,queued,{{{1,2,3},4,5.0F}},plan,error),"actual prepared plan");
    Check(f.bridge->DispatchPrepared(queued,registry,plan)==ScopeResult::Ok,"actual original prepared commit");
    SdTaskInfo info{{sizeof(SdTaskInfo),1}};std::array<std::uint8_t,512> bytes{};std::uint32_t required{};
    Check(f.provider->Poll(f.owner,f.invocation,task,&info,bytes.data(),static_cast<std::uint32_t>(bytes.size()),&required)==SD_OK&&
        info.phase==SD_TASK_SUCCEEDED&&info.flags==SD_TASK_COMMITTED&&info.sequence==3&&info.revision==f.scope->Current().revision,"prepared actual writer revision and compound publication");
    f.Release(task);
}
/** @summary Fill only real invocation entries and retain their identities for exact cleanup.
 * @param fixture Original provider composition. @param count Additional entries. @return Actual retained identities. */
std::vector<SdHandle> RetainInvocations(Fixture& fixture,std::size_t count){
    std::vector<SdHandle> retained(count);
    for(auto& invocation:retained)Check(fixture.provider->Bind(fixture.owner,fixture.session,41,{1,1},fixture.authority.revision,&invocation)==SD_OK,
        "real retained invocation admission");return retained;
}
/** @summary Attempt a real extra binding without issuing replacement authority. @param f Original fixture.
 * @param output Cleared original output. @return Actual bounded admission. */
SdResult ExtraBind(Fixture& f,SdHandle& output){return f.provider->Bind(f.owner,f.session,41,{1,1},f.authority.revision,&output);}
/** @summary Test the canonical shared128 ceiling in both kinds, retained terminal state and original release refund.
 * @param package Actual Base. */
void SharedCapacity(const std::filesystem::path& package){
    Fixture f(package);auto retained=RetainInvocations(f,127);SdHandle rejected{9,9};
    const SdBytes input{reinterpret_cast<const std::uint8_t*>(Wire.data()),static_cast<std::uint32_t>(Wire.size())};
    Check(f.provider->Invoke(f.owner,f.invocation,41,input,&rejected)==SD_RESOURCE_LIMIT&&!rejected.slot&&
        f.scope->Current().retainedOperations==0&&f.effects==0,"128 invocations prevent the 129th mixed reference before task or effect");
    const auto old=retained.back();Check(f.provider->ReleaseInvocation(f.owner,old)==SD_OK,"one invocation frees one shared position");retained.pop_back();
    auto task=f.Invoke();Check(ExtraBind(f,rejected)==SD_RESOURCE_LIMIT&&!rejected.slot,"127 invocations plus task deny additional invocation");
    Check(f.provider->ReleaseInvocation(f.owner,f.invocation)==SD_BUSY,"shared capacity never bypasses original task retention");
    Check(f.provider->Cancel(f.owner,f.invocation,task)==SD_OK,"actual retained cancelled terminal");f.base->Pump();
    Check(ExtraBind(f,rejected)==SD_RESOURCE_LIMIT&&!rejected.slot&&f.effects==0,"cancelled terminal remains charged until actual release");
    f.Release(task);Check(ExtraBind(f,rejected)==SD_OK&&rejected.slot==old.slot&&rejected.generation!=old.generation,"actual release refunds one shared slot with new generation");
    Check(f.provider->ReleaseInvocation(f.owner,rejected)==SD_OK,"refunded shared invocation cleanup");
    // @summary Failed uncommitted original tasks remain retained just like cancellation or completion.
    task=f.Invoke();auto queued=f.Take();Check(f.endpoint->tasks->Begin(queued.task)==ScopeResult::Ok&&
        f.endpoint->tasks->FinishUncommitted(queued.task,ScopeResult::Conflict)==ScopeResult::Ok,"original failed terminal publication");
    Check(ExtraBind(f,rejected)==SD_RESOURCE_LIMIT&&!rejected.slot,"failed retained terminal still consumes shared capacity");
    f.Release(task);Check(ExtraBind(f,rejected)==SD_OK,"failed terminal release refunds capacity");
    Check(f.provider->ReleaseInvocation(f.owner,rejected)==SD_OK,"failed terminal refund cleanup");
    for(const auto invocation:retained)Check(f.provider->ReleaseInvocation(f.owner,invocation)==SD_OK,"shared capacity retained invocation cleanup");
}
/** @summary Remove pending-reservation scheduling cuts before any referenced original owner can leave scope. */
struct ClearReserveCut final {
    /** @summary Every success/failure path removes the fixture-thread callback. */
    ~ClearReserveCut(){mods::native::NativeProviderTestAccess::AfterTaskReserve({});}
};
/** @summary A pending native record consumes shared capacity before original Request publishes any task identity.
 * @param package Actual Base. */
void PendingCapacity(const std::filesystem::path& package){
    Fixture f(package);const auto retained=RetainInvocations(f,126);bool reached=false;SdResult binding=SD_OK;SdHandle denied{9,9};
    ClearReserveCut cleanup;
    mods::native::NativeProviderTestAccess::AfterTaskReserve([&]{reached=true;binding=ExtraBind(f,denied);});
    const auto task=f.Invoke();mods::native::NativeProviderTestAccess::AfterTaskReserve({});
    Check(reached&&binding==SD_RESOURCE_LIMIT&&!denied.slot&&f.effects==0,"pending reserved record counts before original Request without exposing a task");
    Check(f.provider->Cancel(f.owner,f.invocation,task)==SD_OK,"pending-origin task settles through real cancellation");f.base->Pump();f.Release(task);
    SdHandle refunded{};Check(ExtraBind(f,refunded)==SD_OK,"settled actual release refunds pending-origin position");
    Check(f.provider->ReleaseInvocation(f.owner,refunded)==SD_OK,"pending-origin refund cleanup");
    for(const auto invocation:retained)Check(f.provider->ReleaseInvocation(f.owner,invocation)==SD_OK,"pending-origin retained invocation cleanup");
}
/** @summary Force the actual Request admission to fail after native reservation and require exact shared refund.
 * @param package Actual Base. */
void RequestRefund(const std::filesystem::path& package){
    Fixture f(package);const auto retained=RetainInvocations(f,126);runtime::copied::Envelope input;
    Check(f.bridge->Values()->Decode(Wire,"test.echo@1",f.endpoint->storageOwner,input)==runtime::copied::Status::Ok,"refund actual copied value");
    std::vector<runtime::TaskHandle> original;ScopeResult limit=ScopeResult::Ok;
    for(std::size_t i=0;i!=512;++i){runtime::TaskHandle task;limit=f.endpoint->tasks->Admit(f.authority,input,1024,task);
        if(limit!=ScopeResult::Ok)break;original.push_back(task);}
    Check(!original.empty()&&limit==ScopeResult::Busy,"actual original task store is finitely saturated");SdHandle rejected{9,9};
    Check(f.provider->Invoke(f.owner,f.invocation,41,{reinterpret_cast<const std::uint8_t*>(Wire.data()),static_cast<std::uint32_t>(Wire.size())},&rejected)==SD_BUSY&&
        !rejected.slot&&f.effects==0&&f.scope->Current().retainedOperations==original.size(),"actual failed Request exposes no task and adds no original operation");
    Check(ExtraBind(f,rejected)==SD_OK,"failed Request refunds its pending native shared position");
    SdHandle over{9,9};Check(ExtraBind(f,over)==SD_RESOURCE_LIMIT&&!over.slot,"refund does not enlarge shared128 ceiling");
    scripting::SemanticInvocation absent;Check(!f.bridge->Take(absent),"failed Request publishes no bridge item");
    for(const auto task:original)Check(f.endpoint->tasks->Cancel(task)==ScopeResult::Ok&&f.endpoint->tasks->Release(task)==ScopeResult::Ok,"original saturation owner cleanup");
    Check(f.provider->ReleaseInvocation(f.owner,rejected)==SD_OK,"refund invocation cleanup");
    for(const auto invocation:retained)Check(f.provider->ReleaseInvocation(f.owner,invocation)==SD_OK,"refund retained invocation cleanup");
}
/** @summary Own both original workers and release an aborted partial startup before any join or referenced-owner destruction. */
struct AdmissionStart final {
    std::barrier<> start{3};
    std::atomic_bool abort{};
    std::thread first,second;
    bool released{};
    /** @summary Release the ordinary three-participant barrier only after both real workers started. */
    void Release(){released=true;start.arrive_and_wait();}
    /** @summary On partial startup, drop exactly the unstarted participant, release main and join the real original workers. */
    ~AdmissionStart(){
        if(!first.joinable()&&!second.joinable())return;
        if(!released){abort.store(true);if(!second.joinable())start.arrive_and_drop();Release();}
        if(first.joinable())first.join();if(second.joinable())second.join();
    }
};
/** @summary Narrow deterministic startup failure, never OS thread exhaustion or an admission substitute. */
struct InjectedSecondStartFailure final {};
/** @summary Start original workers with failure-safe ownership while preserving the ordinary admission/barrier path.
 * @param f Retained original fixture. @param mixed Whether second worker requests a task.
 * @param failSecond Inject only the second-constructor failure boundary. @param first First output identity.
 * @param second Second output identity. @param a First result. @param b Second result.
 * @param attempts Actual admission-call count. @param finished Original settled worker count. */
void StartAdmissions(Fixture& f,bool mixed,bool failSecond,SdHandle& first,SdHandle& second,SdResult& a,SdResult& b,
    std::atomic_uint& attempts,std::atomic_uint& finished){
    AdmissionStart threads;
    const auto admit=[&](bool task,SdHandle& output,SdResult& result){threads.start.arrive_and_wait();
        if(!threads.abort.load())for(unsigned attempt=0;attempt!=128&&result==SD_BUSY;++attempt){
            ++attempts;result=task?f.provider->Invoke(f.owner,f.invocation,41,{reinterpret_cast<const std::uint8_t*>(Wire.data()),static_cast<std::uint32_t>(Wire.size())},&output):ExtraBind(f,output);
            if(result==SD_BUSY)std::this_thread::yield();}
        ++finished;
    };
    // @summary Each thread owns a copied closure; unwinding cannot destroy a lambda referenced by the waiting first worker.
    threads.first=std::thread(admit,false,std::ref(first),std::ref(a));
    if(failSecond)throw InjectedSecondStartFailure{};
    threads.second=std::thread(admit,mixed,std::ref(second),std::ref(b));threads.Release();
}
/** @summary Race real admissions for the one remaining shared position without running original effects.
 * @param package Actual Base. */
void ConcurrentCapacity(const std::filesystem::path& package){
    for(bool mixed:{false,true}){Fixture f(package);const auto retained=RetainInvocations(f,126);
        SdHandle first{},second{};SdResult a=SD_BUSY,b=SD_BUSY;std::atomic_uint attempts{},finished{};
        StartAdmissions(f,mixed,false,first,second,a,b,attempts,finished);
        Check(finished==2&&attempts>=2,"both original admission workers settled before assertions or cleanup");
        Check((a==SD_OK&&b==SD_RESOURCE_LIMIT)||(b==SD_OK&&a==SD_RESOURCE_LIMIT),"concurrent actual Add admits exactly one final shared position");
        Check(f.effects==0,"concurrent capacity admission executes no effects inline");
        if(a==SD_OK)Check(f.provider->ReleaseInvocation(f.owner,first)==SD_OK,"first concurrent invocation cleanup");
        if(b==SD_OK){if(mixed){Check(f.provider->Cancel(f.owner,f.invocation,second)==SD_OK,"concurrent admitted original task cancellation");f.base->Pump();f.Release(second);}
            else Check(f.provider->ReleaseInvocation(f.owner,second)==SD_OK,"second concurrent invocation cleanup");}
        SdHandle refunded{};Check(ExtraBind(f,refunded)==SD_OK,"concurrent release refunds exactly the shared capacity");
        Check(f.provider->ReleaseInvocation(f.owner,refunded)==SD_OK,"concurrent refunded invocation cleanup");
        for(const auto invocation:retained)Check(f.provider->ReleaseInvocation(f.owner,invocation)==SD_OK,"concurrent original retained invocation cleanup");
    }
}
/** @summary Abort deterministic second-worker startup while joining the actual first worker without admission or late effects.
 * @param package Actual Base. */
void ConcurrentStartFailure(const std::filesystem::path& package){
    Fixture f(package);const auto retained=RetainInvocations(f,126);SdHandle first{},second{};SdResult a=SD_BUSY,b=SD_BUSY;
    std::atomic_uint attempts{},finished{};bool injected=false;
    try{StartAdmissions(f,true,true,first,second,a,b,attempts,finished);}catch(const InjectedSecondStartFailure&){injected=true;}
    Check(injected&&finished==1&&attempts==0&&a==SD_BUSY&&b==SD_BUSY&&!first.slot&&!second.slot&&
        f.effects==0&&f.scope->Current().retainedOperations==0,"failed second start aborts and joins original first worker without admission");
    SdHandle available{};Check(ExtraBind(f,available)==SD_OK,"joined aborted worker cannot consume capacity later");
    SdHandle rejected{9,9};Check(ExtraBind(f,rejected)==SD_RESOURCE_LIMIT&&!rejected.slot&&attempts==0&&finished==1,
        "no late admission after original worker settled");
    Check(f.provider->ReleaseInvocation(f.owner,available)==SD_OK,"startup failure available capacity cleanup");
    for(const auto invocation:retained)Check(f.provider->ReleaseInvocation(f.owner,invocation)==SD_OK,"startup failure original retained invocation cleanup");
}
/** @summary Verify one-way fresh-process identity exhaustion before original request/effects. @param package Actual Base. */
void Exhaustion(const std::filesystem::path& package){
    Check(mods::native::NativeProviderTestAccess::ExhaustBlockRange(),"fresh unused range burned once");
    Check(!mods::native::NativeProviderTestAccess::ExhaustBlockRange(),"range cannot reset or seed twice");
    Fixture f(package,false,false);bool rejected=false;
    try{f.Open();}catch(const std::runtime_error&){rejected=true;}
    Check(rejected&&f.effects==0&&f.scope->Current().retainedOperations==0,"range exhaustion before exposure admission or effect");
    scripting::SemanticInvocation queued;Check(!f.bridge->Take(queued),"range exhaustion publishes no task");
}
}
/** @summary Run isolated original-source native observation qualification. @param argc Argument count.
 * @param argv Actual Base checkout plus optional fresh-process exhaustion mode. @return Zero only on all actual checks. */
int main(int argc,char** argv)try{
    if(argc!=2&&argc!=3)throw std::runtime_error("Expected actual Base path and optional --block-exhaustion");
    if(argc==3){if(std::string_view(argv[2])!="--block-exhaustion")throw std::runtime_error("Unknown fixture mode");Exhaustion(argv[1]);}
    else {Delivery(argv[1]);KindReuse(argv[1]);ExecutionLeases(argv[1]);InFlight(argv[1]);Outcomes(argv[1]);Fences(argv[1]);DeliveryFences(argv[1]);Prepared(argv[1]);
        SharedCapacity(argv[1]);PendingCapacity(argv[1]);RequestRefund(argv[1]);ConcurrentCapacity(argv[1]);ConcurrentStartFailure(argv[1]);}
    std::cout<<"native-provider poll checks="<<checks<<" PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<"native-provider poll FAIL: "<<error.what()<<'\n';return 1;}
