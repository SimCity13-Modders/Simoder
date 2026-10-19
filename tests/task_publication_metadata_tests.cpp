#include "runtime/semantic_tasks.hpp"
#include "scripting/base_host.hpp"
#include "scripting/semantic_bridge.hpp"
#include "scripting/luau_compiler.hpp"
#include "scripting/source_project.hpp"
#include "mods/owned_publication.hpp"
#include <array>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sc13;
using namespace runtime;
int checks{};
constexpr std::string_view Wire="{\"v\":1,\"schema\":\"metadata.v1\",\"value\":[\"null\"]}";
/** @summary Assert actual original-task behavior. @param value Actual condition. @param message Fixed diagnostic. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Verify a callback by copying the original immutable wire. @param input Original value.
 * @param output Reserved bytes. @param size Written bytes. @return Successful verified postcondition. */
bool Echo(const copied::Envelope& input,std::span<char> output,std::size_t& size){
    const auto wire=input.Encode();if(wire.size()>output.size())return false;
    std::memcpy(output.data(),wire.data(),wire.size());size=wire.size();return true;
}
/** @summary Own actual scope/store/codec and fixture-only authority, never a replacement task model. */
struct Fixture final {
    std::shared_ptr<RuntimeCommitDomain> domain;
    std::shared_ptr<RuntimeScope> scope;
    std::shared_ptr<copied::Storage> storage=std::make_shared<copied::Storage>();
    OperationContext context;
    copied::Envelope input;
    SemanticTasks tasks;
    /** @summary Prepare explicit isolated producers. @param shared Actual optional shared writer. */
    explicit Fixture(std::shared_ptr<RuntimeCommitDomain> shared={}):domain(shared?std::move(shared):std::make_shared<RuntimeCommitDomain>()),
        scope(RuntimeScopeTestAccess::Create(domain)),tasks(scope,storage){
        Check(scope->CommitActivation(),"actual scope activation");scope->SetReadiness(true);
        context=RuntimeScopeTestAccess::Issue(*scope);
        Check(storage->Decode(Wire,"metadata.v1",context.owner,input)==copied::Status::Ok,"actual copied input");
    }
    /** @summary Admit through actual forwarded adapter. @return Original issued task. */
    TaskHandle Admit(){TaskHandle task;Check(tasks.Admit(context,input,512,task)==ScopeResult::Ok,"actual semantic admission");return task;}
    /** @summary Admit direct handle for narrow exhaustion injection on the same original store. @return Original scope handle. */
    ScopeHandle Direct(){ScopeHandle task;Check(scope->QueueValue(context,input,storage,512,task)==ScopeResult::Ok,"actual direct admission");return task;}
};
/** @summary Verify original revision, immutable terminal and consistent in-flight observations with retained participants. */
void Publications(){
    auto domain=std::make_shared<RuntimeCommitDomain>();
    {auto writer=domain->ReserveWriter();Check(writer.Acquired()&&writer.Advance()!=0,"real earlier domain writer");}
    Fixture f(domain);auto task=f.Admit();TaskSnapshot observed;
    Check(f.tasks.Poll(task,observed)==ScopeResult::Ok&&observed.sequence==1&&observed.revision==f.context.revision&&
        observed.revision==domain->Revision()&&observed.state==TaskState::Queued&&!observed.committed,"queued original publication");
    const auto admission=observed.revision;
    Check(f.tasks.Begin(task)==ScopeResult::Ok,"preparing transition");
    Check(f.tasks.Poll(task,observed)==ScopeResult::Ok&&observed.sequence==2&&observed.revision==admission&&
        observed.state==TaskState::Running&&!observed.committed,"preparing is a separate original observation");
    std::promise<void> entered,release;auto enteredFuture=entered.get_future();auto leave=release.get_future().share();int effects{};
    auto execution=std::async(std::launch::async,[&]{return f.tasks.Commit(task,[&](const auto& input,auto output,auto& size){
        ++effects;entered.set_value();leave.wait();return Echo(input,output,size);});});
    const bool inFlight=enteredFuture.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    if(!inFlight){release.set_value();static_cast<void>(execution.get());Check(false,"commit callback entered before budget");}
    bool consistent=true;
    for(int i=0;i<32;++i){TaskSnapshot current;const auto status=f.tasks.Poll(task,current);
        consistent=consistent&&status==ScopeResult::Ok&&current.sequence==3&&current.revision==admission+1&&
            current.revision==domain->Revision()&&current.state==TaskState::Running&&current.committed&&current.outcome==ScopeResult::Busy&&!current.value;}
    const bool retained=f.tasks.Release(task)==ScopeResult::Busy;
    release.set_value();const auto outcome=execution.get();
    Check(consistent&&retained&&outcome==ScopeResult::Ok&&effects==1,"whole committed tuple and original participant retained to actual terminal");
    Check(f.tasks.Poll(task,observed)==ScopeResult::Ok&&observed.sequence==4&&observed.revision==admission+1&&
        observed.state==TaskState::Succeeded&&observed.committed&&observed.value.Encode()==Wire,"terminal factual tuple");
    const auto terminal=observed;
    Fixture other(domain);auto another=other.Admit();Check(other.tasks.Begin(another)==ScopeResult::Ok&&other.tasks.Commit(another,Echo)==ScopeResult::Ok,"real other task advances domain");
    Check(domain->Revision()>terminal.revision,"later actual revision differs");
    for(int i=0;i<8;++i)Check(f.tasks.Poll(task,observed)==ScopeResult::Ok&&observed.sequence==terminal.sequence&&observed.revision==terminal.revision&&
        observed.state==terminal.state&&observed.outcome==terminal.outcome&&observed.value.Encode()==terminal.value.Encode(),"poll preserves terminal publication despite later writer");
    Check(f.tasks.Cancel(task)==ScopeResult::Busy&&f.tasks.Commit(task,Echo)==ScopeResult::Stale,"terminal never replaced or replayed");
    Check(RuntimeScopeTestAccess::Revoke(*f.scope,f.context.grantGeneration)==ScopeResult::Ok,"actual authority revoked");
    Check(f.tasks.Poll(task,observed)==ScopeResult::Denied&&observed.sequence==0&&observed.revision==0&&!observed.value&&!observed.committed,"denial clears metadata atomically");
    Check(f.tasks.Release(task)==ScopeResult::Ok&&other.tasks.Release(another)==ScopeResult::Ok,"owner cleanup after revoke");
}
/** @summary Verify all precommit terminal paths and uncertain factual completion. */
void Terminals(){
    for(int mode=0;mode!=5;++mode){Fixture f;auto task=f.Admit();TaskSnapshot snapshot;
        if(mode!=0)Check(f.tasks.Begin(task)==ScopeResult::Ok,"terminal preparation");
        if(mode==0||mode==1){Check(f.tasks.Cancel(task)==ScopeResult::Ok,"actual precommit cancellation");
            Check(f.tasks.Poll(task,snapshot)==ScopeResult::Ok&&snapshot.sequence==static_cast<std::uint64_t>(mode?3:2)&&
                snapshot.revision==f.context.revision&&snapshot.state==TaskState::Cancelled&&!snapshot.committed,"cancel publication retains admission revision");}
        if(mode==2){Check(f.tasks.FinishUncommitted(task,ScopeResult::Conflict)==ScopeResult::Ok,"actual precommit failure");
            Check(f.tasks.Poll(task,snapshot)==ScopeResult::Ok&&snapshot.sequence==3&&snapshot.state==TaskState::Failed&&snapshot.outcome==ScopeResult::Conflict&&!snapshot.committed,"failed uncommitted tuple");}
        if(mode>=3){Check(f.tasks.Commit(task,[mode](const auto&,auto,auto&)->bool{if(mode==4)throw std::runtime_error("controlled callback failure");return false;})==ScopeResult::Indeterminate,"postcondition or exception remains indeterminate");
            Check(f.tasks.Poll(task,snapshot)==ScopeResult::Ok&&snapshot.sequence==4&&snapshot.revision==f.domain->Revision()&&snapshot.committed&&snapshot.state==TaskState::Indeterminate,"uncertain terminal factual revision");}
        const auto sequence=snapshot.sequence;
        Check(f.tasks.FinishUncommitted(task,ScopeResult::Invalid)==ScopeResult::Stale&&f.tasks.Poll(task,snapshot)==ScopeResult::Ok&&snapshot.sequence==sequence,"terminal outcome immutable");
        Check(f.tasks.Release(task)==ScopeResult::Ok,"terminal cleanup");
    }
    for(bool preparing:{false,true}){Fixture f;auto task=f.Admit();if(preparing)Check(f.tasks.Begin(task)==ScopeResult::Ok,"stop preparation");
        f.scope->RequestStop();TaskSnapshot output;output.sequence=123;output.revision=456;
        Check(f.tasks.Poll(task,output)==ScopeResult::Stale&&output.sequence==0&&output.revision==0&&!output.value,"stop denies all metadata");
        Check(f.tasks.Release(task)==ScopeResult::Ok,"stopped original task cleanup");}
    Fixture legacy;const auto task=legacy.Direct();
    Check(legacy.scope->Cancel(legacy.context,task)==ScopeResult::Ok,"shared legacy cancel on actual value task");TaskSnapshot cancelled;
    Check(legacy.scope->PollValue(task,cancelled)==ScopeResult::Ok&&cancelled.sequence==2&&cancelled.state==TaskState::Cancelled,"shared cancel stamps original value tuple");
    Check(legacy.scope->Release(task.owner,task)==ScopeResult::Ok,"legacy-cancel value cleanup");
}
/** @summary Prove checked capacity before effects and the reserved final UINT64_MAX observation. */
void Exhaustion(){
    for(int mode=0;mode!=3;++mode){Fixture f;auto task=f.Direct();TaskSnapshot snapshot;int effects{};
        if(mode){Check(f.scope->BeginValue(task)==ScopeResult::Ok,"exhaustion preparation");}
        Check(!RuntimeScopeTestAccess::SeedTaskSequence(*f.scope,task,UINT64_MAX),"impossible nonterminal exhausted seed rejected");
        Check(RuntimeScopeTestAccess::SeedTaskSequence(*f.scope,task,mode==2?UINT64_MAX-2:UINT64_MAX-1),"narrow actual sequence fixture");
        const auto retainedCharge=f.storage->Used();
        if(mode==0)Check(f.scope->BeginValue(task)==ScopeResult::Busy,"preparing preserves one terminal publication");
        else {const auto result=f.scope->CommitValue(task,[&](const auto& input,auto output,auto& size){++effects;return Echo(input,output,size);});
            Check(result==(mode==2?ScopeResult::Ok:ScopeResult::Busy)&&effects==(mode==2?1:0),"effect marker requires reserved terminal capacity");}
        if(mode!=2)Check(f.scope->Current().retainedOperations==1&&f.scope->Current().running==0&&
            f.domain->Revision()==f.context.revision&&f.storage->Used()==retainedCharge,"exhaustion retains original record, quota and revision without effects");
        if(mode!=2)Check(f.scope->FinishValue(task,ScopeResult::Cancelled)==ScopeResult::Ok,"retained cancellation uses final slot");
        Check(f.scope->PollValue(task,snapshot)==ScopeResult::Ok&&snapshot.sequence==UINT64_MAX&&snapshot.committed==(mode==2),"final observation is reserved and never wraps");
        const auto revision=snapshot.revision;
        Check(f.scope->FinishValue(task,ScopeResult::Invalid)==ScopeResult::Stale&&f.scope->PollValue(task,snapshot)==ScopeResult::Ok&&snapshot.sequence==UINT64_MAX&&snapshot.revision==revision,"exhausted terminal immutable");
        Check(f.scope->Release(task.owner,task)==ScopeResult::Ok,"sequence budget cleanup");
    }
}
/** @summary Pin actual authored Base for prepared-publication verification. @param directory Actual package. @return Exact local pin. */
scripting::BasePackagePin Pin(const std::filesystem::path& directory){scripting::BasePackagePin pin{"0.1.0-alpha",{}};
    pin.files.emplace("mod.toml",scripting::HashText(scripting::ReadBounded(directory/"mod.toml",4096)));
    for(const auto& file:std::filesystem::recursive_directory_iterator(directory/"scripts"))if(file.is_regular_file())
        pin.files.emplace(file.path().lexically_relative(directory).generic_string(),scripting::HashText(scripting::ReadBounded(file.path(),1024U*1024U)));
    return pin;}
