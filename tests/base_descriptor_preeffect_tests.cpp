/** @file @summary Real acquisition selection refuses invalid maps before any native Selected/Parents/console/network effect. */
#include "bootstrap/base_dependency.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace sc13::bootstrap {
/** @summary Observe only actual original preeffect selection; cancellation prevents all fixture UI/filesystem/network effects even on validation regression. */
struct BaseDependencyTestAccess final {
    inline static unsigned selected{};
    /** @summary Install only existing passive Selected observation/cancellation; it never approves input or fabricates a result. */
    static void Arm(BaseDependency& controller)noexcept{controller.beginCut_=[](BaseDependency& original,BaseDependency::BeginCut cut)noexcept{if(cut==BaseDependency::BeginCut::Selected){++selected;original.RequestCancel();}};}
};
}
/** @summary Require actual original selection status and diagnostic count, no synthesized success. */
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
/** @summary All descriptors are DATA and all accepted selections are cancelled at actual existing Selected cut. */
int main(){try{namespace artifact=sc13::platform::base_artifact;using namespace sc13::bootstrap;
    std::array<artifact::FilePin,3> files{{{"mod.toml",1,{},false},{"scripts/init.luau",1,{},false},{"LICENSE",1,{},true}}};
    BaseDependencySelection selection{{2048,{},files},L"0.1.0",L"isolated",L"owned-fixture",L"strict descriptor shape",{}};
    auto& logger=sc13::logging::AsyncLogger::Instance();
    const auto attempt=[&](bool valid){BaseDependency original(logger);BaseDependencyTestAccess::Arm(original);const auto before=BaseDependencyTestAccess::selected;
        Check(!original.Begin(selection,{}),"fixture never progresses to effects");
        Check(original.Error()==(valid?BaseDependencyError::Cancelled:BaseDependencyError::InvalidSelection),"actual strict selection disposition");
        Check(BaseDependencyTestAccess::selected==before+(valid?1U:0U)&&original.Generation()==0&&original.Pump()==BaseDependencyState::Vacant&&!original.Capture().Get()&&original.Release(),"actual preeffect boundary and empty original custody");};
    attempt(true);std::vector<artifact::FilePin> map(files.begin(),files.end());map.push_back(files[1]);selection.artifact.files=map;attempt(false);
    map.back().path="scripts/init.luau/nested.luau";attempt(false);map.back().path="scripts/init.luau-extra.luau";attempt(true);
    map.back().path="scripts/../escape.luau";attempt(false);map.back().path="scripts/aux.luau";attempt(false);
    selection.artifact.files=files;files[1].path="scripts/not-entry.luau";attempt(false);files[1].path="scripts/init.luau";files[2].size=65537;attempt(false);
    std::cout<<"PASS actual descriptor acquisition preeffect; console/network/staging NOT_RUN\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
