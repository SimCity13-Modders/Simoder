#pragma once

#include <cstdint>

namespace sc13::platform {
/** @summary Preserve the existing publication policy's maximum syscall attempts. */
inline constexpr unsigned FilePublicationMaximumAttempts = 11;
/** @summary Preserve the existing publication policy's elapsed backoff budget. */
inline constexpr std::uint64_t FilePublicationBudgetMs = 500;

/** @summary Select bounded backoff only for Win32 access/sharing denial; no permission or path fallback.
 * @param error Immediately captured Win32 code; 5 and 32 are ERROR_ACCESS_DENIED/ERROR_SHARING_VIOLATION.
 * @param attempts Number of actual failed publication attempts, starting at one.
 * @param elapsedMs Elapsed monotonic time since the first attempt.
 * @return Delay up to 50 ms within the existing 500 ms/11-attempt envelope, or zero to stop. */
[[nodiscard]] constexpr std::uint32_t FilePublicationRetryDelay(
    std::uint32_t error, unsigned attempts, std::uint64_t elapsedMs) noexcept {
    if ((error != 5 && error != 32) || attempts == 0 ||
        attempts >= FilePublicationMaximumAttempts || elapsedMs >= FilePublicationBudgetMs) return 0;
    const auto remaining = FilePublicationBudgetMs - elapsedMs;
    return static_cast<std::uint32_t>(remaining < 50 ? remaining : 50);
}
}
