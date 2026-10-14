#pragma once
#include "dev/service_definition.hpp"
#include "../../third_party/tomlplusplus/toml.hpp"
#include <memory>

namespace sc13::dev {
/** @summary Validate service paths, arguments and dependency order before any process starts.
 * @param document Parsed mod.toml developer-service manifest.
 * @param directory Owning mod directory used for containment checks.
 * @return Dependency-ordered service definition for the supported API version.
 * @throws std::runtime_error Invalid version, fields, paths, duplicate IDs or dependencies.
 */
std::shared_ptr<const ServiceModDefinition> ParseServiceManifest(const toml::table& document,
    const std::filesystem::path& directory);
}
