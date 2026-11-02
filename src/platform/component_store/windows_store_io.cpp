/** @file @summary Explicit instance-bound Windows adapter; historical evidence is data and never native handle authority. */
#include "platform/component_store/windows_store_io.hpp"
#include <algorithm>
#include <charconv>

namespace sc13::platform::component_store {
namespace {
/** @summary Refuse unsupported owner mixing before any underlying IO. */
Result Invalid()noexcept{return {Status::Invalid,Effect::None,Durability::None,0};}
/** @summary Encode opaque observations without interpreting their bytes as handles. */
template<class T>std::string Hex(const T& bytes){constexpr char digits[]="0123456789abcdef";std::string value;value.reserve(bytes.size()*2);for(auto b:bytes){const auto n=static_cast<unsigned>(b);value+=digits[n>>4];value+=digits[n&15];}return value;}
/** @summary Produce canonical finite decimal counters. */
std::string Decimal(std::uint64_t n){char b[20];const auto r=std::to_chars(b,b+20,n);return {b,static_cast<std::size_t>(r.ptr-b)};}
/** @summary Only successful observed effects have local evidence in this transaction profile. */
std::string_view EffectName(metadata::Observation effect)noexcept{switch(effect){case metadata::Observation::Created:return "created";case metadata::Observation::Unchanged:return "unchanged";case metadata::Observation::Removed:return "removed";default:return {};}}
/** @summary Validate a bounded canonical opaque historical envelope; no OS object is opened or reconstructed. */
bool Historical(std::string_view text,const EvidenceContext& context){
    if(text.size()>4096||context.sequence==0||context.sequence>metadata::Limits::JournalRecords||EffectName(context.observation).empty())return false;
    std::size_t at{};const auto take=[&](std::string_view v){if(text.substr(at,v.size())!=v)return false;at+=v.size();return true;};
    if(!take("{\"backend\":\""))return false;const auto backend=at;while(at<text.size()&&text[at]!='"'){const auto c=text[at++];if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'))return false;}
    if(at==backend||at-backend>64||!take("\",\"bytes\":\""))return false;
    const auto begin=at;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;const auto number=text.substr(begin,at-begin);std::uint64_t bytes{};const auto parsed=std::from_chars(number.data(),number.data()+number.size(),bytes);
    if(number.empty()||(number.size()>1&&number.front()=='0')||parsed.ec!=std::errc{}||parsed.ptr!=number.data()+number.size()||bytes>metadata::Limits::NativeImageBytes||!take("\",\"directory\":"))return false;
    if(!take("true")&&!take("false"))return false;if(!take(",\"identity\":\""))return false;const auto identity=at;bool nonzero{};
    while(at<text.size()&&text[at]!='"'){const auto c=text[at++];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;nonzero=nonzero||c!='0';}
    if(!nonzero||at==identity||at-identity>96||(at-identity)%2||!take("\",\"installation\":\"")||!take(Hex(context.installation.bytes))||!take("\",\"observation\":\"")||!take(EffectName(context.observation))||
       !take("\",\"schema\":\"simoder.store.local-evidence@1\",\"sequence\":\"")||!take(Decimal(context.sequence))||!take("\",\"transaction\":\"")||!take(Hex(context.transaction.bytes))||!take("\"}"))return false;return at==text.size();
}
}
/** @summary Return actual backend provenance description as diagnostic data. */
std::string_view WindowsStoreIO::BackendDescription()const noexcept{return "windows-file-id-v1";}
/** @summary Name the qualified native guarantee without promising universal power-loss persistence. */
std::string_view WindowsStoreIO::PublicationDescription()const noexcept{return "NTFS same-handle write-through rename OS acknowledgement";}
/** @summary Interpret only the actual qualified result from this native implementation. */
bool WindowsStoreIO::PublicationAcknowledged(const Result& r,PublicationRequirement p)const noexcept{return p==PublicationRequirement::DurableNamespace&&r.status==Status::Ok&&r.effect==Effect::Published&&r.durability==Durability::NamespaceAcknowledged;}
/** @summary Capture fresh local platform facts; serialization does not mint installation consent. */
Result WindowsStoreIO::CaptureAttachment(Parent& parent,const metadata::Identifier& installation,metadata::Attachment& output)noexcept{try{
    if(!Owns(parent))return Invalid();LocalAttachment local;const auto r=WindowsStore::ObserveAttachment(parent,local);if(r.status!=Status::Ok)return r;
    if(!local.root.length||local.root.length>local.root.bytes.size())return Invalid();metadata::Attachment value;value.installation=installation;value.backend=metadata::Backend::Windows;value.root=local.normalizedRootUtf8;std::replace(value.root.begin(),value.root.end(),'\\','/');value.account=Hex(local.account);value.volumeIdentity=local.volumeIdentity;value.objectIdentity=Hex(std::span(local.root.bytes).first(local.root.length));metadata::Error error;if(!metadata::Validate(value,error))return Invalid();output=std::move(value);return r;
}catch(...){return Invalid();}}
/** @summary Bind actual observation bytes to exact logical transaction context, as historical data only. */
Result WindowsStoreIO::EncodeLocalEvidence(const EvidenceContext& c,const Observation& o,std::string& output)const noexcept{try{
    if(!o.identity.length||o.identity.length>o.identity.bytes.size())return Invalid();std::string text="{\"backend\":\"windows-file-id-v1\",\"bytes\":\""+Decimal(o.bytes)+"\",\"directory\":"+(o.directory?"true":"false")+",\"identity\":\""+Hex(std::span(o.identity.bytes).first(o.identity.length))+"\",\"installation\":\""+Hex(c.installation.bytes)+"\",\"observation\":\""+std::string(EffectName(c.observation))+"\",\"schema\":\"simoder.store.local-evidence@1\",\"sequence\":\""+Decimal(c.sequence)+"\",\"transaction\":\""+Hex(c.transaction.bytes)+"\"}";
    if(!Historical(text,c))return Invalid();output=std::move(text);return {Status::Ok,Effect::None,Durability::None,0};
}catch(...){return Invalid();}}
/** @summary Validate old opaque records without converting old platform identities into current authority. */
bool WindowsStoreIO::ValidateHistoricalEvidence(std::string_view text,const EvidenceContext& c)const noexcept{try{return Historical(text,c);}catch(...){return false;}}

/** @summary Bind only actual acquired owners from qualified same-instance calls. */
Result WindowsStoreIO::OpenParent(std::string_view path,Parent& out,Observation& observed)noexcept{if(!Empty(out))return Invalid();auto r=WindowsStore::OpenParent(path,out,observed);Bind(out);return r;}
/** @summary Retain original same-instance lock custody. */
Result WindowsStoreIO::Lock(Store& s,Coordination& out)noexcept{if(!Owns(s)||!Empty(out))return Invalid();auto r=WindowsStore::Lock(s,out);Bind(out);return r;}
/** @summary Clear provenance only after the native original lock was positively closed. */
Result WindowsStoreIO::ReleaseCoordination(Coordination& c)noexcept{if(Empty(c))return {Status::Ok,Effect::None,Durability::None,0};if(!Owns(c))return Invalid();auto result=WindowsStore::ReleaseCoordination(c);if(result.status==Status::Ok&&!c.state_)c.origin_=nullptr;return result;}
/** @summary Bind partial acquisitions even when the native effect outcome is uncertain. */
Result WindowsStoreIO::CreateStore(Parent& p,const Observation& expected,Store& s,Coordination& c,Observation& out)noexcept{if(!Owns(p)||!Empty(s)||!Empty(c))return Invalid();auto r=WindowsStore::CreateStore(p,expected,s,c,out);Bind(s);Bind(c);return r;}
/** @summary Open storage without adopting a raw or foreign native owner. */
Result WindowsStoreIO::OpenStore(Parent& p,Store& s,Observation& o)noexcept{if(!Owns(p)||!Empty(s))return Invalid();auto r=WindowsStore::OpenStore(p,s,o);Bind(s);return r;}
/** @summary Enumerate only through same-instance original store and coordination. */
Result WindowsStoreIO::ListDirectory(Store& s,Coordination& c,InventoryTarget t,std::uint32_t n,std::vector<NamedObservation>& out)noexcept{if(!Owns(s)||!Owns(c))return Invalid();return WindowsStore::ListDirectory(s,c,t,n,out);}
/** @summary Create one derived directory under original custody. */
Result WindowsStoreIO::CreateDirectory(Store& s,Coordination& c,std::string_view p,Observation& o)noexcept{if(!Owns(s)||!Owns(c))return Invalid();return WindowsStore::CreateDirectory(s,c,p,o);}
/** @summary Bind the original bounded read handle, including uncertain acquisition. */
Result WindowsStoreIO::OpenRead(Store& s,Coordination& c,std::string_view p,std::uint64_t n,File& f,Observation& o)noexcept{if(!Owns(s)||!Owns(c)||!Empty(f))return Invalid();auto r=WindowsStore::OpenRead(s,c,p,n,f,o);Bind(f);return r;}
/** @summary Refuse reads through foreign owners without returning bytes. */
Result WindowsStoreIO::Read(File& f,std::span<std::byte> b,std::size_t& n)noexcept{n=0;if(!Owns(f))return Invalid();return WindowsStore::Read(f,b,n);}
/** @summary Bind one actual exclusively created original file. */
Result WindowsStoreIO::CreateFile(Store& s,Coordination& c,std::string_view p,std::uint64_t n,File& f,Observation& o)noexcept{if(!Owns(s)||!Owns(c)||!Empty(f))return Invalid();auto r=WindowsStore::CreateFile(s,c,p,n,f,o);Bind(f);return r;}
/** @summary Write only a same-instance original. */
Result WindowsStoreIO::Write(File& f,std::span<const std::byte> b)noexcept{if(!Owns(f))return Invalid();return WindowsStore::Write(f,b);}
/** @summary Flush only a same-instance original. */
Result WindowsStoreIO::Flush(File& f)noexcept{if(!Owns(f))return Invalid();return WindowsStore::Flush(f);}
/** @summary Clear provenance only after positive actual close. */
Result WindowsStoreIO::Close(File& f)noexcept{if(!Owns(f))return Invalid();auto r=WindowsStore::Close(f);if(r.status==Status::Ok&&!f.state_)f.origin_=nullptr;return r;}
/** @summary Publish exact original record custody; no arbitrary overwrite route. */
Result WindowsStoreIO::PublishRecord(Store& s,Coordination& c,File& f,RecordTarget t,PublicationRequirement p)noexcept{if(!Owns(s)||!Owns(c)||!Owns(f))return Invalid();return WindowsStore::PublishRecord(s,c,f,t,p);}
/** @summary Preserve qualified same-handle generation publication semantics. */
Result WindowsStoreIO::PublishGeneration(Store& s,Coordination& c,std::string_view a,const Observation& o,std::string_view b,PublicationRequirement p)noexcept{if(!Owns(s)||!Owns(c))return Invalid();return WindowsStore::PublishGeneration(s,c,a,o,b,p);}
/** @summary Preserve original conditional selector publication semantics. */
Result WindowsStoreIO::PublishSelection(Store& s,Coordination& c,File& f,const Observation* o,std::span<const std::byte> b,PublicationRequirement p)noexcept{if(!Owns(s)||!Owns(c)||!Owns(f))return Invalid();return WindowsStore::PublishSelection(s,c,f,o,b,p);}
/** @summary Remove only the backend-qualified originally created empty object. */
Result WindowsStoreIO::RemoveCreatedDirectory(Store& s,Coordination& c,std::string_view p,const Observation& o)noexcept{if(!Owns(s)||!Owns(c))return Invalid();return WindowsStore::RemoveCreatedDirectory(s,c,p,o);}
}
