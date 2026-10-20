#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include "runtime/lifecycle_authority_source.hpp"
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
#include <mutex>
#endif

namespace sc13::mods::native { class NativeModuleLoader; }
namespace sc13::runtime {
class RuntimeCommitDomain;
/** @summary Checked factual observation outcome, never operation authority. */
enum class LifecycleObservationStatus {Ok,Busy,Unavailable};
/** @summary Factual observed state; rendering initialization does not imply menu/city readiness. */
struct LifecycleSnapshot final {
    std::uint64_t sequence{};
    bool runtimePrepared{};
    bool menuObserved{};
    bool cityObserved{};
    bool stopping{};
};

/** @summary Delivers bounded, serialized current-state/events outside its state lock. */
class RuntimeLifecycle final {
public:
    using Subscription = std::uint64_t;
    using Callback = std::function<void(const LifecycleSnapshot&)>;
    /** @summary Creates an initially unprepared factual lifecycle. */
    RuntimeLifecycle();
    /** @summary Bind the actual existing Core writer domain; null remains factual-only and cannot mint a source.
     * @param domain Original domain shared by the owning runtime scopes, never a replacement lane. */
    explicit RuntimeLifecycle(std::shared_ptr<RuntimeCommitDomain> domain);
    /** @summary Releases the publisher; already executing delivery owns its state.
     * @pre Caller holds no original writer/publication or lifecycle guard. */
    ~RuntimeLifecycle();
    RuntimeLifecycle(const RuntimeLifecycle&) = delete;
    RuntimeLifecycle& operator=(const RuntimeLifecycle&) = delete;
    /** @summary Subscribes atomically with current state; zero means bounded-capacity rejection.
     * @param callback Receiver, invoked outside the state lock. @return Owned subscription identity or zero.
     * @pre Callback construction/transfer and final capture disposal occur outside publication/State/L guards.
     * A guarded rejection does not make by-value parameter destruction safe; retain owning captures off guards. */
    Subscription SubscribeCurrent(Callback callback);
    /** @summary Closes callback admission without waiting on a possibly reentrant receiver.
     * @param subscription Identity to retire; reserved callbacks may finish. @return False while a callback retains the receiver. */
    bool Unsubscribe(Subscription subscription) noexcept;
    /** @summary Publishes observed facts; stopping is sticky and overflow fails closed.
     * @param observed New factual flags; sequence is generated internally. @return Whether publication was accepted. */
    bool Publish(LifecycleSnapshot observed);
    /** @summary Returns a copied factual snapshot. @return Current state and sequence.
     * @pre Caller holds no original D/publication/State/L guard; guarded consumers use TryCurrent. */
    LifecycleSnapshot Current() const;
    /** @summary Copied close protocol result; notification and authority closure are independent. */
    struct CloseReceipt final {
        LifecycleClosePhase phase{LifecycleClosePhase::Unbound};
        LifecycleCloseReason reason{LifecycleCloseReason::None};
        bool notificationPublished{};
        bool notificationFailed{};
    };
    /** @summary Read original close state without granting rights or advancing any sequence.
     * @return Unbound for a factual-only publisher, Pending as Closing, or actual checkpoint result.
     * @pre Caller holds no original D/publication/State/L guard; guarded consumers use TryCloseStatus. */
    CloseReceipt CloseStatus() const;
    /** @summary Nonthrow observation rejects actual same-D reentry before L/ownership/output changes.
     * @param output Unchanged unless Ok copies actual historical Current. @return Ok, Busy or Unavailable.
     * @pre Publisher remains alive; caller does not own original L. No authority is inferred from historical facts. */
    LifecycleObservationStatus TryCurrent(LifecycleSnapshot& output) const noexcept;
    /** @summary Nonthrow receipt observation uses one coherent cell sample and original L notification facts.
     * @param output Unchanged unless Ok. @return Ok (including factual-only Unbound), Busy or Unavailable.
     * @pre Publisher remains alive; caller does not own original L. */
    LifecycleObservationStatus TryCloseStatus(CloseReceipt& output) const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
    /** @summary Runs one serialized delivery lane. @param state Lifetime retained across reentrant callbacks. */
    static void Deliver(const std::shared_ptr<State>& state) noexcept;
    /** @summary Request sticky intent without locks or ownership changes, including same-domain reentry.
     * @param state Already retained original state. @param reason First close cause. */
    static void RequestCloseIntent(State& state,LifecycleCloseReason reason) noexcept;
    /** @summary Acknowledge only by acquiring original D outside L; never finalize an event.
     * @param state Original retained state. */
    static void AcknowledgeClose(const std::shared_ptr<State>& state) noexcept;
    /** @summary Try original D outside L, then finalize the same pending event outside D.
     * @param state Original retained state. @return True only if this call published its original stop notification. */
    static bool AdvanceClose(const std::shared_ptr<State>& state) noexcept;
    /** @summary Copy notification facts under L plus one coherent original cell word.
     * @param state Original L-held state. @return Original receipt, never source validity or permission. */
    static CloseReceipt ReceiptLocked(const State& state) noexcept;
    /** @summary Stage only the actual original cell off guards; foreign domains remain unavailable.
     * @param expected Original retained domain, compared by exact identity. @return Actual Open source or empty. */
    std::shared_ptr<const LifecycleAuthoritySource> StageAuthoritySource(const RuntimeCommitDomain& expected) const;
    friend class mods::native::NativeModuleLoader;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct RuntimeLifecycleTestAccess;
#endif
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fixture-only negative cuts/read-only facts; no source validity setter or authority producer. */
struct RuntimeLifecycleTestAccess final {
    /** @summary Original allocation boundary to fail once on the fixture thread. */
    enum class AllocationCut { None, Receiver, Snapshot, Event, Subscription };
    /** @summary Original queue accounting without receiver/payload access. */
    struct Observation final {
        std::size_t queued{},pending{};
        bool delivering{};
        std::size_t stateBytes{},eventBytes{},sourceBytes{},receiptBytes{};
    };
    /** @summary Arm one negative allocation cut. @param cut Existing allocation boundary. */
    static void FailAllocation(AllocationCut cut) noexcept;
    /** @summary Observe original L-held queue. @param lifecycle Original publisher. @return Copied counts. */
    static Observation Observe(const RuntimeLifecycle& lifecycle);
    /** @summary Retain actual source off guards for destructor observation, never fabricate one.
     * @param lifecycle Original publisher. @param expected Original domain. @return Original source or empty. */
    static std::shared_ptr<const LifecycleAuthoritySource> Source(const RuntimeLifecycle& lifecycle,const RuntimeCommitDomain& expected);
    /** @summary Exhaust only the original fixture sequence when no delivery/event is outstanding.
     * @param lifecycle Original bound fixture. @return False if unsafe or unbound; never changes source phase. */
    static bool ExhaustSequence(RuntimeLifecycle& lifecycle);
    /** @summary Hold actual original L on a fixture thread for contention tests only.
     * @param lifecycle Original publisher. @return Original lock, never an authority setter.
     * @pre Caller owns no original D/L; publisher outlives the lock. */
    static std::unique_lock<std::mutex> ReserveState(RuntimeLifecycle& lifecycle);
};
#endif
}
