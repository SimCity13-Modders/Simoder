#pragma once
#include "mods/native/native_participant.hpp"
#include <optional>

namespace sc13::scripting {
class SemanticBridge;
class ScriptRuntime;
/** @summary Scalar historical identity copied across the sole transport, never an authority or lifetime owner. */
class SemanticParticipantKey final {
public:
    /** @summary Explicitly distinguish absent, genuine VM and original native State identities. */
    enum class Kind : std::uint8_t { Unbound, ActualScriptVm, RetainedNativeModule };
    /** @summary Create an absent projection; no public scalar identity constructor exists. */
    SemanticParticipantKey() = default;
    /** @summary Read the participant category. @return Exact discriminant. */
    Kind Type() const noexcept { return kind_; }
    /** @summary Project only a genuine script identity. @return Original VM generation, otherwise zero. */
    std::uint64_t ScriptGeneration() const noexcept { return kind_==Kind::ActualScriptVm?script_:0; }
    /** @summary Compare the complete tagged scalar identity. @param other Historical projection. @return Full equality. */
    bool operator==(const SemanticParticipantKey& other) const noexcept;
private:
    Kind kind_{};
    std::uint64_t script_{};
    mods::native::NativeParticipantIdentity native_{};
    friend class SemanticParticipant;
};
/** @summary Immutable participant owns the actual native State or only the actual script's scalar identity. */
class SemanticParticipant final {
public:
    /** @summary Create an unpublished unbound participant. */
    SemanticParticipant() = default;
    /** @summary Borrow the sole scalar projection without acquiring an owner. @return Copied historical key. */
    SemanticParticipantKey Key() const noexcept { return key_; }
    /** @summary Project a genuine VM for script-only manager invariants. @return Original VM generation, otherwise zero. */
    std::uint64_t ScriptGeneration() const noexcept { return key_.ScriptGeneration(); }
    /** @summary Check native atomic local retirement only; external scope/lifecycle remain separate.
     * @return True for an actual locally live native State. */
    bool NativeLocallyLive() const noexcept;
    /** @summary Observe original native scope/lifecycle off guards; scripts have no native lifetime.
     * @return False only for an observed stopped original native lifetime; never an atomic authority proof. */
    bool ObserveOffGuardLifetime() const;
    /** @summary Compare a borrowed retained view with the exact original State.
     * @param participant Actual loader-issued retained State. @return Exact retained and scalar identity equality. */
    bool Matches(const mods::native::NativeParticipant& participant) const noexcept;
private:
    /** @summary Publish a scalar from the actual scheduler-reserved runtime under original B.
     * @param runtime Genuine VM in its SemanticLua constructor. @return False after any previous initialization. */
    bool InitializeScript(const ScriptRuntime& runtime) noexcept;
    /** @summary Retain only an actual loader-issued native participant, outside all guards.
     * @param participant Successful original State capture. */
    explicit SemanticParticipant(const mods::native::NativeParticipant& participant);
    SemanticParticipantKey key_;
    std::optional<mods::native::NativeParticipant> native_;
    friend class SemanticBridge;
};
}
