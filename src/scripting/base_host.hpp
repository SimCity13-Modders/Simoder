#pragma once
#include "scripting/script_runtime.hpp"
#include <filesystem>

namespace sc13::scripting {
/** @summary Host-owned reviewed package identity; never load this pin from the package itself. */
struct BasePackagePin final {
    std::string version;
    /** @summary Exact portable path to uppercase SHA-256, including mod.toml and every executable source. */
    std::map<std::string, std::string> files;
};
/** @summary Own one private verified bootstrap VM; exposes neither readiness nor semantic providers. */
class BaseHost final {
public:
    /** @summary Capture, verify and compile immutable package bytes before entering the shared lane.
     * @param directory Installed source package root; no cache is consulted.
     * @param pin Trusted native caller's exact reviewed version and source hashes, copied on entry.
     * @param observer Optional nonreentrant native lifecycle evidence callback.
     * @throws std::runtime_error Any provenance, resource, singleton or VM failure.
     */
    BaseHost(const std::filesystem::path& directory, BasePackagePin pin,
        std::function<void(ScriptPhase, std::uint64_t)> observer = {});
    /** @summary Retire and close on the existing script lane before releasing singleton ownership. */
    ~BaseHost();
    BaseHost(const BaseHost&) = delete;
    BaseHost& operator=(const BaseHost&) = delete;
    /** @summary Return copied execution evidence; running never means base services are ready. */
    [[nodiscard]] ScriptStatus Status() const;
    /** @summary Drain local bounded diagnostics without publishing or exporting them. */
    [[nodiscard]] std::vector<std::string> DrainLogs();
private:
    std::unique_ptr<ScriptRuntime> runtime_;
    bool ownsSlot_{};
};
}
