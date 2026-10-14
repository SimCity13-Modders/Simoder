#include "test_harness.hpp"
#include "mod_fixture.hpp"
#include "mods/mod_manager.hpp"
#include "mods/mod_state_store.hpp"
#include "mods/patch_registry.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace sc13::tests {
namespace {
/** @summary Validates discovery, persistence, changed files, removal, and failure isolation. */
void TestModManager() {
    const std::filesystem::path root = CreateTemporaryDirectory();
    const std::filesystem::path modsDirectory = root / L"mods";
    const std::filesystem::path statePath = root / L"simoder" / L"state.toon";
    const std::filesystem::path sanitizer = modsDirectory / L"BetterSanitizer";
    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 345.0F);

    /** @summary Compiled cache content must not become a duplicate mod or failed discovery, regardless of case. */
    const std::filesystem::path sharedCache = modsDirectory / L"CaChE";
    WriteModFixture(
        sharedCache, "dawid.better-sanitizer", "Cached Sanitizer", 999.0F);
    WriteModFixture(
        sharedCache / L"generation", "cache.generation", "Cached Generation", 999.0F);

    sc13::mods::PatchRegistry registry;
    sc13::runtime::RuntimeResourceCache cache;
    std::size_t runtimeRefreshCount = 0U;
    std::set<sc13::core::Tgi> lastRuntimeRefresh;
    sc13::mods::ModManager manager(
        modsDirectory, statePath, registry, cache,
        [&runtimeRefreshCount, &lastRuntimeRefresh](
            const std::set<sc13::core::Tgi>& affected,
            std::string& refreshError) {
            ++runtimeRefreshCount;
            lastRuntimeRefresh = affected;
            refreshError.clear();
            return true;
        });
    std::string error;
    SC13_EXPECT(manager.Initialize(error));
    auto entries = manager.Snapshot();
    SC13_EXPECT(entries.size() == 1U);
    SC13_EXPECT(entries.front().state == sc13::mods::ModState::Inactive);
    SC13_EXPECT(entries.front().canEnable);

    /** @summary Invalid cached manifests also remain invisible during refresh and do not affect valid mods. */
    WriteTextFile(sharedCache / L"mod.toml", "invalid TOML");
    SC13_EXPECT(manager.Refresh(error));
    SC13_EXPECT(manager.Snapshot().size() == 1U);

    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", true, error));
    SC13_EXPECT(registry.IsActive("dawid.better-sanitizer"));
    SC13_EXPECT(manager.EnabledOrder().size() == 1U);
    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    SC13_EXPECT(runtimeRefreshCount == 1U);
    WriteTextFile(sanitizer / L"mod.toml", FixtureHeader("dawid.better-sanitizer", "Better Sanitizer") +
        FixturePatch(345) + "[[symbols.properties]]\nname='cost'\nid=0x09AE19D7\nsource='fixture'\n");
    SC13_EXPECT(manager.Refresh(error));
    SC13_EXPECT(manager.Snapshot().front().state == sc13::mods::ModState::Changed);
    WriteModFixture(sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 345);
    SC13_EXPECT(manager.Refresh(error));
    SC13_EXPECT(lastRuntimeRefresh.contains(target));
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        345.0F);

    {
        sc13::mods::PatchRegistry restartedRegistry;
        sc13::runtime::RuntimeResourceCache restartedCache;
        sc13::mods::ModManager restarted(
            modsDirectory, statePath, restartedRegistry, restartedCache);
        SC13_EXPECT(restarted.Initialize(error));
        SC13_EXPECT(restartedRegistry.IsActive("dawid.better-sanitizer"));
        SC13_EXPECT(restarted.EnabledOrder().size() == 1U);
    }

    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 360.0F);
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    SC13_EXPECT(entries.front().state == sc13::mods::ModState::Changed);
    SC13_EXPECT(entries.front().changed);
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        345.0F);

    WriteTextFile(sanitizer / L"mod.toml", FixtureHeader("dawid.better-sanitizer", "Better Sanitizer") + "patches = malformed\n");
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    SC13_EXPECT(entries.front().state == sc13::mods::ModState::Changed);
    SC13_EXPECT(registry.IsActive("dawid.better-sanitizer"));
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        345.0F);

    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 360.0F);
    SC13_EXPECT(manager.Refresh(error));

    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", false, error));
    SC13_EXPECT(!registry.IsActive("dawid.better-sanitizer"));
    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", true, error));
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        360.0F);

    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", false, error));
    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 400.0F);
    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", true, error));
    SC13_EXPECT(runtimeRefreshCount == 5U);
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        400.0F);

    WriteTextFile(sanitizer / L"mod.toml", FixtureHeader("dawid.better-sanitizer", "Better Sanitizer") + "patches = malformed\n");
    SC13_EXPECT(manager.Refresh(error));
    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", false, error));
    entries = manager.Snapshot();
    SC13_EXPECT(entries.front().state == sc13::mods::ModState::Failed);
    SC13_EXPECT(!manager.SetEnabled("dawid.better-sanitizer", true, error));
    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 360.0F);
    SC13_EXPECT(manager.Refresh(error));
    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", true, error));

    const std::filesystem::path invalid = modsDirectory / L"BrokenMod";
    std::error_code directoryError;
    std::filesystem::create_directories(invalid, directoryError);
    SC13_EXPECT(!directoryError);
    WriteTextFile(invalid / L"mod.toml", "invalid TOML");
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    SC13_EXPECT(entries.size() == 2U);
    SC13_EXPECT(registry.IsActive("dawid.better-sanitizer"));

    std::filesystem::remove_all(sanitizer, directoryError);
    SC13_EXPECT(!directoryError);
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    SC13_EXPECT(!registry.IsActive("dawid.better-sanitizer"));
    SC13_EXPECT(manager.EnabledOrder().empty());
    SC13_EXPECT(std::any_of(
        entries.begin(), entries.end(), [](const sc13::mods::ModUiEntry& entry) {
            return entry.id == "dawid.better-sanitizer" &&
                   entry.state == sc13::mods::ModState::Missing;
        }));

    WriteModFixture(
        modsDirectory / L"DuplicateOne", "test.duplicate", "Duplicate One", 410.0F);
    WriteModFixture(
        modsDirectory / L"DuplicateTwo", "test.duplicate", "Duplicate Two", 420.0F);
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    const auto duplicateFailures = std::count_if(
        entries.begin(), entries.end(), [](const sc13::mods::ModUiEntry& entry) {
            return entry.id == "test.duplicate" &&
                   entry.state == sc13::mods::ModState::Failed &&
                   entry.diagnostic.find("DuplicateOne") != std::string::npos &&
                   entry.diagnostic.find("DuplicateTwo") != std::string::npos;
        });
    SC13_EXPECT(duplicateFailures == 2);

    std::vector<sc13::mods::ModId> persisted;
    SC13_EXPECT(sc13::mods::LoadEnabledState(statePath, persisted, error));
    SC13_EXPECT(persisted.empty());
    std::filesystem::remove_all(root, directoryError);
    SC13_EXPECT(!directoryError);
}

}
void TestModLifecycle() { TestModManager(); }
}
