#include "dev/session/owner_capture_lifecycle.hpp"
#include <limits>

namespace sc13::dev::session {
namespace {
/** @summary Create a factual local refusal without pretending that a native operation ran. */
OwnerHookResult Refused(const char* diagnostic) noexcept { return {OwnerHookResult::Kind::Failure, 0, diagnostic}; }
}
OwnerCaptureLifecycle::Outcome OwnerCaptureLifecycle::Initialize(std::uintptr_t target, OwnerHookOperations operations) noexcept {
    const std::lock_guard lock(control_);
    const auto current = CurrentState();
    if (current == State::Enabled && !retiring_.load() && target_ == target &&
        operations_.context == operations.context && operations_.create == operations.create &&
        operations_.enable == operations.enable && operations_.disable == operations.disable)
        return {true, false, current, {OwnerHookResult::Kind::Success, 0, "Owner hook already enabled"}};
    if (current != State::Cold || retiring_.load())
        return {false, false, current, Refused("Owner hook cannot be recreated after decline or retirement; restart required")};
    if (!target || !operations.create || !operations.enable || !operations.disable) {
        state_.store(State::DeclinedNoOwnedHook);
        return {false, true, CurrentState(), Refused("Owner hook adapter or target is incomplete")};
    }
    target_ = target; operations_ = operations;
    std::uintptr_t original{};
    const auto created = operations_.create(operations_.context, original);
    if (created.kind != OwnerHookResult::Kind::Success) {
        state_.store(created.kind != OwnerHookResult::Kind::Failure || original ? State::QuarantinedRetained : State::DeclinedNoOwnedHook);
        return {false, CurrentState() == State::DeclinedNoOwnedHook, CurrentState(), created};
    }
    created_ = true;
    state_.store(State::CreatedUnexposed);
    if (!original) {
        state_.store(State::QuarantinedRetained);
        return {false, false, CurrentState(), Refused("Create succeeded without an original; retained without enabling")};
    }
    original_.store(original, std::memory_order_release);
    if (retiring_.load()) return DisableLocked();
    const auto enabled = operations_.enable(operations_.context);
    if (enabled.kind != OwnerHookResult::Kind::Success) {
        CloseObservation();
        const auto cleanup = DisableLocked();
        // @summary Keep the primary enable failure even when conservative disable succeeds.
        Outcome outcome{false, cleanup.stopped, cleanup.state, enabled};
        outcome.cleanup = cleanup.result;
        outcome.cleanupAttempted = true;
        return outcome;
    }
    if (retiring_.load()) return DisableLocked();
    state_.store(State::Enabled);
    observationOpen_.store(true, std::memory_order_release);
    return {true, false, State::Enabled, enabled};
}
void OwnerCaptureLifecycle::CloseObservation() noexcept {
    retiring_.store(true, std::memory_order_release);
    observationOpen_.store(false, std::memory_order_release);
    // @summary Saturation cannot restore admission: retirement is terminal for this process-owned owner.
    auto value = epoch_.load();
    while (value != std::numeric_limits<std::uint64_t>::max() && !epoch_.compare_exchange_weak(value, value + 1)) {}
}
OwnerCaptureLifecycle::Outcome OwnerCaptureLifecycle::DisableLocked() noexcept {
    if (!created_) {
        if (CurrentState() == State::QuarantinedRetained)
            return {false, false, CurrentState(), Refused("Foreign or unknown hook ownership remains quarantined")};
        state_.store(State::RetiredRetained);
        return {false, true, CurrentState(), {OwnerHookResult::Kind::Success, 0, "No owned hook was created"}};
    }
    const auto disabled = operations_.disable(operations_.context);
    if (disabled.kind != OwnerHookResult::Kind::Success && disabled.kind != OwnerHookResult::Kind::AlreadyDisabled) {
        state_.store(State::QuarantinedRetained);
        return {false, false, CurrentState(), disabled};
    }
    state_.store(State::RetiredRetained);
    return {false, activity_.Drained(), CurrentState(), disabled};
}
OwnerCaptureLifecycle::Outcome OwnerCaptureLifecycle::Retire() noexcept {
    CloseObservation();
    const std::lock_guard lock(control_);
    return DisableLocked();
}
std::uint64_t OwnerCaptureLifecycle::BeginEpoch() const noexcept {
    const auto epoch = epoch_.load(std::memory_order_acquire);
    return ObservationAllowed(epoch) ? epoch : 0;
}
bool OwnerCaptureLifecycle::ObservationAllowed(std::uint64_t epoch) const noexcept {
    return epoch && !retiring_.load(std::memory_order_acquire) && observationOpen_.load(std::memory_order_acquire) &&
        CurrentState() == State::Enabled && epoch_.load(std::memory_order_acquire) == epoch;
}
bool OwnerCaptureObservation::Begin(const OwnerCaptureLifecycle& owner) noexcept {
    if (depth_) {
        epoch_ = 0;
        if (depth_ != std::numeric_limits<std::uint32_t>::max()) ++depth_;
        return false;
    }
    depth_ = 1;
    epoch_ = owner.BeginEpoch();
    return epoch_ != 0;
}
void OwnerCaptureObservation::End() noexcept { if (depth_) --depth_; if (!depth_) epoch_ = 0; }
}
