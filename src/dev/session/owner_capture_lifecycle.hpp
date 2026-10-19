#pragma once
#include "hooks/runtime_hooks.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>

namespace sc13::dev::session {
/** @summary Preserve actual hook-control outcomes without adopting foreign hook ownership. */
struct OwnerHookResult final {
    enum class Kind { Success, AlreadyDisabled, Failure, Foreign };
    Kind kind{Kind::Failure};
    int nativeStatus{};
    const char* diagnostic{"Owner hook operation was not completed"};
};
/** @summary Inject only the three owned MinHook operations; removal is deliberately not a supported operation. */
struct OwnerHookOperations final {
    void* context{};
    OwnerHookResult (*create)(void*, std::uintptr_t&) noexcept{};
    OwnerHookResult (*enable)(void*) noexcept{};
    OwnerHookResult (*disable)(void*) noexcept{};
};
/** @summary Own one process-retained original and revoke optional observation independently of native forwarding. */
class OwnerCaptureLifecycle final {
public:
    enum class State { Cold, CreatedUnexposed, Enabled, DeclinedNoOwnedHook, RetiredRetained, QuarantinedRetained };
    /** @summary Report actual transition results without equating retained code with native object validity. */
    struct Outcome final {
        bool ready{};
        bool stopped{};
        State state{State::Cold};
        OwnerHookResult result;
        OwnerHookResult cleanup;
        bool cleanupAttempted{};
    };
    /** @summary Bind the existing shared runtime callback activity owner.
     * @param activity Process-owned activity, retained through all original calls. */
    explicit OwnerCaptureLifecycle(hooks::RuntimeHookActivity& activity) noexcept : activity_(activity) {}
    /** @summary Create once, publish original before exposure and conservatively reconcile failed enable.
     * @param target Immutable exact-build callback identity. @param operations Actual owned hook-control adapter.
     * @return Readiness or retained/declined failure with its actual status. */
    [[nodiscard]] Outcome Initialize(std::uintptr_t target, OwnerHookOperations operations) noexcept;
    /** @summary Close all observation scopes before attempting disable; never remove or clear original.
     * @return True stopped only after exact owned disable/no-owned-hook acknowledgement. */
    [[nodiscard]] Outcome Retire() noexcept;
    /** @summary Revoke current and other-thread optional scopes without acquiring a callback lock. */
    void CloseObservation() noexcept;
    /** @summary Acquire the current optional observation epoch.
     * @return Nonzero epoch only for an admitted enabled owner. */
    [[nodiscard]] std::uint64_t BeginEpoch() const noexcept;
    /** @summary Reject stale, nested-invalidated or globally revoked scope epochs.
     * @param epoch Scope's previously issued epoch. @return Whether optional work may begin. */
    [[nodiscard]] bool ObservationAllowed(std::uint64_t epoch) const noexcept;
    /** @summary Load the immutable original without claiming ownership of any native data.
     * @return Published trampoline address, never cleared after successful Create. */
    [[nodiscard]] std::uintptr_t Original() const noexcept { return original_.load(std::memory_order_acquire); }
    /** @summary Expose actual lifecycle state for diagnostics and deterministic fixtures.
     * @return Current state, not native quiescence. */
    [[nodiscard]] State CurrentState() const noexcept { return state_.load(std::memory_order_acquire); }
    /** @summary Reuse the single existing runtime activity counter for actual original forwarding.
     * @return Process-owned activity reference; no separate authority or counter is created. */
    [[nodiscard]] hooks::RuntimeHookActivity& Activity() noexcept { return activity_; }
private:
    /** @summary Disable only this successfully created hook and retain its original in every outcome. */
    [[nodiscard]] Outcome DisableLocked() noexcept;
    hooks::RuntimeHookActivity& activity_;
    std::mutex control_;
    std::atomic<State> state_{State::Cold};
    std::atomic<std::uintptr_t> original_{};
    std::atomic_bool retiring_{};
    std::atomic_bool observationOpen_{};
    std::atomic_uint64_t epoch_{1};
    std::uintptr_t target_{};
    OwnerHookOperations operations_;
    bool created_{};
};
/** @summary Keep thread-local scope metadata without treating a borrowed pointer as a retained native object. */
class OwnerCaptureObservation final {
public:
    /** @summary Start at most one optional observation; nesting invalidates observation until the outer scope ends.
     * @param owner Current lifecycle owner. @return Whether this scope acquired an admitted epoch. */
    [[nodiscard]] bool Begin(const OwnerCaptureLifecycle& owner) noexcept;
    /** @summary End one scope without restoring a revoked or nested-invalidated epoch. */
    void End() noexcept;
    /** @summary Test epoch validity against global revocation on every optional-work entry.
     * @param owner Current lifecycle owner. @return Whether this thread's scope is current. */
    [[nodiscard]] bool Allowed(const OwnerCaptureLifecycle& owner) const noexcept { return owner.ObservationAllowed(epoch_); }
private:
    std::uint64_t epoch_{};
    std::uint32_t depth_{};
};
}
