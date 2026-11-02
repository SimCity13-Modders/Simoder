/** @file @summary Finite Base file-selection transaction over explicitly selected native StoreIO and independently selected package inputs. */
#include "platform/component_store/coordinator.hpp"
#include "platform/component_store/verified_state.hpp"
#include "platform/component_store/streaming_digest.hpp"
#include "core/sha256.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <set>
#include <utility>

namespace sc13::platform::component_store {
namespace {
namespace m=metadata;
/** @summary A bounded original operation stops without inventing rollback or IO completion. */
struct Stop final {InstallStatus status;};
/** @summary Lowercase fixed data formatting, not an authority token. */
template<class T>std::string Hex(const T& bytes){constexpr char alphabet[]="0123456789abcdef";std::string text;text.reserve(bytes.size()*2);
    for(auto value:bytes){const auto byte=static_cast<unsigned>(value);text.push_back(alphabet[byte>>4]);text.push_back(alphabet[byte&15]);}return text;}
/** @summary Format only finite caller/native counters without locale or exponent conversion. */
std::string Decimal(std::uint64_t value){char bytes[20];const auto converted=std::to_chars(bytes,bytes+sizeof(bytes),value);return {bytes,static_cast<std::size_t>(converted.ptr-bytes)};}
/** @summary Exact original byte view borrowed only during a synchronous native call. */
std::span<const std::byte> Bytes(std::string_view value){return std::as_bytes(std::span(value.data(),value.size()));}
/** @summary Match Core digest bytes explicitly; no native layout or pointer enters portable identity. */
m::Digest Digest(const core::Sha256Digest& value){m::Digest result;for(std::size_t i=0;i<32;++i)result.bytes[i]=std::to_integer<std::uint8_t>(value[i]);return result;}
/** @summary Hash actual immutable bytes, not a display digest or package name. */
bool Hash(std::span<const std::byte> bytes,m::Digest& output){core::Sha256Digest digest{};std::array<std::byte,65536> scratch{};
    if(!core::Sha256(bytes,scratch,digest))return false;output=Digest(digest);return true;}
/** @summary Strict complete fixture/native pin agreement; the validated Snapshot constructor cannot be replaced by a JSON manifest. */
bool BoundSnapshot(const BaseRequest& request){
    m::Error error;if(!request.captured||!m::Validate(request.desired,error)||request.desired.id!=m::ComponentId::Base)return false;
    const auto& trusted=request.independentlyTrusted;const auto& selected=request.desired;
    if(trusted.size!=selected.archiveBytes||Digest(trusted.digest)!=selected.package||trusted.files.size()!=selected.files.size()||
        request.captured->Count()!=trusted.files.size()||request.captured->Bytes().size()!=trusted.size)return false;
    m::Digest actual;if(!Hash(request.captured->Bytes(),actual)||actual!=selected.package)return false;
    std::set<std::string_view> names;std::array<bool,m::Limits::Files> capturedMembers{};
    for(const auto& expected:trusted.files){if(!names.insert(expected.path).second)return false;
        const auto file=std::find_if(selected.files.begin(),selected.files.end(),[&](const auto& f){return f.path==expected.path;});
        if(file==selected.files.end()||file->bytes!=expected.size||file->sha256!=Digest(expected.digest)||(file->role==m::FileRole::Notice)!=expected.notice)return false;
        if(file->role==m::FileRole::Manifest&&selected.trust.manifest!=file->sha256)return false;
        bool found{};for(std::size_t i=0;i<request.captured->Count();++i)if(request.captured->Name(i)==expected.path){
            if(found||capturedMembers[i]||request.captured->Notice(i)!=expected.notice||request.captured->File(i).size()!=expected.size||!Hash(request.captured->File(i),actual)||actual!=file->sha256)return false;found=true;capturedMembers[i]=true;}
        if(!found)return false;
    }
    for(std::size_t i=0;i<request.captured->Count();++i)if(!capturedMembers[i])return false;
    for(const auto& file:selected.files)if(!names.contains(file.path))return false;return true;
}
/** @summary Compare complete immutable package observations while permitting caller-independent list order. */
bool SamePackage(const m::Component& a,const m::Component& b){
    if(a.id!=b.id||a.package!=b.package||a.archiveBytes!=b.archiveBytes||a.version!=b.version||a.payloadRoot!=b.payloadRoot||a.nativeEntry!=b.nativeEntry||a.requiresBase!=b.requiresBase||
        a.trust.mode!=b.trust.mode||a.trust.source!=b.trust.source||a.trust.manifest!=b.trust.manifest||a.trust.publisher!=b.trust.publisher||a.trust.signature!=b.trust.signature||
        a.compatibility.hostRoot!=b.compatibility.hostRoot||a.compatibility.baseContract!=b.compatibility.baseContract||a.compatibility.catalogueContract!=b.compatibility.catalogueContract||a.compatibility.sdkBundle!=b.compatibility.sdkBundle||a.files.size()!=b.files.size())return false;
    for(const auto& file:a.files)if(std::none_of(b.files.begin(),b.files.end(),[&](const auto& other){return file.path==other.path&&file.bytes==other.bytes&&file.sha256==other.sha256&&file.role==other.role;}))return false;return true;
}
/** @summary Shared original operation state remains live until synchronous file/lock operations have returned. */
struct Original final {
    StoreIO& io;Parent& parent;const Observation& parentObservation;const NativeAdmission& admission;const m::Identifier& installation;
    const m::Component* desired;const base_artifact::TrustedArtifact* independentlyTrusted;
    Store store;Coordination lock;BaseReceipt receipt;bool effects{};
    /** @summary Record actual low-level outcome before deciding whether any new effect is permitted. */
    void ResultOf(Result result){receipt.last=result;if(result.status==Status::Ok)return;
        if(result.status==Status::Unknown||result.effect==Effect::Unknown||result.effect==Effect::Published)throw Stop{InstallStatus::Unknown};
        if(result.status==Status::Busy)throw Stop{receipt.acceptedSelectionPublished?InstallStatus::Unknown:effects?InstallStatus::Interrupted:InstallStatus::Busy};
        if(result.status==Status::Unsupported)throw Stop{effects?InstallStatus::Unknown:InstallStatus::Unsupported};
        throw Stop{receipt.acceptedSelectionPublished?InstallStatus::Unknown:receipt.preparingSelectionPublished?InstallStatus::Interrupted:effects?InstallStatus::Unknown:InstallStatus::Refused};
    }
    /** @summary Recheck the SAME original independently supplied native scope, never a flag in portable state. */
    void Current(){if(!admission.Current(parent,parentObservation,installation,desired,independentlyTrusted))
        throw Stop{receipt.acceptedSelectionPublished?InstallStatus::Unknown:receipt.preparingSelectionPublished?InstallStatus::Interrupted:effects?InstallStatus::Unknown:InstallStatus::Refused};}
    /** @summary Require actual native namespace acknowledgement, not a file flush/readback surrogate. */
    void Published(Result result){ResultOf(result);if(!io.PublicationAcknowledged(result,PublicationRequirement::DurableNamespace))throw Stop{InstallStatus::Unknown};effects=true;}
    /** @summary Read only one bounded ordinary retained original and close it explicitly before returning bytes. */
    std::string Read(std::string_view path,std::size_t maximum,Observation* observed=nullptr){
        Current();File file;Observation actual;ResultOf(io.OpenRead(store,lock,path,maximum,file,actual));
        std::string text;text.resize(static_cast<std::size_t>(actual.bytes));std::size_t received{};const auto read=io.Read(file,std::as_writable_bytes(std::span(text.data(),text.size())),received);
        const auto closed=io.Close(file);ResultOf(read);ResultOf(closed);if(received!=text.size())throw Stop{InstallStatus::Unknown};if(observed)*observed=actual;return text;
    }
    /** @summary Hash actual retained payload bytes with fixed stack scratch; never materialize a native16MiB verification copy. */
    m::Digest ReadDigest(std::string_view path,std::uint32_t expected){
        Current();File file;Observation actual;ResultOf(io.OpenRead(store,lock,path,(std::max<std::size_t>)(expected,1),file,actual));
        core::Sha256Digest digest{};const auto hashed=ReadRetainedDigest(io,file,actual,expected,digest);
        const auto closed=io.Close(file);ResultOf(hashed.read);ResultOf(closed);
        // @summary Successful short reads retain the original Unknown disposition only after actual read/close errors.
        if(hashed.status==DigestStatus::ShortRead)throw Stop{InstallStatus::Unknown};
        if(hashed.status!=DigestStatus::Complete){receipt.metadataError={m::Code::Hash,0};throw Stop{InstallStatus::Refused};}
        return Digest(digest);
    }
    /** @summary Write one exclusively created original to completion without reopening a path or deleting a failure. */
    Observation Write(std::string_view path,std::span<const std::byte> bytes,File& file){
        Current();Observation created;const auto create=io.CreateFile(store,lock,path,(std::max<std::size_t>)(bytes.size(),1),file,created);
        if(create.effect!=Effect::None)effects=true;ResultOf(create);ResultOf(io.Write(file,bytes));ResultOf(io.Flush(file));return created;
    }
    /** @summary Publish a flushed metadata original into a closed derived no-replace namespace. */
    void Record(std::string_view candidate,const RecordTarget& target,std::string_view bytes){
        File file;static_cast<void>(Write(candidate,Bytes(bytes),file));Current();Published(io.PublishRecord(store,lock,file,target,PublicationRequirement::DurableNamespace));ResultOf(io.Close(file));
    }
    /** @summary Create one exact fresh scaffold/declared directory; collisions are never adopted. */
    Observation Directory(std::string_view path){Current();Observation created;const auto result=io.CreateDirectory(store,lock,path,created);
        if(result.effect!=Effect::None)effects=true;ResultOf(result);return created;}
};
/** @summary Capture actual local attachment without treating its account/root bytes as a package issuer. */
m::Attachment AttachmentOf(Original& original){
    m::Attachment result;original.ResultOf(original.io.CaptureAttachment(original.parent,original.installation,result));
    m::Error error;if(!m::Validate(result,error)){original.receipt.metadataError=error;throw Stop{InstallStatus::Refused};}return result;
}
/** @summary Compare every original local attachment fact; portable package hashes cannot authorize relocation. */
bool SameAttachment(const m::Attachment& a,const m::Attachment& b){return a.installation==b.installation&&a.backend==b.backend&&a.root==b.root&&a.account==b.account&&a.volumeIdentity==b.volumeIdentity&&a.objectIdentity==b.objectIdentity;}
/** @summary Typed decodes/hashes preserve their exact original errors without using invalid partial values. */
template<class T>T Decode(Original& original,std::string_view text){T value;if(!m::Decode(text,value,original.receipt.metadataError))throw Stop{InstallStatus::Refused};return value;}
template<class T>std::string Encode(Original& original,const T& value){std::string bytes;if(!m::Encode(value,bytes,original.receipt.metadataError))throw Stop{InstallStatus::Refused};return bytes;}
template<class T>m::Digest Canonical(Original& original,const T& value){m::Digest digest;if(!m::CanonicalDigest(value,digest,original.receipt.metadataError))throw Stop{InstallStatus::Refused};return digest;}

/** @summary Refuse malformed logical/capture evidence without printing imported fields. */
void Require(Original& original,bool value,m::Code code=m::Code::Reference){if(!value){original.receipt.metadataError={code,0};throw Stop{InstallStatus::Refused};}}
/** @summary Enumerate only a closed native namespace; unknown entries remain untouched. */
std::vector<NamedObservation> List(Original& original,InventoryKind kind,std::string_view identity={},std::string_view relative={}){
    original.Current();std::vector<NamedObservation> entries;original.ResultOf(original.io.ListDirectory(original.store,original.lock,{kind,identity,relative},2048,entries));return entries;
}
/** @summary Derive declared directory prefixes; never follow an unexpected enumerated directory. */
std::set<std::string> Directories(const m::Component& component){std::set<std::string> directories;
    for(const auto& file:component.files)for(std::size_t at=file.path.find('/');at!=file.path.npos;at=file.path.find('/',at+1))directories.insert(file.path.substr(0,at));return directories;}
/** @summary Recheck exact complete member map and actual bytes, not merely a content-addressed directory name. */
void VerifyPackage(Original& original,const m::Component& component,bool staged,std::string_view transaction={}){
    const auto digest=Hex(component.package.bytes);const auto directories=Directories(component);std::vector<std::string> visits{std::string{}};visits.insert(visits.end(),directories.begin(),directories.end());std::size_t total{};
    for(const auto& directory:visits){std::map<std::string,bool> expected;const auto prefix=directory.empty()?std::string{}:directory+'/';
        for(const auto& file:component.files){if(!file.path.starts_with(prefix))continue;const auto remainder=file.path.substr(prefix.size());const auto slash=remainder.find('/');
            expected[remainder.substr(0,slash)]=slash!=remainder.npos;}
        const auto entries=List(original,staged?InventoryKind::StagedPackage:InventoryKind::Package,staged?transaction:std::string_view(digest),directory);
        total+=entries.size();Require(original,total<=2048,m::Code::Limit);Require(original,entries.size()==expected.size(),m::Code::Collision);
        for(const auto& entry:entries){const auto found=expected.find(entry.nameUtf8);Require(original,found!=expected.end()&&found->second==entry.observed.directory,m::Code::Collision);}
    }
    for(const auto& file:component.files){const auto path=(staged?"transactions/"+std::string(transaction)+"/payload":component.payloadRoot)+"/"+file.path;
        const auto digestValue=original.ReadDigest(path,file.bytes);Require(original,digestValue==file.sha256,m::Code::Hash);}
}
/** @summary Directory entry identifiers are validated before constructing any referenced metadata pathname. */
std::string StateKey(Original& original,const NamedObservation& entry){Require(original,!entry.observed.directory&&entry.nameUtf8.size()==69&&entry.nameUtf8.ends_with(".json"));
    const auto key=entry.nameUtf8.substr(0,64);m::Digest parsed;Require(original,m::ParseDigest(key,parsed));return key;}
/** @summary Exact transaction directory grammar never accepts a display name as native ownership. */
std::string TransactionKey(Original& original,const NamedObservation& entry){m::Identifier value;Require(original,entry.observed.directory&&m::ParseIdentifier(entry.nameUtf8,value));return entry.nameUtf8;}
/** @summary Parse an exact canonical decimal journal member index, not lexicographic sort order. */
std::uint32_t RecordNumber(Original& original,const NamedObservation& entry){Require(original,!entry.observed.directory&&entry.nameUtf8.ends_with(".json"));
    const auto text=std::string_view(entry.nameUtf8).substr(0,entry.nameUtf8.size()-5);Require(original,!text.empty()&&(text.size()==1||text.front()!='0'));
    std::uint32_t value{};const auto converted=std::from_chars(text.data(),text.data()+text.size(),value);Require(original,converted.ec==std::errc{}&&converted.ptr==text.data()+text.size()&&value&&value<=m::Limits::JournalRecords,m::Code::Sequence);return value;}
/** @summary Read/hash one immutable state from its exact content locator. */
m::State ReadState(Original& original,const m::Digest& digest){const auto bytes=original.Read("states/"+Hex(digest.bytes)+".json",m::Limits::StateBytes);
    auto state=Decode<m::State>(original,bytes);Require(original,Canonical(original,state)==digest,m::Code::Hash);Require(original,state.installation==original.installation);return state;}
/** @summary Actual original plan and journal remain observations until native roots and payloads are freshly verified. */
struct TransactionData final {m::TransactionPlan plan;std::vector<m::JournalRecord> records;std::vector<m::Digest> digests;bool completed{},unresolved{};};
/** @summary Load a bounded original journal, validate every link and referenced local-evidence bytes; never replay it. */
TransactionData ReadTransaction(Original& original,const m::PendingTransaction& reference){
    TransactionData data;const auto tx=Hex(reference.transaction.bytes);const auto prefix="transactions/"+tx+"/";
    data.plan=Decode<m::TransactionPlan>(original,original.Read(prefix+"plan.json",m::Limits::PlanBytes));
    Require(original,Canonical(original,data.plan)==reference.plan&&data.plan.transaction==reference.transaction&&data.plan.installation==original.installation,m::Code::Hash);
    auto entries=List(original,InventoryKind::Journal,tx);Require(original,!entries.empty()&&entries.size()<=m::Limits::JournalRecords,m::Code::Limit);
    std::sort(entries.begin(),entries.end(),[&](const auto& a,const auto& b){return RecordNumber(original,a)<RecordNumber(original,b);});
    std::set<std::uint32_t> evidence;
    for(std::size_t i=0;i<entries.size();++i){const auto index=RecordNumber(original,entries[i]);Require(original,index==i+1,m::Code::Sequence);
        auto record=Decode<m::JournalRecord>(original,original.Read(prefix+"journal/"+Decimal(index)+".json",m::Limits::JournalRecordBytes));
        if(record.backendEvidence){const auto bytes=original.Read(prefix+"evidence/"+Decimal(index)+".json",4096);m::Digest digest;
            Require(original,original.io.ValidateHistoricalEvidence(bytes,{record.installation,record.transaction,record.sequence,record.observation})&&Hash(Bytes(bytes),digest)&&digest==*record.backendEvidence,m::Code::Hash);evidence.insert(index);}
        data.digests.push_back(Canonical(original,record));data.records.push_back(std::move(record));
    }
    Require(original,m::ValidateJournal(data.records,data.plan,original.receipt.metadataError));
    Require(original,reference.journal.sequence<=data.records.size()&&data.digests[static_cast<std::size_t>(reference.journal.sequence-1)]==reference.journal.record,m::Code::Hash);
    data.completed=data.records.back().kind==m::JournalKind::Completed;
    // @summary Only the untouched Prepared prefix can establish the known pre-payload interruption cut on fresh inspection.
    data.unresolved=!data.completed&&data.records.back().kind!=m::JournalKind::Prepared;
    const auto evidenceEntries=List(original,InventoryKind::Evidence,tx);for(const auto& entry:evidenceEntries){const auto index=RecordNumber(original,entry);
        // @summary Only a pending actual Intent can precede its own durable evidence without an Effect acknowledgement.
        const bool interruptedEvidence=!data.completed&&data.records.back().kind==m::JournalKind::Intent&&index==data.records.size()+1;
        if(interruptedEvidence)data.unresolved=true;
        Require(original,evidence.contains(index)||interruptedEvidence,m::Code::Reference);}
    bool havePlan{},haveJournal{},haveEvidence{};
    for(const auto& entry:List(original,InventoryKind::Transaction,tx)){
        if(entry.nameUtf8=="plan.json"&&!entry.observed.directory){havePlan=true;continue;}
        if(entry.nameUtf8=="journal"&&entry.observed.directory){haveJournal=true;continue;}
        if(entry.nameUtf8=="evidence"&&entry.observed.directory){haveEvidence=true;continue;}
        bool transient=!data.completed&&entry.observed.directory&&entry.nameUtf8=="payload";
        if(!data.completed&&!entry.observed.directory){
            constexpr std::array<std::string_view,6> fixed{"attachment.tmp","plan.tmp","state-preparing.tmp","state-accepted.tmp","selector-preparing.tmp","selector-accepted.tmp"};
            transient=std::find(fixed.begin(),fixed.end(),entry.nameUtf8)!=fixed.end();
            for(auto prefixName:{std::string_view("journal-"),std::string_view("evidence-")})if(entry.nameUtf8.starts_with(prefixName)&&entry.nameUtf8.ends_with(".tmp")){
                const auto digits=std::string_view(entry.nameUtf8).substr(prefixName.size(),entry.nameUtf8.size()-prefixName.size()-4);std::uint32_t index{};
                const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),index);
                transient=!digits.empty()&&(digits.size()==1||digits.front()!='0')&&parsed.ec==std::errc{}&&parsed.ptr==digits.data()+digits.size()&&index&&index<=m::Limits::JournalRecords;
            }
        }
        // @summary Unrecognized internal debris is neither adopted nor traversed; an incomplete original remains explicitly unknown.
        if(!transient)throw Stop{data.completed?InstallStatus::Refused:InstallStatus::Unknown};
        data.unresolved=true;
    }
    Require(original,havePlan&&haveJournal&&haveEvidence,m::Code::Reference);return data;
}
/** @summary Current data plus minimal exact preimage retained through the same native coordination lock. */
struct Loaded final {m::Attachment attachment;m::Selector selector;m::State state;std::string selectorBytes;Observation selectorObservation;
    std::set<std::string> states,transactions,packages;std::size_t acceptedStates{};bool completed{},unresolved{};};
