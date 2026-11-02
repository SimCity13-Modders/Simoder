/** @file @summary Actual acquisition owner-state isolation at existing Selected cut, no UI/filesystem/network/VM effects. */
#include "bootstrap/base_dependency.hpp"
#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace sc13::bootstrap {
/** @summary Test-only actual original cut observation; no constructor/permission/success is manufactured. */
struct BaseDependencyTestAccess final {
    struct Observation final {DWORD owner{};bool foreignRefused{},foreignBusy{},foreignGenerationZero{},foreignPumpRefused{},foreignReleaseRefused{},foreignCaptureEmpty{},ownerUnchanged{},foreignReachedCut{};unsigned cuts{};std::exception_ptr failure;};
    inline static Observation* observed{};
    inline static const BaseDependencySelection* selection{};
    /** @summary Keep actual native caller data alive synchronously and stop before every Parents/Console/network effect. */
    static void Arm(BaseDependency& controller,const BaseDependencySelection& selected,Observation& output)noexcept{
        observed=&output;selection=&selected;output.owner=GetCurrentThreadId();
        controller.beginCut_=[](BaseDependency& original,BaseDependency::BeginCut cut)noexcept{
            if(cut!=BaseDependency::BeginCut::Selected)return;
            auto& value=*observed;
            // Even if a future regression accepts a foreign Begin, cancel there without recursive thread creation or OS effects.
            if(GetCurrentThreadId()!=value.owner){value.foreignReachedCut=true;original.RequestCancel();return;}
            ++value.cuts;const auto before=original.Error();
            try{
                std::jthread foreign([&]{
                    value.foreignRefused=!original.Begin(*selection,{});
                    value.foreignBusy=original.Error()==BaseDependencyError::Busy;
                    value.foreignGenerationZero=original.Generation()==0;
                    value.foreignPumpRefused=original.Pump()==BaseDependencyState::Unresolved;
                    value.foreignReleaseRefused=!original.Release();
                    value.foreignCaptureEmpty=!original.Capture().Get();
                });foreign.join();value.ownerUnchanged=original.Error()==before;
                std::jthread cancellation([&]{original.RequestCancel();});cancellation.join();
            }catch(...){value.failure=std::current_exception();original.RequestCancel();}
        };
    }
    /** @summary Clear borrowed diagnostic pointers only after synchronous Begin/joins ended. */
    static void Disarm(BaseDependency& original)noexcept{original.beginCut_=nullptr;observed=nullptr;selection=nullptr;}
};
}
/** @summary Assert actual state/isolation behavior, never injected statuses. */
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
/** @summary Real foreign rejection cannot corrupt owner diagnostics; real atomic cancellation remains cross-thread supported. */
int main(){try{using namespace sc13::bootstrap;namespace artifact=sc13::platform::base_artifact;
    std::array<artifact::FilePin,3> files{{{"mod.toml",1,{},false},{"scripts/init.luau",1,{},false},{"LICENSE",1,{},true}}};
    BaseDependencySelection selected{{2048,{},files},L"0.1.0",L"isolated",L"owned-fixture",L"original thread-entry isolation",{}};
    auto& logger=sc13::logging::AsyncLogger::Instance();
    for(int iteration=0;iteration<16;++iteration){
        BaseDependency original(logger);BaseDependencySelection invalid{};
        Check(!original.Begin(invalid,{})&&original.Error()==BaseDependencyError::InvalidSelection,"prime actual owner diagnostic");
        BaseDependencyTestAccess::Observation observed;BaseDependencyTestAccess::Arm(original,selected,observed);
        const bool admitted=original.Begin(selected,{});BaseDependencyTestAccess::Disarm(original);
        if(observed.failure)std::rethrow_exception(observed.failure);
        Check(!admitted&&observed.cuts==1&&!observed.foreignReachedCut,"only real original Selected cut, foreign Begin never progresses");
        Check(observed.foreignRefused&&observed.foreignBusy&&observed.foreignGenerationZero&&observed.foreignPumpRefused&&observed.foreignReleaseRefused&&observed.foreignCaptureEmpty,"all foreign calls refuse without original owner access");
        Check(observed.ownerUnchanged,"foreign rejection did not mutate actual owner's pre-existing diagnostic");
        Check(original.Error()==BaseDependencyError::Cancelled&&original.Generation()==0&&original.Pump()==BaseDependencyState::Vacant&&!original.Capture().Get()&&original.Release(),"real atomic foreign cancellation observed, no body/capture/effects admitted");
    }
    std::cout<<"PASS actual thread-entry ownership isolation16cases; console/network/staging/VM/fatal NOT_RUN\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
