#pragma once

#include <string>

namespace sc13::mods {
class ModManager;
}

namespace sc13::ui {

/** Binds the process-lifetime UI view model before graphics hooks are created. */
void ConfigureSimoderOverlay(mods::ModManager& manager) noexcept;

/** Creates Direct3D 9 overlay hooks without enabling MinHook globally. */
[[nodiscard]] bool CreateSimoderOverlayHooks(std::string& error) noexcept;

/** @summary Close frame/input admission without waiting for callbacks. */
void RequestStopSimoderOverlay() noexcept;
/** @summary Observe frame, reset and input callback retention.
 * @return Whether no callback still owns the UI context or original graphics procedure.
 */
[[nodiscard]] bool SimoderOverlayDrained() noexcept;
/** @summary Shut down ImGui only after hooks are disabled and all callbacks drain. */
void ShutdownSimoderOverlay() noexcept;

}  // namespace sc13::ui
