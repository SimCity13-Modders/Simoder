#include "runtime/semantic_tasks.hpp"
#include <utility>
namespace sc13::runtime {
namespace {
/** @summary Issues non-reused private adapter identity; this conveys no authority. @return Identity or zero on exhaustion. */
std::uint64_t NextIssuer() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load();
    while (value != UINT64_MAX) if (next.compare_exchange_weak(value, value + 1)) return value;
    return 0;
}
}
SemanticTasks::SemanticTasks(std::shared_ptr<RuntimeScope> scope, std::shared_ptr<copied::Storage> storage)
    : scope_(std::move(scope)), storage_(std::move(storage)), owner_(scope_ ? scope_->Current().generation : 0), issuer_(NextIssuer()) {}
ScopeResult SemanticTasks::Admit(const OperationContext& context, const copied::Envelope& input, std::size_t maxResultWire, TaskHandle& output) {
    output = {};
    if (!scope_ || !storage_ || !issuer_ || context.owner != owner_) return ScopeResult::Denied;
    const auto result = scope_->QueueValue(context, input, storage_, maxResultWire, output.handle_);
    if (result == ScopeResult::Ok) output.issuer_ = issuer_;
    return result;
}
ScopeResult SemanticTasks::Begin(TaskHandle task) { return scope_ && task.issuer_ == issuer_ && task.handle_.owner == owner_ ? scope_->BeginValue(task.handle_) : ScopeResult::Denied; }
ScopeResult SemanticTasks::Commit(TaskHandle task, const ValueCallback& callback, const std::function<ScopeResult()>& guard) { return scope_ && task.issuer_ == issuer_ && task.handle_.owner == owner_ ? scope_->CommitValue(task.handle_, callback, guard) : ScopeResult::Denied; }
ScopeResult SemanticTasks::CommitPrepared(TaskHandle task, RuntimeCommitDomain::Writer& writer,
    mods::PatchRegistry::PublicationGuard& publication, mods::PatchRegistry::PreparedPublication& plan,
    std::uint64_t scopeGeneration, std::uint64_t runtimeEpoch, const copied::Envelope& input) {
    return scope_ && task.issuer_ == issuer_ && task.handle_.owner == owner_
        ? scope_->CommitPreparedValue(task.handle_, writer, publication, plan, scopeGeneration, runtimeEpoch, input)
        : ScopeResult::Denied;
}
ScopeResult SemanticTasks::FinishUncommitted(TaskHandle task, ScopeResult reason) { return scope_ && task.issuer_ == issuer_ && task.handle_.owner == owner_ ? scope_->FinishValue(task.handle_, reason) : ScopeResult::Denied; }
ScopeResult SemanticTasks::Poll(TaskHandle task, TaskSnapshot& output) const {
    output = {};
    return scope_ && task.issuer_ == issuer_ && task.handle_.owner == owner_ ? scope_->PollValue(task.handle_, output) : ScopeResult::Denied;
}
ScopeResult SemanticTasks::Cancel(TaskHandle task) { return scope_ && task.issuer_ == issuer_ && task.handle_.owner == owner_ ? scope_->CancelValue(task.handle_) : ScopeResult::Denied; }
ScopeResult SemanticTasks::Release(TaskHandle task) { return scope_ && task.issuer_ == issuer_ && task.handle_.owner == owner_ ? scope_->Release(owner_, task.handle_) : ScopeResult::Denied; }
}
