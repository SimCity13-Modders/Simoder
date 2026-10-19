#pragma once

#include "mods/mod_types.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sc13::mods {
namespace state_detail {
/** @summary Closed native byte sink shared by legacy and qualified serializers. */
using TextSink=bool(*)(void*,std::string_view);
/** @summary Validate/order existing IDs and emit the unchanged format. @param enabled Input order. @param sink Closed byte sink. @param context Sink state. @param maximum Maximum bytes, SIZE_MAX for legacy. @param error Diagnostic. @return Complete serialization. */
bool Serialize(std::span<const ModId> enabled,TextSink sink,void* context,std::size_t maximum,std::string& error);
/** @summary Native callback result; Stop prevents retries after uncertain preflight/effect. */
enum class Attempt {Succeeded,Failed,Stop,StopAfterEffect};
using Publication=Attempt(*)(void*,std::uint32_t&);
/** @summary Separates native failures, unexpected native callback exceptions, wall elapsed and requested backoff. */
struct RetryResult {bool success{},exception{};std::uint32_t error{},calls{};std::uint64_t elapsedMs{},backoffMs{};};
/** @summary Share existing11/500ms/error5-or32 native retry policy. @param call Closed native effect callback. @param context Native state. @return Actual callback failure counts and elapsed time. */
RetryResult PublishWithRetry(Publication call,void* context) noexcept;
}

/** @summary Loads persisted enabled IDs in deterministic activation order.
 * @param path Existing state file. @param enabled Receives validated ordered IDs, cleared on failure.
 * @param error Receives a diagnostic without secret or mod contents. @return True for valid or absent state. */
[[nodiscard]] bool LoadEnabledState(
    const std::filesystem::path& path,
    std::vector<ModId>& enabled,
    std::string& error) noexcept;

/** @summary Atomically persists IDs with bounded same-path Win32 5/32 retries; persistent denial preserves prior state.
 * @param path Destination state file, serialized by its owning manager. @param enabled Valid ordered IDs.
 * @param error Receives the actual final Win32 code and attempt diagnostics on publication failure.
 * @return True only after the unchanged atomic-replace operation succeeds. */
[[nodiscard]] bool SaveEnabledState(
    const std::filesystem::path& path,
    std::span<const ModId> enabled,
    std::string& error) noexcept;

}  // namespace sc13::mods
