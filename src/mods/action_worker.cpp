#include "mods/action_worker.hpp"
#include <stdexcept>

namespace sc13::mods {
ActionWorker::ActionWorker(Handler handler) : handler_(std::move(handler)) {
    if (!handler_) throw std::invalid_argument("Action worker requires a handler");
    thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}
ActionWorker::~ActionWorker() {
    { const std::scoped_lock lock(mutex_); stopping_ = true; }
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}
bool ActionWorker::Submit(ModAction action) {
    const std::scoped_lock lock(mutex_);
    if (stopping_ || busy_.load()) return false;
    status_ = "Preparing mod operation in background...";
    pending_ = std::move(action);
    busy_.store(true);
    wake_.notify_one();
    return true;
}
std::string ActionWorker::Status() const { const std::scoped_lock lock(mutex_); return status_; }
void ActionWorker::Run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this, &stop] { return stopping_ || stop.stop_requested() || pending_.has_value(); });
        if (stopping_ || stop.stop_requested()) break;
        auto action = std::move(*pending_);
        pending_.reset();
        lock.unlock();
        try {
            auto result = handler_(action);
            lock.lock();
            status_ = std::move(result);
        } catch (...) {
            if (!lock.owns_lock()) lock.lock();
            status_.clear();
        }
        busy_.store(false);
    }
}
}
