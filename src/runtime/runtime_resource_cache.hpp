#pragma once

#include "core/runtime_property_table.hpp"
#include "core/tgi.hpp"
#include "mods/mod_types.hpp"

#include <compare>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
#include <functional>
#endif

namespace sc13::mods {class ManagerCache;}
namespace sc13::runtime {
/** @summary Original cache lifetime, privately issued and weakly observed. */
struct CacheControl;
class CacheSettlement;
/** @summary Original native cache/reset identity; never retains a cache or authorizes work. */
class CacheWitness final {
    friend class RuntimeResourceCache;
    friend class CacheSettlement;
    friend class mods::ManagerCache;
    std::weak_ptr<CacheControl> control_;
    std::uint64_t nonce_{},reset_{};
};
/** @summary Distinguishes complete eviction from metadata/identity failure. */
enum class CacheEvictionStatus {PendingAllocation,PendingUnavailable,PendingResetMismatch,PendingEpochExhausted,Evicted,EvictedDiagnosticPending};
/** @summary Scalar actual eviction result; count is authoritative only on complete phases. */
struct CacheEvictionResult {CacheEvictionStatus status{CacheEvictionStatus::PendingUnavailable};std::size_t invalidated{};};

/** @summary Returns copied historical float evidence without exposing native instance addresses. */
struct RuntimeFloatObservation final {
    float value{};
    float vanilla{};
    std::uint64_t observedAtUnixMs{};
    mods::PatchGeneration generation{};
    std::string status;
    std::size_t instances{};
};

/** Uniquely identifies one game-owned runtime resource instance. */
struct RuntimeResourceKey final {
    core::Tgi target{};
    std::uintptr_t resourceAddress{};

    auto operator<=>(const RuntimeResourceKey&) const = default;
};

/** Classifies how a prepared generation relates to the retained resource state. */
enum class RuntimeResourceBuildStatus {
    Created,
    Rebuilt,
    Reused,
    Restored
};

/** Owns all data needed for a transactional game-memory update. */
struct RuntimeResourceBuildPlan final {
    RuntimeResourceKey key{};
    mods::PatchGeneration generation{};
    CacheWitness cacheWitness;
    std::uint64_t invalidationEpoch{};
    RuntimeResourceBuildStatus status{RuntimeResourceBuildStatus::Created};
    std::vector<core::RuntimePropertyRecord> vanilla;
    std::vector<core::RuntimePropertyRecord> previous;
    std::vector<core::RuntimePropertyRecord> desired;
};

/** Copies retained values needed to restore one live resource during controlled detach. */
struct RuntimeResourceRestoreEntry final {
    RuntimeResourceKey key{};
    std::vector<core::RuntimePropertyRecord> vanilla;
    std::vector<core::RuntimePropertyRecord> lastApplied;
};

/** @summary Captures observed resource outcomes; successful cache data is historical, not a live pointer read. */
struct RuntimeResourceObservation final {
    RuntimeResourceKey key{};
    mods::PatchGeneration generation{};
    std::string status;
    std::string diagnostic;
    std::vector<core::RuntimePropertyRecord> vanilla;
    std::vector<core::RuntimePropertyRecord> applied;
    std::uint64_t observedAtUnixMs{};
    /** @summary Distinguishes the last successful value snapshot from a later rejected attempt. */
    mods::PatchGeneration committedGeneration{};
};

/** Retains reproducible vanilla-derived resource generations without lending pointers to the game. */
class RuntimeResourceCache final {
public:
    /** @summary Creates a nonreused native lifetime identity before usable cache admission. */
    RuntimeResourceCache();
    /** @summary Closes the weak witness without certifying game-consumer quiescence. */
    ~RuntimeResourceCache();
    /** Builds a candidate result from retained vanilla plus the supplied ordered patches. */
    [[nodiscard]] bool Prepare(
        const RuntimeResourceKey& key,
        std::span<const core::RuntimePropertyRecord> current,
        mods::PatchGeneration generation,
        std::span<const mods::RegisteredPatch> patches,
        RuntimeResourceBuildPlan& plan,
        std::string& error) noexcept;

    /** Commits one plan only after the caller has atomically written every desired value. */
    [[nodiscard]] bool Commit(
        const RuntimeResourceBuildPlan& plan,
        std::string& error) noexcept;

