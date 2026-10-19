#include "runtime/runtime_resource_cache.hpp"

#include "mods/patch_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <bit>
#include <limits>

namespace sc13::runtime {
/** @summary Original cache-owned identity, serialized by its cache mutex. */
struct CacheControl {std::uint64_t nonce{},reset{1};bool closed{},exhausted{};std::weak_ptr<const void> canonicalState;bool canonicalStateBound{};};
static_assert(sizeof(CacheControl)<=128&&sizeof(CacheWitness)<=64);
namespace {
/** @summary Nonwrapping process cache incarnation issuer. */
std::atomic<std::uint64_t> cacheNonce{1};
/** @summary Reserve native cache identity or reject before usable admission. @return Unique nonce. */
std::uint64_t NextNonce(){auto value=cacheNonce.load();for(;;){if(value==UINT64_MAX)throw std::overflow_error("Cache identity exhausted");if(cacheNonce.compare_exchange_weak(value,value+1))return value;}}

/** Compares complete runtime records without relying on structure padding. */
[[nodiscard]] bool EqualRecords(
    std::span<const core::RuntimePropertyRecord> left,
    std::span<const core::RuntimePropertyRecord> right) noexcept {
    return left.size() == right.size() &&
           std::equal(
               left.begin(), left.end(), right.begin(),
               [](const core::RuntimePropertyRecord& first,
                  const core::RuntimePropertyRecord& second) {
                   return first.id == second.id &&
                          first.valueBits == second.valueBits &&
                          first.auxiliary == second.auxiliary &&
                          first.metadata == second.metadata;
               });
}

}  // namespace

RuntimeResourceCache::RuntimeResourceCache():control_(std::make_shared<CacheControl>()){control_->nonce=NextNonce();}
RuntimeResourceCache::~RuntimeResourceCache(){const std::scoped_lock lock(mutex_);control_->closed=true;}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
bool RuntimeResourceCache::BindCanonicalState(const std::shared_ptr<const void>& state){if(!state)return false;const std::scoped_lock lock(mutex_);if(control_->closed||control_->exhausted)return false;std::weak_ptr<const void> original(state);if(control_->canonicalStateBound)return !original.owner_before(control_->canonicalState)&&!control_->canonicalState.owner_before(original);control_->canonicalState=original;control_->canonicalStateBound=true;return true;}
#endif
CacheWitness RuntimeResourceCache::Witness() const {const std::scoped_lock lock(mutex_);CacheWitness witness;if(!control_->closed&&!control_->exhausted){witness.control_=control_;witness.nonce_=control_->nonce;witness.reset_=control_->reset;}return witness;}
bool RuntimeResourceCache::MatchesLocked(const CacheWitness& witness) const noexcept {const auto original=witness.control_.lock();return original==control_&&witness.nonce_==control_->nonce&&witness.reset_==control_->reset&&!control_->closed&&!control_->exhausted;}

std::optional<RuntimeFloatObservation> RuntimeResourceCache::ReadObservedFloat(
    const core::Tgi& target, std::uint32_t property) const {
    const std::scoped_lock lock(mutex_);
    std::optional<RuntimeFloatObservation> result;
    std::size_t instances{};
    for (const auto& [key, observation] : observations_) {
        if (key.target != target || observation.applied.empty()) continue;
        const auto value = core::ReadRuntimeFloat(observation.applied, property);
        const auto vanilla = core::ReadRuntimeFloat(observation.vanilla, property);
        if (!value || !vanilla) throw std::runtime_error("Observed property is missing or is not scalar float32");
        if (!std::isfinite(*value) || !std::isfinite(*vanilla)) throw std::runtime_error("Observed property is not finite");
        if (result && (std::bit_cast<std::uint32_t>(result->value) != std::bit_cast<std::uint32_t>(*value) ||
            std::bit_cast<std::uint32_t>(result->vanilla) != std::bit_cast<std::uint32_t>(*vanilla)))
            throw std::runtime_error("Observed instances disagree; no unique float value is available");
        ++instances;
        if (!result || observation.observedAtUnixMs >= result->observedAtUnixMs)
            result = RuntimeFloatObservation{*value, *vanilla, observation.observedAtUnixMs,
                observation.committedGeneration, observation.status, instances};
    }
    if (result) result->instances = instances;
    return result;
}

bool RuntimeResourceCache::Prepare(
    const RuntimeResourceKey& key,
    std::span<const core::RuntimePropertyRecord> current,
    mods::PatchGeneration generation,
    std::span<const mods::RegisteredPatch> patches,
    RuntimeResourceBuildPlan& plan,
    std::string& error) noexcept {
    try {
        plan = {};
        error.clear();
        if (key.resourceAddress == 0U ||
            core::ValidateRuntimePropertyTable(current, current.size()) !=
                core::RuntimeTableStatus::Valid) {
            error = "Runtime resource cache rejected its identity or property table";
            return false;
        }

        std::vector<core::RuntimePropertyRecord> vanilla;
        bool hadExisting = false;
        {
            const std::scoped_lock lock(mutex_);
            if(control_->closed||control_->exhausted){error="Runtime resource cache admission closed";return false;}
            plan.cacheWitness.control_=control_;plan.cacheWitness.nonce_=control_->nonce;plan.cacheWitness.reset_=control_->reset;
            const auto epoch = invalidationEpochs_.find(key.target);
            plan.invalidationEpoch =
                epoch == invalidationEpochs_.end() ? 0U : epoch->second;
            const auto existing = entries_.find(key);
            if (existing != entries_.end()) {
                hadExisting = true;
                const bool matchesVanilla = EqualRecords(current, existing->second.vanilla);
                const bool matchesApplied = EqualRecords(current, existing->second.lastApplied);
                if (!matchesVanilla && !matchesApplied) {
                    error =
                        "Current resource differs from both retained vanilla and last result";
                    return false;
                }
                vanilla = existing->second.vanilla;
                if (existing->second.valid) {
                    plan.key = key;
                    plan.generation = generation;
                    plan.status = RuntimeResourceBuildStatus::Reused;
                    plan.vanilla = existing->second.vanilla;
                    plan.previous.assign(current.begin(), current.end());
                    plan.desired = existing->second.lastApplied;
                    return true;
                }
            } else {
                vanilla.assign(current.begin(), current.end());
            }
        }

        std::vector<core::RuntimePropertyRecord> desired;
        if (!mods::ApplyPatchSequence(key.target, vanilla, patches, desired, error)) {
            return false;
        }
        plan.key = key;
        plan.generation = generation;
        plan.vanilla = std::move(vanilla);
        plan.previous.assign(current.begin(), current.end());
        plan.desired = std::move(desired);
        if (!hadExisting) {
            plan.status = RuntimeResourceBuildStatus::Created;
        } else if (EqualRecords(plan.desired, plan.vanilla) &&
                   !EqualRecords(plan.previous, plan.vanilla)) {
            plan.status = RuntimeResourceBuildStatus::Restored;
        } else {
            plan.status = RuntimeResourceBuildStatus::Rebuilt;
        }
        return true;
    } catch (...) {
        plan = {};
        error = "Runtime resource cache prepare failed due to an allocation exception";
        return false;
    }
}

bool RuntimeResourceCache::Commit(
    const RuntimeResourceBuildPlan& plan,
    std::string& error) noexcept {
    try {
        error.clear();
        if (plan.key.resourceAddress == 0U || plan.vanilla.empty() ||
            plan.vanilla.size() != plan.desired.size()) {
            error = "Runtime resource cache commit received an invalid plan";
            return false;
        }
        const std::scoped_lock lock(mutex_);
        if(!MatchesLocked(plan.cacheWitness)){error="Runtime resource cache owner/reset changed";return false;}
        const auto epoch = invalidationEpochs_.find(plan.key.target);
        const std::uint64_t currentEpoch =
            epoch == invalidationEpochs_.end() ? 0U : epoch->second;
        if (currentEpoch != plan.invalidationEpoch) {
            error = "Runtime resource cache plan was invalidated before commit";
            return false;
        }
        const auto existing = entries_.find(plan.key);
        if (existing != entries_.end() &&
            !EqualRecords(existing->second.vanilla, plan.vanilla)) {
            error = "Runtime resource cache vanilla changed before commit";
            return false;
        }
        Entry committed;
        committed.vanilla = plan.vanilla;
        committed.lastApplied = plan.desired;
        committed.generation = plan.generation;
        committed.valid = true;
        entries_.insert_or_assign(plan.key, std::move(committed));
        try {
            if (observations_.size() >= 128U && !observations_.contains(plan.key)) observations_.erase(observations_.begin());
            observations_.insert_or_assign(plan.key, RuntimeResourceObservation{plan.key, plan.generation,
                EqualRecords(plan.vanilla, plan.desired) ? "observed_unchanged" : "applied", "", plan.vanilla, plan.desired,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()), plan.generation});
        } catch (...) { /* @summary Diagnostic allocation failure must not invalidate a committed memory transaction. */ }
        return true;
    } catch (...) {
        error = "Runtime resource cache commit failed due to an allocation exception";
        return false;
    }
}

