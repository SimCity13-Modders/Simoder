/** @file @summary Strict schema-directed portable metadata codec; no filesystem, credential, native permission or activation operation. */
#include "platform/component_store/metadata.hpp"
#include "core/sha256.hpp"
#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <new>
#include <tuple>
#include <utility>

namespace sc13::platform::component_store::metadata {
namespace {
/** @summary Carry a nonsecret first refusal without allocating diagnostic strings. */
struct Refusal final {Code code;};
/** @summary Refuse a complete metadata value; no caller output has been published. */
void Need(bool condition,Code code=Code::Value){if(!condition)throw Refusal{code};}
/** @summary Validate one scalar and advance only within the caller's actual UTF-8 bytes. */
bool Utf8(std::string_view text,std::size_t& at,std::uint32_t& scalar)noexcept{
    if(at==text.size())return false;const auto first=static_cast<unsigned char>(text[at++]);
    if(first<0x80){scalar=first;return true;}unsigned count{};std::uint32_t minimum{};
    if(first>=0xc2&&first<=0xdf){count=1;scalar=first&31U;minimum=0x80;}
    else if(first>=0xe0&&first<=0xef){count=2;scalar=first&15U;minimum=0x800;}
    else if(first>=0xf0&&first<=0xf4){count=3;scalar=first&7U;minimum=0x10000;}else return false;
    if(text.size()-at<count)return false;for(unsigned i=0;i<count;++i){const auto byte=static_cast<unsigned char>(text[at++]);
        if((byte&0xc0U)!=0x80U)return false;scalar=(scalar<<6)|(byte&63U);}
    return scalar>=minimum&&scalar<=0x10ffff&&!(scalar>=0xd800&&scalar<=0xdfff);
}
/** @summary Check bounded textual data without interpreting it as a path, identity or authority. */
bool Text(std::string_view text,std::size_t maximum,bool empty=false)noexcept{
    if(text.size()>maximum||(!empty&&text.empty()))return false;std::size_t at{};std::uint32_t scalar{};
    while(at<text.size())if(!Utf8(text,at,scalar)||scalar<32||scalar==127)return false;return true;
}
/** @summary Compare ASCII case folds only; the portable path profile rejects non-ASCII bytes. */
char Fold(char value)noexcept{return value>='A'&&value<='Z'?static_cast<char>(value-'A'+'a'):value;}
/** @summary Compare complete names without changing the original spelling. */
bool EqualFold(std::string_view a,std::string_view b)noexcept{
    if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(Fold(a[i])!=Fold(b[i]))return false;return true;
}
/** @summary Nonzero byte strings are observations, never a trust test. */
template<class T>bool Nonzero(const T& value)noexcept{for(auto byte:value.bytes)if(byte)return true;return false;}
/** @summary Parse one lowercase hexadecimal nibble; canonical encoding admits no uppercase aliases. */
bool Hex(char byte,unsigned& value)noexcept{
    if(byte>='0'&&byte<='9'){value=static_cast<unsigned>(byte-'0');return true;}
    if(byte>='a'&&byte<='f'){value=static_cast<unsigned>(byte-'a'+10);return true;}return false;
}
/** @summary Parse fixed logical data atomically. */
template<class T>bool HexValue(std::string_view text,T& output)noexcept{
    if(text.size()!=output.bytes.size()*2)return false;T candidate;
    for(std::size_t i=0;i<candidate.bytes.size();++i){unsigned high{},low{};
        if(!Hex(text[i*2],high)||!Hex(text[i*2+1],low))return false;candidate.bytes[i]=static_cast<std::uint8_t>(high*16+low);}
    if(!Nonzero(candidate))return false;output=candidate;return true;
}
/** @summary Encode exact digest/identifier data without allocation beyond its finite representation. */
template<class T>std::string HexText(const T& value){
    constexpr std::string_view alphabet="0123456789abcdef";std::string result;result.resize(value.bytes.size()*2);
    for(std::size_t i=0;i<value.bytes.size();++i){result[i*2]=alphabet[value.bytes[i]>>4];result[i*2+1]=alphabet[value.bytes[i]&15];}return result;
}
/** @summary Read only one known schema at a time, charging depth/array entries before allocation. */
class Reader final {
public:
    /** @summary Capture caller-owned bytes; no view escapes the synchronous decode. */
    Reader(std::string_view input,Error& error)noexcept:input_(input),error_(error){}
    /** @summary Preserve the first error and original input position. */
    bool Fail(Code code)noexcept{if(error_.code==Code::Ok)error_={code,at_};return false;}
    /** @summary Consume one structural byte with JSON whitespace only. */
    bool Eat(char value)noexcept{Space();if(at_==input_.size()||input_[at_]!=value)return Fail(Code::Syntax);++at_;return true;}
    /** @summary Observe one structural byte without consuming another token. */
    bool Peek(char value)noexcept{Space();return at_<input_.size()&&input_[at_]==value;}
    /** @summary Require a complete document, not a valid prefix. */
    bool End()noexcept{Space();return at_==input_.size()||Fail(Code::Syntax);}
    /** @summary Consume exact null only when present. */
    bool Null()noexcept{Space();if(input_.substr(at_,4)!="null")return false;at_+=4;return true;}
    /** @summary Read a bounded scalar string, including strict UTF-16 escape pairing and decoded-key equality. */
    bool String(std::string& output,std::size_t maximum){
        if(!Eat('"'))return false;output.clear();
        while(at_<input_.size()){
            const auto byte=static_cast<unsigned char>(input_[at_++]);if(byte=='"')return true;
            if(byte=='\\'){
                if(at_==input_.size())return Fail(Code::Syntax);const char escape=input_[at_++];std::uint32_t scalar{};
                switch(escape){case '"':scalar='"';break;case '\\':scalar='\\';break;case '/':scalar='/';break;
                case 'b':scalar=8;break;case 'f':scalar=12;break;case 'n':scalar=10;break;case 'r':scalar=13;break;case 't':scalar=9;break;
                case 'u':if(!Hex4(scalar))return false;if(scalar>=0xd800&&scalar<=0xdbff){
                    if(input_.size()-at_<2||input_[at_]!='\\'||input_[at_+1]!='u')return Fail(Code::Encoding);at_+=2;std::uint32_t low{};
                    if(!Hex4(low)||low<0xdc00||low>0xdfff)return Fail(Code::Encoding);scalar=0x10000+((scalar-0xd800)<<10)+(low-0xdc00);
                }else if(scalar>=0xdc00&&scalar<=0xdfff)return Fail(Code::Encoding);break;
                default:return Fail(Code::Syntax);}
                if(!scalar)return Fail(Code::Encoding);if(!AppendScalar(output,scalar,maximum))return false;
            }else{
                if(byte<32)return Fail(Code::Encoding);const auto start=at_-1;std::size_t end=start;std::uint32_t scalar{};
                if(!Utf8(input_,end,scalar)||!scalar)return Fail(Code::Encoding);
                if(end-start>maximum-output.size())return Fail(Code::Limit);output.append(input_.substr(start,end-start));at_=end;
            }
        }return Fail(Code::Syntax);
    }
    /** @summary Match the decoded schema marker; alternate/unknown schema versions are not downgraded. */
    bool Exact(std::string_view expected){std::string value;return String(value,128)&&(value==expected||Fail(Code::Value));}
    /** @summary Read exact uint32 JSON integer syntax without floating or locale conversion. */
    bool Number(std::uint32_t& output)noexcept{
        Space();if(at_==input_.size()||input_[at_]<'0'||input_[at_]>'9')return Fail(Code::Value);
        const auto start=at_;std::uint64_t value{};while(at_<input_.size()&&input_[at_]>='0'&&input_[at_]<='9'){
            const auto digit=static_cast<unsigned>(input_[at_++]-'0');if(value>(UINT32_MAX-digit)/10)return Fail(Code::Value);value=value*10+digit;}
        if(at_-start>1&&input_[start]=='0')return Fail(Code::Value);output=static_cast<std::uint32_t>(value);return true;
    }
    /** @summary Read full-precision canonical decimal-string uint64; zero is checked by the containing schema. */
    bool Counter(std::uint64_t& output){
        std::string text;if(!String(text,20)||text.empty()||(text.size()>1&&text[0]=='0'))return Fail(Code::Value);
        std::uint64_t value{};for(char byte:text){if(byte<'0'||byte>'9')return Fail(Code::Value);const auto digit=static_cast<unsigned>(byte-'0');
            if(value>(UINT64_MAX-digit)/10)return Fail(Code::Sequence);value=value*10+digit;}output=value;return true;
    }
    /** @summary Read a complete JSON boolean without string coercion. */
    bool Boolean(bool& output)noexcept{Space();if(input_.substr(at_,4)=="true"){at_+=4;output=true;return true;}
        if(input_.substr(at_,5)=="false"){at_+=5;output=false;return true;}return Fail(Code::Value);}
    /** @summary Decode one closed object, rejecting escaped duplicates, unknown keys and omitted fields. */
    template<class F>bool Object(std::initializer_list<std::string_view> names,F field){
        if(!Enter('{'))return false;std::uint64_t seen{};
        if(!Peek('}'))for(;;){std::string key;if(!String(key,64)||!Eat(':'))return false;std::size_t index{};
            for(auto name:names){if(name==key)break;++index;}if(index==names.size())return Fail(Code::UnknownField);
            const auto bit=std::uint64_t{1}<<index;if(seen&bit)return Fail(Code::DuplicateField);seen|=bit;
            if(!field(index))return false;if(Peek('}'))break;if(!Eat(','))return false;
        }
        if(!Eat('}'))return false;--depth_;return seen==((std::uint64_t{1}<<names.size())-1)||Fail(Code::MissingField);
    }
    /** @summary Bound both per-list and aggregate typed allocations before constructing the next element. */
    template<class T,class F>bool Array(std::vector<T>& output,std::size_t maximum,F element){
        if(!Enter('['))return false;if(!Peek(']'))for(;;){
            if(output.size()==maximum||entries_==Limits::AggregateArrayEntries)return Fail(Code::Limit);++entries_;T next;
            if(!element(next))return false;output.push_back(std::move(next));if(Peek(']'))break;if(!Eat(','))return false;
        }if(!Eat(']'))return false;--depth_;return true;
    }
private:
    /** @summary Skip only the four standard JSON whitespace bytes. */
    void Space()noexcept{while(at_<input_.size()&&(input_[at_]==' '||input_[at_]=='\t'||input_[at_]=='\r'||input_[at_]=='\n'))++at_;}
    /** @summary Refuse deeper nesting before consuming a container. */
    bool Enter(char value)noexcept{if(depth_==Limits::Depth)return Fail(Code::Depth);if(!Eat(value))return false;++depth_;return true;}
    /** @summary Decode four hexadecimal escape digits; JSON escapes may use uppercase hex. */
    bool Hex4(std::uint32_t& output)noexcept{
        if(input_.size()-at_<4)return Fail(Code::Encoding);output=0;for(unsigned i=0;i<4;++i){unsigned digit{};char byte=input_[at_++];
            if(byte>='A'&&byte<='F')byte=static_cast<char>(byte-'A'+'a');if(!Hex(byte,digit))return Fail(Code::Encoding);output=output*16+digit;}return true;
    }
    /** @summary Append one Unicode scalar only after its encoded extent fits the actual field capacity. */
    bool AppendScalar(std::string& output,std::uint32_t scalar,std::size_t maximum){
        const auto count=scalar<0x80?1U:scalar<0x800?2U:scalar<0x10000?3U:4U;if(count>maximum-output.size())return Fail(Code::Limit);
        const auto put=[&](unsigned byte){output.push_back(static_cast<char>(byte));};
        if(count==1)put(scalar);else if(count==2){put(0xc0|(scalar>>6));put(0x80|(scalar&63));}
        else if(count==3){put(0xe0|(scalar>>12));put(0x80|((scalar>>6)&63));put(0x80|(scalar&63));}
        else{put(0xf0|(scalar>>18));put(0x80|((scalar>>12)&63));put(0x80|((scalar>>6)&63));put(0x80|(scalar&63));}return true;
    }
    std::string_view input_;Error& error_;std::size_t at_{},depth_{},entries_{};
};

/** @summary Bounded canonical writer; every encoded object below supplies keys in strict ASCII order. */
class Writer final {
public:
    /** @summary Fix maximum complete document bytes before allocating output. */
    explicit Writer(std::size_t maximum):maximum_(maximum){}
    /** @summary Append one exact source fragment after checked size arithmetic. */
    void Raw(std::string_view value){Need(value.size()<=maximum_-bytes_.size(),Code::Limit);bytes_.append(value);}
    /** @summary Encode a scalar string deterministically, without escaping valid non-ASCII UTF-8. */
    void String(std::string_view value){Raw("\"");for(unsigned char byte:value){
        if(byte=='"')Raw("\\\"");else if(byte=='\\')Raw("\\\\");else if(byte<32){constexpr char hex[]="0123456789abcdef";
            char escape[]{'\\','u','0','0',hex[byte>>4],hex[byte&15]};Raw({escape,sizeof(escape)});}else{const char c=static_cast<char>(byte);Raw({&c,1});}}
        Raw("\"");}
    /** @summary Full uint64 strings remain exact in every JSON consumer. */
    void Counter(std::uint64_t value){char buffer[20];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);Need(result.ec==std::errc{},Code::Value);String({buffer,static_cast<std::size_t>(result.ptr-buffer)});}
    /** @summary Small numeric byte/count values are emitted without exponent or leading zeros. */
    void Number(std::uint64_t value){char buffer[20];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);Need(result.ec==std::errc{},Code::Value);Raw({buffer,static_cast<std::size_t>(result.ptr-buffer)});}
    /** @summary Emit an exact decoded field name; canonical order is supplied by its closed schema. */
    void Key(std::string_view name,bool first=false){if(!first)Raw(",");String(name);Raw(":");}
    /** @summary Transfer only the complete original writer-owned result. */
    std::string Take(){return std::move(bytes_);}
private:std::size_t maximum_;std::string bytes_;
};

