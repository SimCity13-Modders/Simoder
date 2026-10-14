#include "dev/service_supervisor.hpp"
#include "mods/mod_definition_parser.hpp"
#include "mods/mod_manager.hpp"
#include "mods/patch_registry.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include <Windows.h>
#include <TlHelp32.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
/** @summary Fail immediately with a behavior-level test diagnostic. */
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** @summary Write an owned manifest fixture without modifying game or installed mods. */
void Write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    Check(bool(file), "Cannot write fixture");
}
/** @summary Find only fixture services parented by the specified host.
 * @param owner Exact process identity whose direct children are inspected.
 * @return Captured service process identities.
 */
std::vector<DWORD> Children(DWORD owner = GetCurrentProcessId()) {
    std::vector<DWORD> result;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    Check(snapshot != INVALID_HANDLE_VALUE, "Cannot inspect fixture process ownership");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID == owner &&
            (_wcsicmp(entry.szExeFile, L"service-a.exe") == 0 || _wcsicmp(entry.szExeFile, L"service-b.exe") == 0))
            result.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return result;
}
/** @summary Await bounded termination of the exact captured child PIDs. */
void Ended(const std::vector<DWORD>& children) {
    for (const auto pid : children) {
        const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!process) {
            Check(GetLastError() == ERROR_INVALID_PARAMETER, "Cannot verify service termination");
            continue;
        }
        const auto ended = WaitForSingleObject(process, 5000);
        CloseHandle(process);
        Check(ended == WAIT_OBJECT_0, "Service survived owner teardown");
    }
}

/** @summary Own an isolated fixture host and clean it up even when an assertion fails. */
struct FixtureHost {
    PROCESS_INFORMATION process{};
    HANDLE ready{};
    /** @summary Terminate only the owned fixture and release its process/event handles. */
    ~FixtureHost() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
        if (ready) CloseHandle(ready);
    }
};

/** @summary Prove that OS job ownership survives abrupt owner exit without C++ destructors.
 * @param executable Current test host executable.
 * @param mod Valid two-service fixture package.
 */
void TestAbruptOwnerExit(const std::filesystem::path& executable, const std::filesystem::path& mod) {
    FixtureHost owner;
    const auto eventName = L"Local\\SimoderServiceReady-" + std::to_wstring(GetCurrentProcessId());
    owner.ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    Check(owner.ready && GetLastError() != ERROR_ALREADY_EXISTS, "Cannot create isolated readiness event");
    auto command = L"\"" + executable.native() + L"\" --fixture-owner \"" + mod.native() + L"\" " + eventName;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, mod.c_str(), &startup, &owner.process), "Cannot start isolated service owner");
    Check(WaitForSingleObject(owner.ready, 10000) == WAIT_OBJECT_0, "Isolated service owner did not become ready");
    const auto children = Children(owner.process.dwProcessId);
    Check(children.size() == 2, "Isolated owner did not create both service children");
    Check(TerminateProcess(owner.process.hProcess, 77), "Cannot simulate abrupt owner exit");
    Check(WaitForSingleObject(owner.process.hProcess, 5000) == WAIT_OBJECT_0, "Fixture owner survived forced exit");
    Ended(children);
}
}

