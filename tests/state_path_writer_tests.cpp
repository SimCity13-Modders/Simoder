#include "native_junction_fixture.hpp"
#include "mods/state_path_writer.hpp"
#include "mods/mod_state_store.hpp"
#include "platform/file_publication_retry.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using W=sc13::mods::StatePathWriter;using A=sc13::mods::StatePathWriterTestAccess;
using namespace std::chrono_literals;
unsigned checks{};
/** @summary Require observed native behavior. @param value Actual condition. @param message Fixed evidence. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Close exactly one test-owned native handle. */
struct Handle final{HANDLE value{INVALID_HANDLE_VALUE};
    /** @summary Own one returned native handle. @param handle Original result. */
    explicit Handle(HANDLE handle=INVALID_HANDLE_VALUE):value(handle){}
    /** @summary Release only original native ownership. */
    ~Handle(){Close();}
    Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;
    /** @summary Explicitly release before an outcome assertion. */
    void Close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
};
/** @summary Verify existing native ancestors before any fixture mutation. @param path Absolute normalized owned path. @param directory Final kind. */
void Plain(const std::filesystem::path& path,bool directory=true){Check(path.is_absolute()&&path==path.lexically_normal(),"normalized absolute native fixture required");auto current=path.root_path();for(const auto& part:path.relative_path()){Check(part!=L"."&&part!=L"..","native fixture component bounded");current/=part;const auto flags=GetFileAttributesW(current.c_str());Check(flags!=INVALID_FILE_ATTRIBUTES&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT),"native fixture ancestor plain");Check(current==path&&!directory?!(flags&FILE_ATTRIBUTE_DIRECTORY):bool(flags&FILE_ATTRIBUTE_DIRECTORY),"native fixture entry kind");}}
/** @summary Check a fixed child remains beneath the original qualified test root. @param root Fresh original root. @param path Fixed effect target. */
void Child(const std::filesystem::path& root,const std::filesystem::path& path){Check(path.is_absolute()&&path==path.lexically_normal(),"fixed effect absolute normalized");const auto relative=path.lexically_relative(root);Check(!relative.empty()&&!relative.is_absolute(),"fixed effect below root");for(const auto& part:relative)Check(part!=L".."&&part!=L".","fixed effect cannot escape root");Plain(root);Plain(path.parent_path());}
/** @summary Create exactly one directory beneath a positively checked parent. @param parent Plain parent. @param leaf Fixed leaf. @return Fresh child. */
std::filesystem::path Directory(const std::filesystem::path& parent,const std::wstring& leaf){Plain(parent);Check(leaf.find_first_of(L"/\\:")==std::wstring::npos&&!leaf.empty(),"fixed directory leaf");const auto path=parent/leaf;Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"exclusive native fixture directory creation");Plain(path);return path;}
/** @summary Bind shared native junction operations to this fixture's original safety checks. */
struct JunctionPolicy final{
    /** @summary Require an actual observation. @param value Predicate. @param message Diagnostic. */
    static void Require(bool value,const char* message){Check(value,message);}
    /** @summary Validate a fixed child before native effects. @param root Owner. @param path Child. */
    static void ValidateChild(const std::filesystem::path& root,const std::filesystem::path& path){Child(root,path);}
    /** @summary Validate native plain ancestors and kind. @param path Target. @param directory Required kind. */
    static void ValidatePlain(const std::filesystem::path& path,bool directory){Plain(path,directory);}
    /** @summary Exclusively create one checked directory. @param parent Plain parent. @param leaf Fixed leaf. @return Created child. */
    static std::filesystem::path CreateDirectory(const std::filesystem::path& parent,const wchar_t* leaf){return Directory(parent,leaf);}
};
using JunctionSupport=sc13::testing::NativeJunctionFixture<JunctionPolicy>;
using JunctionIdentity=JunctionSupport::JunctionIdentity;
using JunctionHandle=JunctionSupport::JunctionHandle;
/** @summary Resolve an actual plain target directory independently of the junction node. @param path Owned target. @return Copied native identity. */
JunctionIdentity JunctionTargetIdentity(const std::filesystem::path& path){Plain(path,true);JunctionHandle handle(JunctionSupport::OpenNode(path));return JunctionSupport::NodeIdentity(handle.value);}
/** @summary Own fresh native roots without recursive cleanup of failed evidence. */
struct Root final{std::filesystem::path path;unsigned next{};
    /** @summary Qualify caller-declared analysis parent before creating a PID root. @param parent Existing root-owned analysis directory. */
    explicit Root(const std::filesystem::path& parent){Plain(parent);bool analysis{};for(const auto& component:parent)if(component==L".analysis")analysis=true;Check(analysis,"native test parent must be declared analysis evidence");wchar_t fs[32]{};Check(GetDriveTypeW(parent.root_path().c_str())==DRIVE_FIXED,"actual fixed-drive profile required");Check(GetVolumeInformationW(parent.root_path().c_str(),nullptr,0,nullptr,nullptr,nullptr,fs,32)!=FALSE&&std::wstring(fs)==L"NTFS","actual NTFS profile required");path=Directory(parent,L"writer-native-"+std::to_wstring(GetCurrentProcessId()));std::cout<<"retained native fixture="<<path.string()<<std::endl;}
    /** @summary Create a distinct plain case directory. @return Native owned parent. */
    std::filesystem::path Case(){return Directory(path,L"case-"+std::to_wstring(++next));}
};
/** @summary Read exact isolated bytes while respecting retained writer DELETE ownership.
 * @param root Original fixture root. @param path Known owned file.
 * @return Complete stable default-stream bytes; failure is never replaced with empty data. */