/** @summary Derive the PREPARING record from an already existing accepted predecessor and immutable original plan/prefix. */
m::State Preparing(const m::TransactionPlan& plan,const m::State* prior,const m::Digest& planDigest,const m::JournalReference& first){
    m::State state;if(prior){state.components=prior->components;state.pendingCleanup=prior->pendingCleanup;state.residualReceipts=prior->residualReceipts;state.retained=prior->retained;}
    state.installation=plan.installation;static_cast<void>(m::Advance(plan.expectedSequence,state.sequence));state.phase=m::Phase::Preparing;state.previousAccepted=plan.expectedState;
    state.pendingTransaction=m::PendingTransaction{plan.transaction,planDigest,first};return state;
}
/** @summary Verify the complete finite reachable metadata history and reject unreferenced/incomplete transaction debris before new maintenance. */
Loaded Load(Original& original){
    Loaded loaded;loaded.attachment=Decode<m::Attachment>(original,original.Read("attachment.json",m::Limits::AttachmentBytes));
    Require(original,SameAttachment(loaded.attachment,AttachmentOf(original)));
    loaded.selectorBytes=original.Read("current.json",m::Limits::SelectorBytes,&loaded.selectorObservation);loaded.selector=Decode<m::Selector>(original,loaded.selectorBytes);
    loaded.state=ReadState(original,loaded.selector.state);Require(original,m::ValidateSelection(loaded.selector,loaded.state,original.receipt.metadataError));
    Require(original,loaded.selector.installation==original.installation);
    const auto currentReference=loaded.state.phase==m::Phase::Preparing?*loaded.state.pendingTransaction:*loaded.state.origin;
    {const auto currentTransaction=ReadTransaction(original,currentReference);loaded.completed=currentTransaction.completed;loaded.unresolved=currentTransaction.unresolved;
        if(loaded.state.phase==m::Phase::Preparing&&currentTransaction.completed)loaded.unresolved=true;}
    m::State walk=loaded.state;m::Digest walkDigest=loaded.selector.state;
    for(std::size_t count=0;;++count){Require(original,count<128,m::Code::Limit);Require(original,loaded.states.insert(Hex(walkDigest.bytes)).second,m::Code::Reference);
        const auto reference=walk.phase==m::Phase::Preparing?*walk.pendingTransaction:*walk.origin;auto transaction=ReadTransaction(original,reference);
        loaded.transactions.insert(Hex(reference.transaction.bytes));const auto planDigest=Canonical(original,transaction.plan);
        std::optional<m::State> prior;if(transaction.plan.expectedState)prior=ReadState(original,*transaction.plan.expectedState);
        Require(original,m::ValidatePlanAgainstPrior(transaction.plan,prior?&*prior:nullptr,original.receipt.metadataError));
        const m::JournalReference first{reference.transaction,1,transaction.digests.front()};const auto preparing=Preparing(transaction.plan,prior?&*prior:nullptr,planDigest,first);
        const auto preparingDigest=Canonical(original,preparing);loaded.states.insert(Hex(preparingDigest.bytes));
        const auto actualPreparing=ReadState(original,preparingDigest);Require(original,Canonical(original,actualPreparing)==preparingDigest,m::Code::Hash);
        if(walk.phase==m::Phase::Accepted){++loaded.acceptedStates;Require(original,m::ValidatePlanResultIntent(transaction.plan,walk,original.receipt.metadataError));}
        else Require(original,walkDigest==preparingDigest,m::Code::Hash);
        if(transaction.unresolved)loaded.unresolved=true;
        if(walk.phase==m::Phase::Accepted&&!transaction.completed){loaded.completed=false;loaded.unresolved=true;}
        for(const auto& component:walk.components)loaded.packages.insert(Hex(component.package.bytes));
        if(!prior)break;walk=std::move(*prior);walkDigest=*transaction.plan.expectedState;
    }
    const auto stateEntries=List(original,InventoryKind::States);Require(original,stateEntries.size()<=256,m::Code::Limit);
    for(const auto& entry:stateEntries)Require(original,loaded.states.contains(StateKey(original,entry)),m::Code::Reference);
    Require(original,stateEntries.size()==loaded.states.size(),m::Code::Reference);
    const auto transactionEntries=List(original,InventoryKind::Transactions);Require(original,transactionEntries.size()<=128,m::Code::Limit);
    for(const auto& entry:transactionEntries)Require(original,loaded.transactions.contains(TransactionKey(original,entry)),m::Code::Reference);
    Require(original,transactionEntries.size()==loaded.transactions.size(),m::Code::Reference);
    for(const auto& entry:List(original,InventoryKind::Packages)){m::Digest digest;Require(original,entry.observed.directory&&m::ParseDigest(entry.nameUtf8,digest)&&loaded.packages.contains(entry.nameUtf8),m::Code::Reference);}
    for(const auto& component:loaded.state.components)VerifyPackage(original,component,false);
    for(const auto& retained:loaded.state.retained){const auto accepted=ReadState(original,retained.acceptedState);Require(original,accepted.phase==m::Phase::Accepted);
        const auto found=std::find_if(accepted.components.begin(),accepted.components.end(),[&](const auto& c){return c.id==retained.key.component&&c.package==retained.key.package&&c.generation==retained.key.generation;});
        Require(original,found!=accepted.components.end());VerifyPackage(original,*found,false);}
    return loaded;
}

