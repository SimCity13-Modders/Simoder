#pragma once
#include "mods/owned_publication.hpp"
#include "scripting/script_command_batch.hpp"
#include "mods/action_worker.hpp"
#include "mods/fixture_observation.hpp"

namespace sc13::mods {
class ManagerCommandRoute;
struct ManagerCommandTicket;
/** @summary Closed native scheduler consumer; production has no route or authority issuer. */
class ManagerCommands final {
    struct Fence;
    friend class scripting::ScriptRuntime;
    friend class scripting::ScriptLane;
    friend struct ManagerCommandTicket;
    friend class ActionWorker;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerCommandsTestAccess;
#endif
    /** @summary Capture original authority and ownership before the first delayed selection barrier.
     * @param vm Actually selected VM. @param registration Original selected registration.
     * @param route Accepted original route. @param origin Accepted original cohort. @return Owned ticket or empty denial. */
    static scripting::ScriptCommandBatch Select(scripting::ScriptRuntime& vm,std::uint64_t registration,
        const std::shared_ptr<ManagerCommandRoute>& route,const std::shared_ptr<PublicationOrigin>& origin);
    /** @summary Try original single occupancy before Lua; true also means routed-but-denied, with no fallback. @param vm Actual selected VM. @param registration Exact selection. @return False only for an observed routeless VM. */
    static bool TrySchedule(scripting::ScriptRuntime& vm,std::uint64_t registration) noexcept;
    /** @summary Execute original retained job with action-local native lifetime, exact lane capture and off-lane cache work. @param job Accepted original cohort. */
    static void ExecuteScheduled(ActionWorker::ScheduledJob& job);
    /** @summary Attach exact extracted bytes before any extraction delay. @param batch Original ticket. @param commands Owned Lua output. */
    static void Attach(scripting::ScriptCommandBatch& batch,std::vector<scripting::ScriptPatchCommand>&& commands);
    /** @summary Deliver through the native typed sink, never legacy fallback. @param batch Exact output. */
    static void Dispatch(scripting::ScriptCommandBatch& batch);
    /** @summary Consume once with exact original manager/scheduler/context/grant validation. @param batch Retained original output. @return Bounded outcome. */
    static runtime::ScopeResult Consume(scripting::ScriptCommandBatch& batch);
    /** @summary Retire exactly the newly owned operation reservation, even after revocation. @param ticket Original ticket. */
    static void Release(ManagerCommandTicket& ticket) noexcept;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fixture-only producer and deterministic interleavings; absent from production Core. */
struct ManagerCommandsTestAccess final {
    /** @summary Copied outcome scalars do not disclose payloads or grant authority. */
    struct Observation final {
        runtime::ScopeResult lastOutcome{runtime::ScopeResult::NotReady};
        std::uint64_t attempts{},commits{},selectedRegistration{},commitRegistration{},generation{};
    };
    /** @summary Install a dormant exact candidate route, without issuing permission or activating it.
     * @param manager Original manager. @param origin Exact prepared candidate or active owner.
     * @param grant Existing reviewed PROP opcode-one provenance. @return Native binding outcome. */
    static runtime::ScopeResult Install(ModManager& manager,const std::shared_ptr<PublicationOrigin>& origin,const runtime::OperationContext& grant);
    /** @summary Intercept real extracted envelopes off every guard; moving the envelope prevents its immediate dispatch.
     * @param vm Original VM. @param callback Persistent interception; empty clears it. */
    static void Intercept(const std::shared_ptr<scripting::ScriptRuntime>& vm,std::function<void(scripting::ScriptCommandBatch&)> callback);
    /** @summary Install persistent sink interleavings outside all compound guards.
     * @param vm Original VM. @param beforeLease Before actual native lifetime entry. @param afterLease After entry, before D. */
    static void Barriers(const std::shared_ptr<scripting::ScriptRuntime>& vm,std::function<void()> beforeLease,std::function<void()> afterLease);
    /** @summary Consume one intercepted envelope; Busy also consumes it. @param batch Original one-shot output. @return Exact outcome. */
    static runtime::ScopeResult Consume(scripting::ScriptCommandBatch& batch){return ManagerCommands::Consume(batch);}
    /** @summary Copy bounded original-route diagnostics. @param vm Original VM. @return Cleared observation when no route exists. */
    static Observation Observe(const std::shared_ptr<scripting::ScriptRuntime>& vm);
    /** @summary Count actual retained reservation capacity without granting permission. @param scope Original scope. @return Retained slots. */
    static std::size_t Reservations(const runtime::RuntimeScope& scope);
    /** @summary Observe actual original participant pending obligation without new authority. @param manager Original owner. @param id Actual map participant. @return Original native facts. */
    static runtime::CacheSettlement::Observation ObserveCache(ModManager& manager,const ModId& id);
    /** @summary Explicit original eviction under a fresh action-local lifetime; no D/grant/script replay. @param manager Original owner. @param id Actual participant. @return Original native result only. */
    static runtime::CacheSettlement::Observation RepairCache(ModManager& manager,const ModId& id);
    /** @summary Retain an existing original pending record through fresh native action admission, without exposing its token.
     * @param manager Original live owner. @param id Actual participant. @param output Empty checked view, unchanged on rejection.
     * @return Actual bounded original lookup outcome. */
    static runtime::CacheSettlement::Result PendingCache(ModManager& manager,const ModId& id,runtime::CacheSettlement::Batch& output);
    /** @summary Set a joined actual post-Prepare/pre-Fence interleaving; null clears it and retired ownership dies off guards.
     * @param vm Original route VM. @param callback Worker callback with only original marker turn retained; never block on that same record. */
    static void CachePreparedBarrier(const std::shared_ptr<scripting::ScriptRuntime>& vm,std::shared_ptr<const std::function<void()>> callback);
    /** @summary Set a borrowed observation after initial V release and before original W admission; null clears it without callable ownership.
     * @param vm Original route VM. @param callback Noncapturing static code, finite joined gate only, never acquires W/V or calls VM controls.
     * @param context Externally owned context retained until actual lane visit joins and probe is cleared under V. */
    static void AdmissionBarrier(const std::shared_ptr<scripting::ScriptRuntime>& vm,void (*callback)(void*),void* context);
    /** @summary Exact weak lifetime evidence and nonrecycled destructor slots, never owning the observed cohort. */
    struct Cohort final {std::weak_ptr<const void> preparation,state,base,origin;fixture::Handle preparationEntry,stateEntry,baseEntry;};
    /** @summary Arm observations for actual promoted preparation, original manager State and real Base off lane.
     * @param manager Exact live manager. @param origin Actual original native origin. @param base Exact original host, already owned by caller.
     * @return Weak evidence; empty handles indicate bounded observation admission failure. */
    static Cohort WatchCohort(ModManager& manager,const std::shared_ptr<PublicationOrigin>& origin,const std::shared_ptr<scripting::BaseHost>& base);
    /** @summary Snapshot actual original pending queue facts without copying its job or retaining its payload. */
    using QueueObservation=ActionQueueObservation;
    /** @summary Set the genuine worker cut after wake and before detach, outside W. @param manager Original owner. @param callback Bounded native observation; empty removes it off W. */
    static void BeforeDetach(ModManager& manager,std::shared_ptr<const std::function<void(QueueObservation)>> callback);
    /** @summary Capture only the already-existing independent close-admission latch before starting teardown. @param manager Exact live owner. @return Original latch, not a manager/State/job owner. */
    static std::shared_ptr<const std::atomic_bool> ActionClosed(ModManager& manager);
    /** @summary Native fixed storage measurements, not fabricated occupancy or authority. */
    struct Layout final {std::size_t scheduledJobBytes{},controlBytes{},priorControlBytes{},vmAnchorBytes{},transferBytes{};};
    /** @summary Measure actual compiled worker/VM handoff storage. @return Native sizeof facts for this configuration. */
    static Layout HandoffLayout() noexcept;
    /** @summary Set joined actual handoff cuts without replacing the scheduler/consumer. @param vm Original VM. @param beforeAcknowledgment After V capture before Work.done on lane. @param beforeConsume After acknowledgment on worker before Attach/Consume. */
    static void HandoffBarriers(const std::shared_ptr<scripting::ScriptRuntime>& vm,
        std::shared_ptr<const std::function<void()>> beforeAcknowledgment,
        std::shared_ptr<const std::function<void()>> beforeConsume);
    /** @summary Hold the actual original W for bounded contention, with no VM lock or lane control inside callback. @param manager Original live owner. @param callback Joined native gate. */
    static void HoldWorker(ModManager& manager,const std::function<void()>& callback);
};
#endif
}