/** @summary Read one closed enum without display-name coercion. */
template<class E>bool Enum(Reader& reader,E& output,std::initializer_list<std::pair<std::string_view,E>> values){
    std::string text;if(!reader.String(text,64))return false;for(const auto& entry:values)if(entry.first==text){output=entry.second;return true;}return reader.Fail(Code::Value);
}
/** @summary Emit only a known enum; invalid cast values refuse instead of selecting a default. */
template<class E>std::string_view Name(E value,std::initializer_list<std::pair<std::string_view,E>> values){for(const auto& entry:values)if(entry.second==value)return entry.first;throw Refusal{Code::Value};}
#define COMPONENT_VALUES {{"simoder.base",ComponentId::Base},{"simoder.devtools.mcp",ComponentId::Mcp}}
#define ROLE_VALUES {{"manifest",FileRole::Manifest},{"script",FileRole::Script},{"native-image",FileRole::NativeImage},{"notice",FileRole::Notice}}
#define TRUST_VALUES {{"private-pin",TrustMode::PrivatePin},{"signed-publisher",TrustMode::SignedPublisher}}
#define PHASE_VALUES {{"accepted",Phase::Accepted},{"preparing",Phase::Preparing}}
#define BACKEND_VALUES {{"windows",Backend::Windows},{"posix",Backend::Posix}}
#define RESIDUAL_VALUES {{"edited",ResidualKind::Edited},{"foreign",ResidualKind::Foreign},{"unreadable",ResidualKind::Unreadable},{"unknown",ResidualKind::Unknown}}
#define ACTION_VALUES {{"create-directory",ActionKind::CreateDirectory},{"stage-file",ActionKind::StageFile},{"publish-generation",ActionKind::PublishGeneration},{"publish-selection",ActionKind::PublishSelection},{"remove-file",ActionKind::RemoveFile},{"remove-directory",ActionKind::RemoveDirectory}}
#define JOURNAL_VALUES {{"prepared",JournalKind::Prepared},{"intent",JournalKind::Intent},{"effect",JournalKind::Effect},{"completed",JournalKind::Completed},{"partial-protected",JournalKind::PartialProtected},{"unknown",JournalKind::Unknown}}
#define OBSERVATION_VALUES {{"none",Observation::None},{"created",Observation::Created},{"unchanged",Observation::Unchanged},{"removed",Observation::Removed},{"protected",Observation::Protected},{"unknown",Observation::Unknown}}

