/** @file @summary Actual retained Windows component-store primitives; no installer policy, authority issuer or recursive cleanup. */
#include "windows_store.hpp"
#if !defined(_WIN32)
#error This source implements the Windows backend only.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>
#undef CreateFile
#undef CreateDirectory
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>

namespace sc13::platform::component_store {
namespace {
using Path=std::filesystem::path;
constexpr std::uint64_t MaximumFile=17ULL*1024*1024;
/** @summary Bound backend locators independently of approved payload-relative240 and native absolute220 policies. */
constexpr std::size_t MaximumManagedLocator=1024;
/** @summary Own only an original synchronous kernel handle; closing never deletes filesystem paths. */
struct Handle final {
    HANDLE value{INVALID_HANDLE_VALUE};
    /** @summary Initialize an inert original handle slot. */
    Handle()noexcept=default;
    /** @summary Adopt one actual opened original, never a borrowed handle. */
    explicit Handle(HANDLE h)noexcept:value(h){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    /** @summary Transfer the same local original without reopening a path. */
    Handle(Handle&& other)noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
    /** @summary Close only this original handle off all external guards. */
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
/** @summary Construct an exact non-authorizing local result. */
Result Outcome(Status status,Effect effect=Effect::None,Durability durability=Durability::None,DWORD error=0)noexcept{return {status,effect,durability,error};}
/** @summary Classify only actual pre-effect Windows refusal; no path readback guesses whether a rename happened. */
Result Error(DWORD error)noexcept{
    if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return Outcome(Status::Absent,Effect::None,Durability::None,error);
    if(error==ERROR_SHARING_VIOLATION||error==ERROR_LOCK_VIOLATION)return Outcome(Status::Busy,Effect::None,Durability::None,error);
    if(error==ERROR_ALREADY_EXISTS||error==ERROR_FILE_EXISTS)return Outcome(Status::Conflict,Effect::None,Durability::None,error);
    return Outcome(Status::IoError,Effect::None,Durability::None,error);
}
/** @summary Strictly decode caller UTF-8; never replace malformed text or embedded NUL. */
bool Wide(std::string_view text,std::wstring& output){
    output.clear();if(text.empty()||text.size()>32760||text.find('\0')!=text.npos)return false;
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);if(count<=0)return false;
    output.resize(static_cast<std::size_t>(count));return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),output.data(),count)==count;
}
/** @summary Encode an actual normalized native path without locale-dependent conversion. */
bool Utf8(std::wstring_view text,std::string& output){
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);if(count<=0)return false;
    output.resize(static_cast<std::size_t>(count));return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),output.data(),count,nullptr,nullptr)==count;
}
/** @summary Preserve exact absolute spelling while enabling ordinary Win32 extended-length operations. */
std::wstring Native(const Path& path){return L"\\\\?\\"+path.native();}
/** @summary Reject portable traversal, device/ADS and ambiguous Win32 aliases before any path effect. */
bool Segment(std::wstring_view part){
    if(part.empty()||part.size()>255||part==L"."||part==L".."||part.back()==L'.'||part.back()==L' ')return false;
    for(const auto c:part)if(c<32||c==127||std::wstring_view(L"<>:\"/\\|?*~").find(c)!=std::wstring_view::npos)return false;
    auto stem=std::wstring(part.substr(0,part.find(L'.')));for(auto& c:stem)if(c>=L'a'&&c<=L'z')c-=L'a'-L'A';
    if(stem==L"CON"||stem==L"PRN"||stem==L"AUX"||stem==L"NUL"||stem==L"CONIN$"||stem==L"CONOUT$")return false;
    if(stem.size()==4&&(stem.starts_with(L"COM")||stem.starts_with(L"LPT"))&&stem[3]>=L'1'&&stem[3]<=L'9')return false;
    return true;
}
/** @summary Decode only a bounded canonical relative locator; the caller still supplies package/metadata policy. */
bool Relative(std::string_view input,Path& output){
    std::wstring text;if(!Wide(input,text)||text.size()>MaximumManagedLocator||text.front()==L'/'||text.find(L'\\')!=text.npos)return false;
    std::size_t start=0;for(std::size_t at=0;at<=text.size();++at)if(at==text.size()||text[at]==L'/'){
        if(!Segment(std::wstring_view(text).substr(start,at-start)))return false;start=at+1;}
    std::replace(text.begin(),text.end(),L'/',L'\\');output=Path(text);return true;
}
/** @summary Bound local fixed-drive absolute syntax without accepting a device namespace or cwd-relative path. */
bool Absolute(std::wstring& text){
    std::replace(text.begin(),text.end(),L'/',L'\\');if(text.size()<4||text.size()>32760||text[1]!=L':'||text[2]!=L'\\')return false;
    if(text[0]>=L'a'&&text[0]<=L'z')text[0]-=L'a'-L'A';if(text[0]<L'A'||text[0]>L'Z')return false;
    std::size_t start=3;for(std::size_t at=3;at<=text.size();++at)if(at==text.size()||text[at]==L'\\'){
        if(!Segment(std::wstring_view(text).substr(start,at-start)))return false;start=at+1;}
    wchar_t drive[]{text[0],L':',L'\\',0};return GetDriveTypeW(drive)==DRIVE_FIXED;
}
/** @summary Capture current ordinary type and stable local object correlation from the SAME original handle. */
bool Info(HANDLE handle,bool directory,Observation& output)noexcept{
    BY_HANDLE_FILE_INFORMATION info{};if(GetFileType(handle)!=FILE_TYPE_DISK||!GetFileInformationByHandle(handle,&info)||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))||bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory||(!directory&&info.nNumberOfLinks!=1))return false;
    output={};output.directory=directory;output.bytes=(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;output.identity.length=12;
    const DWORD words[]{info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow};std::memcpy(output.identity.bytes.data(),words,sizeof(words));return true;
}
/** @summary Refuse named streams instead of treating a default-stream observation as a complete ordinary file. */
bool DefaultStream(HANDLE handle)noexcept{
    alignas(FILE_STREAM_INFO) std::array<std::byte,4096> bytes{};
    if(!GetFileInformationByHandleEx(handle,FileStreamInfo,bytes.data(),static_cast<DWORD>(bytes.size())))return false;
    const auto* value=reinterpret_cast<const FILE_STREAM_INFO*>(bytes.data());return !value->NextEntryOffset&&value->StreamNameLength==14&&std::wstring_view(value->StreamName,7)==L"::$DATA";
}
/** @summary Obtain actual long ordinary spelling for alias refusal, without resolving a caller-supplied reparse target. */
bool FinalPath(HANDLE handle,Path& output){
    std::array<wchar_t,32768> bytes{};const DWORD count=GetFinalPathNameByHandleW(handle,bytes.data(),static_cast<DWORD>(bytes.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!count||count>=bytes.size())return false;std::wstring text(bytes.data(),count);if(!text.starts_with(L"\\\\?\\"))return false;text.erase(0,4);output=Path(text);return true;
}
/** @summary Capture local NTFS support; a convenient path string is not filesystem durability evidence. */
bool Ntfs(const Path& path)noexcept{
    const auto& name=path.native();if(name.size()<3)return false;wchar_t root[]{name[0],L':',L'\\',0};std::array<wchar_t,32> filesystem{};
    return GetDriveTypeW(root)==DRIVE_FIXED&&GetVolumeInformationW(root,nullptr,0,nullptr,nullptr,nullptr,filesystem.data(),static_cast<DWORD>(filesystem.size()))&&std::wstring_view(filesystem.data())==L"NTFS";
}
/** @summary Capture only the effective current account SID; no credential, profile or other-user account is opened. */
bool Account(std::vector<std::byte>& output){
    HANDLE token{};if(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&token)){if(GetLastError()!=ERROR_NO_TOKEN||!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;}Handle retained(token);
    DWORD count{};GetTokenInformation(token,TokenUser,nullptr,0,&count);if(!count||count>65536)return false;std::vector<std::byte> bytes(count);
    if(!GetTokenInformation(token,TokenUser,bytes.data(),count,&count))return false;const auto* user=reinterpret_cast<const TOKEN_USER*>(bytes.data());
    if(!IsValidSid(user->User.Sid))return false;const auto length=GetLengthSid(user->User.Sid);if(!length||length>SECURITY_MAX_SID_SIZE)return false;
    output.resize(length);std::memcpy(output.data(),user->User.Sid,length);return true;
}
/** @summary Same actual current account plus SYSTEM protected inheritable policy, supplied AT creation without a post-create ACL race. */
struct PrivateSecurity final {
    SECURITY_DESCRIPTOR descriptor{};std::vector<std::byte> account,acl;std::array<std::byte,SECURITY_MAX_SID_SIZE> system{};SECURITY_ATTRIBUTES attributes{};
    /** @summary Construct a minimal absolute descriptor backed by this same retained local object. */
    bool Prepare(){
        if(!Account(account))return false;DWORD systemBytes=static_cast<DWORD>(system.size());if(!CreateWellKnownSid(WinLocalSystemSid,nullptr,system.data(),&systemBytes)||EqualSid(account.data(),system.data()))return false;
        const auto bytes=sizeof(ACL)+2*(sizeof(ACCESS_ALLOWED_ACE)-sizeof(DWORD))+account.size()+systemBytes;acl.resize(bytes);auto* value=reinterpret_cast<ACL*>(acl.data());
        if(!InitializeAcl(value,static_cast<DWORD>(acl.size()),ACL_REVISION)||
            !AddAccessAllowedAceEx(value,ACL_REVISION,OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE,FILE_ALL_ACCESS,account.data())||
            !AddAccessAllowedAceEx(value,ACL_REVISION,OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE,FILE_ALL_ACCESS,system.data())||
            !InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION)||!SetSecurityDescriptorOwner(&descriptor,account.data(),FALSE)||
            !SetSecurityDescriptorDacl(&descriptor,TRUE,value,FALSE)||!SetSecurityDescriptorControl(&descriptor,SE_DACL_PROTECTED,SE_DACL_PROTECTED))return false;
        attributes.nLength=sizeof(attributes);attributes.lpSecurityDescriptor=&descriptor;attributes.bInheritHandle=FALSE;return true;
    }
};
/** @summary Verify actual retained owner and closed allow-ACE policy; never repair an existing broader or missing DACL. */
bool PrivatePolicy(HANDLE handle,std::span<const std::byte> account,bool protectedRoot,bool container=true)noexcept{
    PSECURITY_DESCRIPTOR descriptor{};PSID owner{};PACL acl{};const auto result=GetSecurityInfo(handle,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&descriptor);
    if(result!=ERROR_SUCCESS)return false;
    SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision{};bool okay=owner&&IsValidSid(owner)&&!account.empty()&&EqualSid(owner,const_cast<std::byte*>(account.data()))&&
        acl&&IsValidAcl(acl)&&acl->AceCount==2&&GetSecurityDescriptorControl(descriptor,&control,&revision)&&(!protectedRoot||(control&SE_DACL_PROTECTED));
    std::array<std::byte,SECURITY_MAX_SID_SIZE> system{};DWORD systemBytes=static_cast<DWORD>(system.size());okay=okay&&CreateWellKnownSid(WinLocalSystemSid,nullptr,system.data(),&systemBytes);bool seenAccount=false,seenSystem=false;
    for(DWORD index=0;okay&&index<acl->AceCount;++index){void* raw{};if(!GetAce(acl,index,&raw)){okay=false;break;}const auto* ace=static_cast<const ACCESS_ALLOWED_ACE*>(raw);const auto flags=ace->Header.AceFlags;
        const auto propagation=container?(OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE):0;
        if(ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE||ace->Mask!=FILE_ALL_ACCESS||(flags&~(propagation|(protectedRoot?0:INHERITED_ACE)))||
            (flags&(OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE))!=propagation){okay=false;break;}
        const auto sid=reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));if(!IsValidSid(sid)){okay=false;break;}
        if(EqualSid(sid,const_cast<std::byte*>(account.data()))&&!seenAccount)seenAccount=true;else if(EqualSid(sid,system.data())&&!seenSystem)seenSystem=true;else okay=false;
    }
    LocalFree(descriptor);return okay&&seenAccount&&seenSystem;
}
/** @summary Retained ordinary directory reused by descendants, including created DELETE/write-through publication sources. */
enum class DirectoryRole {Ordinary,MutableNamespace};
/** @summary Derive only fixed private metadata-container roles; payload/package paths can never select this sharing mode. */
DirectoryRole Role(const Path& relative){
    std::vector<std::wstring> parts;for(const auto& item:relative)parts.push_back(item.native());
    if(parts.size()==1&&(parts[0]==L"states"||parts[0]==L"packages"||parts[0]==L"transactions"||parts[0]==L"configuration"))return DirectoryRole::MutableNamespace;
    if(parts.size()>=2&&parts[0]==L"transactions"&&parts[1].size()==32){bool nonzero=false;for(const auto c:parts[1]){if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return DirectoryRole::Ordinary;nonzero|=c!=L'0';}
        if(nonzero&&(parts.size()==2||(parts.size()==3&&(parts[2]==L"journal"||parts[2]==L"evidence"))))return DirectoryRole::MutableNamespace;
    }return DirectoryRole::Ordinary;
}
/** @summary Actual ordinary original directory custody with one role fixed before its first open. */
struct Directory final {Handle handle;Path path;Observation observed;DirectoryRole role{DirectoryRole::Ordinary};bool created{},published{},removalPending{},closedForPublication{},publicationAttempted{};};
/** @summary Retain a new ordinary directory with strict sharing; original created sources additionally retain DELETE/write-through. */
std::shared_ptr<Directory> OpenDirectory(const Path& path,bool created,const std::vector<std::byte>* account=nullptr,bool protectedRoot=false,DirectoryRole role=DirectoryRole::Ordinary){
    if(role==DirectoryRole::MutableNamespace&&!account)return {};
    auto owner=std::make_shared<Directory>();owner->path=path;owner->created=created;owner->role=role;
    owner->handle.value=::CreateFileW(Native(path).c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|((created&&role==DirectoryRole::Ordinary)?DELETE:0)|(account?READ_CONTROL:0),
        FILE_SHARE_READ|(role==DirectoryRole::MutableNamespace?FILE_SHARE_WRITE:0),nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT|(created?FILE_FLAG_WRITE_THROUGH:0),nullptr);
    if(owner->handle.value==INVALID_HANDLE_VALUE)return {};
    Path actual;if(!Info(owner->handle.value,true,owner->observed)||!FinalPath(owner->handle.value,actual)||actual.native()!=path.native()||(account&&!PrivatePolicy(owner->handle.value,*account,protectedRoot))){
        CloseHandle(owner->handle.value);owner->handle.value=INVALID_HANDLE_VALUE;SetLastError(ERROR_INVALID_DATA);return {};
    }
    return owner;
}
/** @summary Same selected parent, ordinary ancestry and stable normalized local identity. */
struct ParentData final {Path path;std::vector<std::shared_ptr<Directory>> ancestry;};
/** @summary Actual per-file created evidence survives handle close without claiming persistent installer ownership. */
struct CreatedFile final {Observation observed;std::uint64_t maximum{},written{};bool flushed{},open{},published{},faulted{};};
/** @summary Exact fixed store owners and current-operation created-object inventory; no portable authority is encoded here. */
struct StoreData final {std::shared_ptr<ParentData> parent;std::shared_ptr<Directory> root;std::vector<std::byte> account;std::map<Path,std::shared_ptr<Directory>> directories;std::map<Path,CreatedFile> files;std::map<Path,std::uint32_t> readers;
    /** @summary Actual duplicated coordination owners, including partial file acquisition; uncertainty never becomes release proof. */
    std::uint32_t coordinationFiles{};bool fileClosureUncertain{};};
