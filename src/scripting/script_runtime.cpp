#include "scripting/script_runtime.hpp"
#include "scripting/scheduler_publication.hpp"
#include "scripting/semantic_lua.hpp"
#include "mods/manager_commands.hpp"
#include <lua.h>
#include <lualib.h>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <filesystem>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <array>
#include <exception>
#include <type_traits>
#include <algorithm>
#include <cstring>
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY) && defined(_WIN32)
#include <Windows.h>
#endif

namespace sc13::scripting {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
namespace {
/** @summary One lane-local borrowed destination; it owns no native/runtime object and cannot queue observations. */
struct BudgetCapture final {
    CallbackBudgetTestAccess::Observation* output{};
    ScriptRuntime* runtime{};
    std::uint64_t generation{};
    std::uint32_t depth{};
    bool armed{};
    bool preparationEntry{};
    ScriptRuntime* expectedRuntime{};
};
static_assert(sizeof(BudgetCapture)<=512);
thread_local BudgetCapture budgetCapture;
/** @summary Actual native observer identity exists only around the original lifecycle invocation. */
struct BudgetObserverContext final {ScriptRuntime* runtime{};ScriptPhase phase{ScriptPhase::Create};};
thread_local BudgetObserverContext budgetObserver;
/** @summary Restore the previous real observer identity even after a nested observer or native exception. */
class BudgetObserverFrame final {
    BudgetObserverContext previous_;
public:
    /** @summary Publish only actual native invocation facts. @param runtime Original VM. @param phase Original phase. */
    BudgetObserverFrame(ScriptRuntime& runtime,ScriptPhase phase) noexcept:previous_(budgetObserver){budgetObserver={&runtime,phase};}
    /** @summary End this native invocation without retaining a runtime pointer. */
    ~BudgetObserverFrame(){budgetObserver=previous_;}
    BudgetObserverFrame(const BudgetObserverFrame&)=delete;
    BudgetObserverFrame& operator=(const BudgetObserverFrame&)=delete;
};
static_assert(sizeof(BudgetObserverFrame)<=64);
/** @summary Copy a native monotonic timestamp without formatting. @return Original steady-clock nanoseconds. */
std::int64_t BudgetNow() noexcept{return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
/** @summary Saturate diagnostic counters without altering allocation or watchdog decisions. @param target Fixed counter. @param amount Actual amount. @param output Original observation. */
void BudgetAdd(std::uint64_t& target,std::uint64_t amount,CallbackBudgetTestAccess::Observation& output) noexcept{
    if(amount>std::numeric_limits<std::uint64_t>::max()-target){target=std::numeric_limits<std::uint64_t>::max();output.overflow=true;}else target+=amount;
}
/** @summary Sample only the current thread outside the callback watchdog; raw cycles are never elapsed time. @return Independent availability/errors and raw values. */
CallbackBudgetTestAccess::NativeSample BudgetSample() noexcept{
    CallbackBudgetTestAccess::NativeSample result;
#if defined(_WIN32)
    const auto previousError=GetLastError();result.platformAvailable=true;result.thread=GetCurrentThreadId();
    FILETIME created{},exited{},kernel{},user{};
    result.cpuAvailable=GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)!=FALSE;
    if(result.cpuAvailable){result.kernel100ns=(std::uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime;result.user100ns=(std::uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime;}else result.cpuError=GetLastError();
    ULONG64 cycles{};result.cyclesAvailable=QueryThreadCycleTime(GetCurrentThread(),&cycles)!=FALSE;
    if(result.cyclesAvailable)result.cycles=cycles;else result.cycleError=GetLastError();
    SetLastError(previousError);
#endif
    return result;
}
/** @summary Clear the sole borrowed slot after publishing its terminal disposition. @param completion Actual capture ending. */
void BudgetClear(CallbackBudgetTestAccess::Completion completion) noexcept{
    auto* output=budgetCapture.output;
    budgetCapture=BudgetCapture{};
    if(output){output->completion=completion;output->borrowCleared=true;output->terminal=true;}
}
}
CallbackBudgetTestAccess::ArmResult CallbackBudgetTestAccess::Arm(std::uint64_t generation,ScriptPhase phase,Observation& output) noexcept{
    if(output.arm!=ArmResult::NotRequested)return ArmResult::AlreadyUsed;
    output.generation=generation;output.phase=phase;
    ArmResult result=ArmResult::Armed;
    if(!ScriptRuntime::OnScriptLane())result=ArmResult::WrongLane;
    else if(!generation)result=ArmResult::InvalidGeneration;
    else if(phase!=ScriptPhase::Enable)result=ArmResult::InvalidPhase;
    else if(SchedulerPublicationTestAccess::InsideFence())result=ArmResult::Fenced;
    else if(budgetCapture.output)result=ArmResult::Busy;
    output.arm=result;
    if(result!=ArmResult::Armed){output.terminal=true;return result;}
    budgetCapture.output=&output;budgetCapture.generation=generation;budgetCapture.armed=true;budgetCapture.preparationEntry=false;budgetCapture.expectedRuntime=nullptr;output.borrowCleared=false;return result;
}
CallbackBudgetTestAccess::ArmResult CallbackBudgetTestAccess::ArmPreparationEntry(std::uint64_t generation,Observation& output) noexcept{
    if(output.arm!=ArmResult::NotRequested)return ArmResult::AlreadyUsed;
    output.generation=generation;output.phase=ScriptPhase::Entry;
    ArmResult result=ArmResult::Armed;
    const auto* observed=budgetObserver.runtime;
    if(!ScriptRuntime::OnScriptLane())result=ArmResult::WrongLane;
    else if(!generation)result=ArmResult::InvalidGeneration;
    else if(!observed||budgetObserver.phase!=ScriptPhase::Entry||observed->bootstrap_||!observed->preparation_)result=ArmResult::InvalidPhase;
    else if(observed->vmGeneration_!=generation)result=ArmResult::InvalidGeneration;
    else if(SchedulerPublicationTestAccess::InsideFence())result=ArmResult::Fenced;
    else if(budgetCapture.output)result=ArmResult::Busy;
    output.arm=result;
    if(result!=ArmResult::Armed){output.terminal=true;return result;}
    budgetCapture.output=&output;budgetCapture.generation=generation;budgetCapture.armed=true;
    budgetCapture.preparationEntry=true;budgetCapture.expectedRuntime=budgetObserver.runtime;output.borrowCleared=false;return result;
}
bool CallbackBudgetTestAccess::Cancel(std::uint64_t generation) noexcept{
    if(!ScriptRuntime::OnScriptLane()||!budgetCapture.output||!budgetCapture.armed||budgetCapture.runtime||budgetCapture.generation!=generation)return false;
    BudgetClear(Completion::Cancelled);return true;
}
CallbackBudgetTestAccess::Role CallbackBudgetTestAccess::Begin(ScriptRuntime& runtime) noexcept{
    auto& capture=budgetCapture;
    if(capture.runtime&&capture.output){capture.output->nested=true;if(capture.depth==std::numeric_limits<std::uint32_t>::max()){capture.output->overflow=true;return Role::NestedOverflow;}++capture.depth;capture.output->maximumDepth=std::max(capture.output->maximumDepth,capture.depth);return Role::Nested;}
    if(!capture.output||!capture.armed)return Role::None;
    const bool authenticRole=capture.preparationEntry?
        capture.expectedRuntime==&runtime&&runtime.preparation_&&!runtime.bootstrap_&&capture.output->phase==ScriptPhase::Entry:
        runtime.bootstrap_;
    if(!authenticRole||runtime.vmGeneration_!=capture.generation||!ScriptRuntime::OnScriptLane()||SchedulerPublicationTestAccess::InsideFence()){
        capture.output->arm=ArmResult::MismatchedRuntime;BudgetClear(Completion::Cancelled);return Role::None;
    }
    capture.runtime=&runtime;capture.armed=false;capture.depth=1;
    auto& output=*capture.output;output.maximumDepth=1;output.budgetMs=runtime.budgetMs_;output.usedBefore=runtime.used_;output.peakUsed=runtime.used_;
#if defined(_WIN32)
    const auto previousError=GetLastError();
#endif
    output.preSamplingNs=BudgetNow();output.before=BudgetSample();
#if defined(_WIN32)
    SetLastError(previousError);
#endif
    return Role::Outer;
}
void CallbackBudgetTestAccess::Deadline(ScriptRuntime& runtime) noexcept{
    auto& capture=budgetCapture;if(capture.runtime!=&runtime||!capture.output)return;
    auto& output=*capture.output;output.deadlineNs=std::chrono::duration_cast<std::chrono::nanoseconds>(runtime.deadline_.time_since_epoch()).count();
    output.callbackStartNs=std::chrono::duration_cast<std::chrono::nanoseconds>((runtime.deadline_-std::chrono::milliseconds(runtime.budgetMs_)).time_since_epoch()).count();output.callbackStarted=true;
}
void CallbackBudgetTestAccess::End(ScriptRuntime& runtime,Role role,int status,bool nativeException) noexcept{
    auto& capture=budgetCapture;
    if(role==Role::Nested){if(capture.output&&capture.runtime&&capture.depth>1)--capture.depth;return;}
    if(role!=Role::Outer||capture.runtime!=&runtime||!capture.output)return;
#if defined(_WIN32)
    const auto previousError=GetLastError();
#endif
    auto& output=*capture.output;output.callbackEndNs=BudgetNow();output.luaStatus=status;output.usedAfter=runtime.used_;
    output.after=BudgetSample();output.postSamplingNs=BudgetNow();
    output.sameThread=output.before.thread!=0&&output.before.thread==output.after.thread;
    output.cpuOrderValid=output.sameThread&&output.before.cpuAvailable&&output.after.cpuAvailable&&output.before.kernel100ns<=output.after.kernel100ns&&output.before.user100ns<=output.after.user100ns;
    output.cycleOrderValid=output.sameThread&&output.before.cyclesAvailable&&output.after.cyclesAvailable&&output.before.cycles<=output.after.cycles;
    output.wallOrderValid=output.callbackStarted&&output.preSamplingNs<=output.callbackStartNs&&output.callbackStartNs<=output.deadlineNs&&output.callbackStartNs<=output.callbackEndNs&&output.callbackEndNs<=output.postSamplingNs;
    if(capture.depth!=1)output.nested=true;
#if defined(_WIN32)
    SetLastError(previousError);
#endif
    BudgetClear(nativeException?Completion::NativeException:Completion::LuaReturn);
}
CallbackBudgetTestAccess::Frame::Frame(ScriptRuntime& runtime) noexcept:runtime_(&runtime){role_=Begin(runtime);}
CallbackBudgetTestAccess::Frame::~Frame() noexcept{if(!finished_)Finish(-1,true);}
void CallbackBudgetTestAccess::Frame::Deadline() noexcept{if(role_==Role::Outer)CallbackBudgetTestAccess::Deadline(*runtime_);}
void CallbackBudgetTestAccess::Frame::Finish(int status,bool nativeException) noexcept{if(finished_)return;finished_=true;End(*runtime_,role_,status,nativeException);}
void CallbackBudgetTestAccess::Allocating(ScriptRuntime& runtime,bool hasPointer,std::size_t size) noexcept{
    auto& capture=budgetCapture;if(capture.runtime!=&runtime||!capture.output)return;auto& output=*capture.output;
    BudgetAdd(output.allocationCalls,1,output);BudgetAdd(output.requestedBytes,size,output);if(!size)BudgetAdd(output.freeCalls,1,output);else if(hasPointer)BudgetAdd(output.reallocationCalls,1,output);
}
void CallbackBudgetTestAccess::Allocated(ScriptRuntime& runtime,bool success) noexcept{
    auto& capture=budgetCapture;if(capture.runtime!=&runtime||!capture.output)return;auto& output=*capture.output;
    if(!success)BudgetAdd(output.allocationFailures,1,output);output.peakUsed=std::max(output.peakUsed,std::uint64_t(runtime.used_));
}
void CallbackBudgetTestAccess::Interrupting(const ScriptRuntime& runtime,int gc) noexcept{
    auto& capture=budgetCapture;if(capture.runtime!=&runtime||!capture.output)return;auto& output=*capture.output;
    BudgetAdd(gc>=0?output.gcInterruptEvents:output.instructionInterrupts,1,output);
}
void CallbackBudgetTestAccess::Exceeded(const ScriptRuntime& runtime,std::int64_t elapsed) noexcept{
    auto& capture=budgetCapture;if(capture.runtime!=&runtime||!capture.output)return;auto& output=*capture.output;
    output.exceeded=true;output.detectedElapsedValid=elapsed>=0;output.detectedElapsedUs=elapsed>=0?std::uint64_t(elapsed):0;output.detectedDeadlineNs=std::chrono::duration_cast<std::chrono::nanoseconds>(runtime.deadline_.time_since_epoch()).count();
}
#endif
/** @summary Retains exact scheduler ingress and lane-owned activity independently of the stack scheduler. */
struct ScriptSchedulerHost final {
    /** @summary Retains copied sinks and one fixed native admission latch. @param logs Log sink. @param patches Command sink. */
    ScriptSchedulerHost(ScriptScheduler::LogSink logs,ScriptScheduler::CommandSink patches)
        :log(std::move(logs)),commands(std::move(patches)) {}
    ScriptScheduler::LogSink log;
    ScriptScheduler::CommandSink commands;
    std::atomic_bool stopped{};
    std::shared_ptr<std::atomic_bool> closeAdmission{std::make_shared<std::atomic_bool>(false)};
    /** @summary Exact-host facts below are read and updated only under the existing lane mutex. */
    std::uint64_t activityRevision{1};
    std::size_t selected{},queued{},executing{};
    bool exhausted{};
    /** @summary Invalidates partial observations without wrapping native identity. */
    void Changed() noexcept {if(activityRevision==UINT64_MAX)exhausted=true;else ++activityRevision;}
};
/** @summary Shares one private interpreter lane across managed and standalone runtimes. */
class ScriptLane final {
public:
    /** @summary A caller-owned command uses the VM's permanently reserved control record. */
    struct Work final { void (*run)(void*); void* context; std::exception_ptr error; bool done{}; };
    /** @summary Starts the sole serial interpreter lane. */
    ScriptLane() : worker_([this] { Run(); }) {}
    /** @summary Joins after runtime-owned references have drained and closed every Lua VM. */
    ~ScriptLane() {
        { const std::scoped_lock lock(mutex_); stopping_ = true; }
        wake_.notify_all(); worker_.join();
    }
    /** @summary Reserves a VM identity and control/retirement record before allocating Lua. */
    std::size_t Reserve(ScriptRuntime* runtime) {
        const std::scoped_lock lock(mutex_);
        for (std::size_t i = 0; i < slots_.size(); ++i) if (!slots_[i].runtime) {
            auto generation = nextGeneration_.load();
            do { if (generation == UINT64_MAX) throw std::runtime_error("Script generation space exhausted"); }
            while (!nextGeneration_.compare_exchange_weak(generation, generation + 1));
            slots_[i].runtime = runtime; slots_[i].generation = generation + 1;
            runtime->vmGeneration_ = slots_[i].generation; return i;
        }
        throw std::runtime_error("Script VM/control capacity exhausted (128)");
    }
    /** @summary Releases a closed VM without allocating a teardown request. */
    void Release(std::size_t index) noexcept {
        const std::scoped_lock lock(mutex_);if(slots_[index].host)slots_[index].host->Changed();slots_[index] = {};
    }
    /** @summary Releases only the acknowledged original slot, never another generation.
     * @param index Original slot. @param vm Original runtime. @param generation Original identity. @return Exact native result. */
    runtime::ScopeResult ReleaseExact(std::size_t index,const ScriptRuntime* vm,std::uint64_t generation) noexcept {
        const std::scoped_lock lock(mutex_);
        if(index>=slots_.size())return runtime::ScopeResult::Stale;
        auto& slot=slots_[index];
        if(slot.runtime!=vm || slot.generation!=generation)return runtime::ScopeResult::Stale;
        if(slot.work || slot.executing)return runtime::ScopeResult::Indeterminate;
        if(slot.host)slot.host->Changed();slot={};return runtime::ScopeResult::Ok;
    }
    /** @summary Copies current original-host activity with a try-only L acquisition.
     * @param host Original host. @return Coherent exact-host counters without slot ownership. */
    SchedulerCloseObservation ObserveHost(const std::shared_ptr<ScriptSchedulerHost>& host) noexcept {
        SchedulerCloseObservation result;std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){result.result=runtime::ScopeResult::Busy;return result;}
        result.revision=host->activityRevision;result.selected=host->selected;result.queued=host->queued;result.executing=host->executing;
        result.result=!host->closeAdmission->load() || host->exhausted?runtime::ScopeResult::Denied:
            (host->selected || host->queued || host->executing?runtime::ScopeResult::NotReady:runtime::ScopeResult::Ok);
        return result;
    }
    /** @summary Qualifies the exact current target immediately before a release-only control.
     * @param vm Original VM protected by control and V. @param host Original scheduler host. @param generation Exact identity.
     * @return Try-only native admission. */
    runtime::ScopeResult QualifyClose(const ScriptRuntime& vm,const std::shared_ptr<ScriptSchedulerHost>& host,std::uint64_t generation) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return runtime::ScopeResult::Busy;
        if(vm.laneSlot_>=slots_.size())return runtime::ScopeResult::Stale;
        const auto& slot=slots_[vm.laneSlot_];
        if(slot.runtime!=&vm || slot.generation!=generation || vm.vmGeneration_!=generation)return runtime::ScopeResult::Stale;
        if(slot.host!=host || vm.schedulerHost_!=host || vm.bootstrap_ || !host->closeAdmission->load() || host->exhausted)return runtime::ScopeResult::Denied;
        if(slot.work || slot.executing || host->selected || host->queued || host->executing)return runtime::ScopeResult::Busy;
        return runtime::ScopeResult::Ok;
    }
    /** @summary Bind actual VM and slot together through the shared original-host algorithm.
     * @param index Original slot. @param vm Actual retained VM. @param host Original target host. */
    void Bind(std::size_t index,const std::shared_ptr<ScriptRuntime>& vm,const std::shared_ptr<ScriptSchedulerHost>& host) {
        const std::scoped_lock lock(mutex_);
        const auto result=BindLocked(index,vm,host);
        if(result==runtime::ScopeResult::Cancelled)throw std::runtime_error("Scheduler ordinary admission closed");
        if(result!=runtime::ScopeResult::Ok)throw std::runtime_error("Script host binding unavailable");
    }
    /** @summary Try exact preparation association without a partial VM host write on contention.
     * @param index Original slot. @param vm Actual candidate. @param host Original target host. @return Native binding result. */
    runtime::ScopeResult TryBind(std::size_t index,const std::shared_ptr<ScriptRuntime>& vm,const std::shared_ptr<ScriptSchedulerHost>& host) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return runtime::ScopeResult::Busy;
        return BindLocked(index,vm,host);
    }
    /** @summary Fences current dispatch destruction before the last off-lane scheduler anchor can disappear. */
    void WaitIdle() {
        std::unique_lock lock(mutex_); wake_.wait(lock, [this] { return activeTicks_ == 0; });
    }
    /** @summary Dispatches through the VM's reserved record; same-lane native control does not queue.
     * @param index Private slot. @param run Owned native control operation.
     * @param releaseOnly Closed native Stop/Close/Flush classification; never operational admission authority. */
    template<class Function>
    void Invoke(std::size_t index, Function&& run,bool releaseOnly=false) {
        if(OnLane()) {
            std::shared_ptr<ScriptSchedulerHost> host;
            {const std::scoped_lock lock(mutex_);host=slots_[index].host;if(!releaseOnly && host && host->closeAdmission->load())throw std::runtime_error("Scheduler ordinary admission closed");
                if(host){++host->executing;host->Changed();}}
            try{run();}catch(...){const std::scoped_lock lock(mutex_);if(host){--host->executing;host->Changed();}throw;}
            {const std::scoped_lock lock(mutex_);if(host){--host->executing;host->Changed();}}return;
        }
        Work work{[](void* context) { (*static_cast<std::remove_reference_t<Function>*>(context))(); }, std::addressof(run)};
        std::unique_lock lock(mutex_);
        if (stopping_ || !slots_[index].runtime || slots_[index].work || (!releaseOnly && slots_[index].host && slots_[index].host->closeAdmission->load()))
            throw std::runtime_error("Script control record unavailable");
        slots_[index].work = &work;
        if(slots_[index].host){++slots_[index].host->queued;slots_[index].host->Changed();}
        wake_.notify_all();
        wake_.wait(lock, [&] { return work.done; });
        if (work.error) std::rethrow_exception(work.error);
    }
    /** @summary Returns native lane ownership without exposing a Lua pointer. */
    static bool OnLane() noexcept { return current_ != nullptr; }
    /** @summary Tries exact host counters and at most 32 existing slot witnesses, with no callbacks or waiting.
     * @param host Original host. @param cursor Resumable slot cursor. @param revision Exact host generation. @return Partial native observation. */
    SchedulerCloseObservation PollClose(const std::shared_ptr<ScriptSchedulerHost>& host,std::size_t& cursor,std::uint64_t& revision) noexcept {
        SchedulerCloseObservation result;std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){result.result=runtime::ScopeResult::Busy;return result;}
        result.revision=host->activityRevision;result.selected=host->selected;result.queued=host->queued;result.executing=host->executing;
        if(!host->closeAdmission->load() || host->exhausted){result.result=runtime::ScopeResult::Denied;return result;}
        if(revision!=host->activityRevision){cursor=0;revision=host->activityRevision;}
        if(cursor>=slots_.size())cursor=0;
        for(;cursor<slots_.size() && result.checked<32;++cursor){
            ++result.checked;const auto& slot=slots_[cursor];
            if(slot.host==host && slot.runtime){++result.holders;result.generation=slot.generation;}
        }
        result.result=cursor==slots_.size() && !host->selected && !host->queued && !host->executing?runtime::ScopeResult::Ok:runtime::ScopeResult::NotReady;
        return result;
    }
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    /** @summary Hold actual L for a bounded native fixture gate. @param callback Native gate, never Lua or another lane operation. */
    void HoldPreparation(const std::function<void()>& callback){const std::scoped_lock lock(mutex_);callback();}
    /** @summary Observe original slot generation and same-host association under try L.
     * @param vm Actual retained VM. @param host Original expected host. @return Coherent native association only. */
    runtime::ScopeResult PreparationBinding(const std::shared_ptr<ScriptRuntime>& vm,const std::shared_ptr<ScriptSchedulerHost>& host) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return runtime::ScopeResult::Busy;
        if(vm->laneSlot_>=slots_.size())return runtime::ScopeResult::Stale;
        const auto& slot=slots_[vm->laneSlot_];
        if(slot.runtime!=vm.get() || slot.generation!=vm->vmGeneration_)return runtime::ScopeResult::Stale;
        if((slot.host && slot.host!=host) || (vm->schedulerHost_ && vm->schedulerHost_!=host))return runtime::ScopeResult::Denied;
        const auto published=slot.published.lock();
        if(!slot.host && !vm->schedulerHost_ && !published)return runtime::ScopeResult::NotReady;
        // @summary A partial association is an oracle failure, never evidence of clean unbound ownership.
        return slot.host==host && vm->schedulerHost_==host && published==vm?runtime::ScopeResult::Ok:runtime::ScopeResult::Conflict;
    }
    /** @summary Copies real slot and exact-host work witnesses. @param index Original slot.
     * @param generation Original nonreused VM identity. @param expected Original retained VM. @return Actual native work state. */
    SchedulerCloseObservation ObserveControl(std::size_t index,std::uint64_t generation,const ScriptRuntime* expected) {
        const std::scoped_lock lock(mutex_);const auto& slot=slots_[index];SchedulerCloseObservation result;
        if(slot.runtime!=expected || slot.generation!=generation){result.result=runtime::ScopeResult::Stale;return result;}
        result.result=runtime::ScopeResult::Ok;result.generation=slot.generation;result.queued=slot.work?1:0;result.executing=slot.executing?1:0;
        if(slot.host){result.revision=slot.host->activityRevision;result.selected=slot.host->selected;}return result;
    }