std::size_t RuntimeResourceCache::Invalidate(
    const std::set<core::Tgi>& affected) noexcept {
    std::size_t invalidated{};static_cast<void>(InvalidateChecked(affected,invalidated));return invalidated;
}
/** @summary Shared preallocate/check/evict/diagnostic algorithm, under the actual cache mutex. */
template<class Range> CacheEvictionResult RuntimeResourceCache::EvictTargets(const Range& affected,const CacheWitness* expected) noexcept {
    CacheEvictionResult result;
    try {
        const std::scoped_lock lock(mutex_);
        if(control_->exhausted){result.status=CacheEvictionStatus::PendingEpochExhausted;return result;}
        if(control_->closed)return result;
        if(expected&&!MatchesLocked(*expected)){result.status=CacheEvictionStatus::PendingResetMismatch;return result;}
        try{for(const auto& target:affected)invalidationEpochs_.try_emplace(target,0);}
        catch(...){result.status=CacheEvictionStatus::PendingAllocation;return result;}
        for(const auto& target:affected)if(invalidationEpochs_.find(target)->second==UINT64_MAX){control_->exhausted=true;result.status=CacheEvictionStatus::PendingEpochExhausted;return result;}
        for(const auto& target:affected)++invalidationEpochs_.find(target)->second;
        for (auto& [key, entry] : entries_) {
            if (entry.valid && std::binary_search(affected.begin(),affected.end(),key.target)) {
                entry.valid = false;
                ++result.invalidated;
            }
        }
        result.status=CacheEvictionStatus::Evicted;
        try{for(auto& [key,observation]:observations_)if(std::binary_search(affected.begin(),affected.end(),key.target))observation.status="pending";}
        catch(...){result.status=CacheEvictionStatus::EvictedDiagnosticPending;}
        return result;
    }catch(...){result.invalidated=0;result.status=CacheEvictionStatus::PendingUnavailable;return result;}
}
CacheEvictionResult RuntimeResourceCache::EvictOriginal(std::span<const core::Tgi> affected,const CacheWitness& expected) noexcept {return EvictTargets(affected,&expected);}
bool RuntimeResourceCache::InvalidateChecked(const std::set<core::Tgi>& affected,std::size_t& invalidated) noexcept {const auto result=EvictTargets(affected,nullptr);const bool complete=result.status==CacheEvictionStatus::Evicted||result.status==CacheEvictionStatus::EvictedDiagnosticPending;invalidated=complete?result.invalidated:0;return complete;}

