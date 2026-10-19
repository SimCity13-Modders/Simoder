#include "dev/session/native_owner_capture.hpp"
#include "dev/session/owner_capture_lifecycle.hpp"
#include "core/json.hpp"
#include <Windows.h>
#include <MinHook.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>

namespace sc13::dev::session {
namespace {
constexpr std::uintptr_t kCallback = 0x8a28b0;
using QueryCallback = void(__thiscall*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
OwnerCaptureLifecycle g_lifecycle(hooks::RuntimeHookCallbacks());
thread_local OwnerCaptureObservation g_observation;
thread_local void* g_owner{};

/** @summary Read only committed, accessible spans before following native interface pointers. */
bool Readable(const void* pointer, std::size_t count) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    return begin && VirtualQuery(pointer, &memory, sizeof(memory)) && memory.State == MEM_COMMIT &&
        !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && count <= UINTPTR_MAX - begin &&
        begin + count <= reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
}
/** @summary Preserve all four native stack arguments and record only the borrowed owner during an admitted query. */
void __fastcall ObserveQuery(void* self, void*, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) {
    const hooks::RuntimeHookActivity::Entry activity(g_lifecycle.Activity());
    if (activity.admitted() && g_observation.Allowed(g_lifecycle) && Readable(self, 0x10)) {
        auto* owner = *reinterpret_cast<void**>(static_cast<char*>(self) + 0xc);
        if (Readable(owner, sizeof(void*))) g_owner = owner;
    }
    // @summary Original is published before Enable and retained forever, including failed-enable/late-entry paths.
    const auto original = reinterpret_cast<QueryCallback>(g_lifecycle.Original());
    original(self, a, b, c, d);
}
/** @summary Preserve actual MinHook status and distinguish already-disabled from unowned or failed transitions. */
OwnerHookResult HookResult(MH_STATUS status) noexcept {
    const auto kind = status == MH_OK ? OwnerHookResult::Kind::Success :
        status == MH_ERROR_DISABLED ? OwnerHookResult::Kind::AlreadyDisabled :
        status == MH_ERROR_ALREADY_CREATED ? OwnerHookResult::Kind::Foreign : OwnerHookResult::Kind::Failure;
    return {kind, static_cast<int>(status), MH_StatusToString(status)};
}
/** @summary Create only this exact callback and return its unpublished original for one-time publication. */
OwnerHookResult CreateOwnedHook(void*, std::uintptr_t& original) noexcept {
    void* procedure{};
    const auto status = MH_CreateHook(reinterpret_cast<void*>(kCallback), reinterpret_cast<void*>(&ObserveQuery), &procedure);
    original = reinterpret_cast<std::uintptr_t>(procedure);
    return HookResult(status);
}
/** @summary Enable only the exact hook whose original was already published. */
OwnerHookResult EnableOwnedHook(void*) noexcept { return HookResult(MH_EnableHook(reinterpret_cast<void*>(kCallback))); }
/** @summary Disable without removing the entry or releasing its original trampoline. */
OwnerHookResult DisableOwnedHook(void*) noexcept { return HookResult(MH_DisableHook(reinterpret_cast<void*>(kCallback))); }
/** @summary Render actual transition failures outside callbacks, retaining both failed enable and cleanup outcomes. */
void DescribeFailure(const OwnerCaptureLifecycle::Outcome& outcome, std::string& error) noexcept {
    try {
        error = outcome.result.diagnostic;
        if (outcome.cleanupAttempted) {
            error += "; disable cleanup: ";
            error += outcome.cleanup.diagnostic;
        }
        if (!outcome.stopped && outcome.state == OwnerCaptureLifecycle::State::RetiredRetained)
            error += "; original callbacks remain active; dependencies retained";
    } catch (...) {
        // @summary Reporting allocation failure cannot change the retained lifecycle outcome or escape a noexcept native boundary.
        error.clear();
    }
}
/** @summary Render observational addresses without interpreting them as semantic object identities. */
std::string Hex(std::uintptr_t value) {
    std::ostringstream stream; stream << "0x" << std::hex << value; return stream.str();
}
}
bool InitializeOwnerCapture(std::string& error) noexcept {
    const OwnerHookOperations operations{nullptr, &CreateOwnedHook, &EnableOwnedHook, &DisableOwnedHook};
    if (g_lifecycle.CurrentState() != OwnerCaptureLifecycle::State::Cold) {
        const auto previous = g_lifecycle.Initialize(kCallback, operations);
        if (!previous.ready) DescribeFailure(previous, error);
        return previous.ready;
    }
    const std::array<unsigned char, 8> signature{0x83,0x7c,0x24,0x0c,0x00,0x53,0x8b,0xd9};
    auto* target = reinterpret_cast<void*>(kCallback);
    if (!Readable(target, signature.size()) || std::memcmp(target, signature.data(), signature.size())) {
        static_cast<void>(g_lifecycle.Initialize(0, operations));
        error = "Native query callback differs from the measured four-argument ABI."; return false;
    }
    const auto outcome = g_lifecycle.Initialize(kCallback, operations);
    if (!outcome.ready) DescribeFailure(outcome, error);
    return outcome.ready;
}
void BeginOwnerCapture() noexcept { g_owner = nullptr; static_cast<void>(g_observation.Begin(g_lifecycle)); }
void EndOwnerCapture() noexcept { g_observation.End(); g_owner = nullptr; }
void CloseOwnerCaptureObservation() noexcept { g_lifecycle.CloseObservation(); }
std::string OwnerObservationJson() {
    if (!g_observation.Allowed(g_lifecycle) || !Readable(g_owner, sizeof(void*))) return "{\"available\":false}";
    auto** table = *static_cast<void***>(g_owner);
    if (!Readable(table, 0x100)) return "{\"available\":false}";
    std::string result = "{\"available\":true,\"processId\":" + std::to_string(GetCurrentProcessId()) +
        ",\"threadId\":" + std::to_string(GetCurrentThreadId()) + ",\"owner\":" + core::JsonString(Hex(reinterpret_cast<std::uintptr_t>(g_owner))) +
        ",\"vtable\":" + core::JsonString(Hex(reinterpret_cast<std::uintptr_t>(table))) + ",\"methods\":[";
    for (std::size_t index = 0; index < 64; ++index) {
        if (index) result += ',';
        result += core::JsonString(Hex(reinterpret_cast<std::uintptr_t>(table[index])));
    }
    return g_observation.Allowed(g_lifecycle) ? result + "]}" : "{\"available\":false}";
}
bool ShutdownOwnerCapture(std::string& error) noexcept {
    g_observation.End(); g_owner = nullptr;
    const auto outcome = g_lifecycle.Retire();
    if (!outcome.stopped) DescribeFailure(outcome, error);
    return outcome.stopped;
}
}
