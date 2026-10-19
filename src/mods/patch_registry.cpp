#include "mods/patch_registry.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace sc13::mods {
namespace {

constexpr std::uint32_t kScalarFloatMetadata = 0x000D0000U;

/** Applies one finite float operation without changing the source on failure. */
[[nodiscard]] bool ApplyFloatOperation(
    float current,
    const PropertyPatch& patch,
    float& result) noexcept {
    switch (patch.operation) {
        case PatchOperation::Set: result = patch.value; break;
        case PatchOperation::Add: result = current + patch.value; break;
        case PatchOperation::Subtract: result = current - patch.value; break;
        case PatchOperation::Multiply: result = current * patch.value; break;
        case PatchOperation::Divide:
            if (patch.value == 0.0F) {
                return false;
            }
            result = current / patch.value;
            break;
    }
    return std::isfinite(result);
}

/** Sorts resource blocks by activation sequence with owner as a stable tie-breaker. */
[[nodiscard]] bool RegisteredPatchLess(
    const RegisteredPatch& left,
    const RegisteredPatch& right) noexcept {
    if (left.sequence != right.sequence) {
        return left.sequence < right.sequence;
    }
    return left.owner < right.owner;
}

}  // namespace

const std::vector<RegisteredPatch>* PatchRegistrySnapshot::Find(
    const core::Tgi& target) const noexcept {
    const auto iterator = patches_.find(target);
    return iterator == patches_.end() ? nullptr : &iterator->second;
}

PatchRegistry::PatchRegistry() {
    auto initial = std::make_shared<PatchRegistrySnapshot>();
    current_.store(std::move(initial), std::memory_order_release);
}

std::shared_ptr<PatchRegistrySnapshot> PatchRegistry::BuildSnapshot(
    const ActivePatchSets& active,
    PatchGeneration generation) {
    auto snapshot = std::make_shared<PatchRegistrySnapshot>();
    snapshot->generation_ = generation;
    for (const auto& [owner, owned] : active) {
        for (const ResourcePatchDefinition& patch : owned.patches) {
            snapshot->patches_[patch.target].push_back(
                RegisteredPatch{owner, patch, owned.sequence});
        }
    }
    for (auto& [target, patches] : snapshot->patches_) {
        std::sort(patches.begin(), patches.end(), RegisteredPatchLess);
        std::map<std::uint32_t, ModId> previousOwners;
        for (const RegisteredPatch& registered : patches) {
            for (const PropertyPatch& property : registered.patch.properties) {
                const auto previous = previousOwners.find(property.propertyId);
                if (previous != previousOwners.end() &&
                    previous->second != registered.owner) {
                    snapshot->conflicts_.push_back(PatchConflict{
                        target, property.propertyId, previous->second, registered.owner});
                }
                previousOwners[property.propertyId] = registered.owner;
            }
        }
    }
    return snapshot;
}


/** @summary Retains all allocations before publication and retired objects afterward. */
struct PatchRegistry::PreparedState final {
    PatchRegistry* registry{};
    PatchGeneration expectedGeneration{};
    std::uint64_t nextSequence{};
    ActivePatchSets candidate;
    std::shared_ptr<PatchRegistrySnapshot> snapshot;
    std::shared_ptr<const PatchRegistrySnapshot> retiredSnapshot;
    std::set<core::Tgi> affected;
    bool published{};
};

PatchRegistry::PreparedPublication::PreparedPublication() noexcept = default;
PatchRegistry::PreparedPublication::~PreparedPublication() = default;
PatchRegistry::PreparedPublication::PreparedPublication(PreparedPublication&&) noexcept = default;
PatchRegistry::PreparedPublication& PatchRegistry::PreparedPublication::operator=(
    PreparedPublication&&) noexcept = default;

const std::set<core::Tgi>& PatchRegistry::PreparedPublication::Affected() const noexcept {
    static const std::set<core::Tgi> empty;
    return state_ ? state_->affected : empty;
}

PatchGeneration PatchRegistry::PreparedPublication::ExpectedGeneration() const noexcept {
    return state_ ? state_->expectedGeneration : 0U;
}

void PatchRegistry::PreparedPublication::TakeAffected(std::set<core::Tgi>& affected) noexcept {
    if (state_) affected.swap(state_->affected);
}

PatchRegistry::PublicationGuard::PublicationGuard(PatchRegistry& registry, bool wait) noexcept
    : registry_(&registry), lock_(registry.mutationMutex_, std::defer_lock) {
    if (wait) lock_.lock(); else (void)lock_.try_lock();
}

