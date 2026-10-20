#pragma once

#include "mods/native/native_module_loader.hpp"

namespace sc13::mods::native {
/** @summary Immutable original identity scalars; never an authorization or VM generation. */
struct NativeParticipantIdentity final {
    SdHandle owner{};
    std::array<unsigned char, 32> imageSha256{};
    std::uint64_t scopeGeneration{};
    std::uint32_t incarnation{};
};
/** @summary Coherent local lifetime observation, excluding scope/lifecycle/owner freshness. */
struct NativeParticipantFence final {
    NativeParticipantIdentity identity;
    ModulePhase phase{ModulePhase::Prepared};
    bool retired{};
    bool domainStopping{};
    /** @summary Reports local lifetime only, never whole-runtime validity. @return False after local/domain stop. */
    bool LocallyLive() const noexcept { return !retired && !domainStopping; }
};
/** @summary Retains the actual original native State, without exposing addresses or granting authority.
 * @pre Construct/copy/release outside publication guards; protected readers borrow an already retained view.
 * Scope/lifecycle/owner validity must independently use their original evaluators. */
class NativeParticipant final {
public:
    /** @summary Copies one original retained identity outside guards. @param other Original retained view. */
    NativeParticipant(const NativeParticipant& other) = default;
    /** @summary Replaces a retained view outside guards; rvalues copy so no empty participant is exposed.
     * @param other Original retained view. @return This retained view. */
    NativeParticipant& operator=(const NativeParticipant& other) = default;
    /** @summary Copies immutable scalars and atomic local retirement/domain facts without locks or allocation.
     * @return Local lifetime observation only, not scope/lifecycle/permission freshness. */
    NativeParticipantFence ObserveLocalFence() const noexcept;
    /** @summary Compares exact original retained State and immutable native identity.
     * @param other Already retained original participant. @return True only for the same State. */
    bool SameIdentity(const NativeParticipant& other) const noexcept;
private:
    /** @summary Wraps only a successfully prepared original State outside guards.
     * @param state Exact process-retained loader generation. */
    explicit NativeParticipant(std::shared_ptr<NativeModule::State> state) noexcept;
    std::shared_ptr<NativeModule::State> state_;
    friend class NativeModule;
};
}
