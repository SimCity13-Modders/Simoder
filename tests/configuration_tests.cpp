#include "test_harness.hpp"
#include "mod_fixture.hpp"
#include "config/loader_config.hpp"
#include "logging/log_event.hpp"
#include "mods/mod_state_store.hpp"
#include "platform/file_publication_retry.hpp"
#include <Windows.h>
#include <vector>
#include <array>
#include <fstream>
#include <chrono>
#include <future>
#include <iostream>
#include <utility>

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

/** @summary Verifies exact retry classification/limits without changing a production clock or syscall provider. */
void TestPublicationRetryPolicy() {
    using sc13::platform::FilePublicationRetryDelay;
    SC13_EXPECT(FilePublicationRetryDelay(ERROR_ACCESS_DENIED, 1, 0) == 50);
    SC13_EXPECT(FilePublicationRetryDelay(ERROR_SHARING_VIOLATION, 10, 499) == 1);
    SC13_EXPECT(FilePublicationRetryDelay(ERROR_ACCESS_DENIED, 11, 0) == 0);
    SC13_EXPECT(FilePublicationRetryDelay(ERROR_ACCESS_DENIED, 1, 500) == 0);
    SC13_EXPECT(FilePublicationRetryDelay(ERROR_ACCESS_DENIED, 0, 0) == 0);
    SC13_EXPECT(FilePublicationRetryDelay(ERROR_PATH_NOT_FOUND, 1, 0) == 0);
    SC13_EXPECT(FilePublicationRetryDelay(ERROR_INVALID_PARAMETER, 1, 0) == 0);
}

/** @summary Exercises real transient/persistent Win32 denial while preserving committed state and cleanup semantics. */
void TestStatePublicationDenial() {
    using namespace std::chrono_literals;
    const auto directory = CreateTemporaryDirectory();
    const auto path = directory / L"state.toon";
    const auto temporary = directory / L"state.toon.tmp";
    const std::array<sc13::mods::ModId, 1> original{"fixture.original"};
    const std::array<sc13::mods::ModId, 1> updated{"fixture.updated"};
    std::vector<sc13::mods::ModId> loaded;
    std::string error;
    const bool seeded = sc13::mods::SaveEnabledState(path, original, error);
    SC13_EXPECT(seeded);
    if (!seeded) return;

    // @summary A read-only owned fixture causes actual ERROR_ACCESS_DENIED without changing any ACL.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const bool readOnly = attributes != INVALID_FILE_ATTRIBUTES &&
        SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_READONLY) != FALSE;
    SC13_EXPECT(readOnly);
    if (readOnly) {
        const auto started = std::chrono::steady_clock::now();
        const bool written = sc13::mods::SaveEnabledState(path, updated, error);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        std::cout << "Read-only state publication: " << error << '\n';
        SC13_EXPECT(!written);
        SC13_EXPECT(error.find("Win32 error 5;") != std::string::npos);
        SC13_EXPECT(error.find("attempts=") != std::string::npos && error.find("elapsedMs=") != std::string::npos);
        SC13_EXPECT(elapsed >= 450ms && elapsed < 2s);
        SC13_EXPECT(sc13::mods::LoadEnabledState(path, loaded, error) && loaded == std::vector<sc13::mods::ModId>(original.begin(), original.end()));
        SC13_EXPECT(!std::filesystem::exists(temporary));
        SC13_EXPECT(SetFileAttributesW(path.c_str(), attributes) != FALSE);
    }

    // @summary Hold a real deny-delete reader across writer entry, then release it within the retry budget.
    HANDLE denyDelete = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SC13_EXPECT(denyDelete != INVALID_HANDLE_VALUE);
    if (denyDelete != INVALID_HANDLE_VALUE) {
        std::promise<void> entered;
        auto enteredSignal = entered.get_future();
        auto writer = std::async(std::launch::async, [&] {
            std::string reason;
            entered.set_value();
            const bool success = sc13::mods::SaveEnabledState(path, updated, reason);
            return std::pair{success, reason};
        });
        SC13_EXPECT(enteredSignal.wait_for(2s) == std::future_status::ready);
        // @summary Observe that denial is being retried rather than adding a sleep to hide a failed result.
        SC13_EXPECT(writer.wait_for(100ms) == std::future_status::timeout);
        SC13_EXPECT(CloseHandle(denyDelete) != FALSE);
        const auto result = writer.get();
        std::cout << "Transient sharing state publication: success=" << result.first << "; error=" << result.second << '\n';
        SC13_EXPECT(result.first && result.second.empty());
        SC13_EXPECT(sc13::mods::LoadEnabledState(path, loaded, error) && loaded == std::vector<sc13::mods::ModId>(updated.begin(), updated.end()));
        SC13_EXPECT(!std::filesystem::exists(temporary));
    }

    // @summary Persistent sharing denial must exhaust the same bounded policy and leave the latest state intact.
    denyDelete = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SC13_EXPECT(denyDelete != INVALID_HANDLE_VALUE);
    if (denyDelete != INVALID_HANDLE_VALUE) {
        const auto started = std::chrono::steady_clock::now();
        SC13_EXPECT(!sc13::mods::SaveEnabledState(path, original, error));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        std::cout << "Persistent sharing state publication: " << error << '\n';
        SC13_EXPECT(error.find("Win32 error 5;") != std::string::npos || error.find("Win32 error 32;") != std::string::npos);
        SC13_EXPECT(error.find("attempts=") != std::string::npos && elapsed >= 450ms && elapsed < 2s);
        SC13_EXPECT(CloseHandle(denyDelete) != FALSE);
        SC13_EXPECT(sc13::mods::LoadEnabledState(path, loaded, error) && loaded == std::vector<sc13::mods::ModId>(updated.begin(), updated.end()));
        SC13_EXPECT(!std::filesystem::exists(temporary));
    }
    std::error_code cleanup;
    std::filesystem::remove_all(directory, cleanup);
    SC13_EXPECT(!cleanup);
}

}
void TestConfiguration() { TestLoaderConfig(); TestLogEventClassification(); TestModStateStore(); TestPublicationRetryPolicy(); TestStatePublicationDenial(); }
}
