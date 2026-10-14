#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sc13::reverse {

/** @summary Shares the verified profile between disk preflight and runtime symbol validation. */
inline constexpr std::string_view kSupportedGameVersion = "10.3.6.0";
inline constexpr std::string_view kSupportedGameFileSha256 =
    "4E5F136A7BF58A52C4B97ED9C9A615A118D4A696E13AA8CAA6A0A51FD4516D35";
inline constexpr std::string_view kSupportedGameTextSha256 =
    "D187C19201972ABEBCAA2FA5D8E38263D7A0B0FC3B6803D646636AFB14C49D39";

/** @summary Records disk evidence without assuming that a version difference means a game update. */
struct GameBuildAssessment final {
    std::string fileVersion;
    std::string fileSha256;
    bool verified{};
};

/** @summary Reads an x86 PE, its version metadata and file hash without starting or modifying it. */
[[nodiscard]] bool AssessGameBuild(const std::filesystem::path& executable,
    GameBuildAssessment& assessment, std::string& error) noexcept;

}  // namespace sc13::reverse
