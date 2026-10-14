#include "mods/action_worker.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
/** @summary Stop on a violated observable worker contract. */
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** @summary Wait for completion with a hard deadline rather than an unbounded poll. */
void Completed(sc13::mods::ActionWorker& worker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (worker.Busy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(!worker.Busy(), "Worker did not complete within the deadline");
}
}

/** @summary Verify backpressure, exception containment and joining active work before owner teardown. */
int main() {
    try {
        std::promise<void> entered;
        std::promise<void> release;
        const auto gate = release.get_future().share();
        auto worker = std::make_unique<sc13::mods::ActionWorker>([&](const auto& action) {
            entered.set_value();
            if (gate.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                throw std::runtime_error("Fixture release timed out");
            return action.id;
        });
        Check(worker->Submit({"enable", "fixture", "revision"}), "Initial operation was rejected");
        Check(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "Handler did not start");
        Check(!worker->Submit({"disable", "fixture", {}}), "In-flight operation accepted a duplicate");
        auto destroy = std::async(std::launch::async, [&] { worker.reset(); });
        const auto beforeRelease = destroy.wait_for(std::chrono::milliseconds(50));
        release.set_value();
        Check(beforeRelease == std::future_status::timeout, "Owner teardown returned before its handler finished");
        Check(destroy.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Owner teardown did not join");
        destroy.get();
        sc13::mods::ActionWorker recovery([](const auto& action) -> std::string {
            if (action.kind == "fault") throw std::runtime_error("fixture failure");
            return "recovered";
        });
        Check(recovery.Submit({"fault", {}, {}}), "Failure fixture was rejected");
        Completed(recovery);
        Check(recovery.Submit({"next", {}, {}}), "Failed callback left worker permanently busy");
        Completed(recovery);
        Check(recovery.Status() == "recovered", "Recovery result was not published");
        std::cout << "Action worker backpressure, exception recovery and join passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
