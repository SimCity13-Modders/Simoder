#include "mods/manager_publication.hpp"
#include "mods/manager_commands.hpp"
#include "mods/manager_close.hpp"
#include "mods/mod_state_store.hpp"
#include "mods/mod_definition_parser.hpp"
#include "formats/toon/toon_document.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include "mod_fixture.hpp"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <vector>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <malloc.h>

namespace {
std::atomic_size_t fenceAllocations{},fenceFrees{},fenceCallbacks{};
thread_local const sc13::runtime::RuntimeCommitDomain* failAfterDomain{};
thread_local std::uint64_t failAfterRevision{};
thread_local bool actualPostmarkerFault{},faultInFence{};
}
/** @summary Observe actual native allocations and optionally fail the first allocation after a real marker. */
void BeforeAllocation(){
    if(failAfterDomain&&failAfterDomain->Revision()>=failAfterRevision){failAfterDomain=nullptr;actualPostmarkerFault=true;faultInFence=sc13::scripting::SchedulerPublicationTestAccess::InsideFence();throw std::bad_alloc();}
    if(sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceAllocations;
}
/** @summary Allocate observed scalar storage. @param size Actual bytes. @return Owned allocation. */
void* operator new(std::size_t size){BeforeAllocation();if(auto p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
/** @summary Allocate observed array storage. @param size Actual bytes. @return Owned allocation. */
void* operator new[](std::size_t size){return ::operator new(size);}
/** @summary Release observed scalar storage. @param p Original allocation. */
void operator delete(void* p)noexcept{if(p&&sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceFrees;std::free(p);}
/** @summary Release array storage. @param p Original allocation. */
void operator delete[](void* p)noexcept{::operator delete(p);}
/** @summary Release sized scalar storage. @param p Original allocation. @param size Unused hint. */
void operator delete(void* p,std::size_t size)noexcept{static_cast<void>(size);::operator delete(p);}
/** @summary Release sized array storage. @param p Original allocation. @param size Unused hint. */
void operator delete[](void* p,std::size_t size)noexcept{static_cast<void>(size);::operator delete(p);}
/** @summary Observe aligned native storage. @param size Actual bytes. @param alignment Required alignment. @return Owned allocation. */
void* operator new(std::size_t size,std::align_val_t alignment){BeforeAllocation();if(auto p=_aligned_malloc(size?size:1,static_cast<std::size_t>(alignment)))return p;throw std::bad_alloc();}
/** @summary Observe aligned array storage. @param size Actual bytes. @param alignment Required alignment. @return Owned allocation. */
void* operator new[](std::size_t size,std::align_val_t alignment){return ::operator new(size,alignment);}
/** @summary Release aligned storage. @param p Original allocation. @param alignment Original alignment. */
void operator delete(void* p,std::align_val_t alignment)noexcept{static_cast<void>(alignment);if(p&&sc13::scripting::SchedulerPublicationTestAccess::InsideFence())++fenceFrees;_aligned_free(p);}
/** @summary Release aligned array. @param p Original allocation. @param alignment Original alignment. */
void operator delete[](void* p,std::align_val_t alignment)noexcept{::operator delete(p,alignment);}
/** @summary Release sized aligned scalar. @param p Original allocation. @param size Unused hint. @param alignment Original alignment. */
void operator delete(void* p,std::size_t size,std::align_val_t alignment)noexcept{static_cast<void>(size);::operator delete(p,alignment);}
/** @summary Release sized aligned array. @param p Original allocation. @param size Unused hint. @param alignment Original alignment. */
void operator delete[](void* p,std::size_t size,std::align_val_t alignment)noexcept{static_cast<void>(size);::operator delete(p,alignment);}

namespace {
using namespace sc13::mods;
using namespace sc13::runtime;
using namespace sc13::scripting;
using namespace std::chrono_literals;
using Access=ManagerPublicationTestAccess;
using Preparation=ManagerPreparationTestAccess;
using Scheduler=SchedulerPublicationTestAccess;
using Close=ManagerCloseTestAccess;
using W=StatePathWriter;
using Writer=StatePathWriterTestAccess;
using Memory=Access::MemoryDisposition;
std::atomic_uint checks{};
const sc13::core::Tgi target{0x00B1B104,0x61EFC000,0x719436BD};
/** @summary Require an actual native fact. @param value Observed predicate. @param message Fixed diagnostic. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Require a bounded actual condition without a second predicate call. @param predicate Original observation. @param message Failure. */
template<class F>void Await(F predicate,const char* message){bool done{};const auto end=std::chrono::steady_clock::now()+5s;while(!(done=predicate())&&std::chrono::steady_clock::now()<end)std::this_thread::yield();Check(done,message);}
/** @summary Own only one test-created native handle. */
struct Handle final{HANDLE value{INVALID_HANDLE_VALUE};
    /** @summary Take original native ownership. @param input Real handle. */
    explicit Handle(HANDLE input=INVALID_HANDLE_VALUE):value(input){}
    /** @summary Close only the original handle. */
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;
};
/** @summary Validate absolute plain ancestors before fixture effects. @param path Known native target. @param directory Required final kind. */
void Plain(const std::filesystem::path& path,bool directory=true){Check(path.is_absolute()&&path==path.lexically_normal(),"plain absolute normalized path");auto current=path.root_path();for(const auto& part:path.relative_path()){Check(part!=L"."&&part!=L"..","bounded path component");current/=part;const auto flags=GetFileAttributesW(current.c_str());Check(flags!=INVALID_FILE_ATTRIBUTES&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT),"plain existing native ancestor");Check(current==path&&!directory?!(flags&FILE_ATTRIBUTE_DIRECTORY):bool(flags&FILE_ATTRIBUTE_DIRECTORY),"actual native entry kind");}}
/** @summary Restrict every file effect to original plain fixture descendants. @param root Original owned root. @param path Fixed child. */
void Child(const std::filesystem::path& root,const std::filesystem::path& path){const auto relative=path.lexically_relative(root);Check(path.is_absolute()&&path==path.lexically_normal()&&!relative.empty()&&!relative.is_absolute(),"native descendant path");for(const auto& part:relative)Check(part!=L"."&&part!=L"..","native child cannot escape");Plain(root);Plain(path.parent_path());}
/** @summary Exclusively create a plain test directory. @param parent Qualified parent. @param leaf Fixed child name. @return Original owned child. */
std::filesystem::path Directory(const std::filesystem::path& parent,const std::wstring& leaf){Plain(parent);Check(!leaf.empty()&&leaf.find_first_of(L"/\\:")==std::wstring::npos,"single fixed child");auto path=parent/leaf;Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"exclusive native directory");Plain(path);return path;}
/** @summary Read stable complete default bytes while honoring writer DELETE sharing. @param root Owned root. @param path Known file. @return Actual bytes. */
std::string Read(const std::filesystem::path& root,const std::filesystem::path& path){Child(root,path);Plain(path,false);Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));Check(file.value!=INVALID_HANDLE_VALUE,"real file read handle");LARGE_INTEGER size{};Check(GetFileSizeEx(file.value,&size)&&size.QuadPart>=0&&size.QuadPart<=1024*1024,"bounded file bytes");std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');DWORD count{};Check(ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size(),"full default stream read");char extra{};Check(ReadFile(file.value,&extra,1,&count,nullptr)&&count==0,"exact read EOF");return bytes;}
/** @summary Write only an owned existing/absent plain native file. @param root Original root. @param path Fixed child. @param bytes Exact contents. */
void Write(const std::filesystem::path& root,const std::filesystem::path& path,std::string_view bytes){Child(root,path);const auto flags=GetFileAttributesW(path.c_str());if(flags!=INVALID_FILE_ATTRIBUTES)Plain(path,false);else Check(GetLastError()==ERROR_FILE_NOT_FOUND,"native target absence");Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));Check(file.value!=INVALID_HANDLE_VALUE,"real file write handle");DWORD count{};Check(WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(file.value),"full flushed bytes");}
/** @summary Observe actual absence only; unavailable never becomes absent. @param path Fixed child. @return True only for FILE_NOT_FOUND. */
bool Absent(const std::filesystem::path& path){const auto flags=GetFileAttributesW(path.c_str());if(flags!=INVALID_FILE_ATTRIBUTES)return false;Check(GetLastError()==ERROR_FILE_NOT_FOUND,"positive native absence");return true;}
/** @summary Copy actual native file identity without retaining a sharing handle. @param root Original owned root. @param path Existing plain file. @return Real volume/file identity. */
W::FileIdentity Identity(const std::filesystem::path& root,const std::filesystem::path& path){Child(root,path);Plain(path,false);Handle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));Check(file.value!=INVALID_HANDLE_VALUE,"real native identity handle");FILE_ID_INFO info{};Check(GetFileInformationByHandleEx(file.value,FileIdInfo,&info,sizeof(info))!=FALSE,"actual volume and file identity");W::FileIdentity result;result.volume=info.VolumeSerialNumber;std::memcpy(result.file.data(),info.FileId.Identifier,16);result.valid=true;return result;}
/** @summary Snapshot actual present request bytes and native identity without retaining an open handle. */
struct RequestedFile final{std::string bytes;W::FileIdentity identity;};
/** @summary Capture the real initialized beforestate. @param root Original owner. @param path Fixed existing child. @return Complete native facts. */
RequestedFile BeforeFile(const std::filesystem::path& root,const std::filesystem::path& path){return {Read(root,path),Identity(root,path)};}
/** @summary Require zero-effect denial to preserve both actual bytes and native identity. @param root Original owner. @param path Fixed child. @param before Genuine baseline. */
void UnchangedFile(const std::filesystem::path& root,const std::filesystem::path& path,const RequestedFile& before){Check(Read(root,path)==before.bytes&&Identity(root,path)==before.identity,"no-effect denial preserves original native bytes and file identity");}
/** @summary Establish genuine absence only for this fresh initialized empty test-owned leaf. @param root Exclusive original case. @param path Fixed state child. */
void EmptyFileToAbsent(const std::filesystem::path& root,const std::filesystem::path& path){Check(path==root/"state.toon","absence setup accepts only the fixed owned state leaf");Child(root,path);Plain(path,false);const auto before=BeforeFile(root,path);std::vector<ModId> ids;std::string error;Check(LoadEnabledState(path,ids,error)&&ids.empty()&&!before.bytes.empty(),"owned initialized empty request qualified before absence setup");Check(Identity(root,path)==before.identity,"exact original setup identity preserved before native deletion");Check(DeleteFileW(path.c_str())!=FALSE,"explicit native absence setup removes only owned initialized empty leaf");Check(Absent(path),"native setup establishes genuine FILE_NOT_FOUND");}
/** @summary Retain isolated native evidence roots, including failures. */
struct Root final{std::filesystem::path path;unsigned serial{};
    /** @summary Qualify supplied evidence root before effects. @param parent Root-owned existing analysis directory. */
    explicit Root(const std::filesystem::path& parent){Plain(parent);bool analysis{};for(const auto& part:parent)if(part==L".analysis")analysis=true;Check(analysis,"explicit analysis parent required");wchar_t fs[32]{};Check(GetDriveTypeW(parent.root_path().c_str())==DRIVE_FIXED&&GetVolumeInformationW(parent.root_path().c_str(),nullptr,0,nullptr,nullptr,nullptr,fs,32)&&std::wstring(fs)==L"NTFS","actual fixed NTFS profile");path=Directory(parent,L"manager-persistence-"+std::to_wstring(GetCurrentProcessId()));std::cout<<"retained persistence fixture="<<path.string()<<std::endl;}
    /** @summary Allocate one fresh isolated case. @return Plain directory. */
    std::filesystem::path Case(){return Directory(path,L"case-"+std::to_wstring(++serial));}
};
/** @summary Pin real executable Base input bytes. @param root Verified checkout. @return Exact package pin. */
BasePackagePin Pin(const std::filesystem::path& root){BasePackagePin pin{"0.1.0-alpha",{}};pin.files.emplace("mod.toml",HashText(ReadBounded(root/"mod.toml",4096)));for(const auto& item:std::filesystem::recursive_directory_iterator(root/"scripts"))if(item.is_regular_file())pin.files.emplace(item.path().lexically_relative(root).generic_string(),HashText(ReadBounded(item.path(),1024*1024)));return pin;}
/** @summary One fixed per-attempt record; retaining it never retains a VM, grant, handle or script pointer. */
struct EntryDiagnostic final {
    sc13::scripting::CallbackBudgetTestAccess::Observation observation;
    std::uint64_t make{},attempt{};
    bool requested{},printed{},wrongLane{},wrongGeneration{},busy{},reused{};
};
std::atomic_uint64_t nextMakeOrdinal{};
/** @summary Allocate a diagnostic ordinal without wrapping or changing any preparation attempt.
 * @return Unique nonzero Make ordinal. @throws std::runtime_error Before preparation if the scalar space is exhausted. */
