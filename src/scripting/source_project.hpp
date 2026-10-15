#pragma once
#include "core/sha256.hpp"
#include "mods/mod_types.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace sc13::scripting {
/** @summary Describes validated source inputs shared by discovery and the external compiler. */
struct SourceProject final {
    mods::ModManifest metadata;
    std::filesystem::path directory;
    std::string fingerprint;
    std::string projectKey;
    std::string language;
    std::string entry;
    std::size_t memoryBytes{8U * 1024U * 1024U};
    unsigned budgetMs{2};
    bool hasResources{};
    /** @summary Retain validated current-manifest declarative patches alongside script sources. */
    std::vector<mods::ResourcePatchDefinition> patches;
    /** @summary Retain developer services separately from sandboxed gameplay scripts. */
    std::shared_ptr<const dev::ServiceModDefinition> services;
};
/** @summary Resolve enabled-mod authoring tools; standalone DevTools may use its own sibling executables. */
[[nodiscard]] std::filesystem::path ToolPath(const wchar_t* filename);
/** @summary Find the explicitly installed or configured developer TypeScript toolchain. */
[[nodiscard]] std::filesystem::path TypeScriptToolchainPath();
/** @summary Validates paths and hashes declared inputs, including local script modules and originals. */
[[nodiscard]] SourceProject InspectSourceProject(const std::filesystem::path& directory);
/** @summary Loads a source mod through an immutable external cache generation without partial activation. */
[[nodiscard]] bool LoadSourceProject(const std::filesystem::path& directory,
    mods::ModDefinition& definition, std::string& error) noexcept;
/** @summary Executes an explicit trusted binary with quoted arguments, bounded wait and process-tree cleanup. */
void RunTool(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
    const std::filesystem::path& log, unsigned timeoutMs = 60000);
/** @summary Resolves a relative source path and rejects traversal, streams, links and junctions. */
[[nodiscard]] std::filesystem::path SourcePath(const std::filesystem::path& root, std::string_view relative);
/** @summary Reads a regular file subject to a hard size limit. */
[[nodiscard]] std::string ReadBounded(const std::filesystem::path& path, std::size_t maximum);
/** @summary Hashes a byte string to canonical uppercase SHA-256. */
[[nodiscard]] std::string HashText(std::string_view bytes);
/** @summary Adds the extended Win32 path prefix for deep immutable cache generations. */
[[nodiscard]] std::filesystem::path NativePath(const std::filesystem::path& path);
/** @summary Atomically publish one anchored plain staging directory without replacing a destination.
 * @param staging Owned unpublished directory beside the destination.
 * @param generation Immutable destination in the same plain parent directory.
 * @return True after publication; false when a destination exists and requires full caller validation.
 * @throws std::runtime_error Invalid identity/admission or publication failure; only errors 5/32 receive
 * at most 11 attempts within 500 ms plus native-call overhead. No copy or replacement fallback exists.
 */
[[nodiscard]] bool PublishGeneration(const std::filesystem::path& staging, const std::filesystem::path& generation);
}
