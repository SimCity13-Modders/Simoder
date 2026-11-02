#pragma once

#include "platform/runtime_owner.hpp"
#include "bootstrap/base_dependency.hpp"
#include "runtime/runtime_lifecycle.hpp"
#include "runtime/runtime_scope.hpp"
#include "platform/component_store/selection_data.hpp"

#include <Windows.h>
#include <memory>

namespace sc13::scripting {class BaseHost;struct BasePackagePin;}
namespace sc13::mods { class ModManager; }
namespace sc13::bootstrap {
class SelectionDataSession;
/** @summary Original bootstrap ownership and once-only prepared publication, never native authority.
 * @pre Ownership mutation is serialized by the bootstrap operation guard. All callers hold no Q/State/L
 * or compound publication guard. Same-domain D rejection occurs before ownership staging. */
class RuntimeContext final {
public:
    /** @summary Compose the actual scope and bound publisher on one original domain.
     * @param owner Already acquired process owner; construction occurs after real build verification.
     * @throws std::invalid_argument Null or unacquired ownership is rejected before domain construction. */
    explicit RuntimeContext(std::shared_ptr<platform::RuntimeOwner> owner);
    /** @summary Dispose original session only offguards after synchronous operations have returned; no file deletion. */
    ~RuntimeContext();
    /** @summary Capture immutable installed DATA exactly once against retained native admission/independent pins.
     * @remarks Output grants no runtime/native authority or readiness. Same original constructing thread/offguard only.
     * Context retains session/roots/admission through Unknown and stop; no automatic recovery or recapture.
     * Any acquired current-thread D and script lane are rejected. Caller must exclude all Q/B/L/borrow
     * cuts; this current source has no universal dynamic detector for those preconditions. */
    platform::component_store::SelectionReadStatus CaptureSelectionData(std::string_view root,
        const platform::component_store::metadata::Identifier& installation,
        std::shared_ptr<const platform::component_store::NativeAdmission> admission,
        const platform::base_artifact::TrustedArtifact& trusted,const std::map<std::string,std::string>& executablePins,
        std::string_view version,const platform::component_store::metadata::Compatibility& compatibility,
        std::shared_ptr<const platform::component_store::CapturedSelectionData>& output)noexcept;
    /** @summary Construct one private uncommitted/unscheduled Base VM from SAME retained successful capture and independent pin.
     * @remarks No host getter, readiness, publication, manager pairing or operation authority is supplied. */
    runtime::ScopeResult ConstructPausedCapturedBase(const scripting::BasePackagePin&)noexcept;
    /** @summary Same-thread offguard exact close; Busy/NotReady/uncertainty retain the same original VM/work; later calls only continue it, never resubmit Close.
     * @remarks RequestStopIntent never calls this or any VM callback. Context destruction remains existing offguard
     * VM destructor contract; CloseUnknown is not qualified by this unit. */
    runtime::ScopeResult DisposePausedCapturedBase()noexcept;
    /** @summary Begin explicit acquisition on the original constructing thread, offguards; never auto-starts from loader.
     * @remarks Same independent trust/console consent as BaseDependency. No runtime witness, installation or readiness.
     * @pre No Q/B/L/native borrow guards. Concurrent stop callers retain this Context throughout their call. */
    [[nodiscard]] bool BeginBaseAcquisition(const BaseDependencySelection&,const std::filesystem::path&)noexcept;
    /** @summary Service genuine original cleanup even after domain stop; foreign/D/lane calls return Unresolved. */
    [[nodiscard]] BaseDependencyState PumpBaseAcquisition()noexcept;
    /** @summary Borrow authenticated immutable DATA/history only; stop after the last check may race this return.
     * @remarks Reader must end before Context destruction. Caller retains Context; no current-authority promise.
     * @pre Same original thread/offguards, with Q/B/L/native borrow exclusions as caller obligations. */
    [[nodiscard]] BaseDependency::CaptureReader CaptureBaseAcquisition()const noexcept;
    /** @summary Release only the original terminal slot after real readers/OS cleanup, including after domain stop. */
    [[nodiscard]] bool ReleaseBaseAcquisition()noexcept;
    /** @summary Observe original bounded acquisition error; foreign/D/lane calls return Busy without owner state access. */
    [[nodiscard]] BaseDependencyError BaseAcquisitionError()const noexcept;
    RuntimeContext(const RuntimeContext&) = delete;
    RuntimeContext& operator=(const RuntimeContext&) = delete;
    /** @summary Return original retained ownership without creating a second store.
     * @return Same process guard. @pre Reference transfers occur off publication guards. */
    const std::shared_ptr<platform::RuntimeOwner>& Owner() const noexcept { return owner_; }
    /** @summary Return the original writer lane. @return Same domain, not a reservation. */
    const std::shared_ptr<runtime::RuntimeCommitDomain>& Domain() const noexcept { return domain_; }
    /** @summary Return original scope. @return Same retained scope, never a grant. */
    const std::shared_ptr<runtime::RuntimeScope>& Scope() const noexcept { return scope_; }
    /** @summary Return original bound publisher. @return Same retained lifecycle. */
    const std::shared_ptr<runtime::RuntimeLifecycle>& Lifecycle() const noexcept { return lifecycle_; }
    /** @summary Publish preparation at most once and truthfully revalidate repeated success.
     * @return Zero only for actual prepared facts; Busy for rejected preparation, 15 for an exception.
     * @pre No Q/State/L/compound guard; same D is explicitly rejected before any scope operation. */
    DWORD Prepare() noexcept;
    /** @summary Existing intent path only; never adds lifecycle dispatch, joining or reset.
     * @param reason Existing scope diagnostic. @pre Existing window stop constraints apply. */
    void RequestStopIntent(runtime::StopReason reason) noexcept;
    /** @summary Independent local close diagnostics; none certifies manager or capture destruction. */
    struct CloseObservation final {
        runtime::RuntimeLifecycle::CloseReceipt receipt;
        runtime::RuntimeLifecycle::DeliveryStatus delivery;
        runtime::LifecycleObservationStatus receiptStatus{runtime::LifecycleObservationStatus::Unavailable};
        runtime::LifecycleObservationStatus deliveryStatus{runtime::LifecycleObservationStatus::Unavailable};
        bool scopeDrained{},managerScopesDrained{},publicationThrew{};
    };
    /** @summary Cooperatively close original local state while retaining every original dependency.
     * @param output Independent actual receipt/delivery/scope facts; unchanged on same-D rejection.
     * @return ERROR_BUSY: local close never certifies global services finalizable in R1.
     * @pre Ordinary off-guard call may synchronously execute arbitrary callbacks and destructors. */
    DWORD CloseRetained(CloseObservation& output) noexcept;
private:
    /** @summary Records only whether this exact preparation algorithm ran, never readiness or authority. */
    enum class Attempt { NeverAttempted,Publishing,Prepared,Failed };
    /** @summary Read actual facts without replaying activation/publication.
     * @param requiredReadiness Original scope bits required after setting readiness or on a repeated entry;
     * zero permits the first pre-readiness publication check.
     * @return Truthful factual conjunction, never an atomic operation authorization. */
    bool PreparedFacts(std::uint32_t requiredReadiness) const;
    /** @summary Cache terminal failure and request original stop outside guards.
     * @param error Original nonzero return. @return Same original error. */
    DWORD Fail(DWORD error) noexcept;
    std::shared_ptr<platform::RuntimeOwner> owner_;
    std::shared_ptr<runtime::RuntimeCommitDomain> domain_;
    std::shared_ptr<runtime::RuntimeScope> scope_;
    std::shared_ptr<runtime::RuntimeLifecycle> lifecycle_;
    /** @summary Fixed original controller destroys before domain/scope/lifecycle; no optional pointer publication race or Context backedge. */
    BaseDependency baseAcquisition_;
    DWORD originalThread_{GetCurrentThreadId()};
    bool selectionAttempted_{};
    std::shared_ptr<SelectionDataSession> selectionData_;
    bool pausedBaseAttempted_{};
    std::unique_ptr<scripting::BaseHost> pausedBase_;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ContextPausedBaseTestAccess;
    friend struct ContextBaseAcquisitionTestAccess;

#endif
    Attempt attempt_{Attempt::NeverAttempted};
    DWORD failure_{ERROR_BUSY};
};

/** @summary Shared real root retry routing; caches earlier startup failures without admitting retries. */
class RootInitializationAttempt final {
public:
    /** @summary Either execute real startup once or return this exact attempt's factual result. */
    struct Route final { bool start{};DWORD result{ERROR_BUSY}; };
    /** @summary Choose the actual Initialize path after root argument validation/operation serialization.
     * @param retired Actual monotonic process-stop fact. @param context Original context, if constructed.
     * @return Startup once, same cached failure, shutdown rejection or actual prepared revalidation. */
    Route Enter(bool retired,RuntimeContext* context) noexcept;
    /** @summary Cache the first real startup outcome, including failures preceding context construction.
     * @param result Actual original startup return. @return Same value. */
    DWORD Finish(DWORD result) noexcept;
private:
    bool entered_{},finished_{};
    DWORD result_{ERROR_BUSY};
};

/** @summary Production-used retained shutdown route; scopes never stand in for manager finalization.
 * @param context Original context. @param manager Original manager, possibly absent after startup failure.
 * @param output Actual local diagnostics, unchanged on same-D rejection.
 * @return ERROR_BUSY until separately qualified original global teardown exists; retains all originals.
 * @pre No Q/State/L/compound guard. May execute cooperative callbacks/destructors outside those guards. */
DWORD RetainRuntimeShutdown(RuntimeContext& context,mods::ModManager* manager,
    RuntimeContext::CloseObservation& output) noexcept;
}
