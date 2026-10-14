#include "scripting/script_patch_reducer.hpp"
#include "mods/mod_manager.hpp"

#include "mods/mod_definition_parser.hpp"
#include "mods/resource_project.hpp"
#include "mods/mod_state_store.hpp"
#include "mods/patch_registry.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "scripting/source_project.hpp"

#include <Windows.h>

#include <algorithm>
#include <set>
#include <system_error>
#include <utility>
#include <stdexcept>
#include <bit>

namespace sc13::mods {
namespace {

/** Appends a diagnostic without discarding earlier independent failures. */
void AppendError(std::string& destination, std::string_view message) {
    if (!destination.empty()) {
        destination += "; ";
    }
    destination += message;
}

/** Returns a stable narrow display form for one filesystem path. */
[[nodiscard]] std::string DisplayPath(const std::filesystem::path& path) {
    try {
        return path.filename().string();
    } catch (...) {
        return "<unrepresentable-path>";
    }
}

/** @summary Reserves the shared compiled cache outside mod sources, using Windows case-insensitive naming. */
[[nodiscard]] bool IsSharedModCache(const std::filesystem::path& path) {
    const auto name = path.filename().native();
    return CompareStringOrdinal(name.c_str(), -1, L"cache", -1, TRUE) == CSTR_EQUAL;
}

/** Erases one ID from activation order while preserving all other ordering. */
void EraseEnabled(std::vector<ModId>& order, const ModId& id) {
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
}

/** @summary Collects exact targets from effective declarative and scripted operations. */
std::set<core::Tgi> PatchTargets(std::span<const ResourcePatchDefinition> patches) {
    std::set<core::Tgi> result;
    for (const auto& patch : patches) result.insert(patch.target);
    return result;
}

}  // namespace

ModManager::ModManager(
    std::filesystem::path modsDirectory,
    std::filesystem::path statePath,
    PatchRegistry& registry,
    runtime::RuntimeResourceCache& cache,
    RuntimeResourceRefreshCallback runtimeRefresh, scripting::ScriptScheduler::LogSink scriptLog)
    : modsDirectory_(std::move(modsDirectory)),
      statePath_(std::move(statePath)),
      registry_(registry),
      cache_(cache),
      runtimeRefresh_(std::move(runtimeRefresh)),
      scriptScheduler_(std::make_unique<scripting::ScriptScheduler>(std::move(scriptLog),
          [this](const auto& id, const auto& runtime, const auto& commands, std::string& error) {
              return ApplyScriptCommands(id, runtime, commands, error);
          })),
      actionWorker_(std::make_unique<ActionWorker>([this](const ModAction& action) {
          std::string error;
          const bool success = action.kind == "refresh" ? Refresh(error) :
              action.kind == "reload" ? Reload(action.id, error, action.revision) :
              SetEnabled(action.id, action.kind == "enable", error);
          static_cast<void>(Snapshot());
          return success ? action.kind + " completed: " + action.id : action.kind + " failed: " + error;
      })) {}

ModManager::~ModManager() {
    actionWorker_.reset();
    scriptScheduler_->Shutdown();
    for (auto& [id, record] : records_) {
        if (record.scriptRuntime) record.scriptRuntime->Stop();
        scriptScheduler_->Flush(id, record.scriptRuntime);
    }
    scriptScheduler_.reset();
}

bool ModManager::Initialize(std::string& error) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        error.clear();
        if (initialized_) {
            error = "Mod manager is already initialized";
            return false;
        }

        std::vector<ModId> requested;
        std::string stateError;
        const bool stateValid = LoadEnabledState(statePath_, requested, stateError);
        enabledOrder_.clear();
        std::string refreshError;
        const bool refreshValid = RefreshLocked(refreshError);
        if (!stateValid) {
            AppendError(error, stateError);
        }
        if (!refreshValid) {
            AppendError(error, refreshError);
        }

