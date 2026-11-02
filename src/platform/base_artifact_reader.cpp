#include "platform/base_artifact_reader.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace sc13::platform::base_artifact {
namespace {
/** @summary Retained ordinary source/ancestor handles are always closed off Core guards. */
struct Handle final {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default;
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
};
/** @summary Return true only for a zero-filled byte extent. @param bytes Exact extent. @return Zero-only. */
bool Zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(),bytes.end(),[](std::byte c){return c==std::byte{};});
}
/** @summary ASCII-only fold; no locale or Unicode aliasing. @param c ASCII byte. @return Lowercase byte. */
char Fold(char c) noexcept {return c>='A'&&c<='Z'?static_cast<char>(c-'A'+'a'):c;}
/** @summary Compare Win32 ASCII names case-insensitively. @param a First. @param b Second. @return Equality. */
bool EqualFold(std::string_view a,std::string_view b) noexcept {
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[](char x,char y){return Fold(x)==Fold(y);});
}
} // namespace

/** @summary Reject traversal, ADS, forbidden bytes, DOS aliases and noncanonical components.
 * @param path Exact slash-relative ASCII name. @return Whether the finite profile admits it. */
bool NameValid(std::string_view path) noexcept {
    if(path.empty()||path.size()>MaximumPath||path.front()=='/'||path.back()=='/')return false;
    std::size_t start=0,parts=0;
    while(start<path.size()) {
        const auto slash=path.find('/',start), end=slash==path.npos?path.size():slash;
        const auto p=path.substr(start,end-start);
        if(++parts>16||p.empty()||p.size()>100||p=="."||p==".."||p.back()=='.'||p.back()==' ')return false;
        for(unsigned char c:p)if(c<33||c>126||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return false;
        const auto stem=p.substr(0,p.find('.'));
        if(EqualFold(stem,"con")||EqualFold(stem,"prn")||EqualFold(stem,"aux")||EqualFold(stem,"nul")||
            (stem.size()==4&&(EqualFold(stem.substr(0,3),"com")||EqualFold(stem.substr(0,3),"lpt"))&&stem[3]>='1'&&stem[3]<='9'))return false;
        if(slash==path.npos)break;
        start=end+1;
    }
    return true;
}
/** @summary Validate native trust-map shape without reading an archive or adopting its manifest.
 * @param pin Native caller-selected complete map. @return Whether names and all finite caps hold. */
bool ValidateTrustedArtifact(const TrustedArtifact& pin) noexcept {
    if(pin.size<1024||pin.size>MaximumArtifact||pin.size%512||pin.files.size()<3||pin.files.size()>MaximumEntries)return false;
    std::size_t executable=0,notices=0,total=0;bool manifest=false,entry=false;
    for(std::size_t i=0;i<pin.files.size();++i){const auto& f=pin.files[i];
        if(!NameValid(f.path))return false;
        for(std::size_t j=0;j<i;++j){const auto other=pin.files[j].path;
            if(EqualFold(f.path,other))return false;
            // @summary A file cannot also be a slash-boundary parent; compare BOTH orderings under actual ASCII fold.
            if((f.path.size()>other.size()&&f.path[other.size()]=='/'&&EqualFold(f.path.substr(0,other.size()),other))||
               (other.size()>f.path.size()&&other[f.path.size()]=='/'&&EqualFold(other.substr(0,f.path.size()),f.path)))return false;
        }
        if(f.notice){if(f.path!="LICENSE"||f.size>MaximumNotice)return false;++notices;}
        else {
            if(f.path!="mod.toml"&&(!f.path.starts_with("scripts/")||!f.path.ends_with(".luau")))return false;
            for(char c:f.path)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='/'||c=='.'||c=='-'||c=='_'))return false;
            if(f.size>MaximumExecutable-total)return false;total+=f.size;++executable;
            manifest|=f.path=="mod.toml";entry|=f.path=="scripts/init.luau";
        }
    }
    return notices==1&&executable>=2&&executable<=64&&manifest&&entry;
}
namespace {
/** @summary Decode Python USTAR octal field grammar only, with checked 32-bit accumulation.
 * @param field Octal digits followed by NUL or space terminator(s). @param value Decoded output.
 * @return False for sign/base256/empty/overflow/interior whitespace/nonzero trailing bytes. */
bool Octal(std::span<const std::byte> field,std::uint32_t& value) noexcept {
    value=0;std::size_t digits=0;
    while(digits<field.size()){
        const auto c=std::to_integer<unsigned char>(field[digits]);if(c<'0'||c>'7')break;
        if(value>(UINT32_MAX-(c-'0'))/8)return false;value=value*8+(c-'0');++digits;
    }
    if(digits==0||digits==field.size())return false;
    for(std::size_t i=digits;i<field.size();++i)if(field[i]!=std::byte{}&&field[i]!=std::byte{' '})return false;
    return true;
}
/** @summary Decode a fixed ustar text field and require zero suffix after its first NUL.
 * @param bytes Exact field. @param text Borrowed result. @return False for hidden trailing bytes. */
bool Text(std::span<const std::byte> bytes,std::string_view& text) noexcept {
    std::size_t n=0;while(n<bytes.size()&&bytes[n]!=std::byte{})++n;
    if(!Zero(bytes.subspan(n)))return false;
    text={reinterpret_cast<const char*>(bytes.data()),n};return true;
}
/** @summary Read stable ordinary handle identity/length; reject links, directories and devices.
 * @param handle Retained file. @param info Exact identity. @return Ordinary disk file. */
bool Identity(HANDLE handle,BY_HANDLE_FILE_INFORMATION& info) noexcept {
    return GetFileType(handle)==FILE_TYPE_DISK&&GetFileInformationByHandle(handle,&info)&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_DEVICE))&&info.nNumberOfLinks==1;
}
/** @summary Compare retained identity and exact bounded length. @param a Before. @param b After. @return Same source. */
bool Same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) noexcept {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&
        a.nFileIndexLow==b.nFileIndexLow&&a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow;
}
} // namespace

