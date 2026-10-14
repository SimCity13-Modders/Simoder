#include "dev/session/native_script_bridge.hpp"
#include "dev/session/native_owner_capture.hpp"
#include "session_router.hpp"
#include "reverse/build_compatibility.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace sc13::dev::session {
namespace {
constexpr std::uintptr_t kPreferredBase = 0x400000;
std::atomic<std::uintptr_t> g_base{};

/** @summary Check every page of an observation before reading native pointers. */
bool Readable(const void* pointer, std::size_t length) noexcept {
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (!address || length > UINTPTR_MAX - address) return false;
    const auto end = address + length;
    while (address < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &information, sizeof(information)) ||
            information.State != MEM_COMMIT || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto next = reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize;
        if (next <= address) return false;
        address = next;
    }
    return true;
}

/** @summary Match immutable measured entry bytes before calling any game ABI. */
template<std::size_t N> bool Signature(std::uintptr_t base, std::uintptr_t address, const std::array<unsigned char, N>& bytes) noexcept {
    const auto* target = reinterpret_cast<const void*>(base + address - kPreferredBase);
    return Readable(target, N) && std::memcmp(target, bytes.data(), N) == 0;
}

/** @summary Resolve only a vtable entry whose implementation matches the measured game image. */
void* Method(void* object, std::size_t offset, std::uintptr_t expected) {
    if (!Readable(object, sizeof(void*))) throw std::runtime_error("Native object is unreadable.");
    auto** table = *static_cast<void***>(object);
    if (!Readable(reinterpret_cast<char*>(table) + offset, sizeof(void*))) throw std::runtime_error("Native vtable is unreadable.");
    void* target = table[offset / sizeof(void*)];
    if (expected && reinterpret_cast<std::uintptr_t>(target) != g_base.load() + expected - kPreferredBase)
        throw std::runtime_error("Native vtable differs from the verified ABI.");
    return target;
}

/** @summary Own one measured 12-byte native JS value scope, excluding root-owned cached function scopes. */
class ValueScope final {
public:
    /** @summary Allocate a local value in the selected view's native value pool. */
    ValueScope(std::uintptr_t base, void* context) : base_(base) {
        using Constructor = void(__thiscall*)(void*, void*);
        reinterpret_cast<Constructor>(base_ + 0xa385c0 - kPreferredBase)(&storage_, context);
    }
    /** @summary Release the local value through the game's matching scope destructor. */
    ~ValueScope() {
        using Destructor = void(__thiscall*)(void*);
        reinterpret_cast<Destructor>(base_ + 0xa38500 - kPreferredBase)(&storage_);
    }
    ValueScope(const ValueScope&) = delete;
    ValueScope& operator=(const ValueScope&) = delete;
    /** @summary Return the scope wrapper expected by script invocation and text assignment. */
    void* Scope() noexcept { return &storage_; }
    /** @summary Access the underlying value only for measured type/string observations. */
    void* Value() noexcept { return storage_.value; }
private:
    /** @summary Preserve the measured standard-layout scope and initialized padding. */
    struct Storage final { void* value{}; void* pool{}; unsigned char owned{}; unsigned char padding[3]{}; } storage_;
    static_assert(sizeof(Storage) == 12);
    const std::uintptr_t base_;
};
static_assert(sizeof(void*) == 4, "The native script adapter requires the production x86 ABI.");

/** @summary Copy a bounded UTF-16 result while its local native value scope remains alive. */
std::string CopyResult(void* value) {
    using Type = int(__thiscall*)(void*);
    using String = const wchar_t*(__thiscall*)(void*, std::uint32_t*);
    if (reinterpret_cast<Type>(Method(value, 4, 0))(value) != 4) throw std::runtime_error("The session router did not return JSON text.");
    std::uint32_t length{};
    const auto* text = reinterpret_cast<String>(Method(value, 0x38, 0))(value, &length);
    if (!length || length > 262144 || !Readable(text, static_cast<std::size_t>(length) * sizeof(wchar_t)))
        throw std::runtime_error("Native JSON text is missing or exceeds the observation bound.");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (count <= 0 || count > 1024 * 1024) throw std::runtime_error("Native JSON text has invalid UTF-16 encoding.");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), result.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("Native JSON text conversion failed.");
    return result;
}
}

