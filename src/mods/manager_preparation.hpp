#pragma once
#include "mods/mod_manager.hpp"
#include "scripting/base_host.hpp"
#include "scripting/semantic_bridge.hpp"

namespace sc13::mods {
/** @summary Retain exact inactive-owner preparation without publishing a VM, patches or authority. */
class ManagerPreparation final {
    friend class ManagerClose;
    friend class ManagerPublication;
    friend class OwnerLifecycle;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerPublicationTestAccess;
#endif
public:
    /** @summary Native-owned preparation storage; destroy outside publication guards and the script lane. */
    class Plan final {
        friend class ManagerPreparation;
        friend class ManagerPublication;
        friend class OwnerLifecycle;
        friend class ManagerClose;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        friend struct ManagerPreparationTestAccess;
        friend struct ManagerCommandsTestAccess;
#endif
    public:
        /** @summary Create empty owned storage. */
        Plan()=default;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        /** @summary Observe this actual preparation's destructor entry without changing member retirement order. */
        ~Plan();
#endif
        /** @summary Transfer native resource lifetime. @param other Retained preparation. */
        Plan(Plan&& other) noexcept=default;
        /** @summary Transfer after closing any former owned preparation off publication guards. @param other New storage. @return This plan. */
        Plan& operator=(Plan&& other) noexcept;
        Plan(const Plan&)=delete;
        Plan& operator=(const Plan&)=delete;
    private:
        std::shared_ptr<ModManager::State> state_;
        std::shared_ptr<const void> identity_, stamp_;
        /** @summary Non-owning-of-VM promotion identity avoids a VM/proof ownership cycle. */
        std::shared_ptr<const void> promotion_;
        /** @summary Fixture-selected scope admission remains separate from ordinary closed preparation. */
        bool qualified_{};
        std::shared_ptr<const CapturedCandidate> capture_;
        std::shared_ptr<scripting::BaseHost> base_;
        std::shared_ptr<scripting::SemanticEndpoint> endpoint_;
        std::shared_ptr<runtime::RuntimeScope> scope_;
        ModDefinitionFingerprint fingerprint_;
        ModId owner_;
        std::uint64_t baseEpoch_{}, baseGeneration_{};
        std::uint64_t vmGeneration_{};
        std::shared_ptr<runtime::RuntimeCommitDomain> domain_;
        std::uint64_t domainRevision_{};
        runtime::ScopeSnapshot scopeSnapshot_;
        scripting::SemanticPublication contextSnapshot_;
        std::vector<ResourcePatchDefinition> declarations_;
        /** @summary Immutable consumed VM snapshot captured at initialization, independent of caller-owned definitions. */
        std::shared_ptr<const scripting::ScriptDefinition> loaded_;
        // @summary VM is destroyed first, while its exact endpoint/Base and captured generation remain alive.
        std::shared_ptr<scripting::ScriptRuntime> vm_;
    };
private:
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerPreparationTestAccess;
    /** @summary Prepare an exact capture for an actual inactive entry, without native production issuance.
     * @param manager Actual native manager. @param capture Exact captured bytes and installation.
     * @param base Actual retained verified package. @param observer Fixture-only phase evidence.
     * @param qualified Select existing fixture scope before binding; never selectable by a manifest.
     * @return Complete uncommitted preparation; throws on any rejection. */
    static Plan Prepare(ModManager& manager, std::shared_ptr<const CapturedCandidate> capture,
        std::shared_ptr<scripting::BaseHost> base, std::function<void(scripting::ScriptPhase,std::uint64_t)> observer,
        bool qualified = false);
    /** @summary Copy bounded staged diagnostics for fixture evidence without exposing uncommitted public logs.
     * @param plan Exact isolated preparation. @return Retained log copies. */
    static std::vector<std::string> StagedLogs(const Plan& plan);
#endif
    /** @summary Revalidate native observations without filesystem reads or borrowed-resource access after closure.
     * @param plan Exact preparation. @return Ok, Busy, Stale or closure rejection; never operation permission. */
    static runtime::ScopeResult Current(const Plan& plan);
    /** @summary Verify immutable Base package and original bridge without taking publication locks.
     * @param plan Retained native preparation. @return Exact original package/bridge identity. */
    static bool OriginalBase(const Plan& plan) noexcept;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Closed fixture-only preparation producer; no counterpart is linked into production. */
struct ManagerPreparationTestAccess final {
    /** @summary Install a real after-first-Current/before-bind cut outside all publication guards.
     * @param callback Temporary actual VM borrow; join/release all launched controls before callback return. */
    static void BeforeBind(std::function<void(const std::shared_ptr<scripting::ScriptRuntime>&)> callback);
    /** @summary Probe the real private native binding helper without changing fixture counters.
     * @param manager Original owner, alive throughout probe. @param vm Temporarily borrowed actual candidate. @return Native result. */
    static runtime::ScopeResult Bind(ModManager& manager,const std::shared_ptr<scripting::ScriptRuntime>& vm);
    /** @summary Hold actual L, not scheduler S, for a bounded fixture gate.
     * @param manager Original owner. @param callback Native gate only; never Lua or lane work. */
    static void HoldLane(ModManager& manager,const std::function<void()>& callback);
    /** @summary Read exact original VM/slot/host association without granting eligibility.
     * @param manager Original owner. @param vm Actual fixture candidate. @return Native coherent observation. */
    static runtime::ScopeResult Binding(ModManager& manager,const std::shared_ptr<scripting::ScriptRuntime>& vm);
    /** @summary Capture under an actual entry's native installation binding.
     * @param manager Original manager. @param id Actual entry. @param directory Candidate source.
     * @param pin Full expected Base pin. @return Immutable capture or rejection. */
    static std::shared_ptr<const CapturedCandidate> Capture(ModManager& manager, const ModId& id,
        const std::filesystem::path& directory, scripting::BasePackagePin pin);
    /** @summary Run restricted initialization using actual native participants.
     * @param manager Owner. @param capture Immutable capture. @param base Actual package.
     * @param observer Optional deterministic fixture evidence. @return Owned uncommitted candidate. */
    static ManagerPreparation::Plan Prepare(ModManager& manager, std::shared_ptr<const CapturedCandidate> capture,
        std::shared_ptr<scripting::BaseHost> base, std::function<void(scripting::ScriptPhase,std::uint64_t)> observer={}) {
        return ManagerPreparation::Prepare(manager,std::move(capture),std::move(base),std::move(observer));
    }
    /** @summary Read current native binding validity. @param plan Original preparation. @return Native observation status. */
    static runtime::ScopeResult Current(const ManagerPreparation::Plan& plan){return ManagerPreparation::Current(plan);}
    /** @summary Access actual isolated VM for bypass oracles. @param plan Original preparation. @return Retained VM. */
    static std::shared_ptr<scripting::ScriptRuntime> Vm(const ManagerPreparation::Plan& plan){return plan.vm_;}
    /** @summary Inspect bounded reduced declarations. @param plan Original preparation. @return Immutable local output. */
    static const std::vector<ResourcePatchDefinition>& Declarations(const ManagerPreparation::Plan& plan){return plan.declarations_;}
    /** @summary Observe original scope without issuing permission. @param plan Original preparation. @return Closed native scope. */
    static std::shared_ptr<runtime::RuntimeScope> Scope(const ManagerPreparation::Plan& plan){return plan.scope_;}
    /** @summary Access the exact endpoint for native fixture bypass oracles, never issuance.
     * @param plan Original retained preparation. @return Its original endpoint. */
    static std::shared_ptr<scripting::SemanticEndpoint> Endpoint(const ManagerPreparation::Plan& plan){return plan.endpoint_;}
    /** @summary Observe staged logs only through the native fixture build.
     * @param plan Exact preparation. @return Bounded copied logs. */
    static std::vector<std::string> StagedLogs(const ManagerPreparation::Plan& plan){return ManagerPreparation::StagedLogs(plan);}
};
#endif
}
