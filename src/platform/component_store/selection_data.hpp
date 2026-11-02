/** @file @summary One-shot immutable selected bytes, explicitly DATA only; cannot issue Base/manager/runtime readiness. */
#pragma once
#include "platform/component_store/verified_state.hpp"
namespace sc13::platform::component_store {
/** @summary Own the original verification turn and uncertain native custody until its original caller ends.
 * @remarks Same StoreIO, Parent, observation and native admission MUST outlive this object. Native admission
 * is independently supplied, never inferred from portable metadata. Output is immutable historical DATA,
 * not VerifiedBaseSelection, an activation permission or a claim the current selector cannot change. */
class SelectionDataCapture final:private SelectionSnapshotAdmission {
    metadata::Identifier installation_;
    std::shared_ptr<const CapturedSelectionData> staged_;
    SelectionReadTurn turn_;
    bool attempted_{};
    /** @summary Materialize the exact verified bytes under the original lock, without runtime admission or callbacks. */
    bool Seal(CapturedSelectionData&&)noexcept override;
public:
    /** @summary Bind one genuine borrowed native IO/root/admission; no filesystem effects start in construction. */
    SelectionDataCapture(StoreIO&,Parent&,const Observation&,const NativeAdmission&,metadata::Identifier);
    /** @summary End original local custody only, never replay, delete, activate or mint readiness. */
    ~SelectionDataCapture()override=default;
    SelectionDataCapture(const SelectionDataCapture&)=delete;
    SelectionDataCapture& operator=(const SelectionDataCapture&)=delete;
    /** @summary Capture exactly once using independent pins and complete-history verification.
     * @param output Must be empty; assigned only after positively acknowledged coordination release.
     * @remarks Failure never publishes data. Unknown retains the original turn; no retry on this object.
     * Output can outlive this turn as immutable historical bytes, with no native owner authority. */
    SelectionReadStatus Capture(const base_artifact::TrustedArtifact&,const std::map<std::string,std::string>&,
        std::string_view,const metadata::Compatibility&,std::shared_ptr<const CapturedSelectionData>& output)noexcept;
};
}
