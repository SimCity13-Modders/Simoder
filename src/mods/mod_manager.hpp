#include "mods/action_worker.hpp"
#pragma once

#include "mods/mod_candidate.hpp"
#include "mods/mod_types.hpp"
#include "scripting/script_runtime.hpp"
#include "dev/service_supervisor.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <thread>

namespace sc13::runtime {
class RuntimeResourceCache;
struct RuntimeResourceObservation;
}

namespace sc13::mods {

class PatchRegistry;

/** Rebuilds retained live resources after a committed registry generation change. */
using RuntimeResourceRefreshCallback = std::function<bool(
    const std::set<core::Tgi>& affected,
    std::string& error)>;

/** Owns discovery, lifecycle, persistence, and UI projections for installed mods. */
class ModManager final {
public:
    /** Binds one game-root mods directory and persistent state file to runtime services. */
    ModManager(
        std::filesystem::path modsDirectory,
        std::filesystem::path statePath,
        PatchRegistry& registry,
        runtime::RuntimeResourceCache& cache,
        RuntimeResourceRefreshCallback runtimeRefresh = {},
        scripting::ScriptScheduler::LogSink scriptLog = {});
    /** @summary Stops script scheduling and lifecycle callbacks before runtime service destruction. */
    ~ModManager();

    /** Discovers mods and activates each valid persisted entry with failure isolation. */
    [[nodiscard]] bool Initialize(std::string& error) noexcept;

    /** Rescans immediate mod directories without automatically enabling new mods. */
    [[nodiscard]] bool Refresh(std::string& error) noexcept;

    /** Transactionally changes one mod's enabled state and persists activation order. */
    [[nodiscard]] bool SetEnabled(
        const ModId& id,
        bool enabled,
        std::string& error) noexcept;
    /** @summary Applies a valid pending definition without OFF/ON or activation-order changes. */
    [[nodiscard]] bool Reload(const ModId& id, std::string& error,
        std::string_view expectedRevision = {}) noexcept;
    /** @summary Queues a UI operation on the owned worker; compilation never blocks the render thread. */
    [[nodiscard]] bool RequestAction(std::string action, ModId id = {}, std::string revision = {});
    /** @summary Returns progress or the completed operation result without taking the discovery lock. */
    [[nodiscard]] std::string ActionStatus() const;
    /** @summary Reports an in-flight UI operation for disabling duplicate submissions. */
    [[nodiscard]] bool OperationBusy() const noexcept;
    /** @summary Returns the immutable watched source root; cache remains a reserved child namespace. */
    [[nodiscard]] const std::filesystem::path& ModsDirectory() const noexcept { return modsDirectory_; }

    /** Returns an immutable copy suitable for rendering outside the manager lock. */
    [[nodiscard]] std::vector<ModUiEntry> Snapshot(bool wait = true) const;

    /** Returns the currently persisted deterministic activation order. */
    [[nodiscard]] std::vector<ModId> EnabledOrder() const;

    /** Returns same-property conflicts from the current immutable registry snapshot. */
    [[nodiscard]] std::vector<PatchConflict> Conflicts() const;
    /** @summary Returns historical runtime observations without reading arbitrary game memory. */
    [[nodiscard]] std::vector<runtime::RuntimeResourceObservation> RuntimeObservations() const;
    /** @summary Returns the current published patch generation. */
    [[nodiscard]] PatchGeneration Generation() const noexcept;
    /** @summary Report whether an explicitly enabled developer service mod owns live processes. */
    [[nodiscard]] bool DeveloperServicesActive() const;

private:
    /** Retains the committed definition and an optional changed-on-disk successor. */
    struct Record final {
        ModDefinition definition;
        ModDefinitionFingerprint fingerprint{};
        std::optional<ModDefinition> pendingDefinition;
        std::optional<ModDefinitionFingerprint> pendingFingerprint;
        ModState state{ModState::Discovered};
        std::string diagnostic;
        /** @summary Retains the active VM independently of failed pending compiler generations. */
        std::shared_ptr<scripting::ScriptRuntime> scriptRuntime;
        /** @summary Own optional services for exactly the enabled lifetime of this record. */
        std::unique_ptr<dev::ServiceSupervisor> serviceSupervisor;
        /** @summary Retains ephemeral owner-scoped overrides separately from editable declarative sources. */
        std::vector<ResourcePatchDefinition> scriptPatches;
    };

    /** Stores one complete, stable discovery result before manager mutation. */
    struct Candidate final {
        ModDefinition definition;
        ModDefinitionFingerprint fingerprint{};
    };

    /** Performs refresh while the manager lock is already held. */
    [[nodiscard]] bool RefreshLocked(std::string& error);

    /** Activates one inactive record with optional immediate persistence. */
    [[nodiscard]] bool EnableLocked(
        Record& record,
        bool persist,
        std::string& error);

    /** Deactivates one active or changed record with optional immediate persistence. */
    [[nodiscard]] bool DisableLocked(
        Record& record,
        bool persist,
        std::string& error);
    /** @summary Combines source operations and ephemeral script overrides in deterministic owner order. */
    [[nodiscard]] static std::vector<ResourcePatchDefinition> EffectivePatches(const Record& record);
    /** @summary Preflights a candidate owner patch set against retained vanilla before registry publication. */
    [[nodiscard]] bool ValidateScriptPatchesLocked(const ModId& owner,
        const std::vector<ResourcePatchDefinition>& patches, std::string& error) const;
    /** @summary Publishes only commands from the currently active VM, outside its lock. */
    [[nodiscard]] bool ApplyScriptCommands(const ModId& id,
        const std::shared_ptr<scripting::ScriptRuntime>& runtime,
        const std::vector<scripting::ScriptPatchCommand>& commands, std::string& error);
    /** @summary Creates a read-only evidence binding which never needs the manager lock during Lua callbacks. */
    [[nodiscard]] scripting::ScriptHost ScriptHost();

    /** Applies the current registry generation to retained live resources. */
    void RefreshRuntimeResourcesLocked(
        const std::set<core::Tgi>& affected,
        Record& record,
        std::string_view action) noexcept;

    /** Persists the supplied activation order to the configured state path. */
    [[nodiscard]] bool PersistOrder(
        const std::vector<ModId>& order,
        std::string& error) const;

    std::filesystem::path modsDirectory_;
    std::filesystem::path statePath_;
    PatchRegistry& registry_;
    runtime::RuntimeResourceCache& cache_;
    RuntimeResourceRefreshCallback runtimeRefresh_;
    mutable std::mutex mutex_;
    std::map<ModId, Record, std::less<>> records_;
    std::vector<ModUiEntry> failedDiscoveries_;
    std::vector<ModId> enabledOrder_;
    bool initialized_{};
    /** @summary Owns background callback scheduling; destroyed before the manager's records. */
    std::unique_ptr<scripting::ScriptScheduler> scriptScheduler_;
    mutable std::mutex snapshotMutex_;
    mutable std::vector<ModUiEntry> cachedSnapshot_;
    /** @summary Own queued work separately and stop it before lifecycle/scripting destruction. */
    std::unique_ptr<ActionWorker> actionWorker_;
};

}  // namespace sc13::mods
