#pragma once
#include "dev/session/native_script_bridge.hpp"
#include <filesystem>
#include <functional>

namespace sc13::dev::session {
/** @summary Bind the transport to one admitted runtime adapter and a current developer-service lease check. */
void ConfigureSessionTransport(std::function<ScriptReply(HWND, std::string_view)> executor, std::function<bool()> permitted);
/** @summary Associate wakeups with the actual game window; no window activation or input event is sent. */
void BindSessionWindow(HWND window) noexcept;
/** @summary Consume only the registered wakeup message; all payload stays in the DLL-owned queue. */
[[nodiscard]] bool DispatchSessionMessage(HWND window, UINT message) noexcept;
/** @summary Read complete owner/PID/deadline-bound requests and publish completed replies off the game thread. */
void PollSessionTransport(const std::filesystem::path& directory, bool servicesActive);
/** @summary Report admission and runtime thread verification separately. */
[[nodiscard]] std::string SessionTransportJson();
/** @summary Cancel queued commands and revoke the adapter before hook or manager destruction. */
void ShutdownSessionTransport() noexcept;
}
