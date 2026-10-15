#include "scripting/base_host.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include "mods/mod_definition_parser.hpp"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <algorithm>

namespace {
using namespace sc13::scripting;
int checks{};
/** @summary Fail immediately on a broken fixture or behavior assertion. */
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
/** @summary Require rejection rather than accidental execution. */
template<class F> void Reject(F function, const char* message) {
    bool rejected{}; try { function(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}
/** @summary Write only owned temporary synthetic source files. */
void Write(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc); file << bytes;
    Check(bool(file), "Fixture write failed");
}
/** @summary Produce the supported existing authoring schema without alternate role fields. */
std::string Manifest() {
    return "schemaVersion=2\nformat='simoder.authoring'\nkind='gameplay'\n"
        "[metadata]\nid='simoder.base'\nname='Base fixture'\nversion='0.1.0'\n"
        "[scripts]\nlanguage='luau'\nentry='scripts/init.luau'\nmemoryMiB=8\nbudgetMs=2\n";
}
/** @summary Supply trusted fixture hashes externally, never through package self-assertion. */
BasePackagePin Pin(const std::filesystem::path& root) {
    return {"0.1.0", {{"mod.toml", HashText(ReadBounded(root / "mod.toml", 4096))},
        {"scripts/init.luau", HashText(ReadBounded(root / "scripts/init.luau", 4096))},
        {"scripts/value.luau", HashText(ReadBounded(root / "scripts/value.luau", 4096))}}};
}
/** @summary Restore a harmless private fixture; no production service or readiness is declared. */
void Restore(const std::filesystem::path& root) {
    Write(root / "mod.toml", Manifest());
    Write(root / "scripts/init.luau", "assert(simoder.props == nil)\nassert(require('./value') == 42)\nreturn {onEnable=function() print('enabled') end,onDisable=function() print('disabled') end}");
    Write(root / "scripts/value.luau", "return 42");
}
}

/** @summary Exercise source provenance, reserved identity and shared-lane resource cleanup. */
int main(int argc, char** argv) {
    const auto root = std::filesystem::temp_directory_path() / ("simoder-base-host-" + std::to_string(GetCurrentProcessId()));
    try {
        std::filesystem::remove_all(root); Restore(root);
        const auto pin = Pin(root);
        std::vector<ScriptPhase> phases;
        std::uint64_t generation{};
        bool correctLane = true, correctGeneration = true;
        const auto observer = [&](ScriptPhase phase, std::uint64_t id) {
            correctLane = correctLane && ScriptRuntime::OnScriptLane();
            if (!generation) generation = id;
            correctGeneration = correctGeneration && id == generation && id != 0;
            phases.push_back(phase);
            if (phase == ScriptPhase::Create)
                Write(root / "scripts/value.luau", "error('disk mutation after verified capture')");
        };
        {
            BaseHost host(root, pin, observer);
            Check(host.Status().running && host.Status().generation != 0, "Verified fixture VM unavailable");
            Check(!host.DrainLogs().empty(), "Base lifecycle logs unavailable");
            Reject([&] { BaseHost second(root, pin); }, "Duplicate base host admitted");
            Write(root / "scripts/value.luau", "error('later untrusted bytes')");
            Check(host.Status().running, "Disk mutation altered immutable running VM");
        }
        Check(phases.front() == ScriptPhase::Create && phases.back() == ScriptPhase::Close, "Base lifecycle did not close");
        Check(correctLane && correctGeneration, "Base lifecycle lane or generation changed");
        Check(std::count(phases.begin(), phases.end(), ScriptPhase::Disable) == 1, "Base disable was not once-only");
        Restore(root);
        sc13::mods::ModDefinition ordinary; std::string error;
        Check(!sc13::mods::LoadModDefinition(root, ordinary, error), "Ordinary loader accepted reserved ID");
        Reject([&] { static_cast<void>(InspectSourceProject(root)); }, "Direct source compiler accepted reserved ID");
        ScriptDefinition script; script.bytecode = CompileLuauSource("return {}"); script.entry = "scripts/init.luau";
        Reject([&] { ScriptRuntime spoof("simoder.base", script); }, "Direct VM accepted reserved ID");
        auto bad = pin; bad.version = "different";
        Reject([&] { BaseHost host(root, bad); }, "Version mismatch accepted");
        bad = pin; bad.files.at("mod.toml")[0] = bad.files.at("mod.toml")[0] == '0' ? '1' : '0';
        Reject([&] { BaseHost host(root, bad); }, "Manifest hash mismatch accepted");
        bad = pin; bad.files.emplace("scripts/../escape.luau", std::string(64, '0'));
        Reject([&] { BaseHost host(root, bad); }, "Traversal accepted");
        bad = pin; bad.files.at("mod.toml") = std::string(64, 'a');
        Reject([&] { BaseHost host(root, bad); }, "Noncanonical digest accepted");
        bad = pin;
        for (unsigned i = 0; i < 65; ++i) bad.files.emplace("scripts/extra" + std::to_string(i) + ".luau", std::string(64, '0'));
        Reject([&] { BaseHost host(root, bad); }, "Too many pinned files accepted");
        Write(root / "scripts/value.luau", "return 41");
        Reject([&] { BaseHost host(root, pin); }, "Source tamper accepted");
        Restore(root); Write(root / "scripts/unexpected.luau", "return {}");
        Reject([&] { BaseHost host(root, pin); }, "Unexpected source accepted");
        std::filesystem::remove(root / "scripts/unexpected.luau");
        std::filesystem::remove(root / "scripts/value.luau");
        Reject([&] { BaseHost host(root, pin); }, "Missing source accepted");
        Restore(root);
        Write(root / "scripts/value.luau", std::string(4U * 1024U * 1024U, ' '));
        bad = pin; bad.files.at("scripts/value.luau") = HashText(std::string(4U * 1024U * 1024U, ' '));
        Reject([&] { BaseHost host(root, bad); }, "Source aggregate exceeded bound");
        Restore(root);
        std::filesystem::rename(root / "scripts/value.luau", root / "value-original.luau");
        const auto linked = CreateSymbolicLinkW((root / "scripts/value.luau").c_str(),
            (root / "value-original.luau").c_str(), 0x2U);
        if (linked) {
            Reject([&] { BaseHost host(root, pin); }, "Reparse source accepted despite matching content hash");
            std::filesystem::remove(root / "scripts/value.luau");
        } else std::cout << "REPARSE_LINK_FIXTURE_NOT_AVAILABLE Win32=" << GetLastError() << '\n';
        std::filesystem::rename(root / "value-original.luau", root / "scripts/value.luau");
        Write(root / ".simoder/cache/generation.toml", "self asserted bytecode cache");
        { BaseHost host(root, pin); Check(host.Status().running, "Untrusted cache changed verified compilation"); }
        Write(root / "scripts/init.luau", "this is not valid luau source"); bad = Pin(root);
        Write(root / ".simoder/cache/script.luau.bin", CompileLuauSource("return {}"));
        Reject([&] { BaseHost host(root, bad); }, "Cached valid bytecode bypassed compilation of verified invalid source");
        Restore(root);
        Write(root / "mod.toml", Manifest() + "[resources]\n");
        bad = Pin(root);
        Reject([&] { BaseHost host(root, bad); }, "Dormant resource configuration accepted");
        Restore(root);
        Write(root / "scripts/init.luau", "error('BASE_DAG_REQUIRED_MISSING:lifecycle')");
        bad = Pin(root);
        Reject([&] { BaseHost host(root, bad); }, "Critical startup failure accepted");
        Restore(root);
        auto wrongMemory = Manifest();
        wrongMemory.replace(wrongMemory.find("memoryMiB=8"), 11, "memoryMiB=9");
        Write(root / "mod.toml", wrongMemory); bad = Pin(root);
        Reject([&] { BaseHost host(root, bad); }, "Base memory configuration other than 8 MiB accepted");
        Restore(root);
        // Failed initialization must release both singleton and shared aggregate reservation.
        for (unsigned i = 0; i < 18; ++i) {
            Write(root / "scripts/init.luau", "error('expected initialization failure')");
            bad = Pin(root); Reject([&] { BaseHost host(root, bad); }, "VM failure accepted");
        }
        Restore(root);
        std::vector<std::unique_ptr<ScriptRuntime>> vms;
        script.memoryBytes = 64U * 1024U * 1024U;
        vms.push_back(std::make_unique<ScriptRuntime>("fixture.one", script));
        vms.push_back(std::make_unique<ScriptRuntime>("fixture.two", script));
        Reject([&] { BaseHost host(root, pin); }, "Base ignored shared 128 MiB reservation");
        vms.clear();
        { BaseHost host(root, pin); Check(host.Status().running, "Aggregate reservation leaked after failure"); }
        if (argc == 2) {
            const std::filesystem::path package(argv[1]);
            BasePackagePin production;
            production.version = InspectSourceManifest(ReadBounded(package / "mod.toml", 4096)).metadata.version;
            production.files.emplace("mod.toml", HashText(ReadBounded(package / "mod.toml", 4096)));
            for (const auto& file : std::filesystem::recursive_directory_iterator(package / "scripts")) {
                if (file.is_regular_file()) production.files.emplace(
                    file.path().lexically_relative(package).generic_string(), HashText(ReadBounded(file.path(), 4U * 1024U * 1024U)));
            }
            {
                BaseHost host(package, production);
                Check(host.Status().running && host.Ready(), "Real critical service graph did not qualify");
                auto scope = std::make_shared<sc13::runtime::RuntimeScope>();
                ScriptDefinition denial;
                denial.entry = "scripts/init.luau"; denial.budgetMs = 20;
                denial.bytecode = CompileLuauSource(R"(
                    return { onTick = function()
                        assert(table.isfrozen(base) and #base.descriptors() == 0)
                        local value = assert(base.values.validate('{"v":1,"schema":"fixture.v1","value":["null"]}', 'fixture.v1'))
                        local task, reason = base.request('fixture.unavailable', value)
                        assert(task == nil and reason == 'NotReady')
                    end }
                )");
                ScriptRuntime denied("fixture.production-denial", denial, host.Bind(scope));
                denied.Commit();
                denied.Tick(0);
                Check(denied.Status().running, "Base readiness granted unavailable production authority");
                denied.Stop();
            }
            // Test-only identity: source fixture is reviewed local test code, not a production pin supplier.
            ScriptDefinition dagFixture;
            dagFixture.entry = "scripts/init.luau"; dagFixture.budgetMs = 20;
            dagFixture.bytecode = CompileLuauSource(ReadBounded(package / "tests/runner.luau", 1024U * 1024U));
            dagFixture.modules.emplace("scripts/lib/dag.luau", CompileLuauSource(ReadBounded(package / "scripts/lib/dag.luau", 1024U * 1024U)));
            ScriptRuntime dag("fixture.dag", dagFixture);
            dag.Commit();
            Check(dag.Status().running, "Real DAG regression battery failed");
            dag.Stop();
            // @summary Run authored service units separately; their explicit primitive doubles are not native conformance evidence.
            ScriptDefinition servicesFixture;
            servicesFixture.entry = "scripts/init.luau"; servicesFixture.budgetMs = 20;
            servicesFixture.bytecode = CompileLuauSource(ReadBounded(package / "tests/services.luau", 1024U * 1024U));
            for (const auto& file : std::filesystem::recursive_directory_iterator(package / "scripts")) {
                if (file.is_regular_file() && file.path().extension() == ".luau") servicesFixture.modules.emplace(
                    file.path().lexically_relative(package).generic_string(), CompileLuauSource(ReadBounded(file.path(), 1024U * 1024U)));
            }
            ScriptRuntime services("fixture.service-units", servicesFixture);
            services.Commit();
            Check(services.Status().running, "Authored service unit battery failed");
            services.Stop();
        }
        std::filesystem::remove_all(root);
        std::cout << checks << " base host checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n'; std::filesystem::remove_all(root); return 1;
    }
}