bool InitializeScriptBridge(const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept {
    g_base.store(0);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base != kPreferredBase || fingerprint.machine != 0x14c || core::ToHex(fingerprint.fileSha256) != reverse::kSupportedGameFileSha256) {
        error = "Native session ABI requires the verified x86 game executable and load base.";
        return false;
    }
    if (!Signature(base, 0xa385c0, std::array<unsigned char, 8>{0x8b,0x44,0x24,0x04,0x55,0x56,0x8b,0xf1}) ||
        !Signature(base, 0xa38500, std::array<unsigned char, 8>{0x80,0x79,0x08,0x00,0x74,0x0f,0x8b,0x01}) ||
        !Signature(base, 0x666430, std::array<unsigned char, 8>{0x6a,0xff,0x68,0x88,0x9b,0xc4,0x00,0x64}) ||
        !Signature(base, 0xa36220, std::array<unsigned char, 8>{0x56,0x8b,0xf1,0x83,0xbe,0xb0,0x00,0x00}) ||
        !Signature(base, 0xa350a0, std::array<unsigned char, 8>{0x53,0x8b,0xd9,0x8b,0x4c,0x24,0x08,0x85})) {
        error = "Native session entry signatures differ from the REA evidence.";
        return false;
    }
    g_base.store(base);
    std::string captureError;
    static_cast<void>(InitializeOwnerCapture(captureError));
    return true;
}

