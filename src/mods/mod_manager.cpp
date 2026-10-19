#include "scripting/script_patch_reducer.hpp"
#include "mods/mod_manager.hpp"
#include "mods/fixture_observation.hpp"

#include "mods/mod_definition_parser.hpp"
#include "mods/resource_project.hpp"
#include "mods/mod_state_store.hpp"
#include "mods/patch_registry.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "runtime/runtime_scope.hpp"
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
/** @summary Bound retired module generations retained while callbacks finish. */
constexpr std::size_t MaxRetiredRuntimeScopes = 128;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fixture-only pauses after admission, before any borrowed-resource access. */
thread_local std::function<void()> readerBarrier, callbackBarrier, legacyInitializationBarrier;
#endif

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

/** @summary Drains retired references only after a public operation releases its manager lock. */
struct RetiredDrain final {
    ModManager& manager;
    /** @summary Performs bounded nonblocking drain without changing operation success semantics. */
    ~RetiredDrain() { static_cast<void>(manager.RuntimeScopesDrained()); }
};

/** @summary Reserves a lifecycle transaction without retaining any lock while the interpreter runs. */
struct LifecycleReservation final {
    std::atomic_bool& busy;
    bool acquired{};
    /** @summary Rejects overlapping writers and lane-origin synchronous lifecycle reentry. */
    explicit LifecycleReservation(std::atomic_bool& value, const runtime::RuntimeCommitDomain& domain) : busy(value) {
        bool expected = false;
        acquired = !scripting::ScriptRuntime::OnScriptLane() && !domain.WriterActiveOnCurrentThread() &&
            busy.compare_exchange_strong(expected, true);
    }
    /** @summary Releases only the reservation acquired by this operation. */
    ~LifecycleReservation() { if (acquired) busy.store(false); }
};

/** @summary Retains retired registry stores until manager bookkeeping and lock release complete. */
struct PublicationRetirement final {
    std::unique_lock<std::mutex>& lock;
    PatchRegistry::PreparedPublication plan;
    /** @summary Releases the manager before destruction of the retained registry plan. */
    ~PublicationRetirement() { if (lock.owns_lock()) lock.unlock(); }
};

/** @summary Disposes candidates and retires replaced VMs only after releasing the manager mutex. */
struct ScriptExit final {
    std::unique_lock<std::mutex>& lock;
    scripting::ScriptScheduler& scheduler;
    const ModId& owner;
    std::shared_ptr<scripting::ScriptRuntime>& candidate;
    std::shared_ptr<scripting::ScriptRuntime> retired;
    /** @summary Never holds manager or registry locks across lifecycle code or lane completion. */
    ~ScriptExit() noexcept {
        if (lock.owns_lock()) lock.unlock();
        if (retired) {
            try { retired->Stop(); } catch (...) { /* @summary Retirement failure cannot prevent owner release. */ }
            try { scheduler.Flush(owner, retired); } catch (...) { /* @summary Diagnostic failure cannot prevent owner release. */ }
        }
        candidate.reset();
        scheduler.ReleaseReservation(owner);
    }
};

}  // namespace

ModManager::ModManager(
    std::filesystem::path modsDirectory,
    std::filesystem::path statePath,
    PatchRegistry& registry,
    runtime::RuntimeResourceCache& cache,
    RuntimeResourceRefreshCallback runtimeRefresh, scripting::ScriptScheduler::LogSink scriptLog,
    std::shared_ptr<platform::RuntimeOwner> runtimeOwner,
    std::shared_ptr<runtime::RuntimeCommitDomain> runtimeDomain)
    : state_(std::make_shared<State>()),
      modsDirectory_(std::move(modsDirectory)),
      statePath_(std::move(statePath)),
      registry_(registry),
      cache_(cache),
      runtimeRefresh_(std::move(runtimeRefresh)),
      runtimeOwner_(std::move(runtimeOwner)),
      runtimeDomain_(runtimeDomain ? std::move(runtimeDomain) : std::make_shared<runtime::RuntimeCommitDomain>()),
      mutex_(state_->mutex), records_(state_->records),
      scriptScheduler_(std::make_unique<scripting::ScriptScheduler>(std::move(scriptLog), CommandSink())),
      actionWorker_(std::make_unique<ActionWorker>([weak = std::weak_ptr<State>(state_)](const ModAction& action) {
          const Lease operation(weak.lock());
          if (!operation) return std::string("Mod manager is closed");
          auto* manager = operation.Manager();
          std::string error;
          const bool success = action.kind == "refresh" ? manager->Refresh(error) :
              action.kind == "reload" ? manager->Reload(action.id, error, action.revision) :
              manager->SetEnabled(action.id, action.kind == "enable", error);
          static_cast<void>(manager->Snapshot());
          return success ? action.kind + " completed: " + action.id : action.kind + " failed: " + error;
      })) {
    state_->manager = this; state_->cache = &cache;
    state_->schedulerClose=scriptScheduler_->CloseAdmission();state_->actionClose=actionWorker_->closeAdmission_;
    retiredScopes_.reserve(MaxRetiredRuntimeScopes);
}

