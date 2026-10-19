#include "test_harness.hpp"
#include "mods/patch_registry.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <set>
#include <span>
#include <string>
#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

namespace registry_allocation_probe {
/** @summary Selects a test-thread allocation to fail without production injection controls. */
thread_local int remaining{-1};
/** @summary Counts current-thread reclamation to detect forbidden destruction during commit. */
thread_local std::size_t deallocations{};
}

/** @summary Replaces allocation only in this test executable.
 * @param bytes Requested allocation size.
 * @return Owned allocation or throws at the selected fixture boundary.
 */
void* operator new(std::size_t bytes) {
    if (registry_allocation_probe::remaining == 0) throw std::bad_alloc();
    if (registry_allocation_probe::remaining > 0) --registry_allocation_probe::remaining;
    if (void* allocation = std::malloc(bytes ? bytes : 1U)) return allocation;
    throw std::bad_alloc();
}
/** @summary Releases test allocation. @param allocation Owned allocation. */
void operator delete(void* allocation) noexcept {
    if (allocation) ++registry_allocation_probe::deallocations;
    std::free(allocation);
}
/** @summary Releases sized test allocation. @param allocation Owned allocation. */
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

namespace sc13::tests {
namespace {
/** @summary Proves preparation isolation, stale rejection and allocation-free publication. */
void TestPreparedPublication() {
    using Registry = sc13::mods::PatchRegistry;
    const sc13::core::Tgi target{1U, 2U, 3U};
    const sc13::mods::ResourcePatchDefinition patch{target, {{4U,
        sc13::mods::PropertyType::Float, sc13::mods::PatchOperation::Set, 5.0F}}};
    Registry registry;
    Registry other;
    std::string error;
    error.reserve(256U);
    Registry::PreparedPublication first;
    Registry::PreparedPublication competing;
    SC13_EXPECT(registry.Prepare(Registry::ChangeKind::Activate, "first", {&patch, 1U}, first, error));
    SC13_EXPECT(registry.Prepare(Registry::ChangeKind::Activate, "competing", {&patch, 1U}, competing, error));
    SC13_EXPECT(registry.Current()->generation() == 0U);
    SC13_EXPECT(first.ExpectedGeneration() == 0U && first.Affected().contains(target));
    {
        auto wrong = other.TryReservePublication();
        SC13_EXPECT(!wrong.CanPublish(first));
        SC13_EXPECT(!wrong.Publish(first, 1U));
    }
    bool valid = false;
    bool published = false;
    {
        auto guard = registry.TryReservePublication();
        registry_allocation_probe::remaining = 0;
        valid = guard.CanPublish(first);
        published = guard.Publish(first, 17U);
        registry_allocation_probe::remaining = -1;
        SC13_EXPECT(valid && published);
        SC13_EXPECT(!guard.CanPublish(first));
        SC13_EXPECT(!guard.CanPublish(competing));
        SC13_EXPECT(!guard.Publish(competing, 18U));
    }
    SC13_EXPECT(registry.Current()->generation() == 1U);
    SC13_EXPECT(registry.Current()->domain_revision() == 17U);
    SC13_EXPECT(!registry.IsActive("competing"));

    // A cancelled admission discards its prepared candidate without any publication.
    {
        Registry::PreparedPublication cancelled;
        SC13_EXPECT(registry.Prepare(Registry::ChangeKind::Deactivate, "first", {}, cancelled, error));
    }
    SC13_EXPECT(registry.IsActive("first") && registry.Current()->generation() == 1U);

    Registry::PreparedPublication replacement;
    SC13_EXPECT(registry.Prepare(Registry::ChangeKind::Replace, "first", {&patch, 1U}, replacement, error));
    SC13_EXPECT(!registry.Prepare(Registry::ChangeKind::Deactivate, "first", {}, replacement, error));
    Registry::PreparedPublication moved(std::move(replacement));
    std::size_t releasedAtCommit = 0;
    {
        auto guard = registry.TryReservePublication();
        SC13_EXPECT(!guard.CanPublish(replacement));
        const auto before = registry_allocation_probe::deallocations;
        registry_allocation_probe::remaining = 0;
        published = guard.Publish(moved, 18U);
        registry_allocation_probe::remaining = -1;
        releasedAtCommit = registry_allocation_probe::deallocations - before;
    }
    SC13_EXPECT(published && releasedAtCommit == 0U);
    const auto beforeReclaim = registry_allocation_probe::deallocations;
    moved = Registry::PreparedPublication{};
    SC13_EXPECT(registry_allocation_probe::deallocations > beforeReclaim);

    bool reachedSuccess = false;
    for (int failAt = 0; failAt < 128 && !reachedSuccess; ++failAt) {
        Registry::PreparedPublication candidate;
        registry_allocation_probe::remaining = failAt;
        reachedSuccess = registry.Prepare(Registry::ChangeKind::Replace, "first", {&patch, 1U}, candidate, error);
        registry_allocation_probe::remaining = -1;
        SC13_EXPECT(registry.Current()->generation() == 2U);
    }
    SC13_EXPECT(reachedSuccess);

    // Independently prepared writers race for the same generation; exactly one wins.
    Registry::PreparedPublication left;
    Registry::PreparedPublication right;
    SC13_EXPECT(registry.Prepare(Registry::ChangeKind::Activate, "left", {&patch, 1U}, left, error));
    SC13_EXPECT(registry.Prepare(Registry::ChangeKind::Activate, "right", {&patch, 1U}, right, error));
    std::atomic<int> winners{};
    std::atomic<int> ready{};
    std::atomic<bool> start{};
    const auto attempt = [&](Registry::PreparedPublication& plan) {
        ++ready;
        while (!start.load()) std::this_thread::yield();
        auto guard = registry.TryReservePublication();
        if (guard.Publish(plan, 19U)) ++winners;
    };
    std::thread a(attempt, std::ref(left));
    std::thread b(attempt, std::ref(right));
    while (ready.load() != 2) std::this_thread::yield();
    start = true;
    a.join(); b.join();
    SC13_EXPECT(winners == 1);
    SC13_EXPECT(registry.Current()->generation() == 3U);
    SC13_EXPECT(registry.Current()->domain_revision() == 19U);
}
/** @summary Creates one typed float patch block for registry tests. */
[[nodiscard]] sc13::mods::ResourcePatchDefinition MakeResourcePatch(
    const sc13::core::Tgi& target,
    std::uint32_t propertyId,
    sc13::mods::PatchOperation operation,
    float value) {
    return sc13::mods::ResourcePatchDefinition{
        target,
        {sc13::mods::PropertyPatch{
            propertyId, sc13::mods::PropertyType::Float, operation, value}}};
}

/** @summary Validates ownership, activation order, conflicts, and vanilla-derived application. */
void TestPatchRegistry() {
    constexpr std::uint32_t floatMetadata = 0x000D0000U;
    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const auto setMaintenance = MakeResourcePatch(
        target, 0x09AE19D7U, sc13::mods::PatchOperation::Set, 345.0F);
    const auto addMaintenance = MakeResourcePatch(
        target, 0x09AE19D7U, sc13::mods::PatchOperation::Add, 5.0F);
    const auto multiplyCapacity = MakeResourcePatch(
        target, 0x0AFB9882U, sc13::mods::PatchOperation::Multiply, 2.0F);

    sc13::mods::PatchRegistry registry;
    std::set<sc13::core::Tgi> affected;
    std::string error;
    SC13_EXPECT(registry.Current()->generation() == 0U);
    SC13_EXPECT(registry.Activate(
        "mod.set", std::span(&setMaintenance, 1U), affected, error));
    SC13_EXPECT(affected == std::set<sc13::core::Tgi>{target});
    const std::shared_ptr<const sc13::mods::PatchRegistrySnapshot> first =
        registry.Current();
    SC13_EXPECT(first->generation() == 1U);
    SC13_EXPECT(first->Find(target) != nullptr);
    SC13_EXPECT(first->Find(target)->front().owner == "mod.set");

    SC13_EXPECT(registry.Activate(
        "mod.add", std::span(&addMaintenance, 1U), affected, error));
    const auto second = registry.Current();
    SC13_EXPECT(second->generation() == 2U);
    SC13_EXPECT(second->Find(target)->size() == 2U);
    SC13_EXPECT((*second->Find(target))[0].owner == "mod.set");
    SC13_EXPECT((*second->Find(target))[1].owner == "mod.add");
    SC13_EXPECT(second->conflicts().size() == 1U);
    SC13_EXPECT(second->conflicts().front().propertyId == 0x09AE19D7U);
    SC13_EXPECT(first->Find(target)->size() == 1U);

    std::array<sc13::core::RuntimePropertyRecord, 2> vanilla{
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x0AFB9882U, std::bit_cast<std::uint32_t>(72.0F), {}, floatMetadata}};
    std::vector<sc13::core::RuntimePropertyRecord> rebuilt;
    SC13_EXPECT(sc13::mods::ApplyPatchSequence(
        target, vanilla, *second->Find(target), rebuilt, error));
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(rebuilt, 0x09AE19D7U).value() == 350.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(vanilla, 0x09AE19D7U).value() == 300.0F);

