#pragma once
#include <string>

namespace sc13::dev::session {
/** @summary Observe the query callback's native owner without altering any game query or its result.
 * @param error Failure reason for the optional diagnostic adapter.
 * @return True when the exact supported callback hook was installed.
 */
bool InitializeOwnerCapture(std::string& error) noexcept;
/** @summary Limit native owner observation to the current verified script execution thread. */
void BeginOwnerCapture() noexcept;
/** @summary Revoke observation after the native invocation has returned. */
void EndOwnerCapture() noexcept;
/** @summary Serialize only the observed native owner and bounded module-relative vtable targets.
 * @return JSON describing current callback ownership, never an executable command.
 */
std::string OwnerObservationJson();
/** @summary Revoke optional scopes globally before runtime drain/disable, including other threads' stale TLS. */
void CloseOwnerCaptureObservation() noexcept;
/** @summary Disable the owned callback while retaining its original, MinHook entry and DLL through process exit.
 * @param error Actual disable/ownership failure; caller must retain dependencies on false.
 * @return True only after owned disable/no-hook acknowledgement; never a trampoline-unload certificate. */
[[nodiscard]] bool ShutdownOwnerCapture(std::string& error) noexcept;
}
