#include "mods/mod_definition_parser.hpp"
#include "mods/mod_manager.hpp"
#include "mods/patch_registry.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "scripting/script_runtime.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <bit>

namespace {
int checks{};
/** @summary Fails with an actionable assertion rather than continuing with an invalid VM fixture. */
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
/** @summary Read mutable operation diagnostics after argument evaluation without retaining a stale c_str pointer.
 * @param value Operation or assertion result.
 * @param message Diagnostic string whose storage may change while the operation executes.
 */
void Check(bool value, const std::string& message) { ++checks; if (!value) throw std::runtime_error(message); }
/** @summary Observe owned fixtures with Win32 extended paths independently of the production helper.
 * @param path Logical absolute or relative fixture path.
 * @return Extended drive or UNC path for filesystem access only.
 */
std::filesystem::path FixturePath(const std::filesystem::path& path) {
    auto value = std::filesystem::absolute(path).lexically_normal().native();
    if (value.starts_with(L"\\\\?\\")) return value;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
/** @summary Writes a complete synthetic fixture without any game data. */
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(FixturePath(path.parent_path()));
    std::ofstream file(FixturePath(path), std::ios::binary); file << text;
    Check(bool(file), "Could not write fixture");
}
/** @summary Defines an authoring project using the same contract as Mod Studio and discovery. */
std::string Manifest(std::string_view language = "luau", unsigned memory = 8, unsigned budget = 2) {
    return "schemaVersion=2\nformat=\"simoder.authoring\"\n[metadata]\nid=\"fixture.script\"\nname=\"Fixture\"\nversion=\"1\"\n[scripts]\nlanguage=\"" +
        std::string(language) + "\"\nentry=\"scripts/main." + (language == "luau" ? "luau" : "ts") + "\"\nmemoryMiB=" +
        std::to_string(memory) + "\nbudgetMs=" + std::to_string(budget) + "\n"
        "[[symbols.resources]]\nname='sanitizer'\ntype=0x00B1B104\ngroup=0x61EFC000\ninstance=0x719436BD\nsource='synthetic'\n"
        "[[symbols.properties]]\nname='cost'\nid=0x09AE19D7\ntype='float'\nsource='synthetic'\n";
}
/** @summary Prepare source through its production compiler path and validate the immutable generation. */
sc13::mods::ModDefinition Load(const std::filesystem::path& mod) {
    sc13::mods::ModDefinition result; std::string error;
    if (!sc13::mods::LoadModDefinition(mod, result, error)) throw std::runtime_error(error);
    Check(bool(result.script), "Missing compiled script"); return result;
}
/** @summary Deterministically exercise transient/persistent share denial and non-replacing publication.
 * @param root Owned fixture root for publication operations only.
 */
void VerifyPublication(const std::filesystem::path& root) {
    const auto staging = root / "staging", destination = root / "generation";
    Write(staging / "owned.txt", "immutable-source");
    /** @summary Hold a data-reading directory handle that participates in share-delete enforcement. */
    const auto denyDelete = [&] {
        return CreateFileW(FixturePath(staging).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    };
    auto locked = denyDelete();
    Check(locked != INVALID_HANDLE_VALUE, "Cannot prepare transient publication denial");
    const auto transientStarted = std::chrono::steady_clock::now();
    {
        std::jthread release([locked] { Sleep(150); CloseHandle(locked); });
        Check(sc13::scripting::PublishGeneration(staging, destination), "Transient publication denial was not recovered");
        Check(std::chrono::steady_clock::now() - transientStarted >= std::chrono::milliseconds(100),
            "Transient share-denial fixture did not block publication");
    }
    Check(sc13::scripting::ReadBounded(destination / "owned.txt", 128) == "immutable-source",
        "Transient publication altered staged bytes");
    const auto priorTime = std::filesystem::last_write_time(FixturePath(destination / "owned.txt"));
    Write(staging / "owned.txt", "new-source");
    Check(!sc13::scripting::PublishGeneration(staging, destination), "Existing generation was replaced");
    Check(sc13::scripting::ReadBounded(destination / "owned.txt", 128) == "immutable-source",
        "Collision overwrote an existing generation");
    const auto absent = root / "absent";
    locked = denyDelete();
    Check(locked != INVALID_HANDLE_VALUE, "Cannot prepare persistent publication denial");
    const auto started = std::chrono::steady_clock::now();
    bool rejected{}; std::string publicationError;
    try { static_cast<void>(sc13::scripting::PublishGeneration(staging, absent)); }
    catch (const std::exception& error) {
        const std::string message(error.what());
        publicationError = message;
        rejected = (message.find("Win32 error 32") != std::string::npos || message.find("Win32 error 5") != std::string::npos) &&
            message.find("attempts=") != std::string::npos;
    }
    CloseHandle(locked);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Check(rejected && elapsed >= std::chrono::milliseconds(400) && elapsed < std::chrono::seconds(2),
        "Persistent publication denial was not bounded with an actionable diagnostic: " + publicationError +
        "; elapsedMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
    Check(!std::filesystem::exists(FixturePath(absent)) &&
        sc13::scripting::ReadBounded(staging / "owned.txt", 128) == "new-source",
        "Failed publication changed source or created a partial generation");
    Check(std::filesystem::last_write_time(FixturePath(destination / "owned.txt")) == priorTime,
        "Persistent denial changed a prior generation");
    std::filesystem::remove_all(FixturePath(staging));
    Check(!std::filesystem::exists(FixturePath(staging)), "Owned failed staging was not removable after handles closed");
    Write(staging / "owned.txt", "new-source");
    std::filesystem::create_directory(FixturePath(absent));
    Check(!sc13::scripting::PublishGeneration(staging, absent) && std::filesystem::is_empty(FixturePath(absent)),
        "Empty destination was overwritten");
    bool differentParent{};
    try { static_cast<void>(sc13::scripting::PublishGeneration(staging, root / "other" / "generation")); }
    catch (const std::exception&) { differentParent = true; }
    Check(differentParent, "Publication accepted a different parent");
    const auto mod = root / "Empty";
    Write(mod / "mod.toml", Manifest());
    Write(mod / "scripts/main.luau", "return {}");
    const auto source = sc13::scripting::InspectSourceProject(mod);
    const auto emptyGeneration = root / "cache" / source.projectKey / source.fingerprint;
    std::filesystem::create_directories(FixturePath(emptyGeneration));
    bool emptyRejected{};
    try { sc13::scripting::CompileLuauGeneration(source, emptyGeneration); }
    catch (const std::exception& error) { emptyRejected = std::string(error.what()).find("Existing compiled generation rejected") != std::string::npos; }
    Check(emptyRejected && std::filesystem::is_empty(FixturePath(emptyGeneration)),
        "Compiler accepted or modified an empty invalid generation collision");
    for (const auto& item : std::filesystem::directory_iterator(FixturePath(emptyGeneration.parent_path())))
        Check(!item.path().filename().native().starts_with(L"staging-"), "Collision validation leaked unpublished staging");
}
/** @summary Exercise deep cache creation, reuse, publication races, integrity checks and failed staging cleanup.
 * @param root Owned fixture root; source paths remain below the legacy Win32 limit.
 */
void VerifyDeepCache(const std::filesystem::path& root) {
    auto parent = root / "deep";
    while (parent.native().size() < 195) parent /= "padding123";
    const auto mod = parent / "Mod";
    Check((mod / "scripts/main.luau").native().size() < MAX_PATH,
        "Deep-cache fixture requires a source path below MAX_PATH");
    Write(mod / "mod.toml", Manifest());
    Write(mod / "scripts/helper.luau", "return 'deep-module-ok'");
    Write(mod / "scripts/main.luau", "local value=require('./helper'); return {onEnable=function() simoder.log(value) end}");
    const auto source = sc13::scripting::InspectSourceProject(mod);
    const auto projectCache = parent / "cache" / source.projectKey;
    const auto generation = projectCache / source.fingerprint;
    Check(projectCache.native().size() >= MAX_PATH && generation.native().size() >= MAX_PATH,
        "Regression did not cross cache-directory MAX_PATH boundary");
    const auto definition = Load(mod);
    const auto descriptor = generation / "generation.toml";
    Check(std::filesystem::is_regular_file(FixturePath(descriptor)), "Deep cache descriptor missing");
    Check(!std::filesystem::exists(FixturePath(mod / "cache")), "Deep cache was written inside the mod");
    const auto time = std::filesystem::last_write_time(FixturePath(descriptor));
    Check(Load(mod).script->bytecode == definition.script->bytecode, "Deep cache reuse changed bytecode");
    Check(std::filesystem::last_write_time(FixturePath(descriptor)) == time, "Deep cache reuse rebuilt the generation");
    sc13::scripting::ScriptRuntime runtime("deep", *definition.script);
    runtime.Commit();
    Check(runtime.DrainLogs() == std::vector<std::string>{"deep-module-ok"}, "Deep cached module did not resolve");
    runtime.Stop();
    sc13::scripting::CompileLuauGeneration(source, generation);
    Check(std::filesystem::last_write_time(FixturePath(descriptor)) == time,
        "Publication collision replaced an existing immutable generation");
    Write(generation / "script.luau.bin", "corrupted-bytecode");
    sc13::mods::ModDefinition rejected; std::string error;
    Check(!sc13::mods::LoadModDefinition(mod, rejected, error) && error.find("checksum mismatch") != std::string::npos,
        "Deep cache checksum corruption was accepted");
    bool corruptCollision{};
    try { sc13::scripting::CompileLuauGeneration(source, generation); }
    catch (const std::exception& exception) { corruptCollision = std::string(exception.what()).find("checksum mismatch") != std::string::npos; }
    Check(corruptCollision, "Publication collision bypassed full generation validation");
    Write(generation / "script.luau.bin", definition.script->bytecode);
    Check(Load(mod).script->bytecode == definition.script->bytecode, "Restored deep generation was not reusable");
    Write(mod / "scripts/main.luau", "return {broken syntax");
    Check(!sc13::mods::LoadModDefinition(mod, rejected, error), "Invalid deep-cache source compiled");
    for (const auto& item : std::filesystem::directory_iterator(FixturePath(projectCache)))
        Check(!item.path().filename().native().starts_with(L"staging-"), "Unpublished deep compiler staging leaked");
    Check(std::filesystem::last_write_time(FixturePath(descriptor)) == time,
        "Failed compilation damaged the prior immutable generation");
}
/** @summary Confirms VM failures are contained, bounded and reported by the actual interpreter. */
void Reject(const std::filesystem::path& mod, std::string_view source, const char* expected) {
    Write(mod / "scripts/main.luau", source);
    const auto definition = Load(mod);
    const auto start = std::chrono::steady_clock::now();
    bool rejected{};
    try { sc13::scripting::ScriptRuntime runtime("fixture.script", *definition.script); }
    catch (const std::exception& error) { rejected = std::string_view(error.what()).find(expected) != std::string_view::npos; }
    Check(rejected, "Script was not rejected with the expected diagnostic");
    Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "VM failure exceeded containment deadline");
}
/** @summary Waits for a real scheduler callback with a hard deadline instead of relying on timing guesses. */
template<class Predicate> void Eventually(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(predicate(), message);
}
/** @summary Executes the actual VM/API/registry/cache pipeline with a synthetic owned table instead of game memory. */
void VerifyPropApi(const std::filesystem::path& root) {
    const auto mod = root / "mods/Api";
    Write(mod / "mod.toml", Manifest());

    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const sc13::runtime::RuntimeResourceKey key{target, 0x1000};
    std::vector<sc13::core::RuntimePropertyRecord> table{{0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, 0x000D0000U}};
    sc13::mods::PatchRegistry registry; sc13::runtime::RuntimeResourceCache cache;
    std::mutex tableMutex;
    std::string error;
    sc13::runtime::RuntimeResourceBuildPlan initial;
    Check(cache.Prepare(key, table, 0, {}, initial, error) && cache.Commit(initial, error), "Cannot seed API evidence");
    /** @summary Reuses the production transaction protocol, substituting only the native memory write. */
    const auto refresh = [&](const std::set<sc13::core::Tgi>& targets, std::string& reason) {
        if (!targets.contains(target)) return true;
        const std::scoped_lock lock(tableMutex);
        const auto snapshot = registry.Current();
        const auto* found = snapshot->Find(target);
        sc13::runtime::RuntimeResourceBuildPlan plan;
        if (!cache.Prepare(key, table, snapshot->generation(), found ? std::span(*found) : std::span<const sc13::mods::RegisteredPatch>{}, plan, reason)) return false;
        table = plan.desired; return cache.Commit(plan, reason);
    };
    const auto value = [&] { return cache.ReadObservedFloat(target, 0x09AE19D7U)->value; };
    Write(mod / "scripts/main.luau", "assert(simoder.hasCapability('game.props.float')); local v=simoder.props.getFloat('sanitizer','cost'); assert(v.value==300 and v.vanilla==300 and v.historical); return {onEnable=function() simoder.props.setFloat('sanitizer','cost',345) end, onTick=function() simoder.props.setFloat('sanitizer','cost',345) end}");
    sc13::mods::ModManager manager(root / "mods", root / "state.toon", registry, cache, refresh);
    Check(manager.Initialize(error) && manager.SetEnabled("fixture.script", true, error), error);
    Check(value() == 345, "Staged named override was not applied");
    const auto generation = registry.Current()->generation();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(registry.Current()->generation() == generation, "Identical tick writes churned generations");
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('sanitizer','cost',350) end, onTick=function() simoder.props.setFloat('sanitizer','cost',999); error('discard-batch') end}");
    Check(manager.Refresh(error) && manager.Reload("fixture.script", error), error);
    Eventually([&] { return !manager.Snapshot().front().scriptRunning; }, "Tick fault was not stopped");
    Check(value() == 350, "Failed callback leaked its queued patch");
    const auto active = manager.Snapshot().front().scriptRevision;
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('sanitizer','cost',999); error('initialization') end}");
    Check(manager.Refresh(error) && !manager.Reload("fixture.script", error), "Failed initialization replaced owned patches");
    Check(value() == 350 && manager.Snapshot().front().scriptRevision == active, "Rejected initialization damaged the previous generation");
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('sanitizer',0x12345678,700) end}");
    Check(manager.Refresh(error) && !manager.Reload("fixture.script", error), "Missing observed property was accepted");
    Check(value() == 350, "Failed preflight changed the observed resource");
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('sanitizer','cost',360) end, onTick=function() simoder.props.clearFloat('sanitizer','cost') end}");
    Check(manager.Refresh(error) && manager.Reload("fixture.script", error), error);
    Eventually([&] { return value() == 300; }, "Clear did not restore the vanilla-derived result");
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('sanitizer','cost',365) end}");
    Check(manager.Refresh(error) && manager.Reload("fixture.script", error), error);
    Check(value() == 365, "Final scripted override missing");
    Check(manager.SetEnabled("fixture.script", false, error) && value() == 300, "Disable failed to remove owned script patches");
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('sanitizer','cost',math.huge) end}");
    Check(manager.Refresh(error) && !manager.SetEnabled("fixture.script", true, error), "Non-finite float accepted");
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('00B1B104:61EFC000:00000001719436BD','cost',1) end}");
    Check(manager.Refresh(error) && !manager.SetEnabled("fixture.script", true, error), "64-bit instance was silently truncated");
