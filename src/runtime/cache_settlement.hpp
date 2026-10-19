#pragma once
#include "runtime/runtime_resource_cache.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>

namespace sc13::mods {class ManagerCache;class ManagerCacheAction;}
namespace sc13::runtime {
/** @summary Fixed native eviction obligations; no manager/cache ownership or production issuer. */
class CacheSettlement final {
public:
    enum class Result { Ok, Busy, Invalid, Conflict, NotFound, Capacity, Unavailable };
    enum class Phase { Prepared, CommittedPending, Unqualified, Settled, Cancelled };
    /** @summary Original weak-cache/native-owner identity, never a caller integer or permission. */
    class OwnerToken final {
        friend class CacheSettlement;
        friend class mods::ManagerCache;
        friend class mods::ManagerCacheAction;
        CacheWitness witness_;
        std::uint64_t sequence_{};
    };
    /** @summary One checked native view reference; destruction never cancels an obligation. */
    class Batch final {
        friend class CacheSettlement;
        std::uint32_t slot_{UINT32_MAX};
        std::uint64_t incarnation_{};
    public:
        Batch()=default;
        /** @summary Transfer an existing reference without reacquisition. @param other Original view. */
        Batch(Batch&& other) noexcept;
        /** @summary Release old view and transfer reference. @param other Original view. @return This view. */
        Batch& operator=(Batch&& other) noexcept;
        Batch(const Batch&)=delete;
        Batch& operator=(const Batch&)=delete;
        /** @summary Non-droppable atomic reference release, without locks or record access afterward. */
        ~Batch();
        /** @summary Releases only this view, not pending storage or authority. */
        void Reset() noexcept;
    };
    /** @summary Copied scalar facts; Busy never masquerades as zero completion. */
    struct Observation final {
        Result result{Result::Invalid};Phase phase{Phase::Prepared};
        CacheEvictionStatus eviction{CacheEvictionStatus::PendingUnavailable};
        std::uint64_t incarnation{},ownerSequence{},committedRevision{};
        std::size_t targets{},invalidated{},payloadBytes{};
    };
    /** @summary Reserve exact native targets before effects. @param owner Original native owner. @param targets Unique ordered union. @param output Empty view. @return Admission without marker acknowledgement. */
    static Result Reserve(const OwnerToken& owner,const std::set<core::Tgi>& targets,Batch& output) noexcept;
    /** @summary Rediscover only original nonterminal ownership. @param owner Original token. @param output Empty view. @return Match, contention or NotFound. */
    static Result FindPending(const OwnerToken& owner,Batch& output) noexcept;
    /** @summary Stores first nonzero marker acknowledgement immutably. @param batch Original view. @param revision Native first revision. @return Exact/idempotent acknowledgement. */
    static Result MarkCommitted(const Batch& batch,std::uint64_t revision) noexcept;
    /** @summary Cancels only a Prepared, unacknowledged obligation. @param batch Original view. @return Terminal acknowledgement, without eviction. */
    static Result CancelPrepared(const Batch& batch) noexcept;
    /** @summary Explicit synchronous eviction with caller-owned real cache lifetime. @param cache Live cache, retained for whole call. @param batch Original committed record. @return Copied actual completion or retained pending. */
    static Observation Evict(RuntimeResourceCache& cache,const Batch& batch) noexcept;
    /** @summary Try-only scalar observation with no cache lock/allocation/mutation. @param batch Original view. @return Copied facts or Busy. */
    static Observation Observe(const Batch& batch) noexcept;
private:
    friend class mods::ManagerCache;
    friend class mods::ManagerCacheAction;
    struct Record;struct Store;struct Turn;
    /** @summary Pre-acquired native marker owner; never reacquires admission after Advance. */
    class MarkerReservation final {
        friend class CacheSettlement;
        Record* record_{};
    public:
        MarkerReservation()=default;
        MarkerReservation(const MarkerReservation&)=delete;
        MarkerReservation& operator=(const MarkerReservation&)=delete;
        /** @summary Cancel only own unacknowledged Prepared record before releasing the exact turn. */
        ~MarkerReservation();
        /** @summary Store actual nonzero Advance under retained turn, allocation-free and nonfallible. @param revision Actual original writer return. */
        void Acknowledge(std::uint64_t revision) noexcept;
        /** @summary Release turn, cancelling only unacknowledged preparation; never cancels a marker. */
        void Release() noexcept;
    };
    /** @summary Atomically publish complete original Batch with its marker turn already owned. @param owner Original lineage. @param targets Immutable exact union. @param output Empty view. @param marker Empty stack owner. @return Try admission without a cancellation gap. */
    static Result ReserveMarker(const OwnerToken& owner,const std::set<core::Tgi>& targets,Batch& output,MarkerReservation& marker) noexcept;
    /** @summary Shared fixed-store reservation for ordinary views and pre-acquired native marker actions. @param owner Original lineage. @param targets Exact union. @param output Empty view. @param marker Optional empty original turn recipient. @return Complete publication or no new record. */
    static Result ReserveInternal(const OwnerToken& owner,const std::set<core::Tgi>& targets,Batch& output,MarkerReservation* marker) noexcept;
    /** @summary Sole first-write transition under an already-owned original record turn. @param record Prepared original record. @param revision Actual nonzero first revision. */
    static void AcknowledgeLocked(Record& record,std::uint64_t revision) noexcept;
    /** @summary Initializes one bounded process-lifetime store, never destroys pending records. @return Fixed store or null before effects. */
    static Store* Storage() noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    /** @summary Issue native identity only through trusted fixture ingress. @param cache Original live cache. @param output Empty owner. @return Native witness or failure. */
    static Result IssueOwner(RuntimeResourceCache& cache,OwnerToken& output) noexcept;
    /** @summary Passively copy the existing atomic action count while an unchanged checked Batch pins its slot.
     * @param batch Retained original view; must not be concurrently reset or moved.
     * @param output Cleared on invalid/unavailable input; actual instantaneous count otherwise.
     * @return Explicit validity status, never an ownership, completion or admission grant.
     * @details No turn/store lock or mutable phase access; immutable incarnation is pinned by the view reference. */
    static Result CopyActionCount(const Batch& batch,std::uint32_t& output) noexcept;
#endif
    /** @summary Compares weak original control identity even after cache destruction. */
    static bool SameOwner(const OwnerToken& left,const OwnerToken& right) noexcept;
    /** @summary Copy original record scalars under its action turn. */
    static Observation Copy(const Record& record) noexcept;
    /** @summary Acquire exact record action without global lock; valid view prevents reuse. */
    static Result Acquire(const Batch& batch,Record*& record) noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct CacheSettlementTestAccess;
#endif
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Closed native test ingress, absent from production symbols. */
struct CacheSettlementTestAccess final {
    struct Metrics {std::size_t occupied{},payloadBytes{},externalViews{},ownerBytes{},viewBytes{},recordBytes{},storeBytes{},scratchBytes{};};
    /** @summary Issue only actual original cache owner. */
    static CacheSettlement::Result IssueOwner(RuntimeResourceCache& cache,CacheSettlement::OwnerToken& output) noexcept;
    /** @summary Observe actual store charges under try admission. @param output Cleared facts. @return Busy or exact counts. */
    static CacheSettlement::Result Inspect(Metrics& output) noexcept;
    /** @summary Copy only the actual atomic action count without competing for the observed turn.
     * @param batch Retained original view, not concurrently reset or moved. @param output Actual count, zero on failure.
     * @return Explicit validity status; the copied count does not itself retain an action or grant authority. */
    static CacheSettlement::Result CopyActionCount(const CacheSettlement::Batch& batch,std::uint32_t& output) noexcept;
    /** @summary Hold actual store admission for a joined fixture callback. @param callback Borrowed joined callback. */
    static void HoldAdmission(const std::function<void()>& callback);
    /** @summary Copy original immutable payload under its actual action turn, without allocation. @param batch Original view. @param output Caller storage. @param copied Actual copied length on success, zero otherwise. @return Exact copy or rejection. */
    static CacheSettlement::Result CopyTargets(const CacheSettlement::Batch& batch,std::span<core::Tgi> output,std::size_t& copied) noexcept;
};
#endif
}
