#include "dev/authoring_tools.hpp"
#include <mutex>
#include <stdexcept>

namespace sc13::dev {
namespace {
/** @summary Keep process-local registrations alive through module/static teardown. */
struct Registry {
    std::mutex mutex;
    std::map<std::string, AuthoringToolPaths, std::less<>> owners;
};
/** @summary Avoid static destructor ordering against game-owned service records. */
Registry& State() { static auto* state = new Registry; return *state; }
}

bool IsAuthoringCapability(std::string_view capability) noexcept {
    return capability == "compiler" || capability == "resource-converter" || capability == "typescript-toolchain";
}

AuthoringToolLease::~AuthoringToolLease() {
    if (!registered_) return;
    auto& state = State();
    const std::scoped_lock lock(state.mutex);
    state.owners.erase(owner_);
}

std::unique_ptr<AuthoringToolLease> RegisterAuthoringTools(std::string owner, AuthoringToolPaths paths) {
    if (owner.empty() || paths.empty()) throw std::runtime_error("Authoring registration requires an owner and tools");
    for (const auto& [capability, path] : paths) {
        if (!IsAuthoringCapability(capability))
            throw std::runtime_error("Unknown authoring capability: " + capability);
        if (!path.is_absolute()) throw std::runtime_error("Authoring tool path must be absolute");
    }
    auto lease = std::unique_ptr<AuthoringToolLease>(new AuthoringToolLease(owner));
    auto& state = State();
    const std::scoped_lock lock(state.mutex);
    if (state.owners.contains(owner)) throw std::runtime_error("Developer owner already registered tools");
    for (const auto& [existingOwner, tools] : state.owners) {
        static_cast<void>(existingOwner);
        for (const auto& [capability, path] : paths) {
            static_cast<void>(path);
            if (tools.contains(capability)) throw std::runtime_error("Authoring capability already owned: " + capability);
        }
    }
    state.owners.emplace(std::move(owner), std::move(paths));
    lease->registered_ = true;
    return lease;
}

std::filesystem::path FindAuthoringTool(std::string_view capability) {
    auto& state = State();
    const std::scoped_lock lock(state.mutex);
    for (const auto& [owner, tools] : state.owners) {
        static_cast<void>(owner);
        if (const auto found = tools.find(capability); found != tools.end()) return found->second;
    }
    return {};
}
}