ModManager::Lease::Lease(const std::shared_ptr<State>& state) {
    if (!state) return;
    const std::scoped_lock lock(state->gate);
    if (state->closed || state->closeRequested.load() || !state->manager) return;
    ++state->users; state_ = state; manager_ = state->manager;
}
ModManager::Lease::~Lease() {
    if (!state_) return;
    const std::scoped_lock lock(state_->gate);
    if (--state_->users == 0) state_->drained.notify_all();
}
void ModManager::CloseAndDrain() noexcept {
    runtimeStopping_.store(true);
    std::unique_lock lock(state_->gate);
    state_->closed = true; state_->manager = nullptr;
    state_->drained.wait(lock, [this] { return state_->users == 0; });
    lock.unlock();
    // @summary A native close action acknowledges only after all copied VM/dependency holders have retired off guards.
    auto pins=state_->retirement.pins.load();
    while(pins){state_->retirement.pins.wait(pins);pins=state_->retirement.pins.load();}
    lock.lock();
    state_->cache = nullptr;
    lock.unlock();
    const std::scoped_lock records(state_->mutex);
    state_->retirement.cursor=state_->records.end();state_->retirement.initialized=false;state_->retirement.complete=false;
}

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
ModManager::State::~State(){fixture::Destroying(this,fixture::ObjectKind::State);}
#endif
ModManager::~ModManager() {
    CloseAndDrain();
    StopScopes();
    actionWorker_.reset();
    scriptScheduler_->Shutdown();
    for (auto& [id, record] : records_) {
        if (record.scriptRuntime) record.scriptRuntime->Stop();
        scriptScheduler_->Flush(id, record.scriptRuntime);
    }
    scriptScheduler_.reset();
    // @summary Retained plans keep only closed observations; actual record services/scopes retire before borrowed resources.
    std::map<ModId, Record, std::less<>> retired;
    {
        const std::scoped_lock lock(mutex_);
        retired.swap(records_);
    }
}

bool ModManager::RuntimeStopping() const noexcept {
    return runtimeStopping_.load() || state_->closeRequested.load() || runtimeDomain_->IsStopping();
}

void ModManager::RequestRuntimeStop() noexcept {
    runtimeStopping_.store(true);
    const Lease operation(state_);
    if (!operation) return;
    StopScopes();
}

void ModManager::StopScopes() noexcept {
    const std::scoped_lock lock(mutex_);
    for (auto& [id, record] : records_) {
        static_cast<void>(id);
        if (record.runtimeScope) record.runtimeScope->RequestStop();
    }
    for (const auto& scope : retiredScopes_) scope->RequestStop();
}

bool ModManager::RuntimeScopesDrained() noexcept {
    const Lease operation(state_);
    if (!operation) return false;
    try {
        std::vector<std::shared_ptr<runtime::RuntimeScope>> scopes;
        {
            const std::scoped_lock lock(mutex_);
            scopes.reserve(records_.size() + retiredScopes_.size());
            for (const auto& [id, record] : records_) {
                static_cast<void>(id);
                if (record.runtimeScope) scopes.push_back(record.runtimeScope);
            }
            scopes.insert(scopes.end(), retiredScopes_.begin(), retiredScopes_.end());
        }
        bool drained = true;
        for (const auto& scope : scopes)
            if (!scope->DrainFor(std::chrono::milliseconds(0))) drained = false;
        {
            const std::scoped_lock lock(mutex_);
            std::erase_if(retiredScopes_, [](const auto& scope) { return scope->Current().drained; });
        }
        return drained;
    } catch (...) { return false; }
}

