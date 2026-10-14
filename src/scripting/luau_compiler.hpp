#pragma once
#include "scripting/source_project.hpp"

namespace sc13::scripting {
/** @summary Prepare and atomically publish bytecode using the statically linked Luau compiler.
 * @param source Validated immutable source description.
 * @param generation Destination outside the authoring project.
 * @throws std::runtime_error Invalid source, changed inputs or failed publication.
 */
void CompileLuauGeneration(const SourceProject& source, const std::filesystem::path& generation);
}
