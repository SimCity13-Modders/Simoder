#pragma once

#include "mods/mod_types.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace sc13::mods {

/** @summary Loads persisted enabled IDs in deterministic activation order.
 * @param path Existing state file. @param enabled Receives validated ordered IDs, cleared on failure.
 * @param error Receives a diagnostic without secret or mod contents. @return True for valid or absent state. */
[[nodiscard]] bool LoadEnabledState(
    const std::filesystem::path& path,
    std::vector<ModId>& enabled,
    std::string& error) noexcept;

/** @summary Atomically persists IDs with bounded same-path Win32 5/32 retries; persistent denial preserves prior state.
 * @param path Destination state file, serialized by its owning manager. @param enabled Valid ordered IDs.
 * @param error Receives the actual final Win32 code and attempt diagnostics on publication failure.
 * @return True only after the unchanged atomic-replace operation succeeds. */
[[nodiscard]] bool SaveEnabledState(
    const std::filesystem::path& path,
    std::span<const ModId> enabled,
    std::string& error) noexcept;

}  // namespace sc13::mods
