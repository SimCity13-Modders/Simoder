/** @file @summary Private non-authorizing complete-store capture turn shared with the portable coordinator verifier. */
#pragma once
#include "platform/component_store/coordinator.hpp"
#include <map>
namespace sc13::platform::component_store {
class InstalledSelectionSession;
/** @summary Data-only verified observations cannot construct an installed runtime witness. */
struct CapturedSelectionData final {metadata::Selector selector;metadata::Component component;std::map<std::string,std::string> members;};
/** @summary Native turn disposition; only actual absence is eligible for an independently admitted first installation. */
enum class SelectionReadStatus {Accepted,Absent,Refused,Busy,Interrupted,Unknown};
/** @summary Private native one-attempt snapshot cut; data alone cannot create the session's admitted incarnation. */
class SelectionSnapshotAdmission {
    friend class InstalledSelectionSession;
    friend class SelectionDataCapture;
    friend class SelectionReadTurn;
    /** @summary Only the original native session can construct the synchronous admission endpoint. */
    SelectionSnapshotAdmission()noexcept=default;
    /** @summary Materialize immutable state before binding one actual original Context/attempt, under the verification lock. */
    virtual bool Seal(CapturedSelectionData&&)noexcept=0;
public:
    /** @summary End this borrowed endpoint after the same synchronous turn; no detached callback survives. */
    virtual ~SelectionSnapshotAdmission()=default;
    SelectionSnapshotAdmission(const SelectionSnapshotAdmission&)=delete;
    SelectionSnapshotAdmission& operator=(const SelectionSnapshotAdmission&)=delete;
};
/** @summary Retain original read/coordination custody and reuse the coordinator's complete-history verifier exactly once. */
class SelectionReadTurn final {
    friend class InstalledSelectionSession;
    friend class SelectionDataCapture;
    struct Body;
    /** @summary Bind original native roots/admission; this is not public portable-data authority. */
    SelectionReadTurn(StoreIO&,Parent&,const Observation&,const NativeAdmission&,metadata::Identifier&);
    /** @summary Capture bounded actual members under the same complete verification turn, releasing custody positively before success. */
    SelectionReadStatus Capture(const base_artifact::TrustedArtifact&,const std::map<std::string,std::string>&,
        std::string_view,const metadata::Compatibility&,SelectionSnapshotAdmission&)noexcept;
    /** @summary Run the existing finite coordinator and retain uncertain original release rather than retrying. */
    BaseReceipt Install(const BaseRequest&)noexcept;
    std::unique_ptr<Body> body_;
public:
    /** @summary End local native owners only; never replay, activate or delete package content. */
    ~SelectionReadTurn();
    SelectionReadTurn(const SelectionReadTurn&)=delete;
    SelectionReadTurn& operator=(const SelectionReadTurn&)=delete;
};
}
