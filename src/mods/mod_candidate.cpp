#include "mods/mod_candidate.hpp"
#include "mods/mod_definition_parser.hpp"
#include "mods/resource_project.hpp"
#include "scripting/source_project.hpp"
#include "../../third_party/tomlplusplus/toml.hpp"
#include <stdexcept>

namespace sc13::mods {
namespace {
/** @summary Hash verified current-manifest inputs; payload validation precedes publication. */
[[nodiscard]] bool HashDefinitionFiles(
    const std::filesystem::path& directory,
    ModDefinitionFingerprint& fingerprint,
    std::string& error) noexcept {
    try {
        if (std::filesystem::exists(directory / L"mod.toml")) {
            const auto document = toml::parse(scripting::ReadBounded(
                scripting::SourcePath(directory, "mod.toml"), 8U * 1024U * 1024U));
            if (document["contract"].value<std::string>() == "simoder.raw-resources") {
                ModDefinition imported;
                if (!LoadResourceProject(directory, imported, error)) return false;
                if (!core::Sha256File(directory / L"mod.toml", fingerprint.manifest, error)) return false;
                return true;
            }
            const auto source = scripting::InspectSourceProject(directory);
            const auto hash = scripting::HashText(source.fingerprint);
            const auto bytes = std::as_bytes(std::span(hash.data(), hash.size()));
            if (!core::Sha256(bytes, fingerprint.manifest, error)) return false;
            return true;
        }
    } catch (const std::exception& exception) { error = exception.what(); return false; }
    error = "Missing mod.toml; migrate the retired manifest with Simoder-DevTools";
    return false;
}

} // namespace

/** @summary Bound retries when a mod changes during parsing or compilation. */
[[nodiscard]] bool LoadStableCandidate(
    const std::filesystem::path& directory,
    ModDefinition& definition,
    ModDefinitionFingerprint& fingerprint,
    std::string& error) noexcept {
    for (unsigned int attempt = 0U; attempt < 2U; ++attempt) {
        ModDefinitionFingerprint before;
        if (!HashDefinitionFiles(directory, before, error)) {
            return false;
        }
        ModDefinition parsed;
        if (!LoadModDefinition(directory, parsed, error)) {
            return false;
        }
        ModDefinitionFingerprint after;
        if (!HashDefinitionFiles(directory, after, error)) {
            return false;
        }
        if (before == after) {
            definition = std::move(parsed);
            fingerprint = after;
            return true;
        }
    }
    error = "Mod files changed repeatedly while they were being read";
    return false;
}


} // namespace sc13::mods
