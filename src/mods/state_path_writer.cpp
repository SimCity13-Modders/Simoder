#include "mods/state_path_writer.hpp"
#include <Windows.h>
#include <winternl.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#if defined(SIMODER_SCRIPT_LANE_TEST_WITNESS)
#include "scripting/script_runtime.hpp"
#endif

namespace sc13::mods {
namespace {
constexpr std::size_t RawLimit=1024U*1024U,OwnerLimit=32,OperationLimit=8;
/** @summary Own exactly one native handle, without implicit delete disposition. */
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};Handle()=default;explicit Handle(HANDLE h):value(h){};Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;
    Handle(Handle&& other)noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    Handle&operator=(Handle&& other)noexcept{if(this!=&other){Reset();value=std::exchange(other.value,INVALID_HANDLE_VALUE);}return *this;}
    ~Handle(){Reset();}
    /** @summary Close ownership only; never marks a pathname for deletion. */
    void Reset()noexcept{if(value!=INVALID_HANDLE_VALUE)CloseHandle(std::exchange(value,INVALID_HANDLE_VALUE));}
    explicit operator bool()const noexcept{return value!=INVALID_HANDLE_VALUE;}
};
using W=StatePathWriter;
/** @summary The single documented Boolean-form class10 record, using natural x86 layout. */
struct NativeRenameInformation {BOOLEAN ReplaceIfExists;HANDLE RootDirectory;ULONG FileNameLength;WCHAR FileName[1];};
/** @summary Exact user-mode x86 native ABI, without a WDK import dependency. */
using SetInformationFile=NTSTATUS (NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
/** @summary OS status translation is diagnostic/retry evidence, never GetLastError. */
using StatusToDosError=ULONG (NTAPI*)(NTSTATUS);
constexpr NTSTATUS PendingStatus=static_cast<NTSTATUS>(0x00000103L);
constexpr FILE_INFORMATION_CLASS RenameInformationClass=static_cast<FILE_INFORMATION_CLASS>(10);
/** @summary Qualified pointers and image-relative export identities from the already-loaded system ntdll. */
struct NativeExports {HMODULE module{};SetInformationFile set{};StatusToDosError map{};DWORD imageSize{},setRva{},mapRva{};W::FileIdentity systemFileIdentity;};
/** @summary Stable per-record storage retained until the original native request is positively completed. */
struct NativeRenameContext {
    alignas(NativeRenameInformation) std::array<std::byte,1024> input{};IO_STATUS_BLOCK iosb{};
    W::OperationIdentity operation;HANDLE source{INVALID_HANDLE_VALUE},parent{INVALID_HANDLE_VALUE};
    std::uint64_t submission{},effectSequence{};W::EffectKind effect{W::EffectKind::None};bool outstanding{},completionAcknowledged{};
};
static_assert(sizeof(void*)==4&&sizeof(HANDLE)==4&&sizeof(NTSTATUS)==4&&sizeof(ULONG)==4&&sizeof(FILE_INFORMATION_CLASS)==4&&sizeof(WCHAR)==2);
static_assert(offsetof(NativeRenameInformation,ReplaceIfExists)==0&&offsetof(NativeRenameInformation,RootDirectory)==4&&offsetof(NativeRenameInformation,FileNameLength)==8&&offsetof(NativeRenameInformation,FileName)==12);
static_assert(sizeof(NativeRenameInformation)==16&&alignof(NativeRenameInformation)==4);
static_assert(sizeof(IO_STATUS_BLOCK)==8&&offsetof(IO_STATUS_BLOCK,Status)==0&&offsetof(IO_STATUS_BLOCK,Information)==4);
static_assert(sizeof(NativeRenameInformation)+255*sizeof(WCHAR)==526&&sizeof(NativeRenameContext)<=1152);
static_assert(sizeof(NativeRenameContext)*OperationLimit<=9216);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
thread_local std::function<StatePathWriterTestAccess::CutAction(const StatePathWriterTestAccess::CutObservation&)> cutHook;
#endif
/** @summary Compare bounded native names using ordinal Windows equality. */
bool Equal(std::wstring_view a,std::wstring_view b)noexcept{return a.size()<=4096&&b.size()<=4096&&CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;}
/** @summary Recognize the only ASCII letters admitted in a DOS drive prefix. @param unit Native code unit. @return True for A-Z or a-z. */
bool DriveLetter(wchar_t unit)noexcept{return (unit>=L'A'&&unit<=L'Z')||(unit>=L'a'&&unit<=L'z');}
/** @summary Validate every supplied UTF16 code unit and colon before a NUL-terminated path API can truncate it.
 * @param text Complete path text after the existing code-unit length bound.
 * @return False for NUL, malformed surrogate sequences or a colon outside an absolute ASCII-letter DOS prefix; valid pairs remain unchanged. */
bool ValidPathText(std::wstring_view text)noexcept{
    for(std::size_t index=0;index<text.size();++index){const auto unit=text[index];
        if(unit==L'\0'||(unit>=0xDC00&&unit<=0xDFFF))return false;
        if(unit==L':'&&(index!=1||text.size()<3||!DriveLetter(text[0])||(text[2]!=L'\\'&&text[2]!=L'/')))return false;
        if(unit>=0xD800&&unit<=0xDBFF){if(++index==text.size()||text[index]<0xDC00||text[index]>0xDFFF)return false;}
    }return true;
}
/** @summary Observe current native identity, requiring explicit validity. */
W::FileIdentity Identity(HANDLE handle)noexcept{FILE_ID_INFO info{};W::FileIdentity id;if(GetFileInformationByHandleEx(handle,FileIdInfo,&info,sizeof(info))){id.volume=info.VolumeSerialNumber;std::copy(std::begin(info.FileId.Identifier),std::end(info.FileId.Identifier),id.file.begin());id.valid=true;}return id;}
/** @summary Parse one bounded FILE_STREAM_INFO chain, without importing filesystem success. */
W::QueryDisposition Streams(std::span<const std::byte> bytes)noexcept{
    if(bytes.size()>65536)return W::QueryDisposition::Oversize;
    std::size_t offset{};unsigned entries{};
    for(;;){if(++entries>64||offset>bytes.size()||bytes.size()-offset<offsetof(FILE_STREAM_INFO,StreamName))return W::QueryDisposition::Malformed;
        FILE_STREAM_INFO header{};std::memcpy(&header,bytes.data()+offset,offsetof(FILE_STREAM_INFO,StreamName));
        if(header.StreamSize.QuadPart<0||header.StreamAllocationSize.QuadPart<0)return W::QueryDisposition::Malformed;
        const auto length=static_cast<std::size_t>(header.StreamNameLength);if(length%sizeof(wchar_t)||length>bytes.size()-offset- offsetof(FILE_STREAM_INFO,StreamName))return W::QueryDisposition::Malformed;
        if(length!=14)return W::QueryDisposition::Unsupported;
        constexpr wchar_t expected[]=L"::$DATA";if(std::memcmp(bytes.data()+offset+offsetof(FILE_STREAM_INFO,StreamName),expected,length)!=0)return W::QueryDisposition::Unsupported;
        if(entries!=1)return W::QueryDisposition::Unsupported;
        if(!header.NextEntryOffset)return W::QueryDisposition::Confirmed;
        if(header.NextEntryOffset%8||header.NextEntryOffset<offsetof(FILE_STREAM_INFO,StreamName)+length||header.NextEntryOffset>bytes.size()-offset)return W::QueryDisposition::Malformed;
        offset+=header.NextEntryOffset;
    }
}
/** @summary Consume no lane/publication authority; live caller guards remain an explicit native precondition. */
bool OffLane()noexcept{
#if defined(SIMODER_SCRIPT_LANE_TEST_WITNESS)
    return !scripting::ScriptRuntime::OnScriptLane();
#else
    return true;
#endif
}
}

