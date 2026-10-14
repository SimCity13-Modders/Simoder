#include "reverse/build_compatibility.hpp"

#include "core/sha256.hpp"
#include "reverse/pe_file.hpp"

#include <Windows.h>

#include <cstdio>
#include <vector>

namespace sc13::reverse {

bool AssessGameBuild(const std::filesystem::path& executable,
    GameBuildAssessment& assessment, std::string& error) noexcept {
    try {
        assessment = {};
        PeFile pe;
        core::Sha256Digest hash{};
        if (!pe.Open(executable, error) || !core::Sha256File(executable, hash, error)) {
            return false;
        }
        assessment.fileSha256 = core::ToHex(hash);
        assessment.fileVersion = "unknown";
        DWORD ignored{};
        const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
        if (size > 0U && size <= 1024U * 1024U) {
            std::vector<std::byte> data(size);
            VS_FIXEDFILEINFO* info{};
            UINT infoSize{};
            if (GetFileVersionInfoW(executable.c_str(), 0, size, data.data()) != FALSE &&
                VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) != FALSE &&
                info != nullptr && infoSize >= sizeof(VS_FIXEDFILEINFO) && info->dwSignature == 0xFEEF04BDU) {
                char version[64]{};
                std::snprintf(version, sizeof(version), "%u.%u.%u.%u",
                    HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                    HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
                assessment.fileVersion = version;
            }
        }
        assessment.verified = assessment.fileSha256 == kSupportedGameFileSha256;
        return true;
    } catch (...) {
        assessment = {};
        error = "Could not assess the game executable";
        return false;
    }
}

}  // namespace sc13::reverse
