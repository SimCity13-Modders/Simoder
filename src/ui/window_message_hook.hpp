#pragma once
#include <Windows.h>
#include <string>

namespace sc13::ui {
/** @summary Consume real queued input before the game's frontend can filter dispatch.
 * @param window Verified game window whose queue is observed.
 * @param input Callback returning true only for overlay-owned input.
 * @param error Installation diagnostic on failure.
 * @return Whether the thread-local hook was installed.
 */
bool InitializeWindowMessageHook(HWND window, bool (*input)(HWND, UINT, WPARAM, LPARAM), std::string& error) noexcept;
/** @summary Identify input whose sole overlay delivery authority is the installed queue hook.
 * @return True while the hook owns delivery for this verified HWND and input message.
 */
bool IsObservedWindowInput(HWND window, UINT message) noexcept;
/** @summary Serialize synchronous lifecycle delivery with queued input and context teardown.
 * @return Whether the active overlay callback owns this message.
 */
bool RouteUnqueuedWindowInput(HWND window, UINT message, WPARAM word, LPARAM value) noexcept;
/** @summary Unhook queue observation before destroying the ImGui context. */
void ShutdownWindowMessageHook() noexcept;
}
