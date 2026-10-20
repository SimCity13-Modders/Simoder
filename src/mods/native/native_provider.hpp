#pragma once
#include "mods/native/contract.h"
#include "scripting/base_host.hpp"
#include "scripting/semantic_bridge.hpp"
#include "runtime/runtime_lifecycle.hpp"
#include <memory>
#include <functional>
#include <vector>
#include <variant>

namespace sc13::mods::native {
/** @summary Typed outer owner; genuine script ownership never enters an endpoint or native State. */
class ProviderParticipant final {
public:
    /** @summary Retain one genuine existing VM outside guards. @param runtime Original script runtime. */
    ProviderParticipant(std::shared_ptr<scripting::ScriptRuntime> runtime):lifetime_(std::move(runtime)){}
    /** @summary Retain an original loader-issued State outside guards. @param participant Original native participant. */
    ProviderParticipant(const NativeParticipant& participant):lifetime_(participant){}
private:
    std::variant<std::shared_ptr<scripting::ScriptRuntime>,NativeParticipant> lifetime_;
    friend class NativeProvider;
};
/** @summary Private captured route; only a qualified host producer may construct production inputs. */
struct ProviderRoute final {
    std::uint32_t method{};
    std::string name;
    SdHandle target{};
    scripting::SemanticContextLease context;
    std::uint32_t opcode{};
};
/** @summary Closed C-ABI-shaped adapter over the original Base/scope/task path; no production facet is installed. */
class NativeProvider final {
public:
    /** @summary Retains exact host captures; does not issue a session, grant, VM identity or context.
     * @pre Original participant initialization/VM identity publication happens-before this call; no construction-time VM race is supported.
     * @param owner Existing loader owner. @param base Actual verified Base instance.
     * @param endpoint Actual bound participant, never a fabricated native VM.
     * @param participant Original participant lifetime. @param lifecycle Original factual publisher.
     * @param session Already verified private session mapping; zero never upgrades an unqualified native participant.
     * @param routes Host-reviewed method/target/context mapping; empty in ordinary unqualified construction. */
    NativeProvider(SdHandle owner, std::shared_ptr<scripting::BaseHost> base,
        std::shared_ptr<scripting::SemanticEndpoint> endpoint, ProviderParticipant participant,
        std::shared_ptr<runtime::RuntimeLifecycle> lifecycle, SdHandle session,
        std::vector<ProviderRoute> routes);
    /** @summary Construct only a closed native adapter from exact State-owned lifetimes; no routes or grants.
     * @param base Actual verified Base host. @param participant Original loader-issued participant. */
    NativeProvider(std::shared_ptr<scripting::BaseHost> base,const NativeParticipant& participant);
    /** @summary Closes admission and preserves owner/task dependencies until original cleanup. */
    ~NativeProvider();
    NativeProvider(const NativeProvider&) = delete;
    NativeProvider& operator=(const NativeProvider&) = delete;
    /** @summary Maps the original authority outcome without changing semantics. @param result Existing result. @return ABI result. */
    static SdResult Map(runtime::ScopeResult result) noexcept;
    /** @summary Binds existing authority only. @param owner Exact native owner. @param session Verified session.
     * @param method Reviewed ID. @param target Exact factual reference. @param revision Original revision.
     * @param output Cleared invocation identity. @return Admission, never grant issue. */
    SdResult Bind(SdHandle owner, SdHandle session, std::uint32_t method, SdHandle target,
        std::uint64_t revision, SdHandle* output) noexcept;
    /** @summary Copies and enqueues original work off-lane. @param owner Original owner.
     * @param invocation Bound reference. @param method Same reviewed ID. @param input Caller bytes.
     * @param output Cleared native task reference. @return Admission only. */
    SdResult Invoke(SdHandle owner, SdHandle invocation, std::uint32_t method, SdBytes input, SdHandle* output) noexcept;
    /** @summary Delivers one authorized original task publication and its task-scoped informational execution context.
     * @param owner Original owner. @param invocation Original reference. @param task Original task.
     * @param info Caller-sized output, header-only on failure. @param output Caller buffer, cleared before validation.
     * @param capacity Output bytes. @param required Exact result bytes on OK or authorized BUFFER_SMALL; otherwise zero.
     * @return OK for an observation independently of its task outcome; BUFFER_SMALL discloses only required length. */
    SdResult Poll(SdHandle owner, SdHandle invocation, SdHandle task, SdTaskInfo* info,
        std::uint8_t* output, std::uint32_t capacity, std::uint32_t* required) noexcept;
    /** @summary Cached method snapshots are unavailable without a factual producer.
     * @param owner Original owner. @param invocation Bound reference. @param method Reviewed ID.
     * @param info Caller-sized sanitized output. @param output Cleared caller buffer.
     * @param capacity Output bytes. @param required Always zero. @return Validated NOT_READY or denial. */
    SdResult Snapshot(SdHandle owner, SdHandle invocation, std::uint32_t method, SdSnapshotInfo* info,
        std::uint8_t* output, std::uint32_t capacity, std::uint32_t* required) noexcept;
    /** @summary Cancels actual original task without undo. @param owner Original owner.
     * @param invocation Original reference. @param task Original task. @return BUSY after commit or exact outcome. */
    SdResult Cancel(SdHandle owner, SdHandle invocation, SdHandle task) noexcept;
    /** @summary Owner-only release remains available after revocation. @param owner Issuing owner.
     * @param task Original task. @return Original cleanup outcome; not proof of bridge queue drain. */
    SdResult Release(SdHandle owner, SdHandle task) noexcept;
    /** @summary Invalidates a reference once its tasks/calls release it. @param owner Issuing owner.
     * @param invocation Original reference. @return BUSY while retained, otherwise exact cleanup. */
    SdResult ReleaseInvocation(SdHandle owner, SdHandle invocation) noexcept;
    /** @summary Closes this adapter and stops original scope without invoking foreign code. */
    void Close() noexcept;
private:
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct NativeProviderTestAccess;
#endif
    /** @summary Derive one endpoint from the actual Base and original State, off guards.
     * @param base Verified original Base host. @param participant Actual retained State. @return Closed native endpoint. */
    static std::shared_ptr<scripting::SemanticEndpoint> NativeEndpoint(const std::shared_ptr<scripting::BaseHost>& base,
        const NativeParticipant& participant);
    struct State;
    std::unique_ptr<State> state_;
    /** @summary Reuses the sole original grant evaluator under try-only Q. @return Current admission. */
    runtime::ScopeResult CheckAuthority() const;
    /** @summary Validates the immutable whole binding using the actual B-protected source snapshot. @return Factual binding status. */
    runtime::ScopeResult CheckFacts() const;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fresh-process exhaustion witness, unavailable in production and incapable of restoring issued identities. */
struct NativeProviderTestAccess final {
    /** @summary Retain the actual already constructed endpoint off guards; never construct an identity or issue authority.
     * @param provider Original adapter. @return Its exact immutable endpoint. */
    static std::shared_ptr<scripting::SemanticEndpoint> Endpoint(const NativeProvider& provider);
    /** @summary Burn an entirely unused allocator to its first rejected width; no arbitrary seed or reset.
     * @return True only before any provider block has ever been issued in this process. */
    static bool ExhaustBlockRange() noexcept;
    /** @summary Install a fixture-thread observation cut after original task capture and before delivery revalidation.
     * @param callback Owned fixture callback, or empty to remove it; never an authority or ambient identity source. */
    static void AfterPoll(std::function<void()> callback);
    /** @summary Install a fixture-thread scheduling cut after shared native task reservation and before original Request.
     * @param callback Owned deterministic observation callback, or empty to remove; never an authority/identity source. */
    static void AfterTaskReserve(std::function<void()> callback);
};
#endif
}
