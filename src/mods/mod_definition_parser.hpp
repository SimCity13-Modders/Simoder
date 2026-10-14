#pragma once

#include "mods/mod_types.hpp"
#include "formats/toon/toon_document.hpp"

#include <filesystem>
#include <string>

namespace sc13::mods {

/** @summary Load an installed current TOML mod into its validated typed definition.
 * @param directory Owning mod directory.
 * @param definition Prepared definition; cleared on failure.
 * @param error Actionable validation diagnostic.
 * @return Whether the current manifest and its inputs were accepted.
 */
[[nodiscard]] bool LoadModDefinition(
    const std::filesystem::path& directory,
    ModDefinition& definition,
    std::string& error) noexcept;

/** Validates a stable reverse-domain-like mod identity. */
[[nodiscard]] bool IsValidModId(std::string_view id) noexcept;
/** @summary Load current authoring-manifest patches with named identity/provenance validation.
 * @param directory Owning project containing mod.toml.
 * @param patches Validated typed operations; empty when the manifest declares none.
 * @param error Source/field diagnostic on invalid configuration.
 * @return Whether patch validation succeeded.
 */
[[nodiscard]] bool LoadAuthoringPatches(const std::filesystem::path& directory,
    std::vector<ResourcePatchDefinition>& patches, std::string& error) noexcept;
/** @summary Validate decoded patch configuration independently from file syntax or installation layout. */
[[nodiscard]] bool ValidatePatchConfiguration(const formats::toon::Value::Object& root,
    std::vector<ResourcePatchDefinition>& patches, std::string& error) noexcept;
/** @summary Returns catalog entries for completion even when patch definitions are incomplete. */
[[nodiscard]] bool ListSymbolsJson(const std::filesystem::path& directory,
    std::string_view query, std::string& json, std::string& error) noexcept;
/** @summary Imports optional named script bindings through the existing provenance-checked TOON resolver. */
[[nodiscard]] bool LoadScriptSymbols(const std::filesystem::path& directory,
    scripting::ScriptDefinition& definition, std::string& error) noexcept;

}  // namespace sc13::mods
