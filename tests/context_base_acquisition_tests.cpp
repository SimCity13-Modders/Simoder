/** @file @summary Genuine Context-owned acquisition cancellation/entry checks, all stopped before Parents/Console/network effects. */
#include "bootstrap/runtime_context.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

/** @summary Independent actual compiler layout observations from three separately compiled test-only TUs. */
extern "C" std::array<std::size_t,4> sc13_acquisition_layout_production()noexcept;
extern "C" std::array<std::size_t,4> sc13_acquisition_layout_base()noexcept;
extern "C" std::array<std::size_t,4> sc13_acquisition_layout_auth()noexcept;
namespace sc13::bootstrap {
/** @summary Observe only the original private TEST cut; no fake captured bytes or successful acquisition. */
struct BaseDependencyTestAccess final {
    inline static unsigned selected{};
    inline static RuntimeContext* context{};
    inline static bool domainOnly{};
    inline static std::shared_ptr<RuntimeContext> retained;
    inline static std::exception_ptr failure;
    inline static bool gap{},insideArmRefused{},insideDisarmRefused{};
    /** @summary Compare the genuine privately retained domain identity, never inject a domain. */
    static bool Bound(const BaseDependency& original,const std::shared_ptr<runtime::RuntimeCommitDomain>& domain)noexcept{return original.originalDomain_==domain;}
    /** @summary Always cancel before actual Parents; selected mode proves real domain-only or Context-stop cancellation. */
    static bool Arm(BaseDependency& original,RuntimeContext& owner,bool onlyDomain,std::shared_ptr<RuntimeContext> keep={})noexcept{
        const bool installed=BaseDependency::ArmBeginCutForTest(original,[](BaseDependency& body,BaseDependency::BeginCut cut)noexcept{
            if(gap&&cut==BaseDependency::BeginCut::ContextChecked){context->RequestStopIntent(runtime::StopReason::ExplicitShutdown);return;}
            if(cut!=BaseDependency::BeginCut::Selected)return;++selected;
            insideArmRefused=!BaseDependency::ArmBeginCutForTest(body,[](BaseDependency&,BaseDependency::BeginCut)noexcept{});
            insideDisarmRefused=!BaseDependency::DisarmBeginCutForTest(body);
            try{
                if(domainOnly)context->Domain()->RequestStop();
                else if(retained){auto same=retained;std::jthread stop([same]{same->RequestStopIntent(runtime::StopReason::ExplicitShutdown);});stop.join();}
                else body.RequestCancel();
            }catch(...){failure=std::current_exception();body.RequestCancel();}
        });
        if(installed){selected=0;context=&owner;domainOnly=onlyDomain;retained=std::move(keep);failure={};gap=false;}
        return installed;
    }
    /** @summary Install a no-op TEST observation only if the actual same-thread slot is vacant. */
    static bool ArmEmpty(BaseDependency& owner)noexcept{return BaseDependency::ArmBeginCutForTest(owner,[](BaseDependency&,BaseDependency::BeginCut)noexcept{});}
    /** @summary Observe actual owner-matched removal without touching another owner's fixture metadata. */
    static bool ClearEmpty(BaseDependency& owner)noexcept{return BaseDependency::DisarmBeginCutForTest(owner);}
    /** @summary End all borrowed fixture pointers after synchronous Begin and joined stop callers. */
    static bool Disarm(BaseDependency& original)noexcept{const bool closed=BaseDependency::DisarmBeginCutForTest(original);if(closed){context=nullptr;retained.reset();}return closed;}
};
/** @summary Inspect original private ownership/cut counters; never supply a captured reader or issuer. */
struct ContextBaseAcquisitionTestAccess final {
    /** @summary Verify SAME retained native domain is used by actual controller. */
    static bool Bound(const RuntimeContext& context)noexcept{return BaseDependencyTestAccess::Bound(context.baseAcquisition_,context.domain_);}
    /** @summary Borrow only within this original retained Context's synchronous test. */
    static BaseDependency& Controller(RuntimeContext& context)noexcept{return context.baseAcquisition_;}
    /** @summary Stop exactly between the wrapper precheck and actual controller Begin; before its epoch baseline is captured. */
    static bool ArmGap(RuntimeContext& context)noexcept{
        if(BaseDependencyTestAccess::context!=&context)return false;BaseDependencyTestAccess::gap=true;return true;
    }
};
}
namespace {
using namespace sc13;
using namespace bootstrap;
unsigned checks{},serial{};
/** @summary Fail one actual behavioral observation without retries. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Actual restricted TEST owner uses the production named-object acquisition/security algorithm. */
auto Context(){auto owner=platform::RuntimeOwnerTestAccess::Acquire(L"Global\\Simoder.TEST.U1.ContextAcquisition."+
    std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++serial));
    Check(owner&&owner->IsAcquired(),"actual acquired isolated owner");return std::make_shared<RuntimeContext>(std::move(owner));}
