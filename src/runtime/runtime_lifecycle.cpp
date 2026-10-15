#include "runtime/runtime_lifecycle.hpp"

#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace sc13::runtime {
namespace {
/** @summary Bound simultaneously retained lifecycle receivers. */
constexpr std::size_t MaxSubscriptions = 64;
/** @summary Bound pending factual events while a callback is running. */
constexpr std::size_t MaxEvents = 128;
}
/** @summary Retains receiver lifetime independently of the publisher's subscription map. */
struct RuntimeLifecycle::State final {
    /** @summary One retired-or-live receiver; callbacks never execute while the state lock is held. */
    struct Receiver final { Callback callback; bool active{true}; std::size_t running{}; };
    /** @summary One ordered factual event and its bounded receiver snapshot. */
    struct Event final { LifecycleSnapshot snapshot; std::vector<std::shared_ptr<Receiver>> receivers; };
    std::mutex mutex;
    LifecycleSnapshot current;
    std::map<Subscription, std::shared_ptr<Receiver>> subscriptions;
    std::deque<Event> events;
    Subscription next{1};
    bool delivering{};
    bool closed{};
};

RuntimeLifecycle::RuntimeLifecycle() : state_(std::make_shared<State>()) {}
RuntimeLifecycle::~RuntimeLifecycle() {
    std::map<Subscription, std::shared_ptr<State::Receiver>> retired;
    std::deque<State::Event> cancelled;
    {
        const std::lock_guard lock(state_->mutex);
        state_->closed = true;
        for (const auto& [id, receiver] : state_->subscriptions) { (void)id; receiver->active = false; }
        retired.swap(state_->subscriptions);
        cancelled.swap(state_->events);
    }
}

RuntimeLifecycle::Subscription RuntimeLifecycle::SubscribeCurrent(Callback callback) {
    if (!callback) return 0;
    const auto state = state_;
    auto receiver = std::make_shared<State::Receiver>();
    receiver->callback = std::move(callback);
    Subscription id{};
    {
        const std::lock_guard lock(state->mutex);
        if (state->closed || state->subscriptions.size() >= MaxSubscriptions || state->events.size() >= MaxEvents ||
            state->next == std::numeric_limits<Subscription>::max()) return 0;
        id = state->next;
        state->events.push_back({state->current, {receiver}});
        try { state->subscriptions.emplace(id, receiver); }
        catch (...) { state->events.pop_back(); throw; }
        ++state->next;
    }
    Deliver(state);
    return id;
}

bool RuntimeLifecycle::Unsubscribe(Subscription subscription) noexcept {
    const auto state = state_;
    std::shared_ptr<State::Receiver> retired;
    {
        const std::lock_guard lock(state->mutex);
        const auto found = state->subscriptions.find(subscription);
        if (found != state->subscriptions.end()) {
            found->second->active = false;
            if (found->second->running != 0) return false;
            retired = std::move(found->second);
            state->subscriptions.erase(found);
        }
    }
    return true;
}

bool RuntimeLifecycle::Publish(LifecycleSnapshot observed) {
    const auto state = state_;
    {
        const std::lock_guard lock(state->mutex);
        if (state->closed || state->events.size() >= MaxEvents || state->current.sequence == std::numeric_limits<std::uint64_t>::max())
            return false;
        observed.sequence = state->current.sequence + 1;
        observed.stopping = observed.stopping || state->current.stopping;
        State::Event event{observed, {}};
        event.receivers.reserve(state->subscriptions.size());
        for (const auto& [id, receiver] : state->subscriptions) {
            (void)id;
            event.receivers.push_back(receiver);
        }
        state->events.push_back(std::move(event));
        state->current = observed;
    }
    Deliver(state);
    return true;
}

LifecycleSnapshot RuntimeLifecycle::Current() const {
    const std::lock_guard lock(state_->mutex);
    return state_->current;
}

void RuntimeLifecycle::Deliver(const std::shared_ptr<State>& state) noexcept {
    {
        const std::lock_guard lock(state->mutex);
        if (state->delivering) return;
        state->delivering = true;
    }
    for (;;) {
        State::Event event;
        {
            const std::lock_guard lock(state->mutex);
            if (state->events.empty()) { state->delivering = false; return; }
            event = std::move(state->events.front());
            state->events.pop_front();
        }
        for (const auto& receiver : event.receivers) {
            {
                const std::lock_guard lock(state->mutex);
                if (state->closed || !receiver->active) continue;
                ++receiver->running;
            }
            bool faulted{};
            try { receiver->callback(event.snapshot); }
            catch (...) { faulted = true; }
            {
                const std::lock_guard lock(state->mutex);
                --receiver->running;
                if (faulted) receiver->active = false;
                if (!receiver->active) {
                    for (auto found = state->subscriptions.begin(); found != state->subscriptions.end(); ++found) {
                        if (found->second == receiver) { state->subscriptions.erase(found); break; }
                    }
                }
            }
        }
    }
}
}
