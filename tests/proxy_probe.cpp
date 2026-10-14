#include <Windows.h>
#include <shellapi.h>
#include <d3d9.h>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

/** @summary Load typed functions without nonportable FARPROC casts. */
template <typename Function>
Function FunctionAt(HMODULE module, LPCSTR name) {
    const FARPROC address = GetProcAddress(module, name);
    Function function{};
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

/** @summary Exercise the real proxy from a synthetic GUI process, with no game or package dependencies. */
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr || count != 2) { if (arguments) { LocalFree(arguments); } return 2; }
    const std::filesystem::path reportPath = arguments[1];
    LocalFree(arguments);
    std::array<wchar_t, 32768> path{};
    GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    const auto executableDirectory = std::filesystem::path(path.data()).parent_path();
    const HWND initialConsole = GetConsoleWindow();
    const HMODULE proxy = LoadLibraryW((executableDirectory / "d3d9.dll").c_str());
    GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    const HMODULE original = LoadLibraryExW((std::filesystem::path(path.data()) / "d3d9.dll").c_str(),
        nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (proxy == nullptr || original == nullptr || proxy == original) { return 3; }
    using Create = IDirect3D9*(WINAPI*)(UINT);
    using CreateEx = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    using Status = DWORD(WINAPI*)();
    const Create create = FunctionAt<Create>(proxy, "Direct3DCreate9");
    const Create baseline = FunctionAt<Create>(original, "Direct3DCreate9");
    if (create == nullptr || baseline == nullptr) { return 4; }
    bool exports = true;
    for (WORD ordinal = 16; ordinal <= 38; ++ordinal) {
        if (GetProcAddress(original, MAKEINTRESOURCEA(ordinal)) != nullptr &&
                GetProcAddress(proxy, MAKEINTRESOURCEA(ordinal)) == nullptr) { exports = false; }
    }
    constexpr const char* names[] = {
        "Direct3DCreate9On12", "Direct3DCreate9On12Ex", "Direct3DShaderValidatorCreate9", "PSGPError", "PSGPSampleTexture",
        "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus", "D3DPERF_QueryRepeatFrame", "D3DPERF_SetMarker",
        "D3DPERF_SetOptions", "D3DPERF_SetRegion", "DebugSetLevel", "DebugSetMute",
        "Direct3D9EnableMaximizedWindowedModeShim", "Direct3DCreate9", "Direct3DCreate9Ex"};
    for (const char* name : names) { if (GetProcAddress(proxy, name) == nullptr) { exports = false; } }
    IDirect3D9* invalid = create(0);
    IDirect3D9* invalidOriginal = baseline(0);
    const bool invalidMatches = (invalid == nullptr) == (invalidOriginal == nullptr);
    if (invalid) { invalid->Release(); }
    if (invalidOriginal) { invalidOriginal->Release(); }
    std::atomic_bool creation{true};
    std::vector<std::thread> callers;
    for (unsigned int i = 0; i < 4; ++i) {
        callers.emplace_back([create, &creation] {
            for (unsigned int attempt = 0; attempt < 8; ++attempt) {
                IDirect3D9* api = create(D3D_SDK_VERSION);
                if (api == nullptr || api->GetAdapterCount() == 0) { creation.store(false); }
                if (api) { api->Release(); }
            }
        });
    }
    for (auto& caller : callers) { caller.join(); }
    const auto status = FunctionAt<Status>(proxy, "D3DPERF_GetStatus");
    const auto originalStatus = FunctionAt<Status>(original, "D3DPERF_GetStatus");
    // @summary Warm the real API's thread-local state before comparing its observable error behavior.
    originalStatus();
    status();
    SetLastError(0x1234);
    const DWORD forwardedStatus = status();
    const DWORD forwardedError = GetLastError();
    SetLastError(0x1234);
    const DWORD baselineStatus = originalStatus();
    const DWORD baselineError = GetLastError();
    const bool statusMatches = forwardedStatus == baselineStatus && forwardedError == baselineError;
    const auto createEx = FunctionAt<CreateEx>(proxy, "Direct3DCreate9Ex");
    const auto baselineEx = FunctionAt<CreateEx>(original, "Direct3DCreate9Ex");
    IDirect3D9Ex* extended{}, *originalExtended{};
    const HRESULT extendedResult = createEx(D3D_SDK_VERSION, &extended);
    const HRESULT originalExtendedResult = baselineEx(D3D_SDK_VERSION, &originalExtended);
    const bool extendedMatches = extendedResult == originalExtendedResult;
    if (extended) { extended->Release(); }
    if (originalExtended) { originalExtended->Release(); }
    // @summary Wait only in this test host, never inside the production proxy's graphics or DllMain paths.
    const auto runtimeDirectory = executableDirectory.parent_path() / "simoder";
    const ULONGLONG deadline = GetTickCount64() + 10000;
    std::string log;
    while (GetTickCount64() < deadline) {
        std::ifstream stream(runtimeDirectory / "logs" / "proxy-bootstrap.log");
        log.assign(std::istreambuf_iterator<char>(stream), {});
        if (log.find("runtime.initialize=0") != std::string::npos ||
            log.find("services stopped") != std::string::npos ||
            log.find("runtime unavailable") != std::string::npos ||
            log.find("disabled:") != std::string::npos) { break; }
        Sleep(20);
    }
    const bool consoleUnchanged = initialConsole == GetConsoleWindow();
    std::ofstream(reportPath) << "exports=" << exports << "\ncreation=" << creation.load()
        << "\ninvalidMatches=" << invalidMatches << "\nstatusMatches=" << statusMatches
        << "\nforwardedStatus=" << forwardedStatus << "\nbaselineStatus=" << baselineStatus
        << "\nforwardedError=" << forwardedError << "\nbaselineError=" << baselineError
        << "\nextendedMatches=" << extendedMatches << "\nconsoleUnchanged=" << consoleUnchanged << '\n';
    return exports && creation.load() && invalidMatches && statusMatches && extendedMatches && consoleUnchanged && !log.empty() ? 0 : 1;
}