/** @summary Read exact fixed nonzero digest/identifier strings. */
bool Read(Reader& r,Digest& v){std::string text;return r.String(text,64)&&(ParseDigest(text,v)||r.Fail(Code::Value));}
bool Read(Reader& r,Identifier& v){std::string text;return r.String(text,32)&&(ParseIdentifier(text,v)||r.Fail(Code::Value));}
bool Read(Reader& r,ComponentId& v){return Enum(r,v,COMPONENT_VALUES);}
/** @summary Closed typed readers are declared before nullable templates; there is no dynamic parser registration. */
bool Read(Reader&,FileEntry&);bool Read(Reader&,Trust&);bool Read(Reader&,Compatibility&);bool Read(Reader&,Component&);
bool Read(Reader&,JournalReference&);bool Read(Reader&,PendingTransaction&);bool Read(Reader&,PendingCleanup&);
bool Read(Reader&,Residual&);bool Read(Reader&,ResidualReceipt&);bool Read(Reader&,GenerationKey&);bool Read(Reader&,RetainedGeneration&);
bool Read(Reader&,State&);bool Read(Reader&,Selector&);bool Read(Reader&,Attachment&);bool Read(Reader&,PlanAction&);
bool Read(Reader&,TransactionPlan&);bool Read(Reader&,JournalRecord&);
/** @summary Nullable fields are required keys whose null value never means guessed absence of an OS object. */
template<class T,class F>bool Optional(Reader& r,std::optional<T>& v,F read){if(r.Null()){v.reset();return true;}T next;if(!read(next))return false;v=std::move(next);return true;}
/** @summary Read a nullable typed record without aliasing another object's output. */
template<class T>bool Optional(Reader& r,std::optional<T>& v){return Optional(r,v,[&](T& next){return Read(r,next);});}
/** @summary Encode nullable data with an explicit null key value. */
template<class T,class F>void Optional(Writer& w,const std::optional<T>& v,F write){if(v)write(*v);else w.Raw("null");}
/** @summary Fixed data atoms always use canonical lowercase hexadecimal text. */
void Write(Writer& w,const Digest& v){w.String(HexText(v));}
void Write(Writer& w,const Identifier& v){w.String(HexText(v));}
void Write(Writer& w,ComponentId v){w.String(Name(v,COMPONENT_VALUES));}
/** @summary Render semantic sets deterministically without changing caller-owned order. */
template<class T,class F,class Less>void Array(Writer& w,const std::vector<T>& v,F write,Less less){
    std::vector<const T*> ordered;ordered.reserve(v.size());for(const auto& item:v)ordered.push_back(&item);
    std::sort(ordered.begin(),ordered.end(),[&](const T* a,const T* b){return less(*a,*b);});w.Raw("[");bool first=true;
    for(const auto* item:ordered){if(!first)w.Raw(",");first=false;write(*item);}w.Raw("]");
}
/** @summary Preserve an effect plan's original sequence rather than sorting effects. */
template<class T,class F>void Sequence(Writer& w,const std::vector<T>& v,F write){w.Raw("[");bool first=true;for(const auto& item:v){if(!first)w.Raw(",");first=false;write(item);}w.Raw("]");}

/** @summary Reject invalid enum casts before comparisons can accidentally select a privileged profile. */
void CheckId(ComponentId id){static_cast<void>(Name(id,COMPONENT_VALUES));}
/** @summary Compare the exact logical generation; this cannot resolve a current physical native incarnation. */
GenerationKey Key(const PendingCleanup& v){return {v.component,v.package,v.generation};}
GenerationKey Key(const ResidualReceipt& v){return {v.component,v.package,v.generation};}
GenerationKey Key(const Component& v){return {v.id,v.package,v.generation};}
/** @summary Canonical set order is independent of JSON input order. */
bool KeyLess(const GenerationKey& a,const GenerationKey& b){return std::tie(a.component,a.generation,a.package.bytes)<std::tie(b.component,b.generation,b.package.bytes);}
/** @summary A local reference remains finite and nonzero; its existence/authenticity is a coordinator concern. */
void Check(const JournalReference& v){Need(Nonzero(v.transaction)&&Nonzero(v.record),Code::Reference);Need(v.sequence&&v.sequence<=Limits::JournalRecords,Code::Sequence);}
void Check(const PendingTransaction& v){Need(Nonzero(v.transaction)&&Nonzero(v.plan),Code::Reference);Check(v.journal);Need(v.transaction==v.journal.transaction,Code::Reference);}
void Check(const GenerationKey& v){CheckId(v.component);Need(Nonzero(v.package)&&v.generation,Code::Reference);}
void Check(const PendingCleanup& v){Check(Key(v));Need(Nonzero(v.plan),Code::Reference);Check(v.journal);}
/** @summary Validate protected residual shape without inventing a missing file digest or inferring absence. */
void Check(const Residual& v){Need(RelativePathValid(v.path),Code::Path);static_cast<void>(Name(v.kind,RESIDUAL_VALUES));
    Need(v.bytes.has_value()==v.sha256.has_value(),Code::Reference);if(v.sha256)Need(Nonzero(*v.sha256),Code::Reference);
    if(v.bytes)Need(*v.bytes<=Limits::NativeImageBytes,Code::Limit);
    if(v.kind==ResidualKind::Unreadable||v.kind==ResidualKind::Unknown)Need(!v.bytes,Code::Reference);
}
/** @summary Every inventory is complete, bounded and free of case/structural aliases. */
template<class T,class Path>void UniquePaths(const std::vector<T>& values,Path path){
    for(std::size_t i=0;i<values.size();++i)for(std::size_t j=0;j<i;++j){const auto a=path(values[i]),b=path(values[j]);
        Need(!EqualFold(a,b),Code::Collision);
        if(a.size()<b.size()&&b[a.size()]=='/')Need(!EqualFold(a,b.substr(0,a.size())),Code::Collision);
        if(b.size()<a.size()&&a[b.size()]=='/')Need(!EqualFold(b,a.substr(0,b.size())),Code::Collision);
    }
}
void Check(const ResidualReceipt& v){Check(Key(v));Need(Nonzero(v.plan),Code::Reference);Check(v.journal);
    Need(!v.residuals.empty()&&v.residuals.size()<=Limits::ResidualEntries,Code::Limit);for(const auto& item:v.residuals)Check(item);
    UniquePaths(v.residuals,[](const auto& x)->std::string_view{return x.path;});}
