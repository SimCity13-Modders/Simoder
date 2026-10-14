#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace sc13::mods {
/** @summary Carry one validated UI operation without coupling the worker to lifecycle state. */
struct ModAction { std::string kind; std::string id; std::string revision; };
/** @summary Serialize one bounded operation and join it before its lifecycle owner is destroyed. */
class ActionWorker final {
public:
    using Handler = std::function<std::string(const ModAction&)>;
    /** @summary Start an idle worker with its owner's operation handler. */
    explicit ActionWorker(Handler handler);
    /** @summary Stop accepting work and join any in-flight operation. */
    ~ActionWorker();
    /** @summary Submit only when no operation is queued or executing. */
    [[nodiscard]] bool Submit(ModAction action);
    /** @summary Copy progress or the last completed operation result. */
    [[nodiscard]] std::string Status() const;
    /** @summary Observe operation occupancy without taking the lifecycle lock. */
    [[nodiscard]] bool Busy() const noexcept { return busy_.load(); }
private:
    /** @summary Execute callbacks outside the queue lock while preserving submission order. */
    void Run(std::stop_token stop);
    Handler handler_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<ModAction> pending_;
    std::string status_;
    std::atomic_bool busy_{};
    bool stopping_{};
    std::jthread thread_;
};
}
