#pragma once
#include "dev/service_definition.hpp"
#include <memory>

namespace sc13::dev {
/** @summary Own one mod's process tree; closing the Windows job ends all descendants. */
class ServiceSupervisor final {
public:
    /** @summary Construct an inactive supervisor without creating processes. */
    ServiceSupervisor();
    /** @summary Stop all owned processes before releasing supervisor state. */
    ~ServiceSupervisor();
    ServiceSupervisor(const ServiceSupervisor&) = delete;
    ServiceSupervisor& operator=(const ServiceSupervisor&) = delete;
    /** @summary Start a validated dependency-ordered service set transactionally.
     * @param definition Validated service contract with local executable paths.
     * @param gameRoot Game installation passed to all game-owned services.
     * @param error Diagnostic on failure; all started children are stopped on rollback.
     * @return Whether every service was started and remains alive at startup completion.
     */
    bool Start(const ServiceModDefinition& definition, const std::filesystem::path& gameRoot, std::string& error) noexcept;
    /** @summary Stop all children and reset to the inactive state; safe to call repeatedly. */
    void Stop() noexcept;
    /** @summary Report an exited child without restarting it or changing mod state. */
    [[nodiscard]] std::string Diagnostic() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
