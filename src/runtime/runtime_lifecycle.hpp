#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace sc13::runtime {
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
    /** @summary Releases the publisher; already executing delivery owns its state. */
    ~RuntimeLifecycle();
    RuntimeLifecycle(const RuntimeLifecycle&) = delete;
    RuntimeLifecycle& operator=(const RuntimeLifecycle&) = delete;
    /** @summary Subscribes atomically with current state; zero means bounded-capacity rejection.
     * @param callback Receiver, invoked outside the state lock. @return Owned subscription identity or zero. */
    Subscription SubscribeCurrent(Callback callback);
    /** @summary Closes callback admission without waiting on a possibly reentrant receiver.
     * @param subscription Identity to retire; reserved callbacks may finish. @return False while a callback retains the receiver. */
    bool Unsubscribe(Subscription subscription) noexcept;
    /** @summary Publishes observed facts; stopping is sticky and overflow fails closed.
     * @param observed New factual flags; sequence is generated internally. @return Whether publication was accepted. */
    bool Publish(LifecycleSnapshot observed);
    /** @summary Returns a copied factual snapshot. @return Current state and sequence. */
    LifecycleSnapshot Current() const;
private:
    struct State;
    std::shared_ptr<State> state_;
    /** @summary Runs one serialized delivery lane. @param state Lifetime retained across reentrant callbacks. */
    static void Deliver(const std::shared_ptr<State>& state) noexcept;
};
}
