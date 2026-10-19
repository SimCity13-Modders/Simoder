#pragma once
#include "scripting/script_runtime.hpp"

namespace sc13::scripting {
struct ScriptSchedulerState;
/** @summary Closed native purpose; textual mod names cannot select a publication capability. */
enum class SchedulerPublicationPurpose { Activate, Handoff, Remove };
/** @summary Retains an exact one-shot prepared scheduler change and its deferred reclamation owners. */
class SchedulerPublicationPlan final {
public:
    /** @summary Constructs an invalid empty plan. */
    SchedulerPublicationPlan() = default;
    /** @summary Moves prepared identities without duplicating a publication capability. @param other Source, invalidated by the move. */
    SchedulerPublicationPlan(SchedulerPublicationPlan&& other) noexcept = default;
    /** @summary Moves prepared identities outside publication reservations. @param other Source, invalidated by the move. @return This plan. */
    SchedulerPublicationPlan& operator=(SchedulerPublicationPlan&& other) noexcept = default;
    SchedulerPublicationPlan(const SchedulerPublicationPlan&) = delete;
    SchedulerPublicationPlan& operator=(const SchedulerPublicationPlan&) = delete;
private:
    friend class ScriptScheduler;
    friend class SchedulerPublicationReservation;
    friend class mods::OwnerLifecycle;
    friend class mods::ManagerPublication;
    friend class mods::ManagerClose;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct SchedulerPublicationTestAccess;
#endif
    std::shared_ptr<ScriptSchedulerState> state_;
    std::shared_ptr<runtime::RuntimeCommitDomain> domain_;
    std::shared_ptr<ScriptRuntime> old_, candidate_, retired_;
    std::shared_ptr<const std::byte> node_;
    std::shared_ptr<const void> jointBinding_;
    /** @summary Exact restricted preparation proof; only the canonical joint publisher may supply it. */
    std::shared_ptr<const void> preparationProof_;
    std::string owner_;
    std::uint64_t revision_{}, domainRevision_{}, oldGeneration_{}, candidateGeneration_{};
    std::size_t oldSlot_{}, candidateSlot_{};
    SchedulerPublicationPurpose purpose_{SchedulerPublicationPurpose::Activate};
    bool consumed_{}, absent_{};
};
/** @summary Stack-only typed scheduler reservation shared by standalone and joint publishers. */
class SchedulerPublicationReservation final {
private:
    friend class mods::ManagerCommands;
    friend class ScriptScheduler;
    friend class mods::OwnerLifecycle;
    friend class mods::ManagerPublication;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct mods::OwnerLifecycleTestAccess;
#endif
public:
    /** @summary Late T reservation whose lexical lifetime ends before Q/B/R. */
    class StatusReservation final {
    public:
        /** @summary Tries the canonical diagnostic locks without allocation.
         * @param reservation Existing S/V holder, or null for legacy publication. */
        explicit StatusReservation(SchedulerPublicationReservation* reservation) noexcept;
        /** @summary Reports the status guard result. @return Ok or bounded contention. */
        runtime::ScopeResult Result() const noexcept { return result_; }
    private:
        runtime::ScopeResult result_{runtime::ScopeResult::Ok};
        std::unique_lock<std::mutex> first_, second_;
    };
    /** @summary Tries S then canonical V under the caller's original D.
     * @param plan Exact retained preparation. @param binding Null for standalone, exact record seal for joint. */
    SchedulerPublicationReservation(SchedulerPublicationPlan& plan, const std::shared_ptr<const void>& binding) noexcept;
    /** @summary Releases fixture instrumentation after all participant guards. */
    ~SchedulerPublicationReservation();
private:
    /** @summary Applies only retained swaps/scalars after the caller's sole D marker. */
    void Apply() noexcept;
    /** @summary Closes execution admission without activating a candidate on defensive post-marker inconsistency. */
    void CloseIndeterminate() noexcept;
    /** @summary Binds only retained node/plan scalars while the coordinator holds this exact D/S/V reservation.
     * @param binding Nonreusable record token. @return Ok without activation, or conflict before any seal changes. */
    runtime::ScopeResult BindToken(const std::shared_ptr<const void>& binding) noexcept;
    /** @summary Captures an already removed exact joint node without an activation or revision.
     * @param scheduler Original scheduler. @param owner Exact owner. @param binding Original record token.
     * @param output Empty plan. @return Ok only for the original empty sealed node. */
    static runtime::ScopeResult PrepareRemoved(ScriptScheduler& scheduler,const std::string& owner,
        const std::shared_ptr<const void>& binding,SchedulerPublicationPlan& output);
    /** @summary Reads the actual scheduler owner under caller-held D and try S.
     * @param scheduler Actual scheduler. @param owner Exact key. @param binding Original record seal.
     * @param output Cleared retained owner. @param schedulerGuard Retains S through the coherent observation.
     * @param vmGuard Retains actual V through the caller's scope/status observation.
     * @return Coherent owner or contention/conflict. */
    static runtime::ScopeResult Inspect(ScriptScheduler& scheduler,const std::string& owner,
        const std::shared_ptr<const void>& binding,std::shared_ptr<ScriptRuntime>& output,
        std::unique_lock<std::mutex>& schedulerGuard,std::unique_lock<std::mutex>& vmGuard);
    /** @summary Reserve original scheduler and VM without resolving a successor by textual identity.
     * @param scheduler Original scheduler. @param owner Correlation key. @param binding Exact joint seal.
     * @param expected Original VM. @param node Original selected node identity. @param registration Original selected registration.
     * @param schedulerGuard Retains S. @param vmGuard Retains V. @return Exact current execution qualification. */
    static runtime::ScopeResult InspectExact(ScriptScheduler& scheduler,const std::string& owner,
        const std::shared_ptr<const void>& binding,const std::shared_ptr<ScriptRuntime>& expected,
        const std::shared_ptr<const std::byte>& node,std::uint64_t registration,
        std::unique_lock<std::mutex>& schedulerGuard,std::unique_lock<std::mutex>& vmGuard);
    /** @summary Copies only the actual published running scalar under try T after caller-held V/Q.
     * @param vm Exact retained VM. @param running Cleared first. @return Ok or bounded contention. */
    static runtime::ScopeResult ReadRunning(const std::shared_ptr<ScriptRuntime>& vm,bool& running) noexcept;
    /** @summary Checks original scheduler enrollment and candidate ownership before any record/origin seal.
     * @param scheduler Actual scheduler. @param owner Exact node. @param old Expected current VM or null.
     * @param candidate Uncommitted successor or null. @param domain Original domain. @param binding Existing record seal.
     * @param prepared True only for this output's retained successfully sealed candidate.
     * @param preparationProof Exact canonical restricted preparation, never a generic mode bypass.
     * @return Current native qualification. */
    static runtime::ScopeResult ValidateEnrollment(ScriptScheduler& scheduler,const std::string& owner,
        const std::shared_ptr<ScriptRuntime>& old,const std::shared_ptr<ScriptRuntime>& candidate,
        const std::shared_ptr<runtime::RuntimeCommitDomain>& domain,const std::shared_ptr<const void>& binding,bool prepared,
        const std::shared_ptr<const void>& preparationProof = {});
    /** @summary Brackets actual guard acquisition and destruction without dynamic instrumentation. */
    struct Fence final {
        /** @summary Saves the calling thread's prior instrumentation scope. */
        Fence() noexcept;
        /** @summary Restores instrumentation only after every later-declared participant unlocks. */
        ~Fence();
        bool previous{};
    } fence_;
    SchedulerPublicationPlan& plan_;
    runtime::ScopeResult result_{runtime::ScopeResult::Invalid};
    ScriptRuntime* first_{}, *second_{};
    std::unique_lock<std::mutex> scheduler_, vm1_, vm2_;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fixture-only producer for the isolated native scheduler model, never a production issuer. */
struct SchedulerPublicationTestAccess final {
    /** @summary Binds one original domain before any node exists. @param scheduler Actual native scheduler. @param domain Original retained writer.
     * @return Ok only for a fresh unenrolled scheduler. */
    static runtime::ScopeResult Enroll(ScriptScheduler& scheduler, std::shared_ptr<runtime::RuntimeCommitDomain> domain);
    /** @summary Prepares an exact initial, replacement or removal plan outside every publication fence.
     * @param scheduler Actual enrolled scheduler. @param owner Native fixture name. @param candidate Exact initialized VM, null for removal.
     * @param purpose Closed native purpose. @param output Empty plan receiving retained preparation. @return Ok or pre-publication rejection. */
    static runtime::ScopeResult Prepare(ScriptScheduler& scheduler, const std::string& owner,
        const std::shared_ptr<ScriptRuntime>& candidate, SchedulerPublicationPurpose purpose,
        SchedulerPublicationPlan& output);
    /** @summary Attempts D-S-V-T once; all fallible validation precedes the irreversible marker.
     * @param plan Exact original preparation. @return Ok only after the once-only marker, otherwise no publication. */
    static runtime::ScopeResult Publish(SchedulerPublicationPlan& plan);
    /** @summary Fully closes a retired owner or unpublished candidate off-lane and outside publication reservations.
     * @param plan Retains exact reclamation ownership. @return Ok after full Close/slot release, or Denied for unsafe caller/active ownership. */
    static runtime::ScopeResult Reclaim(SchedulerPublicationPlan& plan);
    /** @summary Erases only an empty fixture reservation, invalidating its native identity against ABA.
     * @param scheduler Original enrolled scheduler. @param owner Exact empty node name. @return Ok, Busy or Denied. */
    static runtime::ScopeResult ReleaseEmpty(ScriptScheduler& scheduler, const std::string& owner);
    /** @summary Obtains the sole node's retained owner through its original writer and scheduler guard.
     * @param scheduler Original enrolled scheduler. @param owner Exact node name. @return Retained current VM, or null for absence/contention. */
    static std::shared_ptr<ScriptRuntime> Current(ScriptScheduler& scheduler, const std::string& owner);
    /** @summary Installs fixture barriers only before scheduling; callbacks never execute inside publication fences.
     * @param vm Exact fixture VM. @param selected Runs after actual lane selection outside L/V. @param extracted Runs after copied batch extraction outside V. */
    static void Barriers(const std::shared_ptr<ScriptRuntime>& vm, std::function<void()> selected,
        std::function<void()> extracted);
    /** @summary Holds an actual VM or status mutex for a deterministic contention test.
     * @param vm Exact retained VM. @param status Selects T instead of V. @param body Bounded fixture barrier, never a publication callback. */
    static void Hold(const std::shared_ptr<ScriptRuntime>& vm, bool status, const std::function<void()>& body);
    /** @summary Holds the actual sole scheduler mutex for a bounded contention test.
     * @param scheduler Actual scheduler. @param body Bounded native test barrier. */
    static void HoldScheduler(ScriptScheduler& scheduler,const std::function<void()>& body);
    /** @summary Reports fixture-thread reservation instrumentation; never calls a callback at the fence. @return True within typed publication attempt. */
    static bool InsideFence() noexcept;
    /** @summary Observes exact deferred-close accounting without granting scheduling eligibility. @param vm Actual retained VM. @return True after full Close/slot release. */
    static bool Closed(const std::shared_ptr<ScriptRuntime>& vm);
    /** @summary Observes the monotonic native closure request before lane teardown executes. @param vm Actual VM. @return Sticky closure request. */
    static bool StopRequested(const std::shared_ptr<ScriptRuntime>& vm) noexcept;
    /** @summary Observes shutdown's sticky latch without retaining any publication participant. @param scheduler Actual scheduler. @return True if stopped was observed. */
    static bool Stopped(ScriptScheduler& scheduler);
};
#endif
}