#ifdef SIMODER_TEST_TYPESCRIPT_AUTHORING
    Write(mod / "mod.toml", Manifest("typescript"));
    Write(mod / "scripts/main.ts", "export function onEnable() { const v=simoder.props.getFloat('sanitizer','cost'); if (!v || !v.historical) throw 'missing'; simoder.props.setFloat('sanitizer','cost',370); }");
    Check(manager.Refresh(error) && manager.SetEnabled("fixture.script", true, error), error);
    Check(value() == 370, "TypeScript PROP bindings used a method receiver or lost data");
    Check(manager.SetEnabled("fixture.script", false, error) && value() == 300, "TypeScript override was not restored");
#endif
    Write(mod / "mod.toml", Manifest());
    Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('sanitizer','cost',-0.0) end, onTick=function() simoder.props.setFloat('sanitizer','cost',0.0) end}");
    Check(manager.Refresh(error) && manager.SetEnabled("fixture.script", true, error), error);
    Eventually([&] { return std::bit_cast<std::uint32_t>(value()) == 0; }, "Positive zero was deduplicated against negative zero");
    Check(manager.SetEnabled("fixture.script", false, error) && value() == 300, "Signed-zero override was not restored");
    const auto other = root / "mods/Other";
    auto otherManifest = Manifest();
    otherManifest.replace(otherManifest.find("fixture.script"), std::string("fixture.script").size(), "fixture.other");
    Write(other / "mod.toml", otherManifest);
    Write(other / "scripts/main.luau", "return {onEnable=function() simoder.props.setFloat('00B1B104:61EFC000:00000000719436BD',0x09AE19D7,400) end}");
    Check(manager.Refresh(error) && manager.SetEnabled("fixture.script", true, error) && manager.SetEnabled("fixture.other", true, error), error);
    Check(value() == 400 && !manager.Conflicts().empty(), "Activation order or script conflict reporting failed");
    Check(manager.SetEnabled("fixture.script", false, error) && value() == 400, "Disabling an earlier owner removed another owner's override");
    Check(manager.SetEnabled("fixture.other", false, error) && value() == 300, "Final owner removal did not restore vanilla");
    const sc13::runtime::RuntimeResourceKey secondKey{target, 0x2000};
    auto conflictingTable = table;
    conflictingTable.front().valueBits = std::bit_cast<std::uint32_t>(301.0F);
    sc13::runtime::RuntimeResourceBuildPlan conflictingPlan;
    Check(cache.Prepare(secondKey, conflictingTable, registry.Current()->generation(), {}, conflictingPlan, error) && cache.Commit(conflictingPlan, error), "Cannot seed ambiguity fixture");
    bool ambiguous{};
    try { static_cast<void>(cache.ReadObservedFloat(target, 0x09AE19D7U)); }
    catch (const std::exception&) { ambiguous = true; }
    Check(ambiguous, "Inconsistent observed instances were silently collapsed");
}
}

