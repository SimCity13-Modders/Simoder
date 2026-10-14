#include "test_harness.hpp"
#include "binary_fixture.hpp"
#include "mod_fixture.hpp"
#include "dev/mod_validation.hpp"
#include "dev/runtime_diagnostics.hpp"
#include "core/json.hpp"
#include "formats/dbpf/dbpf_reader.hpp"
#include "formats/prop/prop_document.hpp"
#include "mods/mod_definition_parser.hpp"
#include "mods/mod_manager.hpp"
#include "mods/patch_registry.hpp"
#include "mods/resource_project.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <fstream>
#include <set>
#include <span>
#include <thread>
#include <vector>

namespace sc13::tests {
namespace {
/** @summary Creates a minimal uncompressed DBPF v3 package containing one PROP resource. */
[[nodiscard]] std::vector<std::byte> BuildSyntheticDbpf(
    const sc13::core::Tgi& tgi, std::span<const std::byte> resource, bool compressed) {
    constexpr std::size_t headerSize = 96;
    constexpr std::size_t indexEntrySize = 32;
    std::vector<std::byte> stored(resource.begin(), resource.end());
    if (compressed) {
        SC13_EXPECT(resource.size() == 4);
        stored = {
            std::byte{0x10}, std::byte{0xFB}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04},
            std::byte{0xE0}, resource[0], resource[1], resource[2], resource[3], std::byte{0xFC}};
    }
    const std::size_t indexOffset = headerSize + stored.size();
    std::vector<std::byte> bytes(indexOffset + 4 + indexEntrySize);
    bytes[0] = std::byte{'D'};
    bytes[1] = std::byte{'B'};
    bytes[2] = std::byte{'P'};
    bytes[3] = std::byte{'F'};
    WriteLittleU32(bytes, 0x04, 3);
    WriteLittleU32(bytes, 0x24, 1);
    WriteLittleU32(bytes, 0x2C, 4 + indexEntrySize);
    WriteLittleU32(bytes, 0x3C, 3);
    WriteLittleU32(bytes, 0x40, static_cast<std::uint32_t>(indexOffset));
    std::copy(stored.begin(), stored.end(), bytes.begin() + headerSize);

