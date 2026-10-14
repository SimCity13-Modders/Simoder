#include "test_harness.hpp"
#include "mod_fixture.hpp"
#include "config/loader_config.hpp"
#include "logging/log_event.hpp"
#include "mods/mod_state_store.hpp"
#include <Windows.h>
#include <vector>
#include <array>
#include <fstream>

namespace sc13::tests {
namespace {
/** @summary Validates generated defaults and typed DeveloperConsole configuration parsing. */
void TestLoaderConfig() {
    const std::filesystem::path directory = CreateTemporaryDirectory();
    sc13::config::LoaderConfig config;
    std::string error;
    SC13_EXPECT(sc13::config::LoadOrCreateConfig(directory, config, error));
    SC13_EXPECT(!config.developerConsole.enabled);
    SC13_EXPECT(config.developerConsole.captureGameLogs);
    SC13_EXPECT(std::filesystem::is_regular_file(directory / L"config.toon"));

    WriteTextFile(
        directory / L"config.toon",
        "developerConsole:\n"
        "  enabled: true\n"
        "  captureGameLogs: false\n"
        "  captureLoaderLogs: true\n"
        "  captureModLogs: true\n"
        "  level: debug\n");
    SC13_EXPECT(sc13::config::LoadOrCreateConfig(directory, config, error));
    SC13_EXPECT(config.developerConsole.enabled);
    SC13_EXPECT(!config.developerConsole.captureGameLogs);
    SC13_EXPECT(config.developerConsole.level == "debug");

    WriteTextFile(
        directory / L"config.toon",
        "developerConsole:\n"
        "  enabled: yes\n");
    SC13_EXPECT(!sc13::config::LoadOrCreateConfig(directory, config, error));
    SC13_EXPECT(!config.developerConsole.enabled);
    std::error_code removeError;
    std::filesystem::remove_all(directory, removeError);
    SC13_EXPECT(!removeError);
}

/** @summary Validates centralized semantic source labels and severity filtering. */
void TestLogEventClassification() {
    SC13_EXPECT(std::string_view(sc13::logging::SourceTypeName(
                    sc13::logging::SourceType::Game)) == "GAME");
    SC13_EXPECT(std::string_view(sc13::logging::SourceTypeName(
                    sc13::logging::SourceType::Loader)) == "LOADER");
    SC13_EXPECT(std::string_view(sc13::logging::SourceTypeName(
                    sc13::logging::SourceType::Mod)) == "MOD");
    sc13::logging::Level level = sc13::logging::Level::Error;
    SC13_EXPECT(sc13::logging::ParseLevel("debug", level));
    SC13_EXPECT(level == sc13::logging::Level::Debug);
    SC13_EXPECT(sc13::logging::PassesMinimum(
        sc13::logging::Level::Warning, sc13::logging::Level::Info));
    SC13_EXPECT(!sc13::logging::PassesMinimum(
        sc13::logging::Level::Trace, sc13::logging::Level::Info));
    SC13_EXPECT(!sc13::logging::ParseLevel("verbose", level));
}

/** @summary Validates atomic enabled-state persistence, ordering, and strict validation. */
void TestModStateStore() {
    const std::filesystem::path directory = CreateTemporaryDirectory();
    const std::filesystem::path statePath = directory / L"state.toon";
    std::vector<sc13::mods::ModId> enabled;
    std::string error;
    SC13_EXPECT(sc13::mods::LoadEnabledState(statePath, enabled, error));
    SC13_EXPECT(enabled.empty());

    const std::array<sc13::mods::ModId, 2> expected{
        "example.first", "example.second"};
    SC13_EXPECT(sc13::mods::SaveEnabledState(statePath, expected, error));
    SC13_EXPECT(!std::filesystem::exists(directory / L"state.toon.tmp"));
    SC13_EXPECT(sc13::mods::LoadEnabledState(statePath, enabled, error));
    SC13_EXPECT(enabled.size() == expected.size());
    SC13_EXPECT(enabled[0] == expected[0]);
    SC13_EXPECT(enabled[1] == expected[1]);

    WriteTextFile(statePath, "enabled[2]: duplicate.id,duplicate.id\n");
    SC13_EXPECT(!sc13::mods::LoadEnabledState(statePath, enabled, error));
    SC13_EXPECT(enabled.empty());

    const std::array<sc13::mods::ModId, 2> invalid{
        "valid.id", "Invalid ID"};
    SC13_EXPECT(!sc13::mods::SaveEnabledState(statePath, invalid, error));
    std::error_code removeError;
    std::filesystem::remove_all(directory, removeError);
    SC13_EXPECT(!removeError);
}

}
void TestConfiguration() { TestLoaderConfig(); TestLogEventClassification(); TestModStateStore(); }
}
