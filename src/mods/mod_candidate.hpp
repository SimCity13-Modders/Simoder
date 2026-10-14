#pragma once
#include "core/sha256.hpp"
#include "mods/mod_types.hpp"

namespace sc13::mods {
/** @summary Identify the complete validated input revision independently of retired sidecar files. */
struct ModDefinitionFingerprint final {
    core::Sha256Digest manifest{};
    auto operator<=>(const ModDefinitionFingerprint&) const = default;
};
/** @summary Read a complete definition only when its verified inputs remain stable.
 * @param directory Owning installed mod directory.
 * @param definition Stable parsed definition.
 * @param fingerprint Identity used for changed-on-disk and reload checks.
 * @param error Diagnostic when validation or bounded stability retries fail.
 * @return Whether a stable candidate was produced.
 */
[[nodiscard]] bool LoadStableCandidate(const std::filesystem::path& directory,
    ModDefinition& definition, ModDefinitionFingerprint& fingerprint, std::string& error) noexcept;
}
