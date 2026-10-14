#include <Windows.h>
#include <d3d9.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include "bootstrap/loader.hpp"
#include "bootstrap/install_context.hpp"

extern "C" {
/** @summary Cache original ordinal targets for transparent x86 assembly tail calls. */
FARPROC g_d3dExports[23]{};
}

namespace {
HMODULE g_proxy{};
HMODULE g_original{};
INIT_ONCE g_originalOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_bootstrapRequested{};
void* g_firstCaller{};
DWORD g_firstCallerThread{};

/** @summary Retrieve a module's absolute path without depending on the working directory. */
std::filesystem::path ModulePath(HMODULE module) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD size = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    return size != 0 && size < buffer.size() ? std::filesystem::path(buffer.data(), buffer.data() + size) : std::filesystem::path{};
}

/** @summary Append bootstrap diagnostics without creating a console or touching game input. */
void WriteLog(const std::filesystem::path& runtimeDirectory, const std::wstring& message) noexcept {
    try {
        const std::wstring line = L"[D3D9-PROXY] pid=" + std::to_wstring(GetCurrentProcessId()) +
            L" tick=" + std::to_wstring(GetTickCount64()) + L" " + message + L"\r\n";
        OutputDebugStringW(line.c_str());
        std::error_code error;
        auto directory = runtimeDirectory / L"logs";
        std::filesystem::create_directories(directory, error);
        HANDLE file = CreateFileW((directory / L"proxy-bootstrap.log").c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            std::array<wchar_t, 32768> local{};
            const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
            if (count == 0 || count >= local.size()) { return; }
            directory = std::filesystem::path(local.data()) / L"Simoder" / L"logs";
            std::filesystem::create_directories(directory, error);
            file = CreateFileW((directory / L"proxy-bootstrap.log").c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) { return; }
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            std::string utf8(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), size, nullptr, nullptr);
            DWORD written{};
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        CloseHandle(file);
    } catch (...) { /* @summary Logging failure must not affect Direct3D forwarding. */ }
}

/** @summary Load the genuine system DLL with x86/WOW64 resolution and cache its original ordinals. */
BOOL CALLBACK LoadOriginal(PINIT_ONCE, PVOID, PVOID*) noexcept {
    std::array<wchar_t, 32768> system{};
    const UINT count = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (count == 0 || count + 11 >= system.size()) { return FALSE; }
    // @summary Construct a trusted path without consulting PATH, the application folder, or CWD.
    constexpr wchar_t suffix[] = L"\\d3d9.dll";
    std::memcpy(system.data() + count, suffix, sizeof(suffix));
    g_original = LoadLibraryExW(system.data(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_original == nullptr || g_original == g_proxy) { return FALSE; }
    for (DWORD index = 0; index < 23; ++index) {
        g_d3dExports[index] = GetProcAddress(g_original, MAKEINTRESOURCEA(index + 16));
    }
    return TRUE;
}

/** @summary Bootstrap off the graphics call stack; a new thread cannot execute inside another thread's DllMain. */
DWORD WINAPI Bootstrap(void*) noexcept {
    std::filesystem::path directory;
    try {
        const auto proxy = ModulePath(g_proxy);
        const auto executable = ModulePath(nullptr);
        directory = proxy.parent_path().parent_path() / L"simoder";
        WriteLog(directory, L"bootstrap=proxy original=" + ModulePath(g_original).wstring() +
            L" executable=" + executable.wstring());
        HMODULE callerModule{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(g_firstCaller), &callerModule)) {
            const auto callerRva = reinterpret_cast<std::uintptr_t>(g_firstCaller) - reinterpret_cast<std::uintptr_t>(callerModule);
            WriteLog(directory, L"firstFactoryCaller=" + ModulePath(callerModule).wstring() +
                L" rva=" + std::to_wstring(callerRva) + L" thread=" + std::to_wstring(g_firstCallerThread));
        }
        if (_wcsicmp(executable.filename().c_str(), L"SimCity.exe") != 0) {
            WriteLog(directory, L"disabled: non-SimCity process; graphics forwarding remains active");
            return 0;
        }
        if (GetFileAttributesW((directory / L"proxy.disabled").c_str()) != INVALID_FILE_ATTRIBUTES) {
            WriteLog(directory, L"disabled: proxy.disabled; graphics forwarding remains active");
            return 0;
        }
        const auto root = proxy.parent_path().parent_path().wstring();
        const auto data = directory.wstring();
        sc13::bootstrap::InstallContext context;
        context.gameRoot = root.c_str();
        context.dataRoot = data.c_str();
        const DWORD result = sc13::bootstrap::Initialize(context);
        WriteLog(directory, L"runtime.initialize=" + std::to_wstring(result) + L" module=" + proxy.wstring());
        if (result != 0) {
            // @summary Rejected initialization owns partial services; explicitly stop them before baseline play.
            sc13::bootstrap::Shutdown();
            WriteLog(directory, L"[WARN] runtime rejected initialization; services stopped; game continues without mods");
        }
        // @summary Hold loaded modules for process lifetime; no DLL shutdown or thread joins run under loader lock.
        return 0;
    } catch (...) {
        WriteLog(directory, L"[WARN] bootstrap exception; graphics forwarding remains active");
        return 0;
    }
}