/** @summary Revalidate every SAME retained ordinary directory, never bind a replacement from a path. */
bool Current(const ParentData& parent)noexcept{
    for(const auto& item:parent.ancestry){Observation now{};if(!Info(item->handle.value,true,now)||now.identity!=item->observed.identity)return false;}return !parent.ancestry.empty();
}
/** @summary Revalidate exact store root and all original ancestors without reopening a created DELETE handle. */
bool Current(const StoreData& store)noexcept{Observation now{};return Current(*store.parent)&&Info(store.root->handle.value,true,now)&&now.identity==store.root->observed.identity&&PrivatePolicy(store.root->handle.value,store.account,true);}
/** @summary Anchor every existing relative parent; reuse SAME created originals instead of relaxing sharing for a reopen. */
bool Parents(StoreData& store,const Path& relative,bool mutation=true){
    if(!Current(store))return false;Path cursor;
    for(const auto& part:relative.parent_path()){
        cursor/=part;auto it=store.directories.find(cursor);
        if(it==store.directories.end()){auto owner=OpenDirectory(store.root->path/cursor,false,&store.account,false,Role(cursor));if(!owner)return false;it=store.directories.emplace(cursor,std::move(owner)).first;}
        if(mutation&&(it->second->published||it->second->publicationAttempted))return false;
        if(it->second->closedForPublication){
            // @summary A historical child identity is not a live lease; reacquire READ-ONLY and compare the exact original, never adopt a path match.
            auto observed=OpenDirectory(it->second->path,false,&store.account,false,DirectoryRole::Ordinary);
            if(!observed||observed->observed.identity!=it->second->observed.identity)return false;
            std::swap(it->second->handle.value,observed->handle.value);it->second->closedForPublication=false;
        }
        Observation now{};if(!Info(it->second->handle.value,true,now)||now.identity!=it->second->observed.identity)return false;
    }return true;
}
/** @summary Compare relative hierarchy components without case-insensitive prefix guessing. */
bool Beneath(const Path& path,const Path& root){auto a=path.begin(),b=root.begin();for(;b!=root.end();++a,++b)if(a==path.end()||*a!=*b)return false;return a!=path.end();}
#if defined(SIMODER_COMPONENT_STORE_TEST)
/** @summary Test-thread-owned write interruption choice; no runtime policy or fake OS durability result is injected. */
thread_local bool failAfterFirstWriteChunk{};
#endif
}
#if defined(SIMODER_COMPONENT_STORE_TEST)
void test::FailWriteAfterFirstChunk(bool enabled)noexcept{failAfterFirstWriteChunk=enabled;}
#endif
struct Parent::State final {std::shared_ptr<ParentData> data;};
struct Store::State final {std::shared_ptr<StoreData> data;};
struct Coordination::State final {Handle handle;std::shared_ptr<StoreData> store;Observation observed;bool releaseAttempted{};};
struct File::State final {Handle handle,coordination;std::shared_ptr<StoreData> store;Path relative;Observation observed,coordinationObserved;std::uint64_t maximum{},offset{};bool created{},flushed{},faulted{},published{},countedRead{},countedCoordination{},closeUncertain{};
    /** @summary Settle both actual original file and duplicated coordination owners before publishing their closed facts. */
    Result Settle()noexcept;
    /** @summary Also settle private failed acquisitions that never reached a public File owner; uncertainty stays on the original store. */
    ~State(){if(store&&Settle().status!=Status::Ok)store->fileClosureUncertain=true;}
};

