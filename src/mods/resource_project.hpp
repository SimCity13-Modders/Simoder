#pragma once
#include "mods/mod_types.hpp"
#include <filesystem>
#include <string>
namespace sc13::mods {
/** @summary Validate schema-three raw resources, immutable payload hashes and full-width identities. */
[[nodiscard]] bool LoadResourceProject(const std::filesystem::path& directory,
    ModDefinition& definition, std::string& error) noexcept;
/** @summary Explain the missing runtime capability without treating an imported project as active. */
[[nodiscard]] std::string ResourceRuntimeDiagnostic(const ModDefinition& definition);
}