/** @summary Native test descriptor DATA only; Selected always cancels before any filesystem/UI/network effects. */
auto Selection(std::array<platform::base_artifact::FilePin,3>& files){
    return BaseDependencySelection{{2048,{},files},L"0.1.0",L"isolated",L"owned-fixture",L"Context acquisition entry/cancel test",{}};
}
/** @summary No replacement, foreign publication, stale-owner callback or object-profile layout dependence is permitted. */
void Registration(){
    Check(sc13_acquisition_layout_production()==sc13_acquisition_layout_base(),"macro-free and BASE_TEST object layouts match");
    Check(sc13_acquisition_layout_production()==sc13_acquisition_layout_auth(),"macro-free and AUTH object layouts match");
    auto& logger=logging::AsyncLogger::Instance();BaseDependency first(logger),second(logger);
    Check(BaseDependencyTestAccess::ArmEmpty(first),"vacant TLS registration accepts original owner");
    Check(!BaseDependencyTestAccess::ArmEmpty(second)&&!BaseDependencyTestAccess::ClearEmpty(second),"another owner cannot replace/remove registration");
    bool foreignArm{},foreignClear{};std::jthread foreign([&]{foreignArm=BaseDependencyTestAccess::ArmEmpty(first);foreignClear=BaseDependencyTestAccess::ClearEmpty(first);});foreign.join();
    Check(!foreignArm&&!foreignClear,"foreign thread cannot register/remove original owner");
    Check(BaseDependencyTestAccess::ClearEmpty(first)&&BaseDependencyTestAccess::ArmEmpty(second)&&BaseDependencyTestAccess::ClearEmpty(second),"quiescent same-owner removal permits next distinct original");
    {BaseDependency temporary(logger);Check(BaseDependencyTestAccess::ArmEmpty(temporary),"actual temporary owner registered");}
    Check(BaseDependencyTestAccess::ArmEmpty(first)&&BaseDependencyTestAccess::ClearEmpty(first),"real owner destruction removed original TLS registration before address reuse");
}
/** @summary Exercise unchanged invalid-input path and prove guard refusal cannot overwrite its actual diagnostic. */
void Entries(const BaseDependencySelection& selected){
    bool missing{};try{RuntimeContext invalid({});}catch(const std::invalid_argument&){missing=true;}
    Check(missing,"missing actual owner rejected");auto context=Context();
    Check(ContextBaseAcquisitionTestAccess::Bound(*context),"controller retains exactly original Context domain");
    Check(!context->BeginBaseAcquisition({},{}),"invalid independent descriptor rejected");
    Check(context->BaseAcquisitionError()==BaseDependencyError::InvalidSelection,"original owner diagnostic");
    auto& controller=ContextBaseAcquisitionTestAccess::Controller(*context);
    Check(BaseDependencyTestAccess::Arm(controller,*context,false),"same actual owner registration");
    const auto denied=[&]{
        Check(!context->BeginBaseAcquisition(selected,{}),"guarded Begin refused");
        Check(context->PumpBaseAcquisition()==BaseDependencyState::Unresolved,"guarded Pump refused");
        Check(!context->CaptureBaseAcquisition().Get(),"guarded capture empty");
        Check(!context->ReleaseBaseAcquisition(),"guarded release refused");
        Check(context->BaseAcquisitionError()==BaseDependencyError::Busy,"guarded error refusal is not owner diagnostic mutation");
    };
    {auto writer=context->Domain()->ReserveWriter();Check(writer.Acquired(),"actual same-domain writer");denied();}
    {runtime::RuntimeCommitDomain foreign;auto writer=foreign.ReserveWriter();Check(writer.Acquired(),"actual foreign-domain writer");denied();
        auto nested=context->Domain()->ReserveWriter();Check(nested.Acquired(),"actual nested original-domain writer");denied();}
    Check(BaseDependencyTestAccess::selected==0,"guarded entry never reached actual Selected cut");
    Check(context->BaseAcquisitionError()==BaseDependencyError::InvalidSelection,"guard refusal preserved original diagnostic");
    std::exception_ptr failure;std::jthread thread([&]{try{denied();}catch(...){failure=std::current_exception();}});thread.join();if(failure)std::rethrow_exception(failure);
    Check(BaseDependencyTestAccess::selected==0,"foreign entry never reached Selected");
    Check(context->BaseAcquisitionError()==BaseDependencyError::InvalidSelection,"foreign entry preserved original diagnostic");
    Check(!context->BeginBaseAcquisition(selected,{})&&BaseDependencyTestAccess::selected==1,"same original attempt remains available and reaches genuine Selected");
    Check(BaseDependencyTestAccess::Disarm(controller),"same quiescent registration removed");
    Check(context->BaseAcquisitionError()==BaseDependencyError::Cancelled,"actual callback cancellation observed");
    Check(BaseDependencyTestAccess::insideArmRefused&&BaseDependencyTestAccess::insideDisarmRefused,"active callback cannot replace/remove registration");
    Check(context->PumpBaseAcquisition()==BaseDependencyState::Vacant&&context->ReleaseBaseAcquisition(),"original no-effect slot remains vacant/releasable");
}
/** @summary Real monotonic domain closes both the pre-Begin epoch race and existing post-selection copy cut. */
void Stops(const BaseDependencySelection& selected){
    {auto context=Context();context->RequestStopIntent(runtime::StopReason::ExplicitShutdown);
        Check(!context->BeginBaseAcquisition(selected,{})&&!context->CaptureBaseAcquisition().Get(),"stop before admission refuses new work/data");
        Check(context->PumpBaseAcquisition()==BaseDependencyState::Vacant&&context->ReleaseBaseAcquisition(),"cleanup remains callable after stop");}
    {auto context=Context();auto& body=ContextBaseAcquisitionTestAccess::Controller(*context);
        Check(BaseDependencyTestAccess::Arm(body,*context,false),"same actual owner registration");Check(ContextBaseAcquisitionTestAccess::ArmGap(*context),"gap observer bound to same actual registered Context");
        Check(!context->BeginBaseAcquisition(selected,{})&&BaseDependencyTestAccess::selected==0,"stop between wrapper check and Begin cannot become a new epoch baseline");
        Check(BaseDependencyTestAccess::Disarm(body),"same quiescent registration removed");Check(context->BaseAcquisitionError()==BaseDependencyError::Cancelled,"original domain refusal classified Cancelled");}
    for(const bool domainOnly:{true,false}){auto context=Context();auto& body=ContextBaseAcquisitionTestAccess::Controller(*context);
        Check(BaseDependencyTestAccess::Arm(body,*context,domainOnly,domainOnly?std::shared_ptr<RuntimeContext>{}:context),"same actual owner registration");
        const auto admitted=context->BeginBaseAcquisition(selected,{});Check(BaseDependencyTestAccess::Disarm(body),"same quiescent registration removed");
        if(BaseDependencyTestAccess::failure)std::rethrow_exception(BaseDependencyTestAccess::failure);
        Check(!admitted&&BaseDependencyTestAccess::selected==1,"real Selected cut stopped before Parents");
        Check(context->BaseAcquisitionError()==BaseDependencyError::Cancelled,"domain-only or joined Context stop cancels original body");
        Check(context->PumpBaseAcquisition()==BaseDependencyState::Vacant&&context->ReleaseBaseAcquisition()&&!context->CaptureBaseAcquisition().Get(),"same original cleanup callable after domain stop");
    }
}
}
/** @summary Qualify only real Context/owner and preeffect cancellation; readers, network, VM, lane and CloseUnknown remain unqualified. */
int main(){try{std::array<platform::base_artifact::FilePin,3> files{{{"mod.toml",1,{},false},{"scripts/init.luau",1,{},false},{"LICENSE",1,{},true}}};
    const auto selected=Selection(files);Registration();Entries(selected);Stops(selected);
    std::cout<<"Context acquisition preeffect checks passed: "<<checks<<"; console/network/readers/VM/lane/CloseUnknown NOT_RUN\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
