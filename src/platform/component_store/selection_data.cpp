/** @file @summary Data-only adapter over the existing complete-history capture turn; no duplicate verifier or runtime issuer. */
#include "platform/component_store/selection_data.hpp"
namespace sc13::platform::component_store {
SelectionDataCapture::SelectionDataCapture(StoreIO& io,Parent& root,const Observation& original,
    const NativeAdmission& admission,metadata::Identifier installation)
    :installation_(installation),turn_(io,root,original,admission,installation_){}
bool SelectionDataCapture::Seal(CapturedSelectionData&& data)noexcept{
    try{if(staged_)return false;auto candidate=std::make_shared<const CapturedSelectionData>(std::move(data));
        staged_=std::move(candidate);return true;}catch(...){return false;}
}
SelectionReadStatus SelectionDataCapture::Capture(const base_artifact::TrustedArtifact& trusted,
    const std::map<std::string,std::string>& executable,std::string_view version,
    const metadata::Compatibility& compatibility,std::shared_ptr<const CapturedSelectionData>& output)noexcept{
    if(attempted_||output)return SelectionReadStatus::Refused;attempted_=true;
    const auto result=turn_.Capture(trusted,executable,version,compatibility,*this);
    if(result==SelectionReadStatus::Accepted){if(!staged_)return SelectionReadStatus::Refused;output=staged_;}
    return result;
}
}
