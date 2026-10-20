#include "scripting/semantic_participant.hpp"
#include "scripting/script_runtime.hpp"

namespace sc13::scripting {
bool SemanticParticipantKey::operator==(const SemanticParticipantKey& other) const noexcept {
    return kind_==other.kind_ && script_==other.script_ && native_.owner.slot==other.native_.owner.slot &&
        native_.owner.generation==other.native_.owner.generation && native_.imageSha256==other.native_.imageSha256 &&
        native_.scopeGeneration==other.native_.scopeGeneration && native_.incarnation==other.native_.incarnation;
}
bool SemanticParticipant::InitializeScript(const ScriptRuntime& runtime) noexcept {
    if(key_.kind_!=SemanticParticipantKey::Kind::Unbound || !runtime.vmGeneration_)return false;
    key_.kind_=SemanticParticipantKey::Kind::ActualScriptVm;key_.script_=runtime.vmGeneration_;return true;
}
SemanticParticipant::SemanticParticipant(const mods::native::NativeParticipant& participant):native_(participant) {
    key_.kind_=SemanticParticipantKey::Kind::RetainedNativeModule;
    key_.native_=participant.ObserveLocalFence().identity;
}
bool SemanticParticipant::NativeLocallyLive() const noexcept { return native_ && native_->ObserveLocalFence().LocallyLive(); }
bool SemanticParticipant::ObserveOffGuardLifetime() const {
    if(!native_)return true;
    std::shared_ptr<runtime::RuntimeScope> scope;std::shared_ptr<runtime::RuntimeLifecycle> lifecycle;
    return native_->StageLifetime(scope,lifecycle);
}
bool SemanticParticipant::Matches(const mods::native::NativeParticipant& participant) const noexcept {
    const auto identity=participant.ObserveLocalFence().identity;
    return native_ && native_->SameIdentity(participant) && key_.native_.owner.slot==identity.owner.slot &&
        key_.native_.owner.generation==identity.owner.generation && key_.native_.imageSha256==identity.imageSha256 &&
        key_.native_.scopeGeneration==identity.scopeGeneration && key_.native_.incarnation==identity.incarnation;
}
}