#endif
private:
    /** @summary Keeps private VM identity valid until synchronous close and release finish. */
    struct Slot final { ScriptRuntime* runtime{}; Work* work{}; std::uint64_t generation{}; std::weak_ptr<ScriptRuntime> published; std::shared_ptr<ScriptSchedulerHost> host; Work* executing{}; };
    /** @summary Mutate only original native association after all fallible identity/work checks under V/L.
     * @param index Original slot. @param vm Actual retained VM. @param host Original target host.
     * @return Ok, retained closed association, or unchanged rejection. */
    runtime::ScopeResult BindLocked(std::size_t index,const std::shared_ptr<ScriptRuntime>& vm,const std::shared_ptr<ScriptSchedulerHost>& host) noexcept {
        using runtime::ScopeResult;
        if(!vm || !host || index>=slots_.size())return ScopeResult::Stale;
        auto& slot=slots_[index];
        if(slot.runtime!=vm.get() || slot.generation!=vm->vmGeneration_)return ScopeResult::Stale;
        if((slot.host && slot.host!=host) || (vm->schedulerHost_ && vm->schedulerHost_!=host))return ScopeResult::Denied;
        // @summary A queued unbound Work has no host charge; never attach a new host before its actual acknowledgement.
        if(slot.work || slot.executing || vm->scheduled_.load() || (slot.host && slot.host->selected))return ScopeResult::Busy;
        if(slot.host!=host || vm->schedulerHost_!=host || slot.published.lock()!=vm){
            if(host->exhausted || host->activityRevision==UINT64_MAX)return ScopeResult::Denied;
            vm->schedulerHost_=host;slot.published=vm;slot.host=host;host->Changed();
        }
        // @summary A close race retains exact original association before rejection; metadata alone never activates a candidate.
        return host->closeAdmission->load() || host->stopped.load()?ScopeResult::Cancelled:ScopeResult::Ok;
    }
    /** @summary Rotates bounded VM visits without retaining the lane mutex during user code. */
    void Run() noexcept {
        current_ = this;
        std::size_t controlCursor{};
        std::size_t visitCursor{};
        bool controlTurn = true;
        auto nextTick = std::chrono::steady_clock::now();
        while (true) {
            Work* work{}; std::shared_ptr<ScriptRuntime> runtime; std::shared_ptr<ScriptSchedulerHost> activityHost;std::size_t workSlot{};std::uint64_t selectedRegistration{};
            {
                std::unique_lock lock(mutex_);
                for (std::size_t n = 0; (controlTurn || std::chrono::steady_clock::now() < nextTick) && n < slots_.size(); ++n) {
                    auto& slot = slots_[(controlCursor + n) % slots_.size()];
                    if (slot.work) {workSlot=(controlCursor+n)%slots_.size();work = slot.work;slot.executing=work;slot.work = nullptr;activityHost=slot.host;
                        if(activityHost){--activityHost->queued;++activityHost->executing;activityHost->Changed();}
                        controlCursor = (controlCursor + n + 1) % slots_.size(); break; }
                }
                if (!work && stopping_) break;
                if (!work && std::chrono::steady_clock::now() < nextTick) {
                    wake_.wait_until(lock, nextTick); continue;
                }
                if (!work) {
                    for (std::size_t n = 0; n < slots_.size(); ++n) {
                        auto& slot = slots_[visitCursor++ % slots_.size()];
                        if (slot.runtime && slot.runtime->scheduled_.load() && (!slot.host || !slot.host->closeAdmission->load())) {
                            runtime = slot.published.lock();
                            if (runtime) { selectedRegistration=runtime->registration_.load(); ++activeTicks_;activityHost=slot.host;if(activityHost){++activityHost->selected;activityHost->Changed();}break; }
                        }
                    }
                    nextTick = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
                }
            }
            if (work) {
                try { work->run(work->context); } catch (...) { work->error = std::current_exception(); }
                { const std::scoped_lock lock(mutex_);slots_[workSlot].executing=nullptr;work->done = true;if(activityHost){--activityHost->executing;activityHost->Changed();} }
                wake_.notify_all();
                controlTurn = false;
            } else if (runtime) {
                try {
                    const auto routed=mods::ManagerCommands::TrySchedule(*runtime,selectedRegistration);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
                    if(!routed && runtime->selectedBarrier_) runtime->selectedBarrier_();
#endif
                    if(!routed)runtime->ScheduledTick(selectedRegistration);
                } catch (...) { /* @summary Isolate host sink failures from the lane. */ }
                runtime.reset();
                { const std::scoped_lock lock(mutex_); --activeTicks_;if(activityHost){--activityHost->selected;activityHost->Changed();} }
                wake_.notify_all();
                controlTurn = true;
            }
        }
        current_ = nullptr;
    }
    inline static thread_local ScriptLane* current_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::array<Slot, 128> slots_{};
    inline static std::atomic_uint64_t nextGeneration_{};
    bool stopping_{};
    std::size_t activeTicks_{};
    std::thread worker_;
};
/** @summary Keeps the original scheduler mutex, sole owner map and stopped latch alive for retained plans. */
struct ScriptSchedulerState final {
    /** @summary A map node has nonreused native identity even while empty; no parallel owner store exists. */
    struct Node final {
        std::shared_ptr<ScriptRuntime> runtime;
        std::shared_ptr<const std::byte> identity;
        std::uint64_t revision{};
        bool sealed{};
        std::shared_ptr<const void> jointBinding;
    };
    std::mutex mutex;
    std::map<std::string,Node> runtimes;
    std::shared_ptr<runtime::RuntimeCommitDomain> domain;
    std::shared_ptr<ScriptLane> lane;
    std::shared_ptr<ScriptSchedulerHost> host;
};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Scalar fixture instrumentation lets allocator/destructor probes detect forbidden fence activity. */
static thread_local bool schedulerPublicationFence{};
#endif
namespace {
/** @summary Reuses one live lane; every VM retains it through final Lua close. */
std::shared_ptr<ScriptLane> AcquireScriptLane() {
    static std::mutex mutex;
    static std::weak_ptr<ScriptLane> current;
    const std::scoped_lock lock(mutex);
    auto lane = current.lock();
    if (!lane) { lane = std::make_shared<ScriptLane>(); current = lane; }
    return lane;
}
/** @summary Bounds combined declared VM budgets in the game's finite x86 address space. */
std::atomic<std::size_t> reservedScriptMemory{};
constexpr std::size_t maximumScriptMemory = 128U * 1024U * 1024U;
/** @summary Accepts the authoring project's full instance spelling only when the verified runtime width is sufficient. */
std::optional<core::Tgi> ParseScriptTgi(std::string_view text) {
    if (const auto compact = core::ParseTgi(text)) return compact;
    const auto separator = text.rfind(':');
    if (separator == text.npos) return {};
    auto instance = text.substr(separator + 1);
    if (instance.starts_with("0x") || instance.starts_with("0X")) instance.remove_prefix(2);
    if (instance.empty() || instance.size() > 16) return {};
    std::uint64_t value{};
    const auto parsed = std::from_chars(instance.data(), instance.data() + instance.size(), value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != instance.data() + instance.size() || value > UINT32_MAX) return {};
    auto prefix = core::ParseTgi(std::string(text.substr(0, separator + 1)) + "0");
    if (prefix) prefix->instance = static_cast<std::uint32_t>(value);
    return prefix;
}
}
void* ScriptRuntime::Allocate(void* context, void* pointer, std::size_t oldSize, std::size_t newSize) {
    auto& self = *static_cast<ScriptRuntime*>(context);
    if (!pointer) oldSize = 0;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    CallbackBudgetTestAccess::Allocating(self,pointer!=nullptr,newSize);
#endif
    if (!newSize) {
        self.used_ -= oldSize;
        std::free(pointer);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        CallbackBudgetTestAccess::Allocated(self,true);
#endif
        return nullptr;
    }
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    if (newSize > self.maximum_ - (self.used_ - oldSize)) CallbackBudgetTestAccess::Allocated(self,false);
#endif
    if (newSize > self.maximum_ - (self.used_ - oldSize)) return nullptr;
    void* result = std::realloc(pointer, newSize);
    if (result) self.used_ = self.used_ - oldSize + newSize;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    CallbackBudgetTestAccess::Allocated(self,result!=nullptr);
#endif
    return result;
}

