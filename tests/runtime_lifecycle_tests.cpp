#include "runtime/runtime_lifecycle.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace sc13::runtime;
using namespace std::chrono_literals;
namespace {
/** @summary Fails an exact behavioral expectation. @param value Observed condition. @param message Failure diagnostic. */
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
/** @summary Verifies current-before-events ordering, sticky stop and reentrant unsubscribe. */
void Ordering() {
    RuntimeLifecycle lifecycle;
    Require(lifecycle.Publish({0, true, false, false, false}), "runtime publication");
    std::vector<std::uint64_t> sequence;
    const auto first = lifecycle.SubscribeCurrent([&](const auto& state) { sequence.push_back(state.sequence); });
    Require(first != 0 && sequence == std::vector<std::uint64_t>{1}, "late current state missing");
    bool selfBusy{};
    RuntimeLifecycle::Subscription self{};
    self = lifecycle.SubscribeCurrent([&](const auto& state) {
        if (state.sequence == 2) selfBusy = !lifecycle.Unsubscribe(self);
    });
    Require(lifecycle.Publish({0, true, true, false, false}), "menu publication");
    Require(selfBusy && lifecycle.Unsubscribe(self), "self unsubscribe did not expose retained callback");
    Require(lifecycle.Publish({0, true, false, true, false}), "city publication");
    Require(sequence == std::vector<std::uint64_t>({1, 2, 3}), "ordered events violated");
    Require(lifecycle.Publish({0, false, false, false, true}), "stop publication");
    Require(lifecycle.Publish({0, true, true, true, false}) && lifecycle.Current().stopping, "stop unstuck");
    Require(lifecycle.Unsubscribe(first), "idle receiver not drained");
}
/** @summary Verifies concurrent unsubscribe closes admission and reports retained current callback. */
void ConcurrentUnsubscribe() {
    RuntimeLifecycle lifecycle;
    std::promise<void> entered, release;
    auto releaseSignal = release.get_future().share();
    std::atomic_int calls{};
    const auto id = lifecycle.SubscribeCurrent([&](const auto& state) {
        ++calls;
        if (state.sequence == 1) { entered.set_value(); Require(releaseSignal.wait_for(10s) == std::future_status::ready, "receiver release timeout"); }
    });
    auto writer = std::async(std::launch::async, [&] { return lifecycle.Publish({0, true}); });
    Require(entered.get_future().wait_for(5s) == std::future_status::ready, "receiver never entered");
    const bool firstBusy = !lifecycle.Unsubscribe(id);
    const bool stillBusy = !lifecycle.Unsubscribe(id);
    const bool queued = lifecycle.Publish({0, true, true});
    release.set_value();
    Require(writer.get(), "writer failed");
    Require(firstBusy && stillBusy && queued && lifecycle.Unsubscribe(id) && calls == 2, "unsubscribe admission/drain race");
}
/** @summary Verifies destruction retires queued delivery but retains reserved callbacks safely. */
void Destruction() {
    auto lifecycle = std::make_unique<RuntimeLifecycle>();
    std::promise<void> entered, release;
    auto releaseSignal = release.get_future().share();
    std::atomic_int calls{};
    lifecycle->SubscribeCurrent([&](const auto& state) {
        ++calls;
        if (state.sequence == 1) { entered.set_value(); Require(releaseSignal.wait_for(10s) == std::future_status::ready, "destruction release timeout"); }
    });
    RuntimeLifecycle* const borrowed = lifecycle.get();
    auto writer = std::async(std::launch::async, [&] { return borrowed->Publish({0, true}); });
    Require(entered.get_future().wait_for(5s) == std::future_status::ready, "destruction receiver never entered");
    Require(lifecycle->Publish({0, true, true}), "queued destruction event");
    lifecycle.reset();
    release.set_value();
    Require(writer.get() && calls == 2, "destruction delivered retired callback");
}
/** @summary Destruction of captured resources can reenter the same live publisher. */
struct ReentrantResource final {
    RuntimeLifecycle* lifecycle;
    bool* destroyed;
    /** @summary Proves capture destruction happens without the publisher state lock held. */
    ~ReentrantResource() { static_cast<void>(lifecycle->Current()); *destroyed = true; }
};
/** @summary Verifies receiver destruction and bounded overload paths avoid locks/reentrant deadlock. */
void ResourceAndBounds() {
    RuntimeLifecycle lifecycle;
    bool destroyed{};
    auto resource = std::make_shared<ReentrantResource>();
    resource->lifecycle = &lifecycle; resource->destroyed = &destroyed;
    const auto id = lifecycle.SubscribeCurrent([resource](const auto&) {});
    resource.reset();
    Require(lifecycle.Unsubscribe(id) && destroyed, "callback capture released under lock");
    std::vector<RuntimeLifecycle::Subscription> subscriptions;
    for (int index = 0; index != 64; ++index) subscriptions.push_back(lifecycle.SubscribeCurrent([](const auto&) {}));
    Require(lifecycle.SubscribeCurrent([](const auto&) {}) == 0, "subscription capacity exceeded");
    for (const auto subscription : subscriptions) Require(subscription != 0 && lifecycle.Unsubscribe(subscription), "bounded subscription cleanup");
    int rejected{};
    const auto flood = lifecycle.SubscribeCurrent([&](const auto& state) {
        if (state.sequence != 1) return;
        for (int index = 0; index != 130; ++index) if (!lifecycle.Publish({0, true})) ++rejected;
    });
    Require(lifecycle.Publish({0, true}) && rejected == 2, "event queue not bounded");
    Require(lifecycle.Unsubscribe(flood), "flood receiver cleanup");
    const auto throwing = lifecycle.SubscribeCurrent([](const auto&) { throw std::runtime_error("receiver failure"); });
    Require(throwing != 0 && lifecycle.Unsubscribe(throwing), "faulted callback not retired");
}
}
/** @summary Executes isolated factual lifecycle ordering and callback-lifetime tests. */
int main() {
    try { Ordering(); ConcurrentUnsubscribe(); Destruction(); ResourceAndBounds(); std::cout << "Lifecycle ordering, bounded delivery and callback drain passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
