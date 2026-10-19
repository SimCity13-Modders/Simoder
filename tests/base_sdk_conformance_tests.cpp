#include "native_junction_fixture.hpp"
#include "scripting/base_host.hpp"
#include "scripting/semantic_bridge.hpp"
#include "scripting/luau_compiler.hpp"
#include "scripting/source_project.hpp"
#include "Luau/Parser.h"
#include "Luau/Lexer.h"
#include "Luau/Allocator.h"
#include "Luau/ParseResult.h"
#include "../third_party/tomlplusplus/toml.hpp"
#include <Windows.h>
#include <winioctl.h>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace sc13::scripting;
using namespace sc13::runtime;
constexpr std::size_t Maximum=1024U*1024U;
unsigned checks{},vmConstructions{};
/** @summary Reject a failed executable oracle. @param condition Native result. @param message Fixed diagnostic. */
void Check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
/** @summary Require the intended exact rejection category rather than unrelated I/O/parser failure. @param action Operation. @param expected Exact source-defined category. @param message Failure diagnostic. */
template<class F>void Reject(F action,std::string_view expected,const char* message){bool denied{};try{action();}catch(const std::exception& error){if(std::string_view(error.what())!=expected)throw std::runtime_error(std::string(message)+": unexpected rejection: "+error.what());denied=true;}Check(denied,message);}
/** @summary Normalize computed digest representation only; never normalize payload bytes. @param bytes Exact input. @return Lowercase digest. */
std::string Digest(std::string_view bytes){auto hash=HashText(bytes);for(auto& c:hash)if(c>='A'&&c<='F')c=static_cast<char>(c-'A'+'a');return hash;}
/** @summary Require each existing root/ancestor to be plain, without reparse traversal. @param path Absolute mapped input. @param directory Whether last component is directory. */
void PlainChain(const std::filesystem::path& path,bool directory){
    Check(path.is_absolute(),"absolute owner-root path required");auto current=path.root_path();
    for(const auto& component:path.relative_path()){
        Check(component!=L"."&&component!=L".."&&component.wstring().find(L':')==std::wstring::npos,"ambiguous path component");
        current/=component;const DWORD attributes=GetFileAttributesW(current.c_str());
        Check(attributes!=INVALID_FILE_ATTRIBUTES,"missing root or ancestor");Check(!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"reparse root or ancestor");
        if(current!=path||directory)Check((attributes&FILE_ATTRIBUTE_DIRECTORY)!=0,"ancestor must be directory");
        else Check(!(attributes&FILE_ATTRIBUTE_DIRECTORY),"input must be file");
    }
}
/** @summary Read one fixed descendant after root/ancestor checks. @param root Owner root. @param relative Fixed source mapping. @return Bounded exact bytes. */
std::string Read(const std::filesystem::path& root,const std::filesystem::path& relative){
    Check(!relative.is_absolute(),"fixed mapping must be relative");PlainChain(root,true);const auto file=root/relative;PlainChain(file,false);return ReadBounded(file,Maximum);
}
/** @summary Strict source-owned profile identity. */
struct Identity{std::array<std::string,5> hashes;};
/** @summary Parse only the closed metadata schema. @param bytes Source-owned metadata. @return Actual fixed hashes. */
Identity ParseIdentity(std::string_view bytes){
    Check(bytes.size()<=16384,"profile metadata bounded");auto table=toml::parse(bytes);
    const std::set<std::string> keys{"schemaVersion","sdkPackageVersion","profile","sdkManifestSha256","baseDeclarationSha256","examplesSdkLockSha256","publishedScriptSha256","taskFixtureSha256"};
    Check(table.size()==keys.size(),"exact profile fields");for(const auto& [key,value]:table){static_cast<void>(value);Check(keys.contains(std::string(key.str())),"unknown profile field");}
    Check(table["schemaVersion"].value<std::int64_t>()==1&&table["sdkPackageVersion"].value<std::string>()=="0.2.0"&&table["profile"].value<std::string>()=="base-luau-alpha-v1","profile version/schema mismatch");
    constexpr std::array<const char*,5> fields{"sdkManifestSha256","baseDeclarationSha256","examplesSdkLockSha256","publishedScriptSha256","taskFixtureSha256"};Identity result;
    for(unsigned i=0;i<fields.size();++i){const auto value=table[fields[i]].value<std::string>();Check(value.has_value()&&value->size()==64&&std::all_of(value->begin(),value->end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}),"strict lowercase digest");result.hashes[i]=*value;}
    return result;
}
/** @summary The five fixed canonical inputs; no caller-controlled authority mapping. */
constexpr std::array<const char*,5> Paths{"sdk-manifest.json","Luau/base.d.luau","sdk.lock.json","examples/BaseApiExample/scripts/init.luau","tests/fixtures/base/task-profile.luau"};
/** @summary Retain only verified script/declaration bytes before any VM construction. */
struct Content{std::array<std::string,5> bytes;};
/** @summary Verify all five exact mapped files. @param sdk Canonical SDK. @param examples Canonical examples. @param identity Source-owned identity. @return Immutable captured input. */
Content Capture(const std::filesystem::path& sdk,const std::filesystem::path& examples,const Identity& identity){Content content;for(unsigned i=0;i<Paths.size();++i){content.bytes[i]=Read(i<2?sdk:examples,Paths[i]);Check(Digest(content.bytes[i])==identity.hashes[i],"canonical input digest mismatch before VM creation");}return content;}
/** @summary Validate a fixed owned effect's containment and every existing parent before mutation. @param root Fresh owned leaf. @param path Exact fixed child. */
void OwnedParent(const std::filesystem::path& root,const std::filesystem::path& path){
    Check(root.is_absolute()&&path.is_absolute()&&root==root.lexically_normal()&&path==path.lexically_normal(),"normalized absolute fixture effect required");
    const auto relative=path.lexically_relative(root);Check(!relative.empty()&&!relative.is_absolute(),"fixture effect must be descendant");for(const auto& part:relative)Check(part!=L"."&&part!=L"..","fixture effect escaped original owner");PlainChain(root,true);PlainChain(path.parent_path(),true);
}
/** @summary Create one directory without recursive traversal, refusing stale collision unless explicitly permitted for the fixed evidence parent. @param parent Verified plain parent. @param name Fixed child name. @param permitExisting Fixed evidence parent only. @return Verified child. */
std::filesystem::path Directory(const std::filesystem::path& parent,const wchar_t* name,bool permitExisting=false){
    PlainChain(parent,true);const auto child=parent/name;
    if(!CreateDirectoryW(child.c_str(),nullptr)){const DWORD error=GetLastError();if(!permitExisting||error!=ERROR_ALREADY_EXISTS)throw std::runtime_error("exclusive fixture directory creation failed Win32="+std::to_string(error));}
    PlainChain(child,true);return child;
}
/** @summary Write only an owned fixed file after positive ancestor/final checks. @param root Fresh owned leaf. @param path Fixed destination. @param bytes Exact fixture bytes. */
void Write(const std::filesystem::path& root,const std::filesystem::path& path,std::string_view bytes){OwnedParent(root,path);const DWORD attributes=GetFileAttributesW(path.c_str());if(attributes!=INVALID_FILE_ATTRIBUTES)PlainChain(path,false);else Check(GetLastError()==ERROR_FILE_NOT_FOUND,"owned write target must be absent or plain");std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));Check(bool(file),"negative fixture write");}
/** @summary Rename a known owned plain entry into an absent owned destination. @param root Fresh owner. @param from Fixed source. @param to Fixed destination. @param directory Source kind. */
void Rename(const std::filesystem::path& root,const std::filesystem::path& from,const std::filesystem::path& to,bool directory){OwnedParent(root,from);OwnedParent(root,to);PlainChain(from,directory);Check(GetFileAttributesW(to.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"rename destination must be absent");std::filesystem::rename(from,to);}
/** @summary Remove only a positively validated plain owned file, never a link or recursive tree. @param root Fresh owner. @param path Fixed child. */
void Remove(const std::filesystem::path& root,const std::filesystem::path& path){OwnedParent(root,path);PlainChain(path,false);Check(std::filesystem::remove(path),"exact owned entry removal");}
/** @summary Count the three declared genuine native mount-point creations, without retries. */
unsigned setCalls{};
/** @summary Bind shared native junction operations to this fixture's original safety checks. */
struct JunctionPolicy final{
    /** @summary Require an actual observation. @param value Predicate. @param message Diagnostic. */
    static void Require(bool value,const char* message){Check(value,message);}
    /** @summary Validate a fixed child before native effects. @param root Owner. @param path Child. */
    static void ValidateChild(const std::filesystem::path& root,const std::filesystem::path& path){OwnedParent(root,path);}
    /** @summary Validate native plain ancestors and kind. @param path Target. @param directory Required kind. */
    static void ValidatePlain(const std::filesystem::path& path,bool directory){PlainChain(path,directory);}
    /** @summary Exclusively create one checked directory. @param parent Plain parent. @param leaf Fixed leaf. @return Created child. */
    static std::filesystem::path CreateDirectory(const std::filesystem::path& parent,const wchar_t* leaf){return Directory(parent,leaf);}
};
using JunctionSupport=sc13::testing::NativeJunctionFixture<JunctionPolicy>;
using JunctionIdentity=JunctionSupport::JunctionIdentity;
using JunctionHandle=JunctionSupport::JunctionHandle;
/** @summary Copy the identity of the exact plain target directory. @param target Plain owned directory. @return Native directory identity. */
JunctionIdentity DirectoryIdentity(const std::filesystem::path& target){PlainChain(target,true);JunctionHandle node(JunctionSupport::OpenNode(target));return JunctionSupport::NodeIdentity(node.value);}
/** @summary Recheck the actual target path and sentinel after rejection or cleanup. @param owner Fresh owner. @param target Plain original target. @param identity Original target identity. @param sentinel Fixed target descendant. @param bytes Expected exact sentinel bytes. */
void VerifyTarget(const std::filesystem::path& owner,const std::filesystem::path& target,const JunctionIdentity& identity,const std::filesystem::path& sentinel,std::string_view bytes){OwnedParent(owner,target);OwnedParent(owner,sentinel);Check(DirectoryIdentity(target)==identity,"original plain target path retains native identity");PlainChain(sentinel,false);Check(ReadBounded(sentinel,Maximum)==bytes,"original target sentinel bytes preserved");}
/** @summary Exercise strict metadata/content failures using copied roots, never canonical mutations. @param identity Actual pin. @param content Actual verified bytes. */
void TransportNegatives(const Identity& identity,const Content& content){
    const auto source=std::filesystem::path(__FILE__);Check(source.is_absolute(),"build must retain source-owned absolute file identity");const auto workspace=source.parent_path().parent_path();PlainChain(workspace,true);
    // @summary The existing trusted analysis parent is proven plain before even creating the fixed evidence directory.
    const auto analysis=workspace/".analysis";PlainChain(analysis,true);const auto evidence=Directory(analysis,L"sdk-profile-negatives",true);
    const auto pid=std::to_wstring(GetCurrentProcessId());const auto root=Directory(evidence,pid.c_str());std::cout<<"owned negative fixture retained at "<<root.string()<<'\n';const auto sdk=Directory(root,L"sdk"),examples=Directory(root,L"examples");
    Directory(sdk,L"Luau");const auto exampleParent=Directory(examples,L"examples");const auto example=Directory(exampleParent,L"BaseApiExample");Directory(example,L"scripts");const auto tests=Directory(examples,L"tests");const auto fixtures=Directory(tests,L"fixtures");Directory(fixtures,L"base");
    for(unsigned i=0;i<Paths.size();++i)Write(root,(i<2?sdk:examples)/Paths[i],content.bytes[i]);
    static_cast<void>(Capture(sdk,examples,identity));
    for(unsigned i=0;i<Paths.size();++i){const auto path=(i<2?sdk:examples)/Paths[i];Write(root,path,content.bytes[i]+"\nTAMPER");Reject([&]{static_cast<void>(Capture(sdk,examples,identity));},"canonical input digest mismatch before VM creation","tampered canonical input admitted");Write(root,path,content.bytes[i]);Remove(root,path);Reject([&]{static_cast<void>(Capture(sdk,examples,identity));},"missing root or ancestor","missing canonical input admitted");Write(root,path,content.bytes[i]);}
    // @summary Genuine mount points exercise the generic reparse-before-kind guard; regular-file symlink tag execution remains SOURCE_ONLY.
    const auto rootAlias=root/L"sdk-alias";const auto rootIdentity=DirectoryIdentity(sdk);const auto rootLinkIdentity=JunctionSupport::Junction(root,rootAlias,sdk,setCalls,3);
    Reject([&]{static_cast<void>(Capture(rootAlias,examples,identity));},"reparse root or ancestor","reparse SDK root admitted");
    VerifyTarget(root,sdk,rootIdentity,sdk/L"sdk-manifest.json",content.bytes[0]);JunctionSupport::RemoveJunction(root,rootAlias,sdk,rootLinkIdentity);VerifyTarget(root,sdk,rootIdentity,sdk/L"sdk-manifest.json",content.bytes[0]);Check(Capture(sdk,examples,identity).bytes==content.bytes,"all canonical bytes restored after root junction");
    const auto declaration=sdk/"Luau/base.d.luau";const auto luau=sdk/L"Luau",originalLuau=sdk/L"Luau-original";Rename(root,luau,originalLuau,true);const auto luauIdentity=DirectoryIdentity(originalLuau);const auto ancestorLinkIdentity=JunctionSupport::Junction(root,luau,originalLuau,setCalls,3);
    Reject([&]{static_cast<void>(Capture(sdk,examples,identity));},"reparse root or ancestor","reparse ancestor admitted");
    VerifyTarget(root,originalLuau,luauIdentity,originalLuau/L"base.d.luau",content.bytes[1]);JunctionSupport::RemoveJunction(root,luau,originalLuau,ancestorLinkIdentity);VerifyTarget(root,originalLuau,luauIdentity,originalLuau/L"base.d.luau",content.bytes[1]);Rename(root,originalLuau,luau,true);Check(Capture(sdk,examples,identity).bytes==content.bytes,"all canonical bytes restored after ancestor junction");
    const auto originalDeclaration=sdk/L"declaration-original";Rename(root,declaration,originalDeclaration,false);const auto target=Directory(root,L"declaration-target");const auto sentinel=target/L"marker.txt";constexpr std::string_view sentinelBytes="native-junction-sentinel";Write(root,sentinel,sentinelBytes);const auto targetIdentity=DirectoryIdentity(target);const auto finalLinkIdentity=JunctionSupport::Junction(root,declaration,target,setCalls,3);
    Reject([&]{static_cast<void>(Capture(sdk,examples,identity));},"reparse root or ancestor","reparse final input admitted");
    VerifyTarget(root,target,targetIdentity,sentinel,sentinelBytes);JunctionSupport::RemoveJunction(root,declaration,target,finalLinkIdentity);VerifyTarget(root,target,targetIdentity,sentinel,sentinelBytes);Rename(root,originalDeclaration,declaration,false);Check(Capture(sdk,examples,identity).bytes==content.bytes,"all canonical bytes restored after final junction");
    Check(setCalls==3,"exactly three genuine mount-point creations without retries");std::cout<<"generic reparse guard: three actual mount points; regular-file symlink tag SOURCE_ONLY\n";
    Check(vmConstructions==0,"transport rejection happened before actual VM construction");
}
/** @summary Validate actual declaration grammar and a rejecting control. @param declaration Verified SDK bytes. */
void Grammar(std::string_view declaration){Luau::Allocator allocator;Luau::AstNameTable names(allocator);Luau::ParseOptions options;options.allowDeclarationSyntax=true;auto parsed=Luau::Parser::parse(declaration.data(),declaration.size(),names,allocator,options);Check(parsed.root&&parsed.errors.empty(),"canonical declaration grammar");constexpr std::string_view malformed="declare class end !";auto bad=Luau::Parser::parse(malformed.data(),malformed.size(),names,allocator,options);Check(!bad.errors.empty(),"declaration parser negative actually rejects");std::cout<<"Luau.Analysis/editor inference NOT_RUN\n";}
/** @summary Pin every actual Base source independently of SDK content transport. @param root Base checkout. @return Trusted complete pin. */
BasePackagePin Pin(const std::filesystem::path& root){PlainChain(root,true);BasePackagePin pin{"0.1.0-alpha",{}};PlainChain(root/"mod.toml",false);pin.files.emplace("mod.toml",HashText(ReadBounded(root/"mod.toml",4096)));PlainChain(root/"scripts",true);for(const auto& e:std::filesystem::recursive_directory_iterator(root/"scripts")){PlainChain(e.path(),e.is_directory());if(e.is_regular_file())pin.files.emplace(e.path().lexically_relative(root).generic_string(),HashText(ReadBounded(e.path(),Maximum)));}return pin;}
/** @summary Create real fixture authority only in separate test host. @return Original active scope. */
std::shared_ptr<RuntimeScope> Scope(){auto scope=std::shared_ptr<RuntimeScope>(RuntimeScopeTestAccess::Create());Check(scope->CommitActivation(),"original fixture scope activated");scope->SetReadiness(true);return scope;}
/** @summary Compile captured actual script into an independently bound VM. @param host Actual Base. @param scope Original scope. @param source Verified/hostile source. @return Committed runtime. */
std::unique_ptr<ScriptRuntime> Mod(BaseHost& host,const std::shared_ptr<RuntimeScope>& scope,std::string_view source){auto binding=host.Bind(scope);const auto grant=RuntimeScopeTestAccess::Issue(*scope);const auto original=scope->Current();Check(grant.owner!=0&&grant.owner==original.generation&&grant.epoch==original.epoch,"one genuine grant matches original scope before endpoint Issue");SemanticBridgeTestAccess::Issue(*binding.semantic,grant);ScriptDefinition definition;definition.entry="scripts/init.luau";definition.language="luau";definition.budgetMs=20;definition.memoryBytes=8U*1024U*1024U;definition.bytecode=CompileLuauSource(std::string(source));++vmConstructions;auto vm=std::make_unique<ScriptRuntime>("fixture.sdk",definition,std::move(binding));vm->Commit();return vm;}
/** @summary Require actual successful marker, never source-text presence. @param vm Original runtime. @param marker Fixed assertion marker. */
void Marker(ScriptRuntime& vm,std::string_view marker){const auto status=vm.Status();if(!status.running)throw std::runtime_error(status.diagnostic);const auto logs=vm.DrainLogs();Check(std::any_of(logs.begin(),logs.end(),[&](const auto& line){return line.find(marker)!=std::string::npos;}),"actual profile completion marker missing");}
/** @summary Pump distinct existing owner/Base turns without timers or retrying constructors. @param host Base. @param vm Mod. */
void Turns(BaseHost& host,ScriptRuntime& vm){for(unsigned i=0;i!=12;++i){vm.Tick(0);host.Pump();}}
/** @summary Native fixture-only producer; not a supported production descriptor. @param effects Actual effect count. @return Reviewed native descriptor. */
SemanticMethod Echo(unsigned& effects){return {"fixture.echo","test.echo@1","script","runtime","fixture-only","echo-confirmed",1024,[&](const copied::Envelope& input,std::span<char> output,std::size_t& size){++effects;const auto wire=input.Encode();if(wire.size()>output.size())return false;std::memcpy(output.data(),wire.data(),wire.size());size=wire.size();return true;}};}
/** @summary Execute both canonical scripts and exact runtime shapes with original native bindings. @param package Base root. @param content Verified canonical bytes. */
void Runtime(const std::filesystem::path& package,const Content& content){
    // @summary Real Base is the first runtime constructed; no synthetic warmup or constructor retry.
    auto bridge=std::make_shared<SemanticBridge>();unsigned effects{},uncertainEffects{};auto uncertain=Echo(uncertainEffects);uncertain.id="fixture.uncertain";uncertain.execute=[&](const copied::Envelope&,std::span<char>,std::size_t&)->bool{++uncertainEffects;throw std::runtime_error("fixture effect became uncertain");};SemanticBridgeTestAccess::Configure(*bridge,{Echo(effects),std::move(uncertain)});SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::Runtime,1,1);SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,7,3);
    BaseHost host(package,Pin(package),{},bridge);Check(host.Ready(),"actual Base qualified");
    ScriptDefinition plain;plain.entry="scripts/init.luau";plain.language="luau";plain.budgetMs=20;plain.memoryBytes=8U*1024U*1024U;plain.bytecode=CompileLuauSource(content.bytes[3]);++vmConstructions;{ScriptRuntime unbound("fixture.sdk.unavailable",plain);unbound.Commit();Marker(unbound,"BASE_API_EXAMPLE_UNAVAILABLE");}
    auto scope=Scope();auto example=Mod(host,scope,content.bytes[3]);Turns(host,*example);Marker(*example,"BASE_API_EXAMPLE_COMPLETE");example->Stop();
    auto tasks=Mod(host,Scope(),content.bytes[4]);Turns(host,*tasks);Marker(*tasks,"BASE_TASK_PROFILE_COMPLETE");Check(effects==1,"canonical cancellation leaves exactly one effect");tasks->Stop();
    constexpr auto shapes=R"lua(
        assert(table.isfrozen(base) and table.isfrozen(base.values))
        local keys={values=true,request=true,run=true,snapshot=true,subscribe=true,descriptors=true}
        local count=0 for key in pairs(base) do assert(keys[key]);count+=1 end assert(count==6)
        assert(not pcall(function() base.extra=true end))
        local bad,reason=base.values.validate('{"v":1,"schema":"test.echo@1","value":["u64","18446744073709551616"]}','test.echo@1') assert(bad==nil and reason=='Invalid')
        assert(not pcall(function() base.values.wire({}) end))
        local wire='{"v":1,"schema":"test.echo@1","value":["i64","-9223372036854775808"]}'
        local value=assert(base.values.validate(wire,'test.echo@1')) assert(base.values.wire(value)==wire)
        local once=false return {onTick=function()
            if once then return end once=true
            local snapshot=assert(base.snapshot()) assert(table.isfrozen(snapshot))
            for _,key in ipairs({'baseEpoch','baseGeneration','generation','runtimeEpoch','revision','sequence','monotonicNs'}) do assert(type(snapshot[key])=='string' and string.match(snapshot[key],'^%d+$')) end
            for _,context in pairs(snapshot.contexts) do assert(table.isfrozen(context));for _,key in ipairs({'slot','generation','epoch'}) do assert(type(context[key])=='string') end end
            local catalog=assert(base.descriptors()) assert(table.isfrozen(catalog) and #catalog==2)
            local seen={} for _,descriptor in ipairs(catalog) do assert(table.isfrozen(descriptor) and descriptor.kind=='task' and descriptor.inputSchema=='test.echo@1');seen[descriptor.id]=true end assert(seen['fixture.echo'] and seen['fixture.uncertain'])
            print('SDK_SHAPES_COMPLETE')
        end}
    )lua";
    auto shape=Mod(host,Scope(),shapes);Turns(host,*shape);Marker(*shape,"SDK_SHAPES_COMPLETE");shape->Stop();
    constexpr auto late=R"lua(
        local task,phase,completed,failed,late= nil,0,0,0,false
        return {onTick=function()
            if phase==0 then phase=1 local value=assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["null"]}','test.echo@1')) task=assert(base.request('fixture.echo',value))
                assert(task.onCompleted(function(outcome) assert(outcome.state=='succeeded');completed+=1 end)==true)
                assert(task.onFailed(function() failed+=1 end)==true)
                local raw=coroutine.create(function() task.await() end) assert(not coroutine.resume(raw))
            elseif phase==1 and completed==1 then phase=2 assert(failed==0 and task.cancel()=='Busy') assert(task.onCompleted(function()late=true end)==true) assert(not late)
            elseif phase==2 and late then phase=3 assert(completed==1 and failed==0) print('SDK_LATE_COMPLETE') end
        end}
    )lua";
    auto callbacks=Mod(host,Scope(),late);Turns(host,*callbacks);Marker(*callbacks,"SDK_LATE_COMPLETE");Check(effects==2,"late callbacks never replay native effect");callbacks->Stop();
    auto uncertainMod=Mod(host,Scope(),R"lua(
        local task,done=nil,false return {onTick=function()
            if not task then local value=assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["null"]}','test.echo@1')) task=assert(base.request('fixture.uncertain',value))
            elseif not done then local outcome=task.poll() if outcome.state=='indeterminate' then assert(outcome.committed and outcome.error=='Indeterminate' and task.cancel()=='Busy');done=true;print('SDK_UNCERTAIN_COMPLETE') end end
        end}
    )lua");Turns(host,*uncertainMod);Marker(*uncertainMod,"SDK_UNCERTAIN_COMPLETE");Check(uncertainEffects==1,"uncertain native effect not replayed");uncertainMod->Stop();
    auto stream=Mod(host,Scope(),R"lua(
        local subscription,started,events,done=nil,false,{},false
        return {onTick=function()
            if not started then started=true subscription=assert(base.subscribe(function(record)
                assert(record);table.insert(events,record.contexts.city.slot)
            end));assert(#events==0)
            elseif #events==3 and not done then assert(table.concat(events,',')=='7,10,11');assert(subscription.unsubscribe()=='Ok');done=true;print('SDK_STREAM_COMPLETE') end
        end}
    )lua");stream->Tick(0);SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,10,1);SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,11,1);Turns(host,*stream);Marker(*stream,"SDK_STREAM_COMPLETE");stream->Stop();
    auto busy=Mod(host,Scope(),R"lua(
        local subscription,started,calls=nil,false,0 return {onTick=function()
            if not started then started=true subscription=assert(base.subscribe(function(record)assert(record);calls+=1;assert(subscription.unsubscribe()=='Busy')end))
            elseif calls==1 then assert(subscription.unsubscribe()=='Stale');print('SDK_UNSUBSCRIBE_COMPLETE') end
        end}
    )lua");Turns(host,*busy);Marker(*busy,"SDK_UNSUBSCRIBE_COMPLETE");busy->Stop();
    auto overflow=Mod(host,Scope(),R"lua(
        local started,faulted=false,false return {onTick=function()
            if not started then started=true assert(base.subscribe(function(record,reason)assert(record==nil and reason=='MailboxOverflow' and not faulted);faulted=true end))
            elseif faulted then print('SDK_OVERFLOW_COMPLETE') end
        end}
    )lua");overflow->Tick(0);for(unsigned i=0;i!=129;++i)SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,11,1);overflow->Tick(0);Marker(*overflow,"SDK_OVERFLOW_COMPLETE");overflow->Stop();
    auto aba=Mod(host,Scope(),R"lua(
        local value=assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["ref","city","11","1"]}','test.echo@1')) local done=false
        return {onTick=function()if not done then done=true local task,reason=base.request('fixture.echo',value);assert(task==nil and reason=='Stale');print('SDK_ABA_COMPLETE') end end}
    )lua");SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,11,1);aba->Tick(0);Marker(*aba,"SDK_ABA_COMPLETE");Check(effects==2,"stale reference did not rebind effect");aba->Stop();
    auto stoppedScope=Scope();auto stopped=Mod(host,stoppedScope,R"lua(
        local started=false return {onTick=function()if started then return end started=true
            local value=assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["null"]}','test.echo@1')) local task=assert(base.request('fixture.echo',value))
            assert(task.onCompleted(function()print('SDK_FORBIDDEN_CALLBACK')end))
            assert(base.run(function()print('SDK_AWAIT_ENTERED');task.await();print('SDK_FORBIDDEN_RESUME')end))
        end}
    )lua");stopped->Tick(0);stopped->Tick(0);Marker(*stopped,"SDK_AWAIT_ENTERED");stopped->Stop();host.Pump();stopped->Tick(0);const auto stoppedLogs=stopped->DrainLogs();Check(std::none_of(stoppedLogs.begin(),stoppedLogs.end(),[](const auto& line){return line.find("SDK_FORBIDDEN_")!=std::string::npos;}),"stopped original generation resumed");Check(effects==2,"stopped queued task had no effect");stopped.reset();Check(bridge->Values()->Used(stoppedScope->Current().generation)==0,"stopped generation value ownership released");
    auto nativeScope=Scope();auto binding=host.Bind(nativeScope);auto endpoint=binding.semantic;const auto grant=RuntimeScopeTestAccess::Issue(*nativeScope);const auto original=nativeScope->Current();Check(grant.owner!=0&&grant.owner==original.generation&&grant.epoch==original.epoch,"native grant matches actual original scope");SemanticBridgeTestAccess::Issue(*endpoint,grant);ScriptDefinition nativeDefinition;nativeDefinition.entry="scripts/init.luau";nativeDefinition.language="luau";nativeDefinition.budgetMs=20;nativeDefinition.memoryBytes=2U*1024U*1024U;nativeDefinition.bytecode=CompileLuauSource("return {}");++vmConstructions;ScriptRuntime native("fixture.sdk.native-owner",nativeDefinition,std::move(binding));native.Commit();
    copied::Envelope value;Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["null"]})","test.echo@1",endpoint->storageOwner,value)==copied::Status::Ok,"native exact owner-bound copied value");TaskHandle task;SemanticContextLease lease;Check(bridge->Request(endpoint,"fixture.echo",value,task,lease)==ScopeResult::Ok,"original native task accepted");SemanticInvocation invocation;Check(bridge->Take(invocation),"original native queued task extracted");Check(bridge->Dispatch(invocation,"fixture.echo",value)==ScopeResult::Ok&&effects==3,"original native effect committed once");TaskSnapshot outcome;auto foreign=host.Bind(Scope());const auto foreignResult=foreign.semantic->tasks->Poll(task,outcome);Check(foreignResult==ScopeResult::Denied&&!outcome.value,"foreign owner cannot disclose native result");Check(bridge->Dispatch(invocation,"fixture.echo",value)!=ScopeResult::Ok&&effects==3,"same task cannot replay");const auto revoked=RuntimeScopeTestAccess::Revoke(*nativeScope,grant.grantGeneration);Check(revoked==ScopeResult::Ok,"actual native revoke completed before denial assertion");Check(endpoint->tasks->Poll(task,outcome)==ScopeResult::Denied&&!outcome.value,"postcommit revocation hides result");Check(endpoint->tasks->Release(task)==ScopeResult::Ok,"revoked owner may release its exact task");native.Stop();
}
}
/** @summary Run exact source-owned profile without production installation. @param argc Three roots required. @param argv Actual Base/SDK/Examples roots. @return Zero after executed assertions pass. */
int main(int argc,char** argv){try{Check(argc==4,"actual Base SDK Examples roots required");const auto lock=std::filesystem::path(__FILE__).parent_path()/"base_sdk_profile.lock.toml";const auto metadata=ReadBounded(lock,16384);const auto identity=ParseIdentity(metadata);const auto content=Capture(std::filesystem::absolute(argv[2]),std::filesystem::absolute(argv[3]),identity);const std::array<std::pair<std::string,std::string>,3> negatives{{{"schemaVersion = 1","schemaVersion = 2"},{"sdkPackageVersion = \"0.2.0\"","sdkPackageVersion = \"0.3.0\""},{"profile = \"base-luau-alpha-v1\"","profile = \"other\""}}};for(const auto& [before,after]:negatives){auto changed=metadata;auto position=changed.find(before);Check(position!=std::string::npos,"negative metadata control found");changed.replace(position,before.size(),after);Reject([&]{static_cast<void>(ParseIdentity(changed));},"profile version/schema mismatch","valid TOML wrong metadata admitted");}TransportNegatives(identity,content);Grammar(content.bytes[1]);Runtime(std::filesystem::absolute(argv[1]),content);std::cout<<checks<<" SDK profile checks passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