/** @summary Complete immutable package constraints remain component-specific. */
void Check(const Component& v){
    CheckId(v.id);Need(v.generation&&Nonzero(v.package),Code::Reference);Need(Text(v.version,128),Code::Value);
    Need(v.payloadRoot=="packages/"+HexText(v.package),Code::Path);Need(!v.files.empty()&&v.files.size()<=Limits::Files,Code::Limit);
    Need(v.compatibility.hostRoot&&v.compatibility.baseContract&&v.compatibility.catalogueContract&&Nonzero(v.compatibility.sdkBundle),Code::Reference);
    Need(Text(v.trust.source,128)&&Nonzero(v.trust.manifest),Code::Reference);static_cast<void>(Name(v.trust.mode,TRUST_VALUES));
    if(v.trust.mode==TrustMode::PrivatePin)Need(!v.trust.publisher&&!v.trust.signature,Code::Reference);
    else Need(v.trust.publisher&&Text(*v.trust.publisher,128)&&v.trust.signature&&Nonzero(*v.trust.signature),Code::Reference);
    std::size_t manifests{},scripts{},images{},notices{};std::uint64_t payload{},noticeBytes{},rawBytes{};bool entry{};
    for(const auto& file:v.files){Need(RelativePathValid(file.path),Code::Path);Need(Nonzero(file.sha256),Code::Reference);static_cast<void>(Name(file.role,ROLE_VALUES));
        rawBytes+=file.bytes;if(file.role==FileRole::Notice){++notices;noticeBytes+=file.bytes;
            Need(file.path=="LICENSE"||file.path=="NOTICE"||(file.path.starts_with("licenses/")&&file.path.ends_with(".txt")),Code::Path);}
        else if(file.role==FileRole::Manifest){++manifests;Need(file.path=="mod.toml",Code::Path);payload+=file.bytes;}
        else if(file.role==FileRole::Script){++scripts;Need(file.path.starts_with("scripts/")&&file.path.ends_with(".luau"),Code::Path);payload+=file.bytes;
            for(char byte:file.path)Need((byte>='a'&&byte<='z')||(byte>='0'&&byte<='9')||byte=='/'||byte=='.'||byte=='-'||byte=='_',Code::Path);entry=entry||file.path=="scripts/init.luau";}
        else{++images;Need(file.path==v.nativeEntry&&file.bytes&&file.bytes<=Limits::NativeImageBytes,Code::Limit);}
    }
    UniquePaths(v.files,[](const auto& x)->std::string_view{return x.path;});Need(manifests==1&&notices&&noticeBytes<=Limits::NoticeBytes,Code::Limit);
    Need(v.archiveBytes>=1024&&v.archiveBytes%512==0&&rawBytes<=v.archiveBytes,Code::Limit);
    if(v.id==ComponentId::Base){Need(v.nativeEntry.empty()&&!v.requiresBase,Code::Reference);Need(!images&&scripts&&entry&&notices==1,Code::Value);
        const auto found=std::find_if(v.files.begin(),v.files.end(),[](const auto& x){return x.role==FileRole::Notice;});Need(found->path=="LICENSE",Code::Path);
        Need(v.archiveBytes<=Limits::BaseArchiveBytes&&payload<=Limits::BasePayloadBytes,Code::Limit);}
    else {Need(v.requiresBase&&Nonzero(*v.requiresBase),Code::Reference);Need(RelativePathValid(v.nativeEntry)&&v.nativeEntry.ends_with(".dll"),Code::Path);
        Need(images==1&&!scripts&&payload<=Limits::NativeManifestBytes&&v.archiveBytes<=Limits::McpArchiveBytes,Code::Limit);}
}
/** @summary A selected MCP component requires exactly its selected Base and matching declared ABI tuple. */
void Components(const std::vector<Component>& values){
    Need(values.size()<=Limits::Components,Code::Limit);const Component* base{};const Component* mcp{};
    for(const auto& item:values){Check(item);auto*& slot=item.id==ComponentId::Base?base:mcp;Need(!slot,Code::Collision);slot=&item;}
    if(mcp){Need(base&&*mcp->requiresBase==base->package,Code::Reference);const auto& a=base->compatibility;const auto& b=mcp->compatibility;
        Need(a.hostRoot==b.hostRoot&&a.baseContract==b.baseContract&&a.catalogueContract==b.catalogueContract&&a.sdkBundle==b.sdkBundle,Code::Reference);}
}
/** @summary Exact original journal references match across carried obligations. */
bool Same(const JournalReference& a,const JournalReference& b){return a.transaction==b.transaction&&a.sequence==b.sequence&&a.record==b.record;}
bool Same(const PendingCleanup& a,const PendingCleanup& b){return Key(a)==Key(b)&&a.plan==b.plan&&Same(a.journal,b.journal);}
/** @summary Keep logical retained generations finite and distinct from the currently selected incarnation. */
void Retained(const std::vector<RetainedGeneration>& values,const std::vector<Component>& selected){
    Need(values.size()<=Limits::Components*16,Code::Limit);std::array<std::size_t,2> counts{};
    for(const auto& item:selected)++counts[item.id==ComponentId::Base?0:1];
    for(std::size_t i=0;i<values.size();++i){const auto& item=values[i];Check(item.key);Need(Nonzero(item.acceptedState),Code::Reference);
        Need(++counts[item.key.component==ComponentId::Base?0:1]<=16,Code::Limit);
        for(const auto& active:selected)Need(!(Key(active)==item.key),Code::Collision);
        for(std::size_t j=0;j<i;++j)Need(!(values[j].key==item.key),Code::Collision);}
}
/** @summary Obligations survive deselection and cannot silently turn protected residuals into removal success. */
void Obligations(const std::vector<PendingCleanup>& cleanup,const std::vector<ResidualReceipt>& receipts){
    Need(cleanup.size()<=Limits::CleanupReferences&&receipts.size()<=Limits::ResidualReceipts,Code::Limit);std::size_t entries{};
    for(std::size_t i=0;i<cleanup.size();++i){Check(cleanup[i]);for(std::size_t j=0;j<i;++j)Need(!(Key(cleanup[i])==Key(cleanup[j])),Code::Collision);}
    for(std::size_t i=0;i<receipts.size();++i){const auto& receipt=receipts[i];Check(receipt);entries+=receipt.residuals.size();Need(entries<=Limits::ResidualEntries,Code::Limit);
        for(std::size_t j=0;j<i;++j)Need(!(Key(receipts[j])==Key(receipt)),Code::Collision);
        const auto found=std::find_if(cleanup.begin(),cleanup.end(),[&](const auto& c){return Key(c)==Key(receipt);});
        if(receipt.preserveAcknowledged)Need(found==cleanup.end(),Code::Reference);
        else Need(found!=cleanup.end()&&found->plan==receipt.plan&&Same(found->journal,receipt.journal),Code::Reference);
    }
}
void Check(const State& v){
    Need(Nonzero(v.installation)&&v.sequence,Code::Reference);static_cast<void>(Name(v.phase,PHASE_VALUES));Components(v.components);Retained(v.retained,v.components);
    if(v.previousAccepted)Need(Nonzero(*v.previousAccepted),Code::Reference);Obligations(v.pendingCleanup,v.residualReceipts);
    if(v.phase==Phase::Preparing){Need(v.pendingTransaction&&!v.origin,Code::Phase);Check(*v.pendingTransaction);}
    else {Need(!v.pendingTransaction&&v.origin,Code::Phase);Check(*v.origin);}
    // @summary Physical package storage is shared by content digest, regardless of logical generation numbers.
    for(const auto& cleanup:v.pendingCleanup){for(const auto& active:v.components)Need(cleanup.package!=active.package,Code::Reference);
        for(const auto& retained:v.retained)Need(cleanup.package!=retained.key.package,Code::Reference);}
}
void Check(const Selector& v){Need(Nonzero(v.installation)&&v.sequence&&Nonzero(v.state),Code::Reference);}
/** @summary Attachment syntax is not a physical-root/account observation or a reattachment permission. */
void Check(const Attachment& v){
    Need(Nonzero(v.installation),Code::Reference);static_cast<void>(Name(v.backend,BACKEND_VALUES));
    Need(Text(v.root,4096)&&Text(v.account,256)&&Text(v.volumeIdentity,128)&&Text(v.objectIdentity,128),Code::Value);
    if(v.backend==Backend::Windows){Need(v.root.size()>=3&&((v.root[0]>='A'&&v.root[0]<='Z')||(v.root[0]>='a'&&v.root[0]<='z'))&&v.root[1]==':'&&v.root[2]=='/',Code::Path);
        Need(v.root.find('\\')==v.root.npos&&v.root.find(':',2)==v.root.npos,Code::Path);}
    else Need(v.root.starts_with('/')&&v.root.find('\\')==v.root.npos,Code::Path);
    const auto start=v.backend==Backend::Windows?3U:1U;std::size_t at=start;
    while(at<v.root.size()){const auto end=v.root.find('/',at);const auto part=v.root.substr(at,end==v.root.npos?v.root.size()-at:end-at);
        Need(!part.empty()&&part!="."&&part!="..",Code::Path);if(v.backend==Backend::Windows)Need(part.back()!='.'&&part.back()!=' ',Code::Path);
        if(end==v.root.npos)break;at=end+1;Need(at<v.root.size(),Code::Path);}
}
/** @summary Finite action paths are relative within a package; selector/generation targets are derived by the coordinator. */
void Check(const PlanAction& v){
    static_cast<void>(Name(v.kind,ACTION_VALUES));if(v.before)Need(Nonzero(*v.before),Code::Reference);if(v.after)Need(Nonzero(*v.after),Code::Reference);
    if(v.kind==ActionKind::PublishSelection){Need(!v.component&&!v.package&&v.path.empty()&&!v.after,Code::Reference);return;}
    Need(v.component&&v.package&&Nonzero(*v.package),Code::Reference);CheckId(*v.component);
    if(v.kind==ActionKind::PublishGeneration){Need(v.path.empty()&&!v.before&&v.after&&*v.after==*v.package,Code::Reference);return;}
    Need(RelativePathValid(v.path),Code::Path);
    if(v.kind==ActionKind::StageFile)Need(!v.before&&v.after,Code::Reference);
    else if(v.kind==ActionKind::RemoveFile)Need(v.before&&!v.after,Code::Reference);
    else Need(!v.before&&!v.after,Code::Reference);
}
void Check(const TransactionPlan& v){
    Need(Nonzero(v.installation)&&Nonzero(v.transaction),Code::Reference);Need((v.expectedSequence==0)==!v.expectedState,Code::Reference);
    if(v.expectedState)Need(Nonzero(*v.expectedState),Code::Reference);std::uint64_t preparing{},accepted{};
    Need(Advance(v.expectedSequence,preparing)&&Advance(preparing,accepted),Code::Sequence);
    Components(v.nextComponents);Retained(v.nextRetained,v.nextComponents);Obligations(v.carryCleanup,v.carryResidualReceipts);
    Need(!v.actions.empty()&&v.actions.size()<=Limits::PlanActions,Code::Limit);std::size_t publications{};
    for(std::size_t i=0;i<v.actions.size();++i){const auto& action=v.actions[i];Check(action);
        if(action.kind==ActionKind::PublishSelection){++publications;Need(action.before==v.expectedState,Code::Reference);continue;}
        for(std::size_t j=0;j<i;++j){const auto& old=v.actions[j];Need(!(old.kind==action.kind&&old.component==action.component&&old.package==action.package&&EqualFold(old.path,action.path)),Code::Collision);}
        if(action.kind==ActionKind::StageFile||action.kind==ActionKind::CreateDirectory||action.kind==ActionKind::PublishGeneration){
            const auto next=std::find_if(v.nextComponents.begin(),v.nextComponents.end(),[&](const auto& c){return c.id==*action.component&&c.package==*action.package;});
            Need(next!=v.nextComponents.end(),Code::Reference);
            if(action.kind==ActionKind::StageFile)Need(std::any_of(next->files.begin(),next->files.end(),[&](const auto& file){return file.path==action.path&&action.after&&file.sha256==*action.after;}),Code::Reference);
            else if(action.kind==ActionKind::CreateDirectory)Need(std::any_of(next->files.begin(),next->files.end(),[&](const auto& file){return file.path.size()>action.path.size()&&file.path.starts_with(action.path)&&file.path[action.path.size()]=='/';}),Code::Reference);
        }
    }
    Need(publications==1&&v.actions.back().kind==ActionKind::PublishSelection,Code::Phase);
    Need(v.newCleanup.size()<=Limits::CleanupReferences&&v.acknowledgeResiduals.size()<=Limits::ResidualReceipts,Code::Limit);
    for(std::size_t i=0;i<v.newCleanup.size();++i){const auto& key=v.newCleanup[i];Check(key);
        for(std::size_t j=0;j<i;++j)Need(!(key==v.newCleanup[j]),Code::Collision);
        for(const auto& c:v.carryCleanup)Need(!(Key(c)==key),Code::Collision);
        for(const auto& c:v.nextComponents)Need(c.package!=key.package,Code::Reference);
        for(const auto& retained:v.nextRetained)Need(retained.key.package!=key.package,Code::Reference);}
    for(std::size_t i=0;i<v.acknowledgeResiduals.size();++i){const auto& key=v.acknowledgeResiduals[i];Check(key);
        for(std::size_t j=0;j<i;++j)Need(!(key==v.acknowledgeResiduals[j]),Code::Collision);
        const auto found=std::find_if(v.carryResidualReceipts.begin(),v.carryResidualReceipts.end(),[&](const auto& x){return Key(x)==key;});
        Need(found!=v.carryResidualReceipts.end()&&!found->preserveAcknowledged,Code::Reference);}
    for(const auto& cleanup:v.carryCleanup){if(std::find(v.acknowledgeResiduals.begin(),v.acknowledgeResiduals.end(),Key(cleanup))!=v.acknowledgeResiduals.end())continue;
        for(const auto& c:v.nextComponents)Need(c.package!=cleanup.package,Code::Reference);
        for(const auto& retained:v.nextRetained)Need(retained.key.package!=cleanup.package,Code::Reference);}
}
void Check(const JournalRecord& v){
    Need(Nonzero(v.installation)&&Nonzero(v.transaction)&&Nonzero(v.plan),Code::Reference);
    Need(v.sequence&&v.sequence<=Limits::JournalRecords,Code::Sequence);Need((v.sequence==1)==!v.previous,Code::Reference);
    if(v.previous)Need(Nonzero(*v.previous),Code::Reference);if(v.backendEvidence)Need(Nonzero(*v.backendEvidence),Code::Reference);
    static_cast<void>(Name(v.kind,JOURNAL_VALUES));static_cast<void>(Name(v.observation,OBSERVATION_VALUES));
    if(v.kind==JournalKind::Prepared)Need(v.sequence==1&&!v.action&&v.observation==Observation::None&&!v.backendEvidence,Code::Phase);
    else {Need(v.sequence>1,Code::Phase);if(v.kind==JournalKind::Intent)Need(v.action&&v.observation==Observation::None&&!v.backendEvidence,Code::Phase);
        else if(v.kind==JournalKind::Effect)Need(v.action&&v.observation!=Observation::None&&v.backendEvidence,Code::Reference);
        else {Need(!v.action,Code::Phase);
            if(v.kind==JournalKind::Completed)Need(v.observation==Observation::None,Code::Phase);
            else if(v.kind==JournalKind::PartialProtected)Need(v.observation==Observation::Protected&&v.backendEvidence,Code::Reference);
            else Need(v.observation==Observation::Unknown,Code::Phase);}}
    if(v.action)Need(*v.action<Limits::PlanActions,Code::Limit);
}