    const auto generationBeforeFailure = second->generation();
    SC13_EXPECT(!registry.Activate(
        "mod.set", std::span(&setMaintenance, 1U), affected, error));
    SC13_EXPECT(registry.Current()->generation() == generationBeforeFailure);
    SC13_EXPECT(registry.Deactivate("mod.add", affected, error));
    SC13_EXPECT(registry.Current()->Find(target)->size() == 1U);

    sc13::mods::PatchRegistry nonConflicting;
    SC13_EXPECT(nonConflicting.Activate(
        "mod.maintenance", std::span(&setMaintenance, 1U), affected, error));
    SC13_EXPECT(nonConflicting.Activate(
        "mod.capacity", std::span(&multiplyCapacity, 1U), affected, error));
    SC13_EXPECT(nonConflicting.Current()->conflicts().empty());
    SC13_EXPECT(sc13::mods::ApplyPatchSequence(
        target, vanilla, *nonConflicting.Current()->Find(target), rebuilt, error));
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(rebuilt, 0x09AE19D7U).value() == 345.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(rebuilt, 0x0AFB9882U).value() == 144.0F);
}

/** @summary Validates generation retention, targeted invalidation, and vanilla restoration. */
void TestRuntimeResourceCache() {
    constexpr std::uint32_t floatMetadata = 0x000D0000U;
    const sc13::core::Tgi targetA{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const sc13::core::Tgi targetB{0x00B1B104U, 0x61EFC000U, 0x11111111U};
    const auto setMaintenance = MakeResourcePatch(
        targetA, 0x09AE19D7U, sc13::mods::PatchOperation::Set, 345.0F);
    sc13::mods::PatchRegistry registry;
    std::set<sc13::core::Tgi> affected;
    std::string error;
    SC13_EXPECT(registry.Activate(
        "mod.cache", std::span(&setMaintenance, 1U), affected, error));
    const auto snapshot = registry.Current();

    std::array<sc13::core::RuntimePropertyRecord, 1> vanillaA{
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, floatMetadata}};
    std::array<sc13::core::RuntimePropertyRecord, 1> vanillaB{
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(100.0F), {}, floatMetadata}};
    sc13::runtime::RuntimeResourceCache cache;
    sc13::runtime::RuntimeResourceBuildPlan planA;
    const sc13::runtime::RuntimeResourceKey keyA{targetA, 0x1000U};
    SC13_EXPECT(cache.Prepare(
        keyA, vanillaA, snapshot->generation(), *snapshot->Find(targetA), planA, error));
    SC13_EXPECT(
        planA.status == sc13::runtime::RuntimeResourceBuildStatus::Created);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(planA.desired, 0x09AE19D7U).value() == 345.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(vanillaA, 0x09AE19D7U).value() == 300.0F);
    SC13_EXPECT(cache.Commit(planA, error));

    sc13::runtime::RuntimeResourceBuildPlan stalePlan;
    SC13_EXPECT(cache.Prepare(
        keyA, planA.desired, snapshot->generation(),
        *snapshot->Find(targetA), stalePlan, error));
    SC13_EXPECT(cache.Invalidate(std::set<sc13::core::Tgi>{targetA}) == 1U);
    SC13_EXPECT(!cache.Commit(stalePlan, error));

    sc13::runtime::RuntimeResourceBuildPlan planB;
    const sc13::runtime::RuntimeResourceKey keyB{targetB, 0x2000U};
    SC13_EXPECT(cache.Prepare(
        keyB, vanillaB, snapshot->generation(), {}, planB, error));
    SC13_EXPECT(cache.Commit(planB, error));
    SC13_EXPECT(cache.size() == 2U);
    SC13_EXPECT(cache.Invalidate(std::set<sc13::core::Tgi>{targetA}) == 0U);

    sc13::runtime::RuntimeResourceBuildPlan reusedB;
    SC13_EXPECT(cache.Prepare(
        keyB, planB.desired, snapshot->generation() + 1U, {}, reusedB, error));
    SC13_EXPECT(
        reusedB.status == sc13::runtime::RuntimeResourceBuildStatus::Reused);

    SC13_EXPECT(registry.Deactivate("mod.cache", affected, error));
    sc13::runtime::RuntimeResourceBuildPlan restoredA;
    SC13_EXPECT(cache.Prepare(
        keyA, planA.desired, registry.Current()->generation(), {}, restoredA, error));
    SC13_EXPECT(
        restoredA.status == sc13::runtime::RuntimeResourceBuildStatus::Restored);
    SC13_EXPECT(
        sc13::core::ReadRuntimeFloat(restoredA.desired, 0x09AE19D7U).value() == 300.0F);
    SC13_EXPECT(cache.Commit(restoredA, error));
    const auto restoreEntries = cache.SnapshotForRestore();
    SC13_EXPECT(restoreEntries.size() == 2U);
    SC13_EXPECT(std::any_of(
        restoreEntries.begin(), restoreEntries.end(),
        [&keyA](const sc13::runtime::RuntimeResourceRestoreEntry& entry) {
            return entry.key == keyA && entry.vanilla.size() == 1U &&
                   entry.lastApplied.size() == 1U;
        }));
}

}
void TestPatchOwnership() { TestPreparedPublication(); TestPatchRegistry(); TestRuntimeResourceCache(); }
}
