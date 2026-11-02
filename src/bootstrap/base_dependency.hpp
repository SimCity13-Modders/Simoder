#pragma once
#include "platform/base_artifact_reader.hpp"
#include "logging/async_logger.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace sc13::bootstrap {
class BaseBootstrap;
/** @summary Independently selected exact anonymous release destination; never parsed from a downloaded manifest.
 * @remarks An accepted descriptor lists complete URLs, not suffix/prefix wildcard authorization. */
struct BaseReleaseUrl final { std::wstring_view exact; };
/** @summary Immutable native bootstrap selection, borrowed only during Begin's bounded private copy.
 * @remarks Selection is a content expectation, not native-code trust, installation or operation permission. */
struct BaseDependencySelection final {
    platform::base_artifact::TrustedArtifact artifact;
    std::wstring_view version,channel,target,reason;
    std::span<const BaseReleaseUrl> urls;
};
/** @summary Actual controller observations; Captured means authenticated bytes only, never installed or ready. */
enum class BaseDependencyState : std::uint8_t {
    Vacant, Decision, ManualPath, Connecting, Sending, Headers, Reading,
    Closing, Captured, Cancelled, Expired, Refused, Unresolved
};
/** @summary Finite diagnostic without a private path, URL query, raw input, or bearer data. */
enum class BaseDependencyError : std::uint8_t {
    None, Busy, InvalidSelection, Console, Input, Deadline, Cancelled, Stage,
    UnsupportedOption, Network, Url, Headers, Body, Capture, Changed, Cleanup
};
/** @summary One original console/acquisition slot across every generation, retained by its original RuntimeContext.
 * @remarks All methods except RequestCancel run on the constructing thread, outside D/B/Q0/Qn/VM guards and
 * DllMain. No worker or cleanup queue is created. Pump is the real owning-thread service route, not a scheduler.
 * The parent must retain this controller and executable module through its last Pump/destructor return.
 * The destructor cooperatively waits for genuine OS closure, never abandons a live callback or frees on timeout.
 * This source unit neither constructs a BaseHost nor performs installation/filesystem transactions. */
class BaseDependency final {
    struct Body;
public:
    /** @summary Move-only actual acquisition reader; no shared/weak Snapshot ownership can escape the original slot.
     * @remarks A borrowed Snapshot/span must not outlive this handle. Destroy/move-assign off Core guards.
     * The original controller remains retained until every such handle has ended. */
    class CaptureReader final {
    public:
        /** @summary Construct an empty reader without allocation or an owner. */
        CaptureReader() noexcept=default;
        /** @summary Finish this actual reader; never publishes installation/readiness. */
        ~CaptureReader();
        CaptureReader(const CaptureReader&)=delete;
        CaptureReader& operator=(const CaptureReader&)=delete;
        /** @summary Transfer the same original reader without incrementing admission. @param other Original handle. */
        CaptureReader(CaptureReader&& other) noexcept;
        /** @summary End current reader then transfer original ownership offguards. @param other Original handle. @return This handle. */
        CaptureReader& operator=(CaptureReader&& other) noexcept;
        /** @summary Borrow the authenticated snapshot while retaining this handle. @return Null for an empty reader. */
        [[nodiscard]] const platform::base_artifact::Snapshot* Get() const noexcept;
    private:
        friend class BaseDependency;
        friend class BaseBootstrap;
        /** @summary Retain SAME actual capture for synchronous component installation; never reconstruct ownership. */
        std::shared_ptr<const platform::base_artifact::Snapshot> Share()const noexcept;
        /** @summary Count one actual reader on the original owning thread. @param body Retained original body. */
        explicit CaptureReader(Body* body) noexcept;
        Body* body_{};
    };
    /** @summary Bind the actual existing logger/console owner without opening handles. @param logger Original logger. */
    explicit BaseDependency(logging::AsyncLogger& logger) noexcept;
    /** @summary Cancel and wait offguards until actual input/WinHTTP callbacks/readers and staging ownership close. */
    ~BaseDependency();
    BaseDependency(const BaseDependency&)=delete;
    BaseDependency& operator=(const BaseDependency&)=delete;
    /** @summary Claim a fresh explicit attempt only when the original slot is genuinely vacant.
     * @param selected Independently accepted immutable native selection; the archive never supplies this trust.
     * @param stagingParent Existing absolute ordinary fixed-drive disposable directory owned by this native bootstrap;
     * at most4096 UTF16 units; foreign ownership is not acquired by passing a name. Parents are retained without delete sharing.
     * @return False for occupied/invalid input. An occupied owner-thread attempt requests cancellation and retains no retry record.
     * A foreign-thread call returns false without inspecting/mutating mutable original owner state. */
    [[nodiscard]] bool Begin(const BaseDependencySelection& selected,const std::filesystem::path& stagingParent) noexcept;
    /** @summary Retire the original selection/configuration/session from any thread, without allocating or clearing its slot. */
    void RequestCancel() noexcept;
    /** @summary Perform one real offguard transition, waiting at most100ms; no deadline is renewed.
     * @return Current observation, including Unresolved when the5000ms close observation expires. */
    [[nodiscard]] BaseDependencyState Pump() noexcept;
    /** @summary Borrow the authenticated original capture while retaining the controller.
     * @return Move-only same-original immutable reader only after capture and network closure. A reader blocks slot recycling. */
    [[nodiscard]] CaptureReader Capture() const noexcept;
    /** @summary Explicitly release a terminal attempt only after all returned capture readers have ended.
     * @return True only after actual offguard disposal. This never queues a retry or begins another attempt. */
    [[nodiscard]] bool Release() noexcept;
    /** @summary Observe the original nonwrapping generation on its owning thread. @return Zero before any admitted attempt or on foreign-thread refusal. */
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    /** @summary Observe the bounded original refusal on its owning thread. @return Foreign-thread Busy is call refusal only, never a mutation of original state; no path/content is included. */
    [[nodiscard]] BaseDependencyError Error() const noexcept;
private:
    std::unique_ptr<Body> body_;
    logging::AsyncLogger& logger_;
    DWORD thread_{};
    std::uint64_t generation_{};
    BaseDependencyError lastError_{};
    /** @summary Saturating original admission epoch; a concurrent invalidation is never cleared by Begin. */
    std::atomic<std::uint64_t> cancellationEpoch_{};
#if defined(SIMODER_BASE_DEPENDENCY_TEST)
    friend struct BaseDependencyTestAccess;
    /** @summary The isolated installed-Base fixture can deny unexpected acquisition at the existing preeffect cut only. */
    friend class BaseBootstrap;
    /** @summary Deterministic actual Begin cut; absent from production and never an approval source. */
    enum class BeginCut : std::uint8_t { Selected, ParentsRetained };
    void (*beginCut_)(BaseDependency&,BeginCut) noexcept{};
#endif
};
}