void ScriptRuntime::Interrupt(lua_State* state, int gc) {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    CallbackBudgetTestAccess::Interrupting(*static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata),gc);
#endif
    if (gc >= 0) return;
    const auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    const auto now=std::chrono::steady_clock::now();
    if (now >= self.deadline_) {
        // @summary Bound numeric failure evidence without allocating or relaxing the original deadline.
        const auto began=self.deadline_-std::chrono::milliseconds(self.budgetMs_);
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(now-began).count();
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        CallbackBudgetTestAccess::Exceeded(self,elapsed);
#endif
        const auto bounded=static_cast<int>(std::min<std::int64_t>(elapsed,std::numeric_limits<int>::max()));
        luaL_error(state, "Simoder callback execution budget exceeded (budgetMs=%d elapsedUs=%d)",static_cast<int>(self.budgetMs_),bounded);
    }
}

int ScriptRuntime::Log(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    std::size_t length{};
    const char* message = luaL_checklstring(state, 1, &length);
    if (length > 2048) luaL_error(state, "Simoder log message exceeds 2048 bytes");
    if (self.logs_.size() < 128) self.logs_.emplace_back(message, length);
    else if (self.droppedLogs_ != UINT64_MAX) ++self.droppedLogs_;
    return 0;
}

int ScriptRuntime::HasCapability(lua_State* state) {
    const auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    const std::string_view name(luaL_checkstring(state, 1));
    lua_pushboolean(state, name == "script.luau" || name == "script.lifecycle" || name == "script.log" ||
        (name == "game.props.float" && (self.preparation_ || bool(self.host_.readFloat))));
    return 1;
}

ScriptPatchCommand ScriptRuntime::PatchTarget(lua_State* state) {
    const auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    std::size_t length{};
    const char* raw = luaL_checklstring(state, 1, &length);
    if (length > 128) luaL_error(state, "PROP resource reference exceeds 128 bytes");
    const std::string_view resource(raw, length);
    const auto alias = self.resourceAliases_.find(resource);
    const auto target = alias == self.resourceAliases_.end() ? ParseScriptTgi(resource) : std::optional(alias->second);
    if (!target || target->type != 0x00B1B104U)
        luaL_error(state, "Expected PROP TYPE:GROUP:INSTANCE; nonzero high instance bits are unsupported");
    ScriptPatchCommand command; command.target = *target;
    std::uint32_t property{};
    if (lua_type(state, 2) == LUA_TNUMBER) {
        const double id = lua_tonumber(state, 2);
        if (!std::isfinite(id) || id < 0 || id > std::numeric_limits<std::uint32_t>::max() || std::floor(id) != id)
            luaL_error(state, "Property ID must be an exact uint32");
        property = static_cast<std::uint32_t>(id);
    } else {
        raw = luaL_checklstring(state, 2, &length);
        std::string_view id(raw, length);
        if (const auto named = self.propertyAliases_.find(id); named != self.propertyAliases_.end()) {
            command.property = named->second.propertyId;
            command.propertyName = named->second.name;
            command.source = named->second.source;
            return command;
        }
        if (id.starts_with("0x")) id.remove_prefix(2);
        if (id.empty() || id.size() > 8) luaL_error(state, "Expected hexadecimal property ID");
        const auto parsed = std::from_chars(id.data(), id.data() + id.size(), property, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size()) luaL_error(state, "Invalid hexadecimal property ID");
    }
    command.property = property;
    return command;
}

