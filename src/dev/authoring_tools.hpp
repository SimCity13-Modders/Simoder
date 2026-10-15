#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace sc13::dev {
/** @summary Describe explicitly installed developer tools without runtime-relative path guessing. */
using AuthoringToolPaths = std::map<std::string, std::filesystem::path, std::less<>>;

/** @summary Identify capabilities supported by authoring manifests and registration.
 * @param capability Exact, case-sensitive capability name.
 * @return True for a supported authoring capability.
 */
[[nodiscard]] bool IsAuthoringCapability(std::string_view capability) noexcept;

/** @summary Own registered authoring capabilities for one enabled developer mod. */
class AuthoringToolLease final {
public:
    /** @summary Revoke only capabilities owned by this registration. */
    ~AuthoringToolLease();
    AuthoringToolLease(const AuthoringToolLease&) = delete;
    AuthoringToolLease& operator=(const AuthoringToolLease&) = delete;
private:
    explicit AuthoringToolLease(std::string owner) : owner_(std::move(owner)) {}
    std::string owner_;
    bool registered_{};
    friend std::unique_ptr<AuthoringToolLease> RegisterAuthoringTools(std::string, AuthoringToolPaths);
};

/** @summary Register an owner's complete tool set atomically; conflicts leave prior registrations unchanged.
 * @param owner Stable developer mod identity.
 * @param paths Absolute paths keyed by supported capability names.
 * @return Lifetime lease; destroying it revokes the registration.
 * @throws std::runtime_error For duplicate owners, conflicts or invalid tool descriptors.
 */
[[nodiscard]] std::unique_ptr<AuthoringToolLease> RegisterAuthoringTools(std::string owner, AuthoringToolPaths paths);
/** @summary Copy a registered tool path under the registry lock.
 * @param capability Supported authoring capability name.
 * @return Registered absolute path; empty when no enabled owner supplies it.
 */
[[nodiscard]] std::filesystem::path FindAuthoringTool(std::string_view capability);
}