Parent::Parent()noexcept=default;Parent::~Parent()=default;
Store::Store()noexcept=default;Store::~Store()=default;
Coordination::Coordination()noexcept=default;Coordination::~Coordination()=default;
File::File()noexcept=default;
/** @summary Close only the same original synchronous file and update its current-operation handle fact; never delete. */
File::~File()=default;

/** @summary Verify same exact fixed coordination owner and retained ancestry before a store effect. */
template<class Lock> bool Locked(const StoreData& store,const Lock* lock)noexcept{
    Observation now{};return lock&&!lock->releaseAttempted&&!store.fileClosureUncertain&&lock->store.get()==&store&&lock->handle.value!=INVALID_HANDLE_VALUE&&Current(store)&&
        Info(lock->handle.value,false,now)&&now.identity==lock->observed.identity&&now.bytes==0;
}
/** @summary Retain the SAME already exclusive kernel file object for original dependent IO; never open an alternate lock. */
template<class Lock,class FileState> bool RetainLock(const Lock* lock,FileState& file)noexcept{
    if(!lock||lock->releaseAttempted||lock->store->fileClosureUncertain)return false;
    if(lock->store->coordinationFiles==std::numeric_limits<std::uint32_t>::max()){SetLastError(ERROR_TOO_MANY_OPEN_FILES);return false;}HANDLE duplicate{};
    if(!DuplicateHandle(GetCurrentProcess(),lock->handle.value,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))return false;
    file.coordination.value=duplicate;file.coordinationObserved=lock->observed;file.countedCoordination=true;++lock->store->coordinationFiles;return true;
}
/** @summary Verify actual same retained coordination before every original file IO, even after the public lock wrapper closes. */
template<class FileState> bool FileCurrent(const FileState& file)noexcept{
    Observation lock{};return !file.store->fileClosureUncertain&&Current(*file.store)&&Info(file.coordination.value,false,lock)&&lock.identity==file.coordinationObserved.identity&&lock.bytes==0;
}
Result WindowsStore::OpenParent(std::string_view name,Parent& output,Observation& observed)noexcept try{
    observed={};if(output.state_)return Outcome(Status::Conflict);std::wstring wide;if(!Wide(name,wide)||!Absolute(wide))return Outcome(Status::Invalid);
    auto data=std::make_shared<ParentData>();
    for(std::size_t at=3;;){auto end=wide.find(L'\\',at);if(end==wide.npos)end=wide.size();
        auto directory=OpenDirectory(Path(wide.substr(0,end)),false);if(!directory)return Outcome(Status::Conflict);data->ancestry.push_back(std::move(directory));if(end==wide.size())break;at=end+1;}
    data->path=data->ancestry.back()->path;
    auto state=std::make_unique<Parent::State>();state->data=std::move(data);observed=state->data->ancestry.back()->observed;output.state_=std::move(state);return Outcome(Status::Ok);
}catch(...){return Outcome(Status::IoError);}