int ScriptRuntime::GetFloat(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    if (self.preparation_) luaL_error(state, "Live PROP reads are unavailable during preparation");
    if (!self.host_.readFloat) luaL_error(state, "Game PROP API is unavailable in this host");
    const auto target = PatchTarget(state);
    std::optional<FloatObservation> observed;
    try { observed = self.host_.readFloat(target.target, target.property); }
    catch (const std::exception& error) { luaL_error(state, "PROP observation rejected: %s", error.what()); }
    if (!observed) { lua_pushnil(state); return 1; }
    lua_newtable(state);
    lua_pushnumber(state, observed->value); lua_setfield(state, -2, "value");
    lua_pushnumber(state, observed->vanilla); lua_setfield(state, -2, "vanilla");
    lua_pushnumber(state, static_cast<double>(observed->observedAtUnixMs)); lua_setfield(state, -2, "observedAtUnixMs");
    lua_pushnumber(state, static_cast<double>(observed->generation)); lua_setfield(state, -2, "generation");
    lua_pushnumber(state, static_cast<double>(observed->instances)); lua_setfield(state, -2, "instances");
    lua_pushlstring(state, observed->status.data(), observed->status.size()); lua_setfield(state, -2, "status");
    lua_pushboolean(state, true); lua_setfield(state, -2, "historical");
    lua_setreadonly(state, -1, true);
    return 1;
}

int ScriptRuntime::SetFloat(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    if ((!self.preparation_ && !self.host_.readFloat && !(self.commandRoute_ && self.sealed_ && self.committed_)) || self.stopping_) luaL_error(state, "PROP writes require an attached host outside onDisable");
    auto command = PatchTarget(state);
    const double value = luaL_checknumber(state, 3);
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) luaL_error(state, "Expected finite float32 value");
    command.value = static_cast<float>(value);
    if (self.commands_.size() >= self.commandLimit_) luaL_error(state, "At most 256 PROP commands per callback");
    self.commands_.push_back(command); return 0;
}

int ScriptRuntime::ClearFloat(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    if ((!self.preparation_ && !self.host_.readFloat && !(self.commandRoute_ && self.sealed_ && self.committed_)) || self.stopping_) luaL_error(state, "PROP writes require an attached host outside onDisable");
    const auto command = PatchTarget(state);
    if (self.commands_.size() >= self.commandLimit_) luaL_error(state, "At most 256 PROP commands per callback");
    self.commands_.push_back(command); return 0;
}

int ScriptRuntime::Time(lua_State* state) {
    const auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    lua_pushnumber(state, std::chrono::duration<double>(std::chrono::steady_clock::now() - self.start_).count());
    return 1;
}

int ScriptRuntime::Require(lua_State* state) {
    auto& self = *static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    self.Observe(ScriptPhase::Require);
    const std::string_view requested(luaL_checkstring(state, 1));
    if ((!requested.starts_with("./") && !requested.starts_with("../")) ||
        requested.find(':') != requested.npos || requested.find('\\') != requested.npos)
        luaL_error(state, "require expects a relative local Luau module");
    const std::string_view caller(lua_tostring(state, lua_upvalueindex(1)));
    const auto currentPath = std::filesystem::path(std::u8string(caller.begin(), caller.end()));
    auto path = (currentPath.parent_path() / std::filesystem::path(std::u8string(requested.begin(), requested.end()))).lexically_normal();
    if (path.extension().empty()) path += ".luau";
    const auto utf8 = path.generic_u8string();
    const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    if (!name.starts_with("scripts/") || !self.modules_.contains(name))
        luaL_error(state, "Local Luau module not found in compiled generation: %s (from %s)", name.c_str(), caller.data());
    if (const auto cached = self.moduleRefs_.find(name); cached != self.moduleRefs_.end()) {
        lua_getref(state, cached->second); return 1;
    }
    if (!self.loadingModules_.insert(name).second) luaL_error(state, "Cyclic Luau module dependency");
    const auto& bytecode = self.modules_.at(name);
    /** @summary Bind require to this module's environment so deferred callbacks retain their source directory. */
    lua_newtable(state);
    const int environment = lua_gettop(state);
    lua_pushlstring(state, name.data(), name.size());
    lua_pushcclosure(state, &Require, "require", 1);
    lua_setfield(state, environment, "require");
    lua_newtable(state);
    lua_pushvalue(state, LUA_GLOBALSINDEX);
    lua_setfield(state, -2, "__index");
    lua_setreadonly(state, -1, true);
    lua_setmetatable(state, environment);
    /** @summary Module globals differ from thread globals; bypass eagerly cached imports from the pinned VM. */
    lua_setsafeenv(state, environment, false);
    const auto chunk = "@" + name;
    int status = luau_load(state, chunk.c_str(), bytecode.data(), bytecode.size(), environment);
    lua_remove(state, environment);
    if (!status) status = lua_pcall(state, 0, 1, 0);
    self.loadingModules_.erase(name);
    if (status) lua_error(state);
    if (lua_isnil(state, -1)) luaL_error(state, "Luau module must return a value");
    self.moduleRefs_[name] = lua_ref(state, -1);
    return 1;
}

void ScriptRuntime::Call(int arguments, int results) {
    const auto commandCount = commands_.size();
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    CallbackBudgetTestAccess::Frame budgetFrame(*this);
#endif
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs_);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    budgetFrame.Deadline();
#endif
    int status{};
    try { status = lua_pcall(state_, arguments, results, 0); }
    catch (...) {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        budgetFrame.Finish(-1,true);
#endif
        commands_.resize(commandCount); lua_settop(state_, 0); throw; }
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    budgetFrame.Finish(status,false);
#endif
    if (status != LUA_OK) {
        commands_.resize(commandCount);
        const char* text = lua_tostring(state_, -1);
        const std::string error = text ? text : "Luau callback failed without a string diagnostic";
        lua_settop(state_, 0);
        throw std::runtime_error(error);
    }
}

int ScriptRuntime::Callback(const char* name) {
    lua_getfield(state_, -1, name);
    if (lua_isnil(state_, -1)) { lua_pop(state_, 1); return -1; }
    if (!lua_isfunction(state_, -1)) throw std::runtime_error(std::string(name) + " must be a function");
    const int reference = lua_ref(state_, -1);
    lua_pop(state_, 1);
    return reference;
}

ScriptRuntime::ScriptRuntime(std::string modId, const ScriptDefinition& definition, ScriptHost host)
    : ScriptRuntime(std::move(modId), definition, std::move(host), false) {}

ScriptRuntime::ScriptRuntime(BaseBootstrap, const ScriptDefinition& definition, ScriptHost host)
    : ScriptRuntime("simoder.base", definition, std::move(host), true) {}

ScriptRuntime::ScriptRuntime(Preparation, std::string modId, const ScriptDefinition& definition, ScriptHost host)
    : ScriptRuntime(std::move(modId), definition, std::move(host), false, true) {}

ScriptRuntime::ScriptRuntime(std::string modId, const ScriptDefinition& definition, ScriptHost host, bool bootstrap, bool preparation)
    : bootstrap_(bootstrap), preparation_(preparation), modId_(std::move(modId)), maximum_(definition.memoryBytes), budgetMs_(definition.budgetMs),
      modules_(definition.modules), resourceAliases_(definition.resourceAliases),
      propertyAliases_(definition.propertyAliases), host_(std::move(host)) {
    if (modId_ == "simoder.base" && !bootstrap_) throw std::runtime_error("Reserved base identity");
    if (host_.semantic) {
        // @summary Account the additional immutable host snapshot before copying; legacy VMs retain no duplicate generation.
        std::size_t snapshotBytes=sizeof(ScriptDefinition)+128;
        const auto add=[&](std::size_t count) {
            if (count>runtime::copied::Storage::OwnerLimit-snapshotBytes) throw std::runtime_error("Semantic definition snapshot limit");
            snapshotBytes+=count;
        };
        add(definition.bytecode.size()); add(definition.entry.size()); add(definition.language.size()); add(definition.revision.size());
        for(const auto& [name,code]:definition.modules) {add(128);add(name.size());add(code.size());}
        for(const auto& [name,target]:definition.resourceAliases) {static_cast<void>(target);add(128);add(name.size());}
        for(const auto& [name,property]:definition.propertyAliases) {add(sizeof(property)+128);add(name.size());add(property.name.size());add(property.source.size());add(property.description.size());add(property.unit.size());add(property.origin.size());}
        if(host_.semantic->bridge->Values()->Account(host_.semantic->storageOwner,snapshotBytes,loadedDefinitionAccounting_)!=runtime::copied::Status::Ok)
            throw std::runtime_error("Semantic definition storage exhausted");
        loadedDefinition_=std::make_shared<const ScriptDefinition>(definition);
    }
    lane_ = AcquireScriptLane();
    laneSlot_ = lane_->Reserve(this);
    try { lane_->Invoke(laneSlot_, [this,&definition] { Initialize(loadedDefinition_ ? *loadedDefinition_ : definition); }); }
    catch (...) { lane_->Release(laneSlot_); throw; }
}

bool ScriptRuntime::OnScriptLane() noexcept { return ScriptLane::OnLane(); }

void ScriptRuntime::Initialize(const ScriptDefinition& definition) {
    Observe(ScriptPhase::Create);
    if (maximum_ < 2U * 1024U * 1024U || maximum_ > 64U * 1024U * 1024U ||
        budgetMs_ < 1 || budgetMs_ > 20 || definition.bytecode.empty() ||
        definition.bytecode.size() > 8U * 1024U * 1024U)
        throw std::runtime_error("Invalid script generation or limits");
    const auto previous = reservedScriptMemory.fetch_add(maximum_);
    if (previous > maximumScriptMemory - maximum_) {
        reservedScriptMemory.fetch_sub(maximum_);
        throw std::runtime_error("Combined script memory budgets exceed the 128 MiB host limit");
    }
    reservedMemory_ = true;
    state_ = lua_newstate(&Allocate, this);
    if (!state_) { reservedScriptMemory.fetch_sub(maximum_); reservedMemory_ = false; throw std::runtime_error("Could not allocate Luau VM"); }
    try {
        lua_callbacks(state_)->userdata = this;
        lua_callbacks(state_)->interrupt = &Interrupt;
        luaL_openlibs(state_);
        for (const auto* name : {"getfenv", "setfenv", "debug", "require", "loadstring"}) {
            lua_pushnil(state_); lua_setglobal(state_, name);
        }
        lua_newtable(state_);
        lua_pushinteger(state_, 1); lua_setfield(state_, -2, "apiVersion");
        lua_pushlstring(state_, modId_.data(), modId_.size()); lua_setfield(state_, -2, "modId");
        lua_pushcfunction(state_, &Log, "simoder.log"); lua_setfield(state_, -2, "log");
        lua_pushcfunction(state_, &Time, "simoder.time"); lua_setfield(state_, -2, "time");
        lua_pushcfunction(state_, &HasCapability, "simoder.hasCapability"); lua_setfield(state_, -2, "hasCapability");
        if (!bootstrap_) {
        lua_newtable(state_);
        lua_pushcfunction(state_, &GetFloat, "simoder.props.getFloat"); lua_setfield(state_, -2, "getFloat");
        lua_pushcfunction(state_, &SetFloat, "simoder.props.setFloat"); lua_setfield(state_, -2, "setFloat");
        lua_pushcfunction(state_, &ClearFloat, "simoder.props.clearFloat"); lua_setfield(state_, -2, "clearFloat");
        lua_setreadonly(state_, -1, true); lua_setfield(state_, -2, "props");
        }
        lua_setreadonly(state_, -1, true); lua_setglobal(state_, "simoder");
        lua_pushcfunction(state_, &Log, "print"); lua_setglobal(state_, "print");
        lua_pushlstring(state_, definition.entry.data(), definition.entry.size());
        lua_pushcclosure(state_, &Require, "require", 1); lua_setglobal(state_, "require");
        if (host_.semantic) {
            for (const auto& [name,code]:host_.semanticModules) {
                if (modules_.contains(name)) throw std::runtime_error("Mod cannot replace verified base presentation modules");
                modules_.emplace(name,code);
            }
            runtime::copied::Reservation accounting;
            if (host_.semantic->bridge->Values()->Account(host_.semantic->storageOwner,sizeof(SemanticLua)+64,accounting)!=runtime::copied::Status::Ok)
                throw std::runtime_error("Semantic VM binding storage exhausted");
            semantics_=std::make_unique<SemanticLua>(*this,std::move(accounting)); semantics_->Install();
        }
        luaL_sandbox(state_);
        luaL_sandboxthread(state_);
        const auto chunk = "@" + definition.entry;
        if (luau_load(state_, chunk.c_str(), definition.bytecode.data(), definition.bytecode.size(), 0)) {
            const char* error = lua_tostring(state_, -1);
            throw std::runtime_error(error ? error : "Invalid Luau bytecode");
        }
        Observe(ScriptPhase::Entry); Call(0, 1);
        if (!lua_istable(state_, -1)) throw std::runtime_error("Script must return a lifecycle table");
        const int lifecycleRef=lua_ref(state_,-1);
        tickRef_ = Callback("onTick");
        disableRef_ = Callback("onDisable");
        const int enable = Callback("onEnable");
        lua_pop(state_, 1);
        if (enable != -1) { Observe(ScriptPhase::Enable); lua_getref(state_, enable); Call(0, 0); lua_unref(state_, enable); }
        if (semantics_ && bootstrap_) semanticQualified_=semantics_->Qualify(lifecycleRef);
        lua_unref(state_,lifecycleRef);
        running_ = true;
        PublishStatus();
    } catch (...) { semantics_.reset(); lua_close(state_); state_ = nullptr; reservedScriptMemory.fetch_sub(maximum_); reservedMemory_ = false; Observe(ScriptPhase::Close); throw; }
}