ScriptReply ExecuteScriptPacket(HWND window, std::string_view packetJson) noexcept {
    ScriptReply reply;
    reply.threadId = GetCurrentThreadId();
    try {
        const auto base = g_base.load();
        if (!base || packetJson.empty() || packetJson.size() > 16384 || packetJson.find('\0') != std::string_view::npos)
            throw std::runtime_error("Native session admission or packet validation failed.");
        const auto* threadSlot = reinterpret_cast<const HANDLE*>(base + 0xe596f8 - kPreferredBase);
        const DWORD nativeThread = Readable(threadSlot, sizeof(HANDLE)) && *threadSlot ? GetThreadId(*threadSlot) : 0;
        const DWORD windowThread = GetWindowThreadProcessId(window, nullptr);
        if (!nativeThread || nativeThread != reply.threadId || windowThread != reply.threadId)
            throw std::runtime_error("Window dispatch is not the verified native game thread: current=" + std::to_string(reply.threadId) +
                ", window=" + std::to_string(windowThread) + ", native=" + std::to_string(nativeThread));
        const auto* rootSlot = reinterpret_cast<void* const*>(base + 0xfe13c4 - kPreferredBase);
        if (!Readable(rootSlot, sizeof(void*)) || !*rootSlot) throw std::runtime_error("Native UI root is not ready.");
        void* root = *rootSlot;
        using Count = int(__thiscall*)(void*);
        using Context = void*(__thiscall*)(void*, int);
        using Compile = bool(__thiscall*)(void*, const char*, void*);
        using Invoke = bool(__thiscall*)(void*, void*, void*, void*);
        using Text = void(__cdecl*)(const char*, void*);
        const int count = reinterpret_cast<Count>(Method(root, 0x54, 0x935f10))(root);
        if (count <= 0 || count > 16) throw std::runtime_error("Native view inventory is not ready or exceeds its bound.");
        const auto contextAt = reinterpret_cast<Context>(Method(root, 0x4c, 0xa3bb80));
        /* @summary Keep callback owner capture scoped to this admitted, game-thread native invocation. */
        struct CaptureScope final {
            CaptureScope() noexcept { BeginOwnerCapture(); }
            ~CaptureScope() { EndOwnerCapture(); }
        } captureScope;
        const std::string packet(packetJson);
        void* selectedContext{};
        int selectedIndex = -1;
        int selectedRank = -1;
        std::string rejectedViews;
        for (int index = 0; index < count; ++index) {
            void* context = contextAt(root, index);
            if (!Readable(context, 0x15c)) continue;
            if (!Readable(static_cast<char*>(root) + 0x40, sizeof(void*))) continue;
            auto** frames = *reinterpret_cast<void***>(static_cast<char*>(root) + 0x40);
            if (!Readable(frames + index, sizeof(void*)) || !Readable(static_cast<char*>(frames[index]) + 8, 1) ||
                *reinterpret_cast<const unsigned char*>(static_cast<char*>(frames[index]) + 8)) continue;
            auto invoke = reinterpret_cast<Invoke>(Method(context, 0x50, 0xa350a0));
            auto compile = reinterpret_cast<Compile>(Method(context, 0x58, 0xa36220));
            ValueScope function(base, context);
            if (!compile(context, kSessionRouterSource.data(), function.Scope())) {
                rejectedViews += " view=" + std::to_string(index) + " compile=false;";
                continue;
            }
            ValueScope argument(base, context);
            ValueScope result(base, context);
            reinterpret_cast<Text>(base + 0x666430 - kPreferredBase)("{\"op\":\"observe\"}", argument.Scope());
            if (!invoke(context, function.Scope(), result.Scope(), argument.Scope())) {
                rejectedViews += " view=" + std::to_string(index) + " invoke=false;";
                continue;
            }
            const auto observation = CopyResult(result.Value());
            if (observation.find("\"ok\":true") == std::string::npos) {
                rejectedViews += " view=" + std::to_string(index) + " reply=" + observation.substr(0, 1024) + ";";
                continue;
            }
            const int rank = observation.find("\"phase\":\"city\"") != std::string::npos ? 4 :
                observation.find("\"phase\":\"main_menu\"") != std::string::npos ? 3 :
                observation.find("\"phase\":\"intro\"") != std::string::npos ? 2 :
                observation.find("\"phase\":\"loading\"") != std::string::npos ? 1 : 0;
            if (rank > selectedRank) { selectedRank = rank; selectedIndex = index; selectedContext = context; }
        }
        if (!selectedContext) {
            reply.retryable = rejectedViews.empty();
            throw std::runtime_error("No native view accepted the fixed session router: count=" + std::to_string(count) + rejectedViews);
        }
        /* @summary Own the compiled function locally, avoiding collisions with the game's shared function-key cache. */
        ValueScope function(base, selectedContext);
        const auto compile = reinterpret_cast<Compile>(Method(selectedContext, 0x58, 0xa36220));
        if (!compile(selectedContext, kSessionRouterSource.data(), function.Scope()))
            throw std::runtime_error("The selected native view could not compile the fixed session router.");
        ValueScope argument(base, selectedContext);
        ValueScope result(base, selectedContext);
        reinterpret_cast<Text>(base + 0x666430 - kPreferredBase)(packet.c_str(), argument.Scope());
        const auto invoke = reinterpret_cast<Invoke>(Method(selectedContext, 0x50, 0xa350a0));
        if (!invoke(selectedContext, function.Scope(), result.Scope(), argument.Scope()))
            throw std::runtime_error("Native script invocation failed; mutation completion is unknown.");
        reply.json = CopyResult(result.Value());
        if (packetJson.find("\"op\":\"world-units\"") != std::string_view::npos &&
            reply.json.size() >= 2 && reply.json.substr(reply.json.size() - 2) == "}}" &&
            reply.json.find("\"ok\":true") != std::string::npos)
            reply.json.insert(reply.json.size() - 2, ",\"nativeBindings\":" + OwnerObservationJson());
        reply.viewIndex = static_cast<unsigned int>(selectedIndex);
        reply.success = true;
    } catch (const std::exception& exception) { reply.error = exception.what(); }
    catch (...) { reply.error = "Native session observation failed."; }
    return reply;
}

void ShutdownScriptBridge() noexcept { g_base.store(0); ShutdownOwnerCapture(); }
}
