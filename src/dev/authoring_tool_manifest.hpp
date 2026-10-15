#pragma once
#include "dev/authoring_tools.hpp"
#include "../../third_party/tomlplusplus/toml.hpp"

namespace sc13::dev {
/** @summary Validate optional authoring tool descriptors without activating or registering them.
 * @param document Parsed manifest containing an optional tools table.
 * @param directory Owning package directory used to resolve contained paths.
 * @return Validated tool paths; empty when tools are absent or the table is empty.
 * @throws std::runtime_error For unsupported capabilities or invalid descriptors and paths.
 */
[[nodiscard]] AuthoringToolPaths ParseAuthoringToolManifest(const toml::table& document,
    const std::filesystem::path& directory);
}