ScriptRuntime::~ScriptRuntime() {
    scheduled_.store(false);
    const std::scoped_lock control(controlMutex_);
    if(CloseAndReleaseLocked(false)!=runtime::ScopeResult::Ok)std::terminate();
}
runtime::ScopeResult ScriptRuntime::CloseAndReleaseLocked(bool requireInactive) {
    if(laneReleased_)return runtime::ScopeResult::Ok;
    if(requireInactive){const std::scoped_lock lock(mutex_);if(committed_ || scheduled_.load())return runtime::ScopeResult::Denied;}
    lane_->Invoke(laneSlot_,[this]{Close();},true);
    const auto result=lane_->ReleaseExact(laneSlot_,this,vmGeneration_);
    if(result==runtime::ScopeResult::Ok)laneReleased_=true;
    return result;
}
void ScriptRuntime::Close() noexcept {
    try { Stop(); }
    catch (...) { CaptureDiagnostic("Script retirement failed; forcing VM close"); }
    bool closed{};
    {
        const std::scoped_lock lock(mutex_);
        committed_ = false; running_ = false; stopping_ = true; commands_.clear();
        if (state_) { semantics_.reset(); lua_close(state_); state_ = nullptr; closed = true; }
        if (reservedMemory_) { reservedScriptMemory.fetch_sub(maximum_); reservedMemory_ = false; }
        PublishStatus();
    }
    if (closed) Observe(ScriptPhase::Close);
}
void ScriptRuntime::Commit() {
    const std::scoped_lock lock(mutex_);
    if(sealed_ || preparation_) throw std::runtime_error("Typed scheduler publication required");
    committed_ = true;
    disableOwed_ = true;
    if (semanticQualified_ && host_.semantic) host_.semantic->bridge->Publish(vmGeneration_);
    const std::scoped_lock statusLock(statusMutex_);
    publishedStatus_.running = running_;
}

bool ScriptRuntime::ExecutionEligible(std::uint64_t selectedRegistration) const noexcept {
    return running_ && committed_ && !stopping_ && state_ && (!schedulerHost_ || !schedulerHost_->closeAdmission->load()) &&
        (!sealed_ || (!retired_ && !stopRequested_.load() && schedulerHost_ && !schedulerHost_->stopped.load() && publicationDomain_ && !publicationDomain_->IsStopping() &&
            scheduled_.load() && selectedRegistration!=0 && registration_.load()==selectedRegistration));
}
void ScriptRuntime::Tick(double deltaSeconds) {
    const auto selectedRegistration=registration_.load();
    if (!OnScriptLane()) {
        const std::scoped_lock control(controlMutex_);
        if(laneReleased_) return;
        lane_->Invoke(laneSlot_, [this, deltaSeconds,selectedRegistration] { TickSelected(deltaSeconds,selectedRegistration); }); return;
    }
    TickSelected(deltaSeconds,selectedRegistration);
}
void ScriptRuntime::TickSelected(double deltaSeconds,std::uint64_t selectedRegistration) {
    const std::scoped_lock lock(mutex_);
    TickSelectedLocked(deltaSeconds,selectedRegistration);
}
void ScriptRuntime::TickSelectedLocked(double deltaSeconds,std::uint64_t selectedRegistration) {
    if (semantics_ && ExecutionEligible(selectedRegistration)) {
        try {semantics_->Pump();}
        catch (...) {lua_settop(state_,0);CaptureDiagnostic("SEMANTIC_PUMP_FAILED");running_=false;semantics_->Stop();PublishStatus();}
    }
    if (!ExecutionEligible(selectedRegistration) || tickRef_ == -1) return;
    const auto now = std::chrono::steady_clock::now();
    if (deltaSeconds < 0) deltaSeconds = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_ = now;
    try {
        Observe(ScriptPhase::Tick); lua_getref(state_, tickRef_); lua_pushnumber(state_, deltaSeconds); Call(1, 0);
    } catch (const std::exception& error) { running_ = false; CaptureDiagnostic(error.what()); if (semantics_) semantics_->Stop(); }
    catch (...) { running_ = false; CaptureDiagnostic("Script callback failed with a nonstandard exception"); if (semantics_) semantics_->Stop(); }
    PublishStatus();
}

void ScriptRuntime::InvokeSelectedTick(std::uint64_t registration,std::uint64_t generation,const mods::ManagerCommandRoute* route,CapturedTick& transfer) {
    if(OnScriptLane())throw std::runtime_error("Automatic command tick requires native worker");
    const std::scoped_lock control(controlMutex_);
    if(laneReleased_)return;
    lane_->Invoke(laneSlot_,[&]{
        CaptureSelectedTick(registration,generation,route,transfer);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        if(transfer.executed&&transfer.beforeAcknowledgment)(*transfer.beforeAcknowledgment)();
#endif
    });
}
void ScriptRuntime::CaptureSelectedTick(std::uint64_t registration,std::uint64_t generation,const mods::ManagerCommandRoute* route,CapturedTick& transfer) {
    const std::scoped_lock lock(mutex_);
    if(!OnScriptLane()||transfer.executed||!transfer.prior.empty()||!transfer.output.empty()||
        commandRoute_.get()!=route||vmGeneration_!=generation||!ExecutionEligible(registration)||
        std::chrono::steady_clock::now()-lastTick_<std::chrono::milliseconds(50))return;
    if(commands_.size()>256)std::terminate();
    const auto originalLimit=commandLimit_;
    transfer.prior.swap(commands_);commandLimit_=256-transfer.prior.size();
    /** @summary Restore ordinary pending ownership and capture exact output even on unexpected native exceptions. */
    struct Restore final {
        ScriptRuntime& vm;CapturedTick& transfer;std::size_t limit;
        /** @summary Swaps cannot allocate, drop permission owners or race another producer under V. */
        ~Restore(){vm.commands_.swap(transfer.output);vm.commands_.swap(transfer.prior);vm.commandLimit_=limit;}
    } restore{*this,transfer,originalLimit};
    transfer.executed=true;
    TickSelectedLocked(-1,registration);
}

void ScriptRuntime::Stop() {
    stopRequested_.store(true);
    scheduled_.store(false);
    if (!OnScriptLane()) {
        const std::scoped_lock control(controlMutex_);
        if(laneReleased_) return;
        lane_->Invoke(laneSlot_, [this] { Stop(); },true); return;
    }
    const std::scoped_lock lock(mutex_);
    if (!committed_ && !disableOwed_) { if(sealed_) {stopping_=true;retired_=true;running_=false;PublishStatus();} return; }
    committed_ = false;
    running_ = false;
    stopping_ = true;
    retired_=sealed_;
    const bool disable=std::exchange(disableOwed_,false);
    if(host_.semantic){host_.semantic->closed=true;host_.semantic->scope->RequestStop(runtime::StopReason::ExplicitShutdown);if(bootstrap_)host_.semantic->bridge->Close();}
    commands_.clear();
    if (disable && disableRef_ != -1) {
        try { Observe(ScriptPhase::Disable); lua_getref(state_, disableRef_); Call(0, 0); }
        catch (const std::exception& error) { CaptureDiagnostic(error.what()); }
        catch (...) { CaptureDiagnostic("Script onDisable failed with a nonstandard exception"); }
    }
    if (semantics_) semantics_->Stop();
    PublishStatus();
}

ScriptStatus ScriptRuntime::Status() const {
    const std::scoped_lock lock(statusMutex_);
    return {publishedStatus_.running, publishedStatus_.memoryBytes,
        std::string(publishedStatus_.diagnostic.data(), publishedStatus_.diagnosticLength),
        publishedStatus_.generation, publishedStatus_.droppedLogs};
}
void ScriptRuntime::CaptureDiagnostic(std::string_view diagnostic) noexcept {
    diagnosticLength_ = std::min(diagnostic.size(), diagnostic_.size());
    if (diagnosticLength_) std::memcpy(diagnostic_.data(), diagnostic.data(), diagnosticLength_);
}
void ScriptRuntime::PublishStatus() noexcept {
    try {
        const std::scoped_lock lock(statusMutex_);
        publishedStatus_.running = running_ && committed_;
        publishedStatus_.memoryBytes = used_;
        publishedStatus_.diagnostic = diagnostic_;
        publishedStatus_.diagnosticLength = diagnosticLength_;
        publishedStatus_.generation = vmGeneration_;
        publishedStatus_.droppedLogs = droppedLogs_;
    } catch (...) { /* @summary Diagnostic publication cannot prevent retirement or VM release. */ }
}
void ScriptRuntime::Observe(ScriptPhase phase) noexcept {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    BudgetObserverFrame budgetObserverFrame(*this,phase);
#endif
    try { if (host_.lifecycle) host_.lifecycle(phase, vmGeneration_); }
    catch (...) { /* @summary Diagnostic observers cannot change committed runtime semantics. */ }
}

std::vector<std::string> ScriptRuntime::DrainLogs() {
    const std::scoped_lock lock(mutex_);
    if (!committed_ && running_) return {};
    auto logs = std::move(logs_); logs_.clear(); return logs;
}
std::vector<SemanticDiagnostic> ScriptRuntime::SemanticDiagnostics() const {
    const std::scoped_lock lock(mutex_);return semantics_?semantics_->Diagnostics():std::vector<SemanticDiagnostic>{};
}

std::vector<ScriptPatchCommand> ScriptRuntime::TakePatchCommands() {
    const std::scoped_lock lock(mutex_);
    auto result = std::move(commands_); commands_.clear(); return result;
}
void ScriptRuntime::Fail(std::string diagnostic) {
    stopRequested_.store(true);
    const std::scoped_lock lock(mutex_); running_ = false; commands_.clear(); CaptureDiagnostic(diagnostic);
    if(sealed_) {committed_=false;stopping_=true;retired_=true;scheduled_.store(false);}
    // @summary Non-Lua admission closes immediately; registry references remain until the existing lane retires them.
    if (host_.semantic) {host_.semantic->closed=true;host_.semantic->scope->RequestStop(runtime::StopReason::ActivationFailed);if(bootstrap_)host_.semantic->bridge->Close();}
    PublishStatus();
}

