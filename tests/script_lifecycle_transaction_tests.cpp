#include "mods/mod_manager.hpp"
#include "mods/patch_registry.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
/** @summary Fails a behavioral assertion. @param value Observed condition. @param message Failure description. */
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
/** @summary Supports owned compiled-cache fixtures beyond MAX_PATH. @param path Owned fixture path. @return Extended absolute Win32 path. */
std::filesystem::path Extended(const std::filesystem::path& path) {
    auto value = std::filesystem::absolute(path).lexically_normal().native();
    if (value.starts_with(L"\\\\?\\")) return value;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
/** @summary Writes a complete synthetic project file. @param path Owned fixture path. @param text UTF-8 contents. */
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << text;
    Require(bool(out), "Cannot write lifecycle fixture");
}
/** @summary Checks reentrant diagnostics, failed persistence and exactly-once retirement without game state. */
void Lifecycle() {
    const auto root = std::filesystem::current_path() / ("u3c-lifecycle-fixture-" + std::to_string(GetCurrentProcessId()));
    Require(!std::filesystem::exists(root), "Fixture path already exists; preserve it for inspection");
    const auto mod = root / "mods" / "fixture";
    const auto state = root / "state.toon";
    Write(mod / "mod.toml", "schemaVersion=2\nformat='simoder.authoring'\n[metadata]\nid='fixture.lifecycle'\nname='Lifecycle'\nversion='1'\n[scripts]\nlanguage='luau'\nentry='scripts/main.luau'\nmemoryMiB=2\nbudgetMs=2\n");
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.log('enabled') end, onDisable=function() simoder.log('retired') end}");
    sc13::mods::PatchRegistry registry;
    sc13::runtime::RuntimeResourceCache cache;
    std::mutex mutex;
    std::condition_variable changed;
    unsigned enabled{}, retired{};
    std::atomic_bool sinkFailure{};
    sc13::mods::ModManager* observed{};
    {
        sc13::mods::ModManager manager(root / "mods", state, registry, cache, {},
            [&](const std::string&, const std::string& message) {
                if (!observed) { sinkFailure = true; return; }
                // @summary Snapshot reentry would deadlock if a lifecycle wait retained the manager mutex.
                const auto snapshot = observed->Snapshot();
                if (snapshot.size() != 1) sinkFailure = true;
                if (message == "retired") {
                    std::string error;
                    // @summary A lane-origin mutation must fail rather than wait for its own lane or transaction.
                    if (observed->SetEnabled("fixture.lifecycle", true, error) || error.empty()) sinkFailure = true;
                }
                { std::lock_guard lock(mutex); if (message == "enabled") ++enabled; if (message == "retired") ++retired; }
                changed.notify_all();
            });
        observed = &manager;
        std::string error;
        Require(manager.Initialize(error), error);
        Require(manager.SetEnabled("fixture.lifecycle", true, error), error);
        {
            std::unique_lock lock(mutex);
            Require(changed.wait_for(lock, std::chrono::seconds(3), [&] { return enabled == 1; }), "Enable diagnostic stalled/reentered manager lock");
        }
        const auto blocked = CreateFileW(state.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(blocked != INVALID_HANDLE_VALUE, "Cannot reserve persistence denial fixture");
        const auto disabled = manager.SetEnabled("fixture.lifecycle", false, error);
        CloseHandle(blocked);
        Require(!disabled && !error.empty(), "Persistence denial accepted a disable");
        Require(manager.Snapshot().front().scriptRunning && registry.IsActive("fixture.lifecycle"), "Failed disable retired the active generation");
        { std::lock_guard lock(mutex); Require(retired == 0, "Failed disable called onDisable"); }
        Require(manager.SetEnabled("fixture.lifecycle", false, error), error);
        {
            std::unique_lock lock(mutex);
            Require(changed.wait_for(lock, std::chrono::seconds(3), [&] { return retired == 1; }), "Retirement diagnostic stalled/reentered transaction");
        }
        Require(!sinkFailure && !registry.IsActive("fixture.lifecycle"), "Reentrant lifecycle sink failed");
    }
    Require(retired == 1 && !sinkFailure, "Shutdown repeated retirement");
    std::filesystem::remove_all(Extended(root));
}
}
/** @summary Executes synthetic transactional lifecycle regression. @return Zero on success. */
int main() {
    try { Lifecycle(); std::cout << "script lifecycle transaction PASS\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