/** @summary Covers compiler cache, language adapters, isolation, modules, lifecycle and transactional HotReload. */
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("simoder-script-tests-" + std::to_string(GetCurrentProcessId()));
    try {
        Check(!std::filesystem::exists(root), "Fixture directory already exists");
        const auto mod = root / "mods/Script";
        Write(mod / "mod.toml", Manifest());
        const auto unicodeModule = std::filesystem::path(u8"scripts/nested/źródło.luau");
        Write(mod / unicodeModule, "return {message='module-ok'}");
        Write(mod / "scripts/nested/helper.luau", "return {message=function() return require('./źródło').message end}");
        Write(mod / "scripts/main.luau", "local helper=require('./nested/helper'); return {onEnable=function() simoder.log(helper.message()) end, onDisable=function() simoder.log('stop-ok') end}");
        const auto definition = Load(mod);
        const auto source = sc13::scripting::InspectSourceProject(mod);
        const auto cache = root / "mods/cache" / source.projectKey / source.fingerprint;
        Check(std::filesystem::exists(FixturePath(cache / "generation.toml")), "Cache was not outside the mod");
        Check(!std::filesystem::exists(FixturePath(mod / "cache")), "Compiler wrote a project-local cache");
        const auto time = std::filesystem::last_write_time(FixturePath(cache / "generation.toml"));
        Check(Load(mod).script->bytecode == definition.script->bytecode, "Repeated compilation changed bytecode");
        Check(std::filesystem::last_write_time(FixturePath(cache / "generation.toml")) == time, "Unchanged generation was rebuilt");
        sc13::scripting::ScriptRuntime runtime("fixture.script", *definition.script);
        Check(runtime.DrainLogs().empty(), "Staged initialization leaked logs");
        runtime.Commit();
        Check(runtime.DrainLogs() == std::vector<std::string>{"module-ok"}, "Local module or staged log failed");
        runtime.Stop();
        Check(runtime.DrainLogs() == std::vector<std::string>{"stop-ok"}, "onDisable was not called");
        runtime.Stop(); Check(runtime.DrainLogs().empty(), "onDisable was called twice");

        Reject(mod, "return {onEnable=function() while true do end end}", "budget exceeded");
        Reject(mod, "simoder.log=nil; return {}", "readonly");
        Reject(mod, "return {onEnable=function() require('../../outside') end}", "not found");
        Write(mod / "mod.toml", Manifest("luau", 2, 20));
        Reject(mod, "return {onEnable=function() local x=string.rep('x', 3*1024*1024) end}", "memory");
        Write(mod / "mod.toml", Manifest());
        Write(mod / "scripts/main.luau", "assert(io==nil and package==nil and debug==nil and os.execute==nil and loadstring==nil and getfenv==nil); privateValue=(privateValue or 0)+1; return {onEnable=function() simoder.log(tostring(privateValue)); assert(not simoder.hasCapability('game.city')) end}");
        const auto isolated = Load(mod);
        sc13::scripting::ScriptRuntime first("first", *isolated.script), second("second", *isolated.script);
        first.Commit(); second.Commit();
        Check(first.DrainLogs() == std::vector<std::string>{"1"} && second.DrainLogs() == std::vector<std::string>{"1"}, "VM globals leaked between mods");

        Write(mod / "scripts/main.luau", "return {onTick=function() error('tick-fault') end}");
        sc13::scripting::ScriptRuntime faulty("faulty", *Load(mod).script);
        faulty.Commit(); faulty.Tick(0.05);
        Check(!faulty.Status().running && faulty.Status().diagnostic.find("tick-fault") != std::string::npos, "Tick error was not isolated");

        Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.log('active') end}");
        sc13::mods::PatchRegistry registry; sc13::runtime::RuntimeResourceCache resources;
        {
            sc13::mods::ModManager manager(root / "mods", root / "state.toon", registry, resources);
            std::string error;
            Check(manager.Initialize(error), error);
            Check(manager.Snapshot().size() == 1, "Cache appeared as another mod");
            Check(manager.SetEnabled("fixture.script", true, error), error);
            const auto active = manager.Snapshot().front().scriptRevision;
            Write(mod / "scripts/main.luau", "return {onEnable=function() error('reload-fault') end}");
            Check(manager.Refresh(error), error);
            Check(manager.Snapshot().front().canReload, "Valid pending bytecode was not previewed");
            Check(!manager.Reload("fixture.script", error), "Faulty initialization replaced the active script");
            Check(manager.Snapshot().front().scriptRevision == active && manager.Snapshot().front().scriptRunning, "Rejected reload damaged the active VM");
            Write(mod / "scripts/main.luau", "return {onEnable=function() simoder.log('next') end}");
            Check(manager.Refresh(error), error);
            const auto preview = manager.Snapshot().front().pendingRevision;
            Check(!manager.Reload("fixture.script", error, "stale"), "Stale preview was accepted");
            Check(manager.Reload("fixture.script", error, preview), error);
            Check(manager.Snapshot().front().scriptRevision != active, "New generation was not published");
            Write(mod / "scripts/main.luau", "return {broken syntax");
            Check(manager.Refresh(error), error);
            Check(manager.Snapshot().front().scriptRunning && !manager.Snapshot().front().canReload, "Invalid source discarded active script");
            Check(manager.SetEnabled("fixture.script", false, error), error);
            Check(!manager.Snapshot().front().scriptRunning, "Disable left script scheduled");
        }
#ifdef SIMODER_TEST_TYPESCRIPT_AUTHORING
        Write(mod / "mod.toml", Manifest("typescript"));
        Write(mod / "scripts/main.ts", "import {message} from './helper'; export function onEnable() { simoder.log(message); }");
        Write(mod / "scripts/helper.ts", "export const message = ['typescript', 'ok'].join('-');");
        const auto typescript = Load(mod);
        sc13::scripting::ScriptRuntime tsRuntime("typescript", *typescript.script);
        tsRuntime.Commit();
        Check(tsRuntime.DrainLogs() == std::vector<std::string>{"typescript-ok"}, "Transpiled TypeScript bundle did not execute in Luau");
        Check(typescript.script->language == "typescript", "Source-language provenance lost");
#endif
        VerifyPropApi(root / "api");
        VerifyPublication(root / "publication");
        VerifyDeepCache(root);
        std::filesystem::remove_all(sc13::scripting::NativePath(root));
        std::cout << checks << " scripting checks passed (production compilation and x86 Luau VM).\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scripting test failed: " << error.what() << "\nRetained fixture: " << root << '\n'; return 1;
    }
}
