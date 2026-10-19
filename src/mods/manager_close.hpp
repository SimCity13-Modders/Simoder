#pragma once
#include "mods/mod_manager.hpp"
#include "mods/manager_publication.hpp"
#include <array>
#include <variant>

namespace sc13::mods {
/** @summary Private close foundation; no production issuer or destruction certificate exists. */
class ManagerClose final {
public:
    /** @summary FoundationReady is a current partial observation, never Finalizable or Closed. */
    enum class Phase { Open, ClosingRequested, Draining, FoundationReady };
    /** @summary Retains only the original native anchor, never borrowed manager resources. */
    class Handle final {
        friend class ManagerClose;
        friend struct ManagerCloseTestAccess;
        std::shared_ptr<ModManager::State> state_;
    };
    /** @summary Exclusive release-only owner; does not increment ordinary native users. */
    class Token final {
        friend class ManagerClose;
    public:
        /** @summary Constructs an invalid owner. */
        Token() = default;
        /** @summary Transfers exact ownership without duplicating finalizer authority. @param other Original owner. */
        Token(Token&& other) noexcept;
        /** @summary Releases previous ownership and transfers the exact nonce. @param other Original owner. @return This token. */
        Token& operator=(Token&& other) noexcept;
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        /** @summary Relinquishes nonce atomically without waiting or releasing borrowed resources. */
        ~Token();
    private:
        /** @summary Clears only this exact owner nonce. */
        void Release() noexcept;
        std::shared_ptr<ModManager::State> state_;
        std::uint64_t nonce_{};
    };
    /** @summary Copied native observations convey no operational or full retirement authority. */
    struct Observation final {
        runtime::ScopeResult result{runtime::ScopeResult::Invalid};
        Phase phase{Phase::Open};
        std::uint64_t generation{}, activityRevision{}, acknowledgements{};
        std::size_t users{}, checked{}, cursor{}, holders{}, selected{}, queued{}, executing{};
        bool admissionsClosed{}, actionBusy{}, remainingRetirement{true}, finalizable{};
        bool actionInFlight{},recordScanComplete{};
        std::uint64_t retired{};
    };
    /** @summary Copies one explicit VM retirement action; never a complete manager/scope certificate. */
    struct RetirementObservation final {
        runtime::ScopeResult result{runtime::ScopeResult::Invalid};
        std::size_t checked{},skipped{},pending{},metadataBytes{},holderBytes{};
        std::uint64_t generation{},vmGeneration{},completion{},retired{};
        bool actionInFlight{},recordScanComplete{},remainingRetirement{true},finalizable{};
    };
    /** @summary Caller-owned original Plan; retain across Busy and never move/destroy during an active borrow. */
    class PreparedRetirement final {
        friend class ManagerClose;
    public:
        /** @summary Creates an empty non-authoritative holder. */
        PreparedRetirement()=default;
        /** @summary Transfers whole original Plan by move construction off guards. @param other Idle source. */
        PreparedRetirement(PreparedRetirement&& other) noexcept;
        /** @summary Destroys old whole Plan before constructing transferred ownership. @param other Idle source. @return This holder. */
        PreparedRetirement& operator=(PreparedRetirement&& other) noexcept;
        PreparedRetirement(const PreparedRetirement&)=delete;
        PreparedRetirement& operator=(const PreparedRetirement&)=delete;
        /** @summary Uses existing ordinary blocking Plan destruction; active lifetime misuse fails fast. */
        ~PreparedRetirement();
        /** @summary Invalidates identity and destroys whole Plan off all guards; no orderly-success claim. */
        void Reset() noexcept;
    private:
        /** @summary Invalidates native identity before whole payload disposal, with this holder turn already owned. */
        void DestroyStorageLocked() noexcept;
        /** @summary Nonowning exact identity points only into the genuinely retained original Plan. */
        struct Target final {scripting::ScriptRuntime* vm{};std::uint64_t generation{};bool completed{};};
        std::shared_ptr<ModManager::State> state_;
        std::variant<std::monostate,ManagerPreparation::Plan,ManagerPublication::Plan> storage_;
        std::array<Target,2> targets_{};
        std::uint64_t generation_{};
        std::size_t count_{};
        mutable std::atomic_bool busy_{};
    };
    /** @summary Scalar-only original holder stages, never a complete scope or loader certificate. */
    struct PreparedObservation final {
        runtime::ScopeResult result{runtime::ScopeResult::Invalid};
        std::uint64_t generation{};
        std::size_t targetCount{},completedTargets{},holderBytes{};
        bool scopePending{true},unqualifiedOwners{true},actionInFlight{},remainingRetirement{true},finalizable{};
    };
    /** @summary Moves only genuine original preparation ownership after try-only native qualification.
     * @param token Original close owner. @param plan Original caller Plan. @param output Empty persistent holder. @return Exact capture result. */
    static runtime::ScopeResult CapturePreparation(Token& token,ManagerPreparation::Plan&& plan,PreparedRetirement& output) noexcept;
    /** @summary Moves original successful publication ownership, never a bare scheduler/weak VM proof.
     * @param token Original close owner. @param plan Original caller Plan. @param output Empty holder. @return Exact result. */
    static runtime::ScopeResult CapturePublication(Token& token,ManagerPublication::Plan&& plan,PreparedRetirement& output) noexcept;
    /** @summary Copies holder stage scalars without Invoke/Lua/destruction. @param holder Original persistent owner. @return Try-only observation. */
    static PreparedObservation ObservePrepared(const PreparedRetirement& holder) noexcept;
    /** @summary Retires at most one original eligible Plan VM, retaining whole Plan across Busy.
     * @param token Exact owner. @param holder Idle caller-owned Plan retained until return. @return One VM/slot result only. */
    static RetirementObservation DrainPrepared(Token& token,PreparedRetirement& holder) noexcept;
    /** @summary Closes ordinary ingress without waiting, callbacks, Lua or borrowed access.
     * @param handle Original retained anchor. @return Ok for the sticky request or Invalid for no anchor. */
    static runtime::ScopeResult Request(const Handle& handle) noexcept;
    /** @summary Tries to acquire one noncounted close owner off every publication fence.
     * @param handle Original requested anchor. @param token Empty output. @return Ok, Busy or bounded rejection. */
    static runtime::ScopeResult Acquire(const Handle& handle, Token& token) noexcept;
    /** @summary Tries actual native/action/lane witnesses and at most 32 existing slots; never destroys resources.
     * @param token Exact exclusive owner. @return Current partial observation; no full closure certificate. */
    static Observation Poll(Token& token) noexcept;
    /** @summary Admits at most one exact current-record VM action after at most 32 record inspections.
     * @pre Off script lane, with no publication guard or callback borrow; accepted native duration is unbounded.
     * @param token Exact noncounted close owner. @return One VM/slot outcome, never full closure. */
    static RetirementObservation DrainOne(Token& token) noexcept;
private:
    struct Action;
    /** @summary Shared original action admission, with State turn already held. @param action Native capture.
     * @param gate Receives actual try gate. @param records Receives actual try M. @return Exact admission result. */
    static runtime::ScopeResult BeginAction(Action& action,std::unique_lock<std::mutex>& gate,std::unique_lock<std::mutex>& records) noexcept;
    /** @summary Pins original borrowed resources after exact target selection while gate/M remain held. @param action Exact capture. @return Sequence/pin admission. */
    static runtime::ScopeResult PinAction(Action& action) noexcept;
    /** @summary Runs shared Scope/Close machinery after all publication guards drop. @param action Pinned original target. @return Native phase outcome. */
    static runtime::ScopeResult RunAction(Action& action) noexcept;
    /** @summary Resolves only original Plan-owned target and rechecks role/current-record exclusion under M.
     * @param holder Genuine original Plan. @param index Bounded target index. @param action Receives exact original target owners.
     * @return Exact provenance/role result. */
    static runtime::ScopeResult SelectPrepared(PreparedRetirement& holder,std::size_t index,Action& action) noexcept;
    /** @summary Joins exact original completion using gate and M already held; performs no destruction.
     * @param state Original live State. @param nonce Current close owner. */
    static void JoinRetirement(ModManager::State& state,std::uint64_t nonce) noexcept;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Isolated native producer and genuine control interleavings; absent from production. */
struct ManagerCloseTestAccess final {
    /** @summary Retains the original manager anchor without granting mutation. @param manager Original owner. @return Exact handle. */
    static ManagerClose::Handle Capture(ModManager& manager);
    /** @summary Accesses the actual scheduler for existing fixture enrollment. @param manager Original owner. @return Its sole scheduler. */
    static scripting::ScriptScheduler& Scheduler(ModManager& manager);
    /** @summary Runs a genuine lane control, never an injected counter. @param vm Actual retained VM. @param callback Native control. */
    static void InvokeControl(const std::shared_ptr<scripting::ScriptRuntime>& vm,const std::function<void()>& callback);
    /** @summary Copies actual queued/executing/selected witnesses. @param vm Actual VM. @return Native slot observation. */
    static scripting::SchedulerCloseObservation Controls(const std::shared_ptr<scripting::ScriptRuntime>& vm);
    /** @summary Pauses a genuinely accepted action before its existing handler, outside queue locks.
     * @param manager Original owner. @param callback Persistent fixture barrier; install before submitting. */
    static void ActionBarrier(ModManager& manager,std::function<void()> callback);
    /** @summary Exposes weak anchor expiry without prolonging ownership. @param handle Original anchor. @return Weak identity. */
    static std::weak_ptr<const void> Anchor(const ManagerClose::Handle& handle);
    /** @summary Retains actual current-record VM without granting runtime permission. @param manager Original owner. @param id Actual record ID. @return Current VM. */
    static std::shared_ptr<scripting::ScriptRuntime> CurrentVM(ModManager& manager,const std::string& id);
    /** @summary Retains exact actual-record scope, never issuing a grant. @param manager Original owner. @param id Actual record ID. @return Original scope. */
    static std::shared_ptr<runtime::RuntimeScope> CurrentScope(ModManager& manager,const std::string& id);
    /** @summary Installs a trusted observer on the actual VM before retirement. @param vm Current VM. @param observer Native lifecycle evidence sink. */
    static void Lifecycle(const std::shared_ptr<scripting::ScriptRuntime>& vm,std::function<void(scripting::ScriptPhase,std::uint64_t)> observer);
    /** @summary Installs actual accepted-action barrier; invoked off every publication guard. @param manager Original owner. @param callback Native gate. */
    static void RetirementBarrier(ModManager& manager,std::function<void()> callback);
    /** @summary Holds actual State gate for a bounded fixture witness, without calling another manager operation.
     * @param handle Original anchor. @param callback Bounded native gate. */
    static void HoldState(const ManagerClose::Handle& handle,const std::function<void()>& callback);
    /** @summary Observes actual closed-slot acknowledgement under control serialization. @param vm Exact VM. @return Native release flag. */
    static bool LaneReleased(const std::shared_ptr<scripting::ScriptRuntime>& vm);
};
#endif
}
