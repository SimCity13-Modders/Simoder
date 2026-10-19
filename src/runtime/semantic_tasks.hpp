#pragma once
#include "runtime/runtime_scope.hpp"
namespace sc13::runtime {
/** @summary Opaque native generation identity, never reconstructed from script numbers. */
class TaskHandle final {
public:
    /** @summary Creates an empty non-authoritative handle. */
    TaskHandle() noexcept = default;
    /** @summary Tests host identity presence. @return Nonempty issued handle. */
    explicit operator bool() const noexcept { return issuer_ && handle_.owner && handle_.serial; }
    bool operator==(const TaskHandle&) const = default;
private:
    ScopeHandle handle_{};
    std::uint64_t issuer_{};
    friend class SemanticTasks;
};
/** @summary Thin copied-value adapter over the sole RuntimeScope authority/task store. */
class SemanticTasks final {
public:
    using ValueCallback = RuntimeScope::ValueCallback;
    /** @summary Binds existing authority and pool without minting permission. @param scope Native scope. @param storage Shared pool. */
    SemanticTasks(std::shared_ptr<RuntimeScope> scope, std::shared_ptr<copied::Storage> storage);
    SemanticTasks(const SemanticTasks&) = delete;
    SemanticTasks& operator=(const SemanticTasks&) = delete;
    /** @summary Reserves original-context operation. @param context Host context. @param input Same-pool value. @param maxResultWire Reviewed limit. @param output Cleared task handle. @return Admission. */
    ScopeResult Admit(const OperationContext& context, const copied::Envelope& input, std::size_t maxResultWire, TaskHandle& output);
    /** @summary Begins cancelable preparation. @param task Native handle. @return Gate. */
    ScopeResult Begin(TaskHandle task);
    /** @summary Commits and completes once using preallocated output. @param task Native handle. @param callback Trusted effect.
     * @param guard Optional trusted atomic context fence forwarded to the sole native commit boundary. @return Outcome. */
    ScopeResult Commit(TaskHandle task, const ValueCallback& callback,
        const std::function<ScopeResult()>& guard = {});
    /** @summary Fails uncommitted task. @param task Handle. @param reason Failure. @return Completion. */
    ScopeResult FinishUncommitted(TaskHandle task, ScopeResult reason);
    /** @summary Delivers only after current rights validation. @param task Handle. @param output Cleared on denial. @return Status. */
    ScopeResult Poll(TaskHandle task, TaskSnapshot& output) const;
    /** @summary Cancels uncommitted work. @param task Handle. @return Status. */
    ScopeResult Cancel(TaskHandle task);
    /** @summary Releases retained state even after revocation. @param task Handle. @return Cleanup status. */
    ScopeResult Release(TaskHandle task);
private:
    friend class sc13::scripting::SemanticBridge;
    /** @summary Bridges only an actual context-reserved registry publication to its original task.
     * @param task Issuer-bound handle. @param writer Shared writer reservation. @param publication Registry reservation.
     * @param plan Prepared native publication. @param scopeGeneration Captured owner. @param runtimeEpoch Captured epoch.
     * @param input Original copied request, never replacement task data.
     * @return Native admission or completion status. */
    ScopeResult CommitPrepared(TaskHandle task, RuntimeCommitDomain::Writer& writer,
        mods::PatchRegistry::PublicationGuard& publication, mods::PatchRegistry::PreparedPublication& plan,
        std::uint64_t scopeGeneration, std::uint64_t runtimeEpoch, const copied::Envelope& input);
    std::shared_ptr<RuntimeScope> scope_;
    std::shared_ptr<copied::Storage> storage_;
    std::uint64_t owner_{};
    std::uint64_t issuer_{};
};
}
