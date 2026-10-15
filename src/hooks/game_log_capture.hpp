#pragma once

#include <string>

namespace sc13::hooks {

/** Creates safe OutputDebugString capture hooks without enabling MinHook globally. */
[[nodiscard]] bool CreateGameLogCaptureHooks(std::string& error) noexcept;

/** @summary Reset capture state while retaining disabled original trampolines until process exit. */
void ResetGameLogCapture() noexcept;

}  // namespace sc13::hooks
