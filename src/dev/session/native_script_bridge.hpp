#pragma once
#include "reverse/game_build.hpp"
#include <Windows.h>
#include <string>
#include <string_view>

namespace sc13::dev::session {
/** @summary Return bounded native observations without conflating dispatch with gameplay completion. */
struct ScriptReply final {
    bool success{};
    bool retryable{};
    std::string json;
    std::string error;
    DWORD threadId{};
    unsigned int viewIndex{};
};
/** @summary Admit the exact measured x86 game ABI before exposing the trusted session router.
 * @param fingerprint Measured executable identity before runtime hook installation.
 * @param error Explicit incompatibility diagnosis.
 * @return True when all required native entry signatures match.
 */
[[nodiscard]] bool InitializeScriptBridge(const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept;
/** @summary Execute the fixed router on the verified owning window/game thread.
 * @param window Current game-owned window handling the private transport wakeup.
 * @param packetJson Serialized typed operation; never executable source.
 * @return Correlated payload or an explicit ABI, thread, view or native-call failure.
 */
[[nodiscard]] ScriptReply ExecuteScriptPacket(HWND window, std::string_view packetJson) noexcept;
/** @summary Clear native script admission after the owning runtime has drained callbacks. */
void ShutdownScriptBridge() noexcept;
}