        if (stateValid) {
            for (const ModId& id : requested) {
                auto record = records_.find(id);
                if (record == records_.end() || record->second.state != ModState::Inactive) {
                    AppendError(error, "Persisted mod '" + id + "' is unavailable");
                    continue;
                }
                std::string activationError;
                if (!EnableLocked(record->second, false, activationError)) {
                    record->second.state = ModState::Failed;
                    record->second.diagnostic = activationError;
                    AppendError(error, "Mod '" + id + "': " + activationError);
                }
            }
        }
        std::string persistenceError;
        if (!PersistOrder(enabledOrder_, persistenceError)) {
            AppendError(error, persistenceError);
        }
        initialized_ = true;
        return error.empty();
    } catch (...) {
        error = "Mod manager initialization failed due to an allocation or I/O exception";
        return false;
    }
}

bool ModManager::Refresh(std::string& error) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        if (!initialized_) {
            error = "Mod manager must be initialized before refresh";
            return false;
        }
        return RefreshLocked(error);
    } catch (...) {
        error = "Mod refresh failed due to an allocation or I/O exception";
        return false;
    }
}

bool ModManager::RefreshLocked(std::string& error) {
    error.clear();
    std::error_code directoryError;
    std::filesystem::create_directories(modsDirectory_, directoryError);
    if (directoryError) {
        error = "Could not create or access the mods directory";
        return false;
    }

    failedDiscoveries_.clear();
    std::map<std::filesystem::path, std::string> failedPaths;
    std::vector<std::filesystem::path> directories;
    for (std::filesystem::directory_iterator iterator(modsDirectory_, directoryError), end;
         !directoryError && iterator != end; iterator.increment(directoryError)) {
        if (IsSharedModCache(iterator->path())) {
            continue;
        }
        std::error_code statusError;
        const std::filesystem::file_status status = iterator->symlink_status(statusError);
        if (statusError) {
            failedDiscoveries_.push_back(ModUiEntry{
                {}, DisplayPath(iterator->path()), {}, {}, ModState::Failed,
                false, false, false, "Could not inspect directory entry"});
            continue;
        }
        const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
        if (std::filesystem::is_symlink(status) ||
            (attributes != INVALID_FILE_ATTRIBUTES &&
             (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)) {
            failedDiscoveries_.push_back(ModUiEntry{
                {}, DisplayPath(iterator->path()), {}, {}, ModState::Failed,
                false, false, false, "Symbolic links and reparse targets are not loaded"});
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            directories.push_back(iterator->path());
        }
    }
    if (directoryError) {
        error = "Could not enumerate the mods directory";
        return false;
    }
    std::sort(directories.begin(), directories.end());

    std::map<ModId, std::vector<Candidate>, std::less<>> grouped;
    for (const std::filesystem::path& directory : directories) {
        Candidate candidate;
        std::string candidateError;
        if (!LoadStableCandidate(
                directory, candidate.definition, candidate.fingerprint, candidateError)) {
            failedPaths.emplace(directory, candidateError);
            failedDiscoveries_.push_back(ModUiEntry{
                {}, DisplayPath(directory), {}, {}, ModState::Failed,
                false, false, false, std::move(candidateError)});
            continue;
        }
        const auto resourceDiagnostic = ResourceRuntimeDiagnostic(candidate.definition);
        if (!resourceDiagnostic.empty()) {
            failedPaths.emplace(directory, resourceDiagnostic);
            const auto& manifest = candidate.definition.manifest;
            failedDiscoveries_.push_back(ModUiEntry{manifest.id, manifest.name, manifest.version,
                manifest.author, ModState::Failed, false, false, false, resourceDiagnostic});
            continue;
        }
        grouped[candidate.definition.manifest.id].push_back(std::move(candidate));
    }

    std::map<ModId, Candidate, std::less<>> unique;
    std::set<ModId, std::less<>> duplicateIds;
    for (auto& [id, candidates] : grouped) {
        if (candidates.size() != 1U) {
            duplicateIds.insert(id);
            std::string paths;
            for (const Candidate& candidate : candidates) {
                if (!paths.empty()) {
                    paths += "; ";
                }
                paths += candidate.definition.directory.string();
            }
            for (const Candidate& candidate : candidates) {
                failedDiscoveries_.push_back(ModUiEntry{
                    id, candidate.definition.manifest.name,
                    candidate.definition.manifest.version,
                    candidate.definition.manifest.author, ModState::Failed,
                    false, false, false,
                    "Duplicate mod ID '" + id + "' paths: " + paths});
            }
            continue;
        }
        unique.emplace(id, std::move(candidates.front()));
    }

    bool persistenceChanged = false;
    for (auto iterator = records_.begin(); iterator != records_.end();) {
        auto candidate = unique.find(iterator->first);
        Record& record = iterator->second;
        const bool registered = registry_.IsActive(iterator->first);
        if (candidate == unique.end()) {
            if (registered) {
                const auto failedPath = failedPaths.find(record.definition.directory);
                if (failedPath != failedPaths.end() || duplicateIds.contains(iterator->first)) {
                    record.state = ModState::Changed;
                    record.pendingDefinition.reset();
                    record.pendingFingerprint.reset();
                    record.diagnostic = failedPath != failedPaths.end()
                        ? "Changed files are invalid; active definition retained: " +
                              failedPath->second
                        : "Duplicate mod ID detected; active definition retained until OFF";
                    ++iterator;
                    continue;
                }
                std::set<core::Tgi> affected = PatchTargets(EffectivePatches(record));
                const std::size_t invalidatedBefore = cache_.Invalidate(affected);
                static_cast<void>(invalidatedBefore);
                std::string deactivateError;
                if (registry_.Deactivate(iterator->first, affected, deactivateError)) {
                    scriptScheduler_->Remove(iterator->first);
                    if (record.scriptRuntime) record.scriptRuntime->Stop();
                    scriptScheduler_->Flush(iterator->first, record.scriptRuntime);
                    record.scriptRuntime.reset();
                    record.scriptPatches.clear();
                    const std::size_t invalidatedAfter = cache_.Invalidate(affected);
                    static_cast<void>(invalidatedAfter);
                    EraseEnabled(enabledOrder_, iterator->first);
                    persistenceChanged = true;
                    record.state = ModState::Missing;
                    record.pendingDefinition.reset();
                    record.pendingFingerprint.reset();
                    record.diagnostic = "Active mod directory is missing or no longer valid";
                    RefreshRuntimeResourcesLocked(
                        affected, record, "removed-mod restoration");
                    ++iterator;
                    continue;
                }
                record.state = ModState::Failed;
                record.diagnostic = "Failed to unload missing mod: " + deactivateError;
                AppendError(error, record.diagnostic);
                ++iterator;
                continue;
            }
            iterator = records_.erase(iterator);
            continue;
        }

        if (registered) {
            if (record.fingerprint != candidate->second.fingerprint) {
                record.pendingDefinition = std::move(candidate->second.definition);
                record.pendingFingerprint = candidate->second.fingerprint;
                record.state = ModState::Changed;
                record.diagnostic =
                    "Files changed while active; preview and Reload, or switch OFF/ON";
            } else {
                record.pendingDefinition.reset();
                record.pendingFingerprint.reset();
                if (record.state == ModState::Changed) record.diagnostic.clear();
                record.state = ModState::Active;
            }
        } else {
            record.definition = std::move(candidate->second.definition);
            record.fingerprint = candidate->second.fingerprint;
            record.pendingDefinition.reset();
            record.pendingFingerprint.reset();
            record.state = ModState::Inactive;
            record.diagnostic.clear();
        }
        unique.erase(candidate);
        ++iterator;
    }

    for (auto& [id, candidate] : unique) {
        Record record;
        record.definition = std::move(candidate.definition);
        record.fingerprint = candidate.fingerprint;
        record.state = ModState::Inactive;
        records_.emplace(id, std::move(record));
    }
    if (persistenceChanged) {
        std::string persistenceError;
        if (!PersistOrder(enabledOrder_, persistenceError)) {
            AppendError(error, persistenceError);
        }
    }
    return error.empty();
}

std::vector<ResourcePatchDefinition> ModManager::EffectivePatches(const Record& record) {
    auto result = record.definition.patches;
    result.insert(result.end(), record.scriptPatches.begin(), record.scriptPatches.end());
    return result;
}

scripting::ScriptHost ModManager::ScriptHost() {
    return {[this](const core::Tgi& target, std::uint32_t property) { return cache_.ReadObservedFloat(target, property); }};
}

bool ModManager::ValidateScriptPatchesLocked(const ModId& owner,
    const std::vector<ResourcePatchDefinition>& candidate, std::string& error) const {
    auto targets = PatchTargets(candidate);
    if (const auto found = records_.find(owner); found != records_.end()) {
        const auto previous = PatchTargets(EffectivePatches(found->second));
        targets.insert(previous.begin(), previous.end());
    }
    if (targets.empty()) return true;
    for (const auto& entry : cache_.SnapshotForTargets(targets)) {
        std::vector<RegisteredPatch> patches;
        std::uint64_t sequence{};
        bool inserted{};
        for (const auto& id : enabledOrder_) {
            const auto effective = id == owner ? candidate : EffectivePatches(records_.at(id));
            if (id == owner) inserted = true;
            for (const auto& patch : effective)
                if (patch.target == entry.key.target) patches.push_back({id, patch, sequence});
            ++sequence;
        }
        if (!inserted) for (const auto& patch : candidate)
            if (patch.target == entry.key.target) patches.push_back({owner, patch, sequence});
        std::vector<core::RuntimePropertyRecord> result;
        if (!ApplyPatchSequence(entry.key.target, entry.vanilla, patches, result, error)) return false;
    }
    return true;
}

bool ModManager::ApplyScriptCommands(const ModId& id,
    const std::shared_ptr<scripting::ScriptRuntime>& runtime,
    const std::vector<scripting::ScriptPatchCommand>& commands, std::string& error) {
    try {
        const std::scoped_lock lock(mutex_);
        const auto found = records_.find(id);
        /** @summary Discard an in-flight callback from a disabled or superseded VM without touching its successor. */
        if (found == records_.end() || found->second.scriptRuntime != runtime || !registry_.IsActive(id)) return true;
        auto& record = found->second;
        auto candidate = scripting::ReduceScriptPatches(record.scriptPatches, commands);
        bool equal = candidate.size() == record.scriptPatches.size();
        for (std::size_t i = 0; equal && i < candidate.size(); ++i) {
            const auto& a = candidate[i]; const auto& b = record.scriptPatches[i];
            equal = a.target == b.target && a.properties.size() == b.properties.size();
            for (std::size_t j = 0; equal && j < a.properties.size(); ++j)
                equal = a.properties[j].propertyId == b.properties[j].propertyId &&
                    std::bit_cast<std::uint32_t>(a.properties[j].value) == std::bit_cast<std::uint32_t>(b.properties[j].value);
        }
        if (equal) return true;
        auto effective = record.definition.patches;
        effective.insert(effective.end(), candidate.begin(), candidate.end());
        if (!ValidateScriptPatchesLocked(id, effective, error)) return false;
        auto affected = PatchTargets(EffectivePatches(record));
        const auto added = PatchTargets(effective); affected.insert(added.begin(), added.end());
        static_cast<void>(cache_.Invalidate(affected));
        if (!registry_.Replace(id, effective, affected, error)) return false;
        record.scriptPatches = std::move(candidate);
        static_cast<void>(cache_.Invalidate(affected));
        RefreshRuntimeResourcesLocked(affected, record, "script update");
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
    catch (...) { error = "Script PROP transaction failed safely"; return false; }
}

bool ModManager::EnableLocked(
    Record& record,
    bool persist,
    std::string& error) {
    error.clear();
    if (record.state != ModState::Inactive) {
        error = "Only an inactive mod can be enabled";
        return false;
    }
    const ModId& id = record.definition.manifest.id;
    scriptScheduler_->Reserve(id);
    std::shared_ptr<scripting::ScriptRuntime> prepared;
    if (record.definition.script) {
        try { prepared = std::make_shared<scripting::ScriptRuntime>(id, *record.definition.script, ScriptHost()); }
        catch (const std::exception& exception) { error = exception.what(); record.diagnostic = error; return false; }
    }
    auto scriptPatches = scripting::ReduceScriptPatches({}, prepared ? prepared->TakePatchCommands() : std::vector<scripting::ScriptPatchCommand>{});
    std::unique_ptr<dev::ServiceSupervisor> preparedServices;
    if (record.definition.services) {
        preparedServices = std::make_unique<dev::ServiceSupervisor>();
        if (!preparedServices->Start(*record.definition.services, modsDirectory_.parent_path(), error)) {
            record.diagnostic = error;
            return false;
        }
    }
    auto effective = record.definition.patches;
    effective.insert(effective.end(), scriptPatches.begin(), scriptPatches.end());
    if (!scriptPatches.empty() && !ValidateScriptPatchesLocked(id, effective, error)) { record.diagnostic = error; return false; }
    const std::vector<ModId> previousOrder = enabledOrder_;
    std::vector<ModId> candidateOrder = previousOrder;
    candidateOrder.push_back(id);
    record.state = ModState::Loading;
    if (persist && !PersistOrder(candidateOrder, error)) {
        record.state = ModState::Inactive;
        record.diagnostic = error;
        return false;
    }
    std::set<core::Tgi> affected = PatchTargets(effective);
    const std::size_t invalidatedBefore = cache_.Invalidate(affected);
    static_cast<void>(invalidatedBefore);
    if (!registry_.Activate(id, effective, affected, error)) {
        if (persist) {
            std::string rollbackError;
            if (!PersistOrder(previousOrder, rollbackError)) {
                error += "; persistence rollback failed: " + rollbackError;
            }
        }
        record.state = ModState::Failed;
        record.diagnostic = error;
        return false;
    }
    const std::size_t invalidatedAfter = cache_.Invalidate(affected);
    static_cast<void>(invalidatedAfter);
    enabledOrder_ = std::move(candidateOrder);
    if (prepared) { prepared->Commit(); scriptScheduler_->Put(id, prepared); }
    record.scriptRuntime = std::move(prepared);
    record.serviceSupervisor = std::move(preparedServices);
    record.scriptPatches = std::move(scriptPatches);
    record.state = ModState::Active;
    record.diagnostic.clear();
    RefreshRuntimeResourcesLocked(affected, record, "enable");
    return true;
}

bool ModManager::DisableLocked(
    Record& record,
    bool persist,
    std::string& error) {
    error.clear();
    if (!registry_.IsActive(record.definition.manifest.id)) {
        error = "Only an active or changed mod can be disabled";
        return false;
    }
    const ModId id = record.definition.manifest.id;
    const std::vector<ModId> previousOrder = enabledOrder_;
    std::vector<ModId> candidateOrder = previousOrder;
    EraseEnabled(candidateOrder, id);
    const ModState previousState = record.state;
    record.state = ModState::Unloading;
    if (persist && !PersistOrder(candidateOrder, error)) {
        record.state = previousState;
        record.diagnostic = error;
        return false;
    }
    std::set<core::Tgi> affected = PatchTargets(EffectivePatches(record));
    const std::size_t invalidatedBefore = cache_.Invalidate(affected);
    static_cast<void>(invalidatedBefore);
    if (!registry_.Deactivate(id, affected, error)) {
        if (persist) {
            std::string rollbackError;
            if (!PersistOrder(previousOrder, rollbackError)) {
                error += "; persistence rollback failed: " + rollbackError;
            }
        }
        record.state = previousState;
        record.diagnostic = error;
        return false;
    }
    scriptScheduler_->Remove(id);
    if (record.scriptRuntime) record.scriptRuntime->Stop();
    scriptScheduler_->Flush(id, record.scriptRuntime);
    record.scriptRuntime.reset();
    record.serviceSupervisor.reset();
    record.scriptPatches.clear();
    const std::size_t invalidatedAfter = cache_.Invalidate(affected);
    static_cast<void>(invalidatedAfter);
    enabledOrder_ = std::move(candidateOrder);
    const bool invalidChangedDefinition =
        previousState == ModState::Changed &&
        (!record.pendingDefinition.has_value() ||
         !record.pendingFingerprint.has_value());
    if (record.pendingDefinition.has_value() && record.pendingFingerprint.has_value()) {
        record.definition = std::move(*record.pendingDefinition);
        record.fingerprint = *record.pendingFingerprint;
        record.pendingDefinition.reset();
        record.pendingFingerprint.reset();
    }
    if (invalidChangedDefinition) {
        record.state = ModState::Failed;
        if (record.diagnostic.empty()) {
            record.diagnostic =
                "Changed files must validate before this mod can be enabled again";
        }
    } else {
        record.state = ModState::Inactive;
        record.diagnostic.clear();
    }
    RefreshRuntimeResourcesLocked(affected, record, "disable");
    return true;
}

void ModManager::RefreshRuntimeResourcesLocked(
    const std::set<core::Tgi>& affected,
    Record& record,
    std::string_view action) noexcept {
    if (!runtimeRefresh_ || affected.empty()) {
        return;
    }
    try {
        std::string refreshError;
        if (!runtimeRefresh_(affected, refreshError)) {
            const std::string liveDiagnostic = "Registry state committed, but live " +
                std::string(action) + " failed: " +
                (refreshError.empty() ? "unspecified runtime refresh error" : refreshError);
            AppendError(record.diagnostic, liveDiagnostic);
        }
    } catch (...) {
        AppendError(
            record.diagnostic,
            "Registry state committed, but live " + std::string(action) +
                " failed with an exception");
    }
}

bool ModManager::SetEnabled(
    const ModId& id,
    bool enabled,
    std::string& error) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        if (!initialized_) {
            error = "Mod manager is not initialized";
            return false;
        }
        if (enabled) {
            std::string refreshError;
            if (!RefreshLocked(refreshError)) {
                error = "Could not refresh mod files before enable: " + refreshError;
                return false;
            }
        }
        const auto record = records_.find(id);
        if (record == records_.end()) {
            error = "Unknown mod ID '" + id + "'";
            return false;
        }
        return enabled ? EnableLocked(record->second, true, error)
                       : DisableLocked(record->second, true, error);
    } catch (...) {
        error = "Mod state change failed due to an allocation or I/O exception";
        return false;
    }
}