/** @summary One original immutable journal prefix; no append path is reopened or replayed after an ambiguous effect. */
class Journal final {
public:
    /** @summary Bind only the exact native-selected transaction and independently hashed finite plan. */
    Journal(Original& original,const m::Identifier& transaction,m::Digest plan):original_(original),transaction_(transaction),plan_(plan),tx_(Hex(transaction.bytes)){}
    /** @summary Persist a new immutable record with namespace acknowledgement before returning its reference. */
    void Append(m::JournalKind kind,std::optional<std::uint32_t> action={},m::Observation observation=m::Observation::None,std::optional<m::Digest> evidence={}){
        m::JournalRecord record;record.installation=original_.installation;record.transaction=transaction_;record.plan=plan_;record.kind=kind;record.action=action;record.observation=observation;record.backendEvidence=evidence;
        if(last_){if(!m::Advance(last_->sequence,record.sequence))throw Stop{InstallStatus::Refused};record.previous=last_->record;}else record.sequence=1;
        const auto bytes=Encode(original_,record);const auto digest=Canonical(original_,record);const auto number=Decimal(record.sequence);
        original_.Record("transactions/"+tx_+"/journal-"+number+".tmp",{RecordKind::Journal,tx_,static_cast<std::uint32_t>(record.sequence)},bytes);
        last_=m::JournalReference{transaction_,record.sequence,digest};original_.receipt.journal=last_;
    }
    /** @summary Serialize observed local correlation separately from portable logical journal identity. */
    void Effect(std::uint32_t action,m::Observation observation,const component_store::Observation& actual){
        if(!last_||!actual.identity.length||actual.identity.length>actual.identity.bytes.size())throw Stop{InstallStatus::Unknown};
        const auto sequence=last_->sequence+1;if(sequence>m::Limits::JournalRecords)throw Stop{InstallStatus::Unknown};
        std::string bytes;original_.ResultOf(original_.io.EncodeLocalEvidence({original_.installation,transaction_,sequence,observation},actual,bytes));
        m::Digest digest;if(!Hash(Bytes(bytes),digest))throw Stop{InstallStatus::Unknown};
        original_.Record("transactions/"+tx_+"/evidence-"+Decimal(sequence)+".tmp",{RecordKind::Evidence,tx_,static_cast<std::uint32_t>(sequence)},bytes);
        Append(m::JournalKind::Effect,action,observation,digest);
    }
    /** @summary Borrow only the already durable original prefix; no future state/record digest is predicted. */
    m::JournalReference Last()const{if(!last_)throw Stop{InstallStatus::Unknown};return *last_;}
private:Original& original_;m::Identifier transaction_;m::Digest plan_;std::string tx_;std::optional<m::JournalReference> last_;
};
/** @summary Install plans contain only declared payload paths and one final fixed selector publication. */
m::TransactionPlan Plan(Original& original,const BaseRequest& request,const Loaded* loaded,bool reuse){
    m::TransactionPlan plan;plan.installation=request.installation;plan.transaction=request.transaction;
    if(loaded){plan.expectedSequence=loaded->state.sequence;plan.expectedState=loaded->selector.state;plan.nextComponents=loaded->state.components;
        plan.carryCleanup=loaded->state.pendingCleanup;plan.carryResidualReceipts=loaded->state.residualReceipts;plan.nextRetained=loaded->state.retained;}
    const auto old=std::find_if(plan.nextComponents.begin(),plan.nextComponents.end(),[](const auto& c){return c.id==m::ComponentId::Base;});
    if(old!=plan.nextComponents.end()){if(old->package!=request.desired.package||old->generation!=request.desired.generation)
        plan.nextRetained.push_back({{old->id,old->package,old->generation},loaded->selector.state});*old=request.desired;}
    else plan.nextComponents.push_back(request.desired);
    if(!reuse){auto directories=Directories(request.desired);std::vector<std::string> ordered(directories.begin(),directories.end());
        std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){const auto x=std::count(a.begin(),a.end(),'/'),y=std::count(b.begin(),b.end(),'/');return x!=y?x<y:a<b;});
        for(const auto& path:ordered)plan.actions.push_back({m::ActionKind::CreateDirectory,m::ComponentId::Base,request.desired.package,path,{},{}});
        std::vector<const m::FileEntry*> files;for(const auto& file:request.desired.files)files.push_back(&file);std::sort(files.begin(),files.end(),[](const auto* a,const auto* b){return a->path<b->path;});
        for(const auto* file:files)plan.actions.push_back({m::ActionKind::StageFile,m::ComponentId::Base,request.desired.package,file->path,{},file->sha256});}
    plan.actions.push_back({m::ActionKind::PublishGeneration,m::ComponentId::Base,request.desired.package,"",{},request.desired.package});
    plan.actions.push_back({m::ActionKind::PublishSelection,{}, {},"",plan.expectedState,{}});
    Require(original,m::ValidatePlanAgainstPrior(plan,loaded?&loaded->state:nullptr,original.receipt.metadataError));return plan;
}
/** @summary Retain metadata/transaction scaffolding without ever deleting it on failure or claiming it a payload generation. */
void Scaffold(Original& original,std::string_view tx,bool fresh){
    if(fresh){static_cast<void>(original.Directory("states"));static_cast<void>(original.Directory("packages"));static_cast<void>(original.Directory("transactions"));}
    static_cast<void>(original.Directory("transactions/"+std::string(tx)));static_cast<void>(original.Directory("transactions/"+std::string(tx)+"/journal"));
    static_cast<void>(original.Directory("transactions/"+std::string(tx)+"/evidence"));
}
/** @summary Publish a fully flushed original selector against the exact admitted old selector; never retry Unknown. */
Observation Selection(Original& original,std::string_view candidate,const m::Selector& selector,const Observation* expected,std::string_view expectedBytes,bool accepted){
    const auto bytes=Encode(original,selector);File file;static_cast<void>(original.Write(candidate,Bytes(bytes),file));original.Current();
    original.Published(original.io.PublishSelection(original.store,original.lock,file,expected,Bytes(expectedBytes),PublicationRequirement::DurableNamespace));
    if(accepted)original.receipt.acceptedSelectionPublished=true;else original.receipt.preparingSelectionPublished=true;
    original.receipt.state=selector.state;
    original.ResultOf(original.io.Close(file));Observation actual;const auto observed=original.Read("current.json",m::Limits::SelectorBytes,&actual);
    // @summary This is additional verification after positive namespace acknowledgement, never readback promotion of an ambiguous rename.
    Require(original,observed==bytes,m::Code::Hash);return actual;
}
/** @summary Check native scope and immutable package inputs before any reserved-tree effect. */
void ValidateRequest(Original& original,const BaseRequest& request){
    if(request.publication!=PublicationRequirement::DurableNamespace||request.desired.id!=m::ComponentId::Base)throw Stop{InstallStatus::Unsupported};
    m::Identifier id;Require(original,m::ParseIdentifier(Hex(request.installation.bytes),id)&&m::ParseIdentifier(Hex(request.transaction.bytes),id));
    original.Current();Require(original,BoundSnapshot(request),m::Code::Hash);static_cast<void>(Encode(original,AttachmentOf(original)));
}
/** @summary Unknown namespace entries are preserved rather than silently adopted into installation history. */
void RootInventory(Original& original){for(const auto& entry:List(original,InventoryKind::Root)){
    const bool directory=entry.nameUtf8=="states"||entry.nameUtf8=="packages"||entry.nameUtf8=="transactions"||entry.nameUtf8=="configuration";
    const bool file=entry.nameUtf8=="coordination.lock"||entry.nameUtf8=="attachment.json"||entry.nameUtf8=="current.json";
    Require(original,(directory&&entry.observed.directory)||(file&&!entry.observed.directory),m::Code::Reference);
}}
/** @summary First install and update allocate only one new generation, never automatically collect protected history. */
void Capacity(Original& original,const Loaded* loaded,const BaseRequest& request){
    if(!loaded){Require(original,request.desired.generation==1,m::Code::Sequence);return;}
    Require(original,loaded->acceptedStates<128&&loaded->states.size()+2<=256&&loaded->transactions.size()<128,m::Code::Limit);
    Require(original,!loaded->transactions.contains(Hex(request.transaction.bytes)),m::Code::Collision);
    std::uint64_t maximum{};for(const auto& c:loaded->state.components)if(c.id==m::ComponentId::Base)maximum=(std::max)(maximum,c.generation);
    for(const auto& c:loaded->state.retained)if(c.key.component==m::ComponentId::Base)maximum=(std::max)(maximum,c.key.generation);
    std::uint64_t next{};Require(original,m::Advance(maximum,next)&&request.desired.generation==next,m::Code::Sequence);
}
/** @summary Exact admitted reuse is limited to a currently retained accepted generation, not a digest-shaped directory. */
bool Reuse(Original& original,const Loaded* loaded,const m::Component& desired){
    if(!loaded)return false;for(const auto& retained:loaded->state.retained)if(retained.key.component==desired.id&&retained.key.package==desired.package){
        const auto state=ReadState(original,retained.acceptedState);const auto found=std::find_if(state.components.begin(),state.components.end(),[&](const auto& c){return c.id==retained.key.component&&c.package==retained.key.package&&c.generation==retained.key.generation;});
        Require(original,found!=state.components.end()&&SamePackage(*found,desired));VerifyPackage(original,desired,false);return true;}return false;
}
/** @summary Read only the original immutable Snapshot member; archive or filesystem paths are never reread. */
std::span<const std::byte> Member(const BaseRequest& request,std::string_view path){for(std::size_t i=0;i<request.captured->Count();++i)if(request.captured->Name(i)==path)return request.captured->File(i);throw Stop{InstallStatus::Refused};}