std::uint64_t NextEntryMakeOrdinal(){
    auto previous=nextMakeOrdinal.load();
    for(;;){
        if(previous==~std::uint64_t{})throw std::runtime_error("persistence diagnostic Make ordinal exhausted");
        if(nextMakeOrdinal.compare_exchange_strong(previous,previous+1))return previous+1;
    }
}
/** @summary Emit fixed stages only outside native guards and protected timers. @param make Actual Make ordinal. @param attempt Existing attempt ordinal. @param stage Fixed stage. */
void EntryProgress(std::uint64_t make,std::uint64_t attempt,const char* stage) noexcept{
    try{std::cerr<<"persistence preparation make="<<make<<" attempt="<<attempt<<" stage="<<stage<<std::endl;}catch(...){}
}
/** @summary Print one immutable joined attempt; diagnostic failures never replace the original native exception.
 * @param entry Actual attempt record retained by its original observer. @param disposition Fixed native return category. */
void ReportEntry(EntryDiagnostic& entry,const char* disposition) noexcept{
    if(entry.printed)return;entry.printed=true;
    try{
        const auto& value=entry.observation;
        std::cerr<<"owner entry diagnostic v=1 make="<<entry.make<<" attempt="<<entry.attempt<<" disposition="<<disposition
            <<" requested="<<entry.requested<<" wrongLane="<<entry.wrongLane<<" wrongGeneration="<<entry.wrongGeneration
            <<" busy="<<entry.busy<<" reused="<<entry.reused<<" arm="<<static_cast<unsigned>(value.arm)
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
    }catch(...){/* @summary Preserve the original test and callback outcome when reporting fails. */}
}
/** @summary Retain actual candidate constituents independently from its one-shot canonical plan. */
struct Candidate final{std::shared_ptr<EntryDiagnostic> diagnostic;ManagerPreparation::Plan preparation;ManagerPublication::Plan plan;std::shared_ptr<RuntimeScope> scope;std::shared_ptr<SemanticEndpoint> endpoint;std::shared_ptr<ScriptRuntime> vm;OperationContext root;SemanticContextLease facts;};
/** @summary Own genuine isolated manager/cache/Base and preserve all native fixture files. */
struct Fixture final{
    std::filesystem::path root,state;PatchRegistry registry;RuntimeResourceCache cache;std::shared_ptr<RuntimeCommitDomain> domain=std::make_shared<RuntimeCommitDomain>();std::shared_ptr<SemanticBridge> bridge=std::make_shared<SemanticBridge>();BasePackagePin pin;std::shared_ptr<BaseHost> base;std::unique_ptr<ModManager> manager;
    /** @summary Construct a real native manager and exact pinned interpreter. @param cases Owned case allocator. @param package Verified Base. @param shared Existing exact singleton when supplied. @param sharedBridge Its exact bridge. @param sharedState Existing test-owned destination for same-path foreign-State proof. */
    Fixture(Root& cases,const std::filesystem::path& package,std::shared_ptr<BaseHost> shared={},std::shared_ptr<SemanticBridge> sharedBridge={},std::filesystem::path sharedState={}):root(cases.Case()),state(sharedState.empty()?root/"state.toon":std::move(sharedState)),pin(Pin(package)){
        Check(bool(shared)==bool(sharedBridge),"paired shared singleton");if(sharedBridge)bridge=std::move(sharedBridge);else{SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::Runtime,1,1);SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,7,3);}base=shared?std::move(shared):std::make_shared<BaseHost>(package,pin,std::function<void(ScriptPhase,std::uint64_t)>{},bridge);Check(base->Ready(),"real Base ready");Directory(root,L"mods");Directory(root/"mods",L"fixture.owner");Directory(root/"mods"/"fixture.owner",L"scripts");Source(1);manager=std::make_unique<ModManager>(root/"mods",state,registry,cache,RuntimeResourceRefreshCallback{},ScriptScheduler::LogSink{},nullptr,domain);std::string error;Check(manager->Initialize(error)&&Access::Enroll(*manager)==ScopeResult::Ok,"actual isolated scheduler enrollment");
    }
    /** @summary Join borrowers before cache and registry ownership ends; preserve files. */
    ~Fixture(){Writer::Hook({});manager.reset();base.reset();}
    /** @summary Write complete authored fixture input. @param value Requested scalar. */
    void Source(float value){sc13::tests::WriteTextFile(root/"mods"/"fixture.owner"/"mod.toml",sc13::tests::FixtureHeader("fixture.owner","Persistence")+"[scripts]\nlanguage='luau'\nentry='scripts/init.luau'\nmemoryMiB=2\nbudgetMs=20\n"+sc13::tests::FixturePatch(3));sc13::tests::WriteTextFile(root/"mods"/"fixture.owner"/"scripts"/"init.luau","simoder.props.setFloat('00B1B104:61EFC000:719436BD','09AE19D7',"+std::to_string(value)+")\nreturn {onTick=function() end,onDisable=function() end}");}
    /** @summary Capture genuine City facts without granting permission. @param candidate Original endpoint. */
    void Facts(Candidate& candidate){copied::Envelope envelope;Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]})","test.echo@1",candidate.endpoint->storageOwner,envelope)==copied::Status::Ok&&bridge->BindReferences(*candidate.endpoint,envelope,candidate.facts)==ScopeResult::Ok,"original factual lease");}
    /** @summary Prepare exact input bytes with bounded existing Busy handling. @param value Scalar. @param observer Native lifecycle witness. @return Genuine candidate. */
    Candidate Make(float value,std::function<void(ScriptPhase,std::uint64_t)> observer={}){
        using Budget=sc13::scripting::CallbackBudgetTestAccess;
        Source(value);Candidate candidate;const auto make=NextEntryMakeOrdinal();std::uint64_t attempt{};
        std::shared_ptr<const CapturedCandidate> capture;EntryProgress(make,0,"Capture");
        Await([&]{try{capture=Preparation::Capture(*manager,"fixture.owner",root/"mods"/"fixture.owner",pin);return true;}catch(const std::runtime_error& error){if(std::string_view(error.what())!="preparation.busy")throw;return false;}},"bounded original capture");
        Await([&]{
            if(attempt==~std::uint64_t{})throw std::runtime_error("persistence diagnostic attempt ordinal exhausted");
            auto entry=std::make_shared<EntryDiagnostic>();entry->make=make;entry->attempt=++attempt;
            Budget::Observation wrongLane;
            entry->wrongLane=Budget::ArmPreparationEntry(1,wrongLane)==Budget::ArmResult::WrongLane&&wrongLane.borrowCleared;
            EntryProgress(make,attempt,"Candidate");
            const auto observed=[entry,observer](ScriptPhase phase,std::uint64_t generation){
                if(phase==ScriptPhase::Close)static_cast<void>(Budget::Cancel(generation));
                if(phase==ScriptPhase::Entry&&!entry->requested){
                    entry->requested=true;Budget::Observation wrongGeneration;
                    entry->wrongGeneration=Budget::ArmPreparationEntry(generation+1,wrongGeneration)==Budget::ArmResult::InvalidGeneration&&wrongGeneration.borrowCleared;
                    static_cast<void>(Budget::ArmPreparationEntry(generation,entry->observation));
                    Budget::Observation busy;
                    entry->busy=Budget::ArmPreparationEntry(generation,busy)==Budget::ArmResult::Busy&&busy.borrowCleared;
                    entry->reused=Budget::ArmPreparationEntry(generation,entry->observation)==Budget::ArmResult::AlreadyUsed&&
                        entry->observation.arm==Budget::ArmResult::Armed&&entry->observation.generation==generation&&!entry->observation.borrowCleared;
                }
                if(observer)observer(phase,generation);
            };
            try{candidate.preparation=Access::Candidate(*manager,capture,base,observed);}
            catch(const std::runtime_error& error){const bool busy=std::string_view(error.what())=="preparation.busy";ReportEntry(*entry,busy?"Busy":"RuntimeFailure");if(!busy)throw;return false;}
            catch(...){ReportEntry(*entry,"NativeFailure");throw;}
            candidate.diagnostic=entry;ReportEntry(*entry,"Success");EntryProgress(make,attempt,"Completed");
            Check(entry->wrongLane&&entry->wrongGeneration&&entry->busy&&entry->reused&&entry->requested&&
                entry->observation.arm==Budget::ArmResult::Armed&&entry->observation.terminal&&entry->observation.borrowCleared&&
                entry->observation.phase==ScriptPhase::Entry&&entry->observation.completion==Budget::Completion::LuaReturn,
                "authentic per-attempt Entry diagnostic clears its borrow and rejects invalid arms");
            return true;
        },"bounded original VM preparation");
        candidate.scope=Preparation::Scope(candidate.preparation);candidate.endpoint=Preparation::Endpoint(candidate.preparation);candidate.vm=Preparation::Vm(candidate.preparation);candidate.scope->SetReadiness(true);candidate.root=RuntimeScopeTestAccess::Issue(*candidate.scope);Check(candidate.root.owner!=0,"existing synthetic reviewed root");Facts(candidate);return candidate;
    }
    /** @summary Bind original typed lifecycle reservations without raw unowned refresh. @param old Current predecessor or absent. @param next Exact successor. */
    void Prepare(Candidate* old,Candidate& next){LifecycleDecision previous,current;Check(LifecycleDecisionTestAccess::RootDecision(next.scope,next.root,current)==ScopeResult::Ok,"typed candidate governing root");if(old){Check(LifecycleDecisionTestAccess::FreshDecision(old->scope,old->root,previous)==ScopeResult::Ok,"tracked predecessor fresh owner");Facts(*old);}std::string error;Check(Access::Prepare(*manager,std::move(next.preparation),previous,current,old?old->facts:SemanticContextLease{},next.facts,next.plan,error)==ScopeResult::Ok,"canonical typed plan prepared");}
};
/** @summary Require exact caller Plan identity preservation after rejected admission. @param before Original facts. @param after Later facts. */
template<class Facts>void SamePlan(const Facts& before,const Facts& after){const auto same=[](const auto& a,const auto& b){return !a.owner_before(b)&&!b.owner_before(a);};Check(same(before.state,after.state)&&same(before.stamp,after.stamp)&&same(before.joint,after.joint)&&same(before.node,after.node)&&before.consumed==after.consumed&&before.jointPrepared==after.jointPrepared&&before.schedulerPrepared==after.schedulerPrepared&&before.absent==after.absent&&before.purpose==after.purpose,"exact input plan preserved before effects");}
/** @summary Clear the TLS native hook on every exit. */
struct Hook final{
    /** @summary Install only genuine writer cuts. @param callback Native passive callback. */
    explicit Hook(std::function<Writer::CutAction(const Writer::CutObservation&)> callback){Writer::Hook([callback=std::move(callback)](const Writer::CutObservation& cut){if(Scheduler::InsideFence())++fenceCallbacks;return callback(cut);});}
    /** @summary Prevent callbacks borrowing dead fixture locals. */
    ~Hook(){Writer::Hook({});}
};
/** @summary Require a genuine post-file rejected memory attempt and no marker. @param value Actual observation. @param result Original memory outcome. */
void Rejected(const Access::FileObservation& value,ScopeResult result){Check(value.admitted&&value.memoryAttempts==1&&value.memory==Memory::RejectedBeforeMarker&&!value.crossed&&value.revision==0&&value.memoryResult==result,"one precise pre-marker memory rejection");Check(value.disk.publicationConfirmed&&value.compensationRequested,"confirmed original intent authorized immediate compensation");}
/** @summary Validate the single compatible native byte snapshot through the shared TOON parser and current enabled-state rules.
 * @param fixture Original native file owner; retained Unknown handles remain untouched. @param ids Exact requested order. */
