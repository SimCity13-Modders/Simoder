#include "hooks/runtime_hooks.hpp"
#include "hooks/game_log_capture.hpp"
#include "hooks/resource_trace.hpp"
#include "logging/async_logger.hpp"
#include "ui/simoder_overlay.hpp"
#include "dev/session/session_transport.hpp"
#include "platform/game_window_dispatch.hpp"
#include <MinHook.h>

namespace sc13::hooks {
namespace {
bool g_initialized{};
bool g_installed{};
std::atomic_bool g_retiring{};
bool g_teardownComplete{};
RuntimeHookActivity g_activity;
/** @summary Preserve dependency ownership when a MinHook shutdown transition fails.
 * @param operation Failed operation label.
 * @param status Actual MinHook status.
 * @return False without clearing any owned callback state.
 */
bool RetainFailure(const char* operation, MH_STATUS status) noexcept {
    logging::AsyncLogger::Instance().WriteFormat(logging::Level::Error,
        "%s failed: %s; runtime hook dependencies retained", operation, MH_StatusToString(status));
    return false;
}
/** @summary Roll back the same owned resources for failure and normal shutdown. */
bool Reset() noexcept {
    if (g_teardownComplete) return true;
    g_retiring = true;
    g_activity.Close();
    ui::RequestStopSimoderOverlay();
    dev::session::ShutdownSessionTransport();
    platform::ShutdownGameWindowDispatch();
    if (!dev::session::SessionTransportDrained() || !platform::GameWindowDispatchDrained() ||
        !ui::SimoderOverlayDrained() || !g_activity.Drained()) return false;
    if (g_initialized) {
        const auto disabled = MH_DisableHook(MH_ALL_HOOKS);
        if (disabled != MH_OK) return RetainFailure("MH_DisableHook", disabled);
    }
    // @summary A callback may enter while hooks were enabled after the first drain observation.
    if (!dev::session::SessionTransportDrained() || !platform::GameWindowDispatchDrained() ||
        !ui::SimoderOverlayDrained() || !g_activity.Drained()) return false;
    // @summary Disabled trampolines and original pointers remain process-owned: a suspended detour may not yet have incremented its entry counter.
    // No MH_RemoveHook/MH_Uninitialize is safe without a stronger execution-quiescence proof.
    dev::session::ShutdownScriptBridge();
    ui::ShutdownSimoderOverlay();
    ResetGameLogCapture();
    ResetResourceTrace();
    g_installed = false;
    g_teardownComplete = true;
    return true;
}
}
RuntimeHookActivity& RuntimeHookCallbacks() noexcept { return g_activity; }
bool PrepareRuntimeHookCallbacks() noexcept { return !g_retiring && g_activity.Open(); }

bool InstallRuntimeHooks(const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept {
    try {
        if (g_retiring) { error = "Runtime hooks retired; restart the process before reinstalling"; return false; }
        if (g_installed) return true;
        if (g_initialized) { error = "Previous runtime hook teardown is incomplete; dependencies retained"; return false; }
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK) {
            error = std::string("MH_Initialize failed: ") + MH_StatusToString(initialized);
            return false;
        }
        g_initialized = true;
        if (!g_activity.Open()) { error = "Previous runtime hook callbacks remain active"; static_cast<void>(Reset()); return false; }
        std::string overlayError;
        if (!ui::CreateSimoderOverlayHooks(overlayError))
            logging::AsyncLogger::Instance().WriteFormat(logging::Level::Warning,
                "Simoder UI hooks were not created: %s; runtime mods continue", overlayError.c_str());
        if (!CreateGameLogCaptureHooks(error) || !CreateResourceTraceHook(fingerprint, error)) {
            static_cast<void>(Reset());
            return false;
        }
        const auto enabled = MH_EnableHook(MH_ALL_HOOKS);
        if (enabled != MH_OK) {
            error = std::string("MH_EnableHook failed: ") + MH_StatusToString(enabled);
            static_cast<void>(Reset());
            return false;
        }
        g_installed = true;
        return true;
    } catch (...) {
        static_cast<void>(Reset());
        error = "Runtime hook setup failed due to an allocation exception";
        return false;
    }
}

bool UninstallRuntimeHooks() noexcept { return Reset(); }
}