/** @summary Decode every required field of each closed record; key order itself is insignificant. */
#define FIELD(n,expression) case n:return (expression)
bool Read(Reader& r,FileEntry& v){return r.Object({"bytes","path","role","sha256"},[&](std::size_t f){switch(f){
    FIELD(0,r.Number(v.bytes));FIELD(1,r.String(v.path,240));FIELD(2,Enum(r,v.role,ROLE_VALUES));FIELD(3,Read(r,v.sha256));}return false;});}
bool Read(Reader& r,Trust& v){return r.Object({"manifest","mode","publisher","signature","source"},[&](std::size_t f){switch(f){
    FIELD(0,Read(r,v.manifest));FIELD(1,Enum(r,v.mode,TRUST_VALUES));FIELD(2,Optional(r,v.publisher,[&](auto& x){return r.String(x,128);}));
    FIELD(3,Optional(r,v.signature));FIELD(4,r.String(v.source,128));}return false;});}
bool Read(Reader& r,Compatibility& v){return r.Object({"baseContract","catalogueContract","hostRoot","sdkBundle"},[&](std::size_t f){switch(f){
    FIELD(0,r.Number(v.baseContract));FIELD(1,r.Number(v.catalogueContract));FIELD(2,r.Number(v.hostRoot));FIELD(3,Read(r,v.sdkBundle));}return false;});}
bool Read(Reader& r,Component& v){return r.Object({"archiveBytes","compatibility","files","generation","id","nativeEntry","package","payloadRoot","requiresBase","trust","version"},[&](std::size_t f){switch(f){
    FIELD(0,r.Counter(v.archiveBytes));FIELD(1,Read(r,v.compatibility));FIELD(2,r.Array(v.files,65,[&](auto& x){return Read(r,x);}));
    FIELD(3,r.Counter(v.generation));FIELD(4,Read(r,v.id));FIELD(5,r.String(v.nativeEntry,240));FIELD(6,Read(r,v.package));
    FIELD(7,r.String(v.payloadRoot,240));FIELD(8,Optional(r,v.requiresBase));FIELD(9,Read(r,v.trust));FIELD(10,r.String(v.version,128));}return false;});}
bool Read(Reader& r,JournalReference& v){return r.Object({"record","sequence","transaction"},[&](std::size_t f){switch(f){
    FIELD(0,Read(r,v.record));FIELD(1,r.Counter(v.sequence));FIELD(2,Read(r,v.transaction));}return false;});}
bool Read(Reader& r,PendingTransaction& v){return r.Object({"journal","plan","transaction"},[&](std::size_t f){switch(f){
    FIELD(0,Read(r,v.journal));FIELD(1,Read(r,v.plan));FIELD(2,Read(r,v.transaction));}return false;});}
bool Read(Reader& r,PendingCleanup& v){return r.Object({"component","generation","journal","package","plan"},[&](std::size_t f){switch(f){
    FIELD(0,Read(r,v.component));FIELD(1,r.Counter(v.generation));FIELD(2,Read(r,v.journal));FIELD(3,Read(r,v.package));FIELD(4,Read(r,v.plan));}return false;});}
bool Read(Reader& r,Residual& v){return r.Object({"bytes","kind","path","sha256"},[&](std::size_t f){switch(f){
    FIELD(0,Optional(r,v.bytes,[&](auto& x){return r.Number(x);}));FIELD(1,Enum(r,v.kind,RESIDUAL_VALUES));
    FIELD(2,r.String(v.path,240));FIELD(3,Optional(r,v.sha256));}return false;});}
bool Read(Reader& r,ResidualReceipt& v){return r.Object({"component","generation","journal","package","plan","preserveAcknowledged","residuals"},[&](std::size_t f){switch(f){
    FIELD(0,Read(r,v.component));FIELD(1,r.Counter(v.generation));FIELD(2,Read(r,v.journal));FIELD(3,Read(r,v.package));FIELD(4,Read(r,v.plan));
    FIELD(5,r.Boolean(v.preserveAcknowledged));FIELD(6,r.Array(v.residuals,Limits::ResidualEntries,[&](auto& x){return Read(r,x);}));}return false;});}
bool Read(Reader& r,GenerationKey& v){return r.Object({"component","generation","package"},[&](std::size_t f){switch(f){
    FIELD(0,Read(r,v.component));FIELD(1,r.Counter(v.generation));FIELD(2,Read(r,v.package));}return false;});}
bool Read(Reader& r,RetainedGeneration& v){return r.Object({"acceptedState","key"},[&](std::size_t f){switch(f){
    FIELD(0,Read(r,v.acceptedState));FIELD(1,Read(r,v.key));}return false;});}
bool Read(Reader& r,State& v){return r.Object({"components","installation","origin","pendingCleanup","pendingTransaction","phase","previousAccepted","residualReceipts","retained","schema","sequence"},[&](std::size_t f){switch(f){
    FIELD(0,r.Array(v.components,Limits::Components,[&](auto& x){return Read(r,x);}));FIELD(1,Read(r,v.installation));FIELD(2,Optional(r,v.origin));
    FIELD(3,r.Array(v.pendingCleanup,Limits::CleanupReferences,[&](auto& x){return Read(r,x);}));FIELD(4,Optional(r,v.pendingTransaction));
    FIELD(5,Enum(r,v.phase,PHASE_VALUES));FIELD(6,Optional(r,v.previousAccepted));
    FIELD(7,r.Array(v.residualReceipts,Limits::ResidualReceipts,[&](auto& x){return Read(r,x);}));
    FIELD(8,r.Array(v.retained,Limits::Components*16,[&](auto& x){return Read(r,x);}));FIELD(9,r.Exact("simoder.store.state@1"));FIELD(10,r.Counter(v.sequence));}return false;});}
bool Read(Reader& r,Selector& v){return r.Object({"installation","schema","sequence","state"},[&](std::size_t f){switch(f){
    FIELD(0,Read(r,v.installation));FIELD(1,r.Exact("simoder.store.selector@1"));FIELD(2,r.Counter(v.sequence));FIELD(3,Read(r,v.state));}return false;});}