/** @summary Exercise service validation, explicit enable/disable ownership and startup rollback. */
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--fixture-child") { Sleep(INFINITE); return 0; }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--fixture-owner") {
        sc13::mods::ModDefinition definition;
        std::string error;
        const std::filesystem::path mod(argv[2]);
        if (!sc13::mods::LoadModDefinition(mod, definition, error) || !definition.services) return 2;
        sc13::dev::ServiceSupervisor supervisor;
        if (!supervisor.Start(*definition.services, mod.parent_path().parent_path(), error)) return 3;
        const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
        if (!ready) return 4;
        const bool signaled = SetEvent(ready) != FALSE;
        CloseHandle(ready);
        if (!signaled) return 5;
        Sleep(INFINITE);
        return 0;
    }
    const auto root = std::filesystem::temp_directory_path() / ("simoder-service-tests-" + std::to_string(GetCurrentProcessId()));
    try {
        Check(!std::filesystem::exists(root), "Fixture root already exists");
        const auto mod = root / "mods/Developer";
        std::filesystem::create_directories(mod);
        std::array<wchar_t, 32768> image{};
        Check(GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size())) != 0, "Cannot locate test host");
        std::filesystem::copy_file(image.data(), mod / "service-a.exe");
        std::filesystem::copy_file(image.data(), mod / "service-b.exe");
        const std::string header = "schemaVersion=2\nformat='simoder.authoring'\nkind='developer-services'\nserviceApiVersion=1\n"
            "[metadata]\nid='fixture.services'\nname='Developer fixture'\nversion='1'\n";
        const std::string entries = "[[services]]\nid='b'\nexecutable='service-b.exe'\narguments=['--fixture-child']\ndependencies=['a']\n"
            "[[services]]\nid='a'\nexecutable='service-a.exe'\narguments=['--fixture-child']\n";
        Write(mod / "mod.toml", header + entries + "[tools]\ncompiler='service-a.exe'\n");
        sc13::mods::ModDefinition definition;
        std::string error;
        Check(sc13::mods::LoadModDefinition(mod, definition, error), error.c_str());
        Check(definition.services && definition.services->services.front().id == "a", "Dependency ordering failed");
        Check(Children().empty(), "Discovery launched a developer process");
        Check(sc13::dev::FindAuthoringTool("compiler").empty(), "Discovery granted authoring tools");
        std::vector<DWORD> destroyedChildren;
        {
            sc13::mods::PatchRegistry registry;
            sc13::runtime::RuntimeResourceCache cache;
            sc13::mods::ModManager manager(root / "mods", root / "state.toon", registry, cache);
            Check(manager.Initialize(error), error.c_str());
            Check(Children().empty(), "Inactive discovery started services");
            Check(!manager.DeveloperServicesActive(), "Inactive service mod granted runtime control");
            Check(manager.SetEnabled("fixture.services", true, error), error.c_str());
            const auto children = Children();
            Check(children.size() == 2, "Enabled mod did not own both children");
            Check(manager.DeveloperServicesActive(), "Enabled services were not observable");
            Check(sc13::dev::FindAuthoringTool("compiler") == mod / "service-a.exe", "Enabled service did not register its tool");
            /** @summary Simulate an isolated service crash and require immediate control revocation. */
            const HANDLE crashed = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, children.front());
            Check(crashed != nullptr, "Cannot open owned crash fixture");
            const bool terminated = TerminateProcess(crashed, 91) != FALSE;
            const auto crashedStatus = terminated ? WaitForSingleObject(crashed, 5000) : WAIT_FAILED;
            CloseHandle(crashed);
            Check(terminated && crashedStatus == WAIT_OBJECT_0, "Crash fixture did not terminate");
            Check(!manager.DeveloperServicesActive(), "Exited service retained runtime control");
            Check(sc13::dev::FindAuthoringTool("compiler").empty(), "Exited service retained authoring tools");
            Check(manager.Snapshot().front().diagnostic.find("code 91") != std::string::npos,
                "Service exit diagnostic lost the exact termination code");
            Check(Children().size() == 1, "Service crash caused automatic restart or lost the remaining child");
            Check(manager.SetEnabled("fixture.services", false, error), error.c_str());
            Ended(children);
            Check(!manager.DeveloperServicesActive(), "Disabled mod retained runtime control");
            Check(sc13::dev::FindAuthoringTool("compiler").empty(), "Disabled mod retained tool registration");
            Check(manager.SetEnabled("fixture.services", true, error), error.c_str());
            destroyedChildren = Children();
            Check(destroyedChildren.size() == 2, "Reenabled manager did not own its services");
        }
        Ended(destroyedChildren);
        TestAbruptOwnerExit(image.data(), mod);
        auto broken = *definition.services;
        broken.services.back().executable = mod / "missing.exe";
        sc13::dev::ServiceSupervisor supervisor;
        Check(!supervisor.Start(broken, root, error), "Partial startup unexpectedly succeeded");
        Ended(Children());
        Write(mod / "mod.toml", header + entries + "dependencies=['b']\n");
        Check(!sc13::mods::LoadModDefinition(mod, definition, error), "Cyclic dependency was accepted");
        Check(error.find("Cyclic") != std::string::npos, "Cycle diagnostic missing");
        Write(mod / "mod.toml", header + "[[services]]\nid='a'\nexecutable='../outside.exe'\n");
        Check(!sc13::mods::LoadModDefinition(mod, definition, error), "Executable traversal was accepted");
        /** @summary Invalid tool contracts must fail discovery without spawning processes or granting capabilities. */
        const auto rejectedTools = [&](std::string_view tools) {
            Write(mod / "mod.toml", header + entries + std::string(tools));
            Check(!sc13::mods::LoadModDefinition(mod, definition, error), "Invalid authoring tool contract accepted");
            Check(Children().empty(), "Invalid tool discovery started services");
            Check(sc13::dev::FindAuthoringTool("compiler").empty(), "Invalid tool discovery registered a compiler");
        };
        rejectedTools("[tools]\narbitrary='service-a.exe'\n");
        rejectedTools("[tools]\ncompiler='../outside.exe'\n");
        rejectedTools("[tools]\ncompiler=7\n");
        rejectedTools("[tools]\ncompiler='mod.toml'\n");
        rejectedTools("[tools]\ntypescript-toolchain='service-a.exe'\n");
        rejectedTools("[tools]\ntypescript-toolchain='missing-toolchain'\n");
        std::filesystem::remove_all(root);
        std::cout << "Service mods: validation, dependency order, explicit lifecycle and rollback passed.\n";
        return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
