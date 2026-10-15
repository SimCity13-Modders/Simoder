#pragma once
#include <Windows.h>
#include <cstdint>

namespace sc13::platform {
/** @summary Describe one validated window attachment; handle reuse cannot reuse its generation. */
struct GameWindowIdentity final { HWND window{}; DWORD process{}; DWORD thread{}; std::uint64_t generation{}; };
using WindowMessageConsumer = bool (*)(HWND, UINT, WPARAM, LPARAM) noexcept;
using WindowStopObserver = void (*)() noexcept;
using WindowResultObserver = void (*)(UINT, WPARAM, LPARAM, LRESULT, bool) noexcept;
/** @summary Configure core message delivery outside loader lock.
 * @param consumer Optional native message consumer, independent of rendering.
 * @param stop Optional nonblocking observer called before close/destruction forwarding.
 */
void ConfigureGameWindowDispatch(WindowMessageConsumer consumer, WindowStopObserver stop) noexcept;
/** @summary Attach only a valid current-process window and invalidate any prior attachment; registration may originate on its render thread.
 * @param window Actual device window, or null to detach.
 * @return True only for a live owned attachment, or successful null detachment.
 */
[[nodiscard]] bool BindGameWindow(HWND window) noexcept;
/** @summary Read the currently attached identity.
 * @return Empty identity when no validated window remains.
 */
[[nodiscard]] GameWindowIdentity CurrentGameWindow() noexcept;
/** @summary Check a complete identity against the live attachment.
 * @param identity Previously captured attachment.
 * @return Whether window, process, thread and generation still match.
 */
[[nodiscard]] bool IsCurrentGameWindow(const GameWindowIdentity& identity) noexcept;
/** @summary Register the optional rendering/input subscriber.
 * @param consumer Null disables overlay delivery without affecting native dispatch.
 */
void SetGameWindowInputConsumer(WindowMessageConsumer consumer) noexcept;
/** @summary Observe completed forwarding without owning the window chain.
 * @param observer Optional diagnostic observer receiving the actual result and capture flag.
 */
void SetGameWindowResultObserver(WindowResultObserver observer) noexcept;
/** @summary Close delivery and detach without waiting inside a window callback. */
void ShutdownGameWindowDispatch() noexcept;
/** @summary Observe callback retention after nonblocking shutdown.
 * @return Whether all callbacks copied before shutdown have returned.
 */
[[nodiscard]] bool GameWindowDispatchDrained() noexcept;
}