bool PatchRegistry::PublicationGuard::CanPublish(const PreparedPublication& plan) const noexcept {
    return lock_.owns_lock() && plan.state_ && plan.state_->registry == registry_ &&
        !plan.state_->published && plan.state_->snapshot &&
        plan.state_->expectedGeneration == registry_->generation_;
}

bool PatchRegistry::PublicationGuard::Publish(
    PreparedPublication& plan, std::uint64_t domainRevision) noexcept {
    if (!CanPublish(plan)) return false;
    auto& state = *plan.state_;
    state.snapshot->domainRevision_ = domainRevision;
    registry_->active_.swap(state.candidate);
    registry_->nextSequence_ = state.nextSequence;
    registry_->generation_ = state.snapshot->generation_;
    state.retiredSnapshot = registry_->current_.exchange(
        std::move(state.snapshot), std::memory_order_acq_rel);
    state.published = true;
    return true;
}

PatchRegistry::PublicationGuard PatchRegistry::TryReservePublication() noexcept {
    return PublicationGuard(*this, false);
}

PatchRegistry::PublicationGuard PatchRegistry::ReservePublication(bool wait) noexcept {
    return PublicationGuard(*this, wait);
}

bool PatchRegistry::Prepare(ChangeKind kind, const ModId& owner,
    std::span<const ResourcePatchDefinition> patches,
    PreparedPublication& plan, std::string& error) noexcept {
    return PrepareImpl(kind,owner,patches,{},plan,error);
}
bool PatchRegistry::PrepareOwned(ChangeKind kind,const ModId& owner,std::span<const ResourcePatchDefinition> patches,
    const std::shared_ptr<const void>& incarnation,PreparedPublication& plan,std::string& error,bool* busy) noexcept {
    if(busy)*busy=false;
    if(!incarnation) {try {error="Missing native owner incarnation";} catch(...) {} return false;}
    return PrepareImpl(kind,owner,patches,incarnation,plan,error,{},nullptr,busy);
}
bool PatchRegistry::PrepareImpl(ChangeKind kind,const ModId& owner,std::span<const ResourcePatchDefinition> patches,
    const std::shared_ptr<const void>& incarnation,PreparedPublication& plan,std::string& error,
    const std::shared_ptr<const void>& successor, bool* absent,bool* busy) noexcept {
    try {
        error.clear();
        if(busy)*busy=false;
        if (absent) *absent = false;
        if (plan.state_) { error = "Preparation requires an empty plan"; return false; }
        if (owner.empty()) { error = "Patch mutation requires a stable owner"; return false; }
        auto state = std::make_unique<PreparedState>();
        state->registry = this;
        {
            std::unique_lock lock(mutationMutex_,std::defer_lock);
            if(busy) {
                if(!lock.try_lock()){*busy=true;error="Patch preparation writer is busy";return false;}
            } else lock.lock();
            state->expectedGeneration = generation_;
            state->nextSequence = nextSequence_;
            state->candidate = active_;
        }
        const bool absentRemoval = absent && kind == ChangeKind::Deactivate && !state->candidate.contains(owner);
        if (!absentRemoval && state->expectedGeneration == std::numeric_limits<PatchGeneration>::max()) {
            error = "Patch generation exhausted"; return false;
        }
        auto existing = state->candidate.find(owner);
        if(existing!=state->candidate.end() && existing->second.incarnation!=incarnation) {
            error="Patch owner belongs to another native incarnation";return false;
        }
        if (absentRemoval) {
            *absent = true;
        } else if (kind == ChangeKind::Activate) {
            if (existing != state->candidate.end()) { error = "Patch owner is already active"; return false; }
            if (state->nextSequence == std::numeric_limits<std::uint64_t>::max()) {
                error = "Patch activation sequence exhausted"; return false;
            }
            OwnedPatchSet owned;
            owned.incarnation=incarnation;
            owned.sequence = state->nextSequence++;
            owned.patches.assign(patches.begin(), patches.end());
            state->candidate.emplace(owner, std::move(owned));
        } else {
            if (existing == state->candidate.end()) { error = "Patch owner is not active"; return false; }
            for (const auto& patch : existing->second.patches) state->affected.insert(patch.target);
            if (kind == ChangeKind::Deactivate) state->candidate.erase(existing);
            else if (kind == ChangeKind::Replace) {
                existing->second.patches.assign(patches.begin(), patches.end());
                if (successor) existing->second.incarnation = successor;
            }
            else { error = "Unknown patch mutation"; return false; }
        }
        if (kind != ChangeKind::Deactivate) {
            for (const auto& patch : patches) state->affected.insert(patch.target);
        }
        state->snapshot = BuildSnapshot(state->candidate, state->expectedGeneration + (absentRemoval ? 0U : 1U));
        plan.state_ = std::move(state);
        return true;
    } catch (...) {
        try { error = "Patch preparation failed"; } catch (...) {}
        return false;
    }
}