bool Read(Reader& r,Attachment& v){return r.Object({"account","backend","installation","objectIdentity","root","schema","volumeIdentity"},[&](std::size_t f){switch(f){
    FIELD(0,r.String(v.account,256));FIELD(1,Enum(r,v.backend,BACKEND_VALUES));FIELD(2,Read(r,v.installation));FIELD(3,r.String(v.objectIdentity,128));
    FIELD(4,r.String(v.root,4096));FIELD(5,r.Exact("simoder.store.attachment@1"));FIELD(6,r.String(v.volumeIdentity,128));}return false;});}
bool Read(Reader& r,PlanAction& v){return r.Object({"after","before","component","kind","package","path"},[&](std::size_t f){switch(f){
    FIELD(0,Optional(r,v.after));FIELD(1,Optional(r,v.before));FIELD(2,Optional(r,v.component));FIELD(3,Enum(r,v.kind,ACTION_VALUES));
    FIELD(4,Optional(r,v.package));FIELD(5,r.String(v.path,240));}return false;});}
bool Read(Reader& r,TransactionPlan& v){return r.Object({"acknowledgeResiduals","actions","carryCleanup","carryResidualReceipts","expectedSequence","expectedState","installation","newCleanup","nextComponents","nextRetained","schema","transaction"},[&](std::size_t f){switch(f){
    FIELD(0,r.Array(v.acknowledgeResiduals,Limits::ResidualReceipts,[&](auto& x){return Read(r,x);}));
    FIELD(1,r.Array(v.actions,Limits::PlanActions,[&](auto& x){return Read(r,x);}));
    FIELD(2,r.Array(v.carryCleanup,Limits::CleanupReferences,[&](auto& x){return Read(r,x);}));
    FIELD(3,r.Array(v.carryResidualReceipts,Limits::ResidualReceipts,[&](auto& x){return Read(r,x);}));
    FIELD(4,r.Counter(v.expectedSequence));FIELD(5,Optional(r,v.expectedState));FIELD(6,Read(r,v.installation));
    FIELD(7,r.Array(v.newCleanup,Limits::CleanupReferences,[&](auto& x){return Read(r,x);}));
    FIELD(8,r.Array(v.nextComponents,Limits::Components,[&](auto& x){return Read(r,x);}));
    FIELD(9,r.Array(v.nextRetained,Limits::Components*16,[&](auto& x){return Read(r,x);}));
    FIELD(10,r.Exact("simoder.store.plan@1"));FIELD(11,Read(r,v.transaction));}return false;});}
bool Read(Reader& r,JournalRecord& v){return r.Object({"action","backendEvidence","installation","kind","observation","plan","previous","schema","sequence","transaction"},[&](std::size_t f){switch(f){
    FIELD(0,Optional(r,v.action,[&](auto& x){return r.Number(x);}));FIELD(1,Optional(r,v.backendEvidence));FIELD(2,Read(r,v.installation));
    FIELD(3,Enum(r,v.kind,JOURNAL_VALUES));FIELD(4,Enum(r,v.observation,OBSERVATION_VALUES));FIELD(5,Read(r,v.plan));
    FIELD(6,Optional(r,v.previous));FIELD(7,r.Exact("simoder.store.journal-record@1"));FIELD(8,r.Counter(v.sequence));FIELD(9,Read(r,v.transaction));}return false;});}
#undef FIELD

/** @summary Typed canonical writers form a closed schema, not a user-extensible object serializer. */
void Write(Writer&,const FileEntry&);void Write(Writer&,const Trust&);void Write(Writer&,const Compatibility&);void Write(Writer&,const Component&);
void Write(Writer&,const JournalReference&);void Write(Writer&,const PendingTransaction&);void Write(Writer&,const PendingCleanup&);
void Write(Writer&,const Residual&);void Write(Writer&,const ResidualReceipt&);void Write(Writer&,const GenerationKey&);void Write(Writer&,const RetainedGeneration&);
void Write(Writer&,const State&);void Write(Writer&,const Selector&);void Write(Writer&,const Attachment&);void Write(Writer&,const PlanAction&);
void Write(Writer&,const TransactionPlan&);void Write(Writer&,const JournalRecord&);
/** @summary Emit nullable known types after their containing record has passed validation. */
template<class T>void Optional(Writer& w,const std::optional<T>& v){Optional(w,v,[&](const auto& item){Write(w,item);});}
void Write(Writer& w,const FileEntry& v){w.Raw("{");w.Key("bytes",true);w.Number(v.bytes);w.Key("path");w.String(v.path);w.Key("role");w.String(Name(v.role,ROLE_VALUES));w.Key("sha256");Write(w,v.sha256);w.Raw("}");}
void Write(Writer& w,const Trust& v){w.Raw("{");w.Key("manifest",true);Write(w,v.manifest);w.Key("mode");w.String(Name(v.mode,TRUST_VALUES));w.Key("publisher");Optional(w,v.publisher,[&](const auto& x){w.String(x);});w.Key("signature");Optional(w,v.signature);w.Key("source");w.String(v.source);w.Raw("}");}
void Write(Writer& w,const Compatibility& v){w.Raw("{");w.Key("baseContract",true);w.Number(v.baseContract);w.Key("catalogueContract");w.Number(v.catalogueContract);w.Key("hostRoot");w.Number(v.hostRoot);w.Key("sdkBundle");Write(w,v.sdkBundle);w.Raw("}");}
void Write(Writer& w,const Component& v){w.Raw("{");w.Key("archiveBytes",true);w.Counter(v.archiveBytes);w.Key("compatibility");Write(w,v.compatibility);w.Key("files");Array(w,v.files,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return a.path<b.path;});
    w.Key("generation");w.Counter(v.generation);w.Key("id");Write(w,v.id);w.Key("nativeEntry");w.String(v.nativeEntry);w.Key("package");Write(w,v.package);w.Key("payloadRoot");w.String(v.payloadRoot);w.Key("requiresBase");Optional(w,v.requiresBase);w.Key("trust");Write(w,v.trust);w.Key("version");w.String(v.version);w.Raw("}");}
void Write(Writer& w,const JournalReference& v){w.Raw("{");w.Key("record",true);Write(w,v.record);w.Key("sequence");w.Counter(v.sequence);w.Key("transaction");Write(w,v.transaction);w.Raw("}");}
void Write(Writer& w,const PendingTransaction& v){w.Raw("{");w.Key("journal",true);Write(w,v.journal);w.Key("plan");Write(w,v.plan);w.Key("transaction");Write(w,v.transaction);w.Raw("}");}
void Write(Writer& w,const PendingCleanup& v){w.Raw("{");w.Key("component",true);Write(w,v.component);w.Key("generation");w.Counter(v.generation);w.Key("journal");Write(w,v.journal);w.Key("package");Write(w,v.package);w.Key("plan");Write(w,v.plan);w.Raw("}");}
void Write(Writer& w,const Residual& v){w.Raw("{");w.Key("bytes",true);Optional(w,v.bytes,[&](auto x){w.Number(x);});w.Key("kind");w.String(Name(v.kind,RESIDUAL_VALUES));w.Key("path");w.String(v.path);w.Key("sha256");Optional(w,v.sha256);w.Raw("}");}
void Write(Writer& w,const ResidualReceipt& v){w.Raw("{");w.Key("component",true);Write(w,v.component);w.Key("generation");w.Counter(v.generation);w.Key("journal");Write(w,v.journal);w.Key("package");Write(w,v.package);w.Key("plan");Write(w,v.plan);w.Key("preserveAcknowledged");w.Raw(v.preserveAcknowledged?"true":"false");
    w.Key("residuals");Array(w,v.residuals,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return a.path<b.path;});w.Raw("}");}
void Write(Writer& w,const GenerationKey& v){w.Raw("{");w.Key("component",true);Write(w,v.component);w.Key("generation");w.Counter(v.generation);w.Key("package");Write(w,v.package);w.Raw("}");}
void Write(Writer& w,const RetainedGeneration& v){w.Raw("{");w.Key("acceptedState",true);Write(w,v.acceptedState);w.Key("key");Write(w,v.key);w.Raw("}");}
void Write(Writer& w,const State& v){w.Raw("{");w.Key("components",true);Array(w,v.components,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return a.id<b.id;});w.Key("installation");Write(w,v.installation);w.Key("origin");Optional(w,v.origin);
    w.Key("pendingCleanup");Array(w,v.pendingCleanup,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return KeyLess(Key(a),Key(b));});w.Key("pendingTransaction");Optional(w,v.pendingTransaction);w.Key("phase");w.String(Name(v.phase,PHASE_VALUES));w.Key("previousAccepted");Optional(w,v.previousAccepted);
    w.Key("residualReceipts");Array(w,v.residualReceipts,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return KeyLess(Key(a),Key(b));});w.Key("retained");Array(w,v.retained,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return KeyLess(a.key,b.key);});
    w.Key("schema");w.String("simoder.store.state@1");w.Key("sequence");w.Counter(v.sequence);w.Raw("}");}