/** @summary Execute one complete original Base transaction; metadata commit is distinct from any loader activation. */
void Run(Original& original,const BaseRequest& request){
    ValidateRequest(original,request);Observation storeObservation;const auto opened=original.io.OpenStore(original.parent,original.store,storeObservation);bool fresh{};
    std::optional<Loaded> loaded;
    if(opened.status==Status::Absent)fresh=true;
    else {original.ResultOf(opened);original.ResultOf(original.io.Lock(original.store,original.lock));RootInventory(original);loaded=Load(original);
        if(loaded->state.phase==m::Phase::Preparing)throw Stop{loaded->unresolved?InstallStatus::Unknown:InstallStatus::Interrupted};if(!loaded->completed||loaded->unresolved)throw Stop{InstallStatus::Unknown};
        for(const auto& component:loaded->state.components)if(component.id==m::ComponentId::Base&&SamePackage(component,request.desired)){
            original.receipt.state=loaded->selector.state;original.receipt.status=InstallStatus::AlreadySelected;return;}}
    Capacity(original,loaded?&*loaded:nullptr,request);const auto reuse=Reuse(original,loaded?&*loaded:nullptr,request.desired);auto plan=Plan(original,request,loaded?&*loaded:nullptr,reuse);
    const auto tx=Hex(request.transaction.bytes);const auto planDigest=Canonical(original,plan);original.receipt.transaction=request.transaction;original.receipt.plan=planDigest;
    if(fresh){original.Current();const auto created=original.io.CreateStore(original.parent,original.parentObservation,original.store,original.lock,storeObservation);
        if(created.effect!=Effect::None)original.effects=true;original.ResultOf(created);}
    Scaffold(original,tx,fresh);if(fresh){const auto attachment=AttachmentOf(original);original.Record("transactions/"+tx+"/attachment.tmp",{RecordKind::Attachment,{},0},Encode(original,attachment));}
    original.Record("transactions/"+tx+"/plan.tmp",{RecordKind::Plan,tx,0},Encode(original,plan));Journal journal(original,request.transaction,planDigest);journal.Append(m::JournalKind::Prepared);
    const auto preparing=Preparing(plan,loaded?&loaded->state:nullptr,planDigest,journal.Last());const auto preparingDigest=Canonical(original,preparing);const auto preparingBytes=Encode(original,preparing);
    original.Record("transactions/"+tx+"/state-preparing.tmp",{RecordKind::State,Hex(preparingDigest.bytes),0},preparingBytes);
    m::Selector preparingSelector{request.installation,preparing.sequence,preparingDigest};const auto preparingSelectorBytes=Encode(original,preparingSelector);
    const auto preparingObservation=Selection(original,"transactions/"+tx+"/selector-preparing.tmp",preparingSelector,loaded?&loaded->selectorObservation:nullptr,loaded?std::string_view(loaded->selectorBytes):std::string_view{},false);
    original.receipt.state=preparingDigest;original.Current();Observation staged;
    if(!reuse)staged=original.Directory("transactions/"+tx+"/payload");
    for(std::size_t i=0;i<plan.actions.size();++i){const auto& action=plan.actions[i];journal.Append(m::JournalKind::Intent,static_cast<std::uint32_t>(i));original.Current();Observation actual;
        if(action.kind==m::ActionKind::CreateDirectory)actual=original.Directory("transactions/"+tx+"/payload/"+action.path);
        else if(action.kind==m::ActionKind::StageFile){File file;static_cast<void>(original.Write("transactions/"+tx+"/payload/"+action.path,Member(request,action.path),file));original.ResultOf(original.io.Close(file));
            const auto bytes=original.Read("transactions/"+tx+"/payload/"+action.path,(std::max<std::size_t>)(Member(request,action.path).size(),1),&actual);m::Digest digest;Require(original,Hash(Bytes(bytes),digest)&&action.after&&digest==*action.after,m::Code::Hash);}
        else if(action.kind==m::ActionKind::PublishGeneration){VerifyPackage(original,request.desired,!reuse,tx);original.Current();
            if(!reuse){original.Published(original.io.PublishGeneration(original.store,original.lock,"transactions/"+tx+"/payload",staged,"packages/"+Hex(request.desired.package.bytes),PublicationRequirement::DurableNamespace));actual=staged;}
            else{for(const auto& entry:List(original,InventoryKind::Packages))if(entry.nameUtf8==Hex(request.desired.package.bytes))actual=entry.observed;
                Require(original,actual.directory&&actual.identity.length);}
        }else if(action.kind==m::ActionKind::PublishSelection){m::State accepted;accepted.installation=request.installation;Require(original,m::Advance(preparing.sequence,accepted.sequence),m::Code::Sequence);accepted.phase=m::Phase::Accepted;
            accepted.components=plan.nextComponents;accepted.retained=plan.nextRetained;accepted.pendingCleanup=plan.carryCleanup;accepted.residualReceipts=plan.carryResidualReceipts;accepted.previousAccepted=plan.expectedState;
            accepted.origin=m::PendingTransaction{request.transaction,planDigest,journal.Last()};Require(original,m::ValidatePlanResultIntent(plan,accepted,original.receipt.metadataError));
            const auto acceptedDigest=Canonical(original,accepted);original.Record("transactions/"+tx+"/state-accepted.tmp",{RecordKind::State,Hex(acceptedDigest.bytes),0},Encode(original,accepted));
            actual=Selection(original,"transactions/"+tx+"/selector-accepted.tmp",{request.installation,accepted.sequence,acceptedDigest},&preparingObservation,preparingSelectorBytes,true);original.receipt.state=acceptedDigest;
        }else throw Stop{InstallStatus::Unsupported};
        journal.Effect(static_cast<std::uint32_t>(i),reuse&&action.kind==m::ActionKind::PublishGeneration?m::Observation::Unchanged:m::Observation::Created,actual);
    }
    journal.Append(m::JournalKind::Completed);original.receipt.status=InstallStatus::Committed;
}
} // namespace