/** @summary Request exactly one background initialization and preserve the graphics API's error state. */
void RequestBootstrap(void* caller) noexcept {
    const DWORD previousError = GetLastError();
    if (!g_bootstrapRequested.exchange(true, std::memory_order_acq_rel)) {
        g_firstCaller = caller;
        g_firstCallerThread = GetCurrentThreadId();
        HMODULE pinned{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&g_proxy), &pinned)) {
            const HANDLE thread = CreateThread(nullptr, 0, &Bootstrap, nullptr, 0, nullptr);
            if (thread != nullptr) { CloseHandle(thread); }
            else { OutputDebugStringW(L"[D3D9-PROXY] bootstrap worker unavailable; graphics forwarding remains active\n"); }
        }
    }
    SetLastError(previousError);
}
}  // namespace

/** @summary Resolve a generic export without changing registers, stack layout, or incoming last-error state. */
extern "C" void WINAPI ProxyResolveExport(DWORD index) noexcept {
    const DWORD previousError = GetLastError();
    if (!InitOnceExecuteOnce(&g_originalOnce, &LoadOriginal, nullptr, nullptr) ||
            index >= 23 || g_d3dExports[index] == nullptr) {
        // @summary An unavailable genuine graphics export cannot safely be replaced with an invented return ABI.
        RaiseException(0xC0000139UL, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        RaiseFailFastException(nullptr, nullptr, 0);
    }
    SetLastError(previousError);
}

/** @summary Forward object creation before scheduling Simoder, allowing recursive overlay discovery to forward normally. */
extern "C" IDirect3D9* WINAPI SimoderDirect3DCreate9(UINT sdkVersion) noexcept {
    ProxyResolveExport(21);
    using Create = IDirect3D9*(WINAPI*)(UINT);
    Create original{};
    std::memcpy(&original, &g_d3dExports[21], sizeof(original));
    IDirect3D9* result = original(sdkVersion);
    if (result != nullptr) { RequestBootstrap(_ReturnAddress()); }
    return result;
}

/** @summary Preserve Direct3D9Ex creation semantics and initialize only after a successful original call. */
extern "C" HRESULT WINAPI SimoderDirect3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** output) noexcept {
    ProxyResolveExport(22);
    using Create = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    Create original{};
    std::memcpy(&original, &g_d3dExports[22], sizeof(original));
    const HRESULT result = original(sdkVersion, output);
    if (SUCCEEDED(result) && output != nullptr && *output != nullptr) { RequestBootstrap(_ReturnAddress()); }
    return result;
}

/** @summary Record only the proxy handle; all library loading and service work happens after attach. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_proxy = instance;
        sc13::bootstrap::SetModuleHandle(instance);
    }
    return TRUE;
}
