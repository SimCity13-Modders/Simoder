#include "mods/fixture_observation.hpp"
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
#include "scripting/script_runtime.hpp"
#include "scripting/scheduler_publication.hpp"
#include <atomic>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace sc13::mods::fixture {
namespace {
constexpr unsigned capacity=16;
/** @summary Fixed process-lifetime object observation; object pointers never confer ownership. */
struct ObjectSlot final {std::atomic<const void*> object{};ObjectKind kind{};std::atomic_uint entries{},thread{};std::atomic_bool onLane{},inFence{},worker{};};
/** @summary Fixed process-lifetime per-VM counters, never reclaimed while readers may exist. */
struct AdmissionSlot final {std::atomic<const void*> vm{};std::atomic_bool enabled{};std::atomic_uint64_t started{},finished{},allocations{},frees{},nested{},offLane{};std::array<std::atomic_uint64_t,static_cast<unsigned>(AdmissionReason::Count)> reasons{};};
std::array<ObjectSlot,capacity> objects;
std::array<AdmissionSlot,capacity> admissions;
std::atomic_uint objectNext{},admissionNext{};
std::atomic_flag registration=ATOMIC_FLAG_INIT;
thread_local AdmissionSlot* current{};
thread_local unsigned workerDepth{};
/** @summary Claim a unique never-recycled fixed slot without wrapping. @param next Monotonic bounded cursor. @return Index or capacity on exhaustion. */
unsigned Claim(std::atomic_uint& next)noexcept{auto value=next.load();while(value<capacity){if(next.compare_exchange_weak(value,value+1))return value;}return capacity;}
/** @summary Validate a never-recycled index identity. @param value Original handle. @return True for a bounded issued-form identity. */
bool Valid(Handle value)noexcept{return value.slot<capacity&&value.identity==value.slot+1;}
/** @summary Try-only off-lane registration serialization; observers never acquire this flag. */
struct Registration final{bool held=!registration.test_and_set();/** @summary Release only this successful claim. */~Registration(){if(held)registration.clear();}};
}
std::uint32_t Thread()noexcept{return GetCurrentThreadId();}
Handle ArmDestruction(const void* object,ObjectKind kind)noexcept{
    if(!object||scripting::ScriptRuntime::OnScriptLane())return {};
    Registration turn;if(!turn.held)return {};
    for(const auto& slot:objects)if(slot.object.load(std::memory_order_acquire)==object)return {};
    const auto index=Claim(objectNext);if(index==capacity)return {};auto& slot=objects[index];slot.kind=kind;slot.object.store(object,std::memory_order_release);return {index,index+1};
}
void Destroying(const void* object,ObjectKind kind)noexcept{
    for(auto& slot:objects)if(slot.object.load(std::memory_order_acquire)==object&&slot.kind==kind){slot.thread.store(Thread());slot.onLane.store(scripting::ScriptRuntime::OnScriptLane());slot.inFence.store(scripting::SchedulerPublicationTestAccess::InsideFence());slot.worker.store(workerDepth!=0);slot.entries.fetch_add(1,std::memory_order_release);}
}
Destruction ObserveDestruction(Handle handle)noexcept{if(!Valid(handle))return {};const auto& slot=objects[handle.slot];return {slot.entries.load(std::memory_order_acquire),slot.thread.load(),slot.onLane.load(),slot.inFence.load(),slot.worker.load()};}
void ForgetDestruction(Handle handle)noexcept{if(Valid(handle)&&objects[handle.slot].entries.load(std::memory_order_acquire)!=0)objects[handle.slot].object.store(nullptr,std::memory_order_release);}
Handle ArmAdmission(const void* vm)noexcept{
    if(!vm||scripting::ScriptRuntime::OnScriptLane())return {};Registration turn;if(!turn.held)return {};for(const auto& slot:admissions)if(slot.enabled.load(std::memory_order_acquire)&&slot.vm.load()==vm)return {};
    const auto index=Claim(admissionNext);if(index==capacity)return {};auto& slot=admissions[index];slot.vm.store(vm);slot.enabled.store(true,std::memory_order_release);return {index,index+1};
}
void StopAdmission(Handle handle)noexcept{if(Valid(handle))admissions[handle.slot].enabled.store(false,std::memory_order_release);}
Admission ObserveAdmission(Handle handle)noexcept{
    if(!Valid(handle))return {};const auto& slot=admissions[handle.slot];Admission value;value.started=slot.started.load();value.finished=slot.finished.load();value.allocations=slot.allocations.load();value.frees=slot.frees.load();value.nested=slot.nested.load();value.offLane=slot.offLane.load();for(unsigned i=0;i<value.reasons.size();++i)value.reasons[i]=slot.reasons[i].load();return value;
}
void Allocating()noexcept{if(current)++current->allocations;}
void Freeing()noexcept{if(current)++current->frees;}
WorkerRegion::WorkerRegion()noexcept{++workerDepth;}
WorkerRegion::~WorkerRegion(){--workerDepth;}
AdmissionInterval::AdmissionInterval(const void* vm)noexcept{
    previous_=current;
    for(auto& slot:admissions)if(slot.enabled.load(std::memory_order_acquire)&&slot.vm.load()==vm){slot_=&slot;if(current)++slot.nested;current=&slot;++slot.started;if(!scripting::ScriptRuntime::OnScriptLane())++slot.offLane;break;}
}
AdmissionInterval::~AdmissionInterval(){if(slot_){auto& slot=*static_cast<AdmissionSlot*>(slot_);++slot.reasons[static_cast<unsigned>(reason_)];++slot.finished;current=static_cast<AdmissionSlot*>(previous_);}}
}
#endif