void DiskOrder(Fixture& fixture,const std::vector<ModId>& ids){
    const auto bytes=Read(fixture.root,fixture.state);sc13::formats::toon::Document document;sc13::formats::toon::ParseError error;
    Check(sc13::formats::toon::Parse(bytes,document,error),"captured requested bytes pass the actual shared TOON parser");
    const auto field=document.root().find("enabled");Check(field!=document.root().end()&&field->second.AsArray()!=nullptr,"captured requested enabled field is an array");
    std::set<ModId,std::less<>> unique;std::vector<ModId> actual;
    for(const auto& value:*field->second.AsArray()){const auto* id=value.AsString();Check(id!=nullptr&&IsValidModId(*id)&&unique.insert(*id).second,"captured requested IDs are valid unique strings");actual.push_back(*id);}
    Check(actual.size()==ids.size()&&actual==ids&&!bytes.empty(),"real exact requested intent order");
}
/** @summary Qualify actual initial publication, removal and consumed absent removal separately. @param cases Owned root. @param package Verified Base. */
void Positive(Root& cases,const std::filesystem::path& package){
    Fixture fixture(cases,package);auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);auto origin=Access::Origin(candidate.plan);{Access::FileOutcome activation;
    Check(Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),activation)==ScopeResult::Ok,"real persistent activation");const auto published=Access::ObserveFile(activation);
    Check(published.memory==Memory::MarkerCommitted&&published.crossed&&published.revision==1&&published.memoryAttempts==1&&published.disk.publicationConfirmed&&!published.compensationRequested,"actual crossed activation evidence");
    Check(fixture.domain->Revision()==1&&candidate.scope->Current().activationCommitted&&fixture.registry.IsActive("fixture.owner")&&fixture.manager->EnabledOrder()==std::vector<ModId>{"fixture.owner"},"memory participants reflect real activation");DiskOrder(fixture,{"fixture.owner"});}
    auto successor=fixture.Make(9);fixture.Prepare(&candidate,successor);auto nextOrigin=Access::Origin(successor.plan);{Access::FileOutcome handoff;
    Check(Access::AttemptPersistence(*fixture.manager,std::move(successor.plan),handoff)==ScopeResult::Ok,"real persistent successor");const auto handed=Access::ObserveFile(handoff);Check(handed.crossed&&handed.revision==2&&handed.memory==Memory::MarkerCommitted&&handed.disk.compensationCalls==0&&candidate.scope->Current().stopping&&successor.scope->Current().activationCommitted,"one successor marker without disk rollback");DiskOrder(fixture,{"fixture.owner"});}
    ManagerPublication::Plan removal;std::string error;Check(Access::Remove(*fixture.manager,nextOrigin,removal,error)==ScopeResult::Ok,"real removal prepared");{Access::FileOutcome removed;Check(Access::AttemptPersistence(*fixture.manager,std::move(removal),removed)==ScopeResult::Ok,"real persistent removal");const auto inactive=Access::ObserveFile(removed);Check(inactive.crossed&&inactive.revision==3&&inactive.memory==Memory::MarkerCommitted&&fixture.manager->EnabledOrder().empty()&&!fixture.registry.IsActive("fixture.owner"),"exact removal marker and inactive projection");DiskOrder(fixture,{});}
    ManagerPublication::Plan absent;Check(Access::Remove(*fixture.manager,nextOrigin,absent,error)==ScopeResult::Ok,"genuine same tombstone removal");const auto snapshot=fixture.registry.Current();Access::FileOutcome unchanged;Check(Access::AttemptPersistence(*fixture.manager,std::move(absent),unchanged)==ScopeResult::Ok,"confirmed requested inactive no-op");const auto noChange=Access::ObserveFile(unchanged);Check(noChange.memory==Memory::NoChangeConsumed&&!noChange.crossed&&noChange.revision==0&&noChange.memoryAttempts==1&&noChange.disk.publicationConfirmed&&!noChange.compensationRequested&&fixture.domain->Revision()==3&&fixture.registry.Current()==snapshot,"consumed absence is distinct from marker crossing");DiskOrder(fixture,{});
}
/** @summary A genuinely foreign same-ID manager cannot consume a caller plan or touch either file. @param cases Owned root. @param package Verified Base. */
void IdentityAdmission(Root& cases,const std::filesystem::path& package){
    Fixture fixture(cases,package);Fixture foreign(cases,package,fixture.base,fixture.bridge,fixture.state);Check(foreign.state==fixture.state,"genuine distinct managers use exact same configured destination");auto candidate=foreign.Make(1);foreign.Prepare(nullptr,candidate);const auto before=Access::ReadPlanFacts(candidate.plan);const auto registry=foreign.registry.Current();const auto beforeFile=BeforeFile(fixture.root,fixture.state);std::atomic_uint cuts{};
    Hook hook([&](const Writer::CutObservation&){++cuts;return Writer::CutAction::Continue;});Access::FileOutcome output;const auto rejected=Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),output);const auto observed=Access::ObserveFile(output);
    Check(rejected==ScopeResult::Conflict&&observed.memory==Memory::NotAttempted&&observed.memoryAttempts==0&&!observed.admitted&&cuts==0,"foreign exact State rejected before all native effects");UnchangedFile(fixture.root,fixture.state,beforeFile);SamePlan(before,Access::ReadPlanFacts(candidate.plan));Check(foreign.registry.Current()==registry&&foreign.domain->Revision()==0&&fixture.domain->Revision()==0,"foreign/current memory unchanged");
    Access::FileOutcome positive;Check(Access::AttemptPersistence(*foreign.manager,std::move(candidate.plan),positive)==ScopeResult::Ok&&foreign.domain->Revision()==1,"same preserved foreign Plan succeeds only with original manager");const auto confirmedBytes=Read(fixture.root,fixture.state);const auto confirmedIdentity=Identity(fixture.root,fixture.state);const auto confirmedCuts=cuts.load();
    ManagerPublication::Plan empty;const auto emptyBefore=Access::ReadPlanFacts(empty);Access::FileOutcome denied;Check(Access::AttemptPersistence(*fixture.manager,std::move(empty),denied)==ScopeResult::Invalid,"empty structural admission precise denial");SamePlan(emptyBefore,Access::ReadPlanFacts(empty));Check(Access::ObserveFile(denied).memoryAttempts==0&&Read(fixture.root,fixture.state)==confirmedBytes&&Identity(fixture.root,fixture.state)==confirmedIdentity&&cuts.load()==confirmedCuts,"empty plan preserves confirmed native bytes and identity without effects");
    Access::FileOutcome replay;const auto moved=Access::ReadPlanFacts(candidate.plan);Check(Access::AttemptPersistence(*foreign.manager,std::move(candidate.plan),replay)==ScopeResult::Invalid,"moved caller Plan cannot publish again");SamePlan(moved,Access::ReadPlanFacts(candidate.plan));Check(Access::ObserveFile(replay).memoryAttempts==0&&Read(fixture.root,fixture.state)==confirmedBytes&&Identity(fixture.root,fixture.state)==confirmedIdentity&&cuts.load()==confirmedCuts&&fixture.domain->Revision()==0&&foreign.domain->Revision()==1,"moved rejection preserves confirmed shared destination and both domains");
}
/** @summary Actual open script lane and closed manager reject before native publication. @param cases Owned root. @param package Verified Base. */
void LaneAndClosingAdmission(Root& cases,const std::filesystem::path& package){
    Fixture fixture(cases,package);const auto beforeFile=BeforeFile(fixture.root,fixture.state);auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);const auto before=Access::ReadPlanFacts(candidate.plan);Access::FileOutcome laneOutput;ScopeResult lane=ScopeResult::Ok;
    Close::InvokeControl(candidate.vm,[&]{Check(ScriptRuntime::OnScriptLane(),"genuine script-lane invocation");lane=Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),laneOutput);});
    Check(lane==ScopeResult::Denied&&Access::ObserveFile(laneOutput).memoryAttempts==0,"actual lane admission blocks native files");UnchangedFile(fixture.root,fixture.state,beforeFile);SamePlan(before,Access::ReadPlanFacts(candidate.plan));
    Check(ManagerClose::Request(Close::Capture(*fixture.manager))==ScopeResult::Ok,"real close admission");Access::FileOutcome closed;Check(Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),closed)==ScopeResult::Cancelled&&Access::ObserveFile(closed).memoryAttempts==0,"closing manager rejects without file intent");UnchangedFile(fixture.root,fixture.state,beforeFile);SamePlan(before,Access::ReadPlanFacts(candidate.plan));
}
/** @summary A real cache reset denies before any native file intent and leaves caller ownership unchanged. @param cases Owned root. @param package Verified Base. */
void CacheAdmission(Root& cases,const std::filesystem::path& package){Fixture fixture(cases,package);const auto beforeFile=BeforeFile(fixture.root,fixture.state);auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);fixture.cache.Clear();const auto before=Access::ReadPlanFacts(candidate.plan);const auto snapshot=fixture.registry.Current();unsigned nativeCuts{};Hook hook([&](const Writer::CutObservation&){++nativeCuts;return Writer::CutAction::Continue;});Access::FileOutcome output;Check(Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),output)==ScopeResult::NotReady,"actual changed cache witness denies before native intent");SamePlan(before,Access::ReadPlanFacts(candidate.plan));const auto actual=Access::ObserveFile(output);Check(!actual.admitted&&actual.memoryAttempts==0&&nativeCuts==0&&fixture.registry.Current()==snapshot&&fixture.domain->Revision()==0,"cache preadmission preserves disk, memory and Plan");UnchangedFile(fixture.root,fixture.state,beforeFile);}
/** @summary Hold real native participants on a separately joined thread through the one memory attempt. */
struct Contender final{
    std::mutex mutex;std::condition_variable changed;bool ready{},released{};std::future<void> thread;
    /** @summary Signal a genuine acquired guard, then await explicit release. */
    void Held(){std::unique_lock lock(mutex);ready=true;changed.notify_all();changed.wait(lock,[&]{return released;});}
    /** @summary Run exact participant guard code without injecting its result. @param action Guard owner. */
    template<class F>void Start(F action){thread=std::async(std::launch::async,[this,action]{action([this]{Held();});});std::unique_lock lock(mutex);Check(changed.wait_for(lock,5s,[&]{return ready;}),"real native contender acquired");}
    /** @summary Release and join before borrowed fixtures or callbacks disappear. */
    void Join(){ {const std::lock_guard lock(mutex);released=true;changed.notify_all();}if(thread.valid())thread.get();}
    /** @summary Preserve joins on exceptional assertion paths. */
    ~Contender(){Join();}
};
/** @summary After actual confirmed file effects, each native final fence rejects once and compensation restores exact prior bytes or absence. @param cases Owned root. @param package Verified Base. */
void FinalFences(Root& cases,const std::filesystem::path& package){
    for(unsigned guard=0;guard!=8;++guard){std::cerr<<"persistence final fence guard="<<guard<<std::endl;Fixture fixture(cases,package);const bool prior=guard%2!=0;const std::string previous="enabled[0]:\n";if(prior)Write(fixture.root,fixture.state,previous);else{Check(fixture.domain->Revision()==0&&!fixture.registry.IsActive("fixture.owner")&&fixture.manager->EnabledOrder().empty(),"genuine absence setup precedes any active owner");EmptyFileToAbsent(fixture.root,fixture.state);}const auto previousBytes=prior?Read(fixture.root,fixture.state):std::string{};auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);const auto snapshot=fixture.registry.Current();Contender contender;bool cut{};
        Hook hook([&](const Writer::CutObservation& point){if(point.cut==Writer::Cut::AfterPublicationEffect&&point.result.lastEffectKind==W::EffectKind::PublishRename&&point.result.lastNativeReturn==W::NativeReturn::Succeeded&&!cut){cut=true;Check(point.result.publicationCalls==1&&point.owned.valid,"actual single file rename identity");
            contender.Start([&](const auto& held){
                if(guard==0){auto lock=fixture.domain->ReserveWriter();Check(lock.Acquired(),"actual D acquired");held();}
                if(guard==1){auto lock=ManagerLifetimeTestAccess::HoldManager(*fixture.manager);Check(lock.owns_lock(),"actual M acquired");held();}
                if(guard==2)Scheduler::HoldScheduler(Access::Scheduler(*fixture.manager),held);
                if(guard==3)Scheduler::Hold(candidate.vm,false,held);
                if(guard==4){auto lock=fixture.registry.TryReservePublication();Check(lock.owns_lock(),"actual R acquired");held();}
                if(guard==5){auto lock=SemanticBridgeTestAccess::ReserveObservation(*fixture.bridge);Check(lock.owns_lock(),"actual B acquired");held();}
                if(guard==6){auto lock=RuntimeScopeTestAccess::ReserveState(*candidate.scope);Check(lock.owns_lock(),"actual Q acquired");held();}
                if(guard==7)Scheduler::Hold(candidate.vm,true,held);
            });}return Writer::CutAction::Continue;});Access::FileOutcome output;const auto result=Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),output);contender.Join();Check(result==ScopeResult::Busy&&cut,"actual one-shot final-fence Busy");const auto actual=Access::ObserveFile(output);Rejected(actual,ScopeResult::Busy);Check(actual.disk.compensationConfirmed&&actual.disk.compensationCalls==1&&fixture.domain->Revision()==0&&fixture.registry.Current()==snapshot&&!candidate.scope->Current().activationCommitted,"precise rejected memory and one native compensation");Check(prior?Read(fixture.root,fixture.state)==previousBytes:Absent(fixture.state),"exact original bytes or absence restored");
    }
}
/** @summary Real revoke after intent denies memory; an actual foreign edit blocks conditional restoration. @param cases Owned root. @param package Verified Base. */
void RevocationAndForeignEdit(Root& cases,const std::filesystem::path& package){
    for(bool foreign:{false,true}){Fixture fixture(cases,package);const std::string previous="enabled[0]:\n";Write(fixture.root,fixture.state,previous);auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);const auto snapshot=fixture.registry.Current();bool revoked{},changed{};
        Hook hook([&](const Writer::CutObservation& cut){if(cut.cut==Writer::Cut::AfterPublicationEffect&&cut.result.lastEffectKind==W::EffectKind::PublishRename&&cut.result.lastNativeReturn==W::NativeReturn::Succeeded&&!revoked){revoked=true;Check(RuntimeScopeTestAccess::Revoke(*candidate.scope,candidate.root.grantGeneration)==ScopeResult::Ok,"actual exact root revocation after intent");}if(foreign&&cut.cut==Writer::Cut::BeforeCompensationValidation&&!changed){changed=true;Write(fixture.root,fixture.state,"foreign edit survives");}return Writer::CutAction::Continue;});Access::FileOutcome output;const auto result=Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),output);const auto actual=Access::ObserveFile(output);
        Check(result==(foreign?ScopeResult::Indeterminate:ScopeResult::Denied)&&revoked,"genuine revoked final authority and independent file disposition");Rejected(actual,ScopeResult::Denied);Check(fixture.domain->Revision()==0&&fixture.registry.Current()==snapshot&&!candidate.scope->Current().activationCommitted,"revoked intent changes no memory");if(foreign)Check(changed&&!actual.disk.compensationConfirmed&&actual.disk.compensationCalls==0&&Read(fixture.root,fixture.state)=="foreign edit survives","original receipt cannot overwrite changed native bytes");else Check(actual.disk.compensationConfirmed&&Read(fixture.root,fixture.state)==previous,"revoked intent restores exact original bytes");
    }
}
/** @summary Genuine City generation change after publication rejects the original immutable factual lease. @param cases Owned root. @param package Verified Base. */
void ContextChange(Root& cases,const std::filesystem::path& package){Fixture fixture(cases,package);const auto previousBytes=Read(fixture.root,fixture.state);auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);const auto snapshot=fixture.registry.Current();bool changed{};Hook hook([&](const Writer::CutObservation& cut){if(cut.cut==Writer::Cut::AfterPublicationEffect&&cut.result.lastEffectKind==W::EffectKind::PublishRename&&cut.result.lastNativeReturn==W::NativeReturn::Succeeded&&!changed){changed=true;SemanticBridgeTestAccess::ObserveContext(*fixture.bridge,SemanticContextKind::City,7,4);}return Writer::CutAction::Continue;});Access::FileOutcome output;Check(Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),output)==ScopeResult::Stale,"genuine original factual lease is stale after intent");const auto actual=Access::ObserveFile(output);Rejected(actual,ScopeResult::Stale);Check(changed&&actual.disk.compensationConfirmed&&Read(fixture.root,fixture.state)==previousBytes&&fixture.domain->Revision()==0&&fixture.registry.Current()==snapshot,"context rejection restores original bytes without marker");}
/** @summary Synthetic interruption at real prewrite/owned-written cuts preserves prior requested bytes and never grants memory authority. @param cases Owned root. @param package Verified Base. */
void InterruptionBeforeMarker(Root& cases,const std::filesystem::path& package){for(bool written:{false,true}){Fixture fixture(cases,package);const std::string previous="enabled[0]:\n";Write(fixture.root,fixture.state,previous);auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);const auto snapshot=fixture.registry.Current();bool cut{};Hook hook([&](const Writer::CutObservation& point){if(point.cut==(written?Writer::Cut::AfterTempVerified:Writer::Cut::BeforeTempCreate)){cut=true;throw std::runtime_error("synthetic interruption outside publication fence");}return Writer::CutAction::Continue;});Access::FileOutcome output;const auto result=Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),output);const auto actual=Access::ObserveFile(output);Check(result==ScopeResult::NotReady&&cut&&actual.memoryAttempts==0&&actual.memory==Memory::NotAttempted&&!actual.crossed&&actual.disk.publicationCalls==0&&!actual.disk.publicationConfirmed&&actual.disk.compensationCalls==0,"actual interrupted native prefix cannot claim a memory or rename marker");Check(written?(actual.disk.temporaryCreateCalls>=1&&actual.disk.temporaryWriteCalls>=1):(actual.disk.temporaryCreateCalls==0&&actual.disk.temporaryWriteCalls==0),"actual native prefix effects remain truthful");Check(Read(fixture.root,fixture.state)==previous&&fixture.domain->Revision()==0&&fixture.registry.Current()==snapshot,"synthetic premarker interruption preserves requested file and memory");}}
/** @summary Suppress only the first proof after a real completed rename; later file-only reconciliation cannot activate memory. @param cases Owned root. @param package Verified Base. */
void RealUnknown(Root& cases,const std::filesystem::path& package){
    Fixture fixture(cases,package);auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);const auto snapshot=fixture.registry.Current();bool suppressed{};Access::FileOutcome output;ScopeResult result=ScopeResult::Ok;ManagerPublication::Plan nested;const auto nestedBefore=Access::ReadPlanFacts(nested);ScopeResult nestedResult=ScopeResult::Ok;
    {Hook hook([&](const Writer::CutObservation& cut){if(cut.cut==Writer::Cut::AfterPublicationEffect&&cut.result.lastEffectKind==W::EffectKind::PublishRename&&cut.result.lastNativeReturn==W::NativeReturn::Succeeded&&!suppressed){suppressed=true;nestedResult=Access::AttemptPersistence(*fixture.manager,std::move(nested),output);return Writer::CutAction::SuppressNextObservation;}return Writer::CutAction::Continue;});result=Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),output);}
    Check(result==ScopeResult::Indeterminate&&nestedResult==ScopeResult::Busy,"native uncertainty and same-action recursion are distinct precise outcomes");SamePlan(nestedBefore,Access::ReadPlanFacts(nested));
    const auto unknown=Access::ObserveFile(output);Check(suppressed&&unknown.admitted&&unknown.disk.kind==W::Kind::Unknown&&!unknown.disk.publicationConfirmed&&unknown.disk.publicationCalls==1&&unknown.disk.fixtureObservationUnavailable&&unknown.memory==Memory::NotAttempted&&unknown.memoryAttempts==0&&!unknown.crossed&&!unknown.compensationRequested,"real native success without proof never attempts memory");Check(fixture.domain->Revision()==0&&fixture.registry.Current()==snapshot,"unconfirmed file has no memory marker");DiskOrder(fixture,{"fixture.owner"});candidate=Candidate{};fixture.manager.reset();fixture.base.reset();
    const auto reconciled=Access::ReconcileFile(output);Check(reconciled.publicationConfirmed&&reconciled.publicationCalls==1,"file-only exact original later proof without rename replay");const auto final=Access::ObserveFile(output);Check(final.memory==Memory::NotAttempted&&final.memoryAttempts==0&&!final.crossed&&fixture.domain->Revision()==0&&fixture.registry.Current()==snapshot,"reconciliation after manager death grants no memory authority");
}
/** @summary Actual cache allocation failure after a real marker preserves committed file and memory, never compensates. @param cases Owned root. @param package Verified Base. */
void MarkerCachePending(Root& cases,const std::filesystem::path& package){
    Fixture fixture(cases,package);auto candidate=fixture.Make(1);fixture.Prepare(nullptr,candidate);actualPostmarkerFault=false;faultInFence=false;failAfterRevision=1;failAfterDomain=fixture.domain.get();Access::FileOutcome output;const auto result=Access::AttemptPersistence(*fixture.manager,std::move(candidate.plan),output);failAfterDomain=nullptr;const auto actual=Access::ObserveFile(output);
    Check(actualPostmarkerFault&&!faultInFence&&result==ScopeResult::Indeterminate,"actual off-fence postmarker cache fault");Check(actual.crossed&&actual.revision==1&&actual.memory==Memory::MarkerCommitted&&actual.memoryAttempts==1&&actual.disk.publicationConfirmed&&!actual.compensationRequested&&actual.disk.compensationCalls==0,"postmarker result retains original commit disposition");Check(fixture.domain->Revision()==1&&candidate.scope->Current().activationCommitted&&fixture.registry.IsActive("fixture.owner"),"real marker and memory survive cache pending");DiskOrder(fixture,{"fixture.owner"});const auto pending=ManagerCommandsTestAccess::ObserveCache(*fixture.manager,"fixture.owner");Check(pending.result==CacheSettlement::Result::Ok&&pending.phase==CacheSettlement::Phase::CommittedPending&&pending.committedRevision==1,"actual existing cache ledger retains original obligation");const auto repaired=ManagerCommandsTestAccess::RepairCache(*fixture.manager,"fixture.owner");Check(repaired.result==CacheSettlement::Result::Ok&&repaired.phase==CacheSettlement::Phase::Settled&&repaired.committedRevision==1&&fixture.domain->Revision()==1,"cache repair does not replay memory");
}
/** @summary Two retained terminal views consume the actual original local capacity; a third action cannot mutate memory. @param cases Owned root. @param package Verified Base. */
void RetainedOutcomeCapacity(Root& cases,const std::filesystem::path& package){
    Fixture fixture(cases,package);auto old=fixture.Make(1);fixture.Prepare(nullptr,old);Access::FileOutcome first;Check(Access::AttemptPersistence(*fixture.manager,std::move(old.plan),first)==ScopeResult::Ok,"first genuine charged file outcome");auto next=fixture.Make(9);fixture.Prepare(&old,next);auto origin=Access::Origin(next.plan);Access::FileOutcome second;Check(Access::AttemptPersistence(*fixture.manager,std::move(next.plan),second)==ScopeResult::Ok,"second genuine retained file outcome");W::Owner owner;Check(Writer::Open(fixture.state,owner).kind==W::Kind::Ok,"same original local capacity owner");const auto before=Writer::Inspect(owner);Check(before.thisOwnerSlotCount==2&&before.rawBytesReserved>=4U*1024U*1024U,"two original retained native record charges");ManagerPublication::Plan removal;std::string error;Check(Access::Remove(*fixture.manager,origin,removal,error)==ScopeResult::Ok,"real third native action prepared");const auto registry=fixture.registry.Current();const auto bytes=Read(fixture.root,fixture.state);Access::FileOutcome third;Check(Access::AttemptPersistence(*fixture.manager,std::move(removal),third)==ScopeResult::NotReady,"actual original local capacity refuses third file preparation");const auto observed=Access::ObserveFile(third);Check(observed.admitted&&observed.memoryAttempts==0&&!observed.crossed&&observed.disk.temporaryCreateCalls==0&&observed.disk.publicationCalls==0&&fixture.domain->Revision()==2&&fixture.registry.Current()==registry&&Read(fixture.root,fixture.state)==bytes,"capacity denial preserves active memory and exact newer file");Check(Writer::Inspect(owner).thisOwnerSlotCount==2,"third action neither evades nor erases original capacity");
}
/** @summary Hold one real VM Close callback while observing the synchronous original action lifetime. */
struct CloseGate final{std::mutex mutex;std::condition_variable changed;bool entered{},released{};bool inFence{},onLane{};
    /** @summary Pause only an actual Close, outside the VM publication guard. */
    void Pause(){std::unique_lock lock(mutex);inFence=Scheduler::InsideFence();onLane=ScriptRuntime::OnScriptLane();entered=true;changed.notify_all();changed.wait(lock,[&]{return released;});}
    /** @summary Release every borrowed callback before cleanup. */
    void Release(){const std::lock_guard lock(mutex);released=true;changed.notify_all();}
    /** @summary Preserve release on caller assertion paths. */
    ~CloseGate(){Release();}
};
/** @summary Unknown must dispose the whole Plan synchronously under original Turn and Lease before returning file-only ownership. @param cases Owned root. @param package Verified Base. */
void WholePlanTeardown(Root& cases,const std::filesystem::path& package){
    Fixture fixture(cases,package);CloseGate gate;auto candidate=fixture.Make(1,[&](ScriptPhase phase,std::uint64_t){if(phase==ScriptPhase::Close)gate.Pause();});fixture.Prepare(nullptr,candidate);ManagerPublication::Plan original=std::move(candidate.plan);std::weak_ptr<ScriptRuntime> weak=candidate.vm;candidate=Candidate{};const auto handle=Close::Capture(*fixture.manager);Access::FileOutcome output;
    auto action=std::async(std::launch::async,[&]{Hook hook([](const Writer::CutObservation& cut){return cut.cut==Writer::Cut::AfterPublicationEffect&&cut.result.lastEffectKind==W::EffectKind::PublishRename&&cut.result.lastNativeReturn==W::NativeReturn::Succeeded?Writer::CutAction::SuppressNextObservation:Writer::CutAction::Continue;});return Access::AttemptPersistence(*fixture.manager,std::move(original),output);});
    /** @summary Join before future destruction on every failing main-thread oracle. */
    struct Joined final{CloseGate& gate;std::future<ScopeResult>& action;/** @summary Release borrowed callback and join native action. */~Joined(){gate.Release();if(action.valid())action.wait();}} joined{gate,action};
    bool entered{};{std::unique_lock lock(gate.mutex);entered=gate.changed.wait_for(lock,5s,[&]{return gate.entered;});}
    if(!entered){gate.Release();action.wait();Check(false,"actual candidate Close must occur before Unknown returns");}
    W::Owner owner;Check(Writer::Open(fixture.state,owner).kind==W::Kind::Ok,"same actual path owner rediscovered");W::Turn competing;const auto turn=W::TryTurn(owner,competing);const bool stillWaiting=action.wait_for(20ms)==std::future_status::timeout;Check(ManagerClose::Request(handle)==ScopeResult::Ok,"real close request during synchronous action");ManagerClose::Token close;const auto acquired=ManagerClose::Acquire(handle,close);ManagerClose::Observation paused;if(acquired==ScopeResult::Ok)paused=ManagerClose::Poll(close);gate.Release();const auto result=action.get();
    Check(stillWaiting&&!gate.inFence&&gate.onLane&&turn.kind==W::Kind::Busy,"real Close completes on lane outside fence while original Turn remains exclusive");Check(acquired==ScopeResult::Ok&&paused.users==1&&paused.result==ScopeResult::NotReady,"original action retains exactly one counted Lease through actual Plan disposal");const auto after=ManagerClose::Poll(close);Check(after.users==0&&weak.expired(),"whole original VM expires and no action borrower escapes returned Unknown");const auto unknown=Access::ObserveFile(output);Check(result==ScopeResult::Indeterminate&&unknown.memoryAttempts==0&&unknown.disk.kind==W::Kind::Unknown,"returned file-only original uncertainty");close=ManagerClose::Token{};fixture.manager.reset();fixture.base.reset();const auto reconciled=Access::ReconcileFile(output);Check(reconciled.publicationConfirmed&&Access::ObserveFile(output).memoryAttempts==0,"file-only reconciliation after genuine teardown");
}
/** @summary Preserve requested-file semantics without claiming automatic startup authority. @param cases Owned root. @param package Verified Base. */
void StartupIntentBoundary(Root& cases,const std::filesystem::path& package){
    Fixture fixture(cases,package);std::vector<ModId> requested;std::string error;const auto initialized=BeforeFile(fixture.root,fixture.state);Check(LoadEnabledState(fixture.state,requested,error)&&requested.empty(),"initialized empty request supplies no activation or permission");UnchangedFile(fixture.root,fixture.state,initialized);const auto absentRequest=fixture.root/"absent-request.toon";Child(fixture.root,absentRequest);Check(Absent(absentRequest)&&LoadEnabledState(absentRequest,requested,error)&&requested.empty()&&Absent(absentRequest),"genuine absent request supplies no activation or permission");
    const std::string malformed="malformed requested intent\n";Write(fixture.root,fixture.state,malformed);Check(!LoadEnabledState(fixture.state,requested,error)&&requested.empty()&&Read(fixture.root,fixture.state)==malformed,"malformed requested file remains exact");
    const std::string missing="enabled[1]: fixture.missing\n";Write(fixture.root,fixture.state,missing);Check(LoadEnabledState(fixture.state,requested,error)&&requested==std::vector<ModId>{"fixture.missing"}&&Read(fixture.root,fixture.state)==missing,"text intent for missing content remains a request only");
    fixture.Source(1);const auto old=Preparation::Capture(*fixture.manager,"fixture.owner",fixture.root/"mods"/"fixture.owner",fixture.pin);fixture.Source(9);const auto changed=Preparation::Capture(*fixture.manager,"fixture.owner",fixture.root/"mods"/"fixture.owner",fixture.pin);Check(old->ContentDigest()!=changed->ContentDigest(),"actual modified script bytes have distinct immutable content identity");
    Check(fixture.domain->Revision()==0&&!fixture.registry.IsActive("fixture.owner")&&fixture.manager->EnabledOrder().empty()&&!Close::CurrentVM(*fixture.manager,"fixture.owner"),"absent, corrupt, missing and changed requested content mint no canonical VM, marker or permission");
}
}
/** @summary Run only explicit root-owned native persistence fixtures. @param argc Exact argument count. @param argv Base checkout and existing analysis parent. @return Zero only when every real oracle passes. */
int main(int argc,char** argv){try{
    Check(argc==3,"verified Base and root-owned NTFS analysis parent required");static_assert(sizeof(Access::FileOutcome)<=512);Check(Access::FileOutcomeBytes()==sizeof(Access::FileOutcome),"actual full file outcome bound");
    Root root{std::filesystem::path(argv[2])};const std::filesystem::path package=argv[1];
    const auto run=[&](const char* name,auto test){std::cout<<"persistence begin "<<name<<std::endl;test(root,package);};
    run("positive lifecycle and explicit no-op",Positive);run("exact original identity admission",IdentityAdmission);
    run("actual lane and closing admission",LaneAndClosingAdmission);run("cache preadmission",CacheAdmission);
    run("all original final fences",FinalFences);run("revocation and foreign edit",RevocationAndForeignEdit);run("original factual context change",ContextChange);
    run("synthetic native interruption prefixes",InterruptionBeforeMarker);
    run("real unknown file-only reconciliation",RealUnknown);run("real marker cache pending",MarkerCachePending);
    run("retained terminal outcome capacity",RetainedOutcomeCapacity);run("whole Plan original Turn and Lease",WholePlanTeardown);
    run("startup intent closed boundary",StartupIntentBoundary);
    Check(fenceAllocations==0&&fenceFrees==0&&fenceCallbacks==0,"no native allocations, frees or observer callbacks under actual publication fence");
    std::cout<<checks<<" native manager persistence checks passed\n";return 0;
}catch(const std::exception& error){failAfterDomain=nullptr;Writer::Hook({});std::cerr<<"manager persistence: "<<error.what()<<'\n';return 1;}}
