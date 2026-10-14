#include "dev/runtime_control.hpp"
#include "mods/mod_manager.hpp"
#include "core/json.hpp"
#include <Windows.h>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>

namespace sc13::dev {
/** @summary Consume one bounded, expiring, PID-bound developer command and acknowledge its exact nonce atomically. */
void ProcessRuntimeControl(mods::ModManager& manager, const std::filesystem::path& directory) {
    const auto request = directory / L"runtime-control.request";
    const auto attributes = GetFileAttributesW(request.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return;
    std::error_code fileError;
    const auto size = std::filesystem::file_size(request, fileError);
    if (fileError || size > 1024U) { std::filesystem::remove(request, fileError); return; }
    std::ifstream input(request, std::ios::binary);
    std::array<std::string, 6> lines;
    for (auto& line : lines) if (!std::getline(input, line)) { input.close(); std::filesystem::remove(request, fileError); return; }
    std::string extra;
    const bool extraLine = static_cast<bool>(std::getline(input, extra));
    input.close();
    std::filesystem::remove(request, fileError);
    if (fileError || extraLine || lines[0] != "SIMODERCTL1" || lines[2].size() != 32U ||
        lines[4].empty() || lines[4].size() > 256U) return;
    for (const char c : lines[2]) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return;
    for (const char c : lines[4]) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return;
    std::uint32_t pid{}; std::int64_t expires{};
    const auto parsedPid = std::from_chars(lines[1].data(), lines[1].data() + lines[1].size(), pid);
    const auto parsedExpiry = std::from_chars(lines[5].data(), lines[5].data() + lines[5].size(), expires);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string error;
    bool success = false;
    if (parsedPid.ec != std::errc{} || parsedPid.ptr != lines[1].data() + lines[1].size() || pid != GetCurrentProcessId() ||
        parsedExpiry.ec != std::errc{} || parsedExpiry.ptr != lines[5].data() + lines[5].size() || expires < now || expires > now + 30000)
        error = "Expired command or mismatched game instance";
    else if (lines[3] != "enable" && lines[3] != "disable") error = "Unsupported control action";
    else if (manager.OperationBusy()) error = "Mod controller is busy";
    else success = manager.SetEnabled(lines[4], lines[3] == "enable", error);
    const auto destination = directory / L"runtime-control-result.json";
    auto temporary = destination; temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "{\"requestId\":" << core::JsonString(lines[2]) << ",\"pid\":" << GetCurrentProcessId()
           << ",\"success\":" << (success ? "true" : "false") << ",\"modId\":" << core::JsonString(lines[4])
           << ",\"enabled\":" << (lines[3] == "enable" ? "true" : "false") << ",\"generation\":" << manager.Generation()
           << ",\"error\":" << core::JsonString(error) << "}";
    output.close();
    if (output) static_cast<void>(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}
}
