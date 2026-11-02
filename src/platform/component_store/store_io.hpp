/** @file @summary Neutral non-authorizing component-store IO boundary; opaque owners are bound to one explicitly selected native backend instance. */
#pragma once
#include "platform/component_store/metadata.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>


/** @summary Preserve caller Windows macros while declaring portable operation names unchanged. */
#pragma push_macro("CreateFile")
#pragma push_macro("CreateDirectory")
#undef CreateFile
#undef CreateDirectory

namespace sc13::platform::component_store {
/** @summary Actual local operation disposition, independently of installer policy or runtime activation. */
enum class Status {Ok,Absent,Busy,Conflict,Invalid,Unsupported,IoError,Unknown};
/** @summary Known physical effect; Unknown forbids automatic retry or inferred rollback. */
enum class Effect {None,Created,Published,Removed,Unknown};
/** @summary Persistence boundary actually established; visibility is not power-loss durability. */
enum class Durability {None,FileFlushed,NamespaceUnqualified,NamespaceAcknowledged};
/** @summary Exact low-level outcome; nativeError is local diagnostic metadata and never an authority decision. */
struct Result final {Status status{Status::Invalid};Effect effect{Effect::None};Durability durability{Durability::None};std::uint32_t nativeError{};};
/** @summary Local backend object correlation for current-operation race checks, never persistent portable ownership. */
struct ObjectIdentity final {
    std::array<std::byte,48> bytes{};std::uint32_t length{};
    /** @summary Compare exact opaque local correlations; matching bytes confer no authority. */
    bool operator==(const ObjectIdentity&)const noexcept=default;
};
/** @summary Ordinary current object metadata captured from its original retained handle. */
struct Observation final {ObjectIdentity identity;std::uint64_t bytes{};bool directory{};};
/** @summary Required namespace acknowledgement; weaker requirements must never be substituted silently. */
enum class PublicationRequirement {AtomicVisibility,DurableNamespace};
/** @summary Closed immutable metadata namespaces; identifiers are locators, never record trust or ownership. */
enum class RecordKind {Attachment,State,Plan,Journal,Evidence};
/** @summary Synchronous derived record target; only Journal/Evidence use nonzero sequence1..2048. */
struct RecordTarget final {RecordKind kind;std::string_view identity;std::uint32_t sequence{};};
/** @summary Closed nonrecursive native maintenance namespaces; arbitrary payload/configuration traversal is unavailable. */
enum class InventoryKind {Root,States,Packages,Transactions,Configuration,Transaction,Journal,Evidence,Package,StagedPackage};
/** @summary Transaction scopes use32-lowerhex identity; Package uses64-lowerhex. Only payload scopes admit relative240/depth16. */
struct InventoryTarget final {InventoryKind kind;std::string_view identity;std::string_view payloadRelative{};};
/** @summary Actual ordinary current entry name/type/identity; observation never adopts the entry or supplies portable authority. */
struct NamedObservation final {std::string nameUtf8;Observation observed;};

class WindowsStore;
class WindowsStoreIO;
class StoreIO;
/** @summary Retained ordinary selected parent and every ordinary ancestor; destruction closes handles only, never deletes paths. */
class Parent final {
public:
    /** @summary Initialize empty retained storage with no filesystem effects. */
    Parent()noexcept;
    /** @summary Close original local handles; no installation state or filesystem object is removed. */
    ~Parent();
    Parent(const Parent&)=delete;Parent& operator=(const Parent&)=delete;
private:friend class WindowsStore;friend class WindowsStoreIO;struct State;std::unique_ptr<State> state_;const StoreIO* origin_{};
};
/** @summary Exclusive fixed-root coordination; releasing local kernel handles never deletes the lock file. */
class Coordination final {
public:
    /** @summary Initialize empty original lock storage without acquisition. */
    Coordination()noexcept;
    /** @summary Release this original kernel ownership and close local handles only. */
    ~Coordination();
    Coordination(const Coordination&)=delete;Coordination& operator=(const Coordination&)=delete;
private:friend class WindowsStore;friend class WindowsStoreIO;struct State;std::unique_ptr<State> state_;const StoreIO* origin_{};
};
/** @summary Retained reserved tree plus ordinary ancestry; opening it does not adopt unknown installer content. */
class Store final {
public:
    /** @summary Initialize empty retained storage without creating reserved paths. */
    Store()noexcept;
    /** @summary Close original local handles without recursive cleanup or foreign deletion. */
    ~Store();
    Store(const Store&)=delete;Store& operator=(const Store&)=delete;
private:friend class WindowsStore;friend class WindowsStoreIO;struct State;std::unique_ptr<State> state_;const StoreIO* origin_{};
};
/** @summary Same original exclusively created or bounded-read file; no external raw handle or cleanup authority escapes. */
class File final {
public:
    /** @summary Initialize empty file custody without IO. */
    File()noexcept;
    /** @summary Close this original synchronous file handle; never delete an entry on destruction. */
    ~File();
    File(const File&)=delete;File& operator=(const File&)=delete;
private:friend class WindowsStore;friend class WindowsStoreIO;struct State;std::unique_ptr<State> state_;const StoreIO* origin_{};
};
/** @summary Data-only evidence context; identifiers and observations do not mint IO, consent or publisher authority. */
struct EvidenceContext final {metadata::Identifier installation,transaction;std::uint64_t sequence{};metadata::Observation observation{metadata::Observation::None};};
/** @summary One explicitly selected native platform implementation; no default backend, runtime factory or unsupported POSIX facade.
 * @remarks The caller MUST retain this same object through every Parent/Store/Coordination/File lifetime.
 * Native provenance checks refuse cross-instance/backend owner mixing. Metadata observes portable data only;
 * platform implementations own local attachment/evidence syntax and qualification of their namespace acknowledgement. */
class StoreIO {
public:
    /** @summary Construct an inert interface without selecting or starting a backend. */
    StoreIO()noexcept=default;
    /** @summary End only after all original opaque owners have ended; no automatic installation cleanup is performed. */
    virtual ~StoreIO()=default;
    StoreIO(const StoreIO&)=delete;StoreIO& operator=(const StoreIO&)=delete;
    /** @summary Describe the actual implementation, not a serialized backend selection or authority. */
    [[nodiscard]] virtual std::string_view BackendDescription()const noexcept=0;
    /** @summary Describe the actual qualified publication guarantee; never a universal power-loss promise. */
    [[nodiscard]] virtual std::string_view PublicationDescription()const noexcept=0;
    /** @summary Interpret an actual native publication result under this backend's qualified requested boundary. */
    [[nodiscard]] virtual bool PublicationAcknowledged(const Result&,PublicationRequirement)const noexcept=0;
    /** @summary Capture fresh local attachment data through actual backend roots/accounts, without a credential-store issuer. */
    virtual Result CaptureAttachment(Parent&,const metadata::Identifier&,metadata::Attachment&)noexcept=0;
    /** @summary Encode exact local observation/context as historical data, not an executable native witness. */
    virtual Result EncodeLocalEvidence(const EvidenceContext&,const Observation&,std::string&)const noexcept=0;
    /** @summary Validate portable historical opaque evidence without interpreting old IDs as current OS ownership. */
    [[nodiscard]] virtual bool ValidateHistoricalEvidence(std::string_view,const EvidenceContext&)const noexcept=0;
    /** @summary Open the independently selected actual ordinary parent and bind its original native provenance. */
    virtual Result OpenParent(std::string_view,Parent&,Observation&)noexcept=0;
    /** @summary Retain the same fixed original kernel-backed coordination. */
    virtual Result Lock(Store&,Coordination&)noexcept=0;
    /** @summary Positively release the original coordination only after every dependent file lease closed.
     * @remarks Same-owner calls are externally serialized. A genuinely empty owner is Ok/None, not proof about any foreign lock or prior Unknown effect.
     * Busy preserves the unattempted lock; uncertain closure retains custody and forbids replay. */
    virtual Result ReleaseCoordination(Coordination&)noexcept=0;
    /** @summary Exclusively create the fixed reserved root only against its admitted original parent. */
    virtual Result CreateStore(Parent&,const Observation&,Store&,Coordination&,Observation&)noexcept=0;
    /** @summary Open existing reserved storage without adopting its contents or installation history. */
    virtual Result OpenStore(Parent&,Store&,Observation&)noexcept=0;
    /** @summary Read a bounded closed namespace; output on failure is never a complete inventory. */
    virtual Result ListDirectory(Store&,Coordination&,InventoryTarget,std::uint32_t,std::vector<NamedObservation>&)noexcept=0;
    /** @summary Create one absent derived ordinary directory; never adopt a collision. */
    virtual Result CreateDirectory(Store&,Coordination&,std::string_view,Observation&)noexcept=0;
    /** @summary Open and retain one bounded original ordinary file without following aliases. */
    virtual Result OpenRead(Store&,Coordination&,std::string_view,std::uint64_t,File&,Observation&)noexcept=0;
    /** @summary Read only this original file into bounded caller storage. */
    virtual Result Read(File&,std::span<std::byte>,std::size_t&)noexcept=0;
    /** @summary Exclusively create a new original file and retain its native identity. */
    virtual Result CreateFile(Store&,Coordination&,std::string_view,std::uint64_t,File&,Observation&)noexcept=0;
    /** @summary Write bounded bytes through the same original created handle. */
    virtual Result Write(File&,std::span<const std::byte>)noexcept=0;
    /** @summary Flush content without substituting it for namespace acknowledgement. */
    virtual Result Flush(File&)noexcept=0;
    /** @summary Close the exact original; ambiguous failure preserves provenance and never proves release. */
    virtual Result Close(File&)noexcept=0;
    /** @summary Publish one immutable original into a closed derived absent record target. */
    virtual Result PublishRecord(Store&,Coordination&,File&,RecordTarget,PublicationRequirement)noexcept=0;
    /** @summary Publish the same verified staged directory without replacement/copy fallback. */
    virtual Result PublishGeneration(Store&,Coordination&,std::string_view,const Observation&,std::string_view,PublicationRequirement)noexcept=0;
    /** @summary Conditionally publish the original selector against the exact original admitted preimage. */
    virtual Result PublishSelection(Store&,Coordination&,File&,const Observation*,std::span<const std::byte>,PublicationRequirement)noexcept=0;
    /** @summary Remove only a known originally created empty directory under the native backend's actual checks. */
    virtual Result RemoveCreatedDirectory(Store&,Coordination&,std::string_view,const Observation&)noexcept=0;
};
}

#pragma pop_macro("CreateDirectory")
#pragma pop_macro("CreateFile")