bool ModManager::Reload(const ModId& id, std::string& error, std::string_view expectedRevision) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        error.clear();
        if (!RefreshLocked(error)) return false;
        const auto found = records_.find(id);
        if (found == records_.end() || !registry_.IsActive(id) || !found->second.pendingDefinition) {
            error = "No valid pending definition for an active mod";
            return false;
        }
        auto& record = found->second;
        const auto& fingerprint = *record.pendingFingerprint;
        if (record.definition.services || record.pendingDefinition->services) {
            error = "Developer service changes require disabling and enabling the mod";
            record.diagnostic = error;
            return false;
        }
        const auto revision = core::ToHex(fingerprint.manifest);
        if (!expectedRevision.empty() && expectedRevision != revision) {
            error = "Reload preview is stale; review the latest pending definition";
            return false;
        }
        const auto& pending = *record.pendingDefinition;
        std::shared_ptr<scripting::ScriptRuntime> prepared;
        if (pending.script) {
            try { prepared = std::make_shared<scripting::ScriptRuntime>(id, *pending.script, ScriptHost()); }
            catch (const std::exception& exception) { error = exception.what(); record.diagnostic = "Reload rejected: " + error; return false; }
        }
        auto scriptPatches = scripting::ReduceScriptPatches({}, prepared ? prepared->TakePatchCommands() : std::vector<scripting::ScriptPatchCommand>{});
        auto effective = pending.patches;
        effective.insert(effective.end(), scriptPatches.begin(), scriptPatches.end());
        auto affected = PatchTargets(EffectivePatches(record));
        const auto newTargets = PatchTargets(effective);
        affected.insert(newTargets.begin(), newTargets.end());
        if (!ValidateScriptPatchesLocked(id, effective, error)) { record.diagnostic = "Reload rejected: " + error; return false; }
        // /** @summary Invalidate around a single publication so in-flight old plans cannot commit. */
        static_cast<void>(cache_.Invalidate(affected));
        if (!registry_.Replace(id, effective, affected, error)) return false;
        scriptScheduler_->Remove(id);
        if (record.scriptRuntime) record.scriptRuntime->Stop();
        scriptScheduler_->Flush(id, record.scriptRuntime);
        if (prepared) { prepared->Commit(); scriptScheduler_->Put(id, prepared); }
        record.scriptRuntime = std::move(prepared);
        record.scriptPatches = std::move(scriptPatches);
        static_cast<void>(cache_.Invalidate(affected));
        record.definition = std::move(*record.pendingDefinition);
        record.fingerprint = *record.pendingFingerprint;
        record.pendingDefinition.reset(); record.pendingFingerprint.reset();
        record.state = ModState::Active; record.diagnostic.clear();
        RefreshRuntimeResourcesLocked(affected, record, "reload");
        return true;
    } catch (...) { error = "Reload failed safely with an exception"; return false; }
}

