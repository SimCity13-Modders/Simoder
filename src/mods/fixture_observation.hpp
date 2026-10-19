#pragma once
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
#include <array>
#include <cstddef>
#include <cstdint>

namespace sc13::mods::fixture {
/** @summary Exact actual native object whose destructor is observed without ownership. */
enum class ObjectKind : unsigned { Preparation, Base, State };
/** @summary Actual TrySchedule branch, recorded where it is taken rather than synthesized by a test. */
enum class AdmissionReason : unsigned { Unset, InitialV, Routeless, ProbeException, MissingControl, WorkerLock, Closed, Busy, FinalV, Ineligible, OriginExpired, Accepted, Count };
/** @summary Nonrecycled slot identity; no pointer or writable target ownership is exposed. */
struct Handle final {std::uint32_t slot{},identity{};explicit operator bool()const noexcept{return identity!=0;}};
/** @summary Copied destructor-entry facts; completion additionally requires actual joined final ownership and weak expiry. */
struct Destruction final {std::uint32_t entries{},thread{};bool onLane{},inFence{},underWorker{};};
/** @summary Stable atomic snapshots; coherent outcome assertions require the caller's actual joined lane barrier. */
struct Admission final {std::uint64_t started{},finished{},allocations{},frees{},nested{},offLane{};std::array<std::uint64_t,static_cast<unsigned>(AdmissionReason::Count)> reasons{};};
/** @summary Arm one exact live object off lane; fixed16 lifetime slots are never recycled. @param object Actual stable address. @param kind Exact native type. @return Empty on invalid/duplicate/full. */
Handle ArmDestruction(const void* object,ObjectKind kind) noexcept;
/** @summary Record entry of the actual owning type, without allocation/callback/ownership. @param object Destructing actual object. @param kind Exact native type. */
void Destroying(const void* object,ObjectKind kind) noexcept;
/** @summary Read actual destructor-entry facts after owning-thread join. @param handle Exact fixed slot. @return Copied facts or empty. */
Destruction ObserveDestruction(Handle handle) noexcept;
/** @summary Stop matching a completed object only after the caller joins destruction and confirms weak expiry; slots never recycle. @param handle Exact original slot. */
void ForgetDestruction(Handle handle) noexcept;
/** @summary Arm exact actual VM admission observation off lane before the tested visits. @param vm Stable retained VM address. @return Empty on duplicate/full/invalid. */
Handle ArmAdmission(const void* vm) noexcept;
/** @summary Prevent later visits from entering a slot; caller must subsequently join a real lane control before final observation. @param handle Exact slot. */
void StopAdmission(Handle handle) noexcept;
/** @summary Copy actual interval counters; never repairs or fabricates branch state. @param handle Exact slot. @return Atomic snapshots. */
Admission ObserveAdmission(Handle handle) noexcept;
/** @summary Count only a real C++ allocation attempt inside this thread's exact native admission interval. */
void Allocating() noexcept;
/** @summary Count only a real nonnull C++ free inside this thread's exact native admission interval. */
void Freeing() noexcept;
/** @summary Return the actual OS thread identity without allocation. @return Win32 thread ID. */
std::uint32_t Thread() noexcept;
/** @summary Match actual W lexical guards for destructor classification; adds no lock or ownership. */
class WorkerRegion final {
public:
    /** @summary Mark this thread inside an acquired original W guard. */
    WorkerRegion() noexcept;
    /** @summary Restore prior classification before lexical lock release. */
    ~WorkerRegion();
    WorkerRegion(const WorkerRegion&)=delete;
    WorkerRegion& operator=(const WorkerRegion&)=delete;
};
/** @summary Exact full TrySchedule interval; declare before all function locals so their destruction remains included. */
class AdmissionInterval final {
public:
    /** @summary Begin for a registered actual VM; no-op if unarmed. @param vm Actual selected VM. */
    explicit AdmissionInterval(const void* vm) noexcept;
    /** @summary End after all later-declared native locals and guards have unwound. */
    ~AdmissionInterval();
    /** @summary Record a reason at its actual source branch. @param reason Actual selected branch. */
    void Reason(AdmissionReason reason) noexcept{reason_=reason;}
    AdmissionInterval(const AdmissionInterval&)=delete;
    AdmissionInterval& operator=(const AdmissionInterval&)=delete;
private:
    void* slot_{};void* previous_{};AdmissionReason reason_{AdmissionReason::Unset};
};
}
#endif
