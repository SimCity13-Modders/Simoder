#pragma once
#include "core/sha256.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace sc13::platform::base_artifact {
/** @summary Finite Base-only transport profile; these limits do not enlarge BaseHost limits. */
inline constexpr std::size_t MaximumEntries=65, MaximumArtifact=8388608,
    MaximumExecutable=4194304, MaximumNotice=65536, MaximumPath=240;
/** @summary Validate the shared finite canonical Base-relative path profile without filesystem effects.
 * @param path Literal slash-relative ASCII path. @return False for traversal, aliases or profile excess.
 * @remarks Syntax only; this grants neither filesystem ownership nor content trust. */
[[nodiscard]] bool NameValid(std::string_view path) noexcept;
/** @summary Native independently accepted content expectation; never obtain this from the imported archive. */
struct FilePin final {
    std::string_view path;
    std::uint32_t size{};
    core::Sha256Digest digest{};
    bool notice{};
};
/** @summary Exact externally accepted archive and install map; caller retains immutable map/path storage throughout synchronous Capture.
 * @remarks Concurrent mutation or destruction of these borrowed native inputs is forbidden. These are independently
 * accepted issuer inputs, never a map derived from mutable archive/package content. */
struct TrustedArtifact final {
    std::uint32_t size{};
    core::Sha256Digest digest{};
    std::span<const FilePin> files;
};
/** @summary Validate only finite native descriptor DATA shape before acquisition/capture effects.
 * @param artifact Independently selected immutable map borrowed throughout return.
 * @return Whether canonical names, unique file membership and existing Base size/count limits hold.
 * @remarks This performs no allocation, filesystem, UI, network, digest verification or permission decision.
 * True does NOT authenticate bytes, supply a release/native issuer, or permit installation/activation.
 * Snapshot::Capture still verifies the complete independently pinned actual archive and member hashes. */
[[nodiscard]] bool ValidateTrustedArtifact(const TrustedArtifact& artifact) noexcept;
/** @summary Refusal categories contain neither private paths nor imported content. */
enum class Error : std::uint8_t { None, InvalidPin, InvalidPath, Open, OrdinaryFile,
    Capture, Changed, Allocation, Hash, ArtifactDigest, Header, Membership, Content };
/** @summary Immutable captured archive and validated file views; this type has no extraction or activation API.
 * @remarks Capture and final disposal must occur outside Core critical guards. Retain shared ownership while
 * borrowing Bytes/File; returned spans cannot outlive their original Snapshot. No source path is reread. */
class Snapshot final {
public:
    Snapshot(const Snapshot&)=delete;
    Snapshot& operator=(const Snapshot&)=delete;
    /** @summary Capture an ordinary disk file once, hash that private snapshot and parse exactly those bytes.
     * @param path Absolute local drive path, at most32760 UTF-16 units and65 retained parent directories;
     * ordinary ancestry only, no caller device/UNC/reparse syntax. Internal extended syntax supports paths beyond260.
     * @param trusted Independently accepted length, digest and complete executable/notice map.
     * @param error Exact refusal or None. @return Immutable original ownership, or empty on refusal. */
    [[nodiscard]] static std::shared_ptr<const Snapshot> Capture(
        const std::filesystem::path& path,const TrustedArtifact& trusted,Error& error) noexcept;
    /** @summary Borrow all authenticated bytes while retaining this Snapshot. @return Same original buffer. */
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return {bytes_.get(),size_}; }
    /** @summary Count validated regular files. @return Complete selected-map count, including LICENSE. */
    [[nodiscard]] std::size_t Count() const noexcept { return count_; }
    /** @summary Borrow one canonical name; caller retains Snapshot. @param index Validated index. @return Empty for invalid index. */
    [[nodiscard]] std::string_view Name(std::size_t index) const noexcept;
    /** @summary Borrow a validated file's exact captured payload; caller retains Snapshot.
     * @param index Validated index. @return Empty for invalid index or an admitted empty file. */
    [[nodiscard]] std::span<const std::byte> File(std::size_t index) const noexcept;
    /** @summary Distinguish the notice from executable BasePackagePin entries. @param index Index. @return True only for LICENSE. */
    [[nodiscard]] bool Notice(std::size_t index) const noexcept;
private:
    /** @summary Fixed record owns its path and indexes the sole original buffer. */
    struct Record final { std::array<char,MaximumPath+1> path{}; std::uint32_t offset{},size{}; bool notice{}; };
    /** @summary Construction is private; only fully verified objects are returned. */
    Snapshot()=default;
    /** @summary Strictly decode the already authenticated original buffer. @param pins Trusted complete map.
     * @param scratch Preallocated CNG object scratch. @return Exact validation result. */
    Error Parse(std::span<const FilePin> pins,std::span<std::byte> scratch) noexcept;
    std::unique_ptr<std::byte[]> bytes_;
    std::uint32_t size_{};
    std::size_t count_{};
    std::array<Record,MaximumEntries> records_{};
};
static_assert(sizeof(Snapshot)<32768,"Fixed snapshot control must fit the 128KiB profile");
} // namespace sc13::platform::base_artifact
