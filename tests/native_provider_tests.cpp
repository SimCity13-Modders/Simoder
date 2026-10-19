#include "mods/native/native_provider.hpp"
#include "scripting/luau_compiler.hpp"
#include "scripting/source_project.hpp"
#include "mods/owned_publication.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace sc13;
using mods::native::NativeProvider;
int checks{};
/** @summary Assert behavior with a fixed nonsensitive diagnostic. @param value Observed condition. @param text Failure. */
void Check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
/** @summary Capture actual complete Base package, fixture-only trusted input. @param directory Actual checkout. @return Exact pin. */
scripting::BasePackagePin Pin(const std::filesystem::path& directory){
    scripting::BasePackagePin pin{"0.1.0-alpha",{}};
    pin.files.emplace("mod.toml",scripting::HashText(scripting::ReadBounded(directory/"mod.toml",4096)));
    for(const auto& file:std::filesystem::recursive_directory_iterator(directory/"scripts"))if(file.is_regular_file())
        pin.files.emplace(file.path().lexically_relative(directory).generic_string(),scripting::HashText(scripting::ReadBounded(file.path(),1024U*1024U)));
    return pin;
}
/** @summary Reject legitimate same-grant reservation substitution at the actual admission boundary. @param package Actual Base package. */
void BindingRace(const std::filesystem::path& package){
    int effects{};auto bridge=std::make_shared<scripting::SemanticBridge>();
    scripting::SemanticBridgeTestAccess::Configure(*bridge,{{"fixture.echo","test.echo@1","script","runtime","fixture","echo",1024,
        [&effects](const runtime::copied::Envelope& input,std::span<char> output,std::size_t& size){
            ++effects;const auto wire=input.Encode();std::memcpy(output.data(),wire.data(),wire.size());size=wire.size();return true;}}});
    scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,scripting::SemanticContextKind::Runtime,1,1);
    auto base=std::make_shared<scripting::BaseHost>(package,Pin(package),std::function<void(scripting::ScriptPhase,std::uint64_t)>{},bridge);
    auto scope=std::shared_ptr<runtime::RuntimeScope>(runtime::RuntimeScopeTestAccess::Create());
    Check(scope->CommitActivation(),"binding scope activation");scope->SetReadiness(true);
    auto host=base->Bind(scope);auto endpoint=host.semantic;
    const auto original=runtime::RuntimeScopeTestAccess::Issue(*scope);scripting::SemanticBridgeTestAccess::Issue(*endpoint,original);
    scripting::ScriptDefinition definition;definition.entry="scripts/init.luau";definition.budgetMs=20;definition.bytecode=scripting::CompileLuauSource("return {}");
    auto vm=std::make_shared<scripting::ScriptRuntime>("fixture.binding-race",definition,std::move(host));
    auto origin=mods::OwnedPublicationTestAccess::Issue(vm,endpoint,"binding-race-source");vm->Commit();
    Check(origin!=nullptr,"actual canonical native origin");
    runtime::copied::Envelope value;
    Check(bridge->Values()->Decode("{\"v\":1,\"schema\":\"test.echo@1\",\"value\":[\"ref\",\"runtime\",\"1\",\"1\"]}","test.echo@1",endpoint->storageOwner,value)==runtime::copied::Status::Ok,"binding codec");
    scripting::SemanticContextLease facts;Check(bridge->BindReferences(*endpoint,value,facts)==runtime::ScopeResult::Ok,"binding factual capture");
    scripting::SemanticEndpointBinding captured;
    Check(scripting::SemanticBridgeTestAccess::CaptureBinding(*bridge,endpoint,captured)==runtime::ScopeResult::Ok&&captured.authority==original,"whole original binding capture");
    const SdHandle owner{91,1},session{92,1};
    NativeProvider old(owner,base,endpoint,vm,std::make_shared<runtime::RuntimeLifecycle>(),session,{{41,"fixture.echo",{1,1},facts,original.opcode}});
    SdHandle oldInvocation{};Check(old.Bind(owner,session,41,{1,1},original.revision,&oldInvocation)==SD_OK,"old binding precheck succeeds");
    const auto rejects=[&](const scripting::SemanticEndpointBinding& expected){
        const auto before=scope->Current().retainedOperations;runtime::TaskHandle task;scripting::SemanticContextLease result;result.operation=999;
        Check(bridge->Request(endpoint,"fixture.echo",value,task,result,&facts,&expected)==runtime::ScopeResult::Stale&&!task&&!result.mask&&!result.operation,"whole binding rejection sanitized");
        Check(scope->Current().retainedOperations==before,"binding rejection has no task leak");
        scripting::SemanticInvocation queue;Check(!bridge->Take(queue),"binding rejection has no queue publication");
    };
    for(int field=0;field!=27;++field){auto changed=captured;
        switch(field){case 0:++changed.authority.owner;break;case 1:++changed.authority.epoch;break;case 2:++changed.authority.grantGeneration;break;
        case 3:++changed.authority.grantRevision;break;case 4:changed.authority.origin=runtime::OrdinaryScriptOrigin{original.owner,1};break;
        case 5:++changed.authority.decision.serial;break;case 6:++changed.authority.decision.revision;break;case 7:++changed.authority.decision.action;break;
        case 8:++changed.authority.reservation;break;case 9:++changed.authority.revision;break;case 10:++changed.authority.opcode;break;
        case 11:changed.authority.readiness|=runtime::CityReady;break;case 12:changed.authority.deadline+=std::chrono::milliseconds(1);break;
        case 13:++std::get<runtime::AgentOrigin>(changed.authority.origin).owner;break;case 14:++std::get<runtime::AgentOrigin>(changed.authority.origin).content;break;
        case 15:++std::get<runtime::AgentOrigin>(changed.authority.origin).actor;break;case 16:++std::get<runtime::AgentOrigin>(changed.authority.origin).channel;break;
        case 17:++std::get<runtime::AgentOrigin>(changed.authority.origin).consentSession;break;case 18:++std::get<runtime::AgentOrigin>(changed.authority.origin).entity;break;
        case 19:++std::get<runtime::AgentOrigin>(changed.authority.origin).extent;break;case 20:changed.scope.reset();break;case 21:changed.tasks.reset();break;
        case 22:changed.endpoint.reset();break;case 23:++changed.generation;break;case 24:++changed.storageOwner;break;
        case 25:++changed.baseEpoch;break;case 26:++changed.baseGeneration;break;}
        rejects(changed);
    }
    mods::LifecycleDecision refreshed;
    Check(scripting::SemanticBridgeTestAccess::RefreshReservation(*bridge,origin,refreshed)==runtime::ScopeResult::Ok&&
        refreshed.Context().reservation!=captured.authority.reservation&&
        refreshed.Context().grantGeneration==captured.authority.grantGeneration&&refreshed.Context().origin==captured.authority.origin&&
        refreshed.Context().decision==captured.authority.decision,"real same-grant refresh after old precheck");
    const auto reservations=runtime::RuntimeScopeTestAccess::Reservations(*scope);
    rejects(captured);base->Pump();Check(effects==0&&runtime::RuntimeScopeTestAccess::Reservations(*scope)==reservations,"old binding cannot adopt real refresh");
    SdHandle rejected{9,9};const auto wire=value.Encode();
    Check(old.Invoke(owner,oldInvocation,41,{reinterpret_cast<const std::uint8_t*>(wire.data()),static_cast<std::uint32_t>(wire.size())},&rejected)==SD_STALE&&!rejected.slot,"old provider remains stale after refresh");
    Check(old.ReleaseInvocation(owner,oldInvocation)==SD_OK,"old stale invocation cleanup");
    scripting::SemanticEndpointBinding beforeConcurrent,observed,afterConcurrent;
    Check(scripting::SemanticBridgeTestAccess::CaptureBinding(*bridge,endpoint,beforeConcurrent)==runtime::ScopeResult::Ok,"before barrier binding");
    std::barrier start(2);mods::LifecycleDecision concurrentDecision;runtime::ScopeResult refreshResult{},captureResult{};
    std::unique_ptr<NativeProvider> concurrentProvider;std::exception_ptr readerFailure;
    std::thread reader([&]{start.arrive_and_wait();try{
        concurrentProvider=std::make_unique<NativeProvider>(owner,base,endpoint,vm,std::make_shared<runtime::RuntimeLifecycle>(),session,
            std::vector<mods::native::ProviderRoute>{{41,"fixture.echo",{1,1},facts,original.opcode}});
        captureResult=scripting::SemanticBridgeTestAccess::CaptureBinding(*bridge,endpoint,observed);
    }catch(...){readerFailure=std::current_exception();}});
    start.arrive_and_wait();refreshResult=scripting::SemanticBridgeTestAccess::RefreshReservation(*bridge,origin,concurrentDecision);reader.join();
    if(readerFailure)std::rethrow_exception(readerFailure);
    Check(refreshResult==runtime::ScopeResult::Ok||refreshResult==runtime::ScopeResult::Busy,"actual barrier refresh result");
    Check(captureResult==runtime::ScopeResult::Ok&&scripting::SemanticBridgeTestAccess::CaptureBinding(*bridge,endpoint,afterConcurrent)==runtime::ScopeResult::Ok&&
        (observed==beforeConcurrent||observed==afterConcurrent),"B-protected capture sees complete old or new binding");
    SdHandle concurrentInvocation{};const auto concurrentAdmission=concurrentProvider->Bind(owner,session,41,{1,1},afterConcurrent.authority.revision,&concurrentInvocation);
    Check(concurrentAdmission==SD_OK||concurrentAdmission==SD_STALE,"concurrent constructor/preflight has whole original or stale binding");
    if(concurrentAdmission==SD_OK)Check(concurrentProvider->ReleaseInvocation(owner,concurrentInvocation)==SD_OK,"concurrent invocation cleanup");
    const auto current=afterConcurrent.authority;
    NativeProvider fresh(owner,base,endpoint,vm,std::make_shared<runtime::RuntimeLifecycle>(),session,{{41,"fixture.echo",{1,1},facts,current.opcode}});
    SdHandle invocation{},task{};Check(fresh.Bind(owner,session,41,{1,1},current.revision,&invocation)==SD_OK,"provider created after refresh binds current tuple");
    Check(fresh.Invoke(owner,invocation,41,{reinterpret_cast<const std::uint8_t*>(wire.data()),static_cast<std::uint32_t>(wire.size())},&task)==SD_OK,"unchanged fresh binding admits once");
    base->Pump();Check(effects==1,"unchanged actual Base path executes once");
    SdTaskInfo info{{sizeof(SdTaskInfo),1}};std::uint32_t required=999;
    Check(fresh.Poll(owner,invocation,task,&info,nullptr,0,&required)==SD_BUFFER_SMALL&&required==wire.size()&&!info.task.slot&&!info.sequence,"fresh completed poll exposes authorized sizing only");
    std::vector<std::uint8_t> copied(required);
    Check(fresh.Poll(owner,invocation,task,&info,copied.data(),static_cast<std::uint32_t>(copied.size()),&required)==SD_OK&&
        info.phase==SD_TASK_SUCCEEDED&&info.error==SD_OK&&info.flags==SD_TASK_COMMITTED&&info.sequence==4&&info.context.slot&&
        std::string_view(reinterpret_cast<const char*>(copied.data()),required)==wire,"fresh original completion and payload");
    Check(fresh.Release(owner,task)==SD_OK&&fresh.ReleaseInvocation(owner,invocation)==SD_OK,"fresh original cleanup");
    fresh.Close();old.Close();vm->Stop();origin.reset();
}
/** @summary Prove expected facts are checked inside actual Request admission and again before commit. @param package Actual Base package. */
void ContextRace(const std::filesystem::path& package){
    int effects{};auto bridge=std::make_shared<scripting::SemanticBridge>();
    scripting::SemanticBridgeTestAccess::Configure(*bridge,{{"fixture.echo","test.echo@1","script","runtime","fixture","echo",1024,
        [&effects](const runtime::copied::Envelope& input,std::span<char> output,std::size_t& size){
            ++effects;const auto wire=input.Encode();std::memcpy(output.data(),wire.data(),wire.size());size=wire.size();return true;}}});
    scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,scripting::SemanticContextKind::Runtime,1,1);
    scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,scripting::SemanticContextKind::City,2,1);
    auto base=std::make_shared<scripting::BaseHost>(package,Pin(package),std::function<void(scripting::ScriptPhase,std::uint64_t)>{},bridge);
    auto scope=std::shared_ptr<runtime::RuntimeScope>(runtime::RuntimeScopeTestAccess::Create());
    Check(scope->CommitActivation(),"race scope activation");scope->SetReadiness(true);
    auto host=base->Bind(scope);auto endpoint=host.semantic;
    const auto authority=runtime::RuntimeScopeTestAccess::Issue(*scope);scripting::SemanticBridgeTestAccess::Issue(*endpoint,authority);
    scripting::ScriptDefinition definition;definition.entry="scripts/init.luau";definition.budgetMs=20;definition.bytecode=scripting::CompileLuauSource("return {}");
    auto vm=std::make_shared<scripting::ScriptRuntime>("fixture.expected-context",definition,std::move(host));vm->Commit();
    const auto decode=[&](std::string_view wire){runtime::copied::Envelope value;
        Check(bridge->Values()->Decode(wire,"test.echo@1",endpoint->storageOwner,value)==runtime::copied::Status::Ok,"race input codec");return value;};
    const auto runtimeRef=decode("{\"v\":1,\"schema\":\"test.echo@1\",\"value\":[\"ref\",\"runtime\",\"1\",\"1\"]}");
    const auto cityRef=decode("{\"v\":1,\"schema\":\"test.echo@1\",\"value\":[\"ref\",\"city\",\"2\",\"1\"]}");
    const auto null=decode("{\"v\":1,\"schema\":\"test.echo@1\",\"value\":[\"null\"]}");
    scripting::SemanticContextLease expected;
    Check(bridge->BindReferences(*endpoint,runtimeRef,expected)==runtime::ScopeResult::Ok,"race original lease");
    const auto rejects=[&](const scripting::SemanticContextLease& original,const runtime::copied::Envelope& value){
        const auto before=scope->Current().retainedOperations;runtime::TaskHandle task;scripting::SemanticContextLease captured;
        captured.operation=999;
        Check(bridge->Request(endpoint,"fixture.echo",value,task,captured,&original)==runtime::ScopeResult::Stale&&!task&&captured.operation==0&&captured.mask==0,"atomic expected rejection sanitized");
        Check(scope->Current().retainedOperations==before,"rejection did not admit task");
        scripting::SemanticInvocation queued;Check(!bridge->Take(queued),"rejection did not publish queue");
    };
    for(int field=0;field!=7;++field){auto altered=expected;
        switch(field){case 0:++altered.baseEpoch;break;case 1:++altered.vmGeneration;break;case 2:++altered.scopeGeneration;break;
        case 3:++altered.runtimeEpoch;break;case 4:++altered.facts[0].slot;break;case 5:++altered.facts[0].generation;break;case 6:++altered.facts[0].epoch;break;}
        rejects(altered,null);
    }
    scripting::SemanticContextLease cityExpected;Check(bridge->BindReferences(*endpoint,cityRef,cityExpected)==runtime::ScopeResult::Ok,"city expected capture");
    rejects(cityExpected,null);
    Check(bridge->ValidateContext(*endpoint,expected)==runtime::ScopeResult::Ok,"early precheck passes");
    scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,scripting::SemanticContextKind::Runtime,1,1);
    rejects(expected,null);base->Pump();Check(effects==0,"old precheck cannot rebind context");
    Check(bridge->BindReferences(*endpoint,runtimeRef,expected)==runtime::ScopeResult::Ok,"current original capture");
    const SdHandle owner{77,1},session{55,1};NativeProvider provider(owner,base,endpoint,vm,std::make_shared<runtime::RuntimeLifecycle>(),session,
        {{41,"fixture.echo",{1,1},expected,authority.opcode}});
    SdHandle invocation{};Check(provider.Bind(owner,session,41,{1,1},authority.revision,&invocation)==SD_OK,"provider original capture");
    scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,scripting::SemanticContextKind::Runtime,1,1);
    const auto wire=null.Encode();SdHandle nativeTask{};
    Check(provider.Invoke(owner,invocation,41,{reinterpret_cast<const std::uint8_t*>(wire.data()),static_cast<std::uint32_t>(wire.size())},&nativeTask)==SD_STALE&&!nativeTask.slot,"provider stale route never rebinds");
    Check(provider.ReleaseInvocation(owner,invocation)==SD_OK,"stale route cleanup");
    Check(bridge->BindReferences(*endpoint,runtimeRef,expected)==runtime::ScopeResult::Ok,"late fence capture");
    runtime::TaskHandle original;scripting::SemanticContextLease captured;
    Check(bridge->Request(endpoint,"fixture.echo",null,original,captured,&expected)==runtime::ScopeResult::Ok,"unchanged expected admits");
    scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,scripting::SemanticContextKind::Runtime,1,1);
    base->Pump();runtime::TaskSnapshot hidden;
    Check(endpoint->tasks->Poll(original,hidden)==runtime::ScopeResult::Ok&&hidden.state==runtime::TaskState::Failed&&!hidden.committed&&effects==0,"late original commit fence rejects changed facts");
    Check(endpoint->tasks->Release(original)==runtime::ScopeResult::Ok,"late rejected task cleanup");
    Check(bridge->BindReferences(*endpoint,runtimeRef,expected)==runtime::ScopeResult::Ok,"positive expected capture");
    const auto extra=bridge->Request(endpoint,"fixture.echo",cityRef,original,captured,&expected);
    constexpr auto runtimeAndCity=(1U<<static_cast<unsigned>(scripting::SemanticContextKind::Runtime))|
        (1U<<static_cast<unsigned>(scripting::SemanticContextKind::City));
    if(extra!=runtime::ScopeResult::Ok||captured.mask!=runtimeAndCity)
        throw std::runtime_error("extra-reference result="+std::to_string(static_cast<int>(extra))+" mask="+std::to_string(captured.mask)+
            " revision="+std::to_string(scope->Current().revision)+" origin="+std::to_string(authority.revision));
    Check(true,"extra valid reference adds requirements");
    base->Pump();Check(endpoint->tasks->Poll(original,hidden)==runtime::ScopeResult::Ok&&hidden.committed&&hidden.state==runtime::TaskState::Succeeded&&effects==1,"actual original task completed once");
    Check(endpoint->tasks->Release(original)==runtime::ScopeResult::Ok,"positive task cleanup");
    provider.Close();vm->Stop();
}
/** @summary Exercise original Base/scope/VM/task path with explicitly synthetic producers only. @param package Actual Base. */
void Exercise(const std::filesystem::path& package){
    int calls{};
    auto bridge=std::make_shared<scripting::SemanticBridge>();
    scripting::SemanticMethod descriptor{"fixture.echo","test.echo@1","script","runtime","fixture-only","echo-confirmed",1024,
        [&calls](const runtime::copied::Envelope& input,std::span<char> output,std::size_t& size){
            ++calls;const auto wire=input.Encode();if(wire.size()>output.size())return false;
            std::memcpy(output.data(),wire.data(),wire.size());size=wire.size();return true;}};
    scripting::SemanticBridgeTestAccess::Configure(*bridge,{descriptor});
    scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,scripting::SemanticContextKind::Runtime,1,1);
    auto base=std::make_shared<scripting::BaseHost>(package,Pin(package),std::function<void(scripting::ScriptPhase,std::uint64_t)>{},bridge);
    auto scope=std::shared_ptr<runtime::RuntimeScope>(runtime::RuntimeScopeTestAccess::Create());
    Check(scope->CommitActivation(),"scope activation");scope->SetReadiness(true);
    auto host=base->Bind(scope);auto endpoint=host.semantic;
    const auto context=runtime::RuntimeScopeTestAccess::Issue(*scope);
    scripting::SemanticBridgeTestAccess::Issue(*endpoint,context);
    scripting::ScriptDefinition definition;definition.entry="scripts/init.luau";definition.budgetMs=20;
    definition.bytecode=scripting::CompileLuauSource("return {}");
    auto participant=std::make_shared<scripting::ScriptRuntime>("fixture.native-provider-adapter",definition,std::move(host));participant->Commit();
    Check(endpoint->generation==participant->Status().generation&&endpoint->generation!=0,"actual VM identity");
    runtime::copied::Envelope reference;
    Check(bridge->Values()->Decode("{\"v\":1,\"schema\":\"test.echo@1\",\"value\":[\"ref\",\"runtime\",\"1\",\"1\"]}","test.echo@1",endpoint->storageOwner,reference)==runtime::copied::Status::Ok,"reference codec");
    scripting::SemanticContextLease lease;
    Check(bridge->BindReferences(*endpoint,reference,lease)==runtime::ScopeResult::Ok,"actual factual binding");
    auto lifecycle=std::make_shared<runtime::RuntimeLifecycle>();
    const SdHandle owner{17,1},session{11,1},target{1,1};
    NativeProvider provider(owner,base,endpoint,participant,lifecycle,session,{{41,"fixture.echo",target,lease,context.opcode}});
    SdHandle invocation{99,99};
    Check(provider.Bind(owner,{},41,target,context.revision,&invocation)==SD_DENIED&&!invocation.slot,"agent cannot select zero session");
    Check(provider.Bind(owner,{12,1},41,target,context.revision,&invocation)==SD_DENIED&&!invocation.slot,"agent cannot substitute session");
    Check(provider.Bind({18,1},session,41,target,context.revision,&invocation)==SD_DENIED&&!invocation.slot,"foreign owner bind");
    Check(provider.Bind(owner,session,41,target,context.revision,&invocation)==SD_OK,"original bind");
    SdHandle rejected{77,88};
    Check(provider.Bind(owner,session,41,{2,1},context.revision,&rejected)==SD_STALE&&!rejected.slot,"wrong factual target");
    Check(provider.Bind(owner,session,41,target,context.revision+1,&rejected)==SD_CONFLICT&&!rejected.slot,"wrong original revision");
    std::array<SdHandle,127> retained{};
    for(auto& handle:retained)Check(provider.Bind(owner,session,41,target,context.revision,&handle)==SD_OK,"bounded invocation admission");
    Check(provider.Bind(owner,session,41,target,context.revision,&rejected)==SD_RESOURCE_LIMIT&&!rejected.slot,"invocation ceiling");
    const auto old=retained[0];Check(provider.ReleaseInvocation(owner,old)==SD_OK,"slot cleanup");
    Check(provider.Bind(owner,session,41,target,context.revision,&retained[0])==SD_OK&&retained[0].slot==old.slot&&retained[0].generation!=old.generation,"generation advances");
    Check(provider.ReleaseInvocation(owner,old)==SD_STALE,"old generation never rebinds");
    for(const auto handle:retained)Check(provider.ReleaseInvocation(owner,handle)==SD_OK,"bounded invocation cleanup");
    std::array<std::uint8_t,32> buffer;buffer.fill(0xAA);std::uint32_t required=999;
    SdSnapshotInfo snapshot{{sizeof(SdSnapshotInfo),1}};
    Check(provider.Snapshot(owner,invocation,41,&snapshot,buffer.data(),buffer.size(),&required)==SD_NOT_READY&&required==0&&snapshot.sequence==0,"snapshot unavailable");
    Check(std::all_of(buffer.begin(),buffer.end(),[](auto byte){return byte==0;}),"snapshot cleared caller storage");
    const std::string wire="{\"v\":1,\"schema\":\"test.echo@1\",\"value\":[\"null\"]}";
    SdBytes bytes{reinterpret_cast<const std::uint8_t*>(wire.data()),static_cast<std::uint32_t>(wire.size())};
    SdHandle task{};
    Check(provider.Invoke(owner,invocation,42,bytes,&task)==SD_DENIED&&!task.slot,"method cannot change after bind");
    const std::string malformed="{}";
    Check(provider.Invoke(owner,invocation,41,{reinterpret_cast<const std::uint8_t*>(malformed.data()),2},&task)==SD_INVALID&&!task.slot,"shared codec rejects malformed input");
    Check(provider.Invoke(owner,invocation,41,bytes,&task)==SD_OK&&calls==0,"enqueue never inline execution");
    Check(provider.ReleaseInvocation(owner,invocation)==SD_BUSY,"task retains invocation");
    SdTaskInfo info{{sizeof(SdTaskInfo),1}};buffer.fill(0xAA);required=999;
    info.header.version=99;
    Check(provider.Poll(owner,invocation,task,&info,buffer.data(),buffer.size(),&required)==SD_VERSION&&required==0&&info.header.version==99&&!info.task.slot,"wrong header sanitized");
    info.header.version=1;
    Check(provider.Poll({18,1},invocation,task,&info,buffer.data(),buffer.size(),&required)==SD_DENIED&&required==0,"foreign poll denied");
    Check(provider.Poll(owner,invocation,task,&info,buffer.data(),buffer.size(),&required)==SD_OK&&required==0&&info.sequence==1&&
        info.task.slot==task.slot&&info.context.slot&&info.phase==SD_TASK_QUEUED&&info.error==SD_BUSY&&!info.flags&&
        info.revision==context.revision,"queued original observation without a payload");
    Check(std::all_of(buffer.begin(),buffer.end(),[](auto byte){return byte==0;}),"poll cleared caller storage");
    SdHandle other{};Check(provider.Bind(owner,session,41,target,context.revision,&other)==SD_OK,"second invocation");
    Check(provider.Poll(owner,other,task,&info,buffer.data(),buffer.size(),&required)==SD_DENIED&&required==0,"task cannot change invocation");
    Check(provider.ReleaseInvocation(owner,other)==SD_OK,"unretained invocation cleanup");
    Check(provider.Cancel(owner,invocation,task)==SD_OK,"cancel before commit");base->Pump();Check(calls==0,"cancelled task effects absent");
    Check(provider.Release(owner,task)==SD_OK,"release cancelled original task");
    Check(provider.Release(owner,task)==SD_STALE,"released task stale");
    std::array<SdHandle,128> queued{};std::size_t queuedCount{};SdResult saturation=SD_OK;
    for(;queuedCount<queued.size();++queuedCount){saturation=provider.Invoke(owner,invocation,41,bytes,&queued[queuedCount]);if(saturation!=SD_OK)break;}
    if(queuedCount==queued.size())saturation=provider.Invoke(owner,invocation,41,bytes,&rejected);
    Check(queuedCount>0&&(saturation==SD_RESOURCE_LIMIT||saturation==SD_BUSY),"task/queue/pool ceiling is finite");
    for(std::size_t i=0;i<queuedCount;++i)Check(provider.Cancel(owner,invocation,queued[i])==SD_OK,"bounded queued task cancellation");
    for(std::size_t i=0;i<queuedCount;++i)base->Pump();
    Check(calls==0,"saturated cancelled queue never executes");
    for(std::size_t i=0;i<queuedCount;++i)Check(provider.Release(owner,queued[i])==SD_OK,"bounded queued task cleanup");
    Check(provider.Invoke(owner,invocation,41,bytes,&task)==SD_OK,"second original enqueue");base->Pump();Check(calls==1,"actual Base pump executes once");
    Check(provider.Cancel(owner,invocation,task)==SD_BUSY,"committed cancel does not undo");
    info={{sizeof(SdTaskInfo),1}};required=999;
    Check(provider.Poll(owner,invocation,task,&info,buffer.data(),buffer.size(),&required)==SD_BUFFER_SMALL&&info.phase==0&&info.flags==0&&
        !info.task.slot&&!info.context.slot&&!info.sequence&&!info.revision&&required==wire.size(),"completed short buffer exposes only actual required bytes");
    std::vector<std::uint8_t> completed(required);
    Check(provider.Poll(owner,invocation,task,&info,completed.data(),static_cast<std::uint32_t>(completed.size()),&required)==SD_OK&&
        info.phase==SD_TASK_SUCCEEDED&&info.error==SD_OK&&info.flags==SD_TASK_COMMITTED&&info.sequence==4&&
        std::string_view(reinterpret_cast<const char*>(completed.data()),required)==wire,"completed original metadata and actual copied payload");
    Check(runtime::RuntimeScopeTestAccess::Revoke(*scope,context.grantGeneration)==runtime::ScopeResult::Ok,"actual grant revoked");
    Check(provider.Poll(owner,invocation,task,&info,buffer.data(),buffer.size(),&required)==SD_DENIED&&required==0,"revocation suppresses retained result");
    Check(provider.Release({18,1},task)==SD_DENIED,"foreign cleanup denied");
    Check(provider.Release(owner,task)==SD_OK,"cleanup after revoke");
    Check(provider.ReleaseInvocation(owner,invocation)==SD_OK,"release original invocation");
    Check(provider.ReleaseInvocation(owner,invocation)==SD_STALE,"released invocation stale");
    // @summary Ordinary unqualified construction runs an actual VM but never manufactures authority or a native VM identity.
    auto closedScope=std::make_shared<runtime::RuntimeScope>();auto closedBinding=base->Bind(closedScope);auto closedEndpoint=closedBinding.semantic;
    auto closedVm=std::make_shared<scripting::ScriptRuntime>("fixture.closed-provider",definition,std::move(closedBinding));closedVm->Commit();
    NativeProvider closed({32,1},base,closedEndpoint,closedVm,lifecycle,{},{});
    Check(closed.Bind({32,1},{},41,target,0,&rejected)==SD_DENIED&&!rejected.slot,"unqualified source remains closed");
    closed.Close();closedVm->Stop();
    provider.Close();
    Check(provider.Bind(owner,session,41,target,context.revision,&rejected)==SD_STALE&&!rejected.slot,"closed provider rejects admission");
    participant->Stop();
}
}
/** @summary Run isolated actual-source adapter qualification. @param argc Count. @param argv Base source directory. @return Process test result. */
int main(int argc,char** argv)try{
    if(argc!=2)throw std::runtime_error("Expected actual Base directory");
    BindingRace(argv[1]);ContextRace(argv[1]);Exercise(argv[1]);std::cout<<"native-provider checks="<<checks<<" PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<"native-provider FAIL: "<<error.what()<<'\n';return 1;}