void ScriptRuntime::ScheduledTick(std::uint64_t selectedRegistration) {
    if (!scheduled_.load() || !schedulerHost_ || schedulerHost_->stopped.load()) return;
    std::vector<ScriptPatchCommand> commands;
    bool sealed{};
    {
        const std::scoped_lock lock(mutex_);
        // @summary Revalidate routelessness across the initial snapshot/install race; never run a new route inline.
        if(commandRoute_||!ExecutionEligible(selectedRegistration) || std::chrono::steady_clock::now()-lastTick_<std::chrono::milliseconds(50)) return;
        TickSelectedLocked(-1,selectedRegistration);
        commands.swap(commands_);sealed=sealed_;
    }
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    const bool hadCommands=!commands.empty();
#endif
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    if(extractedBarrier_ && hadCommands) extractedBarrier_();
#endif
    // @summary Sealed dispatch never falls back to the textual legacy sink or mutates successor diagnostics.
    if(!sealed && !commands.empty()) {
        std::string error;
        const auto borrowed = shared_from_this();
        if (schedulerHost_->stopped.load()) return;
        if (!schedulerHost_->commands || !schedulerHost_->commands(modId_, borrowed, commands, error))
            Fail("PROP transaction rejected: " + error);
    }
    for (const auto& message : DrainLogs()) if (!schedulerHost_->stopped.load() && schedulerHost_->log) schedulerHost_->log(modId_, message);
}