/** @summary Retained original operation; uncertain IO/release prevents reuse and never creates a runtime witness. */
struct SelectionReadTurn::Body final {
    StoreIO& io;Parent& parent;const Observation& observed;const NativeAdmission& admission;m::Identifier& installation;
    std::unique_ptr<Original> operation;std::unique_ptr<File> pending;bool faulted{};
    /** @summary Build only borrowed actual native dependencies, never portable consent flags. */
    Body(StoreIO& port,Parent& root,const Observation& observation,const NativeAdmission& scope,m::Identifier& id)
        :io(port),parent(root),observed(observation),admission(scope),installation(id){}
    /** @summary Start once against the SAME original native admission and root. */
    void Begin(const m::Component* desired=nullptr,const base_artifact::TrustedArtifact* trusted=nullptr){
        if(faulted||operation||pending)throw Stop{InstallStatus::Unknown};operation.reset(new Original{io,parent,observed,admission,installation,desired,trusted});}
    /** @summary Prove actual custody release before exposing captured observations. */
    bool End()noexcept{if(faulted)return false;if(!operation)return true;const auto result=io.ReleaseCoordination(operation->lock);if(result.status!=Status::Ok){faulted=true;return false;}operation.reset();return true;}
    /** @summary Capture one bounded original member, preserving uncertain original close custody. */
    std::string Member(std::string_view path,std::uint32_t expected){
        if(pending)throw Stop{InstallStatus::Unknown};pending=std::make_unique<File>();Observation actual;operation->Current();
        operation->ResultOf(io.OpenRead(operation->store,operation->lock,path,(std::max<std::uint32_t>)(expected,1),*pending,actual));
        Require(*operation,actual.bytes==expected,m::Code::Hash);std::string bytes(expected,'\0');std::size_t received{};
        const auto read=io.Read(*pending,std::as_writable_bytes(std::span(bytes.data(),bytes.size())),received);const auto close=io.Close(*pending);
        operation->ResultOf(read);operation->ResultOf(close);pending.reset();Require(*operation,received==bytes.size(),m::Code::Hash);return bytes;
    }
};
SelectionReadTurn::SelectionReadTurn(StoreIO& io,Parent& parent,const Observation& observed,const NativeAdmission& admission,m::Identifier& installation)
    :body_(std::make_unique<Body>(io,parent,observed,admission,installation)){}
