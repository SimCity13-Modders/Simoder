/** @file @summary Bounded portable installation metadata only; decoded records never issue filesystem, maintenance or runtime authority. */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sc13::platform::component_store::metadata {
/** @summary Fixed codec ceilings; component transport policies remain distinct from native mapped-image limits. */
struct Limits final {
    static constexpr std::size_t SelectorBytes=4096, StateBytes=1048576, AttachmentBytes=16384,
        PlanBytes=1048576, JournalRecordBytes=4096, JournalRecords=2048, Depth=16,
        Components=16, Files=65, RelativePath=240, CleanupReferences=128, ResidualReceipts=128,
        ResidualEntries=2048, AggregateArrayEntries=4096, PlanActions=1023;
    static constexpr std::uint64_t BaseArchiveBytes=8388608, BasePayloadBytes=4194304,
        NoticeBytes=65536, McpArchiveBytes=17825792, NativeImageBytes=16777216, NativeManifestBytes=65536;
};
/** @summary Nonsecret refusal categories; offsets never substitute for a native witness. */
enum class Code {Ok,InputLimit,Depth,Syntax,Encoding,UnknownField,DuplicateField,MissingField,Value,
    Limit,Path,Collision,Reference,Phase,Sequence,Hash,Allocation};
/** @summary First refusal and original input byte position, without copied user text. */
struct Error final {Code code{Code::Ok};std::size_t offset{};};
/** @summary Exact digest data, never a signature, issuer or maintenance permission. */
struct Digest final {std::array<std::uint8_t,32> bytes{};bool operator==(const Digest&)const=default;};
/** @summary Logical CSPRNG identifier data; generation/consent must be supplied by the native coordinator. */
struct Identifier final {std::array<std::uint8_t,16> bytes{};bool operator==(const Identifier&)const=default;};
enum class ComponentId {Base,Mcp};
enum class FileRole {Manifest,Script,NativeImage,Notice};
enum class TrustMode {PrivatePin,SignedPublisher};
enum class Phase {Accepted,Preparing};
enum class Backend {Windows,Posix};
enum class ResidualKind {Edited,Foreign,Unreadable,Unknown};
enum class ActionKind {CreateDirectory,StageFile,PublishGeneration,PublishSelection,RemoveFile,RemoveDirectory};
enum class JournalKind {Prepared,Intent,Effect,Completed,PartialProtected,Unknown};
enum class Observation {None,Created,Unchanged,Removed,Protected,Unknown};

/** @summary One complete relative payload member; no native identity is persisted here. */
struct FileEntry final {std::string path;std::uint32_t bytes{};Digest sha256;FileRole role{FileRole::Manifest};};
/** @summary Independently selected trust metadata observations; validation does not authenticate a publisher. */
struct Trust final {TrustMode mode{TrustMode::PrivatePin};std::string source;Digest manifest;
    std::optional<std::string> publisher;std::optional<Digest> signature;};
/** @summary Declared ABI/data compatibility only; actual host negotiation still checks independently. */
struct Compatibility final {std::uint32_t hostRoot{},baseContract{},catalogueContract{};Digest sdkBundle;};
/** @summary Accepted immutable component generation; payloadRoot is exactly the redundant packages/lowercase-digest locator, never a caller-selected path. */
struct Component final {ComponentId id{ComponentId::Base};std::uint64_t generation{};Digest package;
    std::uint64_t archiveBytes{};std::string version,payloadRoot,nativeEntry;std::vector<FileEntry> files;
    Trust trust;Compatibility compatibility;std::optional<Digest> requiresBase;};
/** @summary An exact journal location derives only from a transaction ID and bounded record sequence. */
struct JournalReference final {Identifier transaction;std::uint64_t sequence{};Digest record;};
/** @summary Original finite pending transaction; no Execute/replay capability is serialized. */
struct PendingTransaction final {Identifier transaction;Digest plan;JournalReference journal;};
/** @summary Durable removal obligation retained even after component deselection. */
struct PendingCleanup final {ComponentId component{ComponentId::Base};Digest package;std::uint64_t generation{};
    Digest plan;JournalReference journal;};
/** @summary Protected residual observation; missing digest/size remains explicitly unknown, never absence. */
struct Residual final {std::string path;ResidualKind kind{ResidualKind::Unknown};
    std::optional<std::uint32_t> bytes;std::optional<Digest> sha256;};
/** @summary Explicit preserve disposition; acknowledgement is data, not proof the user actually consented. */
struct ResidualReceipt final {ComponentId component{ComponentId::Base};Digest package;std::uint64_t generation{};
    Digest plan;JournalReference journal;bool preserveAcknowledged{};std::vector<Residual> residuals;};
/** @summary A generation key contains logical observations only, never a VM/native generation lease. */
struct GenerationKey final {ComponentId component{ComponentId::Base};Digest package;std::uint64_t generation{};
    bool operator==(const GenerationKey&)const=default;};
/** @summary Previously accepted unselected content; its map is in an already existing accepted state, never the state containing this reference. */
struct RetainedGeneration final {GenerationKey key;Digest acceptedState;};
/** @summary Immutable selection state, with obligations separate from successful component selection. */
struct State final {Identifier installation;std::uint64_t sequence{};Phase phase{Phase::Accepted};
    std::vector<Component> components;std::optional<Digest> previousAccepted;
    std::optional<PendingTransaction> pendingTransaction;std::vector<PendingCleanup> pendingCleanup;
    std::vector<ResidualReceipt> residualReceipts;std::vector<RetainedGeneration> retained;
    std::optional<PendingTransaction> origin;};
/** @summary Small selector names exactly one immutable state and sequence, never the newest directory. */
struct Selector final {Identifier installation;std::uint64_t sequence{};Digest state;};
/** @summary Local attachment facts kept separate from portable package identity and from native handles. */
struct Attachment final {Identifier installation;Backend backend{Backend::Windows};
    std::string root,account,volumeIdentity,objectIdentity;};
/** @summary Finite logical effect intent with optional expected/next content observations. */
struct PlanAction final {ActionKind kind{ActionKind::StageFile};std::optional<ComponentId> component;
    std::optional<Digest> package;std::string path;std::optional<Digest> before,after;};
/** @summary Portable plan references source/next selection without persisting absolute native paths or quiescence. */
struct TransactionPlan final {Identifier installation,transaction;std::uint64_t expectedSequence{};
    std::optional<Digest> expectedState;std::vector<Component> nextComponents;std::vector<PlanAction> actions;
    std::vector<PendingCleanup> carryCleanup;std::vector<ResidualReceipt> carryResidualReceipts;
    std::vector<RetainedGeneration> nextRetained;std::vector<GenerationKey> newCleanup,acknowledgeResiduals;};
/** @summary One bounded logical append record; backend evidence is a digest reference, never deserialized OS ownership. */
struct JournalRecord final {Identifier installation,transaction;std::uint64_t sequence{};Digest plan;
    std::optional<Digest> previous;JournalKind kind{JournalKind::Prepared};
    std::optional<std::uint32_t> action;Observation observation{Observation::None};
    std::optional<Digest> backendEvidence;};

/** @summary Validate the existing ASCII slash-relative profile, including DOS names/ADS/aliases; empty is allowed only explicitly. */
[[nodiscard]] bool RelativePathValid(std::string_view path,bool allowEmpty=false) noexcept;
/** @summary Decode canonical lowercase nonzero digest/identifier syntax without issuing trust. */
[[nodiscard]] bool ParseDigest(std::string_view text,Digest& output) noexcept;
[[nodiscard]] bool ParseIdentifier(std::string_view text,Identifier& output) noexcept;
/** @summary Validate complete component/profile constraints and case-insensitive file-map collisions. */
[[nodiscard]] bool Validate(const Component& value,Error& error) noexcept;
/** @summary Validate all state-local dependencies, pending references and protected residual coherence. */
[[nodiscard]] bool Validate(const State& value,Error& error) noexcept;
[[nodiscard]] bool Validate(const Selector& value,Error& error) noexcept;
[[nodiscard]] bool Validate(const Attachment& value,Error& error) noexcept;
[[nodiscard]] bool Validate(const TransactionPlan& value,Error& error) noexcept;
[[nodiscard]] bool Validate(const JournalRecord& value,Error& error) noexcept;

/** @summary Strict bounded UTF-8 JSON decode; unknown/duplicate/missing fields refuse and output is unchanged on failure. */
[[nodiscard]] bool Decode(std::string_view input,Selector& output,Error& error) noexcept;
[[nodiscard]] bool Decode(std::string_view input,State& output,Error& error) noexcept;
[[nodiscard]] bool Decode(std::string_view input,Attachment& output,Error& error) noexcept;
[[nodiscard]] bool Decode(std::string_view input,TransactionPlan& output,Error& error) noexcept;
[[nodiscard]] bool Decode(std::string_view input,JournalRecord& output,Error& error) noexcept;
/** @summary Deterministic canonical encode; caller output is unchanged on refusal/allocation failure. */
[[nodiscard]] bool Encode(const Selector& value,std::string& output,Error& error) noexcept;
[[nodiscard]] bool Encode(const State& value,std::string& output,Error& error) noexcept;
[[nodiscard]] bool Encode(const Attachment& value,std::string& output,Error& error) noexcept;
[[nodiscard]] bool Encode(const TransactionPlan& value,std::string& output,Error& error) noexcept;
[[nodiscard]] bool Encode(const JournalRecord& value,std::string& output,Error& error) noexcept;
/** @summary Domain/length-framed SHA-256 of canonical bytes using the existing Core SHA implementation; no filesystem I/O. */
[[nodiscard]] bool CanonicalDigest(const State& value,Digest& output,Error& error) noexcept;
[[nodiscard]] bool CanonicalDigest(const TransactionPlan& value,Digest& output,Error& error) noexcept;
[[nodiscard]] bool CanonicalDigest(const JournalRecord& value,Digest& output,Error& error) noexcept;
/** @summary Check selector/state content and logical identity; no attachment/current-root or publisher authorization is implied. */
[[nodiscard]] bool ValidateSelection(const Selector& selector,const State& state,Error& error) noexcept;
/** @summary Bind acyclic selection intent and existing obligations to its independently supplied prior accepted state. */
[[nodiscard]] bool ValidatePlanAgainstPrior(const TransactionPlan& plan,const State* prior,Error& error) noexcept;
/** @summary Verify final intent/checked sequence/previous selection and preserved or explicitly acknowledged cleanup obligations. */
[[nodiscard]] bool ValidatePlanResultIntent(const TransactionPlan& plan,const State& result,Error& error) noexcept;
/** @summary Check one original journal chain/plan without executing effects or interpreting native evidence. */
[[nodiscard]] bool ValidateJournal(std::span<const JournalRecord> records,const TransactionPlan& plan,Error& error) noexcept;
/** @summary Checked monotonic advance; maximum values never wrap or become zero-as-latest. */
[[nodiscard]] bool Advance(std::uint64_t value,std::uint64_t& output) noexcept;
}
