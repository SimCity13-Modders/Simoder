#pragma once
#include "mods/action_worker.hpp"

#include "mods/mod_candidate.hpp"
#include "mods/captured_candidate.hpp"
#include "mods/owner_participant.hpp"
#include "mods/patch_registry.hpp"
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
class RuntimeScope;
class RuntimeCommitDomain;
enum class ScopeResult;
struct RuntimeResourceObservation;
}
namespace sc13::platform { class RuntimeOwner; }

namespace sc13::mods {

class PatchRegistry;

/** Rebuilds retained live resources after a committed registry generation change. */
using RuntimeResourceRefreshCallback = std::function<bool(
    const std::set<core::Tgi>& affected,
    std::string& error)>;

/** Owns discovery, lifecycle, persistence, and UI projections for installed mods. */
class ModManager final {
public:
    /** @summary Binds one game-root mods directory and persistent state file to runtime services.
     * @param modsDirectory Canonical installed mod directory. @param statePath Persisted activation state.
     * @param registry Single authoritative patch registry. @param cache Retained runtime resource evidence.
     * @param runtimeRefresh Existing validated live-resource refresh adapter. @param scriptLog Script diagnostics sink.
     * @param runtimeOwner Bootstrap-validated shared process guard, or null when generic admission is unavailable.
     * @param runtimeDomain Shared core writer/revision domain across all per-mod generations. */
    ModManager(
        std::filesystem::path modsDirectory,
        std::filesystem::path statePath,
        PatchRegistry& registry,
        runtime::RuntimeResourceCache& cache,
        RuntimeResourceRefreshCallback runtimeRefresh = {},
        scripting::ScriptScheduler::LogSink scriptLog = {},
        std::shared_ptr<platform::RuntimeOwner> runtimeOwner = {},
        std::shared_ptr<runtime::RuntimeCommitDomain> runtimeDomain = {});
    /** @summary Closes and drains native users before runtime service destruction.
     * @pre External owner calls off the script lane, outside a leased callback and without D/M/publication guards. */
    ~ModManager();
    /** @summary Revoke generic scopes before runtime dependency destruction; call outside game callbacks. */
    void RequestRuntimeStop() noexcept;
    /** @summary Observe safe generic scope drain without replacing legacy service authorization.
     * @return True only when no generic operation retains its record resources. */
    [[nodiscard]] bool RuntimeScopesDrained() noexcept;

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
    friend class ManagerClose;
    friend class ManagerCommands;
    friend class ManagerCommandRoute;
    friend struct ManagerCommandTicket;
    friend class ManagerPreparation;
    friend class ManagerPublication;
    friend class ManagerCache;
    friend class OwnerLifecycle;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerLifetimeTestAccess;
    friend struct ManagerPreparationTestAccess;
    friend struct ManagerPublicationTestAccess;
    friend struct ManagerCommandsTestAccess;
    friend struct ManagerCloseTestAccess;
#endif
    /** @summary Combines local manager stop and lock-free shared runtime stop without granting authority.
     * @return True when existing mutation entry points must reject work. */
    [[nodiscard]] bool RuntimeStopping() const noexcept;
    /** Retains the committed definition and an optional changed-on-disk successor. */
    struct Record final {
        /** @summary Exact process-local installed incarnation, independent of textual ID and filesystem path. */
        std::shared_ptr<const InstallationBinding> installation{InstallationBinding::Create()};
        /** @summary Sole canonical sealed stamp; legacy fields remain the unsealed route until later cutover. */
        OwnerParticipant participant;
        ModDefinition definition;
        ModDefinitionFingerprint fingerprint{};
        std::optional<ModDefinition> pendingDefinition;
        std::optional<ModDefinitionFingerprint> pendingFingerprint;
        ModState state{ModState::Discovered};
        std::string diagnostic;
        /** @summary Retains the active VM independently of failed pending compiler generations. */
        std::shared_ptr<scripting::ScriptRuntime> scriptRuntime;
        /** @summary Own optional services for exactly the enabled lifetime of this record. */
        std::shared_ptr<dev::ServiceSupervisor> serviceSupervisor;
        /** @summary Prepared closed scope committed only after registry/persistence; never authorizes legacy services. */
        std::shared_ptr<runtime::RuntimeScope> runtimeScope;
        /** @summary Retains ephemeral owner-scoped overrides separately from editable declarative sources. */
        std::vector<ResourcePatchDefinition> scriptPatches;
    };
    /** @summary Observe canonical preparation eligibility without creating a second current record.
     * @param record Actual M-guarded entry. @param identity Receives native incarnation.
     * @param stamp Receives sole current stamp observation. @return Whether the entry is inactive and unsealed. */
    static bool PreparationRecord(const Record& record, std::shared_ptr<const void>& identity, std::shared_ptr<const void>& stamp,
        bool qualified = false);