bool RuntimeResourceCache::Contains(const RuntimeResourceKey& key) const {
    const std::scoped_lock lock(mutex_);
    return entries_.contains(key);
}

std::size_t RuntimeResourceCache::size() const {
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

std::vector<RuntimeResourceRestoreEntry>
RuntimeResourceCache::SnapshotForRestore() const {
    const std::scoped_lock lock(mutex_);
    std::vector<RuntimeResourceRestoreEntry> result;
    result.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        result.push_back(RuntimeResourceRestoreEntry{
            key, entry.vanilla, entry.lastApplied});
    }
    return result;
}

std::vector<RuntimeResourceRestoreEntry> RuntimeResourceCache::SnapshotForTargets(const std::set<core::Tgi>& targets) const {
    const std::scoped_lock lock(mutex_);
    std::vector<RuntimeResourceRestoreEntry> result;
    for (const auto& [key, entry] : entries_) {
        if (targets.contains(key.target)) result.push_back({key, entry.vanilla, entry.lastApplied});
    }
    return result;
}

void RuntimeResourceCache::Clear() noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        if(control_->reset==UINT64_MAX)control_->exhausted=true;else ++control_->reset;
        entries_.clear();
        invalidationEpochs_.clear();
        observations_.clear();
    } catch (...) {
    }
}

void RuntimeResourceCache::RecordFailure(const RuntimeResourceKey& key, mods::PatchGeneration generation,
    std::string_view reason) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        if (observations_.size() >= 128U && !observations_.contains(key)) observations_.erase(observations_.begin());
        auto& observation = observations_[key];
        observation.key = key; observation.generation = generation;
        observation.status = "rejected"; observation.diagnostic = reason;
        observation.observedAtUnixMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    } catch (...) { /* @summary Observation failure leaves patch safety and gameplay unchanged. */ }
}

std::vector<RuntimeResourceObservation> RuntimeResourceCache::Observations() const {
    const std::scoped_lock lock(mutex_);
    std::vector<RuntimeResourceObservation> result;
    for (const auto& [key, observation] : observations_) { static_cast<void>(key); result.push_back(observation); }
    return result;
}

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
void RuntimeResourceCacheTestAccess::HoldCache(RuntimeResourceCache& cache,const std::function<void()>& callback){const std::scoped_lock lock(cache.mutex_);callback();}
std::optional<std::uint64_t> RuntimeResourceCacheTestAccess::Epoch(RuntimeResourceCache& cache,const core::Tgi& target){const std::scoped_lock lock(cache.mutex_);const auto found=cache.invalidationEpochs_.find(target);return found==cache.invalidationEpochs_.end()?std::nullopt:std::optional(found->second);}
#endif
const char* RuntimeResourceBuildStatusName(
    RuntimeResourceBuildStatus status) noexcept {
    switch (status) {
        case RuntimeResourceBuildStatus::Created: return "created";
        case RuntimeResourceBuildStatus::Rebuilt: return "rebuilt";
        case RuntimeResourceBuildStatus::Reused: return "reused";
        case RuntimeResourceBuildStatus::Restored: return "restored";
    }
    return "unknown";
}

}  // namespace sc13::runtime