bool PatchRegistry::PrepareMaintenance(const ModId& owner, const std::shared_ptr<const void>& oldKey,
    const std::shared_ptr<const void>& newKey, std::span<const ResourcePatchDefinition> patches,
    PreparedPublication& plan, bool& absent, std::string& error) noexcept {
    absent = false;
    if (!oldKey || newKey == oldKey || (!newKey && !patches.empty())) {
        try { error = "Invalid exact-owner maintenance"; } catch (...) {}
        return false;
    }
    return PrepareImpl(newKey ? ChangeKind::Replace : ChangeKind::Deactivate,
        owner, patches, oldKey, plan, error, newKey, &absent);
}

bool PatchRegistry::Mutate(ChangeKind kind, const ModId& owner,
    std::span<const ResourcePatchDefinition> patches,
    std::set<core::Tgi>& affected, std::string& error) noexcept {
    affected.clear();
    PreparedPublication plan;
    const std::scoped_lock standaloneWriter(standaloneWriterMutex_);
    if (!Prepare(kind, owner, patches, plan, error)) return false;
    bool published = false;
    {
        auto guard = ReservePublication(true);
        published = guard.Publish(plan, Current()->domain_revision());
    }
    if (!published) {
        try { error = "Patch publication is busy or stale"; } catch (...) {}
        return false;
    }
    plan.TakeAffected(affected);
    return true;
}

bool PatchRegistry::Activate(const ModId& owner,
    std::span<const ResourcePatchDefinition> patches,
    std::set<core::Tgi>& affected, std::string& error) noexcept {
    return Mutate(ChangeKind::Activate, owner, patches, affected, error);
}

bool PatchRegistry::Deactivate(const ModId& owner,
    std::set<core::Tgi>& affected, std::string& error) noexcept {
    return Mutate(ChangeKind::Deactivate, owner, {}, affected, error);
}

bool PatchRegistry::Replace(const ModId& owner,
    std::span<const ResourcePatchDefinition> patches,
    std::set<core::Tgi>& affected, std::string& error) noexcept {
    return Mutate(ChangeKind::Replace, owner, patches, affected, error);
}

std::shared_ptr<const PatchRegistrySnapshot> PatchRegistry::Current() const noexcept {
    return current_.load(std::memory_order_acquire);
}

bool PatchRegistry::IsActive(const ModId& owner) const {
    const std::scoped_lock lock(mutationMutex_);
    return active_.contains(owner);
}

bool ApplyPatchSequence(
    const core::Tgi& target,
    std::span<const core::RuntimePropertyRecord> vanilla,
    std::span<const RegisteredPatch> patches,
    std::vector<core::RuntimePropertyRecord>& result,
    std::string& error) noexcept {
    try {
        error.clear();
        if (core::ValidateRuntimePropertyTable(vanilla, vanilla.size()) !=
            core::RuntimeTableStatus::Valid) {
            error = "Vanilla runtime property table is malformed";
            return false;
        }
        std::vector<core::RuntimePropertyRecord> candidate(vanilla.begin(), vanilla.end());
        for (const RegisteredPatch& registered : patches) {
            if (registered.owner.empty() || registered.patch.target != target) {
                error = "Registered patch ownership or target is invalid";
                return false;
            }
            for (const PropertyPatch& patch : registered.patch.properties) {
                const auto property = std::lower_bound(
                    candidate.begin(), candidate.end(), patch.propertyId,
                    [](const core::RuntimePropertyRecord& entry, std::uint32_t identifier) {
                        return entry.id < identifier;
                    });
                if (property == candidate.end() || property->id != patch.propertyId) {
                    error = "Mod '" + registered.owner +
                            "' targets a property missing from the vanilla resource";
                    return false;
                }
                if (patch.type != PropertyType::Float ||
                    property->metadata != kScalarFloatMetadata) {
                    error = "Mod '" + registered.owner +
                            "' targets a property with an incompatible runtime type";
                    return false;
                }
                const float current = std::bit_cast<float>(property->valueBits);
                float replacement = 0.0F;
                if (!std::isfinite(current) ||
                    !ApplyFloatOperation(current, patch, replacement)) {
                    error = "Mod '" + registered.owner +
                            "' produced a non-finite or invalid float result";
                    return false;
                }
                property->valueBits = std::bit_cast<std::uint32_t>(replacement);
            }
        }
        result = std::move(candidate);
        return true;
    } catch (...) {
        result.clear();
        error = "Patch sequence failed due to an allocation exception";
        return false;
    }
}

}  // namespace sc13::mods
