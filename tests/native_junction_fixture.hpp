#pragma once
#include <Windows.h>
#include <winioctl.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sc13::testing {
/** @summary Shared real NTFS junction transport for isolated tests only.
 * @tparam Policy Caller-specific observation, containment and exclusive-directory checks.
 * @details No authority, global counter, shell, retries or privilege adjustment. */
template<class Policy> struct NativeJunctionFixture final{
/** @summary Own only an actual fixture-native handle. */
struct JunctionHandle final{
    HANDLE value{INVALID_HANDLE_VALUE};
    /** @summary Adopt the original native result. @param handle Actual returned handle. */
    explicit JunctionHandle(HANDLE handle):value(handle){}
    /** @summary Close the original handle before exact authored-node removal. */
    ~JunctionHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    JunctionHandle(const JunctionHandle&)=delete;JunctionHandle& operator=(const JunctionHandle&)=delete;
};
/** @summary Copied native volume/object identity, never an address or target path surrogate. */
struct JunctionIdentity final{std::uint64_t volume{};std::array<unsigned char,16> file{};bool operator==(const JunctionIdentity&)const=default;};
/** @summary Read the actual exact node identity from its original native handle. @param handle Nonfollowing node. @return Native copied identity. */
static JunctionIdentity NodeIdentity(HANDLE handle){FILE_ID_INFO value{};Policy::Require(GetFileInformationByHandleEx(handle,FileIdInfo,&value,sizeof(value))!=FALSE,"actual FileIdInfo query");JunctionIdentity result;result.volume=value.VolumeSerialNumber;std::memcpy(result.file.data(),value.FileId.Identifier,16);return result;}
/** @summary Open only the fixed authored node without traversing its reparse data. @param path Fixed owned node. @param write Whether creating mount-point data. @return Actual handle. */
static HANDLE OpenNode(const std::filesystem::path& path,bool write=false){const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|(write?GENERIC_WRITE:0U),FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);if(handle==INVALID_HANDLE_VALUE){const auto error=GetLastError();throw std::runtime_error("native nonfollowing open failed Win32="+std::to_string(error));}return handle;}
/** @summary Exact bounded Microsoft mount-point layout using installed Win32 headers, without WDK dependency. */
struct MountBuffer final{DWORD tag{};WORD dataLength{},reserved{},substituteOffset{},substituteLength{},printOffset{},printLength{};wchar_t paths[(16384-16)/sizeof(wchar_t)]{};};
static_assert(sizeof(wchar_t)==2&&offsetof(MountBuffer,paths)==16&&sizeof(MountBuffer)==16384);
/** @summary Prove genuine mount-point tag/data and original own-node identity. @param handle Actual nonfollowing node. @param original Original node identity. @param substitute Exact substitute name. @param print Exact print name. */
static void VerifyJunction(HANDLE handle,const JunctionIdentity& original,const std::wstring& substitute,const std::wstring& print){
    Policy::Require(NodeIdentity(handle)==original,"junction still original authored native node");FILE_ATTRIBUTE_TAG_INFO attributes{};Policy::Require(GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&attributes,sizeof(attributes))!=FALSE,"actual native attribute/tag query");Policy::Require((attributes.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))==(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)&&attributes.ReparseTag==IO_REPARSE_TAG_MOUNT_POINT,"actual directory mount-point provenance required");
    MountBuffer returned{};DWORD bytes{};if(!DeviceIoControl(handle,FSCTL_GET_REPARSE_POINT,nullptr,0,&returned,sizeof(returned),&bytes,nullptr)){const auto error=GetLastError();throw std::runtime_error("actual GET_REPARSE_POINT failed Win32="+std::to_string(error));}
    Policy::Require(bytes>=16&&bytes<=sizeof(returned)&&returned.tag==IO_REPARSE_TAG_MOUNT_POINT&&returned.reserved==0&&std::size_t(returned.dataLength)+8==bytes,"actual returned mount-point header/length");
    const auto valid=[&](WORD offset,WORD length){return offset%2==0&&length%2==0&&std::size_t(offset)+length+sizeof(wchar_t)<=bytes-16;};
    Policy::Require(valid(returned.substituteOffset,returned.substituteLength)&&valid(returned.printOffset,returned.printLength),"actual returned string offsets remain bounded");
    const auto* substituteText=reinterpret_cast<const wchar_t*>(reinterpret_cast<const std::byte*>(returned.paths)+returned.substituteOffset);
    const auto* printText=reinterpret_cast<const wchar_t*>(reinterpret_cast<const std::byte*>(returned.paths)+returned.printOffset);
    Policy::Require(std::wstring_view(substituteText,returned.substituteLength/2)==substitute&&substituteText[returned.substituteLength/2]==L'\0'&&std::wstring_view(printText,returned.printLength/2)==print&&printText[returned.printLength/2]==L'\0',"actual GET names equal exact owned target, with bounded terminators");
}
/** @summary Create one genuine bounded native junction with no retry, privilege adjustment, shell or fallback. @param owner Fresh plain owner. @param link Fixed absent child. @param target Fixed plain directory. @param setCalls Caller-owned actual SET counter. @param maximumSetCalls Fixed finite call budget. @return Actual original junction identity. */
static JunctionIdentity Junction(const std::filesystem::path& owner,const std::filesystem::path& link,const std::filesystem::path& target,unsigned& setCalls,unsigned maximumSetCalls){
    Policy::Require(maximumSetCalls>0&&maximumSetCalls<=3,"bounded caller-owned native SET budget");
    Policy::ValidateChild(owner,link);Policy::ValidateChild(owner,target);Policy::ValidatePlain(target,true);Policy::Require(link==link.lexically_normal()&&target==target.lexically_normal()&&link.root_path()==target.root_path(),"same-volume fixed normalized native junction paths");Policy::Require(GetFileAttributesW(link.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"junction destination must be absent");
    const auto node=Policy::CreateDirectory(link.parent_path(),link.filename().c_str());JunctionHandle handle(OpenNode(node,true));const auto original=NodeIdentity(handle.value);const auto print=target.wstring();const auto substitute=std::wstring(L"\\??\\")+print;
    Policy::Require(print.size()<=2048&&substitute.size()<=2052,"fixed native mount-point name bound");MountBuffer buffer{};const auto substituteBytes=substitute.size()*sizeof(wchar_t),printBytes=print.size()*sizeof(wchar_t);const auto total=16+substituteBytes+sizeof(wchar_t)+printBytes+sizeof(wchar_t);Policy::Require(total<=sizeof(buffer)&&total-8<=MAXWORD,"checked native reparse payload bytes");
    buffer.tag=IO_REPARSE_TAG_MOUNT_POINT;buffer.dataLength=static_cast<WORD>(total-8);buffer.substituteLength=static_cast<WORD>(substituteBytes);buffer.printOffset=static_cast<WORD>(substituteBytes+sizeof(wchar_t));buffer.printLength=static_cast<WORD>(printBytes);std::memcpy(buffer.paths,substitute.data(),substituteBytes);std::memcpy(reinterpret_cast<std::byte*>(buffer.paths)+buffer.printOffset,print.data(),printBytes);
    ++setCalls;Policy::Require(setCalls<=maximumSetCalls,"at most caller-declared actual SET calls");DWORD returned{};const BOOL success=DeviceIoControl(handle.value,FSCTL_SET_REPARSE_POINT,&buffer,static_cast<DWORD>(total),nullptr,0,&returned,nullptr);const DWORD error=success?ERROR_SUCCESS:GetLastError();std::cout<<"nativeSET case="<<setCalls<<" success="<<(success?1:0)<<" error="<<error<<" inputBytes="<<total<<std::endl;if(!success)throw std::runtime_error("actual junction SET failed Win32="+std::to_string(error));
    VerifyJunction(handle.value,original,substitute,print);const DWORD attributes=GetFileAttributesW(link.c_str());Policy::Require(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&(attributes&FILE_ATTRIBUTE_DIRECTORY),"actual path directory/reparse attributes");return original;
}
/** @summary Remove only the exact positively verified authored junction, never its target or a recursive tree. @param owner Original plain owner. @param link Fixed junction node. @param target Exact plain target. @param identity Original junction identity. */
static void RemoveJunction(const std::filesystem::path& owner,const std::filesystem::path& link,const std::filesystem::path& target,const JunctionIdentity& identity){Policy::ValidateChild(owner,link);Policy::ValidateChild(owner,target);{JunctionHandle node(OpenNode(link));VerifyJunction(node.value,identity,std::wstring(L"\\??\\")+target.wstring(),target.wstring());}Policy::Require(RemoveDirectoryW(link.c_str())!=FALSE,"exact positively verified authored junction removal");Policy::Require(GetFileAttributesW(link.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"exact authored link absent after removal");}
};
}