Result WindowsStore::OpenStore(Parent& parent,Store& output,Observation& observed)noexcept try{
    observed={};if(!parent.state_||output.state_||!Current(*parent.state_->data))return Outcome(Status::Invalid);
    std::vector<std::byte> account;if(!Account(account))return Error(GetLastError());const auto path=parent.state_->data->path/L".simoder";auto root=OpenDirectory(path,false,&account,true,DirectoryRole::MutableNamespace);if(!root)return Error(GetLastError());
    auto data=std::make_shared<StoreData>();data->parent=parent.state_->data;data->root=std::move(root);data->account=std::move(account);auto state=std::make_unique<Store::State>();state->data=std::move(data);observed=state->data->root->observed;output.state_=std::move(state);return Outcome(Status::Ok);
}catch(...){return Outcome(Status::IoError);}

Result WindowsStore::Lock(Store& store,Coordination& output)noexcept try{
    if(!store.state_||output.state_||!Current(*store.state_->data))return Outcome(Status::Invalid);auto state=std::make_unique<Coordination::State>();state->store=store.state_->data;
    state->handle.value=::CreateFileW(Native(state->store->root->path/L"coordination.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(state->handle.value==INVALID_HANDLE_VALUE)return Error(GetLastError());
    Path actual;if(!Info(state->handle.value,false,state->observed)||state->observed.bytes||!DefaultStream(state->handle.value)||!PrivatePolicy(state->handle.value,state->store->account,false,false)||!FinalPath(state->handle.value,actual)||actual.native()!=(state->store->root->path/L"coordination.lock").native())return Outcome(Status::Conflict);
    output.state_=std::move(state);return Outcome(Status::Ok);
}catch(...){return Outcome(Status::IoError);}

Result WindowsStore::CreateStore(Parent& parent,const Observation& expected,Store& output,Coordination& lock,Observation& created)noexcept try{
    created={};if(!parent.state_||output.state_||lock.state_||!Current(*parent.state_->data)||!expected.directory||expected.identity!=parent.state_->data->ancestry.back()->observed.identity)return Outcome(Status::Conflict);
    if(!Ntfs(parent.state_->data->path))return Outcome(Status::Unsupported);
    PrivateSecurity security;if(!security.Prepare())return Outcome(Status::Unsupported,Effect::None,Durability::None,GetLastError());
    const auto path=parent.state_->data->path/L".simoder";if(!::CreateDirectoryW(Native(path).c_str(),&security.attributes))return Error(GetLastError());
    auto root=OpenDirectory(path,false,&security.account,true,DirectoryRole::MutableNamespace);if(!root)return Outcome(Status::Unknown,Effect::Created,Durability::None,GetLastError());
    auto data=std::make_shared<StoreData>();data->parent=parent.state_->data;data->root=std::move(root);data->account=std::move(security.account);auto original=std::make_unique<Store::State>();original->data=data;output.state_=std::move(original);created=data->root->observed;
    auto state=std::make_unique<Coordination::State>();state->store=data;state->handle.value=::CreateFileW(Native(path/L"coordination.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(state->handle.value==INVALID_HANDLE_VALUE)return Outcome(Status::Unknown,Effect::Created,Durability::None,GetLastError());
    if(!Info(state->handle.value,false,state->observed)||state->observed.bytes||!DefaultStream(state->handle.value)||!PrivatePolicy(state->handle.value,data->account,false,false))return Outcome(Status::Unknown,Effect::Created);
    lock.state_=std::move(state);return Outcome(Status::Ok,Effect::Created);
}catch(...){return Outcome(Status::Unknown,Effect::Unknown);}

Result WindowsStore::ObserveAttachment(Parent& parent,LocalAttachment& output)noexcept try{
    output={};if(!parent.state_||!Current(*parent.state_->data))return Outcome(Status::Invalid);LocalAttachment actual;actual.backend="windows-file-id-v1";actual.root=parent.state_->data->ancestry.back()->observed.identity;
    if(!Utf8(parent.state_->data->path.native(),actual.normalizedRootUtf8)||!Account(actual.account))return Outcome(Status::IoError);
    BY_HANDLE_FILE_INFORMATION information{};if(!GetFileInformationByHandle(parent.state_->data->ancestry.back()->handle.value,&information))return Error(GetLastError());
    constexpr char hex[]="0123456789abcdef";actual.volumeIdentity.resize(8);for(unsigned at=0;at<8;++at)actual.volumeIdentity[at]=hex[(information.dwVolumeSerialNumber>>(4*(7-at)))&15];
    output=std::move(actual);return Outcome(Status::Ok);
}catch(...){return Outcome(Status::IoError);}

Result WindowsStore::CreateDirectory(Store& store,Coordination& lock,std::string_view name,Observation& created)noexcept try{
    created={};Path relative;if(!store.state_||!Locked(*store.state_->data,lock.state_.get())||!Relative(name,relative)||!Parents(*store.state_->data,relative))return Outcome(Status::Conflict);
    if(!Ntfs(store.state_->data->root->path))return Outcome(Status::Unsupported);
    auto& data=*store.state_->data;const auto path=data.root->path/relative;if(!::CreateDirectoryW(Native(path).c_str(),nullptr))return Error(GetLastError());
    auto owner=OpenDirectory(path,true,&data.account,false,Role(relative));if(!owner)return Outcome(Status::Unknown,Effect::Created,Durability::None,GetLastError());created=owner->observed;
    if(!data.directories.emplace(relative,std::move(owner)).second)return Outcome(Status::Unknown,Effect::Created);return Outcome(Status::Ok,Effect::Created);
}catch(...){return Outcome(Status::Unknown,Effect::Unknown);}

Result WindowsStore::OpenRead(Store& store,Coordination& lock,std::string_view name,std::uint64_t maximum,File& output,Observation& observed)noexcept try{
    observed={};Path relative;if(!store.state_||output.state_||!maximum||maximum>MaximumFile||!Locked(*store.state_->data,lock.state_.get())||!Relative(name,relative)||!Parents(*store.state_->data,relative,false))return Outcome(Status::Invalid);
    auto state=std::make_unique<File::State>();state->store=store.state_->data;state->relative=relative;state->maximum=maximum;const auto path=state->store->root->path/relative;
    if(!RetainLock(lock.state_.get(),*state))return Error(GetLastError());
    state->handle.value=::CreateFileW(Native(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(state->handle.value==INVALID_HANDLE_VALUE)return Error(GetLastError());Path actual;
    if(!Info(state->handle.value,false,state->observed)||state->observed.bytes>maximum||!DefaultStream(state->handle.value)||!PrivatePolicy(state->handle.value,state->store->account,false,false)||!FinalPath(state->handle.value,actual)||actual.native()!=path.native())return Outcome(Status::Conflict);
    auto& readers=state->store->readers[relative];if(readers==std::numeric_limits<std::uint32_t>::max())return Outcome(Status::Conflict);++readers;state->countedRead=true;
    observed=state->observed;output.state_=std::move(state);return Outcome(Status::Ok);
}catch(...){return Outcome(Status::IoError);}

Result WindowsStore::CreateFile(Store& store,Coordination& lock,std::string_view name,std::uint64_t maximum,File& output,Observation& created)noexcept try{
    created={};Path relative;if(!store.state_||output.state_||!maximum||maximum>MaximumFile||!Locked(*store.state_->data,lock.state_.get())||!Relative(name,relative)||!Parents(*store.state_->data,relative))return Outcome(Status::Invalid);
    if(!Ntfs(store.state_->data->root->path))return Outcome(Status::Unsupported);
    auto state=std::make_unique<File::State>();state->store=store.state_->data;state->relative=relative;state->maximum=maximum;state->created=true;
    if(!RetainLock(lock.state_.get(),*state))return Error(GetLastError());
    state->handle.value=::CreateFileW(Native(state->store->root->path/relative).c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(state->handle.value==INVALID_HANDLE_VALUE)return Error(GetLastError());output.state_=std::move(state);auto& original=*output.state_;
    if(!Info(original.handle.value,false,original.observed)||original.observed.bytes||!DefaultStream(original.handle.value)||!PrivatePolicy(original.handle.value,original.store->account,false,false))return Outcome(Status::Unknown,Effect::Created);
    created=original.observed;if(!original.store->files.emplace(relative,CreatedFile{created,maximum,0,false,true,false,false}).second)return Outcome(Status::Unknown,Effect::Created);
    return Outcome(Status::Ok,Effect::Created);
}catch(...){return Outcome(Status::Unknown,Effect::Unknown);}

Result WindowsStore::Read(File& file,std::span<std::byte> output,std::size_t& received)noexcept{
    received=0;if(!file.state_||file.state_->faulted||output.size()>MaximumFile)return Outcome(Status::Invalid);auto& state=*file.state_;Observation now{};
    if(!FileCurrent(state)||!Info(state.handle.value,false,now)||now.identity!=state.observed.identity||now.bytes>state.maximum||state.offset>now.bytes)return Outcome(Status::Conflict);
    LARGE_INTEGER offset{};offset.QuadPart=static_cast<LONGLONG>(state.offset);if(!SetFilePointerEx(state.handle.value,offset,nullptr,FILE_BEGIN))return Error(GetLastError());
    const auto wanted=static_cast<DWORD>(std::min<std::uint64_t>(output.size(),now.bytes-state.offset));DWORD actual{};
    if(wanted&&!ReadFile(state.handle.value,output.data(),wanted,&actual,nullptr))return Error(GetLastError());if(actual!=wanted)return Outcome(Status::Conflict);
    state.offset+=actual;received=actual;return Outcome(Status::Ok);
}
Result WindowsStore::Write(File& file,std::span<const std::byte> bytes)noexcept{
    if(!file.state_||!file.state_->created||file.state_->faulted||file.state_->published)return Outcome(Status::Invalid);auto& state=*file.state_;auto it=state.store->files.find(state.relative);
    if(it==state.store->files.end()||it->second.published||it->second.faulted||!FileCurrent(state)||it->second.written>state.maximum||bytes.size()>state.maximum-it->second.written)return Outcome(Status::Conflict);
    // @summary Invalidate both durable facts BEFORE any possible write; a later partial failure must survive original handle close.
    if(!bytes.empty()){it->second.flushed=false;state.flushed=false;}
    LARGE_INTEGER end{};end.QuadPart=static_cast<LONGLONG>(it->second.written);if(!SetFilePointerEx(state.handle.value,end,nullptr,FILE_BEGIN))return Error(GetLastError());
    std::size_t done=0;while(done<bytes.size()){DWORD actual{};const auto count=static_cast<DWORD>(std::min<std::size_t>(65536,bytes.size()-done));
        if(!WriteFile(state.handle.value,bytes.data()+done,count,&actual,nullptr)||actual!=count){state.faulted=true;it->second.faulted=true;return Outcome(Status::Unknown,Effect::Unknown,Durability::None,GetLastError());}done+=actual;
#if defined(SIMODER_COMPONENT_STORE_TEST)
        if(failAfterFirstWriteChunk&&done<bytes.size()){failAfterFirstWriteChunk=false;state.faulted=true;it->second.faulted=true;return Outcome(Status::Unknown,Effect::Unknown,Durability::None,ERROR_WRITE_FAULT);}
#endif
    }
    it->second.written+=done;it->second.flushed=false;state.flushed=false;return Outcome(Status::Ok,done?Effect::Created:Effect::None);
}
Result WindowsStore::Flush(File& file)noexcept{
    if(!file.state_||!file.state_->created||file.state_->faulted||file.state_->published)return Outcome(Status::Invalid);auto& state=*file.state_;
    if(!FileCurrent(state))return Outcome(Status::Conflict);
    if(!FlushFileBuffers(state.handle.value)){state.faulted=true;auto failed=state.store->files.find(state.relative);if(failed!=state.store->files.end()){failed->second.faulted=true;failed->second.flushed=false;}return Outcome(Status::Unknown,Effect::Unknown,Durability::None,GetLastError());}
    state.flushed=true;auto it=state.store->files.find(state.relative);if(it==state.store->files.end())return Outcome(Status::Unknown,Effect::Unknown);it->second.flushed=true;return Outcome(Status::Ok,Effect::None,Durability::FileFlushed);
}
Result File::State::Settle()noexcept{
    auto& state=*this;
    if(state.closeUncertain)return Outcome(Status::Unknown,Effect::Unknown);
    if(state.handle.value!=INVALID_HANDLE_VALUE){
        if(!CloseHandle(state.handle.value)){state.closeUncertain=true;state.store->fileClosureUncertain=true;return Outcome(Status::Unknown,Effect::Unknown,Durability::None,GetLastError());}
        state.handle.value=INVALID_HANDLE_VALUE;
    }
    if(state.coordination.value!=INVALID_HANDLE_VALUE){
        if(!CloseHandle(state.coordination.value)){state.closeUncertain=true;state.store->fileClosureUncertain=true;return Outcome(Status::Unknown,Effect::Unknown,Durability::None,GetLastError());}
        state.coordination.value=INVALID_HANDLE_VALUE;
    }
    if(state.countedCoordination){
        if(!state.store->coordinationFiles){state.closeUncertain=true;state.store->fileClosureUncertain=true;return Outcome(Status::Unknown,Effect::Unknown);}
        --state.store->coordinationFiles;state.countedCoordination=false;
    }
    if(state.created){auto it=state.store->files.find(state.relative);if(it!=state.store->files.end())it->second.open=false;}
    if(state.countedRead){auto it=state.store->readers.find(state.relative);if(it==state.store->readers.end()||!it->second){state.closeUncertain=true;state.store->fileClosureUncertain=true;return Outcome(Status::Unknown);}
        if(!--it->second)state.store->readers.erase(it);state.countedRead=false;}
    return Outcome(Status::Ok);
}
Result WindowsStore::Close(File& file)noexcept{
    if(!file.state_)return Outcome(Status::Invalid);
    const auto result=file.state_->Settle();if(result.status==Status::Ok)file.state_.reset();return result;
}

Result WindowsStore::ReleaseCoordination(Coordination& lock)noexcept{
    if(!lock.state_)return lock.origin_?Outcome(Status::Invalid):Outcome(Status::Ok);auto& original=*lock.state_;
    if(original.releaseAttempted||original.store->fileClosureUncertain)return Outcome(Status::Unknown,Effect::Unknown);
    if(!Locked(*original.store,&original))return Outcome(Status::Conflict);
    if(original.store->coordinationFiles)return Outcome(Status::Busy);
    original.releaseAttempted=true;
    if(!CloseHandle(original.handle.value))return Outcome(Status::Unknown,Effect::Unknown,Durability::None,GetLastError());
    original.handle.value=INVALID_HANDLE_VALUE;lock.state_.reset();lock.origin_=nullptr;return Outcome(Status::Ok);
}

/** @summary Rename the SAME retained write-through source to one absolute derived destination under still-retained ordinary ancestors.
 * @remarks This is the single fixed route, not a retry/fallback for any prior Unknown operation; source is never reopened. */
Result Rename(HANDLE source,const Path& destinationRoot,const Path& relative,bool replace) {
    const auto destination=destinationRoot/relative;if(destination.native().size()>32760)return Outcome(Status::Invalid);
    const auto name=Native(destination);const auto length=name.size()*sizeof(wchar_t);if(length>std::numeric_limits<DWORD>::max())return Outcome(Status::Invalid);
    std::vector<std::byte> bytes(offsetof(FILE_RENAME_INFO,FileName)+length+sizeof(wchar_t));auto* info=reinterpret_cast<FILE_RENAME_INFO*>(bytes.data());
    info->ReplaceIfExists=replace?TRUE:FALSE;info->RootDirectory=nullptr;info->FileNameLength=static_cast<DWORD>(length);std::memcpy(info->FileName,name.data(),length);
    if(!SetFileInformationByHandle(source,FileRenameInfo,info,static_cast<DWORD>(bytes.size()))){const auto error=GetLastError();
        if(error==ERROR_ALREADY_EXISTS||error==ERROR_FILE_EXISTS||error==ERROR_NOT_SAME_DEVICE||error==ERROR_SHARING_VIOLATION)return Error(error);
        return Outcome(Status::Unknown,Effect::Unknown,Durability::None,error);}
    return Outcome(Status::Ok,Effect::Published,Durability::NamespaceAcknowledged);
}
/** @summary Validate one closed immutable locator shape and derive its fixed backend-relative path. */
bool RecordPath(RecordTarget target,Path& path){
    const auto hex=[](std::string_view value,std::size_t size){bool nonzero=false;if(value.size()!=size)return false;for(const auto c:value){if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;nonzero|=c!='0';}return nonzero;};
    std::string locator;
    switch(target.kind){
    case RecordKind::Attachment:if(!target.identity.empty()||target.sequence)return false;locator="attachment.json";break;
    case RecordKind::State:if(!hex(target.identity,64)||target.sequence)return false;locator="states/"+std::string(target.identity)+".json";break;
    case RecordKind::Plan:if(!hex(target.identity,32)||target.sequence)return false;locator="transactions/"+std::string(target.identity)+"/plan.json";break;
    case RecordKind::Journal:if(!hex(target.identity,32)||!target.sequence||target.sequence>2048)return false;locator="transactions/"+std::string(target.identity)+"/journal/"+std::to_string(target.sequence)+".json";break;
    case RecordKind::Evidence:if(!hex(target.identity,32)||!target.sequence||target.sequence>2048)return false;locator="transactions/"+std::string(target.identity)+"/evidence/"+std::to_string(target.sequence)+".json";break;
    default:return false;
    }return Relative(locator,path);
}
Result WindowsStore::PublishRecord(Store& store,Coordination& lock,File& candidate,RecordTarget target,PublicationRequirement requirement)noexcept try{
    (void)requirement;Path destination;if(!store.state_||!candidate.state_||!RecordPath(target,destination)||!Locked(*store.state_->data,lock.state_.get()))return Outcome(Status::Invalid);
    auto& data=*store.state_->data;auto& original=*candidate.state_;if(!Ntfs(data.root->path))return Outcome(Status::Unsupported);
    if(original.store.get()!=&data||!original.created||!original.flushed||original.faulted||original.published||original.relative==destination||!Parents(data,destination))return Outcome(Status::Conflict);
    Observation current{};if(!Info(original.handle.value,false,current)||current.identity!=original.observed.identity||!current.bytes)return Outcome(Status::Conflict);
    auto entry=data.files.find(original.relative);if(entry==data.files.end()||entry->second.faulted||!entry->second.flushed)return Outcome(Status::Conflict);
    const auto result=Rename(original.handle.value,data.root->path,destination,false);if(result.status!=Status::Ok)return result;
    original.published=true;entry->second.published=true;
    if(!FlushFileBuffers(original.handle.value)){original.faulted=true;entry->second.faulted=true;return Outcome(Status::Unknown,Effect::Published,Durability::NamespaceUnqualified,GetLastError());}
    return result;
}catch(...){return Outcome(Status::Unknown,Effect::Unknown);}
Result WindowsStore::PublishGeneration(Store& store,Coordination& lock,std::string_view from,const Observation& expected,std::string_view to,PublicationRequirement requirement)noexcept try{
    (void)requirement;Path source,target;if(!store.state_||!Locked(*store.state_->data,lock.state_.get())||!Relative(from,source)||!Relative(to,target)||source==target||Beneath(target,source)||Beneath(source,target))return Outcome(Status::Invalid);
    auto& data=*store.state_->data;if(!Ntfs(data.root->path))return Outcome(Status::Unsupported);auto it=data.directories.find(source);
    Observation now{};if(it==data.directories.end()||!it->second->created||it->second->role==DirectoryRole::MutableNamespace||it->second->published||it->second->publicationAttempted||
        !Info(it->second->handle.value,true,now)||now.identity!=expected.identity||!expected.directory||!Parents(data,source/L"publish-leaf")||!Parents(data,target))return Outcome(Status::Conflict);
    for(const auto& [path,record]:data.files)if(Beneath(path,source)&&(!record.flushed||record.open||record.published||record.faulted))return Outcome(Status::Conflict);
    for(const auto& [path,count]:data.readers)if(count&&(path==source||Beneath(path,source)))return Outcome(Status::Conflict);
    // @summary Prepare every local bookkeeping allocation before the original rename; only noexcept swaps follow its acknowledgement.
    auto directories=data.directories;auto files=data.files;std::vector<std::pair<std::shared_ptr<Directory>,Path>> relocated;
    for(const auto& [path,owner]:data.directories)if(path==source||Beneath(path,source)){
        const auto next=path==source?target:target/path.lexically_relative(source);if(directories.contains(next))return Outcome(Status::Conflict);
        directories.erase(path);directories.emplace(next,owner);relocated.emplace_back(owner,data.root->path/next);
    }
    for(const auto& [path,record]:data.files)if(Beneath(path,source)){
        const auto next=target/path.lexically_relative(source);if(files.contains(next))return Outcome(Status::Conflict);auto published=record;published.published=true;files.erase(path);files.emplace(next,published);
    }
    // @summary Explicitly settle ONLY our descendant directory leases after all file owners close; the original source-root lease never closes.
    it->second->publicationAttempted=true;
    for(auto& [path,owner]:data.directories)if(Beneath(path,source)&&owner->handle.value!=INVALID_HANDLE_VALUE){
        Observation original;if(!Info(owner->handle.value,true,original)||original.identity!=owner->observed.identity)return Outcome(Status::Unknown,Effect::None);
        if(!CloseHandle(owner->handle.value))return Outcome(Status::Unknown,Effect::None,Durability::None,GetLastError());
        owner->handle.value=INVALID_HANDLE_VALUE;owner->closedForPublication=true;
    }
    const auto result=Rename(it->second->handle.value,data.root->path,target,false);if(result.status!=Status::Ok)return result;
    for(auto& [owner,path]:relocated){owner->published=true;owner->path.swap(path);}data.directories.swap(directories);data.files.swap(files);return result;
}catch(...){return Outcome(Status::Unknown,Effect::Unknown);}

Result WindowsStore::PublishSelection(Store& store,Coordination& lock,File& candidate,const Observation* expected,std::span<const std::byte> expectedBytes,PublicationRequirement requirement)noexcept try{
    (void)requirement;if(!store.state_||!candidate.state_||!Locked(*store.state_->data,lock.state_.get()))return Outcome(Status::Invalid);auto& data=*store.state_->data;auto& next=*candidate.state_;
    if(!Ntfs(data.root->path))return Outcome(Status::Unsupported);
    if(next.store.get()!=&data||!next.created||!next.flushed||next.faulted||next.published||next.relative==Path(L"current.json")||expectedBytes.size()>4096)return Outcome(Status::Conflict);
    Observation nextInfo{};if(!Info(next.handle.value,false,nextInfo)||nextInfo.identity!=next.observed.identity||!nextInfo.bytes||nextInfo.bytes>4096)return Outcome(Status::Conflict);
    File current;Observation actual{};const auto opened=OpenRead(store,lock,"current.json",4096,current,actual);
    if(expected){
        if(opened.status!=Status::Ok||expected->directory||actual.identity!=expected->identity||actual.bytes!=expectedBytes.size())return Outcome(Status::Conflict);
        std::array<std::byte,4096> bytes{};std::size_t received{};const auto read=Read(current,std::span(bytes).first(expectedBytes.size()),received);
        if(read.status!=Status::Ok||received!=expectedBytes.size()||!std::equal(expectedBytes.begin(),expectedBytes.end(),bytes.begin()))return Outcome(Status::Conflict);
        const auto closed=Close(current);if(closed.status!=Status::Ok)return closed;
    }else if(opened.status!=Status::Absent||!expectedBytes.empty())return Outcome(Status::Conflict);
    // @summary Close only our selector preimage under same retained coordination; this is cooperating-writer comparison, not physical file-ID CAS.
    if(!Locked(data,lock.state_.get()))return Outcome(Status::Conflict);
    auto result=Rename(next.handle.value,data.root->path,Path(L"current.json"),expected!=nullptr);if(result.status!=Status::Ok)return result;
    next.published=true;auto it=data.files.find(next.relative);if(it!=data.files.end())it->second.published=true;
    if(!FlushFileBuffers(next.handle.value)){next.faulted=true;if(it!=data.files.end())it->second.faulted=true;return Outcome(Status::Unknown,Effect::Published,Durability::NamespaceUnqualified,GetLastError());}
    return result;
}catch(...){return Outcome(Status::Unknown,Effect::Unknown);}

/** @summary Derive only a closed nonrecursive namespace, independently of package names or arbitrary caller paths. */
bool InventoryPath(InventoryTarget target,Path& relative){
    const bool transaction=target.kind==InventoryKind::Transaction||target.kind==InventoryKind::Journal||target.kind==InventoryKind::Evidence||target.kind==InventoryKind::StagedPackage;
    const bool package=target.kind==InventoryKind::Package;
    if(transaction||package){if(target.identity.size()!=(package?64u:32u))return false;bool nonzero=false;for(const auto c:target.identity){if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;nonzero|=c!='0';}if(!nonzero)return false;}
    else if(!target.identity.empty())return false;
    if(target.kind!=InventoryKind::Package&&target.kind!=InventoryKind::StagedPackage&&!target.payloadRelative.empty())return false;
    std::string name;switch(target.kind){
    case InventoryKind::Root:relative.clear();return true;
    case InventoryKind::States:name="states";break;case InventoryKind::Packages:name="packages";break;
    case InventoryKind::Transactions:name="transactions";break;case InventoryKind::Configuration:name="configuration";break;
    case InventoryKind::Transaction:name="transactions/"+std::string(target.identity);break;
    case InventoryKind::Journal:name="transactions/"+std::string(target.identity)+"/journal";break;
    case InventoryKind::Evidence:name="transactions/"+std::string(target.identity)+"/evidence";break;
    case InventoryKind::Package:name="packages/"+std::string(target.identity);break;
    case InventoryKind::StagedPackage:name="transactions/"+std::string(target.identity)+"/payload";break;
    default:return false;
    }
    if(!target.payloadRelative.empty()){
        Path suffix;std::wstring wide;if(!Wide(target.payloadRelative,wide)||wide.size()>240||!Relative(target.payloadRelative,suffix))return false;
        std::size_t depth=0;for(const auto& part:suffix){(void)part;if(++depth>16)return false;}name+="/"+std::string(target.payloadRelative);
    }
    return Relative(name,relative);
}
/** @summary Compare actual names using Windows ordinal case comparison; failure conservatively collides instead of supplying an alias. */
struct OrdinalLess final {
    /** @summary Provide bounded case-fold collision detection without locale-dependent rewriting. */
    bool operator()(const std::wstring& a,const std::wstring& b)const noexcept{return CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_LESS_THAN;}
};
Result WindowsStore::ListDirectory(Store& store,Coordination& lock,InventoryTarget target,std::uint32_t maximum,std::vector<NamedObservation>& output)noexcept try{
    output.clear();Path relative;if(!store.state_||!maximum||maximum>2048||!InventoryPath(target,relative)||!Locked(*store.state_->data,lock.state_.get()))return Outcome(Status::Invalid);
    auto& data=*store.state_->data;auto directory=data.root;
    if(!relative.empty()){if(!Parents(data,relative/L"inventory-leaf",false))return Error(GetLastError());auto it=data.directories.find(relative);if(it==data.directories.end())return Outcome(Status::Conflict);directory=it->second;}
    WIN32_FIND_DATAW found{};HANDLE search=FindFirstFileExW(Native(directory->path/L"*").c_str(),FindExInfoBasic,&found,FindExSearchNameMatch,nullptr,0);
    if(search==INVALID_HANDLE_VALUE){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND?Outcome(Status::Ok):Error(error);}
    /** @summary Close only this original nonrecursive enumeration handle, never a directory or entry. */
    struct Search final {HANDLE value;~Search(){FindClose(value);}} retained{search};std::set<std::wstring,OrdinalLess> names;
    for(;;){const std::wstring name=found.cFileName;if(name!=L"."&&name!=L".."){
            if(output.size()>=maximum||!Segment(name)||!names.insert(name).second)return Outcome(Status::Conflict,Effect::None,Durability::None,ERROR_BUFFER_OVERFLOW);
            NamedObservation entry;if(!Utf8(name,entry.nameUtf8))return Outcome(Status::Conflict);const auto item=relative/Path(name);const bool isDirectory=(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
            if(found.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))return Outcome(Status::Conflict);
            if(relative.empty()&&name==L"coordination.lock")entry.observed=lock.state_->observed;
            else if(isDirectory){auto it=data.directories.find(item);if(it!=data.directories.end()){
                    if(it->second->closedForPublication&&!Parents(data,item/L"inventory-leaf",false))return Outcome(Status::Conflict);
                    if(!Info(it->second->handle.value,true,entry.observed)||entry.observed.identity!=it->second->observed.identity||!PrivatePolicy(it->second->handle.value,data.account,false))return Outcome(Status::Conflict);
                }else{auto captured=OpenDirectory(data.root->path/item,false,&data.account,false,Role(item));if(!captured)return Outcome(Status::Conflict);entry.observed=captured->observed;}}
            else{Handle captured(::CreateFileW(Native(data.root->path/item).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                if(captured.value==INVALID_HANDLE_VALUE||!Info(captured.value,false,entry.observed)||!DefaultStream(captured.value)||!PrivatePolicy(captured.value,data.account,false,false))return Outcome(Status::Conflict);}
            output.push_back(std::move(entry));
        }
        if(!FindNextFileW(search,&found)){const auto error=GetLastError();if(error!=ERROR_NO_MORE_FILES)return Error(error);break;}
    }
    if(!Locked(data,lock.state_.get()))return Outcome(Status::Conflict);std::sort(output.begin(),output.end(),[](const auto& a,const auto& b){return a.nameUtf8<b.nameUtf8;});return Outcome(Status::Ok);
}catch(...){return Outcome(Status::IoError);}

Result WindowsStore::RemoveCreatedDirectory(Store& store,Coordination& lock,std::string_view name,const Observation& expected)noexcept try{
    Path relative;if(!store.state_||!Locked(*store.state_->data,lock.state_.get())||!Relative(name,relative)||!Parents(*store.state_->data,relative))return Outcome(Status::Invalid);auto& data=*store.state_->data;auto it=data.directories.find(relative);Observation now{};
    if(it==data.directories.end()||!it->second->created||it->second->role==DirectoryRole::MutableNamespace||it->second->published||it->second->publicationAttempted||!expected.directory)return Outcome(Status::Conflict);
    if(it->second->removalPending)return Outcome(Status::Unknown,Effect::Unknown);
    if(!Info(it->second->handle.value,true,now)||now.identity!=expected.identity)return Outcome(Status::Conflict);
    FILE_DISPOSITION_INFO disposition{TRUE};if(!SetFileInformationByHandle(it->second->handle.value,FileDispositionInfo,&disposition,sizeof(disposition)))return Error(GetLastError());
    // @summary A successful disposition is only delete-pending; retain original evidence until actual close and namespace absence are established.
    auto& original=*it->second;original.removalPending=true;
    if(!CloseHandle(original.handle.value))return Outcome(Status::Unknown,Effect::Unknown,Durability::NamespaceUnqualified,GetLastError());
    original.handle.value=INVALID_HANDLE_VALUE;
    Handle remaining(::CreateFileW(Native(original.path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(remaining.value!=INVALID_HANDLE_VALUE)return Outcome(Status::Unknown,Effect::Unknown,Durability::NamespaceUnqualified);
    const auto error=GetLastError();
    if(error!=ERROR_FILE_NOT_FOUND)return Outcome(Status::Unknown,Effect::Unknown,Durability::NamespaceUnqualified,error);
    data.directories.erase(it);return Outcome(Status::Ok,Effect::Removed,Durability::NamespaceUnqualified);
}catch(...){return Outcome(Status::Unknown,Effect::Unknown);}
}
