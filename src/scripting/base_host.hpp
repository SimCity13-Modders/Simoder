#pragma once
#include "scripting/script_runtime.hpp"
#include "runtime/runtime_scope.hpp"
#include <filesystem>

namespace sc13::scripting {
class SemanticBridge;
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
        std::function<void(ScriptPhase, std::uint64_t)> observer = {}, std::shared_ptr<SemanticBridge> bridge = {});
    /** @summary Retire and close on the existing script lane before releasing singleton ownership. */
    ~BaseHost();
    BaseHost(const BaseHost&) = delete;
    BaseHost& operator=(const BaseHost&) = delete;
    /** @summary Return copied execution evidence; running never means base services are ready. */
    [[nodiscard]] ScriptStatus Status() const;
    /** @summary Drain local bounded diagnostics without publishing or exporting them. */
    [[nodiscard]] std::vector<std::string> DrainLogs();
    /** @summary Creates private mod transport bindings without granting any operation permission. @param scope Original native scope. @return Native host with pin-verified Luau presentation. */
    [[nodiscard]] ScriptHost Bind(std::shared_ptr<runtime::RuntimeScope> scope) const;
    /** @summary Advances one base semantic turn on the existing interpreter lane, never from a game callback. */
    void Pump();
    /** @summary Reports native proof of the captured actual critical graph; grants no authority. @return True after this exact VM commits. */
    bool Ready() const noexcept;
private:
    std::unique_ptr<ScriptRuntime> runtime_;
    bool ownsSlot_{};
    std::shared_ptr<SemanticBridge> bridge_;
    std::map<std::string,std::string> presentationModules_;
};
}
