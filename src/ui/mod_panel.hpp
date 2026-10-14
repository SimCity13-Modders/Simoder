#pragma once
#include <string>

namespace sc13::mods { class ModManager; }
namespace sc13::ui {
/** @summary Keep view state independent of Win32 hooks and graphics device ownership. */
struct ModPanelState { bool open{}; std::string feedback; };
/** @summary Draw the mod controls inside an already started ImGui frame.
 * @param manager Lifecycle owner; null produces an empty disabled view.
 * @param state View-local open/feedback state owned by the overlay instance.
 */
void DrawModPanel(mods::ModManager* manager, ModPanelState& state);
}
