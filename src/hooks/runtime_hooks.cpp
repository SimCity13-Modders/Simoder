#include "hooks/runtime_hooks.hpp"
#include "hooks/game_log_capture.hpp"
#include "hooks/resource_trace.hpp"
#include "logging/async_logger.hpp"
#include "ui/simoder_overlay.hpp"
#include "dev/session/session_transport.hpp"
#include <MinHook.h>

namespace sc13::hooks {
namespace {
bool g_initialized{};
bool g_installed{};
/** @summary Roll back the same owned resources for failure and normal shutdown. */
void Reset() noexcept {
    if (!g_initialized) return;
    dev::session::ShutdownSessionTransport();
    dev::session::ShutdownScriptBridge();
    MH_DisableHook(MH_ALL_HOOKS);
    ui::ShutdownSimoderOverlay();
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    ResetGameLogCapture();
    ResetResourceTrace();
    g_initialized = false;
    g_installed = false;
}
}

bool InstallRuntimeHooks(const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept {
    try {
        if (g_installed) return true;
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK) {
            error = std::string("MH_Initialize failed: ") + MH_StatusToString(initialized);
            return false;
        }
        g_initialized = true;
        std::string overlayError;
        if (!ui::CreateSimoderOverlayHooks(overlayError))
            logging::AsyncLogger::Instance().WriteFormat(logging::Level::Warning,
                "Simoder UI hooks were not created: %s; runtime mods continue", overlayError.c_str());
        if (!CreateGameLogCaptureHooks(error) || !CreateResourceTraceHook(fingerprint, error)) {
            Reset();
            return false;
        }
        const auto enabled = MH_EnableHook(MH_ALL_HOOKS);
        if (enabled != MH_OK) {
            error = std::string("MH_EnableHook failed: ") + MH_StatusToString(enabled);
            Reset();
            return false;
        }
        g_installed = true;
        return true;
    } catch (...) {
        Reset();
        error = "Runtime hook setup failed due to an allocation exception";
        return false;
    }
}

void UninstallRuntimeHooks() noexcept { Reset(); }
}
