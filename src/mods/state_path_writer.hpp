#pragma once
#include "mods/mod_state_store.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace sc13::mods {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
struct StatePathWriterTestAccess;
#endif
/** @summary Closed cooperative native writer; no production Owner issuer or script filesystem access.
 * @pre Explicit action callers own the native lifetime and hold no D/M/S/V/R/B/Q/T/current borrower guard. View replacement/final disposal occurs outside those guards. */
class StatePathWriter final {
    struct OwnerState;struct Record;struct Native;
public:
    /** @summary Admission/protocol result without converting current observations into historical success. */
    enum class Kind {Ok,Busy,Rejected,Conflict,Published,Compensated,Unknown,Closed};
    /** @summary Exact native owner's retained admission state. */
    enum class OwnerPhase {Open,Quarantined,Closed};
    /** @summary Current original operation stage, including outstanding deletion acknowledgement. */
    enum class Phase {Prepared,TemporaryOwned,Written,Publishing,Published,CleaningTemporary,CleanupDeletePending,CompensatingRestore,CompensatingDeletePending,Rejected,CompensationFailed,Compensated,Unknown,Cancelled,Settled};
    /** @summary Retained protocol history; confirmed fact booleans remain monotonic. */
    enum class History {Prepared,Published,Rejected,CompensationFailed,Compensated,Unknown,Cancelled};
    /** @summary Fresh native default-stream observation, never inferred from history. */
    enum class Current {Unobserved,MatchesCandidate,MatchesRestoration,ExpectedAbsent,Conflict,Unknown,OwnerClosed};
    /** @summary Original temporary/deletion evidence, separate from publication. */
    enum class Cleanup {None,StillTemporary,DispositionRequested,AbsenceConfirmed,Conflict,Unknown};
    /** @summary Classified publication/cleanup effect; temporary creation/write counts are separate. */
    enum class EffectKind {None,PublishRename,RestoreRename,RemovePublishedCandidate,RemoveTemporary};
    /** @summary Immediately retained return of the last classified native effect. */
    enum class NativeReturn {NotCalled,Succeeded,Failed,Pending};
    /** @summary Selected rename backend; native status and derived DOS error are separate evidence domains. */
    enum class RenameBackend {None,NtdllParentRelative};
    /** @summary Copied scalar native rename evidence, never a pointer into retained request storage.
     * @details nativeError remains the actual last Win32 operation error. mappedError is only an explicitly valid RtlNtStatusToDosError conversion. */
    struct RenameEvidence {
        RenameBackend backend{RenameBackend::None};EffectKind effect{EffectKind::None};
        std::uint64_t submission{};
        std::uint32_t callStatus{},finalStatus{},mappedError{},submittedLength{},waitResult{},waitError{},information{},setInformationRva{},mapErrorRva{};
        bool callValid{},finalValid{},mappedErrorValid{},outstanding{},completionAcknowledged{},informationValid{},exportsQualified{},waitAttempted{};
    };
    static_assert(sizeof(RenameEvidence)<=64);
    /** @summary Actual observation/parser classification; unavailable never means absence. */
    enum class QueryDisposition {NotQueried,Confirmed,Conflict,Unavailable,Malformed,Unsupported,Oversize};
    /** @summary Opaque original owner/operation/pool incarnation; scalars cannot mint one. */
    class OperationIdentity final {friend class StatePathWriter;std::uint64_t owner_{},sequence_{},incarnation_{};public:bool operator==(const OperationIdentity&)const=default;};
    /** @summary Copied native file identity; valid flag is mandatory proof. */
    struct FileIdentity {std::uint64_t volume{};std::array<unsigned char,16> file{};bool valid{};bool operator==(const FileIdentity&)const=default;};
    /** @summary Fixed native protocol outcome, distinct historical/current/effect/query facts.
     * @details nativeError contains only recorded Win32 diagnostics; native rename never writes a synthesized GetLastError into it. rename contains the independent native and translated domains. */
    struct Result {
        Kind kind{Kind::Rejected};Phase phase{Phase::Prepared};OwnerPhase ownerPhase{OwnerPhase::Open};OperationIdentity operation;
        std::uint64_t ownerGeneration{},operationSequence{},compensationSequence{},latestEffectSequence{};
        History history{History::Prepared};Current current{Current::Unobserved};Cleanup cleanup{Cleanup::None};EffectKind lastEffectKind{EffectKind::None};NativeReturn lastNativeReturn{NativeReturn::NotCalled};
        std::uint32_t nativeError{},observationError{},temporaryCreateCalls{},temporaryWriteCalls{},publicationCalls{},compensationCalls{},cleanupDispositionCalls{};std::uint64_t elapsedBackoffMs{};
        QueryDisposition observationDisposition{QueryDisposition::NotQueried};bool fixtureObservationUnavailable{},settled{},publicationConfirmed{},compensationConfirmed{};
        RenameEvidence rename;
    };
    /** @summary Fresh native observation; no implicit effect or settlement. */
    struct Observation {Result result;FileIdentity parent,destination,owned;bool candidateBytesMatch{},restorationBytesMatch{},priorAbsent{},defaultStreamOnly{};};
    /** @summary Copyable original native owner; final destruction must occur outside compound guards. */
    class Owner {friend class StatePathWriter;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        friend struct StatePathWriterTestAccess;
#endif
        std::shared_ptr<OwnerState> state_;public:explicit operator bool()const noexcept{return bool(state_);}};
    /** @summary Exclusive move-only native admission; release performs no filesystem work. */
    class Turn {friend class StatePathWriter;std::shared_ptr<OwnerState> state_;public:Turn()=default;Turn(const Turn&)=delete;Turn&operator=(const Turn&)=delete;Turn(Turn&&)noexcept;Turn&operator=(Turn&&)noexcept;~Turn();};
    /** @summary Original prepared record view; dropping it cannot cancel an owner slot. */
    class Intent {friend class StatePathWriter;std::shared_ptr<OwnerState> owner_;std::shared_ptr<Record> record_;};
    /** @summary Original attempted record view; history persists after settlement/close. */
    class Receipt {friend class StatePathWriter;std::shared_ptr<OwnerState> owner_;std::shared_ptr<Record> record_;};
    /** @summary Copied retained-slot metadata; native pending state never authorizes handle inspection or release. */
    struct OperationSummary {OperationIdentity operation;Phase phase{};History history{};Cleanup cleanup{};bool settled{},nativeRenameOutstanding{};std::size_t rawBytesReserved{};};
    struct OperationList {Result result;std::array<OperationSummary,2> operations{};std::size_t count{};};
    /** @summary Try original native admission, with no I/O. @param owner Retained owner. @param output Empty turn. @return Native admission result. */
    static Result TryTurn(const Owner& owner,Turn& output) noexcept;
    /** @summary Reserve buffers/capture prior/default bytes before filesystem effects. @param turn Original owner turn. @param enabled Valid IDs. @param output Empty intent. @return Exact preparation outcome. */
    static Result Prepare(Turn& turn,std::span<const ModId> enabled,Intent& output) noexcept;
    /** @summary Attempt original publication once. @param turn Original turn. @param intent Original prepared record. @param output Empty receipt. @return Native result. */
    static Result Publish(Turn& turn,Intent& intent,Receipt& output) noexcept;
    /** @summary Attempt exact latest conditional compensation once. @param turn Original turn. @param receipt Original receipt. @return Native result. */
    static Result Compensate(Turn& turn,const Receipt& receipt) noexcept;
    /** @summary Read actual current file facts without effects. @param turn Original live turn. @param receipt Original receipt. @return Historical/current evidence. */
    static Observation Observe(Turn& turn,const Receipt& receipt) noexcept;
    /** @summary Copy retained ledger through same try-only scalar gate, even after owner closes. @param receipt Original view. @return Busy or history; never fresh filesystem assertion. */
    static Result HistoryView(const Receipt& receipt) noexcept;
    /** @summary Reconcile original effect/cleanup without replaying ambiguous effects; sole explicit zero-wait native completion action. @param turn Original turn. @param operation Exact opaque identity. @return Native evidence, retaining Unknown and all resources if completion remains unqualified. */
    static Result Reconcile(Turn& turn,const OperationIdentity& operation) noexcept;
    /** @summary Acknowledge known terminal operation, retaining charge through external views. @param turn Original turn. @param operation Exact identity. @return Native outcome. */
    static Result Settle(Turn& turn,const OperationIdentity& operation) noexcept;
    /** @summary Cancel only untouched Prepared ownership. @param turn Original turn. @param operation Exact identity. @return Native outcome. */
    static Result CancelPrepared(Turn& turn,const OperationIdentity& operation) noexcept;
    /** @summary Copy at most two original slots without I/O. @param turn Original turn. @return Fixed inventory. */
    static OperationList Operations(Turn& turn) noexcept;
    /** @summary Explicitly close only an owner with no unresolved slots. @param owner Original owner. @return Close result. */
    static Result CloseOwner(Owner& owner) noexcept;
private:
    friend class ManagerPublication;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct StatePathWriterTestAccess;
#endif
    static Result Open(const std::filesystem::path&,Owner&) noexcept;
    static Result Snapshot(const OwnerState&,const Record&) noexcept;
    static std::shared_ptr<Record> Find(OwnerState&,const OperationIdentity&) noexcept;
    static Observation Inspect(OwnerState&,Record&) noexcept;
    /** @summary Resume original proof/cleanup; generic callers cannot consume outstanding native completion. @param owner Original owner. @param record Original record. @param explicitCompletion True only from public explicit Reconcile. @return Original protocol outcome. */
    static Result ReconcileRecord(OwnerState& owner,Record& record,bool explicitCompletion=false) noexcept;
    static Result Temporary(OwnerState&,Record&,bool) noexcept;
    static Result CleanupTemporary(OwnerState&,Record&) noexcept;
    static bool Qualify(const std::filesystem::path&,OwnerState&) noexcept;
    static bool InvokeCut(unsigned,OwnerState&,Record&,bool&) noexcept;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Closed native logical cuts: callback cannot supply syscall outcomes, bytes or identities. */
struct StatePathWriterTestAccess {
    enum class Cut {BeforeTempCreate,AfterTempIdentity,AfterTempVerified,BeforePublicationEffect,AfterPublicationEffect,BeforeCompensationValidation,AfterDispositionEffect,BeforeTemporaryCleanupValidation};
    enum class CutAction {Continue,SuppressNextObservation};
    struct CutObservation {Cut cut{};StatePathWriter::Result result;StatePathWriter::FileIdentity owned;std::array<wchar_t,256> parentLeaf{};std::array<wchar_t,129> temporaryLeaf{};};
    /** @summary Actual fixed metadata and raw-pool sizes, with no fixture-supplied counters. */
    struct Metrics {std::size_t ownerPoolOccupied{},globalOperationPoolOccupied{},rawBytesReserved{},thisOwnerSlotCount{},ownerBytes{},recordBytes{},nativeRenameContextBytes{},renameEvidenceBytes{};};
    /** @summary Copied loaded-system export provenance for an independent analysis-only host inspector.
     * @details modulePath is the checked OS-derived physical file; mappedPath is its actual native device mapping. FileIdentity is copied metadata, never an issuer. Addresses identify only OS image exports; no original source/parent handle, input buffer or IOSB address is exposed. File hash/version remain separate inspector evidence. */
    struct NativeExportObservation {StatePathWriter::Kind kind{StatePathWriter::Kind::Rejected};std::uintptr_t moduleBase{},setInformationAddress{},mapErrorAddress{};std::uint32_t imageSize{},setInformationRva{},mapErrorRva{};std::array<wchar_t,4097> modulePath{},mappedPath{};StatePathWriter::FileIdentity systemFileIdentity;bool qualified{};};
    /** @summary Issue isolated qualified owner only in test host. @param path Test-owned local NTFS path. @param owner Empty output. @return Native result. */
    static StatePathWriter::Result Open(const std::filesystem::path& path,StatePathWriter::Owner& owner) noexcept;
    /** @summary Install a TLS native fixture hook outside guards. @param callback Logical cut only. */
    static void Hook(std::function<CutAction(const CutObservation&)> callback);
    /** @summary Copy actual fixed pool charges. @param owner Original owner. @return Actual metrics. */
    static Metrics Inspect(const StatePathWriter::Owner& owner) noexcept;
    /** @summary Copy actual qualified loaded-system module/path/export facts before effects, under original try-only admission. @param owner Original qualified owner. @return Rejected/Busy/Unknown or exact qualified image facts; pending never triggers inspection. */
    static NativeExportObservation ExportProvenance(const StatePathWriter::Owner& owner) noexcept;
    /** @summary Exercise only the bounded native stream-data parser, never inject a filesystem query result. @param bytes Copied FILE_STREAM_INFO chain. @return Parser disposition. */
    static StatePathWriter::QueryDisposition ParseStreams(std::span<const std::byte> bytes) noexcept;
};
#endif
}