SelectionReadTurn::~SelectionReadTurn()=default;
SelectionReadStatus SelectionReadTurn::Capture(const base_artifact::TrustedArtifact& trusted,const std::map<std::string,std::string>& executablePins,
    std::string_view version,const m::Compatibility& compatibility,SelectionSnapshotAdmission& admission)noexcept{
    auto& b=*body_;try{b.Begin();auto& original=*b.operation;Observation observed;const auto opened=b.io.OpenStore(b.parent,original.store,observed);
        if(opened.status==Status::Absent){b.operation.reset();return SelectionReadStatus::Absent;}original.ResultOf(opened);original.ResultOf(b.io.Lock(original.store,original.lock));
        const auto selector=Decode<m::Selector>(original,original.Read("current.json",m::Limits::SelectorBytes));b.installation=selector.installation;
        RootInventory(original);auto loaded=Load(original);if(loaded.unresolved||!loaded.completed)throw Stop{InstallStatus::Unknown};
        if(loaded.state.phase!=m::Phase::Accepted)throw Stop{InstallStatus::Interrupted};
        const auto found=std::find_if(loaded.state.components.begin(),loaded.state.components.end(),[](const auto& c){return c.id==m::ComponentId::Base;});Require(original,found!=loaded.state.components.end());const auto& component=*found;
        Require(original,component.package==Digest(trusted.digest)&&component.archiveBytes==trusted.size&&component.version==version&&component.files.size()==trusted.files.size());
        const auto& actual=component.compatibility;Require(original,actual.hostRoot==compatibility.hostRoot&&actual.baseContract==compatibility.baseContract&&actual.catalogueContract==compatibility.catalogueContract&&actual.sdkBundle==compatibility.sdkBundle);
        CapturedSelectionData captured;captured.selector=loaded.selector;captured.component=component;std::set<std::string_view> unique;std::uint64_t executable{},notice{};
        for(const auto& expected:trusted.files){Require(original,unique.insert(expected.path).second);const auto file=std::find_if(component.files.begin(),component.files.end(),[&](const auto& f){return f.path==expected.path;});
            Require(original,file!=component.files.end()&&file->bytes==expected.size&&file->sha256==Digest(expected.digest)&&(file->role==m::FileRole::Notice)==expected.notice);
            if(expected.notice){Require(original,expected.path=="LICENSE");notice+=expected.size;}else{const auto hash=executablePins.find(std::string(expected.path));Require(original,hash!=executablePins.end()&&hash->second==core::ToHex(expected.digest));executable+=expected.size;}
            auto bytes=b.Member(component.payloadRoot+"/"+file->path,file->bytes);m::Digest digest;Require(original,Hash(Bytes(bytes),digest)&&digest==file->sha256,m::Code::Hash);captured.members.emplace(file->path,std::move(bytes));}
        Require(original,captured.members.size()==executablePins.size()+1&&executable<=base_artifact::MaximumExecutable&&notice<=base_artifact::MaximumNotice);
        Require(original,original.Read("current.json",m::Limits::SelectorBytes)==loaded.selectorBytes);VerifyPackage(original,component,false);original.Current();
        // @summary This exact native snapshot is admitted under the SAME original coordination before releasing it.
        // Later selector changes describe a next-launch selection, never replacement of this admitted captured incarnation.
        if(!admission.Seal(std::move(captured)))throw Stop{InstallStatus::Refused};
        if(!b.End())return SelectionReadStatus::Unknown;return SelectionReadStatus::Accepted;
    }catch(const Stop& stop){if(b.pending){b.faulted=true;return SelectionReadStatus::Unknown;}if(!b.End())return SelectionReadStatus::Unknown;
        return stop.status==InstallStatus::Unknown?SelectionReadStatus::Unknown:stop.status==InstallStatus::Busy?SelectionReadStatus::Busy:stop.status==InstallStatus::Interrupted?SelectionReadStatus::Interrupted:SelectionReadStatus::Refused;}
    catch(...){if(b.pending){b.faulted=true;return SelectionReadStatus::Unknown;}if(!b.End())return SelectionReadStatus::Unknown;return SelectionReadStatus::Refused;}
}
BaseReceipt SelectionReadTurn::Install(const BaseRequest& request)noexcept{auto& b=*body_;BaseReceipt result;try{
    b.Begin(&request.desired,&request.independentlyTrusted);b.operation->receipt.transaction=request.transaction;
    try{Run(*b.operation,request);}catch(const Stop& stop){b.operation->receipt.status=stop.status;}catch(...){b.operation->receipt.status=b.operation->effects?InstallStatus::Unknown:InstallStatus::Refused;}
    result=b.operation->receipt;if(!b.End())result.status=InstallStatus::Unknown;
}catch(...){result.status=InstallStatus::Unknown;}return result;}

