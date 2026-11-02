/** @file @summary Explicit Windows StoreIO adapter over qualified primitives; no runtime backend registry or POSIX substitute. */
#pragma once
#include "platform/component_store/windows_store.hpp"


/** @summary Preserve caller Windows macros while declaring portable operation names unchanged. */
#pragma push_macro("CreateFile")
#pragma push_macro("CreateDirectory")
#undef CreateFile
#undef CreateDirectory

namespace sc13::platform::component_store {
/** @summary Instance-bound native adapter; caller retains it through every original opaque owner lifetime. */
class WindowsStoreIO final:public StoreIO {
public:
    /** @summary Construct only inert adapter identity; no IO, token observation or backend effect starts. */
    WindowsStoreIO()noexcept=default;
    /** @summary End after all original owners; never delete installation state or manufacture cleanup. */
    ~WindowsStoreIO()override=default;
    [[nodiscard]] std::string_view BackendDescription()const noexcept override;
    [[nodiscard]] std::string_view PublicationDescription()const noexcept override;
    [[nodiscard]] bool PublicationAcknowledged(const Result&,PublicationRequirement)const noexcept override;
    Result CaptureAttachment(Parent&,const metadata::Identifier&,metadata::Attachment&)noexcept override;
    Result EncodeLocalEvidence(const EvidenceContext&,const Observation&,std::string&)const noexcept override;
    [[nodiscard]] bool ValidateHistoricalEvidence(std::string_view,const EvidenceContext&)const noexcept override;
    /** @summary Delegate only actual same-instance native ownership; foreign/partial outputs are never adopted. */
    Result OpenParent(std::string_view,Parent&,Observation&)noexcept override;
    Result Lock(Store&,Coordination&)noexcept override;
    /** @summary Release only same-instance original coordination after actual dependent closure. */
    Result ReleaseCoordination(Coordination&)noexcept override;
    Result CreateStore(Parent&,const Observation&,Store&,Coordination&,Observation&)noexcept override;
    Result OpenStore(Parent&,Store&,Observation&)noexcept override;
    Result ListDirectory(Store&,Coordination&,InventoryTarget,std::uint32_t,std::vector<NamedObservation>&)noexcept override;
    Result CreateDirectory(Store&,Coordination&,std::string_view,Observation&)noexcept override;
    Result OpenRead(Store&,Coordination&,std::string_view,std::uint64_t,File&,Observation&)noexcept override;
    Result Read(File&,std::span<std::byte>,std::size_t&)noexcept override;
    Result CreateFile(Store&,Coordination&,std::string_view,std::uint64_t,File&,Observation&)noexcept override;
    Result Write(File&,std::span<const std::byte>)noexcept override;
    Result Flush(File&)noexcept override;
    Result Close(File&)noexcept override;
    Result PublishRecord(Store&,Coordination&,File&,RecordTarget,PublicationRequirement)noexcept override;
    Result PublishGeneration(Store&,Coordination&,std::string_view,const Observation&,std::string_view,PublicationRequirement)noexcept override;
    Result PublishSelection(Store&,Coordination&,File&,const Observation*,std::span<const std::byte>,PublicationRequirement)noexcept override;
    Result RemoveCreatedDirectory(Store&,Coordination&,std::string_view,const Observation&)noexcept override;
private:
    /** @summary Native state is private and nonserializable; matching this tag is necessary but never installer authority. */
    template<class T>bool Owns(const T& value)const noexcept{return value.origin_==this&&static_cast<bool>(value.state_);}
    /** @summary A fresh out-owner cannot transfer a different native backend's live state. */
    template<class T>bool Empty(const T& value)const noexcept{return !value.origin_&&!value.state_;}
    /** @summary Bind even a partial/Unknown acquired native original; never discard its provenance to retry elsewhere. */
    template<class T>void Bind(T& value)const noexcept{if(value.state_)value.origin_=this;}
};
}

#pragma pop_macro("CreateDirectory")
#pragma pop_macro("CreateFile")