    /** Marks only entries for affected TGIs stale and returns their count. */
    [[nodiscard]] std::size_t Invalidate(
        const std::set<core::Tgi>& affected) noexcept;
    /** @summary Execute the existing eviction while distinguishing failure from a valid zero-count result.
     * @param affected Exact affected targets. @param invalidated Receives count on complete success, zero on failure.
     * @return True only after every requested target was marked pending; partial failure never implies rollback. */
    [[nodiscard]] bool InvalidateChecked(const std::set<core::Tgi>& affected,std::size_t& invalidated) noexcept;

    /** Reports whether one game-owned instance has retained vanilla state. */
    [[nodiscard]] bool Contains(const RuntimeResourceKey& key) const;

    /** Returns the number of retained game-owned resource identities. */
    [[nodiscard]] std::size_t size() const;

    /** Returns owned restoration snapshots without exposing internal cache storage. */
    [[nodiscard]] std::vector<RuntimeResourceRestoreEntry> SnapshotForRestore() const;
    /** @summary Copies only affected vanilla tables so script updates avoid scanning/copying unrelated payloads. */
    [[nodiscard]] std::vector<RuntimeResourceRestoreEntry> SnapshotForTargets(const std::set<core::Tgi>& targets) const;
    /** @summary Records an observed rejection without retaining a dereferenceable game pointer. */
    void RecordFailure(const RuntimeResourceKey& key, mods::PatchGeneration generation,
        std::string_view reason) noexcept;
    /** @summary Returns bounded historical evidence for UI and external tools. */
    [[nodiscard]] std::vector<RuntimeResourceObservation> Observations() const;
    /** @summary Reads one bounded historical field and rejects inconsistent values across observed instances. */
    [[nodiscard]] std::optional<RuntimeFloatObservation> ReadObservedFloat(
        const core::Tgi& target, std::uint32_t property) const;

    /** Releases loader-owned snapshots during controlled shutdown. */
    void Clear() noexcept;

private:
    friend class CacheSettlement;
    friend class mods::ManagerCache;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    /** @summary Bind one exact original native State for this cache lifetime, never reset by Clear. @param state Original State control-block identity. @return Exact first/idempotent bind. */
    bool BindCanonicalState(const std::shared_ptr<const void>& state);
#endif
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct RuntimeResourceCacheTestAccess;
#endif
    /** @summary Capture live original witness under actual cache mutex. */
    CacheWitness Witness() const;
    /** @summary Validate exact original control/reset under already-held cache mutex. */
    bool MatchesLocked(const CacheWitness& witness) const noexcept;
    /** @summary Shared preallocate/check/evict/diagnostic algorithm; caller owns real cache lifetime. */
    template<class Range> CacheEvictionResult EvictTargets(const Range& targets,const CacheWitness* expected) noexcept;
    /** @summary Evict immutable bounded original payload, never rebuild registry or game state. */
    CacheEvictionResult EvictOriginal(std::span<const core::Tgi> targets,const CacheWitness& expected) noexcept;
    std::shared_ptr<CacheControl> control_;
    /** Stores one process-lifetime vanilla snapshot and last committed result. */
    struct Entry final {
        std::vector<core::RuntimePropertyRecord> vanilla;
        std::vector<core::RuntimePropertyRecord> lastApplied;
        mods::PatchGeneration generation{};
        bool valid{};
    };

    mutable std::mutex mutex_;
    std::map<RuntimeResourceKey, Entry> entries_;
    std::map<core::Tgi, std::uint64_t> invalidationEpochs_;
    std::map<RuntimeResourceKey, RuntimeResourceObservation> observations_;
};

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Joined native contention fixtures, no writable epoch/nonce fields. */
struct RuntimeResourceCacheTestAccess final {
    /** @summary Hold actual cache mutex for a borrowed joined callback. */
    static void HoldCache(RuntimeResourceCache& cache,const std::function<void()>& callback);
    /** @summary Observe an actual epoch bookkeeping node without modifying it. */
    static std::optional<std::uint64_t> Epoch(RuntimeResourceCache& cache,const core::Tgi& target);
};
#endif

/** Returns a stable diagnostic label for a cache build status. */
[[nodiscard]] const char* RuntimeResourceBuildStatusName(
    RuntimeResourceBuildStatus status) noexcept;

}  // namespace sc13::runtime
