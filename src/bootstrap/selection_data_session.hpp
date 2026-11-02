/** @file @summary Original Context-owned DATA-only installed selection verification; never a runtime witness. */
#pragma once
#include "platform/component_store/selection_data.hpp"
#include "platform/component_store/windows_store_io.hpp"
#include "platform/runtime_owner.hpp"
#include "runtime/runtime_scope.hpp"
#include <atomic>
namespace sc13::bootstrap {
class RuntimeContext;
}
namespace sc13::scripting {class BaseHost;struct BasePackagePin;struct ScriptDefinition;}
namespace sc13::bootstrap {
/** @summary Native session retains same IO/root/admission/owner through every uncertain turn; no Context backedge. */
class SelectionDataSession final:private platform::component_store::NativeAdmission {
    friend class RuntimeContext;
    friend class scripting::BaseHost;
    using Data=platform::component_store::CapturedSelectionData;
    using Status=platform::component_store::SelectionReadStatus;
    std::shared_ptr<platform::RuntimeOwner> owner_;
    std::shared_ptr<runtime::RuntimeCommitDomain> domain_;
    std::shared_ptr<runtime::RuntimeScope> scope_;
    std::shared_ptr<const platform::component_store::NativeAdmission> admission_;
    platform::component_store::WindowsStoreIO io_;
    platform::component_store::Parent root_;
    platform::component_store::Observation observed_;
    platform::component_store::metadata::Identifier installation_;
    std::uint32_t thread_{};std::uint64_t generation_{},epoch_{};
    bool attempted_{};
    std::unique_ptr<platform::component_store::SelectionDataCapture> turn_;
    std::shared_ptr<const Data> captured_;
    /** @summary Compile only original privately retained complete captured bytes using independently supplied native executable pins. */
    scripting::ScriptDefinition Compile(const scripting::BasePackagePin&)const;
    /** @summary Bind only original acquired Context identities and independently retained native admission. */
    SelectionDataSession(std::shared_ptr<platform::RuntimeOwner>,std::shared_ptr<runtime::RuntimeCommitDomain>,
        std::shared_ptr<runtime::RuntimeScope>,std::shared_ptr<const platform::component_store::NativeAdmission>,std::uint32_t);
    /** @summary Validate native identities and original offguard turn; no grants/readiness are inferred. */
    bool Current(const platform::component_store::Parent&,const platform::component_store::Observation&,
        const platform::component_store::metadata::Identifier&,const platform::component_store::metadata::Component*,
        const platform::base_artifact::TrustedArtifact*)const noexcept override;
    /** @summary One complete DATA capture; same retained turn survives Unknown and output only follows positive release. */
    Status Capture(std::string_view,const platform::component_store::metadata::Identifier&,
        const platform::base_artifact::TrustedArtifact&,const std::map<std::string,std::string>&,
        std::string_view,const platform::component_store::metadata::Compatibility&,std::shared_ptr<const Data>&)noexcept;
public:
    /** @summary Release original local custody after original Context lifetime; never replay or delete. */
    ~SelectionDataSession()override=default;
};
}