std::vector<ModUiEntry> ModManager::Snapshot(bool wait) const {
    std::unique_lock lock(mutex_, std::defer_lock);
    if (wait) lock.lock(); else static_cast<void>(lock.try_lock());
    if (!lock.owns_lock()) {
        const std::scoped_lock snapshotLock(snapshotMutex_); return cachedSnapshot_;
    }
    std::vector<ModUiEntry> result;
    result.reserve(records_.size() + failedDiscoveries_.size());
    for (const auto& [id, record] : records_) {
        const bool registered = registry_.IsActive(id);
        const auto scriptStatus = record.scriptRuntime ? record.scriptRuntime->Status() : scripting::ScriptStatus{};
        const auto serviceDiagnostic = record.serviceSupervisor ? record.serviceSupervisor->Diagnostic() : std::string{};
        result.push_back(ModUiEntry{
            id,
            record.definition.manifest.name,
            record.definition.manifest.version,
            record.definition.manifest.author,
            record.state,
            record.state == ModState::Inactive && !registered,
            registered,
            record.state == ModState::Changed,
            serviceDiagnostic.empty() ? record.diagnostic : serviceDiagnostic,
            EffectivePatches(record),
            record.pendingDefinition ? record.pendingDefinition->patches : std::vector<ResourcePatchDefinition>{},
            registered && record.pendingDefinition.has_value() && !record.definition.services && !record.pendingDefinition->services,
            record.pendingFingerprint ? core::ToHex(record.pendingFingerprint->manifest) : "",
            record.definition.script ? record.definition.script->language : "",
            record.definition.script ? record.definition.script->revision : "",
            record.pendingDefinition && record.pendingDefinition->script ? record.pendingDefinition->script->revision : "",
            scriptStatus.diagnostic, scriptStatus.memoryBytes, scriptStatus.running});
    }
    result.insert(result.end(), failedDiscoveries_.begin(), failedDiscoveries_.end());
    { const std::scoped_lock snapshotLock(snapshotMutex_); cachedSnapshot_ = result; }
    return result;
}