    std::size_t cursor = indexOffset;
    WriteLittleU32(bytes, cursor, 0);
    cursor += 4;
    WriteLittleU32(bytes, cursor, tgi.type);
    cursor += 4;
    WriteLittleU32(bytes, cursor, tgi.group);
    cursor += 4;
    WriteLittleU32(bytes, cursor, 0);
    cursor += 4;
    WriteLittleU32(bytes, cursor, tgi.instance);
    cursor += 4;
    WriteLittleU32(bytes, cursor, headerSize);
    cursor += 4;
    WriteLittleU32(bytes, cursor, static_cast<std::uint32_t>(stored.size()));
    cursor += 4;
    WriteLittleU32(bytes, cursor, static_cast<std::uint32_t>(resource.size()));
    cursor += 4;
    WriteLittleU16(bytes, cursor, compressed ? 0xFFFFU : 0x0000U);
    WriteLittleU16(bytes, cursor + 2, 1);
    return bytes;
}

/** @summary Exercises offline source ambiguity, runtime arithmetic, reload order and failure evidence. */
void TestDeveloperWorkflow() {
    const auto root = CreateTemporaryDirectory();
    const auto game = root / L"game";
    const auto modsPath = root / L"mods";
    std::filesystem::create_directory(game);
    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    std::vector<std::byte> prop;
    AppendBigU32(prop, 1); AppendBigU32(prop, 0x09AE19D7U);
    AppendBigU16(prop, 0x0DU); AppendBigU16(prop, 0);
    AppendBigU32(prop, std::bit_cast<std::uint32_t>(300.0F));
    const auto bytes = BuildSyntheticDbpf(target, prop, false);
    {
        std::ofstream output(game / L"vanilla.package", std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    WriteModFixture(modsPath / L"first", "fixture.first", "First", 345.0F);
    WriteModFixture(modsPath / L"second", "fixture.second", "Second", 5.0F);
    sc13::mods::ModDefinition first, second;
    std::string error;
    SC13_EXPECT(sc13::mods::LoadModDefinition(modsPath / L"first", first, error));
    SC13_EXPECT(sc13::mods::LoadModDefinition(modsPath / L"second", second, error));
    second.patches.front().properties.front().operation = sc13::mods::PatchOperation::Add;
    const std::array definitions{first, second};
    auto report = sc13::dev::ValidateMods(definitions, game);
    SC13_EXPECT(report.valid);
    SC13_EXPECT(report.steps.size() == 2U);
    SC13_EXPECT(report.steps.front().before == 300.0F);
    SC13_EXPECT(report.steps.back().after == 350.0F);
    SC13_EXPECT(report.conflicts.size() == 1U);
    SC13_EXPECT(report.resources.front().resourceSha256.size() == 64U);
    SC13_EXPECT(sc13::dev::ValidationJson(report).find("\"after\":350") != std::string::npos);
    std::filesystem::copy_file(game / L"vanilla.package", game / L"duplicate.package");
    report = sc13::dev::ValidateMods(definitions, game);
    SC13_EXPECT(!report.valid);
    SC13_EXPECT(report.resources.front().packages.size() == 2U);
    SC13_EXPECT(report.resources.front().error.find("Ambiguous") != std::string::npos);
    std::filesystem::remove(game / L"duplicate.package");
    auto missing = first;
    missing.patches.front().properties.front().propertyId = 1;
    report = sc13::dev::ValidateMods(std::span(&missing, 1), game);
    SC13_EXPECT(!report.valid);
    missing.patches.front().target.instance = 1;
    report = sc13::dev::ValidateMods(std::span(&missing, 1), game);
    SC13_EXPECT(!report.valid);
    SC13_EXPECT(report.resources.front().error == "Target resource not found");

    sc13::mods::PatchRegistry registry;
    sc13::runtime::RuntimeResourceCache cache;
    sc13::mods::ModManager manager(modsPath, root / L"state.toon", registry, cache);
    SC13_EXPECT(manager.Initialize(error));
    SC13_EXPECT(manager.SetEnabled("fixture.first", true, error));
    SC13_EXPECT(manager.SetEnabled("fixture.second", true, error));
    const auto oldSnapshot = registry.Current();
    const auto oldOrder = manager.EnabledOrder();
    const sc13::runtime::RuntimeResourceKey key{target, 1234};
    const std::array table{sc13::core::RuntimePropertyRecord{0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, 0x000D0000U}};
    sc13::runtime::RuntimeResourceBuildPlan plan;
    SC13_EXPECT(cache.Prepare(key, table, oldSnapshot->generation(), *oldSnapshot->Find(target), plan, error));
    SC13_EXPECT(cache.Commit(plan, error));
    SC13_EXPECT(cache.Observations().front().status == "applied");
    WriteModFixture(modsPath / L"first", "fixture.first", "First", 400.0F);
    SC13_EXPECT(manager.Refresh(error));
    SC13_EXPECT(manager.Snapshot().front().canReload);
    SC13_EXPECT(!manager.Reload("fixture.first", error, "stale-preview"));
    SC13_EXPECT(manager.Generation() == oldSnapshot->generation());
    SC13_EXPECT(manager.Reload("fixture.first", error));
    SC13_EXPECT(manager.EnabledOrder() == oldOrder);
    SC13_EXPECT(registry.Current()->generation() == oldSnapshot->generation() + 1U);
    SC13_EXPECT(registry.Current()->Find(target)->front().owner == "fixture.first");
    SC13_EXPECT(oldSnapshot->Find(target)->front().patch.properties.front().value == 345.0F);
    SC13_EXPECT(registry.Current()->Find(target)->front().patch.properties.front().value == 400.0F);
    SC13_EXPECT(cache.Observations().front().status == "pending");
    const auto stableGeneration = manager.Generation();
    WriteTextFile(modsPath / L"first" / L"mod.toml", "invalid TOML");
    SC13_EXPECT(!manager.Reload("fixture.first", error));
    SC13_EXPECT(manager.Generation() == stableGeneration);
    SC13_EXPECT(registry.IsActive("fixture.first"));
    WriteTextFile(modsPath / L"first" / L"mod.toml",
        FixtureHeader("fixture.first", "First") + FixturePatch(5, "0x01"));
    SC13_EXPECT(!manager.Reload("fixture.first", error));
    SC13_EXPECT(manager.Generation() == stableGeneration);
    cache.RecordFailure(key, stableGeneration, "fixture rejection");
    const auto diagnostic = sc13::dev::RuntimeDiagnosticsJson(manager, 123);
    SC13_EXPECT(diagnostic.find("fixture rejection") != std::string::npos);
    SC13_EXPECT(diagnostic.find("historical committed resource values") != std::string::npos);
    WriteModFixture(modsPath / L"first", "fixture.first", "First", 450.0F);
    const auto diagnosticPath = root / L"runtime-status.json";
    {
        sc13::dev::RuntimeDiagnostics watcher(manager, diagnosticPath);
        for (unsigned int attempt = 0; attempt < 100U && !std::filesystem::exists(diagnosticPath); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        SC13_EXPECT(std::filesystem::exists(diagnosticPath));
        SC13_EXPECT(manager.Snapshot().front().canReload);
        SC13_EXPECT(manager.Snapshot().front().pendingPatches.front().properties.front().value == 450.0F);
        SC13_EXPECT(manager.Generation() == stableGeneration);
    }
    SC13_EXPECT(!std::filesystem::exists(diagnosticPath));
    SC13_EXPECT(sc13::core::JsonString("a\n\"b") == "\"a\\u000a\\\"b\"");
    cache.Clear(); SC13_EXPECT(cache.Observations().empty());
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    SC13_EXPECT(!cleanupError);
}

/** @summary Validates DBPF header, index, TGI lookup, and exact extraction. */
void TestDbpf() {
    const sc13::core::Tgi tgi{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const std::vector<std::byte> resource{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto packageBytes = BuildSyntheticDbpf(tgi, resource, false);

    wchar_t temporaryDirectory[MAX_PATH]{};
    wchar_t temporaryFile[MAX_PATH]{};
    SC13_EXPECT(GetTempPathW(MAX_PATH, temporaryDirectory) != 0);
    SC13_EXPECT(GetTempFileNameW(temporaryDirectory, L"s13", 0, temporaryFile) != 0);
    {
        std::ofstream stream(temporaryFile, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(packageBytes.data()),
            static_cast<std::streamsize>(packageBytes.size()));
        SC13_EXPECT(static_cast<bool>(stream));
    }

    sc13::formats::dbpf::DbpfPackage package;
    std::string error;
    SC13_EXPECT(package.Open(temporaryFile, error));
    const auto* entry = package.Find(tgi);
    SC13_EXPECT(entry != nullptr);
    std::vector<std::byte> extracted;
    SC13_EXPECT(package.ReadResource(*entry, extracted, error));
    SC13_EXPECT(extracted == resource);

    const auto compressedPackageBytes = BuildSyntheticDbpf(tgi, resource, true);
    {
        std::ofstream stream(temporaryFile, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(compressedPackageBytes.data()),
            static_cast<std::streamsize>(compressedPackageBytes.size()));
        SC13_EXPECT(static_cast<bool>(stream));
    }
    SC13_EXPECT(package.Open(temporaryFile, error));
    entry = package.Find(tgi);
    SC13_EXPECT(entry != nullptr);
    SC13_EXPECT(entry->isCompressed());
    SC13_EXPECT(package.ReadResource(*entry, extracted, error));
    SC13_EXPECT(extracted == resource);
    SC13_EXPECT(DeleteFileW(temporaryFile) != FALSE);
}

/** @summary TOML import preserves wide IDs, validates payload hashes and blocks unsupported runtime activation. */
void TestResourceProject() {
    const auto root = CreateTemporaryDirectory();
    const auto directory = root / "mods" / "raw";
    std::filesystem::create_directories(directory / "assets");
    const std::string hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto payload = directory / "assets" / (hash+".bin");
    WriteTextFile(payload,"abc");
    const std::string manifest = "schemaVersion = 3\ncontract = \"simoder.raw-resources\"\nrequiredCapabilities = [\"resource.register\"]\n[metadata]\nid = \"raw\"\nname = \"Raw test\"\nversion = \"1.0\"\nauthor = \"CI\"\n[[resources]]\nid = \"00B1B104:00000001:1234567800000001\"\noperation = \"add\"\npayload = \"assets/"+hash+".bin\"\nsha256 = \""+hash+"\"\nsize = 3\n";
    WriteTextFile(directory / "mod.toml", manifest);
    sc13::mods::ModDefinition definition;
    std::string error;
    SC13_EXPECT(sc13::mods::LoadModDefinition(directory,definition,error));
    SC13_EXPECT(definition.resources.size()==1U);
    if (!definition.resources.empty()) SC13_EXPECT(definition.resources[0].id=="00B1B104:00000001:1234567800000001");
    SC13_EXPECT(!sc13::dev::ValidateMods(std::span(&definition,1U),{}).valid);
    sc13::mods::PatchRegistry registry;
    sc13::runtime::RuntimeResourceCache cache;
    sc13::mods::ModManager manager(root/"mods",root/"state.toon",registry,cache);
    SC13_EXPECT(manager.Initialize(error));
    const auto entries = manager.Snapshot();
    SC13_EXPECT(entries.size()==1U);
    if (!entries.empty()) {
        SC13_EXPECT(entries[0].name=="Raw test");
        SC13_EXPECT(!entries[0].canEnable);
        SC13_EXPECT(entries[0].diagnostic.find("resource.register")!=std::string::npos);
    }
    SC13_EXPECT(!registry.IsActive("raw"));
    WriteTextFile(payload,"bad");
    SC13_EXPECT(!sc13::mods::LoadModDefinition(directory,definition,error));
    SC13_EXPECT(error.find("SHA-256")!=std::string::npos);
    WriteTextFile(payload,"abc");
    std::filesystem::create_directories(directory/"assets"/"scripts");
    WriteTextFile(directory/"assets"/"scripts"/"example.json","abc");
    auto namedManifest = manifest;
    const auto originalPath = "assets/"+hash+".bin";
    namedManifest.replace(namedManifest.find(originalPath), originalPath.size(), "assets/scripts/example.json");
    WriteTextFile(directory/"mod.toml",namedManifest);
    SC13_EXPECT(sc13::mods::LoadModDefinition(directory,definition,error));
    auto unsafeManifest = namedManifest;
    const std::string namedPath = "assets/scripts/example.json";
    unsafeManifest.replace(unsafeManifest.find(namedPath), namedPath.size(), "assets/scripts/../../outside.bin");
    WriteTextFile(directory/"mod.toml",unsafeManifest);
    SC13_EXPECT(!sc13::mods::LoadModDefinition(directory,definition,error));
    SC13_EXPECT(error.find("Unsafe asset path")!=std::string::npos);
    WriteTextFile(directory/"mod.toml",manifest+"\n[[resources]]\nid = \"00B1B104:00000001:1234567800000001\"\noperation=\"add\"\npayload=\"../outside.bin\"\nsha256=\""+hash+"\"\nsize=3\n");
    SC13_EXPECT(!sc13::mods::LoadModDefinition(directory,definition,error));
    WriteTextFile(directory/"mod.toml",manifest);
    WriteTextFile(directory/"mod.toon","id: duplicate\n");
    SC13_EXPECT(!sc13::mods::LoadModDefinition(directory,definition,error));
    SC13_EXPECT(error.find("Ambiguous")!=std::string::npos);
    /** @summary Reject the retired installed filename even when its payload contract is valid. */
    SC13_EXPECT(std::filesystem::remove(directory / "mod.toon"));
    std::filesystem::rename(directory / "mod.toml", directory / "project.toml");
    SC13_EXPECT(!sc13::mods::LoadModDefinition(directory, definition, error));
    SC13_EXPECT(error.find("Missing mod.toml") != std::string::npos);
    std::filesystem::remove_all(root);
}

}
void TestResourceIntegration() { TestDbpf(); TestDeveloperWorkflow(); TestResourceProject(); }
}