void Write(Writer& w,const Selector& v){w.Raw("{");w.Key("installation",true);Write(w,v.installation);w.Key("schema");w.String("simoder.store.selector@1");w.Key("sequence");w.Counter(v.sequence);w.Key("state");Write(w,v.state);w.Raw("}");}
void Write(Writer& w,const Attachment& v){w.Raw("{");w.Key("account",true);w.String(v.account);w.Key("backend");w.String(Name(v.backend,BACKEND_VALUES));w.Key("installation");Write(w,v.installation);w.Key("objectIdentity");w.String(v.objectIdentity);w.Key("root");w.String(v.root);w.Key("schema");w.String("simoder.store.attachment@1");w.Key("volumeIdentity");w.String(v.volumeIdentity);w.Raw("}");}
void Write(Writer& w,const PlanAction& v){w.Raw("{");w.Key("after",true);Optional(w,v.after);w.Key("before");Optional(w,v.before);w.Key("component");Optional(w,v.component);w.Key("kind");w.String(Name(v.kind,ACTION_VALUES));w.Key("package");Optional(w,v.package);w.Key("path");w.String(v.path);w.Raw("}");}
void Write(Writer& w,const TransactionPlan& v){w.Raw("{");w.Key("acknowledgeResiduals",true);Array(w,v.acknowledgeResiduals,[&](const auto& x){Write(w,x);},KeyLess);
    w.Key("actions");Sequence(w,v.actions,[&](const auto& x){Write(w,x);});w.Key("carryCleanup");Array(w,v.carryCleanup,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return KeyLess(Key(a),Key(b));});
    w.Key("carryResidualReceipts");Array(w,v.carryResidualReceipts,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return KeyLess(Key(a),Key(b));});w.Key("expectedSequence");w.Counter(v.expectedSequence);w.Key("expectedState");Optional(w,v.expectedState);w.Key("installation");Write(w,v.installation);
    w.Key("newCleanup");Array(w,v.newCleanup,[&](const auto& x){Write(w,x);},KeyLess);w.Key("nextComponents");Array(w,v.nextComponents,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return a.id<b.id;});
    w.Key("nextRetained");Array(w,v.nextRetained,[&](const auto& x){Write(w,x);},[](const auto& a,const auto& b){return KeyLess(a.key,b.key);});w.Key("schema");w.String("simoder.store.plan@1");w.Key("transaction");Write(w,v.transaction);w.Raw("}");}
void Write(Writer& w,const JournalRecord& v){w.Raw("{");w.Key("action",true);Optional(w,v.action,[&](auto x){w.Number(x);});w.Key("backendEvidence");Optional(w,v.backendEvidence);w.Key("installation");Write(w,v.installation);w.Key("kind");w.String(Name(v.kind,JOURNAL_VALUES));w.Key("observation");w.String(Name(v.observation,OBSERVATION_VALUES));w.Key("plan");Write(w,v.plan);w.Key("previous");Optional(w,v.previous);w.Key("schema");w.String("simoder.store.journal-record@1");w.Key("sequence");w.Counter(v.sequence);w.Key("transaction");Write(w,v.transaction);w.Raw("}");}

/** @summary Compare entire canonical data, including protected inventories, rather than only visible identifiers. */
template<class T>bool Equivalent(const T& a,const T& b){Writer left(Limits::StateBytes),right(Limits::StateBytes);Write(left,a);Write(right,b);return left.Take()==right.Take();}
/** @summary Match complete set members without depending on input array order. */
template<class T>bool EquivalentSet(const std::vector<T>& a,const std::vector<T>& b){
    if(a.size()!=b.size())return false;for(const auto& item:a)if(std::none_of(b.begin(),b.end(),[&](const auto& other){return Equivalent(item,other);}))return false;return true;
}
/** @summary Refuse internal/allocation errors without changing the user's result object. */
template<class F>bool Guard(Error& error,F action)noexcept{
    error={};try{action();return true;}catch(const Refusal& failure){error={failure.code,0};}
    catch(const std::bad_alloc&){error={Code::Allocation,0};}catch(...){error={Code::Value,0};}return false;
}
/** @summary Decode into a private original and move only a completely validated record to its caller. */
template<class T>bool DecodeValue(std::string_view input,T& output,Error& error,std::size_t maximum)noexcept{
    error={};if(input.empty()||input.size()>maximum){error={Code::InputLimit,0};return false;}
    try{Reader reader(input,error);T next;if(!Read(reader,next)||!reader.End())return false;Check(next);output=std::move(next);return true;}
    catch(const Refusal& failure){if(error.code==Code::Ok)error={failure.code,0};}
    catch(const std::bad_alloc&){error={Code::Allocation,0};}catch(...){error={Code::Value,0};}return false;
}
/** @summary Validate before canonical output allocation, then publish only the complete bounded text. */
template<class T>bool EncodeValue(const T& value,std::string& output,Error& error,std::size_t maximum)noexcept{
    return Guard(error,[&]{Check(value);Writer writer(maximum);Write(writer,value);auto complete=writer.Take();output=std::move(complete);});
}
/** @summary Hash exact canonical UTF-8 with domain-NUL-LE64-length framing, reusing existing Core SHA rather than adding another hash implementation. */
template<class T>bool DigestValue(const T& value,Digest& output,Error& error,std::size_t maximum)noexcept{
    return Guard(error,[&]{Check(value);Writer writer(maximum);Write(writer,value);const auto encoded=writer.Take();
        constexpr std::string_view domain="simoder.component-store.metadata@1";std::array<std::byte,9> framing{};
        const auto length=static_cast<std::uint64_t>(encoded.size());for(unsigned i=0;i<8;++i)framing[i+1]=std::byte((length>>(i*8))&255U);
        std::array<std::byte,65536> workspace{};core::Sha256Digest digest{};
        Need(core::Sha256({std::as_bytes(std::span(domain.data(),domain.size())),framing,std::as_bytes(std::span(encoded.data(),encoded.size()))},workspace,digest),Code::Hash);
        Digest next;for(std::size_t i=0;i<next.bytes.size();++i)next.bytes[i]=std::to_integer<std::uint8_t>(digest[i]);Need(Nonzero(next),Code::Hash);output=next;
    });
}
/** @summary Sequence hashing before inspecting its error; function-argument evaluation order cannot hide a hash failure. */
template<class T>Digest RequiredDigest(const T& value,std::size_t maximum){Digest digest;Error error;
    if(!DigestValue(value,digest,error,maximum))throw Refusal{error.code};return digest;}
/** @summary Compare a new result against its complete acyclic plan, including every retained old obligation. */
void PlanResult(const TransactionPlan& plan,const State& result){
    Check(plan);Check(result);Need(result.phase==Phase::Accepted&&result.installation==plan.installation,Code::Reference);
    std::uint64_t preparing{},accepted{};Need(Advance(plan.expectedSequence,preparing)&&Advance(preparing,accepted)&&result.sequence==accepted,Code::Sequence);
    Need(result.previousAccepted==plan.expectedState&&EquivalentSet(result.components,plan.nextComponents)&&EquivalentSet(result.retained,plan.nextRetained),Code::Reference);
    const auto planDigest=RequiredDigest(plan,Limits::PlanBytes);
    Need(result.origin&&result.origin->transaction==plan.transaction&&result.origin->plan==planDigest&&result.origin->journal.transaction==plan.transaction,Code::Reference);
    const auto ack=[&](const GenerationKey& key){return std::find(plan.acknowledgeResiduals.begin(),plan.acknowledgeResiduals.end(),key)!=plan.acknowledgeResiduals.end();};
    std::size_t expectedCleanup=plan.newCleanup.size();for(const auto& item:plan.carryCleanup){if(ack(Key(item)))continue;++expectedCleanup;
        Need(std::any_of(result.pendingCleanup.begin(),result.pendingCleanup.end(),[&](const auto& other){return Same(item,other);}),Code::Reference);}
    Need(result.pendingCleanup.size()==expectedCleanup,Code::Reference);
    for(const auto& key:plan.newCleanup){const auto found=std::find_if(result.pendingCleanup.begin(),result.pendingCleanup.end(),[&](const auto& item){return Key(item)==key;});
        Need(found!=result.pendingCleanup.end()&&found->plan==planDigest&&found->journal.transaction==plan.transaction&&Same(found->journal,result.origin->journal),Code::Reference);}
    for(const auto& item:plan.carryResidualReceipts){auto expected=item;if(ack(Key(item)))expected.preserveAcknowledged=true;
        Need(std::any_of(result.residualReceipts.begin(),result.residualReceipts.end(),[&](const auto& other){return Equivalent(expected,other);}),Code::Reference);}
    for(const auto& item:result.residualReceipts){const bool carried=std::any_of(plan.carryResidualReceipts.begin(),plan.carryResidualReceipts.end(),[&](const auto& old){return Key(old)==Key(item);});
        if(carried)continue;Need(!item.preserveAcknowledged&&std::find(plan.newCleanup.begin(),plan.newCleanup.end(),Key(item))!=plan.newCleanup.end()&&
            item.plan==planDigest&&Same(item.journal,result.origin->journal),Code::Reference);}
}
} // namespace