/** @summary One bounded original operation; no strong pointer back to its Owner. */
struct StatePathWriter::Record {
    std::size_t pool{};std::weak_ptr<OwnerState> localChargeOwner;OperationIdentity identity;Result result;std::unique_ptr<char[]> raw;
    std::size_t previousSize{},candidateSize{};bool priorAbsent{},attempted{},compensationAttempted{},restore{},suppress{},disposition{},removeCandidate{},publicationNativeSuccess{},restorationNativeSuccess{};
    std::uint64_t expectedSequence{};FileIdentity prior,candidate,restoration,ownedIdentity;Handle owned;std::array<wchar_t,129> temporary{};
    NativeRenameContext rename;
    /** @summary Release raw charge only after actual last record owner disappears; an outstanding-request invariant failure terminates before any member/handle/raw release. */
    ~Record();
};
/** @summary One provisional/live/quarantined/closed-retained native owner-pool entry. */
struct StatePathWriter::OwnerState {
    std::size_t pool{};std::uint64_t generation{},sequence{},latest{};std::atomic_bool turn{};std::atomic_size_t slotCount{};std::atomic<OwnerPhase> phase{OwnerPhase::Open};
    std::array<Handle,32> ancestors;std::size_t ancestorCount{};FileIdentity parentIdentity;std::array<wchar_t,3841> parent{};std::array<wchar_t,256> leaf{};std::array<std::shared_ptr<Record>,2> records;
    NativeExports exports;
    /** @summary Release native ancestors and final owner-pool charge outside registry guards; pending invariant failure terminates before member destruction. */
    ~OwnerState();
};
/** @summary Shared native qualification/effect helpers and fixed process-lifetime retention pools. */
struct StatePathWriter::Native {
    static_assert(sizeof(wchar_t)==2);
    static_assert(sizeof(OwnerState::parent)+sizeof(OwnerState::leaf)==8194);
    static_assert(sizeof(Record::temporary)==258);
    static_assert(sizeof(NativeRenameInformation)+255*sizeof(WCHAR)<=sizeof(NativeRenameContext::input));
    static_assert(2*RawLimit*OperationLimit==16U*1024U*1024U);
    static_assert(sizeof(OwnerState)<=16384&&sizeof(Record)<=4096);
    struct OwnerSlot {std::atomic_bool occupied{};std::weak_ptr<OwnerState> lifetime;std::shared_ptr<OwnerState> active;};
    struct Pool {std::mutex mutex;std::array<OwnerSlot,OwnerLimit> owners;std::array<std::atomic_bool,OperationLimit> operations{};std::uint64_t nextOwner{1},nextOperation{1};};
    /** @summary One bounded volatile registry retains quarantines without record-to-owner cycles. */
    static Pool& Registry(){alignas(Pool) static std::byte bytes[sizeof(Pool)];static Pool* pool=::new(static_cast<void*>(bytes)) Pool;return *pool;}
    /** @summary Copy a failure-only result without reading unowned mutable state. */
    static Result Fail(Kind kind=Kind::Rejected)noexcept{Result result;result.kind=kind;return result;}
    /** @summary Return current original retained parent handle. */
    static HANDLE Parent(OwnerState& owner)noexcept{return owner.ancestorCount?owner.ancestors[owner.ancestorCount-1].value:INVALID_HANDLE_VALUE;}
    /** @summary Inspect only retained scalars before any owner-handle operation. @param owner Original owner. @return Outstanding original record, if any. */
    static const Record* Pending(const OwnerState& owner)noexcept{for(const auto& record:owner.records)if(record&&record->rename.outstanding)return record.get();return nullptr;}
    /** @summary Copy an unavailable current view without touching handles or consuming completion. @param owner Original owner. @param record Requested retained record. @return Pending-first scalar outcome. */
    static Result PendingView(const OwnerState& owner,const Record& record)noexcept{auto result=Snapshot(owner,record);result.kind=Kind::Unknown;result.current=Current::Unknown;result.observationDisposition=QueryDisposition::NotQueried;result.observationError=0;return result;}
    /** @summary Bound an image-relative range before interpreting PE export storage. @param size Image extent. @param rva Range start. @param bytes Required bytes. @return True only inside the image. */
    static bool Range(DWORD size,DWORD rva,std::size_t bytes)noexcept{return rva<size&&bytes<=size-rva;}
    /** @summary Require committed image storage owned by the exact already-loaded module. @param module Original image base. @param pointer Inspected address. @param bytes Range length. @param executable Require an executable code page. @return Qualified complete page range. */
    static bool ImageMemory(HMODULE module,const void* pointer,std::size_t bytes,bool executable=false)noexcept{
        auto address=reinterpret_cast<std::uintptr_t>(pointer);if(!bytes||bytes>std::numeric_limits<std::uintptr_t>::max()-address)return false;const auto end=address+bytes;
        while(address<end){MEMORY_BASIC_INFORMATION memory{};if(VirtualQuery(reinterpret_cast<const void*>(address),&memory,sizeof(memory))!=sizeof(memory)||memory.AllocationBase!=module||memory.Type!=MEM_IMAGE||memory.State!=MEM_COMMIT||(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
            if(executable&&!(memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
            const auto base=reinterpret_cast<std::uintptr_t>(memory.BaseAddress);if(memory.RegionSize>std::numeric_limits<std::uintptr_t>::max()-base||base+memory.RegionSize<=address)return false;address=std::min<std::uintptr_t>(end,base+memory.RegionSize);
        }return true;
    }
    /** @summary Locate one exact direct export; a forwarder is unavailable, never another backend. @param module Qualified loaded image. @param size PE image extent. @param directory Bounded export directory. @param name Exact ASCII name. @param address Resolved direct address. @param rva Exact export RVA. @return Matching table/API/image ownership. */
    static bool Export(HMODULE module,DWORD size,const IMAGE_DATA_DIRECTORY& directory,const char* name,FARPROC& address,DWORD& rva)noexcept{
        const auto* base=reinterpret_cast<const std::byte*>(module);if(!Range(size,directory.VirtualAddress,directory.Size)||directory.Size<sizeof(IMAGE_EXPORT_DIRECTORY)||!ImageMemory(module,base+directory.VirtualAddress,directory.Size))return false;
        const auto& exports=*reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+directory.VirtualAddress);
        if(!exports.NumberOfNames||exports.NumberOfNames>65536||!exports.NumberOfFunctions||exports.NumberOfFunctions>65536)return false;
        const auto namesSize=std::size_t(exports.NumberOfNames)*sizeof(DWORD),ordinalsSize=std::size_t(exports.NumberOfNames)*sizeof(WORD),functionsSize=std::size_t(exports.NumberOfFunctions)*sizeof(DWORD);
        if(!Range(size,exports.AddressOfNames,namesSize)||!Range(size,exports.AddressOfNameOrdinals,ordinalsSize)||!Range(size,exports.AddressOfFunctions,functionsSize)||!ImageMemory(module,base+exports.AddressOfNames,namesSize)||!ImageMemory(module,base+exports.AddressOfNameOrdinals,ordinalsSize)||!ImageMemory(module,base+exports.AddressOfFunctions,functionsSize))return false;
        const auto* names=reinterpret_cast<const DWORD*>(base+exports.AddressOfNames);const auto* ordinals=reinterpret_cast<const WORD*>(base+exports.AddressOfNameOrdinals);const auto* functions=reinterpret_cast<const DWORD*>(base+exports.AddressOfFunctions);const auto length=std::strlen(name)+1;
        for(DWORD index=0;index<exports.NumberOfNames;++index){if(!Range(size,names[index],length)||!ImageMemory(module,base+names[index],length))return false;if(std::memcmp(base+names[index],name,length))continue;
            if(ordinals[index]>=exports.NumberOfFunctions)return false;rva=functions[ordinals[index]];if(!Range(size,rva,1)||(rva>=directory.VirtualAddress&&rva-directory.VirtualAddress<directory.Size)||!ImageMemory(module,base+rva,1,true))return false;
            address=GetProcAddress(module,name);HMODULE actual{};return address&&reinterpret_cast<const std::byte*>(address)==base+rva&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&actual)&&actual==module;
        }return false;
    }
    /** @summary Join the actual loaded mapping with the OS-derived existing system file, without loading or modifying it.
     * @param module Already-loaded native image. @param expected OS-derived physical filename.
     * @param identity Receives valid original file identity. @param mapped Receives the checked native device path.
     * @return True only for one plain expected disk file and a complete matching mapped-file name. */
    static bool SystemImage(HMODULE module,const wchar_t* expected,FileIdentity& identity,std::array<wchar_t,4097>& mapped)noexcept{
        Handle file(CreateFileW(expected,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(!file||GetFileType(file.value)!=FILE_TYPE_DISK)return false;
        FILE_ATTRIBUTE_TAG_INFO attributes{};if(!GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attributes,sizeof(attributes))||(attributes.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
        const auto observed=Identity(file.value);if(!observed.valid)return false;
        std::array<wchar_t,4097> finalName{};const auto finalCount=GetFinalPathNameByHandleW(file.value,finalName.data(),static_cast<DWORD>(finalName.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_NT);
        const auto mappedCount=K32GetMappedFileNameW(GetCurrentProcess(),module,mapped.data(),static_cast<DWORD>(mapped.size()));
        if(!finalCount||finalCount>=finalName.size()||!mappedCount||mappedCount>=mapped.size()||!Equal(std::wstring_view(finalName.data(),finalCount),std::wstring_view(mapped.data(),mappedCount)))return false;
        identity=observed;return true;
    }
    /** @summary Compare the complete original loaded image, direct exports and expected-file identity.
     * @param original Original qualified binding. @param current Fresh qualified binding. @return Exact retained binding equality. */
    static bool SameExports(const NativeExports& original,const NativeExports& current)noexcept{return original.module==current.module&&original.set==current.set&&original.map==current.map&&original.imageSize==current.imageSize&&original.setRva==current.setRva&&original.mapRva==current.mapRva&&original.systemFileIdentity.valid&&current.systemFileIdentity==original.systemFileIdentity;}
    /** @summary Qualify actual loaded x86 system ntdll and both direct exports before any file effect.
     * @param output Receives only completely qualified bindings. @param physicalPath Optional checked physical system path. @param mappedPath Optional checked native device path.
     * @return False for unexpected mapped file, image, forwarder or owner; no loading/fallback occurs.
     * @details Independent host evidence must bind this actual path/RVA pair to OS file identity, hash and version; this gate never pins one OS version globally. */
    static bool Exports(NativeExports& output,std::array<wchar_t,4097>* physicalPath=nullptr,std::array<wchar_t,4097>* mappedPath=nullptr)noexcept{
        NativeExports candidate;candidate.module=GetModuleHandleW(L"ntdll.dll");if(!candidate.module)return false;
        std::array<wchar_t,4097> system{},mapped{};
        BOOL wow64{};if(!IsWow64Process(GetCurrentProcess(),&wow64))return false;const auto directory=wow64?GetSystemWow64DirectoryW(system.data(),static_cast<UINT>(system.size())):GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));if(!directory||directory>=system.size()-11)return false;
        constexpr wchar_t suffix[]=L"\\ntdll.dll";std::copy(std::begin(suffix),std::end(suffix),system.begin()+directory);if(!SystemImage(candidate.module,system.data(),candidate.systemFileIdentity,mapped))return false;
        const auto* base=reinterpret_cast<const std::byte*>(candidate.module);if(!ImageMemory(candidate.module,base,sizeof(IMAGE_DOS_HEADER)))return false;const auto& dos=*reinterpret_cast<const IMAGE_DOS_HEADER*>(base);if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<sizeof(IMAGE_DOS_HEADER)||dos.e_lfanew>3072||!ImageMemory(candidate.module,base+dos.e_lfanew,sizeof(IMAGE_NT_HEADERS32)))return false;
        const auto& nt=*reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos.e_lfanew);if(nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER32)||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_EXPORT||nt.OptionalHeader.SizeOfImage<4096||nt.OptionalHeader.SizeOfImage>128U*1024U*1024U)return false;
        candidate.imageSize=nt.OptionalHeader.SizeOfImage;FARPROC set{},map{};const auto& directoryEntry=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];if(!Export(candidate.module,candidate.imageSize,directoryEntry,"NtSetInformationFile",set,candidate.setRva)||!Export(candidate.module,candidate.imageSize,directoryEntry,"RtlNtStatusToDosError",map,candidate.mapRva))return false;
        candidate.set=reinterpret_cast<SetInformationFile>(set);candidate.map=reinterpret_cast<StatusToDosError>(map);output=candidate;if(physicalPath)*physicalPath=system;if(mappedPath)*mappedPath=mapped;return true;
    }
    /** @summary Revalidate the original export binding before temporary creation or submission. @param owner Original qualified owner. @return Identical qualified current system module and exports. */
    static bool OriginalExports(const OwnerState& owner)noexcept{if(Pending(owner))return false;NativeExports current;return Exports(current)&&SameExports(owner.exports,current);}
    /** @summary Compose one bounded existing parent plus fixed leaf outside registry locks. */
    static std::wstring Path(const OwnerState& owner,const wchar_t* leaf){std::wstring path(owner.parent.data());if(!path.ends_with(L'\\'))path+=L'\\';path+=leaf;return path;}
    /** @summary Query qualified streams, plain identity and single hardlink from actual handle. */
    static QueryDisposition File(HANDLE handle,FileIdentity& identity,DWORD& error)noexcept{
        BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(handle,&info)){error=GetLastError();return QueryDisposition::Unavailable;}
        if(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)||info.nNumberOfLinks!=1||GetFileType(handle)!=FILE_TYPE_DISK)return QueryDisposition::Unsupported;
        identity=Identity(handle);if(!identity.valid){error=GetLastError();return QueryDisposition::Unavailable;}
        alignas(FILE_STREAM_INFO) std::array<std::byte,65536> buffer{};
        if(!GetFileInformationByHandleEx(handle,FileStreamInfo,buffer.data(),static_cast<DWORD>(buffer.size()))){error=GetLastError();return error==ERROR_MORE_DATA||error==ERROR_INSUFFICIENT_BUFFER?QueryDisposition::Oversize:QueryDisposition::Unavailable;}
        return Streams(buffer);
    }
    /** @summary Read/copy or compare default bytes, bounded to one MiB and fixed scratch. */
    static bool Bytes(HANDLE handle,char* output,std::size_t& size,const char* expected=nullptr,std::size_t expectedSize=0,QueryDisposition* disposition=nullptr)noexcept{
        if(disposition)*disposition=QueryDisposition::Unavailable;LARGE_INTEGER length{},zero{};if(!GetFileSizeEx(handle,&length)||length.QuadPart<0)return false;
        if(static_cast<unsigned long long>(length.QuadPart)>RawLimit){if(disposition)*disposition=QueryDisposition::Oversize;return false;}
        size=static_cast<std::size_t>(length.QuadPart);if(expected&&size!=expectedSize){if(disposition)*disposition=QueryDisposition::Conflict;return false;}
        if(!SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN))return false;
        std::array<char,65536> scratch{};std::size_t offset{};
        while(offset<size){const auto count=static_cast<DWORD>(std::min(scratch.size(),size-offset));DWORD read{};if(!ReadFile(handle,scratch.data(),count,&read,nullptr)||read!=count)return false;if(output)std::memcpy(output+offset,scratch.data(),count);if(expected&&std::memcmp(expected+offset,scratch.data(),count)){if(disposition)*disposition=QueryDisposition::Conflict;return false;}offset+=count;}if(disposition)*disposition=QueryDisposition::Confirmed;return true;
    }
    /** @summary Open a non-followed existing leaf; missing is distinguished only by exact FILE_NOT_FOUND. */
    static Handle OpenFile(OwnerState& owner,const wchar_t* leaf,DWORD desired=GENERIC_READ|FILE_READ_ATTRIBUTES,DWORD* error=nullptr) {
        if(Pending(owner))return {};
        const auto path=Path(owner,leaf);const HANDLE handle=CreateFileW(path.c_str(),desired,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);const DWORD captured=handle==INVALID_HANDLE_VALUE?GetLastError():0;if(error)*error=captured;return Handle(handle);
    }
    /** @summary Prove original handle still names the exact retained native parent/leaf. */
    static bool Located(HANDLE handle,OwnerState& owner,const wchar_t* leaf)noexcept{
        if(Pending(owner))return false;
        alignas(FILE_NAME_INFO) std::array<std::byte,65536> buffer{};
        if(!GetFileInformationByHandleEx(handle,FileNameInfo,buffer.data(),static_cast<DWORD>(buffer.size())))return false;
        const auto* info=reinterpret_cast<const FILE_NAME_INFO*>(buffer.data());if(info->FileNameLength%2||info->FileNameLength>8190)return false;
        try{auto path=Path(owner,leaf);return path.size()>2&&Equal(std::wstring_view(info->FileName,info->FileNameLength/2),std::wstring_view(path).substr(2));}catch(...){return false;}
    }
    /** @summary Require the actual existing handle's normalized long leaf to agree with the requested spelling.
     * @param handle Already qualified ordinary target. @param leaf Requested bounded leaf.
     * @return False on unavailable, oversized, unexpected-prefix or differing long-name observations; no alternate query or fallback. */
    static bool NormalizedLongLeaf(HANDLE handle,std::wstring_view leaf)noexcept{
        std::array<wchar_t,4101> buffer{}; // @summary Existing4096 path limit plus the actual four-unit extended DOS prefix and terminator.
        const auto length=GetFinalPathNameByHandleW(handle,buffer.data(),static_cast<DWORD>(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(!length||length>=buffer.size())return false;
        const std::wstring_view observed(buffer.data(),length);
        if(observed.size()<=7||!observed.starts_with(L"\\\\?\\")||!DriveLetter(observed[4])||observed[5]!=L':'||observed[6]!=L'\\')return false;
        const auto separator=observed.find_last_of(L'\\');
        return separator!=std::wstring_view::npos&&separator+1<observed.size()&&Equal(observed.substr(separator+1),leaf);
    }
    /** @summary Preserve original owner as quarantined on uncertain effects. */
    static Result Unknown(OwnerState& owner,Record& record)noexcept{owner.phase=OwnerPhase::Quarantined;record.result.kind=Kind::Unknown;record.result.phase=Phase::Unknown;record.result.history=History::Unknown;return Snapshot(owner,record);}
    /** @summary Reopen cooperative admission only when every original unresolved effect was proved. */
    static void Thaw(OwnerState& owner)noexcept{if(Pending(owner))return;for(const auto& record:owner.records)if(record&&(record->result.phase==Phase::Unknown||record->result.phase==Phase::CleanupDeletePending||record->result.phase==Phase::CompensatingDeletePending))return;owner.phase=OwnerPhase::Open;}
    /** @summary Require exact native parent and original prior/current default bytes before effects. */
    static bool Expected(OwnerState& owner,Record& record,bool compensation) {
        if(Pending(owner)||record.rename.outstanding)return false;
        if(Identity(Parent(owner))!=owner.parentIdentity)return false;
        DWORD openError{};auto file=OpenFile(owner,owner.leaf.data(),GENERIC_READ|FILE_READ_ATTRIBUTES,&openError);if(!file)return !compensation&&record.priorAbsent&&openError==ERROR_FILE_NOT_FOUND;
        DWORD error{};FileIdentity id;if(File(file.value,id,error)!=QueryDisposition::Confirmed)return false;
        const auto& expectedId=compensation?record.candidate:record.prior;
        std::size_t size{};return id==expectedId&&Bytes(file.value,nullptr,size,record.raw.get()+(compensation?RawLimit:0),compensation?record.candidateSize:record.previousSize);
    }
    /** @summary Prove an unsuccessful delete left the exact original candidate and no delete disposition pending. @return True only after positive handle, destination, stream and byte observations. */
    static bool CandidateUnchanged(OwnerState& owner,Record& record) {
        if(Pending(owner)||record.rename.outstanding)return false;
        if(!record.owned||record.disposition||!Located(record.owned.value,owner,owner.leaf.data()))return false;
        FILE_STANDARD_INFO standard{};if(!GetFileInformationByHandleEx(record.owned.value,FileStandardInfo,&standard,sizeof(standard))||standard.DeletePending)return false;
        DWORD error{};FileIdentity id;std::size_t size{};
        if(File(record.owned.value,id,error)!=QueryDisposition::Confirmed||id!=record.candidate||id!=record.ownedIdentity||!Bytes(record.owned.value,nullptr,size,record.raw.get()+RawLimit,record.candidateSize)||!Expected(owner,record,true))return false;
        FileIdentity after;return File(record.owned.value,after,error)==QueryDisposition::Confirmed&&after==id&&Located(record.owned.value,owner,owner.leaf.data())&&GetFileInformationByHandleEx(record.owned.value,FileStandardInfo,&standard,sizeof(standard))&&!standard.DeletePending;
    }
    /** @summary Confirm exact still-temporary object and no pathname replacement before cleanup/publication. */
    static bool StillTemporary(OwnerState& owner,Record& record) {
        if(Pending(owner)||record.rename.outstanding)return false;
        if(!record.owned||!Located(record.owned.value,owner,record.temporary.data()))return false;
        DWORD error{};FileIdentity id;if(File(record.owned.value,id,error)!=QueryDisposition::Confirmed||id!=record.ownedIdentity)return false;
        auto named=OpenFile(owner,record.temporary.data());return named&&Identity(named.value)==record.ownedIdentity;
    }
    /** @summary Shared submission classification; Pending and unavailable are never retryable failures. */
    enum class RenameResult {Unavailable,Pending,Succeeded,Failed};
    /** @summary Join one authoritative completed native status and preserve its separate conversion domain.
     * @param owner Original qualified owner. @param record Original retained submission. @param status Authoritative returned/completed status. @return Actual completed classification. */
    static RenameResult FinishRename(OwnerState& owner,Record& record,NTSTATUS status)noexcept{
        auto& context=record.rename;auto& evidence=record.result.rename;
        context.outstanding=false;context.completionAcknowledged=true;evidence.outstanding=false;evidence.completionAcknowledged=true;evidence.finalStatus=static_cast<std::uint32_t>(status);evidence.finalValid=true;
        const bool success=status>=0;record.result.lastNativeReturn=success?NativeReturn::Succeeded:NativeReturn::Failed;
        if(success){if(context.effect==EffectKind::PublishRename)record.publicationNativeSuccess=true;else record.restorationNativeSuccess=true;return RenameResult::Succeeded;}
        evidence.mappedError=owner.exports.map(status);evidence.mappedErrorValid=evidence.mappedError!=ERROR_MR_MID_NOT_FOUND&&evidence.mappedError!=ERROR_SUCCESS;return RenameResult::Failed;
    }
    /** @summary Issue only the single native class10 parent-relative rename using stable original storage.
     * @param owner Original qualified owner. @param record Original record/source. @param effect Publication or restoration. @param error Derived retry-only DOS error, never stored as GetLastError. @return Actual submission/completion classification. */
    static RenameResult Rename(OwnerState& owner,Record& record,EffectKind effect,std::uint32_t& error)noexcept{
        error=ERROR_GEN_FAILURE;if(Pending(owner)||record.rename.outstanding||!record.owned||!OriginalExports(owner)||(effect!=EffectKind::PublishRename&&effect!=EffectKind::RestoreRename))return RenameResult::Unavailable;
        const auto length=wcsnlen_s(owner.leaf.data(),owner.leaf.size());if(!length||length>255||std::wstring_view(owner.leaf.data(),length).find_first_of(L"\\/:")!=std::wstring_view::npos||std::wstring_view(owner.leaf.data(),length)==L"."||std::wstring_view(owner.leaf.data(),length)==L"..")return RenameResult::Unavailable;
        const auto leafBytes=length*sizeof(WCHAR);if(leafBytes>std::numeric_limits<ULONG>::max()-sizeof(NativeRenameInformation))return RenameResult::Unavailable;const auto size=sizeof(NativeRenameInformation)+leafBytes;if(size>record.rename.input.size()||record.rename.submission==UINT64_MAX)return RenameResult::Unavailable;
        auto& context=record.rename;context.input.fill(std::byte{});context.iosb.Status=PendingStatus;context.iosb.Information=0;context.operation=record.identity;context.source=record.owned.value;context.parent=Parent(owner);context.effect=effect;context.effectSequence=effect==EffectKind::PublishRename?record.result.operationSequence:record.result.compensationSequence;context.completionAcknowledged=false;++context.submission;
        auto* info=reinterpret_cast<NativeRenameInformation*>(context.input.data());info->ReplaceIfExists=TRUE;info->RootDirectory=context.parent;info->FileNameLength=static_cast<ULONG>(leafBytes);std::memcpy(info->FileName,owner.leaf.data(),leafBytes);
        auto& evidence=record.result.rename;evidence={};evidence.backend=RenameBackend::NtdllParentRelative;evidence.effect=effect;evidence.submission=context.submission;evidence.submittedLength=static_cast<std::uint32_t>(size);evidence.exportsQualified=true;evidence.setInformationRva=owner.exports.setRva;evidence.mapErrorRva=owner.exports.mapRva;
        record.result.lastEffectKind=effect;if(effect==EffectKind::PublishRename)++record.result.publicationCalls;else ++record.result.compensationCalls;
        const NTSTATUS status=owner.exports.set(context.source,&context.iosb,context.input.data(),static_cast<ULONG>(size),RenameInformationClass);evidence.callStatus=static_cast<std::uint32_t>(status);evidence.callValid=true;
        if(status==PendingStatus){context.outstanding=true;evidence.outstanding=true;record.result.lastNativeReturn=NativeReturn::Pending;record.result.current=Current::Unknown;record.result.observationDisposition=QueryDisposition::NotQueried;record.result.observationError=0;Unknown(owner,record);return RenameResult::Pending;}
        const auto result=FinishRename(owner,record,status);if(evidence.mappedErrorValid)error=evidence.mappedError;return result;
    }
    /** @summary Sole zero-wait completion consumer, called only by explicit Reconcile under its original Turn.
     * @param owner Original strongly retained owner. @param record Original outstanding record. @return True only after exact original submission, synchronization witness and qualified final IOSB are acknowledged once. */
    static bool CompleteRename(OwnerState& owner,Record& record)noexcept{
        auto& context=record.rename;auto& evidence=record.result.rename;if(!context.outstanding)return true;
        const auto* pending=Pending(owner);if(pending!=&record||context.completionAcknowledged||!evidence.callValid||evidence.callStatus!=static_cast<std::uint32_t>(PendingStatus)||!evidence.outstanding||evidence.completionAcknowledged||evidence.finalValid||context.operation!=record.identity||context.submission!=evidence.submission||!context.submission||context.source!=record.owned.value||context.parent!=Parent(owner)||context.effect!=evidence.effect||(context.effect!=EffectKind::PublishRename&&context.effect!=EffectKind::RestoreRename)||context.effectSequence!=(context.effect==EffectKind::PublishRename?record.result.operationSequence:record.result.compensationSequence))return false;
        evidence.waitAttempted=true;evidence.waitResult=WaitForSingleObject(context.source,0);evidence.waitError=evidence.waitResult==WAIT_FAILED?GetLastError():0;if(evidence.waitResult!=WAIT_OBJECT_0)return false;
        const NTSTATUS finalStatus=context.iosb.Status;
        // @summary Only zero-success or a negative terminal status is qualified here; pending/other informational values cannot consume the original request.
        if(finalStatus==PendingStatus||finalStatus>0)return false;
        evidence.information=static_cast<std::uint32_t>(context.iosb.Information);evidence.informationValid=true;FinishRename(owner,record,finalStatus);return true;
    }
};

StatePathWriter::Record::~Record(){if(rename.outstanding)std::terminate();owned.Reset();raw.reset();if(auto owner=localChargeOwner.lock())owner->slotCount.fetch_sub(1,std::memory_order_release);Native::Registry().operations[pool].store(false,std::memory_order_release);}
StatePathWriter::OwnerState::~OwnerState(){if(Native::Pending(*this))std::terminate();for(auto& record:records)record.reset();for(auto& handle:ancestors)handle.Reset();Native::Registry().owners[pool].occupied.store(false,std::memory_order_release);}
/** @summary Transfer exclusive admission without reopening it. @param other Original native turn. */
StatePathWriter::Turn::Turn(Turn&& other)noexcept:state_(std::move(other.state_)){}
/** @summary Release old admission before its native ownership can die. @param other Source turn. @return This turn. */
StatePathWriter::Turn&StatePathWriter::Turn::operator=(Turn&& other)noexcept{if(this!=&other){if(state_)state_->turn.store(false,std::memory_order_release);state_=std::move(other.state_);}return *this;}
/** @summary Release only native admission, never cancel or settle an operation. */
StatePathWriter::Turn::~Turn(){if(state_)state_->turn.store(false,std::memory_order_release);}
/** @summary Copy original fixed ledger while its turn is held. @return Exact historical scalars. */
StatePathWriter::Result StatePathWriter::Snapshot(const OwnerState& owner,const Record& record)noexcept{auto result=record.result;result.ownerPhase=owner.phase;result.ownerGeneration=owner.generation;result.latestEffectSequence=owner.latest;result.operation=record.identity;return result;}
/** @summary Resolve only original two-slot opaque identity, never path/text/address fallback. @return Retained exact record or empty. */
std::shared_ptr<StatePathWriter::Record> StatePathWriter::Find(OwnerState& owner,const OperationIdentity& identity)noexcept{for(const auto& record:owner.records)if(record&&record->identity==identity)return record;return {};}

/** @summary Qualify a closed local NTFS namespace after provisional pool admission. @return False before any file write on unsupported inputs. */
bool StatePathWriter::Qualify(const std::filesystem::path& input,OwnerState& owner)noexcept{
    try{
        const auto supplied=input.wstring();if(supplied.empty()||supplied.size()>4095||!ValidPathText(supplied)||supplied.starts_with(L"\\\\")||supplied.find(L':',2)!=std::wstring::npos)return false;
        if((input.has_root_name()&&!input.is_absolute())||(input.has_root_directory()&&!input.has_root_name()))return false;
        // @summary Reject forbidden supplied component spellings before Windows can normalize them; the explicit lexical dot remains allowed.
        for(const auto& component:input){if(component==L"..")return false;if(component==L".")continue;
            const auto& part=component.native();if(!part.empty()&&(part.back()==L'.'||part.back()==L' '))return false;
        }
        std::array<wchar_t,4097> full{};const auto length=GetFullPathNameW(supplied.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);if(!length||length>=full.size())return false;
        std::filesystem::path path(full.data());const auto leaf=path.filename().wstring(),parent=path.parent_path().wstring();if(leaf.empty()||leaf.size()>255||parent.size()>3840||leaf.ends_with(L'.')||leaf.ends_with(L' '))return false;
        if(std::any_of(leaf.begin(),leaf.end(),[](wchar_t c){return c<32||std::wstring_view(L"<>\"|?*").find(c)!=std::wstring_view::npos;}))return false;
        const auto stem=std::wstring_view(leaf).substr(0,leaf.find(L'.'));if(Equal(stem,L"CON")||Equal(stem,L"PRN")||Equal(stem,L"AUX")||Equal(stem,L"NUL")||Equal(stem,L"CLOCK$"))return false;
        if(stem.size()==4&&(Equal(stem.substr(0,3),L"COM")||Equal(stem.substr(0,3),L"LPT"))&&((stem[3]>=L'1'&&stem[3]<=L'9')||stem[3]==0xB9||stem[3]==0xB2||stem[3]==0xB3))return false;
        if(path.root_name().wstring().size()!=2||GetDriveTypeW(path.root_path().c_str())!=DRIVE_FIXED)return false;
        std::filesystem::path current=path.root_path();
        const auto openDirectory=[&](const std::filesystem::path& directory,bool final){if(owner.ancestorCount==owner.ancestors.size())return false;Handle handle(CreateFileW(directory.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|SYNCHRONIZE|(final?FILE_ADD_FILE:0),FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(!handle)return false;FILE_ATTRIBUTE_TAG_INFO attributes{};FILE_CASE_SENSITIVE_INFO sensitive{};if(!GetFileInformationByHandleEx(handle.value,FileAttributeTagInfo,&attributes,sizeof(attributes))||!(attributes.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!GetFileInformationByHandleEx(handle.value,FileCaseSensitiveInfo,&sensitive,sizeof(sensitive))||sensitive.Flags)return false;owner.ancestors[owner.ancestorCount++]=std::move(handle);return true;};
        if(!openDirectory(current,Equal(current.wstring(),parent)))return false;
        for(const auto& part:path.parent_path().relative_path()){current/=part;if(!openDirectory(current,Equal(current.wstring(),parent)))return false;}
        std::array<wchar_t,4097> canonical{};const auto longSize=GetLongPathNameW(parent.c_str(),canonical.data(),static_cast<DWORD>(canonical.size()));if(!longSize||longSize>=canonical.size()||!Equal(parent,canonical.data()))return false;
        std::array<wchar_t,32> filesystem{};if(!GetVolumeInformationByHandleW(Native::Parent(owner),nullptr,0,nullptr,nullptr,nullptr,filesystem.data(),static_cast<DWORD>(filesystem.size()))||!Equal(filesystem.data(),L"NTFS"))return false;
        FILE_STANDARD_INFO parentStandard{};
        if(!GetFileInformationByHandleEx(Native::Parent(owner),FileStandardInfo,&parentStandard,sizeof(parentStandard))||!parentStandard.Directory||parentStandard.DeletePending)return false;
        std::copy(parent.begin(),parent.end(),owner.parent.begin());std::copy(leaf.begin(),leaf.end(),owner.leaf.begin());owner.parentIdentity=Identity(Native::Parent(owner));if(!owner.parentIdentity.valid)return false;
        DWORD openError{};auto target=Native::OpenFile(owner,owner.leaf.data(),GENERIC_READ|FILE_READ_ATTRIBUTES,&openError);
        // @summary An absent tilde spelling has no actual long-leaf evidence; existing literal tildes still require Located below.
        if(!target)return openError==ERROR_FILE_NOT_FOUND&&leaf.find(L'~')==std::wstring::npos;
        FILE_STANDARD_INFO targetStandard{};
        if(!GetFileInformationByHandleEx(target.value,FileStandardInfo,&targetStandard,sizeof(targetStandard))||targetStandard.Directory||targetStandard.DeletePending||targetStandard.NumberOfLinks!=1)return false;
        DWORD error{};FileIdentity id;if(Native::File(target.value,id,error)!=QueryDisposition::Confirmed)return false;
        return Native::NormalizedLongLeaf(target.value,leaf)&&Native::Located(target.value,owner,owner.leaf.data());
    }catch(...){return false;}
}
/** @summary Mint only a qualified original native Owner through the closed fixture issuer. @return Exact admission outcome. */
StatePathWriter::Result StatePathWriter::Open(const std::filesystem::path& path,Owner& output)noexcept{
    if(output.state_||!OffLane())return Native::Fail();auto& registry=Native::Registry();std::size_t slot=OwnerLimit;std::uint64_t generation{};
    {std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock())return Native::Fail(Kind::Busy);if(registry.nextOwner==UINT64_MAX)return Native::Fail();for(std::size_t i=0;i<OwnerLimit;++i)if(!registry.owners[i].occupied.load()&&registry.owners[i].lifetime.expired()){slot=i;registry.owners[i].occupied.store(true);generation=registry.nextOwner++;break;}}
    if(slot==OwnerLimit)return Native::Fail();std::shared_ptr<OwnerState> candidate;
    try{candidate=std::make_shared<OwnerState>();candidate->pool=slot;candidate->generation=generation;}catch(...){registry.owners[slot].occupied.store(false);return Native::Fail();}
    if(!Native::Exports(candidate->exports)||!Qualify(path,*candidate))return Native::Fail();
    {std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock())return Native::Fail(Kind::Busy);for(const auto& entry:registry.owners)if(entry.active&&entry.active->parentIdentity==candidate->parentIdentity&&Equal(entry.active->leaf.data(),candidate->leaf.data())){output.state_=entry.active;auto result=Native::Fail(Kind::Ok);result.ownerGeneration=output.state_->generation;result.ownerPhase=output.state_->phase.load(std::memory_order_acquire);return result;}registry.owners[slot].active=candidate;registry.owners[slot].lifetime=candidate;output.state_=candidate;}
    auto result=Native::Fail(Kind::Ok);result.ownerGeneration=output.state_->generation;result.ownerPhase=output.state_->phase.load(std::memory_order_acquire);return result;
}
StatePathWriter::Result StatePathWriter::TryTurn(const Owner& owner,Turn& output)noexcept{
    if(output.state_||!owner.state_||!OffLane())return Native::Fail();bool expected{};if(!owner.state_->turn.compare_exchange_strong(expected,true,std::memory_order_acquire))return Native::Fail(Kind::Busy);
    if(owner.state_->phase==OwnerPhase::Closed){owner.state_->turn.store(false,std::memory_order_release);return Native::Fail(Kind::Closed);}output.state_=owner.state_;auto result=Native::Fail(Kind::Ok);result.ownerGeneration=owner.state_->generation;result.ownerPhase=owner.state_->phase.load();return result;
}
StatePathWriter::Result StatePathWriter::Prepare(Turn& turn,std::span<const ModId> enabled,Intent& output)noexcept{
    if(!turn.state_||output.record_||!OffLane())return Native::Fail();auto& owner=*turn.state_;if(Native::Pending(owner)||owner.phase!=OwnerPhase::Open||owner.sequence==UINT64_MAX)return Native::Fail(Kind::Unknown);
    if(owner.slotCount.load(std::memory_order_acquire)>=2)return Native::Fail();std::size_t local=2;for(std::size_t i=0;i<2;++i)if(!owner.records[i]){local=i;break;}if(local==2)return Native::Fail();auto& registry=Native::Registry();std::size_t pool=OperationLimit;std::uint64_t incarnation{};
    {std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock())return Native::Fail(Kind::Busy);if(registry.nextOperation==UINT64_MAX)return Native::Fail();for(std::size_t i=0;i<OperationLimit;++i){bool free{};if(registry.operations[i].compare_exchange_strong(free,true)){pool=i;incarnation=registry.nextOperation++;break;}}}
    if(pool==OperationLimit)return Native::Fail();std::shared_ptr<Record> record;
    try{
        record=std::make_shared<Record>();record->pool=pool;record->raw=std::make_unique<char[]>(2*RawLimit);record->expectedSequence=owner.latest;record->identity.owner_=owner.generation;record->identity.sequence_=++owner.sequence;record->identity.incarnation_=incarnation;record->result.operationSequence=owner.sequence;
        struct Sink {Record& record;} sink{*record};std::string error;const auto append=[](void* context,std::string_view bytes){auto& r=static_cast<Sink*>(context)->record;if(bytes.size()>RawLimit-r.candidateSize)return false;std::memcpy(r.raw.get()+RawLimit+r.candidateSize,bytes.data(),bytes.size());r.candidateSize+=bytes.size();return true;};if(!state_detail::Serialize(enabled,append,&sink,RawLimit,error))return Native::Fail();
        if(Identity(Native::Parent(owner))!=owner.parentIdentity)return Native::Fail(Kind::Conflict);
        DWORD openError{};auto prior=Native::OpenFile(owner,owner.leaf.data(),GENERIC_READ|FILE_READ_ATTRIBUTES,&openError);if(!prior){if(openError!=ERROR_FILE_NOT_FOUND)return Native::Fail();record->priorAbsent=true;}
        else {DWORD nativeError{};if(Native::File(prior.value,record->prior,nativeError)!=QueryDisposition::Confirmed||!Native::Bytes(prior.value,record->raw.get(),record->previousSize))return Native::Fail();FileIdentity after;if(Native::File(prior.value,after,nativeError)!=QueryDisposition::Confirmed||after!=record->prior)return Native::Fail();}
        record->result.kind=Kind::Ok;record->localChargeOwner=turn.state_;owner.records[local]=record;owner.slotCount.fetch_add(1,std::memory_order_release);output.owner_=turn.state_;output.record_=record;return Snapshot(owner,*record);
    }catch(...){if(!record)registry.operations[pool].store(false);return Native::Fail();}
}
/** @summary Deliver copied native facts outside registry locks; hook cannot replace syscalls. @return False on rejected/throwing cut. */
bool StatePathWriter::InvokeCut(unsigned cut,OwnerState& owner,Record& record,bool& suppress)noexcept{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    if(cutHook)try{StatePathWriterTestAccess::CutObservation observation;observation.cut=static_cast<StatePathWriterTestAccess::Cut>(cut);observation.result=Snapshot(owner,record);observation.owned=record.ownedIdentity;std::copy(owner.leaf.begin(),owner.leaf.end(),observation.parentLeaf.begin());observation.temporaryLeaf=record.temporary;const auto action=cutHook(observation);if(action==StatePathWriterTestAccess::CutAction::SuppressNextObservation){if(cut!=4&&cut!=6)return false;suppress=true;}}catch(...){return false;}
#else
    static_cast<void>(cut);static_cast<void>(owner);static_cast<void>(record);static_cast<void>(suppress);
#endif
    return true;
}

/** @summary Query actual identities/default bytes/streams without changing history or issuing effects. @return Fresh facts with explicit validity. */
StatePathWriter::Observation StatePathWriter::Inspect(OwnerState& owner,Record& record)noexcept{
    if(Native::Pending(owner)||record.rename.outstanding){Observation observation;observation.result=Native::PendingView(owner,record);return observation;}
    Observation observation;observation.result=Snapshot(owner,record);observation.priorAbsent=record.priorAbsent;observation.owned=record.ownedIdentity;
    if(record.suppress){record.suppress=false;observation.result.current=Current::Unknown;observation.result.observationDisposition=QueryDisposition::Unavailable;observation.result.observationError=0;observation.result.fixtureObservationUnavailable=true;return observation;}
    observation.result.fixtureObservationUnavailable=false;
    try{
        observation.parent=Identity(Native::Parent(owner));if(!observation.parent.valid||observation.parent!=owner.parentIdentity){observation.result.current=Current::Unknown;observation.result.observationDisposition=QueryDisposition::Unavailable;return observation;}
        DWORD openError{};auto current=Native::OpenFile(owner,owner.leaf.data(),GENERIC_READ|FILE_READ_ATTRIBUTES,&openError);if(!current){observation.result.observationError=openError;if(openError==ERROR_FILE_NOT_FOUND){observation.result.current=Current::ExpectedAbsent;observation.result.observationDisposition=QueryDisposition::Confirmed;}else{observation.result.current=Current::Unknown;observation.result.observationDisposition=QueryDisposition::Unavailable;}return observation;}
        DWORD error{};const auto disposition=Native::File(current.value,observation.destination,error);observation.result.observationDisposition=disposition;observation.result.observationError=error;observation.defaultStreamOnly=disposition==QueryDisposition::Confirmed;
        if(disposition!=QueryDisposition::Confirmed){observation.result.current=disposition==QueryDisposition::Unsupported?Current::Conflict:Current::Unknown;return observation;}
        std::size_t size{};const bool candidate=observation.destination==(record.candidate.valid?record.candidate:record.ownedIdentity);const bool restoration=observation.destination==(record.restoration.valid?record.restoration:record.ownedIdentity);
        QueryDisposition candidateRead=QueryDisposition::Conflict,restorationRead=QueryDisposition::Conflict;
        observation.candidateBytesMatch=candidate&&Native::Bytes(current.value,nullptr,size,record.raw.get()+RawLimit,record.candidateSize,&candidateRead);
        observation.restorationBytesMatch=restoration&&Native::Bytes(current.value,nullptr,size,record.raw.get(),record.previousSize,&restorationRead);
        const auto required=record.restore?restorationRead:candidateRead;if(required==QueryDisposition::Unavailable||required==QueryDisposition::Oversize){observation.result.current=Current::Unknown;observation.result.observationDisposition=required;return observation;}
        observation.result.current=record.restore&&observation.restorationBytesMatch?Current::MatchesRestoration:observation.candidateBytesMatch?Current::MatchesCandidate:Current::Conflict;
    }catch(...){observation.result.current=Current::Unknown;observation.result.observationDisposition=QueryDisposition::Unavailable;}
    return observation;
}
/** @summary Own/create/write/flush/verify one original temporary, retaining every partial effect. @return Written, qualified cleanup or quarantine. */
StatePathWriter::Result StatePathWriter::Temporary(OwnerState& owner,Record& record,bool restore)noexcept{
    if(Native::Pending(owner)||record.rename.outstanding)return Native::PendingView(owner,record);
    if(!Native::OriginalExports(owner)){record.result.kind=Kind::Rejected;record.result.phase=restore?Phase::CompensationFailed:Phase::Rejected;record.result.history=restore?History::CompensationFailed:History::Rejected;return Snapshot(owner,record);}
    try{
        record.restore=restore;record.disposition=false;record.removeCandidate=false;record.ownedIdentity={};
        for(unsigned collision=0;collision<8;++collision){
            const auto sequence=restore?record.result.compensationSequence:record.result.operationSequence;
            const int count=swprintf_s(record.temporary.data(),record.temporary.size(),L".simoder-state-%016llx-%016llx-%u.tmp",static_cast<unsigned long long>(owner.generation),static_cast<unsigned long long>(sequence),collision);if(count<=0||count>128)return Native::Fail();
            if(!InvokeCut(0,owner,record,record.suppress)){record.result.phase=restore?Phase::CompensationFailed:Phase::Rejected;record.result.history=restore?History::CompensationFailed:History::Rejected;record.result.kind=Kind::Rejected;return Snapshot(owner,record);}
            const auto path=Native::Path(owner,record.temporary.data());++record.result.temporaryCreateCalls;
            const HANDLE created=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);const DWORD error=created==INVALID_HANDLE_VALUE?GetLastError():0;record.owned=Handle(created);
            if(record.owned)break;record.result.nativeError=error;if(error!=ERROR_FILE_EXISTS&&error!=ERROR_ALREADY_EXISTS)break;
        }
        if(!record.owned){record.result.kind=Kind::Rejected;record.result.phase=restore?Phase::CompensationFailed:Phase::Rejected;record.result.history=restore?History::CompensationFailed:History::Rejected;return Snapshot(owner,record);}
        record.result.phase=Phase::TemporaryOwned;record.result.cleanup=Cleanup::StillTemporary;DWORD error{};const auto qualified=Native::File(record.owned.value,record.ownedIdentity,error);
        if(qualified!=QueryDisposition::Confirmed)return Native::Unknown(owner,record);
        if(!InvokeCut(1,owner,record,record.suppress))return CleanupTemporary(owner,record);
        const char* bytes=record.raw.get()+(restore?0:RawLimit);const auto size=restore?record.previousSize:record.candidateSize;std::size_t offset{};
        while(offset<size){const auto chunk=static_cast<DWORD>(std::min<std::size_t>(65536,size-offset));DWORD written{};++record.result.temporaryWriteCalls;const bool success=WriteFile(record.owned.value,bytes+offset,chunk,&written,nullptr)!=FALSE;const DWORD writeError=success?ERROR_SUCCESS:GetLastError();if(!success||written!=chunk){record.result.nativeError=writeError;return CleanupTemporary(owner,record);}offset+=chunk;}
        if(!FlushFileBuffers(record.owned.value)){record.result.nativeError=GetLastError();return CleanupTemporary(owner,record);}
        std::size_t checked{};FileIdentity after;if(!Native::Bytes(record.owned.value,nullptr,checked,bytes,size)||Native::File(record.owned.value,after,error)!=QueryDisposition::Confirmed||after!=record.ownedIdentity)return CleanupTemporary(owner,record);
        record.result.phase=Phase::Written;if(!InvokeCut(2,owner,record,record.suppress))return CleanupTemporary(owner,record);
        record.result.kind=Kind::Ok;return Snapshot(owner,record);
    }catch(...){if(record.owned)return CleanupTemporary(owner,record);record.result.kind=Kind::Rejected;record.result.phase=restore?Phase::CompensationFailed:Phase::Rejected;record.result.history=restore?History::CompensationFailed:History::Rejected;return Snapshot(owner,record);}
}
/** @summary Dispose only a positively still-temporary original handle; never deletes a replacement pathname. @return Confirmed cleanup or retained pending. */
StatePathWriter::Result StatePathWriter::CleanupTemporary(OwnerState& owner,Record& record)noexcept{
    if(Native::Pending(owner)||record.rename.outstanding)return Native::PendingView(owner,record);
    try{
        if(!InvokeCut(7,owner,record,record.suppress)||!Native::StillTemporary(owner,record)||!Native::Expected(owner,record,record.restore))return Native::Unknown(owner,record);
        record.result.phase=Phase::CleaningTemporary;FILE_DISPOSITION_INFO disposition{TRUE};++record.result.cleanupDispositionCalls;
        const bool success=SetFileInformationByHandle(record.owned.value,FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE;const DWORD error=success?0:GetLastError();record.result.lastEffectKind=EffectKind::RemoveTemporary;record.result.lastNativeReturn=success?NativeReturn::Succeeded:NativeReturn::Failed;record.result.nativeError=error;
        if(!success)return Native::Unknown(owner,record);record.disposition=true;record.result.cleanup=Cleanup::DispositionRequested;record.result.phase=Phase::CleanupDeletePending;
        const bool hook=InvokeCut(6,owner,record,record.suppress);record.owned.Reset();if(!hook)return Native::Unknown(owner,record);return ReconcileRecord(owner,record);
    }catch(...){return Native::Unknown(owner,record);}
}
/** @summary Confirm original native effects without replaying ambiguous rename/disposition. @param owner Original owner. @param record Original record. @param explicitCompletion Only public Reconcile permits exact zero-wait completion. @return Retained history/current outcome. */
StatePathWriter::Result StatePathWriter::ReconcileRecord(OwnerState& owner,Record& record,bool explicitCompletion)noexcept{
    if(Native::Pending(owner)||record.rename.outstanding){if(!explicitCompletion||!record.rename.outstanding||!Native::CompleteRename(owner,record))return Native::PendingView(owner,record);}
    try{
        if(record.result.settled)return Snapshot(owner,record);
        if((record.result.publicationConfirmed&&!record.compensationAttempted)||record.result.compensationConfirmed||record.result.phase==Phase::CompensationFailed){auto observed=Inspect(owner,record);record.result.current=observed.result.current;record.result.observationDisposition=observed.result.observationDisposition;record.result.observationError=observed.result.observationError;record.result.fixtureObservationUnavailable=observed.result.fixtureObservationUnavailable;return Snapshot(owner,record);}
        if(record.suppress){record.suppress=false;record.result.fixtureObservationUnavailable=true;record.result.observationDisposition=QueryDisposition::Unavailable;record.result.observationError=0;return Native::Unknown(owner,record);}
        if(record.disposition){
            DWORD openError{};auto file=Native::OpenFile(owner,record.removeCandidate?owner.leaf.data():record.temporary.data(),GENERIC_READ|FILE_READ_ATTRIBUTES,&openError);
            if(file){record.result.current=Current::Conflict;return Native::Unknown(owner,record);}
            record.result.observationError=openError;if(openError!=ERROR_FILE_NOT_FOUND){owner.phase=OwnerPhase::Quarantined;record.result.kind=Kind::Unknown;record.result.phase=record.removeCandidate?Phase::CompensatingDeletePending:Phase::CleanupDeletePending;return Snapshot(owner,record);}
            record.result.cleanup=Cleanup::AbsenceConfirmed;record.result.observationDisposition=QueryDisposition::Confirmed;
            if(record.removeCandidate){record.result.phase=Phase::Compensated;record.result.history=History::Compensated;record.result.kind=Kind::Compensated;record.result.compensationConfirmed=true;record.result.current=Current::ExpectedAbsent;owner.latest=record.result.compensationSequence;}
            else {record.result.phase=record.restore?Phase::CompensationFailed:Phase::Rejected;record.result.history=record.restore?History::CompensationFailed:History::Rejected;record.result.kind=Kind::Rejected;}
            Native::Thaw(owner);return Snapshot(owner,record);
        }
        if((record.publicationNativeSuccess&&!record.compensationAttempted)||record.restorationNativeSuccess){
            auto observed=Inspect(owner,record);record.result.current=observed.result.current;record.result.observationDisposition=observed.result.observationDisposition;record.result.observationError=observed.result.observationError;record.result.fixtureObservationUnavailable=observed.result.fixtureObservationUnavailable;
            DWORD openError{};auto temporary=Native::OpenFile(owner,record.temporary.data(),GENERIC_READ|FILE_READ_ATTRIBUTES,&openError);const bool absent=!temporary&&openError==ERROR_FILE_NOT_FOUND;
            const bool positive=absent&&observed.defaultStreamOnly&&(record.restorationNativeSuccess?observed.restorationBytesMatch:observed.candidateBytesMatch);
            if(!positive)return Native::Unknown(owner,record);
            if(record.restorationNativeSuccess){record.restoration=observed.destination;record.result.compensationConfirmed=true;record.result.kind=Kind::Compensated;record.result.phase=Phase::Compensated;record.result.history=History::Compensated;owner.latest=record.result.compensationSequence;}
            else {record.candidate=observed.destination;record.result.publicationConfirmed=true;record.result.kind=Kind::Published;record.result.phase=Phase::Published;record.result.history=History::Published;owner.latest=record.result.operationSequence;}
            record.result.cleanup=Cleanup::None;record.owned.Reset();Native::Thaw(owner);return Snapshot(owner,record);
        }
        if(record.owned&&Native::StillTemporary(owner,record))return CleanupTemporary(owner,record);
        return Snapshot(owner,record);
    }catch(...){return Native::Unknown(owner,record);}
}
StatePathWriter::Result StatePathWriter::Publish(Turn& turn,Intent& intent,Receipt& output)noexcept{
    if(!turn.state_||intent.owner_!=turn.state_||!intent.record_||output.record_||!OffLane())return Native::Fail();auto& owner=*turn.state_;auto& record=*intent.record_;
    if(Native::Pending(owner)||record.rename.outstanding)return Native::PendingView(owner,record);
    if(record.attempted||record.result.settled||record.result.phase!=Phase::Prepared||owner.phase!=OwnerPhase::Open)return Native::Fail(Kind::Conflict);
    record.attempted=true;output.owner_=turn.state_;output.record_=intent.record_;
    try{
        if(owner.latest!=record.expectedSequence||!Native::Expected(owner,record,false)){record.result.kind=Kind::Conflict;record.result.phase=Phase::Rejected;record.result.history=History::Rejected;record.result.current=Current::Conflict;return Snapshot(owner,record);}
        auto prepared=Temporary(owner,record,false);if(prepared.kind!=Kind::Ok)return prepared;
        struct Context {OwnerState& owner;Record& record;} context{owner,record};
        const auto effect=[](void* pointer,std::uint32_t& error){auto& [owner,record]=*static_cast<Context*>(pointer);
            if(!InvokeCut(3,owner,record,record.suppress)||owner.latest!=record.expectedSequence||!Native::Expected(owner,record,false)||!Native::StillTemporary(owner,record))return state_detail::Attempt::Stop;
            record.result.phase=Phase::Publishing;const auto renamed=Native::Rename(owner,record,EffectKind::PublishRename,error);const bool success=renamed==Native::RenameResult::Succeeded;
            if(renamed==Native::RenameResult::Pending)return state_detail::Attempt::StopAfterEffect;
            if(renamed==Native::RenameResult::Unavailable)return state_detail::Attempt::Stop;
            if(!InvokeCut(4,owner,record,record.suppress)||record.suppress){record.suppress=true;Native::Unknown(owner,record);return state_detail::Attempt::StopAfterEffect;}
            if(success)return state_detail::Attempt::Succeeded;
            if(!Native::StillTemporary(owner,record)||!Native::Expected(owner,record,false)){Native::Unknown(owner,record);return state_detail::Attempt::StopAfterEffect;}return state_detail::Attempt::Failed;};
        const auto result=state_detail::PublishWithRetry(effect,&context);record.result.elapsedBackoffMs=result.backoffMs;
        if(result.exception)return Native::Unknown(owner,record);
        if(record.publicationNativeSuccess)return ReconcileRecord(owner,record);
        if(record.result.phase==Phase::Unknown)return Snapshot(owner,record);
        return CleanupTemporary(owner,record);
    }catch(...){if(record.publicationNativeSuccess)return Native::Unknown(owner,record);if(record.owned)return CleanupTemporary(owner,record);record.result.phase=Phase::Rejected;record.result.history=History::Rejected;record.result.kind=Kind::Rejected;return Snapshot(owner,record);}
}

StatePathWriter::Result StatePathWriter::Compensate(Turn& turn,const Receipt& receipt)noexcept{
    if(!turn.state_||receipt.owner_!=turn.state_||!receipt.record_||!OffLane())return Native::Fail();auto& owner=*turn.state_;auto& record=*receipt.record_;
    if(Native::Pending(owner)||record.rename.outstanding)return Native::PendingView(owner,record);
    if(record.compensationAttempted){auto observed=Inspect(owner,record);record.result.current=observed.result.current;record.result.observationDisposition=observed.result.observationDisposition;record.result.observationError=observed.result.observationError;record.result.fixtureObservationUnavailable=observed.result.fixtureObservationUnavailable;return Snapshot(owner,record);}
    if(owner.phase!=OwnerPhase::Open||record.result.settled||!record.result.publicationConfirmed||owner.latest!=record.result.operationSequence||owner.sequence==UINT64_MAX){auto result=Snapshot(owner,record);result.kind=owner.phase==OwnerPhase::Quarantined?Kind::Unknown:Kind::Conflict;result.current=Current::Unobserved;return result;}
    try{
        if(!InvokeCut(5,owner,record,record.suppress)||!Native::Expected(owner,record,true)){record.result.kind=Kind::Conflict;record.result.current=Current::Conflict;return Snapshot(owner,record);}
        record.compensationAttempted=true;record.result.compensationSequence=++owner.sequence;
        if(record.priorAbsent){
            DWORD openError{};record.owned=Native::OpenFile(owner,owner.leaf.data(),GENERIC_READ|FILE_READ_ATTRIBUTES|DELETE,&openError);if(!record.owned){record.result.kind=Kind::Conflict;record.result.phase=Phase::CompensationFailed;record.result.history=History::CompensationFailed;record.result.nativeError=openError;return Snapshot(owner,record);}
            DWORD error{};FileIdentity id;std::size_t size{};if(Native::File(record.owned.value,id,error)!=QueryDisposition::Confirmed||id!=record.candidate||!Native::Located(record.owned.value,owner,owner.leaf.data())||!Native::Bytes(record.owned.value,nullptr,size,record.raw.get()+RawLimit,record.candidateSize)){record.owned.Reset();record.result.kind=Kind::Conflict;record.result.phase=Phase::CompensationFailed;record.result.history=History::CompensationFailed;return Snapshot(owner,record);}
            record.ownedIdentity=id;
            struct Context {OwnerState& owner;Record& record;} context{owner,record};
            const auto effect=[](void* pointer,std::uint32_t& error){auto& [owner,record]=*static_cast<Context*>(pointer);
                if(!InvokeCut(3,owner,record,record.suppress)||owner.latest!=record.result.operationSequence||!Native::CandidateUnchanged(owner,record)){Native::Unknown(owner,record);return state_detail::Attempt::Stop;}
                FILE_DISPOSITION_INFO disposition{TRUE};++record.result.compensationCalls;
                const bool success=SetFileInformationByHandle(record.owned.value,FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE;error=success?0:GetLastError();record.result.lastEffectKind=EffectKind::RemovePublishedCandidate;record.result.lastNativeReturn=success?NativeReturn::Succeeded:NativeReturn::Failed;record.result.nativeError=error;
                if(success){record.removeCandidate=true;record.disposition=true;record.result.phase=Phase::CompensatingDeletePending;record.result.cleanup=Cleanup::DispositionRequested;}
                const bool hook=InvokeCut(6,owner,record,record.suppress);
                if(success){record.owned.Reset();if(!hook||record.suppress){record.suppress=true;Native::Unknown(owner,record);return state_detail::Attempt::StopAfterEffect;}return state_detail::Attempt::Succeeded;}
                if(!hook||record.suppress||!Native::CandidateUnchanged(owner,record)){record.suppress=record.suppress||!hook;Native::Unknown(owner,record);return state_detail::Attempt::StopAfterEffect;}
                return state_detail::Attempt::Failed;};
            const auto retried=state_detail::PublishWithRetry(effect,&context);record.result.elapsedBackoffMs=retried.backoffMs;
            if(retried.exception)return Native::Unknown(owner,record);
            if(record.result.phase==Phase::Unknown)return Snapshot(owner,record);
            if(record.disposition)return ReconcileRecord(owner,record);
            record.owned.Reset();record.result.kind=Kind::Rejected;record.result.phase=Phase::CompensationFailed;record.result.history=History::CompensationFailed;return Snapshot(owner,record);
        }
        auto temporary=Temporary(owner,record,true);if(temporary.kind!=Kind::Ok)return temporary;
        struct Context {OwnerState& owner;Record& record;} context{owner,record};
        const auto effect=[](void* pointer,std::uint32_t& error){auto& [owner,record]=*static_cast<Context*>(pointer);
            if(!InvokeCut(3,owner,record,record.suppress)||owner.latest!=record.result.operationSequence||!Native::Expected(owner,record,true)||!Native::StillTemporary(owner,record))return state_detail::Attempt::Stop;
            record.result.phase=Phase::CompensatingRestore;const auto renamed=Native::Rename(owner,record,EffectKind::RestoreRename,error);const bool success=renamed==Native::RenameResult::Succeeded;
            if(renamed==Native::RenameResult::Pending)return state_detail::Attempt::StopAfterEffect;
            if(renamed==Native::RenameResult::Unavailable)return state_detail::Attempt::Stop;
            if(!InvokeCut(4,owner,record,record.suppress)||record.suppress){record.suppress=true;Native::Unknown(owner,record);return state_detail::Attempt::StopAfterEffect;}
            if(success)return state_detail::Attempt::Succeeded;
            if(!Native::StillTemporary(owner,record)||!Native::Expected(owner,record,true)){Native::Unknown(owner,record);return state_detail::Attempt::StopAfterEffect;}return state_detail::Attempt::Failed;};
        const auto result=state_detail::PublishWithRetry(effect,&context);record.result.elapsedBackoffMs=result.backoffMs;
        if(result.exception)return Native::Unknown(owner,record);
        if(record.restorationNativeSuccess)return ReconcileRecord(owner,record);if(record.result.phase==Phase::Unknown)return Snapshot(owner,record);return CleanupTemporary(owner,record);
    }catch(...){if(record.restorationNativeSuccess||record.disposition)return Native::Unknown(owner,record);if(record.owned)return CleanupTemporary(owner,record);record.result.phase=Phase::CompensationFailed;record.result.history=History::CompensationFailed;record.result.kind=Kind::Rejected;return Snapshot(owner,record);}
}
StatePathWriter::Observation StatePathWriter::Observe(Turn& turn,const Receipt& receipt)noexcept{
    if(!turn.state_||receipt.owner_!=turn.state_||!receipt.record_||!OffLane()){Observation result;result.result=Native::Fail();return result;}
    if(Native::Pending(*turn.state_)||receipt.record_->rename.outstanding){Observation result;result.result=Native::PendingView(*turn.state_,*receipt.record_);return result;}
    auto& owner=*turn.state_;if(owner.phase==OwnerPhase::Closed){Observation result;result.result=Snapshot(owner,*receipt.record_);result.result.current=Current::OwnerClosed;return result;}
    auto result=Inspect(owner,*receipt.record_);receipt.record_->result.current=result.result.current;receipt.record_->result.observationDisposition=result.result.observationDisposition;receipt.record_->result.observationError=result.result.observationError;receipt.record_->result.fixtureObservationUnavailable=result.result.fixtureObservationUnavailable;return result;
}
StatePathWriter::Result StatePathWriter::HistoryView(const Receipt& receipt)noexcept{
    if(!receipt.owner_||!receipt.record_)return Native::Fail();auto& owner=*receipt.owner_;bool free{};if(!owner.turn.compare_exchange_strong(free,true,std::memory_order_acquire))return Native::Fail(Kind::Busy);
    if(Native::Pending(owner)||receipt.record_->rename.outstanding){auto result=Native::PendingView(owner,*receipt.record_);owner.turn.store(false,std::memory_order_release);return result;}
    auto result=Snapshot(owner,*receipt.record_);result.current=owner.phase==OwnerPhase::Closed?Current::OwnerClosed:Current::Unobserved;result.observationDisposition=QueryDisposition::NotQueried;result.observationError=0;result.fixtureObservationUnavailable=false;owner.turn.store(false,std::memory_order_release);return result;
}
StatePathWriter::Result StatePathWriter::Reconcile(Turn& turn,const OperationIdentity& operation)noexcept{
    if(!turn.state_||!OffLane())return Native::Fail();auto record=Find(*turn.state_,operation);if(!record)return Native::Fail();return ReconcileRecord(*turn.state_,*record,true);
}
StatePathWriter::Result StatePathWriter::Settle(Turn& turn,const OperationIdentity& operation)noexcept{
    if(!turn.state_||!OffLane())return Native::Fail();auto& owner=*turn.state_;auto record=Find(owner,operation);if(!record)return Native::Fail();
    if(Native::Pending(owner)||record->rename.outstanding)return Native::PendingView(owner,*record);
    const auto phase=record->result.phase;if(phase!=Phase::Published&&phase!=Phase::Compensated&&phase!=Phase::Rejected&&phase!=Phase::CompensationFailed)return Native::Fail(Kind::Unknown);
    if(record->owned)return Native::Fail(Kind::Unknown);record->result.settled=true;record->result.phase=Phase::Settled;record->result.kind=Kind::Ok;for(auto& slot:owner.records)if(slot==record){slot.reset();break;}Native::Thaw(owner);return Snapshot(owner,*record);
}
StatePathWriter::Result StatePathWriter::CancelPrepared(Turn& turn,const OperationIdentity& operation)noexcept{
    if(!turn.state_||!OffLane())return Native::Fail();auto& owner=*turn.state_;auto record=Find(owner,operation);if(!record)return Native::Fail();if(Native::Pending(owner)||record->rename.outstanding)return Native::PendingView(owner,*record);if(record->attempted||record->result.phase!=Phase::Prepared||record->result.temporaryCreateCalls)return Native::Fail();record->result.kind=Kind::Ok;record->result.history=History::Cancelled;record->result.phase=Phase::Cancelled;record->result.settled=true;for(auto& slot:owner.records)if(slot==record){slot.reset();break;}return Snapshot(owner,*record);
}
StatePathWriter::OperationList StatePathWriter::Operations(Turn& turn)noexcept{
    OperationList list;if(!turn.state_)return list;const auto* pending=Native::Pending(*turn.state_);list.result=pending?Native::PendingView(*turn.state_,*pending):Native::Fail(Kind::Ok);list.result.ownerGeneration=turn.state_->generation;list.result.ownerPhase=turn.state_->phase.load();list.result.latestEffectSequence=turn.state_->latest;for(const auto& record:turn.state_->records)if(record){auto& summary=list.operations[list.count++];summary.operation=record->identity;summary.phase=record->result.phase;summary.history=record->result.history;summary.cleanup=record->result.cleanup;summary.settled=record->result.settled;summary.nativeRenameOutstanding=record->rename.outstanding;summary.rawBytesReserved=2*RawLimit;}return list;
}
StatePathWriter::Result StatePathWriter::CloseOwner(Owner& value)noexcept{
    if(!value.state_||!OffLane())return Native::Fail();auto owner=value.state_;bool free{};if(!owner->turn.compare_exchange_strong(free,true,std::memory_order_acquire))return Native::Fail(Kind::Busy);
    if(const auto* pending=Native::Pending(*owner)){auto result=Native::PendingView(*owner,*pending);owner->turn.store(false,std::memory_order_release);return result;}
    if(owner->phase==OwnerPhase::Closed){owner->turn.store(false,std::memory_order_release);return Native::Fail(Kind::Closed);}
    if(owner->phase==OwnerPhase::Quarantined||std::any_of(owner->records.begin(),owner->records.end(),[](const auto& record){return bool(record);})){owner->turn.store(false,std::memory_order_release);return Native::Fail(Kind::Unknown);}
    std::shared_ptr<OwnerState> detached;{auto& registry=Native::Registry();std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock()){owner->turn.store(false,std::memory_order_release);return Native::Fail(Kind::Busy);}owner->phase=OwnerPhase::Closed;detached=std::move(registry.owners[owner->pool].active);}
    owner->turn.store(false,std::memory_order_release);for(auto& handle:owner->ancestors)handle.Reset();detached.reset();auto result=Native::Fail(Kind::Closed);result.ownerPhase=OwnerPhase::Closed;result.ownerGeneration=owner->generation;return result;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
StatePathWriter::Result StatePathWriterTestAccess::Open(const std::filesystem::path& path,StatePathWriter::Owner& owner)noexcept{return StatePathWriter::Open(path,owner);}
void StatePathWriterTestAccess::Hook(std::function<CutAction(const CutObservation&)> callback){cutHook=std::move(callback);}
StatePathWriterTestAccess::Metrics StatePathWriterTestAccess::Inspect(const StatePathWriter::Owner& owner)noexcept{
    Metrics metrics;auto& registry=StatePathWriter::Native::Registry();for(const auto& entry:registry.owners)if(entry.occupied.load())++metrics.ownerPoolOccupied;for(const auto& entry:registry.operations)if(entry.load())++metrics.globalOperationPoolOccupied;metrics.rawBytesReserved=metrics.globalOperationPoolOccupied*2*RawLimit;metrics.ownerBytes=sizeof(StatePathWriter::OwnerState);metrics.recordBytes=sizeof(StatePathWriter::Record);metrics.nativeRenameContextBytes=sizeof(NativeRenameContext);metrics.renameEvidenceBytes=sizeof(StatePathWriter::RenameEvidence);
    // @summary This is the actual admission counter, updated at exact slot attachment/detachment; no fixture-supplied count or mutable slot read.
    if(owner.state_)metrics.thisOwnerSlotCount=owner.state_->slotCount.load(std::memory_order_acquire);
    return metrics;
}
StatePathWriterTestAccess::NativeExportObservation StatePathWriterTestAccess::ExportProvenance(const StatePathWriter::Owner& value)noexcept{
    NativeExportObservation observation;if(!value.state_||!OffLane())return observation;auto owner=value.state_;bool free{};if(!owner->turn.compare_exchange_strong(free,true,std::memory_order_acquire)){observation.kind=StatePathWriter::Kind::Busy;return observation;}
    if(StatePathWriter::Native::Pending(*owner)){observation.kind=StatePathWriter::Kind::Unknown;owner->turn.store(false,std::memory_order_release);return observation;}
    NativeExports current;if(owner->phase==StatePathWriter::OwnerPhase::Closed||!StatePathWriter::Native::Exports(current,&observation.modulePath,&observation.mappedPath)||!StatePathWriter::Native::SameExports(owner->exports,current)){owner->turn.store(false,std::memory_order_release);return observation;}
    const auto& exports=current;observation.moduleBase=reinterpret_cast<std::uintptr_t>(exports.module);observation.setInformationAddress=reinterpret_cast<std::uintptr_t>(exports.set);observation.mapErrorAddress=reinterpret_cast<std::uintptr_t>(exports.map);observation.imageSize=exports.imageSize;observation.setInformationRva=exports.setRva;observation.mapErrorRva=exports.mapRva;observation.systemFileIdentity=exports.systemFileIdentity;observation.qualified=true;observation.kind=StatePathWriter::Kind::Ok;
    owner->turn.store(false,std::memory_order_release);return observation;
}
StatePathWriter::QueryDisposition StatePathWriterTestAccess::ParseStreams(std::span<const std::byte> bytes)noexcept{return Streams(bytes);}
#endif
}
