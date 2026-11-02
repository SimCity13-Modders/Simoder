/** @file @summary Real Context-owned native selection DATA session, independent of SDK/VM/manager readiness. */
#include "bootstrap/selection_data_session.hpp"
#include "scripting/script_runtime.hpp"
#include "scripting/base_host.hpp"
#include "scripting/base_package_compile.hpp"
#include <stdexcept>
#include <Windows.h>
namespace sc13::bootstrap {
namespace io=platform::component_store;
SelectionDataSession::SelectionDataSession(std::shared_ptr<platform::RuntimeOwner> owner,
    std::shared_ptr<runtime::RuntimeCommitDomain> domain,std::shared_ptr<runtime::RuntimeScope> scope,
    std::shared_ptr<const io::NativeAdmission> admission,std::uint32_t thread)
    :owner_(std::move(owner)),domain_(std::move(domain)),scope_(std::move(scope)),admission_(std::move(admission)),thread_(thread){
    if(!owner_||!owner_->IsAcquired()||!domain_||!scope_||!admission_)throw std::invalid_argument("Original selection DATA owner missing");
    const auto snapshot=scope_->Current();generation_=snapshot.generation;epoch_=snapshot.epoch;
}
bool SelectionDataSession::Current(const io::Parent& root,const io::Observation& observed,
    const io::metadata::Identifier& installation,const io::metadata::Component* desired,
    const platform::base_artifact::TrustedArtifact* trusted)const noexcept{try{
    if(GetCurrentThreadId()!=thread_||!owner_->IsAcquired()||domain_->IsStopping()||
        runtime::RuntimeCommitDomain::AnyWriterActiveOnCurrentThread()||scripting::ScriptRuntime::OnScriptLane()||
        &root!=&root_||observed.identity!=observed_.identity||installation!=installation_)return false;
    const auto snapshot=scope_->Current();return !snapshot.stopping&&snapshot.generation==generation_&&snapshot.epoch==epoch_&&
        admission_->Current(root,observed,installation,desired,trusted);
}catch(...){return false;}}
SelectionDataSession::Status SelectionDataSession::Capture(std::string_view path,const io::metadata::Identifier& installation,
    const platform::base_artifact::TrustedArtifact& trusted,const std::map<std::string,std::string>& pins,
    std::string_view version,const io::metadata::Compatibility& compatibility,std::shared_ptr<const Data>& output)noexcept{try{
    if(attempted_||output||GetCurrentThreadId()!=thread_||domain_->IsStopping()||
        runtime::RuntimeCommitDomain::AnyWriterActiveOnCurrentThread()||scripting::ScriptRuntime::OnScriptLane())return Status::Refused;
    attempted_=true;installation_=installation;
    const auto opened=io_.OpenParent(path,root_,observed_);
    if(opened.status!=io::Status::Ok)return opened.status==io::Status::Busy?Status::Busy:opened.status==io::Status::Unknown?Status::Unknown:Status::Refused;
    if(!Current(root_,observed_,installation_,nullptr,nullptr))return Status::Refused;
    turn_=std::make_unique<io::SelectionDataCapture>(io_,root_,observed_,static_cast<const io::NativeAdmission&>(*this),installation_);
    const auto result=turn_->Capture(trusted,pins,version,compatibility,output);
    if(result==Status::Accepted)captured_=output;return result;
}catch(...){return Status::Refused;}}
}

namespace sc13::bootstrap {
scripting::ScriptDefinition SelectionDataSession::Compile(const scripting::BasePackagePin& pin)const{
    if(!captured_||!Current(root_,observed_,installation_,nullptr,nullptr)||pin.version!=captured_->component.version)
        throw std::runtime_error("Original captured Base construction refused");
    std::map<std::string,std::string> executable;
    for(const auto& [name,bytes]:captured_->members)if(name!="LICENSE")executable.emplace(name,bytes);
    auto result=scripting::CompileCapturedBasePackage(executable,pin);
    if(!Current(root_,observed_,installation_,nullptr,nullptr))throw std::runtime_error("Original captured Base retired during compilation");
    return result;
}
}