bool RelativePathValid(std::string_view path,bool allowEmpty)noexcept{
    if(path.empty())return allowEmpty;if(path.size()>Limits::RelativePath||path.front()=='/'||path.back()=='/')return false;
    std::size_t at{},parts{};while(at<path.size()){const auto end=path.find('/',at);const auto part=path.substr(at,end==path.npos?path.size()-at:end-at);
        if(++parts>16||part.empty()||part.size()>100||part=="."||part==".."||part.back()=='.'||part.back()==' ')return false;
        for(unsigned char byte:part)if(byte<33||byte>126||byte=='\\'||byte==':'||byte=='*'||byte=='?'||byte=='"'||byte=='<'||byte=='>'||byte=='|')return false;
        const auto stem=part.substr(0,part.find('.'));if(EqualFold(stem,"con")||EqualFold(stem,"prn")||EqualFold(stem,"aux")||EqualFold(stem,"nul")||
            (stem.size()==4&&(EqualFold(stem.substr(0,3),"com")||EqualFold(stem.substr(0,3),"lpt"))&&stem[3]>='1'&&stem[3]<='9'))return false;
        if(end==path.npos)break;at=end+1;
    }return true;
}
bool ParseDigest(std::string_view text,Digest& output)noexcept{return HexValue(text,output);}
bool ParseIdentifier(std::string_view text,Identifier& output)noexcept{return HexValue(text,output);}
bool Advance(std::uint64_t value,std::uint64_t& output)noexcept{if(value==UINT64_MAX)return false;output=value+1;return true;}

bool Validate(const Component& v,Error& e)noexcept{return Guard(e,[&]{Check(v);});}
bool Validate(const State& v,Error& e)noexcept{return Guard(e,[&]{Check(v);});}
bool Validate(const Selector& v,Error& e)noexcept{return Guard(e,[&]{Check(v);});}
bool Validate(const Attachment& v,Error& e)noexcept{return Guard(e,[&]{Check(v);});}
bool Validate(const TransactionPlan& v,Error& e)noexcept{return Guard(e,[&]{Check(v);});}
bool Validate(const JournalRecord& v,Error& e)noexcept{return Guard(e,[&]{Check(v);});}
bool Decode(std::string_view s,Selector& v,Error& e)noexcept{return DecodeValue(s,v,e,Limits::SelectorBytes);}
bool Decode(std::string_view s,State& v,Error& e)noexcept{return DecodeValue(s,v,e,Limits::StateBytes);}
bool Decode(std::string_view s,Attachment& v,Error& e)noexcept{return DecodeValue(s,v,e,Limits::AttachmentBytes);}
bool Decode(std::string_view s,TransactionPlan& v,Error& e)noexcept{return DecodeValue(s,v,e,Limits::PlanBytes);}
bool Decode(std::string_view s,JournalRecord& v,Error& e)noexcept{return DecodeValue(s,v,e,Limits::JournalRecordBytes);}
bool Encode(const Selector& v,std::string& s,Error& e)noexcept{return EncodeValue(v,s,e,Limits::SelectorBytes);}
bool Encode(const State& v,std::string& s,Error& e)noexcept{return EncodeValue(v,s,e,Limits::StateBytes);}
bool Encode(const Attachment& v,std::string& s,Error& e)noexcept{return EncodeValue(v,s,e,Limits::AttachmentBytes);}
bool Encode(const TransactionPlan& v,std::string& s,Error& e)noexcept{return EncodeValue(v,s,e,Limits::PlanBytes);}
bool Encode(const JournalRecord& v,std::string& s,Error& e)noexcept{return EncodeValue(v,s,e,Limits::JournalRecordBytes);}
bool CanonicalDigest(const State& v,Digest& d,Error& e)noexcept{return DigestValue(v,d,e,Limits::StateBytes);}
bool CanonicalDigest(const TransactionPlan& v,Digest& d,Error& e)noexcept{return DigestValue(v,d,e,Limits::PlanBytes);}
bool CanonicalDigest(const JournalRecord& v,Digest& d,Error& e)noexcept{return DigestValue(v,d,e,Limits::JournalRecordBytes);}

bool ValidateSelection(const Selector& selector,const State& state,Error& error)noexcept{
    return Guard(error,[&]{Check(selector);Check(state);Need(selector.installation==state.installation&&selector.sequence==state.sequence,Code::Reference);
        const auto digest=RequiredDigest(state,Limits::StateBytes);Need(selector.state==digest,Code::Hash);
        if(state.previousAccepted)Need(*state.previousAccepted!=digest,Code::Reference);for(const auto& old:state.retained)Need(old.acceptedState!=digest,Code::Reference);
    });
}
bool ValidatePlanAgainstPrior(const TransactionPlan& plan,const State* prior,Error& error)noexcept{
    return Guard(error,[&]{Check(plan);if(!prior){Need(!plan.expectedState&&plan.expectedSequence==0&&plan.carryCleanup.empty()&&plan.carryResidualReceipts.empty()&&plan.newCleanup.empty()&&plan.nextRetained.empty(),Code::Reference);return;}
        Check(*prior);Need(prior->phase==Phase::Accepted&&prior->installation==plan.installation&&prior->sequence==plan.expectedSequence&&plan.expectedState,Code::Reference);
        const auto digest=RequiredDigest(*prior,Limits::StateBytes);Need(*plan.expectedState==digest,Code::Hash);
        Need(EquivalentSet(plan.carryCleanup,prior->pendingCleanup)&&EquivalentSet(plan.carryResidualReceipts,prior->residualReceipts),Code::Reference);
        for(const auto& key:plan.newCleanup){const bool selected=std::any_of(prior->components.begin(),prior->components.end(),[&](const auto& c){return Key(c)==key;});
            const bool retained=std::any_of(prior->retained.begin(),prior->retained.end(),[&](const auto& c){return c.key==key;});Need(selected||retained,Code::Reference);}
        for(const auto& old:plan.nextRetained){const bool previouslyRetained=std::any_of(prior->retained.begin(),prior->retained.end(),[&](const auto& c){return Equivalent(c,old);});
            const bool previouslySelected=old.acceptedState==digest&&std::any_of(prior->components.begin(),prior->components.end(),[&](const auto& c){return Key(c)==old.key;});Need(previouslyRetained||previouslySelected,Code::Reference);}
    });
}
bool ValidatePlanResultIntent(const TransactionPlan& plan,const State& result,Error& error)noexcept{return Guard(error,[&]{PlanResult(plan,result);});}
bool ValidateJournal(std::span<const JournalRecord> records,const TransactionPlan& plan,Error& error)noexcept{
    return Guard(error,[&]{Check(plan);Need(!records.empty()&&records.size()<=Limits::JournalRecords,Code::Limit);
        const auto planDigest=RequiredDigest(plan,Limits::PlanBytes);std::optional<std::uint32_t> pending;std::size_t completed{};Digest previous;
        for(std::size_t i=0;i<records.size();++i){const auto& record=records[i];Check(record);
            Need(record.installation==plan.installation&&record.transaction==plan.transaction&&record.plan==planDigest&&record.sequence==i+1,Code::Reference);
            if(i==0)Need(record.kind==JournalKind::Prepared,Code::Phase);else Need(record.previous&&*record.previous==previous,Code::Hash);
            if(record.kind==JournalKind::Intent){Need(!pending&&record.action&&*record.action==completed&&completed<plan.actions.size(),Code::Sequence);pending=record.action;}
            else if(record.kind==JournalKind::Effect){Need(pending&&record.action==pending,Code::Sequence);
                const auto kind=plan.actions[*pending].kind;
                const bool removal=kind==ActionKind::RemoveFile||kind==ActionKind::RemoveDirectory;
                const bool reuse=kind==ActionKind::PublishGeneration&&record.observation==Observation::Unchanged;
                Need(removal?record.observation==Observation::Removed:(record.observation==Observation::Created||reuse),Code::Phase);
                pending.reset();++completed;}
            else if(record.kind==JournalKind::Completed){Need(!pending&&completed==plan.actions.size()&&i+1==records.size()&&record.observation==Observation::None,Code::Phase);}
            else if(record.kind==JournalKind::PartialProtected){Need(i+1==records.size()&&record.observation==Observation::Protected,Code::Phase);}
            else if(record.kind==JournalKind::Unknown)Need(i+1==records.size()&&record.observation==Observation::Unknown,Code::Phase);
            previous=RequiredDigest(record,Limits::JournalRecordBytes);
        }
    });
}
} // namespace sc13::platform::component_store::metadata


