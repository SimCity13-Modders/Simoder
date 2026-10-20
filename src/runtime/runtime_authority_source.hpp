#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <variant>

namespace sc13::runtime {
class RuntimeScope;
/** @summary Missing qualified ingress; never a permissive ordinary-role default. */
struct UnqualifiedOrigin final { bool operator==(const UnqualifiedOrigin&) const = default; };
/** @summary Original script owner and immutable loaded-content correlation. */
struct OrdinaryScriptOrigin final {
    std::uint64_t owner{},content{};
    bool operator==(const OrdinaryScriptOrigin&) const = default;
};
/** @summary Distinct native provenance scalars; not a State factory or permission proof.
 * @remarks No native source constructor exists in C0; every attempted native review remains unavailable. */
struct OrdinaryNativeOrigin final {
    std::uint64_t owner{},content{};
    std::uint32_t nativeOwnerSlot{},nativeOwnerGeneration{},incarnation{};
    std::array<unsigned char,32> imageSha256{};
    bool operator==(const OrdinaryNativeOrigin&) const = default;
};
/** @summary Authenticated agent causality remains distinct from the executing participant category. */
struct AgentOrigin final {
    std::uint64_t owner{},content{},actor{},channel{},consentSession{},entity{},extent{};
    bool operator==(const AgentOrigin&) const = default;
};
/** @summary Internal copied provenance; a scalar alternative alone never establishes a qualified source. */
using OperationOrigin=std::variant<UnqualifiedOrigin,OrdinaryScriptOrigin,AgentOrigin,OrdinaryNativeOrigin>;

/** @summary Private immutable source/proof boundary for the single RuntimeScope algorithm/store.
 * @remarks Production cannot construct this source or its decision/actor/control proofs in C0.
 * A future actual issuer requires its separately audited producer, not a boolean availability setter. */
class RuntimeAuthoritySource final {
public:
    /** @summary Copy only an already existing private source; copying does not install it in any scope.
     * @param other Original immutable source. */
    RuntimeAuthoritySource(const RuntimeAuthoritySource& other)=default;
    /** @summary Copy an existing immutable source into unpublished storage only.
     * @param other Existing source. @return This copy, never an issued grant. */
    RuntimeAuthoritySource& operator=(const RuntimeAuthoritySource& other)=default;
private:
    /** @summary Pending immutable review data; production callers cannot construct this private type. */
    struct Review final {
        OperationOrigin origin;
        std::uint64_t requested{1},required{1},approved{1},revision{1},action{1};
        std::uint32_t opcode{1},readiness{1};
        bool supportsRestricted{},sessionAllowed{},entityAllowed{},mandatoryAllowed{true},allParticipants{};
        std::chrono::steady_clock::time_point expires{std::chrono::steady_clock::now()+std::chrono::minutes(1)};
        bool operator==(const Review&) const = default;
    };
    /** @summary Non-authorizing reference to one original bounded pending review. */
    struct Challenge final {std::uint64_t owner{},serial{};};
    /** @summary Inspection never bypasses the original effective-capability evaluator. */
    struct PermissionView final {std::uint64_t capabilities{};bool restricted{},admissionClosed{};};
    /** @summary One source-bound local-action input; exact pending review is still consumed once by Scope. */
    class Decision final {
        /** @summary An empty value is not a local-action proof. */
        Decision()=delete;
        std::uint64_t owner_{},content_{};
        Challenge challenge_;
        Review review_;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        /** @summary Test-local action driver only, unavailable in production.
         * @param source Existing isolated source. @param challenge Original pending reference. @param review Exact reviewed input. */
        Decision(const RuntimeAuthoritySource& source,Challenge challenge,const Review& review)
            :owner_(source.owner_),content_(source.content_),challenge_(challenge),review_(review){}
        friend struct RuntimeScopeTestAccess;
#endif
        friend class RuntimeScope;
    };
    /** @summary Source-bound authenticated join input; arbitrary public actor/channel integers are not proof. */
    class Participant final {
        /** @summary An empty value is not an authenticated participant proof. */
        Participant()=delete;
        std::uint64_t owner_{},content_{},actor_{},channel_{};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        /** @summary Test-local verifier input, never a production authenticated actor.
         * @param source Existing fixture source. @param actor Fixture actor. @param channel Fixture channel. */
        Participant(const RuntimeAuthoritySource& source,std::uint64_t actor,std::uint64_t channel)
            :owner_(source.owner_),content_(source.content_),actor_(actor),channel_(channel){}
        friend struct RuntimeScopeTestAccess;
#endif
        friend class RuntimeScope;
    };
    /** @summary Retained source-bound revoke/disconnect instruction, never an authority upgrade. */
    class Control final {
        /** @summary An empty value cannot select an original source to revoke. */
        Control()=delete;
        std::uint64_t owner_{},content_{},subject_{};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        /** @summary Test-local revocation driver only.
         * @param source Existing fixture source. @param subject Exact grant/session/channel reference. */
        Control(const RuntimeAuthoritySource& source,std::uint64_t subject)
            :owner_(source.owner_),content_(source.content_),subject_(subject){}
        friend struct RuntimeScopeTestAccess;
#endif
        friend class RuntimeScope;
    };
    std::uint64_t owner_{},content_{};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    /** @summary Explicit fixture-only source construction; never used by a production scope.
     * @param owner Original fixture scope generation. @param content Isolated immutable fixture content correlation. */
    RuntimeAuthoritySource(std::uint64_t owner,std::uint64_t content):owner_(owner),content_(content){}
    friend struct RuntimeScopeTestAccess;
#endif
    friend class RuntimeScope;
};
}
