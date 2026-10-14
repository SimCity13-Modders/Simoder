#pragma once
#include "mods/mod_types.hpp"
#include "scripting/script_patch_command.hpp"
#include <vector>

namespace sc13::scripting {
/** @summary Reduce one callback's owner-scoped commands without publishing runtime state.
 * @param previous Previously committed scripted overrides for this owner.
 * @param commands Ordered set/clear commands from a successful callback.
 * @return Deterministically ordered candidate override set with at most 256 fields.
 * @throws std::runtime_error When the resulting persistent set exceeds its limit.
 */
[[nodiscard]] std::vector<mods::ResourcePatchDefinition> ReduceScriptPatches(
    const std::vector<mods::ResourcePatchDefinition>& previous,
    const std::vector<ScriptPatchCommand>& commands);
}
