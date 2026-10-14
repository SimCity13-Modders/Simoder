#pragma once
#include <filesystem>

namespace sc13::mods { class ModManager; }
namespace sc13::dev {
/** @summary Consume one bounded, expiring, game-instance-bound command for an enabled developer service.
 * @param manager Runtime lifecycle owner that commits the requested mod transition.
 * @param directory Fixed runtime diagnostic directory containing the request/result files.
 */
void ProcessRuntimeControl(mods::ModManager& manager, const std::filesystem::path& directory);
}