BaseReceipt ExecuteBase(const BaseRequest& request)noexcept{
    Original original{request.io,request.root,request.originalRoot,request.admission,request.installation,&request.desired,&request.independentlyTrusted};original.receipt.transaction=request.transaction;
    try{Run(original,request);}catch(const Stop& stop){original.receipt.status=stop.status;}
    catch(...){original.receipt.status=original.effects?InstallStatus::Unknown:InstallStatus::Refused;}return original.receipt;
}
BaseInspection InspectBase(StoreIO& io,Parent& root,const Observation& observed,const NativeAdmission& admission,const m::Identifier& installation)noexcept{
    Original original{io,root,observed,admission,installation,nullptr,nullptr};BaseInspection result;
    try{original.Current();Observation storeObservation;original.ResultOf(original.io.OpenStore(root,original.store,storeObservation));original.ResultOf(original.io.Lock(original.store,original.lock));RootInventory(original);
        auto loaded=Load(original);result.selector=std::move(loaded.selector);result.state=std::move(loaded.state);result.attachment=std::move(loaded.attachment);
        result.status=loaded.unresolved?InstallStatus::Unknown:result.state->phase==m::Phase::Preparing?InstallStatus::Interrupted:loaded.completed?InstallStatus::AlreadySelected:InstallStatus::Unknown;
    }catch(const Stop& stop){result.status=stop.status;}catch(...){result.status=InstallStatus::Refused;}
    result.last=original.receipt.last;result.metadataError=original.receipt.metadataError;return result;
}
} // namespace sc13::platform::component_store


