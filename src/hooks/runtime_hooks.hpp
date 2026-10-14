#pragma once
#include "reverse/game_build.hpp"
#include <string>

namespace sc13::hooks {
/** @summary Install only supported game hooks after exact-build validation.
 * @param fingerprint Verified game image identity.
 * @param error Actionable diagnostic on failure.
 * @return Whether the complete required hook transaction was enabled.
 */
[[nodiscard]] bool InstallRuntimeHooks(const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept;
/** @summary Disable all owned hooks and release their state outside loader lock. */
void UninstallRuntimeHooks() noexcept;
}