ScriptScheduler::ScriptScheduler(LogSink sink, CommandSink commands)
    : state_(std::make_shared<ScriptSchedulerState>()), lane_(AcquireScriptLane()),
      host_(std::make_shared<ScriptSchedulerHost>(std::move(sink),std::move(commands))) {
    state_->lane=lane_;state_->host=host_;
}
ScriptScheduler::~ScriptScheduler() { Shutdown(); }
runtime::ScopeResult ScriptScheduler::BindPreparation(const std::shared_ptr<ScriptRuntime>& vm) noexcept {
    using runtime::ScopeResult;
    if(!vm)return ScopeResult::Invalid;
    if(ScriptRuntime::OnScriptLane())return ScopeResult::Denied;
    std::unique_lock guard(vm->mutex_,std::try_to_lock);if(!guard.owns_lock())return ScopeResult::Busy;
    if(!vm->preparation_ || vm->bootstrap_ || vm->lane_!=lane_ || !vm->state_ || !vm->running_ || vm->committed_ || vm->sealed_ ||
        vm->scheduled_.load() || vm->stopping_ || vm->retired_ || vm->stopRequested_.load() || vm->laneReleased_.load() ||
        !vm->host_.semantic || !vm->host_.semantic->scope || vm->host_.semantic->ScriptGeneration()!=vm->vmGeneration_ ||
        (vm->schedulerHost_ && vm->schedulerHost_!=host_))return ScopeResult::Denied;
    return lane_->TryBind(vm->laneSlot_,vm,host_);
}
std::shared_ptr<std::atomic_bool> ScriptScheduler::CloseAdmission() const noexcept{return host_->closeAdmission;}
SchedulerCloseObservation ScriptScheduler::PollClose(std::size_t& cursor,std::uint64_t& revision) noexcept{return lane_->PollClose(host_,cursor,revision);}
SchedulerCloseObservation ScriptScheduler::ObserveCloseActivity() noexcept{return lane_->ObserveHost(host_);}
runtime::ScopeResult ScriptScheduler::QualifyCloseLocked(const std::shared_ptr<ScriptSchedulerState>& anchor,
    const std::shared_ptr<ScriptRuntime>& vm,std::uint64_t generation,bool preparation) noexcept {
    using runtime::ScopeResult;
    if(vm->vmGeneration_!=generation)return ScopeResult::Stale;
    if(vm->bootstrap_ || vm->schedulerHost_!=anchor->host || vm->lane_!=anchor->lane)return ScopeResult::Denied;
    if(preparation && (!vm->preparation_ || vm->committed_ || vm->sealed_ || vm->scheduled_.load()))return ScopeResult::Denied;
    return vm->laneReleased_?ScopeResult::Ok:vm->lane_->QualifyClose(*vm,anchor->host,generation);
}
runtime::ScopeResult ScriptScheduler::InspectCloseTarget(const std::shared_ptr<ScriptSchedulerState>& anchor,
    const std::shared_ptr<ScriptRuntime>& vm,std::uint64_t generation,bool preparation) noexcept {
    using runtime::ScopeResult;if(!anchor || !vm || ScriptRuntime::OnScriptLane())return ScopeResult::Invalid;
    try{
        std::unique_lock control(vm->controlMutex_,std::try_to_lock);if(!control.owns_lock())return ScopeResult::Busy;
        std::unique_lock lock(vm->mutex_,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
        return QualifyCloseLocked(anchor,vm,generation,preparation);
    }catch(...){return ScopeResult::Indeterminate;}
}
runtime::ScopeResult ScriptScheduler::CloseCurrent(const std::shared_ptr<ScriptSchedulerState>& anchor,
    const std::shared_ptr<ScriptRuntime>& vm,std::uint64_t generation,bool& closedNow) noexcept {
    using runtime::ScopeResult;closedNow=false;
    if(!anchor || !vm || ScriptRuntime::OnScriptLane() || (anchor->domain && anchor->domain->WriterActiveOnCurrentThread()))return ScopeResult::Denied;
    try{
        std::unique_lock control(vm->controlMutex_,std::try_to_lock);if(!control.owns_lock())return ScopeResult::Busy;
        std::unique_lock lock(vm->mutex_,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
        const auto admitted=QualifyCloseLocked(anchor,vm,generation,false);if(admitted!=ScopeResult::Ok)return admitted;
        if(vm->laneReleased_)return ScopeResult::Ok;
        vm->stopRequested_=true;vm->scheduled_=false;lock.unlock();
        const auto result=vm->CloseAndReleaseLocked(false);closedNow=result==ScopeResult::Ok;return result;
    }catch(...){return ScopeResult::Indeterminate;}
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
void ScriptScheduler::HoldPreparationLane(const std::function<void()>& callback){
    if(ScriptRuntime::OnScriptLane())throw std::runtime_error("Preparation lane hold requires an off-lane caller");
    lane_->HoldPreparation(callback);
}
runtime::ScopeResult ScriptScheduler::PreparationBinding(const std::shared_ptr<ScriptRuntime>& vm) noexcept {
    if(!vm)return runtime::ScopeResult::Invalid;
    std::unique_lock guard(vm->mutex_,std::try_to_lock);if(!guard.owns_lock())return runtime::ScopeResult::Busy;
    if(vm->lane_!=lane_)return runtime::ScopeResult::Denied;
    return lane_->PreparationBinding(vm,host_);
}
void ScriptScheduler::InvokeCloseControl(const std::shared_ptr<ScriptRuntime>& vm,const std::function<void()>& callback){
    if(!vm || !callback)throw std::invalid_argument("Exact VM and native control required");
    auto invoke=[&callback]{callback();};
    if(ScriptRuntime::OnScriptLane()){
        if(vm->laneReleased_)throw std::runtime_error("Original VM slot released");
        vm->lane_->Invoke(vm->laneSlot_,invoke);return;
    }
    const std::scoped_lock control(vm->controlMutex_);
    if(vm->laneReleased_)throw std::runtime_error("Original VM slot released");
    vm->lane_->Invoke(vm->laneSlot_,invoke);
}
SchedulerCloseObservation ScriptScheduler::ObserveCloseControl(const std::shared_ptr<ScriptRuntime>& vm){
    if(!vm)return {};return vm->lane_->ObserveControl(vm->laneSlot_,vm->vmGeneration_,vm.get());
}
#endif
void ScriptScheduler::Prepare(const std::shared_ptr<ScriptRuntime>& vm) {
    if(ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler mutation from a script callback is unavailable");
    if(host_->closeAdmission->load())throw std::runtime_error("Scheduler ordinary admission closed");
    if(vm) {
        const std::scoped_lock lock(vm->mutex_);
        if(vm->preparation_ || vm->schedulerHost_ || vm->scheduled_.load() || vm->committed_ || vm->stopping_ || vm->laneReleased_)
            throw std::runtime_error("Script runtime is already bound or unavailable");
        lane_->Bind(vm->laneSlot_,vm,host_);
    }
}
void ScriptScheduler::Shutdown() {
    if(ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler shutdown from a script callback is unavailable");
    std::map<std::string,ScriptSchedulerState::Node> current;
    {
        const std::scoped_lock lock(state_->mutex);
        if(state_->host->stopped) return;
        state_->host->stopped=true;current.swap(state_->runtimes);
        for(auto& [owner,node]:current) if(node.runtime) node.runtime->scheduled_.store(false);
    }
    lane_->WaitIdle();
    for(auto& [owner,node]:current) if(node.runtime) {
        auto& vm=node.runtime;vm->Stop();Flush(owner,vm);
        const std::scoped_lock control(vm->controlMutex_);
        if(vm->CloseAndReleaseLocked(false)!=runtime::ScopeResult::Ok)throw std::runtime_error("Original VM release invariant failed");
    }
}
void ScriptScheduler::Flush(const std::string& owner,const std::shared_ptr<ScriptRuntime>& vm) {
    if(ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler flush from a script callback is unavailable");
    if(!vm) return;
    try {
        const std::scoped_lock control(vm->controlMutex_);
        if(vm->laneReleased_) return;
        vm->lane_->Invoke(vm->laneSlot_,[&]{for(const auto& message:vm->DrainLogs()) if(host_->log) host_->log(owner,message);},true);
    } catch(...) { /* @summary Sink failure cannot prevent retirement. */ }
}
void ScriptScheduler::Put(const std::string& owner,std::shared_ptr<ScriptRuntime> vm) {
    if(ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler mutation from a script callback is unavailable");
    bool needsPrepare{};
    if(vm) {
        const std::scoped_lock lock(vm->mutex_);
        if(vm->sealed_ || vm->preparation_ || (vm->schedulerHost_ && vm->schedulerHost_!=host_)) throw std::runtime_error("Foreign or typed runtime cannot use textual publication");
        needsPrepare=!vm->schedulerHost_;
    }
    if(needsPrepare) Prepare(vm);
    std::shared_ptr<ScriptRuntime> old;
    {
        const std::scoped_lock lock(state_->mutex);
        if(state_->host->stopped || state_->host->closeAdmission->load()) throw std::runtime_error("Script scheduler is stopping");
        auto& node=state_->runtimes.at(owner);
        std::unique_lock<std::mutex> vmGuard;
        if(vm) {vmGuard=std::unique_lock(vm->mutex_,std::try_to_lock);if(!vmGuard.owns_lock()) throw std::runtime_error("Script runtime is busy");}
        if(node.sealed || (vm && (vm->sealed_ || vm->preparation_ || vm->schedulerHost_!=host_))) throw std::runtime_error("Typed scheduler publication required");
        old=std::move(node.runtime);if(old) old->scheduled_.store(false);
        node.runtime=std::move(vm);++node.revision;
        if(node.runtime) node.runtime->scheduled_.store(true);
    }
}
void ScriptScheduler::Reserve(const std::string& owner) {
    if(ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler mutation from a script callback is unavailable");
    const std::scoped_lock lock(state_->mutex);
    if(state_->host->stopped || state_->host->closeAdmission->load()) throw std::runtime_error("Script scheduler is stopping");
    if(!state_->runtimes.contains(owner) && state_->runtimes.size()>=128) throw std::runtime_error("Script scheduler owner capacity exhausted");
    if(!state_->runtimes.contains(owner)) {
        ScriptSchedulerState::Node node;node.identity=std::make_shared<const std::byte>();state_->runtimes.emplace(owner,std::move(node));
    }
}
void ScriptScheduler::ReleaseReservation(const std::string& owner) {
    const std::scoped_lock lock(state_->mutex);
    const auto found=state_->runtimes.find(owner);
    if(found!=state_->runtimes.end() && found->second.sealed) throw std::runtime_error("Typed scheduler publication required");
    if(found!=state_->runtimes.end() && !found->second.runtime) state_->runtimes.erase(found);
}
void ScriptScheduler::Remove(const std::string& owner,bool retainReservation) {
    if(ScriptRuntime::OnScriptLane()) throw std::runtime_error("Scheduler mutation from a script callback is unavailable");
    std::shared_ptr<ScriptRuntime> old;
    {
        const std::scoped_lock lock(state_->mutex);
        if(const auto found=state_->runtimes.find(owner);found!=state_->runtimes.end()) {
            if(found->second.sealed) throw std::runtime_error("Typed scheduler publication required");
            if(found->second.runtime) found->second.runtime->scheduled_.store(false);
            old=std::move(found->second.runtime);if(!retainReservation) state_->runtimes.erase(found);else ++found->second.revision;
        }
    }
}

runtime::ScopeResult ScriptScheduler::PreparePublication(const std::string& owner,const std::shared_ptr<ScriptRuntime>& candidate,
    SchedulerPublicationPurpose purpose,SchedulerPublicationPlan& output,const std::shared_ptr<const void>& preparationProof) {
    using runtime::ScopeResult;
    if(ScriptRuntime::OnScriptLane() || owner.empty() || output.state_) return ScopeResult::Invalid;
    if(purpose!=SchedulerPublicationPurpose::Activate && purpose!=SchedulerPublicationPurpose::Handoff && purpose!=SchedulerPublicationPurpose::Remove) return ScopeResult::Invalid;
    if((purpose==SchedulerPublicationPurpose::Remove)!=!candidate) return ScopeResult::Invalid;
    std::shared_ptr<runtime::RuntimeCommitDomain> domain;
    // @summary Initial scheduler observation is try-only, before any candidate preparation effects.
    {std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;domain=state_->domain;}
    if(!domain || domain->WriterActiveOnCurrentThread() || domain->IsStopping()) return ScopeResult::Denied;
    // @summary Host/lane binding is completed outside D/S/V/T publication reservations.
    if(candidate) {
        {const std::scoped_lock lock(candidate->mutex_);
            if(candidate->bootstrap_ || (candidate->preparation_ && (!preparationProof || candidate->preparationProof_!=preparationProof)) || (!candidate->preparation_ && preparationProof) || candidate->modId_!=owner || candidate->committed_ || !candidate->running_ || candidate->stopping_ ||
                !candidate->state_ || candidate->sealed_ || candidate->laneReleased_.load() || candidate->stopRequested_.load() ||
                (candidate->schedulerHost_ && candidate->schedulerHost_!=host_) || candidate->lane_.get()!=lane_.get()) return ScopeResult::Denied;}
        bool bound{};{const std::scoped_lock lock(candidate->mutex_);bound=bool(candidate->schedulerHost_);}
        if(!bound) {
            if(!preparationProof)Prepare(candidate);
            else {const std::scoped_lock vm(candidate->mutex_);lane_->Bind(candidate->laneSlot_,candidate,host_);}
        }
    }
    SchedulerPublicationPlan plan;
    plan.state_=state_;plan.domain_=domain;plan.owner_=owner;plan.candidate_=candidate;plan.purpose_=purpose;plan.preparationProof_=preparationProof;
    if(candidate) {plan.candidateGeneration_=candidate->vmGeneration_;plan.candidateSlot_=candidate->laneSlot_;}
    // @summary Reserve the node allocator/key/token before taking D or S; a node-handle insertion cannot allocate.
    std::map<std::string,ScriptSchedulerState::Node> staging;
    if(purpose==SchedulerPublicationPurpose::Activate) {
        ScriptSchedulerState::Node node;node.identity=std::make_shared<const std::byte>();node.sealed=true;
        staging.emplace(owner,std::move(node));
    }
    {
        auto writer=domain->ReserveWriter();if(!writer.Acquired()) return ScopeResult::Busy;
        std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock()) return ScopeResult::Busy;
        if(state_->host->stopped || state_->host->closeAdmission->load() || !writer.CanAdvance()) return ScopeResult::Cancelled;
        auto found=state_->runtimes.find(owner);
        if(found==state_->runtimes.end()) {
            if(purpose!=SchedulerPublicationPurpose::Activate || state_->runtimes.size()>=128) return ScopeResult::Denied;
            found=state_->runtimes.insert(staging.extract(staging.begin())).position;
        }
        auto& node=found->second;
        if(!node.sealed || (purpose==SchedulerPublicationPurpose::Activate)!=!node.runtime) return ScopeResult::Conflict;
        plan.node_=node.identity;plan.jointBinding_=node.jointBinding;plan.revision_=node.revision;plan.domainRevision_=domain->Revision();plan.old_=node.runtime;
        if(plan.old_) {plan.oldGeneration_=plan.old_->vmGeneration_;plan.oldSlot_=plan.old_->laneSlot_;}
        if(candidate) {
            std::unique_lock vm(candidate->mutex_,std::try_to_lock);if(!vm.owns_lock()) return ScopeResult::Busy;
            if(candidate->sealed_ || candidate->committed_ || candidate->stopping_ || !candidate->running_ || candidate->stopRequested_.load() ||
                candidate->laneReleased_.load() || !candidate->state_ || candidate->schedulerHost_!=host_ || candidate->lane_!=lane_) return ScopeResult::Denied;
            candidate->publicationDomain_=domain;candidate->publicationNode_=node.identity;candidate->sealed_=true;
        }
    }
    output=std::move(plan);return ScopeResult::Ok;
}

runtime::ScopeResult ScriptScheduler::PublishPublication(SchedulerPublicationPlan& plan) {
    using runtime::ScopeResult;
    if(ScriptRuntime::OnScriptLane() || !plan.state_ || !plan.domain_ || plan.consumed_ || plan.preparationProof_) return ScopeResult::Invalid;
    auto writer=plan.domain_->ReserveWriter();if(!writer.Acquired()) return ScopeResult::Busy;
    SchedulerPublicationReservation reservation(plan,{});
    if(reservation.result_!=ScopeResult::Ok) return reservation.result_;
    SchedulerPublicationReservation::StatusReservation status(&reservation);if(status.Result()!=ScopeResult::Ok) return status.Result();
    if(!writer.Advance()) return ScopeResult::Cancelled;
    reservation.Apply();return ScopeResult::Ok;
}

SchedulerPublicationReservation::SchedulerPublicationReservation(SchedulerPublicationPlan& plan,
    const std::shared_ptr<const void>& binding) noexcept : plan_(plan) {
    using runtime::ScopeResult;
    result_=[&]() noexcept {
    if(!plan.state_ || !plan.domain_ || plan.consumed_ || plan.jointBinding_!=binding) return ScopeResult::Invalid;
    if(plan.purpose_!=SchedulerPublicationPurpose::Activate && plan.purpose_!=SchedulerPublicationPurpose::Handoff && plan.purpose_!=SchedulerPublicationPurpose::Remove) return ScopeResult::Invalid;
    scheduler_=std::unique_lock(plan.state_->mutex,std::try_to_lock);if(!scheduler_.owns_lock()) return ScopeResult::Busy;
    const auto found=plan.state_->runtimes.find(plan.owner_);
    if(plan.state_->host->stopped || plan.state_->host->closeAdmission->load() || plan.domain_->IsStopping()) return ScopeResult::Cancelled;
    if(plan.state_->domain!=plan.domain_ || plan.domain_->Revision()!=plan.domainRevision_ || found==plan.state_->runtimes.end()) return ScopeResult::Stale;
    auto& node=found->second;
    if(!node.sealed || node.jointBinding!=binding || node.identity!=plan.node_ || node.revision!=plan.revision_ || node.runtime!=plan.old_ || node.revision==UINT64_MAX)
        return ScopeResult::Stale;
    if(plan.absent_) return plan.purpose_==SchedulerPublicationPurpose::Remove && !plan.old_ && !plan.candidate_ && !node.runtime && binding ? ScopeResult::Ok : ScopeResult::Invalid;
    if((plan.purpose_==SchedulerPublicationPurpose::Remove)!=!plan.candidate_ ||
        (plan.purpose_==SchedulerPublicationPurpose::Activate)!=!plan.old_) return ScopeResult::Invalid;
    auto first=plan.old_.get(),second=plan.candidate_.get();
    if(first && second && first->vmGeneration_>second->vmGeneration_) std::swap(first,second);
    if(!first) std::swap(first,second);
    if(!first || first==second) return ScopeResult::Invalid;
    first_=first;second_=second;
    vm1_=std::unique_lock(first->mutex_,std::try_to_lock);if(!vm1_.owns_lock()) return ScopeResult::Busy;
    if(second) {vm2_=std::unique_lock(second->mutex_,std::try_to_lock);if(!vm2_.owns_lock()) return ScopeResult::Busy;}
    const auto valid=[&](const std::shared_ptr<ScriptRuntime>& vm,std::uint64_t generation,std::size_t slot) noexcept {
        return vm && vm->vmGeneration_==generation && vm->laneSlot_==slot && !vm->laneReleased_ && vm->state_ &&
            vm->lane_==plan.state_->lane && vm->schedulerHost_==plan.state_->host && vm->publicationDomain_==plan.domain_ &&
            vm->publicationNode_==plan.node_ && vm->sealed_ && !vm->bootstrap_;
    };
    if(plan.old_ && (!valid(plan.old_,plan.oldGeneration_,plan.oldSlot_) ||
        (plan.purpose_==SchedulerPublicationPurpose::Handoff && (!plan.old_->ExecutionEligible(node.revision) || plan.old_->retired_)))) return ScopeResult::Denied;
    if(plan.candidate_ && (!valid(plan.candidate_,plan.candidateGeneration_,plan.candidateSlot_) ||
        plan.candidate_->committed_ || !plan.candidate_->running_ || plan.candidate_->stopping_ || plan.candidate_->retired_ || plan.candidate_->stopRequested_.load() ||
        plan.candidate_->scheduled_.load() ||
        (plan.candidate_->preparation_ && (!plan.preparationProof_ || plan.candidate_->preparationProof_!=plan.preparationProof_)) ||
        (plan.preparationProof_ && plan.candidate_->preparationProof_!=plan.preparationProof_))) return ScopeResult::Denied;
    return ScopeResult::Ok;
    }();
}
SchedulerPublicationReservation::~SchedulerPublicationReservation() {
}
SchedulerPublicationReservation::Fence::Fence() noexcept {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    previous=schedulerPublicationFence;schedulerPublicationFence=true;
#endif
}
SchedulerPublicationReservation::Fence::~Fence() {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    schedulerPublicationFence=previous;
#endif
}
SchedulerPublicationReservation::StatusReservation::StatusReservation(SchedulerPublicationReservation* reservation) noexcept {
    using runtime::ScopeResult;
    if(!reservation) return;
    if(reservation->result_!=ScopeResult::Ok) {result_=reservation->result_;return;}
    if(!reservation->first_) return;
    first_=std::unique_lock(reservation->first_->statusMutex_,std::try_to_lock);if(!first_.owns_lock()) {result_=ScopeResult::Busy;return;}
    if(reservation->second_) {second_=std::unique_lock(reservation->second_->statusMutex_,std::try_to_lock);if(!second_.owns_lock()) result_=ScopeResult::Busy;}
}
void SchedulerPublicationReservation::Apply() noexcept {
    auto& plan=plan_;auto& node=plan.state_->runtimes.find(plan.owner_)->second;
    if(plan.absent_) {plan.consumed_=true;return;}
    ++node.revision;
    if(plan.old_) {
        plan.old_->committed_=false;plan.old_->retired_=true;plan.old_->scheduled_.store(false);
        plan.old_->publishedStatus_.running=false;
    }
    node.runtime.swap(plan.candidate_); // @summary Old map ownership remains retained in the plan; no final release at the fence.
    plan.retired_.swap(plan.candidate_);
    if(node.runtime) {
        if(plan.preparationProof_)node.runtime->preparation_=false;
        node.runtime->registration_.store(node.revision);node.runtime->committed_=true;node.runtime->disableOwed_=true;
        node.runtime->publishedStatus_.running=true;node.runtime->scheduled_.store(true);
    }
    plan.consumed_=true;
}
void SchedulerPublicationReservation::CloseIndeterminate() noexcept {
    auto& plan=plan_;
    if(plan.old_) {
        plan.old_->committed_=false;plan.old_->retired_=true;plan.old_->scheduled_.store(false);
        plan.old_->publishedStatus_.running=false;plan.retired_=plan.old_;
    }
    plan.consumed_=true;
}
runtime::ScopeResult SchedulerPublicationReservation::PrepareRemoved(ScriptScheduler& scheduler,const std::string& owner,
    const std::shared_ptr<const void>& binding,SchedulerPublicationPlan& output) {
    using runtime::ScopeResult;
    if(!binding || output.state_ || ScriptRuntime::OnScriptLane()) return ScopeResult::Invalid;
    SchedulerPublicationPlan plan;plan.state_=scheduler.state_;plan.domain_=scheduler.state_->domain;
    plan.owner_=owner;plan.jointBinding_=binding;plan.purpose_=SchedulerPublicationPurpose::Remove;plan.absent_=true;
    if(!plan.domain_) return ScopeResult::Invalid;
    {
        auto writer=plan.domain_->ReserveWriter();if(!writer.Acquired()) return ScopeResult::Busy;
        std::unique_lock lock(plan.state_->mutex,std::try_to_lock);if(!lock.owns_lock()) return ScopeResult::Busy;
        const auto found=plan.state_->runtimes.find(owner);
        if(found==plan.state_->runtimes.end() || found->second.runtime || found->second.jointBinding!=binding) return ScopeResult::Conflict;
        plan.node_=found->second.identity;plan.revision_=found->second.revision;plan.domainRevision_=plan.domain_->Revision();
    }
    output=std::move(plan);return ScopeResult::Ok;
}
runtime::ScopeResult SchedulerPublicationReservation::Inspect(ScriptScheduler& scheduler,const std::string& owner,
    const std::shared_ptr<const void>& binding,std::shared_ptr<ScriptRuntime>& output,
    std::unique_lock<std::mutex>& schedulerGuard,std::unique_lock<std::mutex>& vmGuard) {
    using runtime::ScopeResult;output.reset();
    schedulerGuard=std::unique_lock(scheduler.state_->mutex,std::try_to_lock);if(!schedulerGuard.owns_lock()) return ScopeResult::Busy;
    const auto found=scheduler.state_->runtimes.find(owner);
    if(found==scheduler.state_->runtimes.end() || found->second.jointBinding!=binding) return ScopeResult::Conflict;
    output=found->second.runtime;
    if(output) {vmGuard=std::unique_lock(output->mutex_,std::try_to_lock);if(!vmGuard.owns_lock())return ScopeResult::Busy;}
    return ScopeResult::Ok;
}
runtime::ScopeResult SchedulerPublicationReservation::ReadRunning(const std::shared_ptr<ScriptRuntime>& vm,bool& running) noexcept {
    using runtime::ScopeResult;running=false;if(!vm) return ScopeResult::Ok;
    std::unique_lock status(vm->statusMutex_,std::try_to_lock);if(!status.owns_lock())return ScopeResult::Busy;
    running=vm->publishedStatus_.running;return ScopeResult::Ok;
}
runtime::ScopeResult SchedulerPublicationReservation::InspectExact(ScriptScheduler& scheduler,const std::string& owner,
    const std::shared_ptr<const void>& binding,const std::shared_ptr<ScriptRuntime>& expected,
    const std::shared_ptr<const std::byte>& node,std::uint64_t registration,
    std::unique_lock<std::mutex>& schedulerGuard,std::unique_lock<std::mutex>& vmGuard) {
    using runtime::ScopeResult;
    schedulerGuard=std::unique_lock(scheduler.state_->mutex,std::try_to_lock);if(!schedulerGuard.owns_lock())return ScopeResult::Busy;
    const auto found=scheduler.state_->runtimes.find(owner);
    if(!expected || scheduler.host_->stopped || found==scheduler.state_->runtimes.end() ||
        found->second.runtime!=expected || found->second.identity!=node || !found->second.sealed ||
        found->second.jointBinding!=binding || found->second.revision!=registration || !binding)return ScopeResult::Conflict;
    vmGuard=std::unique_lock(expected->mutex_,std::try_to_lock);if(!vmGuard.owns_lock())return ScopeResult::Busy;
    if(expected->publicationNode_!=node || expected->publicationDomain_!=scheduler.state_->domain ||
        !expected->sealed_ || !expected->ExecutionEligible(registration))return ScopeResult::Stale;
    return ScopeResult::Ok;
}
runtime::ScopeResult SchedulerPublicationReservation::ValidateEnrollment(ScriptScheduler& scheduler,const std::string& owner,
    const std::shared_ptr<ScriptRuntime>& old,const std::shared_ptr<ScriptRuntime>& candidate,
    const std::shared_ptr<runtime::RuntimeCommitDomain>& domain,const std::shared_ptr<const void>& binding,bool prepared,const std::shared_ptr<const void>& preparationProof) {
    using runtime::ScopeResult;if(ScriptRuntime::OnScriptLane() || !domain) return ScopeResult::Invalid;
    auto writer=domain->ReserveWriter();if(!writer.Acquired())return ScopeResult::Busy;
    std::unique_lock lock(scheduler.state_->mutex,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
    if(scheduler.state_->domain!=domain || scheduler.host_->stopped) return ScopeResult::Conflict;
    const auto found=scheduler.state_->runtimes.find(owner);
    if(old && (found==scheduler.state_->runtimes.end() ||
        (found->second.runtime!=old && (found->second.runtime || !found->second.jointBinding || found->second.jointBinding!=binding)))) return ScopeResult::Conflict;
    if(!old && found!=scheduler.state_->runtimes.end() && found->second.runtime) return ScopeResult::Conflict;
    if(found!=scheduler.state_->runtimes.end() && found->second.jointBinding && found->second.jointBinding!=binding)return ScopeResult::Conflict;
    if(candidate) {
        std::unique_lock vm(candidate->mutex_,std::try_to_lock);if(!vm.owns_lock())return ScopeResult::Busy;
        if(candidate->modId_!=owner || candidate->lane_!=scheduler.lane_ || candidate->bootstrap_ || (candidate->preparation_ && (!preparationProof || candidate->preparationProof_!=preparationProof)) || (!candidate->preparation_ && preparationProof) ||
            (candidate->schedulerHost_ && candidate->schedulerHost_!=scheduler.host_) ||
            (candidate->sealed_ && (!prepared || found==scheduler.state_->runtimes.end() || candidate->publicationNode_!=found->second.identity ||
                candidate->publicationDomain_!=domain)) || candidate->committed_ || candidate->scheduled_ || candidate->stopRequested_)
            return ScopeResult::Denied;
    }
    return ScopeResult::Ok;
}
runtime::ScopeResult SchedulerPublicationReservation::BindToken(const std::shared_ptr<const void>& binding) noexcept {
    using runtime::ScopeResult;
    auto& plan=plan_;if(!binding || result_!=ScopeResult::Ok)return ScopeResult::Invalid;
    auto& node=plan.state_->runtimes.find(plan.owner_)->second;
    if(node.jointBinding && node.jointBinding!=binding) return ScopeResult::Conflict;
    node.jointBinding=binding;plan.jointBinding_=binding;return ScopeResult::Ok;
}

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
runtime::ScopeResult SchedulerPublicationTestAccess::Enroll(ScriptScheduler& scheduler,std::shared_ptr<runtime::RuntimeCommitDomain> domain) {
    if(!domain || ScriptRuntime::OnScriptLane()) return runtime::ScopeResult::Invalid;
    const std::scoped_lock lock(scheduler.state_->mutex);
    if(scheduler.state_->host->stopped || scheduler.state_->domain || !scheduler.state_->runtimes.empty()) return runtime::ScopeResult::Denied;
    scheduler.state_->domain=std::move(domain);return runtime::ScopeResult::Ok;
}
runtime::ScopeResult SchedulerPublicationTestAccess::Prepare(ScriptScheduler& scheduler,const std::string& owner,
    const std::shared_ptr<ScriptRuntime>& candidate,SchedulerPublicationPurpose purpose,SchedulerPublicationPlan& output) {
    return scheduler.PreparePublication(owner,candidate,purpose,output);
}
runtime::ScopeResult SchedulerPublicationTestAccess::Publish(SchedulerPublicationPlan& plan) {return ScriptScheduler::PublishPublication(plan);}
runtime::ScopeResult SchedulerPublicationTestAccess::Reclaim(SchedulerPublicationPlan& plan) {
    using runtime::ScopeResult;
    if(ScriptRuntime::OnScriptLane() || !plan.state_ || (plan.domain_ && plan.domain_->WriterActiveOnCurrentThread())) return ScopeResult::Denied;
    const auto close=[](const std::shared_ptr<ScriptRuntime>& vm) {
        if(!vm) return ScopeResult::Ok;
        const std::scoped_lock control(vm->controlMutex_);
        return vm->CloseAndReleaseLocked(true);
    };
    const auto outcome=close(plan.consumed_?plan.retired_:plan.candidate_);
    if(outcome!=ScopeResult::Ok)return outcome;
    // @summary Defensive indeterminate cleanup also closes the candidate that was retained but never activated.
    return plan.consumed_ && plan.candidate_!=plan.retired_ ? close(plan.candidate_) : ScopeResult::Ok;
}
runtime::ScopeResult SchedulerPublicationTestAccess::ReleaseEmpty(ScriptScheduler& scheduler,const std::string& owner) {
    using runtime::ScopeResult;
    std::map<std::string,ScriptSchedulerState::Node>::node_type retained;
    auto domain=scheduler.state_->domain;if(!domain) return ScopeResult::Denied;
    auto writer=domain->ReserveWriter();if(!writer.Acquired()) return ScopeResult::Busy;
    std::unique_lock lock(scheduler.state_->mutex,std::try_to_lock);if(!lock.owns_lock()) return ScopeResult::Busy;
    const auto found=scheduler.state_->runtimes.find(owner);
    if(found==scheduler.state_->runtimes.end() || found->second.runtime || found->second.jointBinding) return ScopeResult::Denied;
    // @summary Extraction retains the complete node until reservations leave scope, preventing final token release under S.
    retained=scheduler.state_->runtimes.extract(found);lock.unlock();return ScopeResult::Ok;
}
std::shared_ptr<ScriptRuntime> SchedulerPublicationTestAccess::Current(ScriptScheduler& scheduler,const std::string& owner) {
    std::shared_ptr<runtime::RuntimeCommitDomain> domain;
    {std::unique_lock lock(scheduler.state_->mutex,std::try_to_lock);if(!lock.owns_lock())return {};domain=scheduler.state_->domain;}
    if(!domain) return {};
    auto writer=domain->ReserveWriter();if(!writer.Acquired()) return {};
    std::unique_lock lock(scheduler.state_->mutex,std::try_to_lock);if(!lock.owns_lock())return {};const auto found=scheduler.state_->runtimes.find(owner);
    return found==scheduler.state_->runtimes.end()?std::shared_ptr<ScriptRuntime>{}:found->second.runtime;
}
void SchedulerPublicationTestAccess::Barriers(const std::shared_ptr<ScriptRuntime>& vm,std::function<void()> selected,std::function<void()> extracted) {
    const std::scoped_lock lock(vm->mutex_);if(vm->scheduled_.load()) throw std::runtime_error("Barrier installation after scheduling");
    vm->selectedBarrier_=std::move(selected);vm->extractedBarrier_=std::move(extracted);
}
void SchedulerPublicationTestAccess::Hold(const std::shared_ptr<ScriptRuntime>& vm,bool status,const std::function<void()>& body) {
    std::unique_lock lock(status?vm->statusMutex_:vm->mutex_);body();
}
void SchedulerPublicationTestAccess::HoldScheduler(ScriptScheduler& scheduler,const std::function<void()>& body) {
    const std::scoped_lock lock(scheduler.state_->mutex);body();
}
bool SchedulerPublicationTestAccess::InsideFence() noexcept {return schedulerPublicationFence;}
bool SchedulerPublicationTestAccess::Closed(const std::shared_ptr<ScriptRuntime>& vm) {
    const std::scoped_lock control(vm->controlMutex_);return vm->laneReleased_;
}
bool SchedulerPublicationTestAccess::StopRequested(const std::shared_ptr<ScriptRuntime>& vm) noexcept {return vm->stopRequested_.load();}
bool SchedulerPublicationTestAccess::Stopped(ScriptScheduler& scheduler) {
    std::unique_lock lock(scheduler.state_->mutex,std::try_to_lock);return lock.owns_lock() && scheduler.state_->host->stopped;
}
#endif
}