    /** @summary Retains the original canonical map/M and closes borrowed-resource reachability before teardown. */
    struct State final {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        /** @summary Observe actual State destruction entry; canonical members still retire in their original order. */
        ~State();
#endif
        mutable std::mutex mutex;
        std::map<ModId, Record, std::less<>> records;
        std::mutex gate;
        std::condition_variable drained;
        ModManager* manager{};
        runtime::RuntimeResourceCache* cache{};
        std::size_t users{};
        bool closed{};
        /** @summary Sticky native close admission and original retained ingress latches, independent of borrowed manager lifetime. */
        std::atomic_bool closeRequested{};
        std::shared_ptr<std::atomic_bool> schedulerClose, actionClose;
        /** @summary One noncounted close owner, bounded resumable cursor and historical foundation acknowledgement. */
        std::atomic_uint64_t closeOwner{};
        std::uint64_t closeSequence{}, closeRevision{}, foundationAcknowledgements{};
        std::size_t closeCursor{},closeHolders{};
        /** @summary Fixed original-record retirement progress; no VM or dependency ownership is duplicated here. */
        struct Retirement final {
            std::atomic_bool turn{};
            std::atomic_uint32_t pins{};
            runtime::ScopeResult completed{};
            std::uint64_t sequence{},completedSequence{},completedOwner{},cursorOwner{},cursorGeneration{},joinedSequence{},retired{};
            std::map<ModId,Record,std::less<>>::iterator cursor;
            bool initialized{},complete{};
        } retirement;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        /** @summary Actual accepted native close action barrier, invoked outside every publication guard. */
        std::shared_ptr<const std::function<void()>> retirementBarrier;
#endif
    };
    /** @summary Native operation lifetime, independent of publication locks and script execution locks. */
    class Lease final {
    public:
        /** @summary Attempts admission before dereferencing borrowed resources. @param state Original anchor. */
        explicit Lease(const std::shared_ptr<State>& state);
        /** @summary Releases a counted operation and wakes an external teardown owner. */
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        /** @summary Reports admitted lifetime without granting runtime permission. @return Admission result. */
        explicit operator bool() const noexcept { return static_cast<bool>(state_); }
        /** @summary Returns the manager protected by this operation. @return Original owner or null. */
        ModManager* Manager() const noexcept { return manager_; }
    private:
        std::shared_ptr<State> state_;
        ModManager* manager_{};
    };
    /** @summary Closes admission and drains users without holding publication locks; destructor only. */
    void CloseAndDrain() noexcept;
    /** @summary Revokes retained scopes during admitted runtime stop or private post-drain destruction. */
    void StopScopes() noexcept;

    /** Stores one complete, stable discovery result before manager mutation. */
    struct Candidate final {
        ModDefinition definition;
        ModDefinitionFingerprint fingerprint{};
    };

    /** Performs refresh while the manager lock is already held. */
    [[nodiscard]] bool RefreshLocked(std::string& error, std::unique_lock<std::mutex>& lock);

    /** Activates one inactive record with optional immediate persistence. */
    [[nodiscard]] bool EnableLocked(
        Record& record,
        bool persist,
        std::string& error, std::unique_lock<std::mutex>& lock);

    /** @summary Deactivates one active or changed record with optional immediate persistence.
     * @param finalizePending False only for missing-directory cleanup, which retains the prior definition. */
    [[nodiscard]] bool DisableLocked(
        Record& record,
        bool persist,
        std::string& error, std::unique_lock<std::mutex>& lock, bool finalizePending = true);
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
    /** @summary Builds the actual weak scheduler callback with execution-time native lifetime admission.
     * @return Revocable native sink, never a runtime permission issuer. */
    scripting::ScriptScheduler::CommandSink CommandSink();

    /** @summary Separates normal publication admission from rollback of a registry change already committed. */
    enum class PublicationPurpose { Mutation, Rollback };
    /** @summary Prepares and publishes legacy state after joining the shared domain before manager/registry locks.
     * @param kind Existing owner mutation. @param id Actual owner. @param patches Pure candidate definitions.
     * @param prepared Empty plan retained until locks release. @param lock Initial manager preparation lock.
     * @param error Receives failure before effects. @param owner Optional actual script owner to revalidate.
     * @param expectedRuntime Actual source runtime. @param purpose Rollback bypasses stop only for existing committed cleanup.
     * @param validatePatches Preserves the existing operation-specific script preflight policy.
     * @return True after exact registry/domain publication. */
    [[nodiscard]] bool PublishPrepared(PatchRegistry::ChangeKind kind, const ModId& id,
        std::span<const ResourcePatchDefinition> patches, PatchRegistry::PreparedPublication& prepared,
        std::unique_lock<std::mutex>& lock, std::string& error,
        const ModId* owner = nullptr, const scripting::ScriptRuntime* expectedRuntime = nullptr,
        PublicationPurpose purpose = PublicationPurpose::Mutation, bool validatePatches = false);