/** @summary Qualify metadata of the actual original writer's compound registry publication. @param package Real Base. */
void Prepared(const std::filesystem::path& package){
    auto bridge=std::make_shared<scripting::SemanticBridge>();
    scripting::SemanticMethod method{"fixture.registry","metadata.v1","script","runtime","fixture","prepared",512,Echo};method.registryPublication=true;
    scripting::SemanticBridgeTestAccess::Configure(*bridge,{method});scripting::SemanticBridgeTestAccess::ObserveContext(*bridge,scripting::SemanticContextKind::Runtime,1,1);
    scripting::BaseHost base(package,Pin(package),{},bridge);Fixture f;
    auto host=base.Bind(f.scope);auto endpoint=host.semantic;scripting::SemanticBridgeTestAccess::Issue(*endpoint,f.context);
    scripting::ScriptDefinition definition;definition.entry="scripts/init.luau";definition.budgetMs=20;definition.bytecode=scripting::CompileLuauSource("return {}");
    auto vm=std::make_shared<scripting::ScriptRuntime>("fixture.metadata-prepared",definition,std::move(host));
    auto origin=mods::OwnedPublicationTestAccess::Issue(vm,endpoint,"metadata-prepared-source");vm->Commit();
    Check(mods::OwnedPublicationTestAccess::Transition(*origin,mods::PublicationOrigin::Phase::Active),"actual registry owner active");
    copied::Envelope input;Check(bridge->Values()->Decode(Wire,"metadata.v1",endpoint->storageOwner,input)==copied::Status::Ok,"prepared original pool input");
    TaskHandle task;scripting::SemanticContextLease context;
    Check(bridge->Request(endpoint,"fixture.registry",input,task,context)==ScopeResult::Ok,"original prepared task admitted");
    scripting::SemanticInvocation invocation;Check(bridge->Take(invocation),"original prepared queue captured");
    mods::PatchRegistry registry;mods::OwnedPublication plan;std::string error;
    Check(mods::OwnedPublicationTestAccess::Prepare(origin,registry,invocation,{{{1,2,3},4,5.0F}},plan,error),"actual registry plan prepared");
    Check(bridge->DispatchPrepared(invocation,registry,plan)==ScopeResult::Ok,"actual original prepared writer committed");
    TaskSnapshot snapshot;Check(endpoint->tasks->Poll(task,snapshot)==ScopeResult::Ok&&snapshot.sequence==3&&snapshot.committed&&
        snapshot.revision==f.domain->Revision()&&snapshot.revision>f.context.revision&&snapshot.state==TaskState::Succeeded&&snapshot.value,"prepared compound publication records actual writer revision");
    const auto terminal=snapshot;
    {auto writer=f.domain->ReserveWriter();Check(writer.Acquired()&&writer.Advance()>terminal.revision,"actual unrelated prepared-domain writer");}
    Check(endpoint->tasks->Poll(task,snapshot)==ScopeResult::Ok&&snapshot.sequence==terminal.sequence&&snapshot.revision==terminal.revision,"prepared observation survives later writer");
    Check(endpoint->tasks->Release(task)==ScopeResult::Ok,"prepared original cleanup");vm->Stop();
}
}
/** @summary Run only isolated actual-source metadata qualification. @param argc Count. @param argv Actual Base path. @return Test process result. */
int main(int argc,char** argv)try{
    Check(argc==2,"actual Base path required");Publications();Terminals();Exhaustion();Prepared(argv[1]);
    std::cout<<"task-publication metadata checks="<<checks<<" PASS\n";return 0;
}catch(const std::exception& failure){std::cerr<<"task-publication metadata FAIL: "<<failure.what()<<'\n';return 1;}
