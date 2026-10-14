#include "dev/session/native_owner_capture.hpp"
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
QueryCallback g_original{};
bool g_installed{};
thread_local bool g_observing{};
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
    if (g_observing && Readable(self, 0x10)) {
        auto* owner = *reinterpret_cast<void**>(static_cast<char*>(self) + 0xc);
        if (Readable(owner, sizeof(void*))) g_owner = owner;
    }
    g_original(self, a, b, c, d);
}
/** @summary Render observational addresses without interpreting them as semantic object identities. */
std::string Hex(std::uintptr_t value) {
    std::ostringstream stream; stream << "0x" << std::hex << value; return stream.str();
}
}
bool InitializeOwnerCapture(std::string& error) noexcept {
    if (g_installed) return true;
    const std::array<unsigned char, 8> signature{0x83,0x7c,0x24,0x0c,0x00,0x53,0x8b,0xd9};
    auto* target = reinterpret_cast<void*>(kCallback);
    if (!Readable(target, signature.size()) || std::memcmp(target, signature.data(), signature.size())) {
        error = "Native query callback differs from the measured four-argument ABI."; return false;
    }
    const auto created = MH_CreateHook(target, reinterpret_cast<void*>(&ObserveQuery), reinterpret_cast<void**>(&g_original));
    if (created != MH_OK) { error = MH_StatusToString(created); return false; }
    const auto enabled = MH_EnableHook(target);
    if (enabled != MH_OK) { MH_RemoveHook(target); g_original = nullptr; error = MH_StatusToString(enabled); return false; }
    g_installed = true; return true;
}
void BeginOwnerCapture() noexcept { g_owner = nullptr; g_observing = g_installed; }
void EndOwnerCapture() noexcept { g_observing = false; g_owner = nullptr; }
std::string OwnerObservationJson() {
    if (!g_observing || !Readable(g_owner, sizeof(void*))) return "{\"available\":false}";
    auto** table = *static_cast<void***>(g_owner);
    if (!Readable(table, 0x100)) return "{\"available\":false}";
    std::string result = "{\"available\":true,\"processId\":" + std::to_string(GetCurrentProcessId()) +
        ",\"threadId\":" + std::to_string(GetCurrentThreadId()) + ",\"owner\":" + core::JsonString(Hex(reinterpret_cast<std::uintptr_t>(g_owner))) +
        ",\"vtable\":" + core::JsonString(Hex(reinterpret_cast<std::uintptr_t>(table))) + ",\"methods\":[";
    for (std::size_t index = 0; index < 64; ++index) {
        if (index) result += ',';
        result += core::JsonString(Hex(reinterpret_cast<std::uintptr_t>(table[index])));
    }
    return result + "]}";
}
void ShutdownOwnerCapture() noexcept {
    g_observing = false; g_owner = nullptr;
    if (g_installed) { MH_DisableHook(reinterpret_cast<void*>(kCallback)); MH_RemoveHook(reinterpret_cast<void*>(kCallback)); }
    g_installed = false; g_original = nullptr;
}
}