bool ModManager::Initialize(std::string& error) noexcept {
    const Lease operation(state_);
    if (!operation) { error = "Mod manager is closed"; return false; }
    const RetiredDrain drain{*this};
    const LifecycleReservation reservation(lifecycleBusy_, *runtimeDomain_);
    if (!reservation.acquired) { error = "Lifecycle transaction busy or script-lane reentry"; return false; }
    try {
        std::unique_lock lock(mutex_);
        error.clear();
        if (RuntimeStopping()) { error = "Mod runtime is stopping"; return false; }
        if (initialized_) {
            error = "Mod manager is already initialized";
            return false;
        }

        std::vector<ModId> requested;
        std::string stateError;
        const bool stateValid = LoadEnabledState(statePath_, requested, stateError);
        enabledOrder_.clear();
        std::string refreshError;
        const bool refreshValid = RefreshLocked(refreshError, lock);
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
                if (!EnableLocked(record->second, false, activationError, lock)) {
                    if (!lock.owns_lock()) lock.lock();
                    record->second.state = ModState::Failed;
                    record->second.diagnostic = activationError;
                    AppendError(error, "Mod '" + id + "': " + activationError);
                }
                if (!lock.owns_lock()) lock.lock();
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
    const Lease operation(state_);
    if (!operation) { error = "Mod manager is closed"; return false; }
    const RetiredDrain drain{*this};
    const LifecycleReservation reservation(lifecycleBusy_, *runtimeDomain_);
    if (!reservation.acquired) { error = "Lifecycle transaction busy or script-lane reentry"; return false; }
    try {
        std::unique_lock lock(mutex_);
        if (!initialized_) {
            error = "Mod manager must be initialized before refresh";
            return false;
        }
        return RefreshLocked(error, lock);
    } catch (...) {
        error = "Mod refresh failed due to an allocation or I/O exception";
        return false;
    }
}

bool ModManager::RefreshLocked(std::string& error, std::unique_lock<std::mutex>& lock) {
    error.clear();
    if (RuntimeStopping()) { error = "Mod runtime is stopping"; return false; }
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
        if(record.participant.jointBinding) {
            if(candidate!=unique.end())unique.erase(candidate);
            ++iterator;continue;
        }
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
                std::string deactivateError;
                const bool deactivated = DisableLocked(record, false, deactivateError, lock, false);
                if (!lock.owns_lock()) lock.lock();
                if (deactivated) {
                    persistenceChanged = true;
                    record.state = ModState::Missing;
                    record.pendingDefinition.reset();
                    record.pendingFingerprint.reset();
                    record.diagnostic = "Active mod directory is missing or no longer valid";
                    ++iterator;
                    continue;
                }
                record.state = ModState::Failed;
                record.diagnostic = "Failed to unload missing mod: " + deactivateError;
                AppendError(error, record.diagnostic);
                ++iterator;
                continue;
            }
            if (record.runtimeScope) {
                record.runtimeScope->RequestStop();
                if (retiredScopes_.size() >= MaxRetiredRuntimeScopes) {
                    AppendError(error, "Retired runtime generations are still draining");
                    ++iterator;
                    continue;
                }
                retiredScopes_.push_back(std::move(record.runtimeScope));
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
    return {[weak = std::weak_ptr<State>(state_)](const core::Tgi& target, std::uint32_t property)
        -> std::optional<scripting::FloatObservation> {
        const auto state = weak.lock();
        const Lease operation(state);
        if (!operation) return std::nullopt;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        if (callbackBarrier) callbackBarrier();
#endif
        return state->cache->ReadObservedFloat(target, property);
    }};
}

scripting::ScriptScheduler::CommandSink ModManager::CommandSink() {
    return [weak = std::weak_ptr<State>(state_)](const auto& id, const auto& runtime,
        const auto& commands, std::string& error) {
        const Lease operation(weak.lock());
        if (!operation) { error = "Mod manager is closed"; return false; }
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        if (callbackBarrier) callbackBarrier();
#endif
        return operation.Manager()->ApplyScriptCommands(id, runtime, commands, error);
    };
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

bool ModManager::PublishPrepared(PatchRegistry::ChangeKind kind, const ModId& id,
    std::span<const ResourcePatchDefinition> patches, PatchRegistry::PreparedPublication& prepared,
    std::unique_lock<std::mutex>& lock, std::string& error,
    const ModId* owner, const scripting::ScriptRuntime* expectedRuntime, PublicationPurpose purpose, bool validatePatches) {
    // @summary Legacy synchronous drains join only after releasing preparation locks; new tasks stay try-only.
    lock.unlock();
    auto writer = runtimeDomain_->ReserveWriter(true);
    lock.lock();
    if (!writer.Acquired()) { error = "Recursive runtime writer admission is busy"; return false; }
    if (purpose == PublicationPurpose::Mutation && (RuntimeStopping() || !writer.CanAdvance())) {
        error = "Runtime writer is stopped or exhausted"; return false;
    }
    const auto sealed=records_.find(id);
    if(sealed!=records_.end() && sealed->second.participant.jointBinding){error="Sealed owner requires canonical publication";return false;}
    if (owner) {
        const auto current = records_.find(*owner);
        if (current == records_.end() || current->second.scriptRuntime.get() != expectedRuntime) {
            error = "Stale script owner generation"; return false;
        }
    }
    // @summary Legacy drain preparation uses fresh owner/order state while serialized, before any effects.
    // @summary The separate prepared API remains available for future nonblocking operations; none is activated.
    if (validatePatches &&
        !ValidateScriptPatchesLocked(id, std::vector<ResourcePatchDefinition>(patches.begin(), patches.end()), error)) return false;
    if (!registry_.Prepare(kind, id, patches, prepared, error)) return false;
    auto publication = registry_.ReservePublication(true);
    if (!publication.CanPublish(prepared)) { error = "Prepared registry publication became stale"; return false; }
    // @summary No allocation, callback, refresh, persistence, logging or waiting beyond this fence.
    const auto revision = purpose == PublicationPurpose::Rollback ? writer.AdvanceRollback() : writer.Advance();
    if (!revision) { error = "Runtime writer stopped before publication"; return false; }
    return publication.Publish(prepared, revision);
}

bool ModManager::ApplyScriptCommands(const ModId& id,
    const std::shared_ptr<scripting::ScriptRuntime>& runtime,
    const std::vector<scripting::ScriptPatchCommand>& commands, std::string& error) {
    const Lease operation(state_);
    if (!operation) { error = "Mod manager is closed"; return false; }
    try {
        std::unique_lock lock(mutex_);
        PublicationRetirement publication{lock};
        if (RuntimeStopping()) { error = "Mod runtime is stopping"; return false; }
        const auto found = records_.find(id);
        /** @summary Discard an in-flight callback from a disabled or superseded VM without touching its successor. */
        if (found == records_.end() || found->second.scriptRuntime != runtime || !registry_.IsActive(id)) {
            error = "Stale script owner generation"; return false;
        }
        auto& record = found->second;
        if(record.participant.jointBinding){error="Sealed commands require exact canonical consumer";return false;}
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
        auto affected = PatchTargets(EffectivePatches(record));
        const auto added = PatchTargets(effective); affected.insert(added.begin(), added.end());
        if (!PublishPrepared(PatchRegistry::ChangeKind::Replace, id, effective, publication.plan,
                lock, error, &id, runtime.get(), PublicationPurpose::Mutation, true)) return false;
        // @summary Re-resolve after joining the domain: lifecycle removal may have retired the former record.
        const auto committed = records_.find(id);
        if (committed == records_.end() || committed->second.scriptRuntime != runtime) {
            error = "Script owner retired after registry publication"; return false;
        }
        committed->second.scriptPatches = std::move(candidate);
        static_cast<void>(cache_.Invalidate(affected));
        RefreshRuntimeResourcesLocked(affected, committed->second, "script update");
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
    catch (...) { error = "Script PROP transaction failed safely"; return false; }
}

bool ModManager::EnableLocked(
    Record& record,
    bool persist,
    std::string& error, std::unique_lock<std::mutex>& lock) {
    error.clear();
    if(record.participant.jointBinding){error="Sealed owner requires canonical publication";return false;}
    if (RuntimeStopping()) { error = "Mod runtime is stopping"; return false; }
    if (record.state != ModState::Inactive) {
        error = "Only an inactive mod can be enabled";
        return false;
    }
    const ModId id = record.definition.manifest.id;
    const auto definition = record.definition;
    const auto expectedGeneration = registry_.Current()->generation();
    // @summary Keep migrated generic admission closed throughout candidate preparation.
    if (record.runtimeScope && retiredScopes_.size() >= MaxRetiredRuntimeScopes) { error = "Retired runtime generations are still draining"; return false; }
    auto preparedScope = runtimeOwner_ ? std::make_shared<runtime::RuntimeScope>(runtimeOwner_, runtimeDomain_) : nullptr;
    if (definition.script) scriptScheduler_->Reserve(id);
    std::shared_ptr<scripting::ScriptRuntime> prepared;
    ScriptExit exit{lock, *scriptScheduler_, id, prepared};
    PublicationRetirement publication{lock};
    if (definition.script) {
        lock.unlock();
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        if(legacyInitializationBarrier)legacyInitializationBarrier();
#endif
        try { prepared = std::make_shared<scripting::ScriptRuntime>(id, *definition.script, ScriptHost()); }
        catch (const std::exception& exception) { lock.lock(); error = exception.what(); record.diagnostic = error; return false; }
        scriptScheduler_->Prepare(prepared);
        lock.lock();
        if (RuntimeStopping() || registry_.Current()->generation() != expectedGeneration || record.state != ModState::Inactive) {
            error = "Script candidate became stale while preparing: stopping="+std::to_string(RuntimeStopping())+
                "; expectedGeneration="+std::to_string(expectedGeneration)+"; observedGeneration="+
                std::to_string(registry_.Current()->generation())+"; state="+ModStateName(record.state);
            return false;
        }
    }
    auto scriptPatches = scripting::ReduceScriptPatches({}, prepared ? prepared->TakePatchCommands() : std::vector<scripting::ScriptPatchCommand>{});
    std::shared_ptr<dev::ServiceSupervisor> preparedServices;
    if (record.definition.services) {
        preparedServices = std::make_shared<dev::ServiceSupervisor>();
        if (!preparedServices->Start(*record.definition.services, modsDirectory_.parent_path(), error)) {
            record.diagnostic = error;
            return false;
        }
    }
    auto effective = record.definition.patches;
    effective.insert(effective.end(), scriptPatches.begin(), scriptPatches.end());
    if (preparedScope && ((prepared && !preparedScope->RetainResource(prepared)) ||
                          (preparedServices && !preparedScope->RetainResource(preparedServices)))) {
        error = "Cannot retain prepared runtime resources";
        record.diagnostic = error;
        return false;
    }
    // @summary Preserve early enable rejection before persistence; the serialized preflight rechecks races later.
    if (!scriptPatches.empty() && !ValidateScriptPatchesLocked(id, effective, error)) { record.diagnostic = error; return false; }
    const std::vector<ModId> previousOrder = enabledOrder_;
    std::vector<ModId> candidateOrder = previousOrder;
    candidateOrder.push_back(id);
    record.state = ModState::Loading;
    if (RuntimeStopping()) { record.state = ModState::Inactive; error = "Mod runtime is stopping"; return false; }
    if (persist && !PersistOrder(candidateOrder, error)) {
        record.state = ModState::Inactive;
        record.diagnostic = error;
        return false;
    }
    std::set<core::Tgi> affected = PatchTargets(effective);
    if (!PublishPrepared(PatchRegistry::ChangeKind::Activate, id, effective, publication.plan, lock, error,
            nullptr, nullptr, PublicationPurpose::Mutation, !scriptPatches.empty())) {
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
    // @summary A generic scope cannot observe activation until both persistence and registry committed.
    if (RuntimeStopping() || (preparedScope && !preparedScope->CommitActivation())) {
        std::string rollbackError;
        std::set<core::Tgi> rollbackAffected;
        error = "Runtime scope activation rejected";
        PublicationRetirement rollback{lock};
        if (!PublishPrepared(PatchRegistry::ChangeKind::Deactivate, id, {}, rollback.plan, lock,
                rollbackError, nullptr, nullptr, PublicationPurpose::Rollback))
            AppendError(error, "registry rollback failed: " + rollbackError);
        rollbackAffected = rollback.plan.Affected();
        static_cast<void>(cache_.Invalidate(rollbackAffected));
        if (persist && !PersistOrder(previousOrder, rollbackError)) AppendError(error, "persistence rollback failed: " + rollbackError);
        record.state = ModState::Failed;
        record.diagnostic = error;
        if (preparedScope) preparedScope->RequestStop(runtime::StopReason::ActivationFailed);
        return false;
    }
    const std::size_t invalidatedAfter = cache_.Invalidate(affected);
    static_cast<void>(invalidatedAfter);
    enabledOrder_ = std::move(candidateOrder);
    if (prepared) { prepared->Commit(); scriptScheduler_->Put(id, prepared); }
    record.scriptRuntime = std::move(prepared);
    record.serviceSupervisor = std::move(preparedServices);
    if (record.runtimeScope) {
        record.runtimeScope->RequestStop();
        retiredScopes_.push_back(std::move(record.runtimeScope));
    }
    record.runtimeScope = std::move(preparedScope);
    if (record.runtimeScope) record.runtimeScope->SetReadiness(true);
    record.scriptPatches = std::move(scriptPatches);
    record.state = ModState::Active;
    record.diagnostic.clear();
    RefreshRuntimeResourcesLocked(affected, record, "enable");
    return true;
}

bool ModManager::DisableLocked(
    Record& record,
    bool persist,
    std::string& error, std::unique_lock<std::mutex>& lock, bool finalizePending) {
    error.clear();
    if(record.participant.jointBinding){error="Sealed owner requires canonical publication";return false;}
    PublicationRetirement publication{lock};
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
    if (!PublishPrepared(PatchRegistry::ChangeKind::Deactivate, id, {}, publication.plan, lock, error)) {
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
    if (record.runtimeScope) record.runtimeScope->RequestStop();
    std::shared_ptr<scripting::ScriptRuntime> candidate;
    ScriptExit exit{lock, *scriptScheduler_, id, candidate, std::move(record.scriptRuntime)};
    scriptScheduler_->Remove(id);
    record.serviceSupervisor.reset();
    // @summary Public-operation exit drains the stopped scope after releasing the manager lock.
    record.scriptPatches.clear();
    const std::size_t invalidatedAfter = cache_.Invalidate(affected);
    static_cast<void>(invalidatedAfter);
    enabledOrder_ = std::move(candidateOrder);
    const bool invalidChangedDefinition =
        previousState == ModState::Changed &&
        (!record.pendingDefinition.has_value() ||
         !record.pendingFingerprint.has_value());
    if (finalizePending && record.pendingDefinition.has_value() && record.pendingFingerprint.has_value()) {
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
    const Lease operation(state_);
    if (!operation) { error = "Mod manager is closed"; return false; }
    const RetiredDrain drain{*this};
    const LifecycleReservation reservation(lifecycleBusy_, *runtimeDomain_);
    if (!reservation.acquired) { error = "Lifecycle transaction busy or script-lane reentry"; return false; }
    try {
        std::unique_lock lock(mutex_);
        if (RuntimeStopping()) { error = "Mod runtime is stopping"; return false; }
        if (!initialized_) {
            error = "Mod manager is not initialized";
            return false;
        }
        if (enabled) {
            std::string refreshError;
            if (!RefreshLocked(refreshError, lock)) {
                error = "Could not refresh mod files before enable: " + refreshError;
                return false;
            }
        }
        const auto record = records_.find(id);
        if (record == records_.end()) {
            error = "Unknown mod ID '" + id + "'";
            return false;
        }
        return enabled ? EnableLocked(record->second, true, error, lock)
                       : DisableLocked(record->second, true, error, lock);
    } catch (...) {
        error = "Mod state change failed due to an allocation or I/O exception";
        return false;
    }
}

bool ModManager::Reload(const ModId& id, std::string& error, std::string_view expectedRevision) noexcept {
    const Lease operation(state_);
    if (!operation) { error = "Mod manager is closed"; return false; }
    const RetiredDrain drain{*this};
    const LifecycleReservation reservation(lifecycleBusy_, *runtimeDomain_);
    if (!reservation.acquired) { error = "Lifecycle transaction busy or script-lane reentry"; return false; }
    try {
        std::unique_lock lock(mutex_);
        error.clear();
        if (RuntimeStopping()) { error = "Mod runtime is stopping"; return false; }
        if (!RefreshLocked(error, lock)) return false;
        const auto found = records_.find(id);
        if (found == records_.end() || !registry_.IsActive(id) || !found->second.pendingDefinition) {
            error = "No valid pending definition for an active mod";
            return false;
        }
        auto& record = found->second;
        if(record.participant.jointBinding){error="Sealed reload requires canonical publication";return false;}
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
        const auto pending = *record.pendingDefinition;
        const auto expectedGeneration = registry_.Current()->generation();
        if (record.runtimeScope && retiredScopes_.size() >= MaxRetiredRuntimeScopes) {
            error = "Retired runtime generations are still draining";
            return false;
        }
        auto preparedScope = runtimeOwner_ ? std::make_shared<runtime::RuntimeScope>(runtimeOwner_, runtimeDomain_) : nullptr;
        std::shared_ptr<scripting::ScriptRuntime> prepared;
        ScriptExit exit{lock, *scriptScheduler_, id, prepared};
        PublicationRetirement publication{lock};
        if (pending.script) {
            scriptScheduler_->Reserve(id);
            lock.unlock();
            try { prepared = std::make_shared<scripting::ScriptRuntime>(id, *pending.script, ScriptHost()); }
            catch (const std::exception& exception) { lock.lock(); error = exception.what(); record.diagnostic = "Reload rejected: " + error; return false; }
            scriptScheduler_->Prepare(prepared);
            lock.lock();
            if (RuntimeStopping() || registry_.Current()->generation() != expectedGeneration || !registry_.IsActive(id)) {
                error = "Reload candidate became stale while preparing"; return false;
            }
        }
        if (preparedScope && prepared && !preparedScope->RetainResource(prepared)) {
            error = "Cannot retain prepared reload runtime";
            record.diagnostic = "Reload rejected: " + error;
            return false;
        }
        auto scriptPatches = scripting::ReduceScriptPatches({}, prepared ? prepared->TakePatchCommands() : std::vector<scripting::ScriptPatchCommand>{});
        auto effective = pending.patches;
        effective.insert(effective.end(), scriptPatches.begin(), scriptPatches.end());
        auto affected = PatchTargets(EffectivePatches(record));
        const auto newTargets = PatchTargets(effective);
        affected.insert(newTargets.begin(), newTargets.end());
        if (RuntimeStopping()) { error = "Mod runtime is stopping"; return false; }
        const auto previousPatches = EffectivePatches(record);
        if (!PublishPrepared(PatchRegistry::ChangeKind::Replace, id, effective, publication.plan, lock, error,
                nullptr, nullptr, PublicationPurpose::Mutation, true)) { record.diagnostic = "Reload rejected: " + error; return false; }
        if (RuntimeStopping() || (preparedScope && !preparedScope->CommitActivation())) {
            error = "Reload runtime scope activation rejected";
            std::string rollbackError;
            PublicationRetirement rollback{lock};
            if (!PublishPrepared(PatchRegistry::ChangeKind::Replace, id, previousPatches, rollback.plan, lock,
                    rollbackError, nullptr, nullptr, PublicationPurpose::Rollback))
                AppendError(error, "registry rollback failed: " + rollbackError);
            static_cast<void>(cache_.Invalidate(affected));
            if (preparedScope) preparedScope->RequestStop(runtime::StopReason::ActivationFailed);
            record.diagnostic = error;
            RefreshRuntimeResourcesLocked(affected, record, "reload rollback");
            return false;
        }
        if (record.runtimeScope) {
            record.runtimeScope->RequestStop();
            retiredScopes_.push_back(std::move(record.runtimeScope));
        }
        exit.retired = std::move(record.scriptRuntime);
        scriptScheduler_->Remove(id, bool(prepared));
        if (prepared) { prepared->Commit(); scriptScheduler_->Put(id, prepared); }
        record.scriptRuntime = std::move(prepared);
        record.runtimeScope = std::move(preparedScope);
        if (record.runtimeScope) record.runtimeScope->SetReadiness(true);
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
    const Lease operation(state_);
    if (!operation) return {};
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
    const Lease operation(state_);
    if (!operation) return false;
    const std::scoped_lock lock(mutex_);
    for (const auto& [id, record] : records_) {
        if (registry_.IsActive(id) && record.serviceSupervisor && record.serviceSupervisor->Diagnostic().empty()) return true;
    }
    return false;
}

bool ModManager::RequestAction(std::string action, ModId id, std::string revision) {
    const Lease operation(state_);
    if (!operation) return false;
    if (RuntimeStopping()) return false;
    if (action != "refresh" && action != "reload" && action != "enable" && action != "disable") return false;
    return actionWorker_->Submit({std::move(action), std::move(id), std::move(revision)});
}
std::string ModManager::ActionStatus() const {
    const Lease operation(state_);
    return operation ? actionWorker_->Status() : "Mod manager is closed";
}
bool ModManager::OperationBusy() const noexcept {
    const Lease operation(state_);
    return operation && actionWorker_->Busy();
}

std::vector<ModId> ModManager::EnabledOrder() const {
    const Lease operation(state_);
    if (!operation) return {};
    const std::scoped_lock lock(mutex_);
    return enabledOrder_;
}

std::vector<PatchConflict> ModManager::Conflicts() const {
    const Lease operation(state_);
    if (!operation) return {};
    return registry_.Current()->conflicts();
}
std::vector<runtime::RuntimeResourceObservation> ModManager::RuntimeObservations() const {
    const Lease operation(state_);
    if (!operation) return {};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    if (readerBarrier) readerBarrier();
#endif
    return cache_.Observations();
}
PatchGeneration ModManager::Generation() const noexcept {
    const Lease operation(state_);
    if (!operation) return {};
    return registry_.Current()->generation();
}

bool ModManager::PreparationRecord(const Record& record, std::shared_ptr<const void>& identity,
    std::shared_ptr<const void>& stamp,bool qualified) {
    identity=record.participant.identity; stamp=record.participant.stamp;
    if(qualified && record.participant.jointBinding)
        return !record.participant.stamp->cleanupPending && !record.serviceSupervisor &&
            ((record.participant.stamp->current && record.state==ModState::Active && record.scriptRuntime) ||
             (record.participant.CanonicalEmpty() &&
              record.state==ModState::Inactive && !record.scriptRuntime));
    return record.state==ModState::Inactive && !record.scriptRuntime && !record.serviceSupervisor &&
        !record.participant.jointBinding && !record.participant.stamp->current && !record.participant.stamp->tombstone;
}

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
void ManagerLifetimeTestAccess::LegacyInitializationBarrier(std::function<void()> callback) {
    legacyInitializationBarrier=std::move(callback);
}
ManagerLifetimeTestAccess::Plan ManagerLifetimeTestAccess::Capture(ModManager& manager, const ModId& id) {
    Plan result;
    const ModManager::Lease operation(manager.state_);
    if (!operation) return result;
    const std::scoped_lock lock(manager.mutex_);
    const auto found = manager.records_.find(id);
    if (found == manager.records_.end()) return result;
    result.state_ = manager.state_; result.owner_ = id;
    result.identity_ = found->second.participant.identity;
    result.stamp_ = found->second.participant.stamp;
    return result;
}
runtime::ScopeResult ManagerLifetimeTestAccess::Validate(const Plan& plan) {
    using runtime::ScopeResult;
    if (!plan.state_) return ScopeResult::Invalid;
    const ModManager::Lease operation(plan.state_);
    if (!operation) return ScopeResult::Cancelled;
    const std::unique_lock lock(plan.state_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return ScopeResult::Busy;
    const auto found = plan.state_->records.find(plan.owner_);
    if (found == plan.state_->records.end() || found->second.participant.identity != plan.identity_ ||
        found->second.participant.stamp != plan.stamp_) return ScopeResult::Stale;
    return ScopeResult::Ok;
}
std::unique_lock<std::mutex> ManagerLifetimeTestAccess::HoldManager(ModManager& manager) {
    return std::unique_lock<std::mutex>(manager.mutex_);
}
scripting::ScriptHost ManagerLifetimeTestAccess::Host(ModManager& manager) { return manager.ScriptHost(); }
scripting::ScriptScheduler::CommandSink ManagerLifetimeTestAccess::Sink(ModManager& manager) { return manager.CommandSink(); }
void ManagerLifetimeTestAccess::ReaderBarrier(std::function<void()> callback) { readerBarrier = std::move(callback); }
void ManagerLifetimeTestAccess::CallbackBarrier(std::function<void()> callback) { callbackBarrier = std::move(callback); }
bool ManagerLifetimeTestAccess::Closed(const Plan& plan) {
    if (!plan.state_) return true;
    const std::scoped_lock lock(plan.state_->gate);
    return plan.state_->closed;
}
bool ManagerLifetimeTestAccess::ManagerAvailable(const Plan& plan) {
    if (!plan.state_) return false;
    const std::unique_lock lock(plan.state_->mutex, std::try_to_lock);
    return lock.owns_lock();
}
bool ManagerLifetimeTestAccess::ReplaceStamp(ModManager& manager, const ModId& id) {
    const ModManager::Lease operation(manager.state_);
    if (!operation) return false;
    auto next = std::make_shared<OwnerParticipant::Stamp>();
    std::shared_ptr<OwnerParticipant::Stamp> retired;
    {
        const std::scoped_lock lock(manager.mutex_);
        const auto found = manager.records_.find(id);
        if (found == manager.records_.end()) return false;
        next->revision = found->second.participant.stamp->revision + 1;
        retired.swap(found->second.participant.stamp);
        found->second.participant.stamp.swap(next);
    }
    return true;
}
std::weak_ptr<runtime::RuntimeScope> ManagerLifetimeTestAccess::AttachScope(ModManager& manager, const ModId& id) {
    const ModManager::Lease operation(manager.state_);
    if (!operation) return {};
    auto scope = std::make_shared<runtime::RuntimeScope>(nullptr, manager.runtimeDomain_);
    const std::scoped_lock lock(manager.mutex_);
    const auto found = manager.records_.find(id);
    if (found == manager.records_.end() || found->second.runtimeScope) return {};
    found->second.runtimeScope = scope;
    return scope;
}
std::weak_ptr<const void> ManagerLifetimeTestAccess::Anchor(const Plan& plan) { return plan.state_; }
#endif

bool ModManager::PersistOrder(
    const std::vector<ModId>& order,
    std::string& error) const {
    return SaveEnabledState(statePath_, order, error);
}

}  // namespace sc13::mods