bool ModManager::DeveloperServicesActive() const {
    const std::scoped_lock lock(mutex_);
    for (const auto& [id, record] : records_) {
        if (registry_.IsActive(id) && record.serviceSupervisor && record.serviceSupervisor->Diagnostic().empty()) return true;
    }
    return false;
}

bool ModManager::RequestAction(std::string action, ModId id, std::string revision) {
    if (action != "refresh" && action != "reload" && action != "enable" && action != "disable") return false;
    return actionWorker_->Submit({std::move(action), std::move(id), std::move(revision)});
}
std::string ModManager::ActionStatus() const { return actionWorker_->Status(); }
bool ModManager::OperationBusy() const noexcept { return actionWorker_->Busy(); }

std::vector<ModId> ModManager::EnabledOrder() const {
    const std::scoped_lock lock(mutex_);
    return enabledOrder_;
}

std::vector<PatchConflict> ModManager::Conflicts() const {
    return registry_.Current()->conflicts();
}
std::vector<runtime::RuntimeResourceObservation> ModManager::RuntimeObservations() const {
    return cache_.Observations();
}
PatchGeneration ModManager::Generation() const noexcept {
    return registry_.Current()->generation();
}

bool ModManager::PersistOrder(
    const std::vector<ModId>& order,
    std::string& error) const {
    return SaveEnabledState(statePath_, order, error);
}

}  // namespace sc13::mods
