#pragma once

#include "core/runtime_property_table.hpp"
#include "mods/mod_types.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace sc13::mods {
struct OwnedPublicationTestAccess;

/** Exposes one immutable generation of active TGI-indexed patch definitions. */
class PatchRegistrySnapshot final {
public:
    /** Returns the snapshot generation. */
    [[nodiscard]] PatchGeneration generation() const noexcept { return generation_; }

    /** @summary Returns the domain revision captured by this exact publication.
     * @return Revision supplied by the native domain coordinator, or zero for an unbound registry.
     */
    [[nodiscard]] std::uint64_t domain_revision() const noexcept { return domainRevision_; }

    /** Returns patches for one TGI without scanning unrelated mods. */
    [[nodiscard]] const std::vector<RegisteredPatch>* Find(
        const core::Tgi& target) const noexcept;

    /** Returns conflicts detected while this snapshot was built. */
    [[nodiscard]] const std::vector<PatchConflict>& conflicts() const noexcept {
        return conflicts_;
    }

private:
    friend class PatchRegistry;
    PatchGeneration generation_{};
    std::uint64_t domainRevision_{};
    std::map<core::Tgi, std::vector<RegisteredPatch>> patches_;
    std::vector<PatchConflict> conflicts_;
};

/** Owns active mod patches and atomically publishes immutable hot-path snapshots. */
class PatchRegistry final {
    struct PreparedState;
public:
    /** @summary Selects one existing owner mutation for pure preparation. */
    enum class ChangeKind { Activate, Deactivate, Replace };

    /** @summary Owns candidate and retired storage; destroy after outer writer locks release. */
    class PreparedPublication final {
    public:
        /** @summary Creates an empty unpublished plan without allocation. */
        PreparedPublication() noexcept;
        /** @summary Releases candidate or retired storage outside publication reservations. */
        ~PreparedPublication();
        /** @summary Transfers sole ownership of prepared and retired storage. */
        PreparedPublication(PreparedPublication&&) noexcept;
        /** @summary Transfers storage; caller must release outer writer locks first. */
        PreparedPublication& operator=(PreparedPublication&&) noexcept;
        PreparedPublication(const PreparedPublication&) = delete;
        PreparedPublication& operator=(const PreparedPublication&) = delete;
        /** @summary Returns the preallocated affected target set. */
        [[nodiscard]] const std::set<core::Tgi>& Affected() const noexcept;
        /** @summary Returns the registry generation against which this candidate was prepared. */
        [[nodiscard]] PatchGeneration ExpectedGeneration() const noexcept;
        /** @summary Transfers affected targets without allocation after publication.
         * @param affected Output set swapped with the plan's preallocated target set.
         */
        void TakeAffected(std::set<core::Tgi>& affected) noexcept;
    private:
        friend class PatchRegistry;
        std::unique_ptr<PreparedState> state_;
    };

    /** @summary Reserves publication without waiting; performs no candidate allocation. */
    class PublicationGuard final {
    public:
        /** @summary Reports whether this nonblocking reservation acquired the writer lock. */
        [[nodiscard]] bool owns_lock() const noexcept { return lock_.owns_lock(); }
        /** @summary Checks registry identity and generation before the irreversible boundary.
         * @param plan Candidate owned by the caller until all outer locks release.
         * @return True only for a current, complete, unconsumed plan belonging to this registry.
         */
        [[nodiscard]] bool CanPublish(const PreparedPublication& plan) const noexcept;
        /** @summary Publishes a validated plan without allocation or retired-state destruction.
         * @param plan Plan accepted by CanPublish under this same reservation.
         * @param domainRevision Exact outer-domain revision assigned at the commit boundary.
         * @return False only when the reservation or plan is invalid; no effects on failure.
         */
        [[nodiscard]] bool Publish(PreparedPublication& plan, std::uint64_t domainRevision) noexcept;
    private:
        friend class PatchRegistry;
        /** @summary Attempts a single registry reservation.
         * @param registry Native registry being reserved.
         * @param wait True for an existing legacy writer, false for try-only admission.
         */
        explicit PublicationGuard(PatchRegistry& registry, bool wait) noexcept;
        PatchRegistry* registry_{};
        std::unique_lock<std::mutex> lock_;
    };

    /** Creates an empty generation-zero registry. */
    PatchRegistry();

    /** @summary Copies and builds a complete candidate without publishing any state.
     * @param kind Existing owner mutation to prepare.
     * @param owner Stable owner identifier.
     * @param patches Replacement definitions; ignored for deactivation.
     * @param plan Empty output plan, unchanged on preparation failure.
     * @param error Preparation diagnostic, assigned outside publication locks.
     * @return True if a complete candidate is ready for generation validation.
     */
    [[nodiscard]] bool Prepare(ChangeKind kind, const ModId& owner,
        std::span<const ResourcePatchDefinition> patches,
        PreparedPublication& plan, std::string& error) noexcept;

    /** @summary Attempts the registry writer reservation in the documented outer lock order. */
    [[nodiscard]] PublicationGuard TryReservePublication() noexcept;
    /** @summary Reserves an existing serialized legacy writer before its commit boundary.
     * @param wait True only for legacy callers outside earlier/later lock inversions.
     * @return Owning reservation or an empty nonblocking reservation.
     */
    [[nodiscard]] PublicationGuard ReservePublication(bool wait = false) noexcept;

