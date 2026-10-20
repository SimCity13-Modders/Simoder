#pragma once
#include "runtime/semantic_tasks.hpp"
#include "scripting/semantic_participant.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace sc13::mods { class OwnedPublication; class OwnerLifecycle; class ManagerCommands; }
namespace sc13::mods::native { class NativeProvider; }
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
namespace sc13::mods { class PublicationOrigin; class LifecycleDecision; }
#endif
namespace sc13::scripting {
class ScriptRuntime;
/** @summary Native factual context kinds are independent from authority grants and textual references. */
enum class SemanticContextKind : std::uint8_t { Runtime,Region,City,Entity };
/** @summary Copied host observation used only to detect stale context identities. */
struct SemanticContextFact final {
    std::uint64_t slot{},epoch{};
    std::uint32_t generation{};
    bool operator==(const SemanticContextFact&) const = default;
};
/** @summary Private bound context lease; a decoded reference alone cannot construct one. */
struct SemanticContextLease final {
    std::array<SemanticContextFact,4> facts{};
    SemanticParticipantKey participant;
    std::uint64_t baseEpoch{};
    std::uint64_t operation{};
    std::uint64_t scopeGeneration{},runtimeEpoch{};
    std::uint8_t mask{};
};
/** @summary Immutable native publication; sequence and timestamp belong to the source, not a consumer poll. */
struct SemanticPublication final {
    std::uint64_t sequence{}, monotonicNs{}, baseEpoch{}, baseGeneration{};
    std::array<SemanticContextFact,4> contexts{};
    bool singlePlayerKnown{};
};
/** @summary Reviewed native operation metadata; executable producers are never supplied by a script. */
struct SemanticMethod final {
    std::string id, inputSchema, lane, context, capability, postcondition;
    std::size_t maxResultBytes{};
    runtime::SemanticTasks::ValueCallback execute;
    /** @summary Reviewed internal registry-only route; false for every existing generic descriptor. */
    bool registryPublication{};
};
class SemanticBridge;
/** @summary Private endpoint retains the original scope and cannot be rebound by public mod IDs. */
struct SemanticEndpoint final {
    std::shared_ptr<SemanticBridge> bridge;
    std::shared_ptr<runtime::SemanticTasks> tasks;
    std::shared_ptr<runtime::RuntimeScope> scope;
    runtime::OperationContext context;
    std::uint64_t storageOwner{};
    std::atomic_bool closed{};
    runtime::copied::Reservation accounting;
    /** @summary Project a genuine script VM after original publication. @return Actual VM generation, otherwise zero. */
    std::uint64_t ScriptGeneration() const noexcept { return participant_.ScriptGeneration(); }
    /** @summary Borrow immutable identity; initial publication is serialized by original B. @return Sole participant. */
    const SemanticParticipant& Participant() const noexcept { return participant_; }
private:
    SemanticParticipant participant_;
    friend class SemanticBridge;
};
/** @summary Copied immutable host binding captured under original B; identities never issue authority. */
struct SemanticEndpointBinding final {
    std::shared_ptr<SemanticEndpoint> endpoint;
    std::shared_ptr<runtime::RuntimeScope> scope;
    std::shared_ptr<runtime::SemanticTasks> tasks;
    runtime::OperationContext authority;
    SemanticParticipantKey participant;
    std::uint64_t storageOwner{},baseEpoch{},baseGeneration{};
    bool operator==(const SemanticEndpointBinding&) const = default;
};
/** @summary Bounded copied request crosses VM boundaries without entering another interpreter inline. */
struct SemanticInvocation final {
    std::shared_ptr<SemanticEndpoint> origin;
    runtime::TaskHandle task;
    runtime::copied::Envelope value;
    SemanticParticipantKey participant;
    std::uint64_t epoch{};
    std::size_t method{};
    SemanticContextLease context;
};
/** @summary Private base transport; authority remains exclusively in RuntimeScope. */
class SemanticBridge final : public std::enable_shared_from_this<SemanticBridge> {
public:
    /** @summary Creates an unavailable transport with one shared copied-storage pool. */
    SemanticBridge();
    /** @summary Creates an unbound endpoint for once-only genuine VM publication; a scope alone is never native identity.
     * @param scope Original script scope. @return Unbound closed-authority endpoint. */
    std::shared_ptr<SemanticEndpoint> Bind(std::shared_ptr<runtime::RuntimeScope> scope);
    /** @summary Reserves the single originating task and copied invocation before enqueue. @param endpoint Calling VM's immutable binding. @param method Reviewed catalog name. @param value Copied input. @param output Receives opaque retained task. @param context Cleared captured facts on rejection. @param expected Optional original factual context. @param binding Optional complete original authority binding. Both expected captures are checked under original B and never retained. @return Admission status. */
    runtime::ScopeResult Request(const std::shared_ptr<SemanticEndpoint>& endpoint, std::string_view method,
        const runtime::copied::Envelope& value, runtime::TaskHandle& output,SemanticContextLease& context,
        const SemanticContextLease* expected = nullptr, const SemanticEndpointBinding* binding = nullptr);
    /** @summary Removes one queued invocation for a later script turn; never executes Lua. @param output Receives private origin token. @return Whether a request was removed. */
    bool Take(SemanticInvocation& output);
    /** @summary Executes only the reserved native descriptor on its original task. @param invocation Private copied token. @param method Lua-resolved descriptor. @param value Same immutable input. @return Native outcome. */
    runtime::ScopeResult Dispatch(const SemanticInvocation& invocation, std::string_view method,
        const runtime::copied::Envelope& value);
    /** @summary Commits an isolated prepared registry plan on the original native task with a reserved context fence.
     * @param invocation Original issuer-bound request. @param registry Native registry owner.
     * @param plan Caller-owned prepared storage retained beyond all reservations.
     * @return Registry-only completion or a prepublication rejection; never asserts live application. */
    runtime::ScopeResult DispatchPrepared(const SemanticInvocation& invocation, mods::PatchRegistry& registry,
        mods::OwnedPublication& plan);
    /** @summary Closes old epoch admission and cancels queued requests without running scripts. */
    void Close() noexcept;
    /** @summary Exposes immutable native descriptor metadata to the verified base only. @return Catalog lifetime is this bridge. */
    const std::vector<SemanticMethod>& Methods() const noexcept { return methods_; }
    /** @summary Returns the shared bounded value pool. @return Pool retaining all envelopes. */
    std::shared_ptr<runtime::copied::Storage> Values() const noexcept { return values_; }
    /** @summary Publishes a proof only after native qualification of the actual VM's critical services. @param generation Actual VM generation. */
    /** @summary Reads factual readiness without granting operation authority. @return Ready generation or zero. */
    std::uint64_t ReadyGeneration() const noexcept;
    /** @summary Reads this immutable base transport epoch. @return Unique process-local epoch. */
    std::uint64_t Epoch() const noexcept { return epoch_; }
    /** @summary Rejects stale private bindings without replacing their identities. @param endpoint Original VM. @param lease Native resolved context. @return Current context status. */
    runtime::ScopeResult ValidateContext(const SemanticEndpoint& endpoint,const SemanticContextLease& lease) const;
    /** @summary Atomic-only factual fence used inside the existing authority commit boundary; never locks or invokes script code. */
    runtime::ScopeResult ContextFence(const SemanticEndpoint& endpoint,const SemanticContextLease& lease) const noexcept;
    /** @summary Binds only factual reference lifetime, never permissions; foreign references remain untrusted decoded values. */
    runtime::ScopeResult BindReferences(const SemanticEndpoint& endpoint,const runtime::copied::Envelope& value,SemanticContextLease& output) const;
    /** @summary Atomically capture current state and its stream cursor. @return Copied exact native publication. */
    SemanticPublication CurrentPublication() const noexcept;
    /** @summary Read one ordered source publication without entering Lua or silently dropping history.
     * @param cursor Last consumed sequence; updated only on success.
     * @param output Empty on unavailable or overflow.
     * @return Ok, NotReady for no newer publication, or Conflict when the bounded history was overrun. */
    runtime::ScopeResult NextPublication(std::uint64_t& cursor, SemanticPublication& output) const noexcept;
private:
    friend class SemanticLua;
    friend class mods::native::NativeProvider;
    friend class mods::ManagerCommands;
    friend class ScriptRuntime;
    friend class mods::OwnerLifecycle;
    /** @summary Publish a genuine scheduler-reserved VM key exactly once under B.
     * @param endpoint Original unpublished endpoint. @param runtime Genuine constructing VM. @return Initial transition only. */
    bool InitializeScript(const std::shared_ptr<SemanticEndpoint>& endpoint,const ScriptRuntime& runtime) noexcept;
    /** @summary Build a closed endpoint from original State lifetimes outside guards.
     * @param participant Actual loader-issued retained State. @return Closed-authority original endpoint. */
    std::shared_ptr<SemanticEndpoint> BindNative(const mods::native::NativeParticipant& participant);
    /** @summary Register one preallocated endpoint; weak-holder disposal occurs after unlock.
     * @param endpoint Fully staged original endpoint. @return Same registered endpoint. */
    std::shared_ptr<SemanticEndpoint> Register(std::shared_ptr<SemanticEndpoint> endpoint);
    /** @summary Copies a complete original binding under B, without granting rights or retaining caller output.
     * @pre Immutable ownership staging happens off guards; initial participant publication and authority replacement use original B.
     * @param endpoint Original registered endpoint. @param output Cleared copied ownership.
     * @return Explicit factual availability, never operation authority. */
    runtime::ScopeResult CaptureBinding(const std::shared_ptr<SemanticEndpoint>& endpoint,SemanticEndpointBinding& output) const;
    /** @summary Compare every captured identity and authority field while original B excludes refresh.
     * @param endpoint Exact requesting endpoint. @param expected Immutable original capture.
     * @return Stale on any substitution, otherwise factual readiness. */
    runtime::ScopeResult ValidateBindingLocked(const std::shared_ptr<SemanticEndpoint>& endpoint,const SemanticEndpointBinding& expected) const;
    /** @summary Only the host's captured actual base generation may publish its private readiness lease. */
    void Publish(std::uint64_t generation) noexcept;
    std::shared_ptr<runtime::copied::Storage> values_;
    std::vector<SemanticMethod> methods_;
    mutable std::mutex mutex_;
    std::array<SemanticInvocation, 128> queue_{};
    std::array<std::weak_ptr<SemanticEndpoint>,128> endpoints_{};
    runtime::copied::Reservation accounting_;
    std::size_t first_{}, count_{}, bytes_{};
    std::uint64_t epoch_{}, readyGeneration_{};
    bool closed_{};
    std::array<SemanticContextFact,4> contexts_{};
    bool singlePlayerKnown_{};
    std::uint64_t nextOperation_{1};
    std::array<std::atomic_uint64_t,4> contextStamps_{};
    std::atomic_bool retiredStamp_{},singlePlayerStamp_{};
    std::array<SemanticPublication,128> publications_{};
    std::uint64_t publicationSequence_{};
    /** @summary Publish a copied factual transition while holding mutex_; no receiver or script runs here. */
    void PublishObservation() noexcept;
    /** @summary Resolves copied references against current host facts under the transport mutex. */
    runtime::ScopeResult ResolveContext(const SemanticEndpoint& endpoint,const SemanticMethod& method,
        const runtime::copied::Envelope& value,SemanticContextLease& output) const;
    /** @summary Revalidates captured native facts while their actual invalidation mutex is reserved. */
    runtime::ScopeResult ValidateContextLocked(const SemanticEndpoint& endpoint,const SemanticContextLease& lease) const noexcept;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    /** @summary Refresh an exact fixture origin using existing owned reservation validation under D/B/Q.
     * @param origin Original constructor-bound VM incarnation. @param output Empty typed reservation owner.
     * @return Exact admission without grant issue, revision advance or captured-task mutation. */
    runtime::ScopeResult RefreshFixtureReservation(const std::shared_ptr<mods::PublicationOrigin>& origin,
        mods::LifecycleDecision& output);
    friend struct SemanticBridgeTestAccess;
#endif
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fixture catalog and authority binding exist only in the separately compiled test host. */
struct SemanticBridgeTestAccess final {
    /** @summary Copy bounded original queue counters without altering admission.
     * @param bridge Original bridge. @return Count, wire bytes and next operation serial under the same B. */
    static std::array<std::uint64_t,3> ObserveQueue(const SemanticBridge& bridge);
    /** @summary Copies actual original binding only in isolated fixtures; no authority is issued.
     * @param bridge Original bridge. @param endpoint Actual registered VM. @param output Cleared tuple.
     * @return Factual capture result. */
    static runtime::ScopeResult CaptureBinding(const SemanticBridge& bridge,const std::shared_ptr<SemanticEndpoint>& endpoint,SemanticEndpointBinding& output);
    /** @summary Try-reserve the actual observation mutex for bounded fixture contention only.
     * @param bridge Exact isolated bridge. @return Nonblocking observation lock without a factual update. */
    static std::unique_lock<std::mutex> ReserveObservation(SemanticBridge& bridge);
    /** @summary Installs reviewed synthetic descriptors before base startup. @param bridge Closed transport. @param methods Bounded synthetic native producers. */
    static void Configure(SemanticBridge& bridge, std::vector<SemanticMethod> methods);
    /** @summary Binds a preissued fixture context without installing any production authority. @param endpoint Native endpoint. @param context Original RuntimeScope fixture context. */
    static void Issue(SemanticEndpoint& endpoint, runtime::OperationContext context);
    /** @summary Derive one current binding from the SAME existing grant, retaining its authentic typed ticket.
     * @pre Caller owns no publication/bridge/scope guard; same-domain writer reentry returns Busy.
     * @param bridge Exact original transport. @param origin Actual native VM/endpoint/scope incarnation.
     * @param output Empty lifetime owner, retained through all captured tasks and endpoint retirement.
     * @return Exact bounded validation; failure preserves binding and disposes local ownership off guards. */
    static runtime::ScopeResult RefreshReservation(SemanticBridge& bridge,
        const std::shared_ptr<mods::PublicationOrigin>& origin, mods::LifecycleDecision& output);
    /** @summary Advances a factual fixture context; replacing the same slot/generation still invalidates old epochs. @param bridge Fixture transport. @param kind Typed context. @param slot Existing synthetic slot. @param generation Synthetic generation. @param singlePlayerKnown Explicit fixture game context. */
    static void ObserveContext(SemanticBridge& bridge,SemanticContextKind kind,std::uint64_t slot,std::uint32_t generation,bool singlePlayerKnown=true);
};
#endif
}
