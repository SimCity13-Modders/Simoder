/** @file @summary Native caller-owned Base installation coordination; never an installer-consent or package-trust issuer. */
#pragma once
#include "platform/component_store/metadata.hpp"
#include "platform/component_store/store_io.hpp"
#include "platform/base_artifact_reader.hpp"
#include <memory>
#include <optional>

namespace sc13::platform::component_store {
/** @summary Borrow the same independently admitted native maintenance scope through synchronous return.
 * @remarks Production callers must supply their actual offguard native owner/choice and independently trusted
 * artifact selection. No production UI/issuer is implemented here. A deserialized bool, matching installation
 * string, manifest/signature or this abstract interface alone is not admission. Arbitrary same-user native code
 * is outside the protection claim. Isolated test callers explicitly own their approved disposable scope. */
class NativeAdmission {
public:
    /** @summary End the caller's own scope after every original operation has returned; no filesystem cleanup is implied. */
    virtual ~NativeAdmission()=default;
    /** @summary Verify this exact retained original root, logical installation and native-selected content remain admitted.
     * @param desired Null only for read-only inspection, never a wildcard maintenance scope.
     * @param independentlyTrusted Null only for inspection; never obtained solely from the metadata being authenticated.
     * @return Actual current native/offguard admission; false closes further effects without pretending previous effects rolled back. */
    [[nodiscard]] virtual bool Current(const Parent& root,const Observation& originalRoot,
        const metadata::Identifier& installation,const metadata::Component* desired,
        const base_artifact::TrustedArtifact* independentlyTrusted)const noexcept=0;
};
/** @summary Bounded operation receipt describes file selection only, never Base readiness or native execution. */
enum class InstallStatus {Refused,Busy,Committed,AlreadySelected,Interrupted,Unknown,Unsupported};
/** @summary Exact returned data does not authorize retry, rollback, deletion, loading or client changes. */
struct BaseReceipt final {
    InstallStatus status{InstallStatus::Refused};Result last;metadata::Error metadataError;
    bool preparingSelectionPublished{},acceptedSelectionPublished{};
    metadata::Identifier transaction;std::optional<metadata::Digest> plan,state;
    std::optional<metadata::JournalReference> journal;
};
/** @summary Original immutable inputs are retained unchanged by the native caller until ExecuteBase returns. */
struct BaseRequest final {
    StoreIO& io;Parent& root;const Observation& originalRoot;const NativeAdmission& admission;
    metadata::Identifier installation,transaction;const metadata::Component& desired;
    const base_artifact::TrustedArtifact& independentlyTrusted;
    std::shared_ptr<const base_artifact::Snapshot> captured;
    PublicationRequirement publication{PublicationRequirement::DurableNamespace};
};
/** @summary Read-only original selector/state/attachment outcome; missing/invalid/preparing is not automatically adopted or recovered. */
struct BaseInspection final {
    InstallStatus status{InstallStatus::Refused};Result last;metadata::Error metadataError;
    std::optional<metadata::Selector> selector;std::optional<metadata::State> state;
    std::optional<metadata::Attachment> attachment;
};
/** @summary One finite native Base-only installation/update using genuine captured archive bytes and independent pins.
 * @remarks MCP package capture is explicitly unsupported before effects in steps1-3; it belongs to the separate
 * step4 profile. Existing package generations are never mutated/deleted. No removal, reattachment, automatic
 * recovery, activation, generic privileged path, credential store or T1/Root5 implementation is provided.
 * Unknown publication preserves evidence and is never promoted through convenient readback. */
[[nodiscard]] BaseReceipt ExecuteBase(const BaseRequest& request)noexcept;
/** @summary Inspect bounded existing state under the same lock and attachment/current native scope; no create/write/replay. */
[[nodiscard]] BaseInspection InspectBase(StoreIO& io,Parent& root,const Observation& originalRoot,
    const NativeAdmission& admission,const metadata::Identifier& installation)noexcept;
}