std::string Read(const std::filesystem::path& root,const std::filesystem::path& path){
    Child(root,path);Plain(path,false);
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error("native read open error="+std::to_string(GetLastError()));
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    Check(GetFileType(file.value)==FILE_TYPE_DISK&&
        GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attributes,sizeof(attributes))&&
        !(attributes.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"native read handle plain file");
    LARGE_INTEGER size{};
    Check(GetFileSizeEx(file.value,&size)&&size.QuadPart>=0&&size.QuadPart<=16LL*1024*1024,"bounded native fixture read size");
    std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');
    std::size_t offset{};
    while(offset<bytes.size()){
        DWORD count{};
        Check(ReadFile(file.value,bytes.data()+offset,static_cast<DWORD>(bytes.size()-offset),&count,nullptr)&&
            count>0&&count<=bytes.size()-offset,"complete native fixture read");
        offset+=count;
    }
    char extra{};DWORD count{};LARGE_INTEGER after{};
    Check(ReadFile(file.value,&extra,1,&count,nullptr)&&count==0&&GetFileSizeEx(file.value,&after)&&
        after.QuadPart==size.QuadPart,"native fixture read EOF and stable size");
    return bytes;
}
/** @summary Write a known owned plain/absent file with no recursive directory operation. @param root Original root. @param path Fixed file. @param bytes Native test data. */
void Write(const std::filesystem::path& root,const std::filesystem::path& path,std::string_view bytes){Child(root,path);const auto flags=GetFileAttributesW(path.c_str());if(flags!=INVALID_FILE_ATTRIBUTES)Plain(path,false);else Check(GetLastError()==ERROR_FILE_NOT_FOUND,"native test target absent");Handle handle(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));Check(handle.value!=INVALID_HANDLE_VALUE,"native test file create");DWORD written{};Check(bytes.size()<=MAXDWORD&&WriteFile(handle.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size(),"native test full write");Check(FlushFileBuffers(handle.value)!=FALSE,"native test flush");}
/** @summary Query the actual original native object identity. @param path Known owned regular file. @return FileIdInfo evidence. */
W::FileIdentity Identity(const std::filesystem::path& path){Handle handle(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));Check(handle.value!=INVALID_HANDLE_VALUE,"native identity open");FILE_ID_INFO value{};Check(GetFileInformationByHandleEx(handle.value,FileIdInfo,&value,sizeof(value))!=FALSE,"actual FileIdInfo required");W::FileIdentity result;result.volume=value.VolumeSerialNumber;std::memcpy(result.file.data(),value.FileId.Identifier,16);result.valid=true;return result;}
/** @summary Write/read a genuine named stream, separate from supplied-path syntax. @param root Original root. @param base Known plain default file. @param create Create versus delete test stream. */
void Stream(const std::filesystem::path& root,const std::filesystem::path& base,bool create){Child(root,base);Plain(base,false);const auto name=base.wstring()+L":c1-test";if(create){Handle handle(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Check(handle.value!=INVALID_HANDLE_VALUE,"genuine named stream creation required");constexpr char payload[]="native-stream";DWORD written{};Check(WriteFile(handle.value,payload,sizeof(payload)-1,&written,nullptr)&&written==sizeof(payload)-1,"genuine stream bytes written");Check(FlushFileBuffers(handle.value)!=FALSE,"genuine stream flush");}else Check(DeleteFileW(name.c_str())!=FALSE,"remove only exact test-owned named stream");}
/** @summary Prove actual named-stream bytes survive writer rejection. @param root Original root. @param base Known default file. */
void StreamReadback(const std::filesystem::path& root,const std::filesystem::path& base){Child(root,base);Plain(base,false);const auto name=base.wstring()+L":c1-test";Handle stream(CreateFileW(name.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Check(stream.value!=INVALID_HANDLE_VALUE,"original named stream remains present");std::array<char,32> bytes{};DWORD read{};Check(ReadFile(stream.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&std::string_view(bytes.data(),read)=="native-stream","actual named stream readback unchanged");}
/** @summary Perform an exact test-owned external move without replacement or fallback. @param root Fresh root. @param from Original file. @param to Absent fixed sibling. */
void Move(const std::filesystem::path& root,const std::filesystem::path& from,const std::filesystem::path& to){Child(root,from);Child(root,to);Plain(from,false);Check(GetFileAttributesW(to.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"external move destination absent");Check(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"real isolated external move required");}
/** @summary Require completed native rename evidence without replacing identity/default-byte proof.
 * @param result Actual copied writer result. @param effect Original rename kind. @param success Required signed terminal classification. */
void NativeCompleted(const W::Result& result,W::EffectKind effect,bool success=true){
    const auto& rename=result.rename;constexpr std::uint32_t pending=0x00000103U;
    Check(rename.backend==W::RenameBackend::NtdllParentRelative&&rename.exportsQualified&&rename.effect==effect,"qualified single native rename backend and original effect");
    Check(rename.callValid&&rename.finalValid&&rename.submission!=0&&!rename.outstanding&&rename.completionAcknowledged,"actual submitted native call and acknowledged final result required");
    Check(rename.finalStatus!=pending&&(rename.callStatus==pending||rename.callStatus==rename.finalStatus),"nonpending final status joins the actual immediate or deferred native return");
    Check((static_cast<std::int32_t>(rename.finalStatus)>=0)==success&&result.lastNativeReturn==(success?W::NativeReturn::Succeeded:W::NativeReturn::Failed),"signed completed NTSTATUS and native classification agree");
    Check(rename.submittedLength>=sizeof(wchar_t)+16&&rename.submittedLength<=526&&rename.setInformationRva!=0&&rename.mapErrorRva!=0,"bounded actual native input and qualified export RVAs");
}
/** @summary Copy protocol diagnostics with distinct Win32, raw NTSTATUS and derived DOS domains. @param label Fixed scenario. @param result Native result. */
void Report(const char* label,const W::Result& result){
    const auto& rename=result.rename;
    std::cerr<<label<<" kind="<<static_cast<int>(result.kind)<<" phase="<<static_cast<int>(result.phase)<<" history="<<static_cast<int>(result.history)<<" current="<<static_cast<int>(result.current)<<" native="<<static_cast<int>(result.lastNativeReturn)<<" win32Error="<<result.nativeError<<" creates="<<result.temporaryCreateCalls<<" writes="<<result.temporaryWriteCalls<<" publishes="<<result.publicationCalls<<" compensation="<<result.compensationCalls<<" cleanup="<<result.cleanupDispositionCalls<<" backoff="<<result.elapsedBackoffMs
        <<" renameBackend="<<static_cast<int>(rename.backend)<<" renameEffect="<<static_cast<int>(rename.effect)<<" renameSubmission="<<rename.submission<<" callNTSTATUS="<<rename.callStatus<<" callValid="<<rename.callValid<<" finalNTSTATUS="<<rename.finalStatus<<" finalValid="<<rename.finalValid<<" derivedDosError="<<rename.mappedError<<" derivedDosValid="<<rename.mappedErrorValid<<" outstanding="<<rename.outstanding<<" acknowledged="<<rename.completionAcknowledged<<" inputBytes="<<rename.submittedLength<<" exportsQualified="<<rename.exportsQualified<<" waitAttempted="<<rename.waitAttempted<<" waitResult="<<rename.waitResult<<" waitError="<<rename.waitError<<" information="<<rename.information<<" informationValid="<<rename.informationValid<<std::endl;
}
/** @summary Retain original writer Owner/Turn until explicit settlement/close. */
struct Session final{std::filesystem::path parent,path;W::Owner owner;W::Turn turn;
    /** @summary Open mandatory actual qualified profile. @param root Native fixture root. @param existing Optional previous raw bytes. */
    Session(Root& root,const std::string* existing=nullptr):parent(root.Case()),path(parent/L"state.toon"){if(existing)Write(root.path,path,*existing);const auto opened=A::Open(path,owner);Report("open",opened);Check(opened.kind==W::Kind::Ok,"actual qualified owner Open required, unsupported is failure");
        const auto exports=A::ExportProvenance(owner);
        Check(exports.kind==W::Kind::Ok&&exports.qualified&&exports.modulePath.front()!=L'\0'&&exports.modulePath.back()==L'\0',"copied qualified system-module path before writer Turn/effects");
        Check(exports.moduleBase!=0&&exports.imageSize!=0&&exports.setInformationAddress>=exports.moduleBase&&exports.mapErrorAddress>=exports.moduleBase&&exports.setInformationRva!=0&&exports.mapErrorRva!=0&&exports.setInformationRva<exports.imageSize&&exports.mapErrorRva<exports.imageSize&&exports.setInformationAddress-exports.moduleBase==exports.setInformationRva&&exports.mapErrorAddress-exports.moduleBase==exports.mapErrorRva,"actual copied export addresses agree with bounded image RVAs");
        std::wcerr<<L"copied OS module path="<<exports.modulePath.data()<<L" base="<<exports.moduleBase<<L" imageBytes="<<exports.imageSize<<L" NtSetInformationFile="<<exports.setInformationAddress<<L" NtSetInformationFileRva="<<exports.setInformationRva<<L" RtlNtStatusToDosError="<<exports.mapErrorAddress<<L" RtlNtStatusToDosErrorRva="<<exports.mapErrorRva<<std::endl;
        Check(exports.systemFileIdentity.valid&&Identity(std::filesystem::path(exports.modulePath.data()))==exports.systemFileIdentity,"copied physical system-file identity matches the actual read-only file");
        Check(exports.mappedPath.front()!=L'\0'&&exports.mappedPath.back()==L'\0'&&std::wstring_view(exports.mappedPath.data()).starts_with(L"\\Device\\"),"complete copied mapped native device path required");
        std::wcerr<<L"copied OS mapped path="<<exports.mappedPath.data()<<L" volume="<<exports.systemFileIdentity.volume<<std::endl;
        const auto metrics=A::Inspect(owner);
        Check(metrics.nativeRenameContextBytes>1024&&metrics.nativeRenameContextBytes<=1152&&metrics.nativeRenameContextBytes*8<=9216&&metrics.renameEvidenceBytes==sizeof(W::RenameEvidence)&&metrics.renameEvidenceBytes<=64&&metrics.recordBytes<=4096&&metrics.ownerBytes<=16384,"actual compiled native context/evidence/record/owner budgets");
        Check(W::TryTurn(owner,turn).kind==W::Kind::Ok,"original native turn");}
    /** @summary Release original admission before any receipt/Owner last disposal. */
    ~Session(){A::Hook({});turn=W::Turn{};}
    /** @summary Reserve genuine untouched operation. @param value Enabled ID. @param intent Empty output. @return Native operation identity. */
    W::OperationIdentity Prepare(W::Intent& intent,std::string value="fixture.updated"){const std::array<sc13::mods::ModId,1> enabled{std::move(value)};const auto result=W::Prepare(turn,enabled,intent);Report("prepare",result);Check(result.kind==W::Kind::Ok,"actual bounded Prepare");return result.operation;}
    /** @summary Publish and positively confirm actual owned-object rename. @param intent Original operation. @param receipt Empty output. @return Native result. */
    W::Result Publish(W::Intent& intent,W::Receipt& receipt){const auto result=W::Publish(turn,intent,receipt);Report("publish",result);NativeCompleted(result,W::EffectKind::PublishRename);Check(result.kind==W::Kind::Published&&result.publicationConfirmed&&result.history==W::History::Published,"actual parent-handle rename and positive identity/default-stream confirmation required");return result;}
    /** @summary Explicitly settle known operation before releasing views. @param id Original opaque identity. */
    void Settle(const W::OperationIdentity& id){Check(W::Settle(turn,id).kind==W::Kind::Ok,"known original operation settled");}
    /** @summary Explicitly close after no owner slot remains. */
    void Close(){turn=W::Turn{};Check(W::CloseOwner(owner).kind==W::Kind::Closed,"original owner explicit close");}
};

/** @summary Mandatory real parent-relative rename, raw-content restoration and closed receipt history. @param root Native evidence owner. */
void PositiveAndHistory(Root& root){const std::string previous="enabled[1]:   fixture.original\n\n";Session session(root,&previous);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;const auto result=session.Publish(intent,receipt);Check(result.publicationCalls==1&&result.lastNativeReturn==W::NativeReturn::Succeeded,"actual completed native status/count distinct from confirmation");const auto observed=W::Observe(session.turn,receipt);Check(observed.owned.valid&&observed.destination.valid&&observed.owned==observed.destination&&observed.defaultStreamOnly&&observed.candidateBytesMatch,"actual same object/default-stream evidence");const auto candidate=Identity(session.path);Check(Read(root.path,session.path)=="enabled[1]: fixture.updated\n","shared canonical serialization bytes");Check(W::HistoryView(receipt).kind==W::Kind::Busy,"history same native turn reentry Busy");const auto compensated=W::Compensate(session.turn,receipt);Report("restore",compensated);NativeCompleted(compensated,W::EffectKind::RestoreRename);Check(compensated.kind==W::Kind::Compensated&&compensated.publicationConfirmed&&compensated.compensationConfirmed,"actual restore positively confirmed");Check(Read(root.path,session.path)==previous&&Identity(session.path)!=candidate,"restore exact raw DEFAULT bytes, new identity allowed");const auto repeat=W::Compensate(session.turn,receipt);Check(repeat.compensationCalls==compensated.compensationCalls&&repeat.compensationConfirmed,"compensation does not repeat native effect");Write(root.path,session.path,"external after confirmed restore");const auto changed=W::Observe(session.turn,receipt);Check(changed.result.history==W::History::Compensated&&changed.result.current==W::Current::Conflict&&changed.result.compensationConfirmed,"historical compensated differs from fresh current conflict");session.Settle(id);session.Close();const auto history=W::HistoryView(receipt);Check(history.kind!=W::Kind::Busy&&history.current==W::Current::OwnerClosed&&history.publicationConfirmed&&history.compensationConfirmed&&history.settled,"closed history has no current-file authority");W::Turn denied;Check(W::TryTurn(session.owner,denied).kind==W::Kind::Closed,"closed owner cannot reopen live turn");}
/** @summary Prior-absence compensation performs exact original deletion, including real reader-delayed absence. @param root Fresh test root. */
void AbsenceAndPending(Root& root){Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;session.Publish(intent,receipt);Handle reader(CreateFileW(session.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Check(reader.value!=INVALID_HANDLE_VALUE,"actual delete-sharing reader");const auto compensated=W::Compensate(session.turn,receipt);Report("absence",compensated);Check(!compensated.compensationConfirmed&&(compensated.kind==W::Kind::Unknown||compensated.phase==W::Phase::CompensatingDeletePending),"real held reader delays positive absence and retains pending evidence");const auto pending=A::Inspect(session.owner);Check(pending.thisOwnerSlotCount==1&&pending.rawBytesReserved>=2U*1024U*1024U,"delete pending retains operation charge");Check(W::Settle(session.turn,id).kind!=W::Kind::Ok,"pending deletion cannot settle");const auto calls=compensated.compensationCalls;reader.Close();const auto recovered=W::Reconcile(session.turn,id);Check(recovered.kind==W::Kind::Compensated&&recovered.compensationConfirmed&&recovered.compensationCalls==calls,"positive absence reconciliation never repeats disposition");Check(GetFileAttributesW(session.path.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"actual prior absence restored");session.Settle(id);session.Close();}
/** @summary Native completed success before first proof remains Unknown after real original move/replacement, then exact original recovery. @param root Owned fixture. */
void UnknownRealConflict(Root& root){Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;const auto moved=session.parent/L"original-moved.toon";W::FileIdentity original;bool cut{};A::Hook([&](const A::CutObservation& observation){if(observation.cut==A::Cut::AfterPublicationEffect&&observation.result.lastNativeReturn==W::NativeReturn::Succeeded){NativeCompleted(observation.result,W::EffectKind::PublishRename);Check(!cut&&observation.result.publicationCalls==1&&observation.owned.valid,"real completed native success cut");cut=true;original=observation.owned;W::Turn reentrant;Check(W::TryTurn(session.owner,reentrant).kind==W::Kind::Busy&&W::HistoryView(receipt).kind==W::Kind::Busy,"actual publication cut cannot reenter owner/history");Move(root.path,session.path,moved);Write(root.path,session.path,"external replacement");}return A::CutAction::Continue;});const auto result=W::Publish(session.turn,intent,receipt);A::Hook({});Report("real unknown",result);NativeCompleted(result,W::EffectKind::PublishRename);Check(cut&&result.kind==W::Kind::Unknown&&result.history==W::History::Unknown&&!result.publicationConfirmed&&result.lastNativeReturn==W::NativeReturn::Succeeded&&result.publicationCalls==1,"native completed success alone is not confirmed historical publication");Check(Identity(moved)==original&&Read(root.path,session.path)=="external replacement","unknown preserves actual original and external replacement");const auto before=A::Inspect(session.owner);intent={};receipt={};const auto operations=W::Operations(session.turn);Check(operations.count==1&&operations.operations[0].operation==id&&before.thisOwnerSlotCount==1,"last caller receipt drop cannot erase original Unknown");Check(W::Settle(session.turn,id).kind!=W::Kind::Ok,"unknown cannot settle");session.turn=W::Turn{};Check(W::CloseOwner(session.owner).kind!=W::Kind::Closed,"unknown cannot close");session.owner={};W::Owner rediscovered;Check(A::Open(session.path,rediscovered).kind==W::Kind::Ok,"quarantined original key rediscovery after every external owner dropped");session.owner=rediscovered;Check(A::Inspect(session.owner).thisOwnerSlotCount==1&&A::Inspect(session.owner).rawBytesReserved>=2U*1024U*1024U,"registry retains original Unknown charge after all caller views drop");Check(W::TryTurn(rediscovered,session.turn).kind==W::Kind::Ok,"original quarantine permits reconcile turn");W::Intent blocked;const std::array<sc13::mods::ModId,1> enabled{"fixture.blocked"};Check(W::Prepare(session.turn,enabled,blocked).kind!=W::Kind::Ok,"unknown forbids new operation");const auto still=W::Reconcile(session.turn,id);Check(still.kind==W::Kind::Unknown&&still.publicationCalls==1&&!still.publicationConfirmed,"real conflicting observation does not replay or guess");Child(root.path,session.path);Check(DeleteFileW(session.path.c_str())!=FALSE,"remove only known test replacement");Move(root.path,moved,session.path);Check(Identity(session.path)==original,"same original native object returned");const auto repaired=W::Reconcile(session.turn,id);Check(repaired.kind==W::Kind::Published&&repaired.publicationConfirmed&&repaired.publicationCalls==1,"only actual original proof confirms publication");session.Settle(id);session.Close();}
/** @summary Observation suppression never overrides native completed success; confirmed later edit retains chronology. @param root Owned fixture. */
void SuppressedAndConfirmed(Root& root){Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;bool suppressed{};A::Hook([&](const A::CutObservation& cut){if(cut.cut==A::Cut::AfterPublicationEffect&&cut.result.lastNativeReturn==W::NativeReturn::Succeeded){NativeCompleted(cut.result,W::EffectKind::PublishRename);suppressed=true;return A::CutAction::SuppressNextObservation;}return A::CutAction::Continue;});const auto unknown=W::Publish(session.turn,intent,receipt);A::Hook({});NativeCompleted(unknown,W::EffectKind::PublishRename);Check(suppressed&&unknown.kind==W::Kind::Unknown&&!unknown.publicationConfirmed&&unknown.lastNativeReturn==W::NativeReturn::Succeeded&&unknown.nativeError==0&&unknown.fixtureObservationUnavailable&&unknown.observationError==0,"suppressed proof is not fake OS failure or historical success");const auto repaired=W::Reconcile(session.turn,id);Check(repaired.publicationConfirmed&&repaired.publicationCalls==1,"actual later positive original observation");Write(root.path,session.path,"later confirmed edit");const auto changed=W::Observe(session.turn,receipt);Check(changed.result.history==W::History::Published&&changed.result.publicationConfirmed&&changed.result.current==W::Current::Conflict,"previously confirmed then external edit differs from first-proof Unknown");const auto failed=W::Compensate(session.turn,receipt);Check(!failed.compensationConfirmed&&failed.compensationCalls==0,"changed candidate cannot be destructively compensated");session.Settle(id);session.Close();}
/** @summary Real completed restore success without first positive proof retains compensation uncertainty. @param root Original fixture. */
void CompensationUnknown(Root& root){const std::string previous="original compensation content";Session session(root,&previous);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;session.Publish(intent,receipt);const auto moved=session.parent/L"unconfirmed-restoration.toon";bool cut{};W::FileIdentity restored;A::Hook([&](const A::CutObservation& observation){if(observation.cut==A::Cut::AfterPublicationEffect&&observation.result.lastEffectKind==W::EffectKind::RestoreRename&&observation.result.lastNativeReturn==W::NativeReturn::Succeeded){NativeCompleted(observation.result,W::EffectKind::RestoreRename);cut=true;restored=observation.owned;Move(root.path,session.path,moved);Write(root.path,session.path,"external before restoration proof");}return A::CutAction::Continue;});const auto unknown=W::Compensate(session.turn,receipt);A::Hook({});NativeCompleted(unknown,W::EffectKind::RestoreRename);Check(cut&&unknown.kind==W::Kind::Unknown&&unknown.publicationConfirmed&&!unknown.compensationConfirmed&&unknown.lastNativeReturn==W::NativeReturn::Succeeded,"restore completed NTSTATUS success is separate from confirmed compensation");Check(Identity(moved)==restored&&Read(root.path,moved)==previous,"real unconfirmed restoration object retained");const auto calls=unknown.compensationCalls;Check(W::Settle(session.turn,id).kind!=W::Kind::Ok,"unconfirmed compensation cannot settle");Check(DeleteFileW(session.path.c_str())!=FALSE,"remove only known compensation test replacement");Move(root.path,moved,session.path);const auto confirmed=W::Reconcile(session.turn,id);Check(confirmed.kind==W::Kind::Compensated&&confirmed.compensationConfirmed&&confirmed.publicationConfirmed&&confirmed.compensationCalls==calls,"only original restored identity/default bytes confirms once without replay");session.Settle(id);session.Close();}
/** @summary Native temp-cut exceptions retain actual temp effects and exact cleanup ownership. @param root Owned test roots. */
void TempExceptions(Root& root){{Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;A::Hook([](const A::CutObservation& cut){if(cut.cut==A::Cut::BeforeTempCreate)throw std::runtime_error("before any native effect");return A::CutAction::Continue;});const auto rejected=W::Publish(session.turn,intent,receipt);A::Hook({});Check(rejected.kind==W::Kind::Rejected&&rejected.temporaryCreateCalls==0&&rejected.temporaryWriteCalls==0&&rejected.publicationCalls==0,"only genuinely pre-effect exception reports no filesystem effects");session.Settle(id);session.Close();}for(bool verified:{false,true}){Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;std::filesystem::path temp;bool cut{};A::Hook([&](const A::CutObservation& observation){if(observation.cut==(verified?A::Cut::AfterTempVerified:A::Cut::AfterTempIdentity)){cut=true;temp=session.parent/observation.temporaryLeaf.data();Check(Identity(temp)==observation.owned,"actual created temp identity before hook exception");throw std::runtime_error("owned native temp exception");}return A::CutAction::Continue;});const auto result=W::Publish(session.turn,intent,receipt);A::Hook({});Report("temp exception",result);Check(cut&&result.temporaryCreateCalls>=1&&result.publicationCalls==0&&(!verified||result.temporaryWriteCalls>=1),"zero publication does not erase real temp effects");Check(W::CancelPrepared(session.turn,id).kind!=W::Kind::Ok,"temp effect cannot be cancelled as NoEffects");auto known=result;if(known.kind==W::Kind::Unknown)known=W::Reconcile(session.turn,id);Check(known.kind==W::Kind::Rejected&&known.cleanup==W::Cleanup::AbsenceConfirmed,"only positively owned temporary cleanup settles exception");Check(GetFileAttributesW(temp.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"actual owned temporary absent");session.Settle(id);session.Close();}}
/** @summary A moved/replaced temp followed by throw preserves foreign spelling and original quarantine. @param root Original roots. */
void TempReplacement(Root& root){Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;std::filesystem::path temp;const auto moved=session.parent/L"retained-original.tmp";W::FileIdentity original;A::Hook([&](const A::CutObservation& cut){if(cut.cut==A::Cut::AfterTempVerified){temp=session.parent/cut.temporaryLeaf.data();original=cut.owned;Move(root.path,temp,moved);Write(root.path,temp,"foreign temp replacement");throw std::runtime_error("changed original temp location");}return A::CutAction::Continue;});const auto result=W::Publish(session.turn,intent,receipt);A::Hook({});Check(result.kind==W::Kind::Unknown&&!result.publicationConfirmed&&result.publicationCalls==0&&result.temporaryWriteCalls>=1,"moved temp uncertainty retains actual earlier effects");Check(Identity(moved)==original&&Read(root.path,temp)=="foreign temp replacement","foreign temp spelling never deleted");intent={};receipt={};Check(W::Operations(session.turn).count==1&&A::Inspect(session.owner).thisOwnerSlotCount==1,"temp Unknown survives last view drop");Check(W::Settle(session.turn,id).kind!=W::Kind::Ok,"temp Unknown not evicted");Check(DeleteFileW(temp.c_str())!=FALSE,"remove exact owned test replacement");Move(root.path,moved,temp);const auto recovered=W::Reconcile(session.turn,id);Check(recovered.kind==W::Kind::Rejected&&recovered.cleanup==W::Cleanup::AbsenceConfirmed,"original still-temp positive proof permits exact cleanup");session.Settle(id);session.Close();}

/** @summary Actual named-stream admission and per-effect revalidation protect ordinary base paths. @param root Native root. */
void Streams(Root& root){const auto parent=root.Case(),path=parent/L"state.toon";Write(root.path,path,"default");Stream(root.path,path,true);W::Owner denied;Check(A::Open(path,denied).kind==W::Kind::Rejected,"ordinary pathname with genuine named stream rejected");Check(Read(root.path,path)=="default","stream admission preserved default bytes");StreamReadback(root.path,path);Stream(root.path,path,false);
    for(bool absence:{false,true}){const std::string previous="previous";Session session(root,absence?nullptr:&previous);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;session.Publish(intent,receipt);Stream(root.path,session.path,true);const auto before=Read(root.path,session.path);const auto failed=W::Compensate(session.turn,receipt);Check(failed.compensationCalls==0&&!failed.compensationConfirmed&&Read(root.path,session.path)==before,"named stream blocks both restore and absence compensation");const auto observed=W::Observe(session.turn,receipt);Check(observed.result.current==W::Current::Conflict||observed.result.current==W::Current::Unknown,"fresh observation includes actual stream profile");StreamReadback(root.path,session.path);Stream(root.path,session.path,false);session.Settle(id);session.Close();}
    Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;std::filesystem::path temp;bool added{};A::Hook([&](const A::CutObservation& cut){if(cut.cut==A::Cut::AfterTempVerified){temp=session.parent/cut.temporaryLeaf.data();Stream(root.path,temp,true);added=true;}return A::CutAction::Continue;});const auto result=W::Publish(session.turn,intent,receipt);A::Hook({});Check(added&&result.kind==W::Kind::Unknown&&result.publicationCalls==0,"changed owned temp stream quarantines before rename/cleanup");Check(GetFileAttributesW(temp.c_str())!=INVALID_FILE_ATTRIBUTES,"named-stream temp not destructively cleaned");StreamReadback(root.path,temp);Stream(root.path,temp,false);const auto recovered=W::Reconcile(session.turn,id);Check(recovered.kind==W::Kind::Rejected&&recovered.cleanup==W::Cleanup::AbsenceConfirmed,"restored exact temp profile permits positive cleanup");session.Settle(id);session.Close();}
/** @summary Parse malformed stream data through the real bounded native parser without claiming filesystem execution. */
void StreamParser(){constexpr wchar_t name[]=L"::$DATA";std::vector<std::byte> bytes(offsetof(FILE_STREAM_INFO,StreamName)+(std::size(name)-1)*sizeof(wchar_t));auto* stream=reinterpret_cast<FILE_STREAM_INFO*>(bytes.data());stream->StreamNameLength=static_cast<DWORD>((std::size(name)-1)*sizeof(wchar_t));std::memcpy(stream->StreamName,name,stream->StreamNameLength);Check(A::ParseStreams(bytes)==W::QueryDisposition::Confirmed,"actual parser default-stream positive");stream->StreamNameLength=1;Check(A::ParseStreams(bytes)==W::QueryDisposition::Malformed,"odd native stream name length rejected");stream->StreamNameLength=static_cast<DWORD>((std::size(name)-1)*sizeof(wchar_t));stream->NextEntryOffset=1;Check(A::ParseStreams(bytes)==W::QueryDisposition::Malformed,"unaligned native stream link rejected");std::vector<std::byte> oversize(65537);Check(A::ParseStreams(oversize)==W::QueryDisposition::Oversize,"native stream parser fixed64KiB limit");}

/** @summary Record retained case-fixture identity and actual flags without changing stream formatting.
 * @param phase Fixed observation phase. @param path Exact retained directory.
 * @param identity Copied native identity. @param flags Actual case-sensitive flags. */
void CaseFixtureEvidence(const char* phase,const std::filesystem::path& path,const JunctionIdentity& identity,DWORD flags){
    constexpr char hex[]="0123456789abcdef";std::array<char,33> file{};
    for(std::size_t i=0;i<identity.file.size();++i){file[i*2]=hex[identity.file[i]>>4];file[i*2+1]=hex[identity.file[i]&15];}
    std::cout<<"retained case-sensitive fixture="<<path.string()<<" phase="<<phase<<" volume="<<identity.volume<<" file="<<file.data()<<" flags="<<flags<<std::endl;
}
/** @summary Provision only the mandatory case-sensitive negative in a freshly owned, explicitly supplied test root.
 * @details The audited runner qualifies the exact existing root and its inherited rights. No TEMP fallback,
 * permission change, retry, skip or cleanup is permitted; native identity and flag readback remain mandatory. */
void CaseSensitiveFixture(){
    std::array<wchar_t,4097> text{};
    const auto length=GetEnvironmentVariableW(L"SIMODER_WRITER_CASE_FIXTURE_ROOT",text.data(),static_cast<DWORD>(text.size()));
    Check(length>0&&length<text.size(),"explicit bounded case fixture root required");
    const std::filesystem::path parent(std::wstring(text.data(),length));Plain(parent);
    wchar_t fs[32]{};
    Check(GetDriveTypeW(parent.root_path().c_str())==DRIVE_FIXED,"case fixture fixed-drive profile required before creation");
    Check(GetVolumeInformationW(parent.root_path().c_str(),nullptr,0,nullptr,nullptr,nullptr,fs,32)!=FALSE&&std::wstring(fs)==L"NTFS","case fixture NTFS profile required before creation");
    const auto outer=Directory(parent,L"simoder-writer-case-"+std::to_wstring(GetCurrentProcessId()));
    const auto analysis=Directory(outer,L".analysis");Root isolated(analysis);
    const auto sensitive=Directory(isolated.path,L"case-sensitive");Child(isolated.path,sensitive);
    Check(std::filesystem::is_empty(sensitive),"fresh case-sensitive fixture must be empty");
    const auto original=JunctionTargetIdentity(sensitive);
    Handle directory(CreateFileW(sensitive.c_str(),FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    Check(directory.value!=INVALID_HANDLE_VALUE,"case sensitivity fixture handle");
    Check(JunctionSupport::NodeIdentity(directory.value)==original,"case fixture original provisioning identity");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    Check(GetFileInformationByHandleEx(directory.value,FileAttributeTagInfo,&attributes,sizeof(attributes))!=FALSE&&
        (attributes.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))==FILE_ATTRIBUTE_DIRECTORY,"case fixture original plain directory handle");
    std::cout<<"case fixture original directory attributes="<<attributes.FileAttributes<<" reparseTag="<<attributes.ReparseTag<<std::endl;
    FILE_CASE_SENSITIVE_INFO initial{};
    Check(GetFileInformationByHandleEx(directory.value,FileCaseSensitiveInfo,&initial,sizeof(initial))!=FALSE&&initial.Flags==0,"actual initially case-insensitive fixture required");
    CaseFixtureEvidence("before-set",sensitive,original,initial.Flags);
    FILE_CASE_SENSITIVE_INFO info{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    if(!SetFileInformationByHandle(directory.value,FileCaseSensitiveInfo,&info,sizeof(info))){const auto error=GetLastError();throw std::runtime_error("actual case-sensitive directory fixture required Win32="+std::to_string(error));}
    FILE_CASE_SENSITIVE_INFO applied{};
    Check(GetFileInformationByHandleEx(directory.value,FileCaseSensitiveInfo,&applied,sizeof(applied))!=FALSE&&applied.Flags==FILE_CS_FLAG_CASE_SENSITIVE_DIR,"actual case-sensitive flag readback required");
    Check(JunctionSupport::NodeIdentity(directory.value)==original,"case SET preserved original native identity");
    CaseFixtureEvidence("after-set",sensitive,original,applied.Flags);
    directory.Close();W::Owner cases;
    Check(A::Open(sensitive/L"state.toon",cases).kind==W::Kind::Rejected,"actual case-sensitive profile rejected");
    Plain(sensitive);JunctionHandle retained(JunctionSupport::OpenNode(sensitive));
    Check(JunctionSupport::NodeIdentity(retained.value)==original,"rejected writer preserved original case directory identity");
    FILE_CASE_SENSITIVE_INFO after{};
    Check(GetFileInformationByHandleEx(retained.value,FileCaseSensitiveInfo,&after,sizeof(after))!=FALSE&&after.Flags==applied.Flags,"rejected writer preserved actual case-sensitive flag");
    Check(GetFileAttributesW((sensitive/L"state.toon").c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"rejected case-sensitive target remains absent");
    Check(std::filesystem::is_empty(sensitive),"rejected writer left case-sensitive directory empty");
    CaseFixtureEvidence("after-rejection",sensitive,original,after.Flags);
}
/** @summary Same-key native aliases share admission; unsupported target/path kinds fail before effects. @param root Native test root. */
void Qualification(Root& root){const std::string prior="alias";Session session(root,&prior);W::Owner alias;
    {
        // @summary Keep the genuine admissible prefix alive while testing complete, length-bearing malformed native strings.
        const auto prefixIdentity=Identity(session.path);const auto before=A::Inspect(session.owner);
        auto embeddedNul=session.path.native();embeddedNul.push_back(L'\0');embeddedNul+=L".ignored";
        const auto high=static_cast<wchar_t>(0xD800),low=static_cast<wchar_t>(0xDC00);
        const std::array<std::wstring,18> invalidText{embeddedNul,session.path.native()+std::wstring(1,high),
            session.path.native()+std::wstring{high,L'x'},session.path.native()+std::wstring(1,low),
            session.path.native()+std::wstring{low,high},session.path.native()+L'.',session.path.native()+L' ',
            (std::filesystem::path(session.parent.native()+L'.')/session.path.filename()).native(),
            (std::filesystem::path(session.parent.native()+L' ')/session.path.filename()).native(),
            L":state.toon",L"1:\\state.toon",session.path.root_name().native()+L"state.toon",L"\\state.toon",
            session.path.native()+L":stream",L"\\\\simoder-invalid\\share\\state.toon",L"\\\\?\\"+session.path.native(),
            (session.parent/L".."/session.parent.filename()/session.path.filename()).native(),(session.parent/L"NUL").native()};
        for(const auto& text:invalidText){
            const std::filesystem::path invalid{text};Check(invalid.native()==text,"malformed path fixture preserves every supplied UTF16 code unit");
            W::Owner rejected;const auto result=A::Open(invalid,rejected);
            Check(result.kind==W::Kind::Rejected&&!rejected,"complete malformed text or forbidden namespace rejects before owner admission");
            Check(result.temporaryCreateCalls==0&&result.temporaryWriteCalls==0&&result.publicationCalls==0&&
                result.compensationCalls==0&&result.cleanupDispositionCalls==0&&result.lastNativeReturn==W::NativeReturn::NotCalled,
                "malformed path rejects without temporary, publication, compensation or cleanup effects");
            const auto after=A::Inspect(session.owner);
            Check(after.ownerPoolOccupied==before.ownerPoolOccupied&&after.globalOperationPoolOccupied==before.globalOperationPoolOccupied&&
                after.rawBytesReserved==before.rawBytesReserved&&after.thisOwnerSlotCount==before.thisOwnerSlotCount,
                "malformed path rejection releases provisional owner and creates no operation charge");
            Check(Identity(session.path)==prefixIdentity&&Read(root.path,session.path)==prior,
                "every rejected spelling preserves the genuine valid sentinel identity and bytes");
        }
        // @summary A real NTFS file containing BMP Unicode and a supplementary scalar must remain admissible without a skip or fallback.
        std::wstring unicodeLeaf=L"state-\u0105-";unicodeLeaf.push_back(static_cast<wchar_t>(0xD83D));
        unicodeLeaf.push_back(static_cast<wchar_t>(0xDE80));unicodeLeaf+=L".toon";
        const auto unicodePath=root.Case()/unicodeLeaf;const std::string unicodeBytes="native Unicode path control";
        Check(unicodePath.filename().native()==unicodeLeaf,"positive path retains BMP and supplementary UTF16 units");
        Write(root.path,unicodePath,unicodeBytes);const auto unicodeIdentity=Identity(unicodePath);
        W::Owner unicodeOwner;Check(A::Open(unicodePath,unicodeOwner).kind==W::Kind::Ok,"actual valid surrogate pair filename qualifies");
        Check(Identity(unicodePath)==unicodeIdentity&&Read(root.path,unicodePath)==unicodeBytes,"Unicode admission preserves actual file identity and bytes");
        Check(W::CloseOwner(unicodeOwner).kind==W::Kind::Closed,"Unicode positive owner explicitly closes");
        // @summary Reject an absent ambiguous tilde leaf, then prove the same existing literal leaf through actual native long-name qualification.
        const auto tildePath=root.Case()/L"literal~1.toon";const std::string tildeBytes="existing literal tilde control";
        Check(GetFileAttributesW(tildePath.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"actual tilde leaf starts absent");
        W::Owner absentTilde;const auto beforeTilde=A::Inspect(session.owner);const auto absentResult=A::Open(tildePath,absentTilde);
        Check(absentResult.kind==W::Kind::Rejected&&!absentTilde&&absentResult.temporaryCreateCalls==0&&absentResult.temporaryWriteCalls==0&&
            absentResult.publicationCalls==0&&absentResult.compensationCalls==0&&absentResult.cleanupDispositionCalls==0&&
            absentResult.lastNativeReturn==W::NativeReturn::NotCalled,"absent tilde leaf rejected without effects or owner authority");
        const auto afterTilde=A::Inspect(session.owner);
        Check(afterTilde.ownerPoolOccupied==beforeTilde.ownerPoolOccupied&&afterTilde.globalOperationPoolOccupied==beforeTilde.globalOperationPoolOccupied&&
            afterTilde.rawBytesReserved==beforeTilde.rawBytesReserved&&GetFileAttributesW(tildePath.c_str())==INVALID_FILE_ATTRIBUTES&&
            GetLastError()==ERROR_FILE_NOT_FOUND,"absent tilde rejection preserves absence and releases provisional capacity");
        Write(root.path,tildePath,tildeBytes);const auto tildeIdentity=Identity(tildePath);W::Owner literalTilde;
        Check(A::Open(tildePath,literalTilde).kind==W::Kind::Ok,"same existing literal tilde leaf passes genuine normalized long-name check");
        Check(Identity(tildePath)==tildeIdentity&&Read(root.path,tildePath)==tildeBytes,"existing literal tilde admission preserves native identity and bytes");
        Check(W::CloseOwner(literalTilde).kind==W::Kind::Closed,"literal tilde positive owner explicitly closes");
        W::Owner dotOwner;W::Turn dotTurn;const auto dotPath=session.parent/L"."/session.path.filename();
        Check(A::Open(dotPath,dotOwner).kind==W::Kind::Ok&&W::TryTurn(dotOwner,dotTurn).kind==W::Kind::Busy,
            "allowed lexical dot converges on the real existing owner and original turn");
    }
    // @summary Exercise a real filename-relative alias without the explicitly forbidden parent traversal.
    // @details Prepare all potentially throwing values before changing this single-threaded fixture's cwd.
    const auto originalDirectory=std::filesystem::current_path();
    const auto relative=session.path.filename();
    Child(root.path,session.parent);Plain(session.parent,true);
    Check(!relative.empty()&&!relative.has_parent_path()&&!relative.is_absolute(),"plain relative alias fixture");
    Check(SetCurrentDirectoryW(session.parent.c_str())!=FALSE,"enter owned alias fixture directory");
    const auto relativeResult=A::Open(relative,alias); // @summary Open is noexcept; no throwing operation precedes restoration.
    const bool restored=SetCurrentDirectoryW(originalDirectory.c_str())!=FALSE;
    Check(restored&&std::filesystem::current_path()==originalDirectory,"restore original fixture working directory");
    Check(relativeResult.kind==W::Kind::Ok,"actual relative alias converges");W::Turn busy;Check(W::TryTurn(alias,busy).kind==W::Kind::Busy,"same qualified aliases share original turn");const auto uppercase=session.parent/L"STATE.TOON";W::Owner casing;Check(A::Open(uppercase,casing).kind==W::Kind::Ok&&W::TryTurn(casing,busy).kind==W::Kind::Busy,"ordinal case alias converges");std::array<wchar_t,4097> shortName{};const auto shortLength=GetShortPathNameW(session.path.c_str(),shortName.data(),static_cast<DWORD>(shortName.size()));if(shortLength&&shortLength<shortName.size()&&CompareStringOrdinal(std::filesystem::path(shortName.data()).filename().c_str(),-1,session.path.filename().c_str(),-1,TRUE)!=CSTR_EQUAL){W::Owner shortOwner;Check(A::Open(shortName.data(),shortOwner).kind==W::Kind::Rejected,"actual differing short target alias rejected");}else std::cout<<"target short-name distinction SOURCE_ONLY: no distinct native leaf reported\n";session.Close();
    const auto parent=root.Case(),path=parent/L"state.toon",hard=parent/L"hard.toon";Write(root.path,path,"hardlink");Check(CreateHardLinkW(hard.c_str(),path.c_str(),nullptr)!=FALSE,"actual NTFS hardlink fixture");W::Owner owner;Check(A::Open(path,owner).kind==W::Kind::Rejected&&Read(root.path,path)=="hardlink","actual multiple-link target rejected intact");
    for(const auto& invalid:{parent/L"state.toon:bad",parent/L".."/L"escaped.toon",parent/L"trailing. ",parent/L"CON",parent/L"missing"/L"state.toon"}){W::Owner rejected;Check(A::Open(invalid,rejected).kind==W::Kind::Rejected,"unsupported supplied namespace rejected");}
    const auto target=Directory(parent,L"link-target"),link=parent/L"link",sentinel=target/L"state.toon";
    const std::string sentinelBytes="native writer junction sentinel";Write(root.path,sentinel,sentinelBytes);
    const auto targetIdentity=JunctionTargetIdentity(target);
    W::Owner direct;Check(A::Open(sentinel,direct).kind==W::Kind::Ok,"same plain writer target is genuinely admissible");
    Check(W::CloseOwner(direct).kind==W::Kind::Closed,"plain positive control explicitly closes");direct={};
    unsigned junctionCalls{};const auto junctionIdentity=JunctionSupport::Junction(root.path,link,target,junctionCalls,1);
    W::Owner linked;Check(A::Open(link/L"state.toon",linked).kind==W::Kind::Rejected,"actual reparse ancestor rejected");
    Check(JunctionTargetIdentity(target)==targetIdentity&&Read(root.path,sentinel)==sentinelBytes,"reparse rejection preserves original target identity and bytes");
    JunctionSupport::RemoveJunction(root.path,link,target,junctionIdentity);
    Check(junctionCalls==1&&JunctionTargetIdentity(target)==targetIdentity&&Read(root.path,sentinel)==sentinelBytes,"exact junction removal preserves original target and single SET budget");
    std::cout<<"writer generic reparse guard: actual mount point; symbolic-link tag NOT_RUN\n";
    CaseSensitiveFixture();}
/** @summary Compare actual legacy/shared canonical bytes and strict empty/invalid/duplicate behavior. @param root Owned roots. */
void Serializer(Root& root){for(bool empty:{true,false}){Session session(root);const std::array<sc13::mods::ModId,2> ids{"fixture.first","fixture.second"};const auto enabled=empty?std::span<const sc13::mods::ModId>{}:std::span<const sc13::mods::ModId>{ids};W::Intent intent;const auto prepared=W::Prepare(session.turn,enabled,intent);Check(prepared.kind==W::Kind::Ok,"shared serializer valid input");const auto id=prepared.operation;W::Receipt receipt;session.Publish(intent,receipt);const auto legacy=session.parent/L"legacy.toon";std::string error;Check(sc13::mods::SaveEnabledState(legacy,enabled,error),"legacy unchanged serializer succeeds");Check(Read(root.path,legacy)==Read(root.path,session.path),"legacy and canonical exact enabled order/bytes");if(empty)Check(Read(root.path,session.path)=="enabled[0]:\n","empty serialization unchanged");session.Settle(id);session.Close();}
    Session session(root);for(const auto& ids:{std::array<sc13::mods::ModId,2>{"fixture.same","fixture.same"},std::array<sc13::mods::ModId,2>{"fixture.valid","Invalid ID"}}){W::Intent intent;const auto result=W::Prepare(session.turn,ids,intent);Check(result.kind==W::Kind::Rejected&&result.temporaryCreateCalls==0&&result.publicationCalls==0&&A::Inspect(session.owner).thisOwnerSlotCount==0,"duplicate/invalid input rejected before any temp or slot");}session.Close();}
/** @summary Real sharing contention preserves the exact unchanged native retry policy. @param root Native root. */
void Sharing(Root& root){using sc13::platform::FilePublicationRetryDelay;Check(sc13::platform::FilePublicationMaximumAttempts==11&&sc13::platform::FilePublicationBudgetMs==500,"shared legacy retry limits unchanged");Check(FilePublicationRetryDelay(5,1,0)==50&&FilePublicationRetryDelay(32,10,499)==1&&FilePublicationRetryDelay(5,11,0)==0&&FilePublicationRetryDelay(32,1,500)==0&&FilePublicationRetryDelay(87,1,0)==0,"same closed5/32 retry policy");
    for(bool transient:{true,false}){const std::string previous="real sharing prior";Session session(root,&previous);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;Handle reader(CreateFileW(session.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Check(reader.value!=INVALID_HANDLE_VALUE,"genuine deny-delete reader opened");std::promise<void> failed;auto signal=failed.get_future();std::atomic_uint failures{};std::atomic_bool codesValid{true};auto publish=[&]{A::Hook([&](const A::CutObservation& cut){if(cut.cut==A::Cut::AfterPublicationEffect&&cut.result.lastNativeReturn==W::NativeReturn::Failed){NativeCompleted(cut.result,W::EffectKind::PublishRename,false);const auto& rename=cut.result.rename;const auto count=failures.fetch_add(1)+1;if(!rename.mappedErrorValid||(rename.mappedError!=5&&rename.mappedError!=32)||cut.result.publicationCalls!=count||rename.submission!=cut.result.publicationCalls)codesValid=false;if(count==1)failed.set_value();}return A::CutAction::Continue;});auto result=W::Publish(session.turn,intent,receipt);A::Hook({});return result;};
        W::Result result;
        if(transient){auto action=std::async(std::launch::async,publish);const bool reached=signal.wait_for(2s)==std::future_status::ready;reader.Close();result=action.get();NativeCompleted(result,W::EffectKind::PublishRename);Check(reached,"actual failed native publication observed before reader release");Check(result.kind==W::Kind::Published&&result.publicationConfirmed&&result.publicationCalls>=2,"real sharing release allows original owned-temp retry");}
        else{result=publish();reader.Close();Check(!result.publicationConfirmed&&result.publicationCalls>=1&&Read(root.path,session.path)==previous,"persistent real sharing failure preserves exact prior");}
        Report("sharing",result);Check(failures>=1&&codesValid&&result.publicationCalls<=11&&result.elapsedBackoffMs<=500,"real effect counts and bounded shared backoff, no generic retry");if(result.kind==W::Kind::Unknown)result=W::Reconcile(session.turn,id);Check(result.kind==W::Kind::Published||result.kind==W::Kind::Rejected,"sharing reaches known publish or exact-temp cleanup");session.Settle(id);session.Close();}
}
/** @summary Genuine read-only native disposition failures exercise shared retries after DELETE admission, never a fabricated return. @param root Native evidence root. */
void DispositionRetries(Root& root){
    for(bool transient:{true,false}){
        Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;session.Publish(intent,receipt);
        const auto original=Identity(session.path);const auto candidate=Read(root.path,session.path);unsigned failures{};bool armed{},validErrors{true};
        A::Hook([&](const A::CutObservation& cut){
            if(cut.cut==A::Cut::BeforePublicationEffect&&!armed){
                // @summary This cut occurs after the genuine compensation DELETE handle was opened; no sharing-open failure can impersonate a disposition call.
                Child(root.path,session.path);Plain(session.path,false);Check(SetFileAttributesW(session.path.c_str(),FILE_ATTRIBUTE_READONLY)!=FALSE,"actual candidate read-only attribute set after native DELETE handle admission");armed=true;
            }
            if(cut.cut==A::Cut::AfterDispositionEffect&&cut.result.lastNativeReturn==W::NativeReturn::Failed){
                ++failures;validErrors=validErrors&&(cut.result.nativeError==ERROR_ACCESS_DENIED||cut.result.nativeError==ERROR_SHARING_VIOLATION);
                Check(cut.result.compensationCalls==failures&&cut.result.lastEffectKind==W::EffectKind::RemovePublishedCandidate,"actual failed SetFileInformationByHandle disposition counter, not open failure");
                Check(Identity(session.path)==original&&Read(root.path,session.path)==candidate,"failed actual disposition retains exact native candidate and bytes");
                if(transient)Check(SetFileAttributesW(session.path.c_str(),FILE_ATTRIBUTE_NORMAL)!=FALSE,"release actual read-only failure after native FALSE observation");
            }
            return A::CutAction::Continue;
        });
        const auto result=W::Compensate(session.turn,receipt);A::Hook({});
        // @summary Restore only this known fixture attribute before assertions, including a profile that unexpectedly rejected before the actual syscall.
        const auto attributes=GetFileAttributesW(session.path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES)Check(SetFileAttributesW(session.path.c_str(),FILE_ATTRIBUTE_NORMAL)!=FALSE,"restore only surviving known fixture attributes");
        Report("actual disposition retries",result);
        Check(armed&&failures>=1&&validErrors,"mandatory native host qualification: actual disposition FALSE5/32 must be reached; unsupported profile blocks rather than skips");
        Check(result.compensationCalls<=11&&result.elapsedBackoffMs<=500,"actual disposition shares unchanged11call500ms backoff bounds");
        if(transient){Check(result.kind==W::Kind::Compensated&&result.compensationConfirmed&&result.compensationCalls>=2&&result.lastNativeReturn==W::NativeReturn::Succeeded,"actual FALSE then attribute release permits one positively confirmed native deletion");Check(GetFileAttributesW(session.path.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"transient failed disposition ultimately restores genuine absence");}
        else{Check(result.kind==W::Kind::Rejected&&result.history==W::History::CompensationFailed&&!result.compensationConfirmed&&result.lastNativeReturn==W::NativeReturn::Failed,"persistent actual FALSE remains known compensation failure without fabricated success");Check(result.compensationCalls==failures&&Identity(session.path)==original&&Read(root.path,session.path)==candidate,"every persistent native disposition failure counted and original candidate retained");}
        session.Settle(id);session.Close();
    }
}
/** @summary Genuine external pre-effect edits prevent overwrite and retain original temporary obligations. @param root Original owned roots. */
void ExternalPreflight(Root& root){const std::string previous="prior exact external";Session session(root,&previous);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;bool changed{};A::Hook([&](const A::CutObservation& cut){if(cut.cut==A::Cut::BeforePublicationEffect&&!changed){changed=true;Write(root.path,session.path,"external before rename");}return A::CutAction::Continue;});const auto result=W::Publish(session.turn,intent,receipt);A::Hook({});Check(changed&&result.publicationCalls==0&&!result.publicationConfirmed&&Read(root.path,session.path)=="external before rename","real final preflight blocks overwrite of changed destination");if(result.kind==W::Kind::Unknown){Write(root.path,session.path,previous);const auto recovered=W::Reconcile(session.turn,id);Check(recovered.kind==W::Kind::Rejected,"restored same original prior permits exact temp reconciliation only");}session.Settle(id);session.Close();}
/** @summary Unique CREATE_NEW collision attempts never truncate or delete foreign objects. @param root Native root. */
void Collisions(Root& root){Session session(root);W::Intent intent;const auto id=session.Prepare(intent);W::Receipt receipt;std::vector<std::filesystem::path> collided;A::Hook([&](const A::CutObservation& cut){if(cut.cut==A::Cut::BeforeTempCreate){const auto path=session.parent/cut.temporaryLeaf.data();Write(root.path,path,"foreign collision");collided.push_back(path);}return A::CutAction::Continue;});const auto result=W::Publish(session.turn,intent,receipt);A::Hook({});Check(!collided.empty()&&collided.size()<=8&&result.temporaryCreateCalls<=8&&result.publicationCalls==0&&!result.publicationConfirmed,"bounded actual CREATE_NEW collision calls");for(const auto& path:collided)Check(Read(root.path,path)=="foreign collision","foreign temp collision remains exact");Check(result.kind==W::Kind::Rejected,"collision exhaustion is known no-owned-temp rejection");session.Settle(id);session.Close();}
/** @summary Newer cooperative operation permanently supersedes old compensation authority. @param root Native roots. */
void Supersession(Root& root){const std::string previous="original";Session session(root,&previous);W::Intent first,second;const auto a=session.Prepare(first,"fixture.first");W::Receipt old;const auto oldResult=session.Publish(first,old);const auto b=session.Prepare(second,"fixture.second");W::Receipt current;const auto now=session.Publish(second,current);const auto bytes=Read(root.path,session.path);const auto refused=W::Compensate(session.turn,old);Check(refused.compensationCalls==0&&!refused.compensationConfirmed&&Read(root.path,session.path)==bytes,"superseded receipt cannot overwrite newer cooperative effect");Check(now.latestEffectSequence!=oldResult.latestEffectSequence,"actual latest cooperative native identity advanced");session.Settle(a);session.Settle(b);session.Close();}
/** @summary Raw previous and canonical candidate bounds are exact, not truncation. @param root Native root. */
void RawLimits(Root& root){const std::string exact(1024U*1024U,'x');{Session session(root,&exact);W::Intent intent;const auto id=session.Prepare(intent);Check(W::CancelPrepared(session.turn,id).kind==W::Kind::Ok,"exact1MiB previous captured without normalization");session.Close();}
    const std::string oversized(1024U*1024U+1,'x');{Session session(root,&oversized);W::Intent intent;const std::array<sc13::mods::ModId,1> enabled{"fixture.limit"};const auto result=W::Prepare(session.turn,enabled,intent);Check(result.kind==W::Kind::Rejected&&Read(root.path,session.path)==oversized&&A::Inspect(session.owner).thisOwnerSlotCount==0,"previous1MiB+1 rejects without truncation/effects");session.Close();}
    Session session(root);std::vector<sc13::mods::ModId> enabled;std::size_t bytes=0;for(unsigned i=0;bytes<=1024U*1024U;++i){auto id="fixture.limit"+std::to_string(i);bytes+=id.size()+1;enabled.push_back(std::move(id));}W::Intent intent;const auto rejected=W::Prepare(session.turn,enabled,intent);Check(rejected.kind==W::Kind::Rejected&&rejected.publicationCalls==0&&A::Inspect(session.owner).thisOwnerSlotCount==0,"actual valid unique candidate above1MiB rejected before effects");while(bytes+11+std::to_string(enabled.size()).size()>1024U*1024U){bytes-=enabled.back().size()+1;enabled.pop_back();}const auto nearLimit=W::Prepare(session.turn,enabled,intent);Check(nearLimit.kind==W::Kind::Ok&&nearLimit.temporaryCreateCalls==0,"largest generated valid candidate below1MiB admitted without effects");Check(W::CancelPrepared(session.turn,nearLimit.operation).kind==W::Kind::Ok,"near-limit prepared buffer explicitly cancelled");session.Close();}
/** @summary Shared local/global payload limits retain charges through cancelled caller views. @param root Native roots. */
void OperationCapacity(Root& root){std::vector<std::unique_ptr<Session>> owners;std::vector<W::Intent> intents;std::vector<W::OperationIdentity> ids;for(unsigned i=0;i!=4;++i){owners.push_back(std::make_unique<Session>(root));for(unsigned j=0;j!=2;++j){W::Intent intent;ids.push_back(owners.back()->Prepare(intent));intents.push_back(std::move(intent));}}const auto full=A::Inspect(owners.front()->owner);Check(full.globalOperationPoolOccupied==8&&full.rawBytesReserved==16U*1024U*1024U&&full.thisOwnerSlotCount==2,"actual8global2local16MiB raw pools");W::Intent excess;const std::array<sc13::mods::ModId,1> enabled{"fixture.excess"};Check(W::Prepare(owners.front()->turn,enabled,excess).kind!=W::Kind::Ok,"third local operation denied");Session ninth(root);Check(W::Prepare(ninth.turn,enabled,excess).kind!=W::Kind::Ok,"ninth global record denied before effect");ninth.Close();for(unsigned i=0;i!=8;++i)Check(W::CancelPrepared(owners[i/2]->turn,ids[i]).kind==W::Kind::Ok,"exact untouched operation explicitly cancelled");Check(A::Inspect(owners.front()->owner).globalOperationPoolOccupied==8,"cancelled caller views keep native payload charge");intents.clear();Check(A::Inspect(owners.front()->owner).globalOperationPoolOccupied==0,"last actual view disposal releases raw pool");for(auto& owner:owners)owner->Close();std::cout<<"writer native metadata owner="<<full.ownerBytes<<" record="<<full.recordBytes<<" nativeRenameContext="<<full.nativeRenameContextBytes<<" renameEvidence="<<full.renameEvidenceBytes<<" raw="<<full.rawBytesReserved<<std::endl;}
/** @summary Terminal records retain the same two-per-owner capacity until every actual Intent and Receipt copy is dropped. @param root Native evidence roots. */
void TerminalViewCapacity(Root& root){
    for(bool settled:{false,true}){
        Session session(root);W::Intent first,second;W::Receipt firstReceipt,secondReceipt;
        const auto firstId=session.Prepare(first,"fixture.first");
        if(settled){session.Publish(first,firstReceipt);session.Settle(firstId);}else Check(W::CancelPrepared(session.turn,firstId).kind==W::Kind::Ok,"first actual prepared operation cancelled");
        const auto secondId=session.Prepare(second,"fixture.second");
        if(settled){session.Publish(second,secondReceipt);session.Settle(secondId);}else Check(W::CancelPrepared(session.turn,secondId).kind==W::Kind::Ok,"second actual prepared operation cancelled");
        auto firstCopy=first,firstLast=first;auto receiptCopy=firstReceipt,receiptLast=firstReceipt;
        const auto full=A::Inspect(session.owner);
        Check(W::Operations(session.turn).count==0&&full.thisOwnerSlotCount==2&&full.globalOperationPoolOccupied==2&&full.rawBytesReserved==4U*1024U*1024U,"terminal ledger detached while both original local and global native charges remain; global capacity is not exhausted");
        const std::array<sc13::mods::ModId,1> enabled{"fixture.third"};W::Intent third;
        const auto denied=[&]{const auto result=W::Prepare(session.turn,enabled,third);Check(result.kind==W::Kind::Rejected&&result.temporaryCreateCalls==0&&result.publicationCalls==0&&A::Inspect(session.owner).thisOwnerSlotCount==2&&A::Inspect(session.owner).globalOperationPoolOccupied==2,"third same-owner terminal record rejected despite six globally free slots");};
        denied();first={};firstReceipt={};firstCopy={};denied();
        if(settled){firstLast={};denied();receiptCopy={};denied();receiptLast={};}else firstLast={};
        Check(A::Inspect(session.owner).thisOwnerSlotCount==1&&A::Inspect(session.owner).globalOperationPoolOccupied==1,"only actual last first-record view releases one local/global native charge");
        const auto admitted=W::Prepare(session.turn,enabled,third);Check(admitted.kind==W::Kind::Ok&&A::Inspect(session.owner).thisOwnerSlotCount==2,"same owner admits replacement only after actual record destruction");
        Check(W::CancelPrepared(session.turn,admitted.operation).kind==W::Kind::Ok,"replacement operation explicitly cancelled");third={};second={};secondReceipt={};
        Check(A::Inspect(session.owner).thisOwnerSlotCount==0&&A::Inspect(session.owner).globalOperationPoolOccupied==0&&A::Inspect(session.owner).rawBytesReserved==0,"all terminal Intent and Receipt copies finally release exact native capacity");session.Close();
    }
}
/** @summary Closed retained owner metadata remains within the same32 pool. @param root Native roots. */
void OwnerCapacity(Root& root){std::vector<W::Owner> owners;for(unsigned i=0;i!=32;++i){const auto parent=root.Case();W::Owner owner;Check(A::Open(parent/L"state.toon",owner).kind==W::Kind::Ok,"actual bounded owner admission");Check(W::CloseOwner(owner).kind==W::Kind::Closed,"owner explicitly closed without unresolved operation");owners.push_back(std::move(owner));}const auto parent=root.Case();W::Owner excess;Check(A::Open(parent/L"state.toon",excess).kind!=W::Kind::Ok,"closed retained metadata cannot allocate33rd owner");const auto generation=W::HistoryView(W::Receipt{});Check(generation.kind==W::Kind::Rejected,"empty history grants no identity");owners.clear();Check(A::Open(parent/L"state.toon",excess).kind==W::Kind::Ok,"actual final closed reference release returns bounded owner capacity");Check(W::CloseOwner(excess).kind==W::Kind::Closed,"new owner closes explicitly");}
}
/** @summary Execute isolated real-native writer qualification only after independent draft promotion. @param argc One root-owned existing analysis parent. @param argv Native evidence parent. @return Zero on every mandatory actual-host oracle. */
int main(int argc,char** argv){try{Check(argc==2,"existing declared analysis parent argument required");Root root{std::filesystem::path(argv[1])};
    /** @summary Flush fixed scenario identity before actual native effects. @param label Test name. @param body Isolated native test. */
    const auto run=[](const char* label,const auto& body){std::cerr<<"state writer begin "<<label<<std::endl;body();};
    run("mandatory native positive/history",[&]{PositiveAndHistory(root);});run("absence delete pending",[&]{AbsenceAndPending(root);});run("real post-success unknown",[&]{UnknownRealConflict(root);});run("suppressed versus confirmed",[&]{SuppressedAndConfirmed(root);});run("unconfirmed compensation",[&]{CompensationUnknown(root);});run("temp exceptions",[&]{TempExceptions(root);});run("temp replacement",[&]{TempReplacement(root);});run("native streams",[&]{Streams(root);});run("stream parser",StreamParser);run("qualification",[&]{Qualification(root);});run("shared serializer",[&]{Serializer(root);});run("real sharing",[&]{Sharing(root);});run("actual disposition retries",[&]{DispositionRetries(root);});run("external preflight",[&]{ExternalPreflight(root);});run("native collisions",[&]{Collisions(root);});run("supersession",[&]{Supersession(root);});run("raw limits",[&]{RawLimits(root);});run("operation pools",[&]{OperationCapacity(root);});run("terminal view capacity",[&]{TerminalViewCapacity(root);});run("owner pool",[&]{OwnerCapacity(root);});std::cout<<checks<<" state writer native checks passed\n";return 0;
}catch(const std::exception& error){A::Hook({});std::cerr<<error.what()<<'\n';return 1;}}