std::shared_ptr<const Snapshot> Snapshot::Capture(const std::filesystem::path& path,const TrustedArtifact& trusted,Error& error) noexcept {
    error=Error::InvalidPin;if(!ValidateTrustedArtifact(trusted))return {};
    try {
        const auto& native=path.native();error=Error::InvalidPath;
        if(native.size()<4||native.size()>32760||native[1]!=L':'||native[2]!=L'\\'||
            !((native[0]>=L'A'&&native[0]<=L'Z')||(native[0]>=L'a'&&native[0]<=L'z')))return {};
        const std::array<wchar_t,4> drive{native[0],L':',L'\\',L'\0'};
        if(GetDriveTypeW(drive.data())!=DRIVE_FIXED)return {};
        static_assert(sizeof(Snapshot)+65530+65*sizeof(Handle)+2048<131072,
            "Index, maximum owned path, retained ancestry and parser control must remain finite");
        // Exact owned path allocation, no substring-sized temporary or caller device-prefix acceptance.
        auto extended=std::unique_ptr<wchar_t[]>(new wchar_t[native.size()+5]);
        std::memcpy(extended.get(),L"\\\\?\\",4*sizeof(wchar_t));
        std::memcpy(extended.get()+4,native.c_str(),(native.size()+1)*sizeof(wchar_t));
        std::array<Handle,65> parents;std::size_t parentCount=0,begin=3;
        for(std::size_t i=3;i<=native.size();++i){
            if(i<native.size()&&native[i]!=L'\\')continue;
            const auto part=std::wstring_view(native).substr(begin,i-begin);
            if(part.empty()||part==L"."||part==L".."||part.back()==L'.'||part.back()==L' ')return {};
            for(wchar_t c:part)if(c<L' '||c==L':'||c==L'/'||c==L'*'||c==L'?'||c==L'"'||c==L'<'||c==L'>'||c==L'|')return {};
            if(i==native.size())break;
            if(parentCount==parents.size())return {};
            const auto saved=extended[i+4];extended[i+4]=L'\0';
            auto& h=parents[parentCount++];h.value=CreateFileW(extended.get(),FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            extended[i+4]=saved;
            FILE_ATTRIBUTE_TAG_INFO tag{};
            if(h.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandleEx(h.value,FileAttributeTagInfo,&tag,sizeof(tag))||
                !(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){error=Error::OrdinaryFile;return {};}
            begin=i+1;
        }
        Handle source;source.value=CreateFileW(extended.get(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(source.value==INVALID_HANDLE_VALUE){error=Error::Open;return {};}
        BY_HANDLE_FILE_INFORMATION before{},after{};
        if(!Identity(source.value,before)){error=Error::OrdinaryFile;return {};}
        if(before.nFileSizeHigh||before.nFileSizeLow!=trusted.size){error=Error::Changed;return {};}
        error=Error::Allocation;
        auto result=std::shared_ptr<Snapshot>(new Snapshot);
        result->bytes_.reset(new std::byte[trusted.size]);result->size_=trusted.size;
        auto scratch=std::unique_ptr<std::byte[]>(new std::byte[65536]);
        std::uint32_t position=0;
        while(position<trusted.size){DWORD got=0;const DWORD want=static_cast<DWORD>((std::min)(std::uint32_t{65536},trusted.size-position));
            if(!ReadFile(source.value,result->bytes_.get()+position,want,&got,nullptr)||got!=want){error=Error::Capture;return {};}
            position+=got;
        }
        std::byte extra{};DWORD got=0;
        if(!ReadFile(source.value,&extra,1,&got,nullptr)||got){error=Error::Capture;return {};}
        if(!Identity(source.value,after)||!Same(before,after)){error=Error::Changed;return {};}
        core::Sha256Digest digest{};
        if(!core::Sha256(result->Bytes(),{scratch.get(),65536},digest)){error=Error::Hash;return {};}
        if(digest!=trusted.digest){error=Error::ArtifactDigest;return {};}
        error=result->Parse(trusted.files,{scratch.get(),65536});
        if(error!=Error::None)return {};
        return result;
    }catch(...){error=Error::Allocation;return {};}
}

Error Snapshot::Parse(std::span<const FilePin> pins,std::span<std::byte> scratch) noexcept {
    static_assert(sizeof(Record)*MaximumEntries+sizeof(std::array<bool,MaximumEntries>)+1024<131072);
    // No recursion; local arrays total less than 2KiB, excluding separately owned 64KiB CNG scratch.
    std::array<bool,MaximumEntries> found{};std::size_t offset=0;
    while(offset<size_){
        if(size_-offset<512)return Error::Header;
        const auto header=Bytes().subspan(offset,512);
        if(Zero(header)){
            if(size_-offset<1024||!Zero(Bytes().subspan(offset)))return Error::Header;
            return count_==pins.size()?Error::None:Error::Membership;
        }
        if(count_==MaximumEntries)return Error::Membership;
        if(std::memcmp(header.data()+257,"ustar\0" "00",8)!=0||header[156]!=std::byte{'0'}||
            !Zero(header.subspan(157,100))||!Zero(header.subspan(265,64))||!Zero(header.subspan(500,12)))return Error::Header;
        std::uint32_t checksum=0,size=0,mode=0,uid=0,gid=0,time=0,major=0,minor=0;
        if(!Octal(header.subspan(148,8),checksum)||!Octal(header.subspan(124,12),size)||
            !Octal(header.subspan(100,8),mode)||!Octal(header.subspan(108,8),uid)||!Octal(header.subspan(116,8),gid)||
            !Octal(header.subspan(136,12),time))return Error::Header;
        // tarfile emits zero device fields for regular files; accept octal zero or all-NUL only.
        if((!Zero(header.subspan(329,8))&&(!Octal(header.subspan(329,8),major)||major))||
            (!Zero(header.subspan(337,8))&&(!Octal(header.subspan(337,8),minor)||minor)))return Error::Header;
        if(mode!=0644||uid||gid||time)return Error::Header;
        std::uint32_t sum=0;for(std::size_t i=0;i<512;++i)sum+=i>=148&&i<156?32:std::to_integer<unsigned char>(header[i]);
        if(sum!=checksum)return Error::Header;
        std::string_view name,prefix;
        if(!Text(header.first(100),name)||!Text(header.subspan(345,155),prefix))return Error::Header;
        std::array<char,MaximumPath+1> joined{};
        if(name.empty()||prefix.size()+name.size()+(prefix.empty()?0:1)>MaximumPath)return Error::Header;
        std::size_t length=0;if(!prefix.empty()){std::memcpy(joined.data(),prefix.data(),prefix.size());length=prefix.size();joined[length++]='/';}
        std::memcpy(joined.data()+length,name.data(),name.size());length+=name.size();
        const std::string_view path(joined.data(),length);if(!NameValid(path))return Error::Header;
        std::size_t selected=pins.size();for(std::size_t i=0;i<pins.size();++i)if(pins[i].path==path){selected=i;break;}
        if(selected==pins.size()||found[selected]||size!=pins[selected].size)return Error::Membership;
        const auto dataOffset=offset+512;
        if(size>size_-dataOffset)return Error::Header;
        const std::size_t padded=(static_cast<std::size_t>(size)+511)&~std::size_t{511};
        if(padded>size_-dataOffset||!Zero(Bytes().subspan(dataOffset+size,padded-size)))return Error::Header;
        core::Sha256Digest digest{};if(!core::Sha256(Bytes().subspan(dataOffset,size),scratch,digest))return Error::Hash;
        if(digest!=pins[selected].digest)return Error::Content;
        auto& record=records_[count_++];record.path=joined;record.offset=static_cast<std::uint32_t>(dataOffset);record.size=size;record.notice=pins[selected].notice;
        found[selected]=true;offset=dataOffset+padded;
    }
    return Error::Header;
}
std::string_view Snapshot::Name(std::size_t index) const noexcept {return index<count_?std::string_view(records_[index].path.data()):std::string_view{};}
std::span<const std::byte> Snapshot::File(std::size_t index) const noexcept {return index<count_?Bytes().subspan(records_[index].offset,records_[index].size):std::span<const std::byte>{};}
bool Snapshot::Notice(std::size_t index) const noexcept {return index<count_&&records_[index].notice;}
} // namespace sc13::platform::base_artifact