    /** Applies the current registry generation to retained live resources. */
    void RefreshRuntimeResourcesLocked(
        const std::set<core::Tgi>& affected,
        Record& record,
        std::string_view action) noexcept;

    /** Persists the supplied activation order to the configured state path. */
    [[nodiscard]] bool PersistOrder(
        const std::vector<ModId>& order,
        std::string& error) const;

    /** @summary Declared before aliases and callbacks so every native borrower uses this exact anchor. */
    std::shared_ptr<State> state_;
    std::filesystem::path modsDirectory_;
    std::filesystem::path statePath_;
    PatchRegistry& registry_;
    runtime::RuntimeResourceCache& cache_;
    RuntimeResourceRefreshCallback runtimeRefresh_;
    /** @summary Retain one bootstrap-validated process owner across all per-mod scope generations. */
    std::shared_ptr<platform::RuntimeOwner> runtimeOwner_;
    /** @summary One shared core writer/revision domain across all module generations. */
    std::shared_ptr<runtime::RuntimeCommitDomain> runtimeDomain_;
    /** @summary Sticky close gate set before waiting for the discovery lock. */
    std::atomic_bool runtimeStopping_{};
    /** @summary Nonblocking transaction reservation rejects overlapping lifecycle writers without holding a lock across Lua. */
    std::atomic_bool lifecycleBusy_{};
    /** @summary Bounded retired generations, drained outside the manager lock. */
    std::vector<std::shared_ptr<runtime::RuntimeScope>> retiredScopes_;
    /** @summary Aliases preserve the one original manager M and map, never secondary writable storage. */
    std::mutex& mutex_;
    std::map<ModId, Record, std::less<>>& records_;
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

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Closed actual-manager fixture adapter; supplies no production lifecycle or consent issuer. */
struct ManagerLifetimeTestAccess final {
    /** @summary Retains exact canonical membership and its sole current stamp, never a copied authority record. */
    class Plan final {
        friend struct ManagerLifetimeTestAccess;
        std::shared_ptr<ModManager::State> state_;
        std::shared_ptr<const void> identity_;
        std::shared_ptr<OwnerParticipant::Stamp> stamp_;
        ModId owner_;
    };
    /** @summary Captures an actual entry under M. @param manager Original owner. @param id Exact key. @return Retained observation. */
    static Plan Capture(ModManager& manager, const ModId& id);
    /** @summary Revalidates actual M/membership/stamp without borrowed access on closed owners.
     * @param plan Retained observation. @return Bounded status, not publication or permission. */
    static runtime::ScopeResult Validate(const Plan& plan);
    /** @summary Holds actual M for deterministic contention. @param manager Original owner. @return Actual guard. */
    static std::unique_lock<std::mutex> HoldManager(ModManager& manager);
    /** @summary Creates the real weak ScriptHost callback. @param manager Original owner. @return Retained callback. */
    static scripting::ScriptHost Host(ModManager& manager);
    /** @summary Copies the real scheduler sink wrapper for revocation tests. @param manager Original owner. @return Native wrapper. */
    static scripting::ScriptScheduler::CommandSink Sink(ModManager& manager);
    /** @summary Executes an actual public-reader interleaving point after admission and before borrowed access.
     * @param callback Deterministic fixture barrier, installed only on the invoking thread. */
    static void ReaderBarrier(std::function<void()> callback);
    /** @summary Executes the real ScriptHost after-admission interleaving point on its invoking thread.
     * @param callback Deterministic native barrier. */
    static void CallbackBarrier(std::function<void()> callback);
    /** @summary Pause real legacy initialization after M unlock, outside every publication guard.
     * @param callback Fixture-thread barrier; absent from production. */
    static void LegacyInitializationBarrier(std::function<void()> callback);
    /** @summary Inspects closure through retained state. @param plan Exact anchor. @return Whether admission is closed. */
    static bool Closed(const Plan& plan);
    /** @summary Probes original retained M without manager or borrowed-resource dereferences.
     * @param plan Original retained anchor. @return Whether M is currently available. */
    static bool ManagerAvailable(const Plan& plan);
    /** @summary Replaces only the actual participant stamp without entering joint lifecycle.
     * @param manager Original fixture owner. @param id Actual key. @return Whether replacement succeeded. */
    static bool ReplaceStamp(ModManager& manager, const ModId& id);
    /** @summary Attaches a real closed scope to the actual legacy record for destruction-order qualification.
     * @param manager Original owner. @param id Actual key. @return Weak actual scope observation. */
    static std::weak_ptr<runtime::RuntimeScope> AttachScope(ModManager& manager, const ModId& id);
    /** @summary Observes anchor reclamation without retaining it. @param plan Actual anchor observation. @return Weak state identity. */
    static std::weak_ptr<const void> Anchor(const Plan& plan);
};
#endif

}  // namespace sc13::mods