    /** @summary Registers patches in a standalone registry, without a runtime domain.
     * Live ModManager writers must use its domain-coordinated prepared seam instead.
     */
    [[nodiscard]] bool Activate(
        const ModId& owner,
        std::span<const ResourcePatchDefinition> patches,
        std::set<core::Tgi>& affected,
        std::string& error) noexcept;

    /** @summary Removes patches from a standalone registry, without a runtime domain. */
    [[nodiscard]] bool Deactivate(
        const ModId& owner,
        std::set<core::Tgi>& affected,
        std::string& error) noexcept;
    /** @summary Replaces standalone registry patches while preserving activation order. */
    [[nodiscard]] bool Replace(const ModId& owner,
        std::span<const ResourcePatchDefinition> patches,
        std::set<core::Tgi>& affected, std::string& error) noexcept;

    /** Atomically loads the current immutable snapshot. */
    [[nodiscard]] std::shared_ptr<const PatchRegistrySnapshot> Current() const noexcept;

    /** Reports whether an owner currently has a committed patch set. */
    [[nodiscard]] bool IsActive(const ModId& owner) const;

private:
    /** Stores one complete active mod patch set before snapshot expansion. */
    struct OwnedPatchSet final {
        std::uint64_t sequence{};
        std::vector<ResourcePatchDefinition> patches;
        /** @summary Native incarnation key only; never retains a VM, endpoint or scope graph. */
        std::shared_ptr<const void> incarnation;
    };

    friend class OwnerLifecycle;
    friend class OwnedPublication;
    /** @summary Prepare one exact sealed removal or direct successor replacement.
     * @param owner Native record owner. @param oldKey Exact current seal.
     * @param newKey Successor seal, or null for release-only removal.
     * @param patches Complete successor set. @param plan Empty retained output.
     * @param absent Reports an already absent owner without publishing a generation.
     * @param error Preparation diagnostic. @return Whether preparation succeeded. */
    bool PrepareMaintenance(const ModId& owner, const std::shared_ptr<const void>& oldKey,
        const std::shared_ptr<const void>& newKey, std::span<const ResourcePatchDefinition> patches,
        PreparedPublication& plan, bool& absent, std::string& error) noexcept;

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct OwnedPublicationTestAccess;
#endif
    /** @summary Prepare an exact native incarnation without adopting an existing foreign or legacy owner.
     * @param kind Existing mutation. @param owner Native-derived owner. @param patches Copied effective patches.
     * @param incarnation Retained nonreusable native identity. @param plan Empty output. @param error Diagnostic.
     * @param busy Optional try-only mode; true reports actual writer contention instead of waiting.
     * @return Whether a complete owner-bound candidate was prepared. */
    bool PrepareOwned(ChangeKind kind,const ModId& owner,std::span<const ResourcePatchDefinition> patches,
        const std::shared_ptr<const void>& incarnation,PreparedPublication& plan,std::string& error,bool* busy=nullptr) noexcept;
    /** @summary Share preparation/generation logic for legacy unsealed and native sealed owners.
     * @param kind Existing mutation. @param owner Native owner. @param patches Copied patches.
     * @param incarnation Null for legacy, exact retained seal otherwise. @param plan Empty output.
     * @param successor Optional direct replacement seal for private lifecycle handoff.
     * @param absent Non-null only for private maintenance, enabling exact absent removal.
     * @param busy Non-null selects try-only snapshot capture and reports writer contention.
     * @param error Diagnostic. @return Whether preparation succeeded. */
    bool PrepareImpl(ChangeKind kind,const ModId& owner,std::span<const ResourcePatchDefinition> patches,
        const std::shared_ptr<const void>& incarnation,PreparedPublication& plan,std::string& error,
        const std::shared_ptr<const void>& successor = {}, bool* absent = nullptr,bool* busy=nullptr) noexcept;

    using ActivePatchSets = std::map<ModId, OwnedPatchSet, std::less<>>;

    /** Builds one deterministic immutable snapshot from candidate active sets. */
    [[nodiscard]] static std::shared_ptr<PatchRegistrySnapshot> BuildSnapshot(
        const ActivePatchSets& active,
        PatchGeneration generation);

    /** @summary Implements legacy entry points using the same prepared publication protocol. */
    [[nodiscard]] bool Mutate(ChangeKind kind, const ModId& owner,
        std::span<const ResourcePatchDefinition> patches,
        std::set<core::Tgi>& affected, std::string& error) noexcept;

    mutable std::mutex mutationMutex_;
    std::mutex standaloneWriterMutex_;
    ActivePatchSets active_;
    std::uint64_t nextSequence_{1U};
    PatchGeneration generation_{};
    std::atomic<std::shared_ptr<const PatchRegistrySnapshot>> current_;
};

/** Rebuilds one runtime PROP table from vanilla plus an ordered patch list. */
[[nodiscard]] bool ApplyPatchSequence(
    const core::Tgi& target,
    std::span<const core::RuntimePropertyRecord> vanilla,
    std::span<const RegisteredPatch> patches,
    std::vector<core::RuntimePropertyRecord>& result,
    std::string& error) noexcept;

}  // namespace sc13::mods
