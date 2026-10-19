#include "runtime/cache_settlement.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <exception>
#if defined(SIMODER_SCRIPT_LANE_TEST_WITNESS)
#include "scripting/script_runtime.hpp"
#endif

namespace sc13::runtime {
namespace {
constexpr std::size_t Slots=128,Targets=8192;
/** @summary Reject actual script lane where the compiled host supplies its native witness. */
bool OffLane()noexcept{
#if defined(SIMODER_SCRIPT_LANE_TEST_WITNESS)
    return !scripting::ScriptRuntime::OnScriptLane();
#else
    return true;
#endif
}
/** @summary Nonblocking exclusive admission; never waits for an action under store guard. */
struct AtomicTurn final {
    std::atomic_bool* flag{};
    explicit AtomicTurn(std::atomic_bool& value) noexcept {bool empty{};if(value.compare_exchange_strong(empty,true,std::memory_order_acquire))flag=&value;}
    ~AtomicTurn(){if(flag)flag->store(false,std::memory_order_release);}
    explicit operator bool()const noexcept{return flag!=nullptr;}
};
}
/** @summary Fixed original obligation metadata; payload lives separately in the same store allocation. */
struct CacheSettlement::Record final {
    std::atomic_bool occupied{},turn{};
    std::atomic<std::uint32_t> references{},actions{};
    std::uint64_t incarnation{};
    OwnerToken owner;
    Phase phase{Phase::Prepared};
    CacheEvictionStatus eviction{CacheEvictionStatus::PendingUnavailable};
    std::uint64_t revision{};
    std::size_t count{},invalidated{};
    bool retired{};
};
/** @summary Fixed volatile process-lifetime store; no callbacks, cache owner or static teardown. */
struct CacheSettlement::Store final {
    std::mutex mutex;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    std::uint64_t nextOwner{1};
#endif
    std::array<Record,Slots> records;
    std::array<std::array<core::Tgi,Targets>,Slots> payload;
};
/** @summary An accepted internal action owns the record through direct acknowledgement. */
struct CacheSettlement::Turn final {
    Record* record{};
    explicit Turn(Record* value)noexcept:record(value){}
    ~Turn(){if(record){record->actions.fetch_sub(1,std::memory_order_release);record->turn.store(false,std::memory_order_release);}}
};
CacheSettlement::Store* CacheSettlement::Storage() noexcept {
    static Store* store=[]() noexcept ->Store* {
        static_assert(sizeof(core::Tgi)==12);
        static_assert(sizeof(CacheWitness)<=64&&sizeof(OwnerToken)<=96&&sizeof(Batch)<=32);
        static_assert(sizeof(Record)<=384);
        static_assert(sizeof(Store)-sizeof(Store::records)-sizeof(Store::payload)<=4096);
        static_assert(sizeof(Store::payload)==12U*1024U*1024U);
        void* memory=VirtualAlloc(nullptr,sizeof(Store),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if(!memory)return nullptr;
        return ::new(memory) Store;
    }();return store;
}
CacheSettlement::Batch::Batch(Batch&& other)noexcept:slot_(std::exchange(other.slot_,UINT32_MAX)),incarnation_(std::exchange(other.incarnation_,0)){}
CacheSettlement::Batch& CacheSettlement::Batch::operator=(Batch&& other)noexcept {if(this!=&other){Reset();slot_=std::exchange(other.slot_,UINT32_MAX);incarnation_=std::exchange(other.incarnation_,0);}return *this;}
CacheSettlement::Batch::~Batch(){Reset();}
void CacheSettlement::Batch::Reset() noexcept {const auto slot=std::exchange(slot_,UINT32_MAX);incarnation_=0;if(slot==UINT32_MAX)return;auto* store=Storage();if(!store||slot>=Slots)std::terminate();if(store->records[slot].references.fetch_sub(1,std::memory_order_release)==0)std::terminate();}
bool CacheSettlement::SameOwner(const OwnerToken& left,const OwnerToken& right) noexcept {
    const auto& a=left.witness_;const auto& b=right.witness_;
    return left.sequence_&&left.sequence_==right.sequence_&&a.nonce_==b.nonce_&&a.reset_==b.reset_&&!a.control_.owner_before(b.control_)&&!b.control_.owner_before(a.control_);
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
CacheSettlement::Result CacheSettlement::IssueOwner(RuntimeResourceCache& cache,OwnerToken& output) noexcept {
    if(output.sequence_||!OffLane())return Result::Invalid;
    try{auto witness=cache.Witness();if(!witness.nonce_)return Result::Unavailable;auto* store=Storage();if(!store)return Result::Unavailable;
        std::unique_lock gate(store->mutex,std::try_to_lock);if(!gate.owns_lock())return Result::Busy;
        if(store->nextOwner==UINT64_MAX)return Result::Unavailable;
        output.witness_=std::move(witness);output.sequence_=store->nextOwner++;return Result::Ok;
    }catch(...){return Result::Unavailable;}
}
#endif
CacheSettlement::Observation CacheSettlement::Copy(const Record& record) noexcept {Observation result;result.result=Result::Ok;result.phase=record.phase;result.eviction=record.eviction;result.incarnation=record.incarnation;result.ownerSequence=record.owner.sequence_;result.committedRevision=record.revision;result.targets=record.count;result.invalidated=record.invalidated;result.payloadBytes=record.count*sizeof(core::Tgi);return result;}
CacheSettlement::Result CacheSettlement::Acquire(const Batch& batch,Record*& output) noexcept {
    output=nullptr;if(batch.slot_>=Slots||!batch.incarnation_)return Result::Invalid;auto* store=Storage();if(!store)return Result::Unavailable;
    auto& record=store->records[batch.slot_];bool free{};if(!record.turn.compare_exchange_strong(free,true,std::memory_order_acquire))return Result::Busy;
    if(!record.occupied.load(std::memory_order_acquire)||record.incarnation!=batch.incarnation_){record.turn.store(false,std::memory_order_release);return Result::Invalid;}
    if(record.actions.load()!=0)std::terminate();record.actions.store(1,std::memory_order_release);output=&record;return Result::Ok;
}
CacheSettlement::MarkerReservation::~MarkerReservation(){Release();}
void CacheSettlement::AcknowledgeLocked(Record& record,std::uint64_t revision)noexcept {if(!revision||record.phase!=Phase::Prepared||record.revision)std::terminate();record.revision=revision;record.phase=Phase::CommittedPending;}
void CacheSettlement::MarkerReservation::Acknowledge(std::uint64_t revision)noexcept {if(!record_)std::terminate();AcknowledgeLocked(*record_,revision);}
void CacheSettlement::MarkerReservation::Release()noexcept {auto* record=std::exchange(record_,nullptr);if(!record)return;if(!record->revision){if(record->phase!=Phase::Prepared)std::terminate();record->phase=Phase::Cancelled;}if(record->actions.fetch_sub(1,std::memory_order_release)!=1)std::terminate();record->turn.store(false,std::memory_order_release);}
CacheSettlement::Result CacheSettlement::Reserve(const OwnerToken& owner,const std::set<core::Tgi>& targets,Batch& output)noexcept{return ReserveInternal(owner,targets,output,nullptr);}
CacheSettlement::Result CacheSettlement::ReserveMarker(const OwnerToken& owner,const std::set<core::Tgi>& targets,Batch& output,MarkerReservation& marker)noexcept{static_assert(sizeof(MarkerReservation)<=64);return ReserveInternal(owner,targets,output,&marker);}
CacheSettlement::Result CacheSettlement::ReserveInternal(const OwnerToken& owner,const std::set<core::Tgi>& targets,Batch& output,MarkerReservation* marker) noexcept {
    if(!owner.sequence_||!owner.witness_.nonce_||output.incarnation_||(marker&&marker->record_)||!OffLane())return Result::Invalid;
    if(targets.size()>Targets)return Result::Capacity;
    auto* store=Storage();if(!store)return Result::Unavailable;
    // @summary Weak-control disposal scratch is declared before the guard, so all final weak releases occur afterward.
    std::array<OwnerToken,Slots> disposal;
    static_assert(sizeof(disposal)<=65536);
    std::unique_lock gate(store->mutex,std::try_to_lock);if(!gate.owns_lock())return Result::Busy;
    std::size_t candidate=Slots,detached{};
    for(std::size_t i=0;i<Slots;++i){auto& record=store->records[i];AtomicTurn action(record.turn);if(!action) {
            // @summary Owner metadata is immutable while occupied; record turn is still required before comparing it.
            if(record.occupied.load(std::memory_order_acquire))return Result::Busy;
            continue;
        }
        if(record.occupied.load(std::memory_order_acquire)){
            const bool terminal=record.phase==Phase::Settled||record.phase==Phase::Cancelled;
            if(!terminal&&SameOwner(record.owner,owner))return Result::Conflict;
            if(terminal&&record.references.load(std::memory_order_acquire)==0&&record.actions.load(std::memory_order_acquire)==0){
                disposal[detached++]=std::move(record.owner);std::fill_n(store->payload[i].begin(),record.count,core::Tgi{});record.count=0;record.invalidated=0;record.revision=0;record.occupied.store(false,std::memory_order_release);
            }
        }
        if(!record.occupied.load(std::memory_order_acquire)&&!record.retired){if(record.incarnation==UINT64_MAX)record.retired=true;else if(candidate==Slots)candidate=i;}
    }
    if(candidate==Slots)return Result::Capacity;
    auto& record=store->records[candidate];AtomicTurn action(record.turn);if(!action)return Result::Busy;
    if(record.occupied.load()||record.retired||record.incarnation==UINT64_MAX)return Result::Busy;
    // @summary Fixed trivial payload copy cannot allocate or expose a partially constructed record.
    record.owner=owner;++record.incarnation;record.count=targets.size();record.invalidated=0;record.revision=0;record.phase=Phase::Prepared;record.eviction=CacheEvictionStatus::PendingUnavailable;
    std::copy(targets.begin(),targets.end(),store->payload[candidate].begin());record.references.store(1,std::memory_order_release);
    if(marker){record.actions.store(1,std::memory_order_release);marker->record_=&record;action.flag=nullptr;}
    record.occupied.store(true,std::memory_order_release);
    output.slot_=static_cast<std::uint32_t>(candidate);output.incarnation_=record.incarnation;return Result::Ok;
}
CacheSettlement::Result CacheSettlement::FindPending(const OwnerToken& owner,Batch& output) noexcept {
    if(!owner.sequence_||output.incarnation_)return Result::Invalid;auto* store=Storage();if(!store)return Result::Unavailable;
    std::unique_lock gate(store->mutex,std::try_to_lock);if(!gate.owns_lock())return Result::Busy;
    for(std::size_t i=0;i<Slots;++i){auto& record=store->records[i];if(!record.occupied.load(std::memory_order_acquire))continue;
        AtomicTurn action(record.turn);if(!action)return Result::Busy;
        if(!SameOwner(owner,record.owner))continue;if(record.phase==Phase::Settled||record.phase==Phase::Cancelled)continue;
        auto count=record.references.load(std::memory_order_acquire);for(;;){if(count==UINT32_MAX)return Result::Capacity;if(record.references.compare_exchange_weak(count,count+1,std::memory_order_acq_rel))break;}
        output.slot_=static_cast<std::uint32_t>(i);output.incarnation_=record.incarnation;return Result::Ok;
    }return Result::NotFound;
}
CacheSettlement::Result CacheSettlement::MarkCommitted(const Batch& batch,std::uint64_t revision) noexcept {if(!revision)return Result::Invalid;Record* record{};const auto result=Acquire(batch,record);if(result!=Result::Ok)return result;Turn action(record);if(record->revision)return record->revision==revision?Result::Ok:Result::Conflict;if(record->phase!=Phase::Prepared)return Result::Conflict;AcknowledgeLocked(*record,revision);return Result::Ok;}
CacheSettlement::Result CacheSettlement::CancelPrepared(const Batch& batch) noexcept {Record* record{};const auto result=Acquire(batch,record);if(result!=Result::Ok)return result;Turn action(record);if(record->phase!=Phase::Prepared||record->revision)return Result::Conflict;record->phase=Phase::Cancelled;return Result::Ok;}
CacheSettlement::Observation CacheSettlement::Observe(const Batch& batch) noexcept {Record* record{};const auto result=Acquire(batch,record);if(result!=Result::Ok){Observation output;output.result=result;return output;}Turn action(record);return Copy(*record);}
CacheSettlement::Observation CacheSettlement::Evict(RuntimeResourceCache& cache,const Batch& batch) noexcept {
    if(!OffLane()){Observation output;output.result=Result::Invalid;return output;}
    Record* record{};const auto result=Acquire(batch,record);if(result!=Result::Ok){Observation output;output.result=result;return output;}Turn action(record);
    if(record->phase==Phase::Settled)return Copy(*record);
    if(!record->revision||record->phase==Phase::Cancelled){auto output=Copy(*record);output.result=Result::Conflict;return output;}
    if(record->owner.witness_.control_.expired()){record->phase=Phase::Unqualified;record->eviction=CacheEvictionStatus::PendingUnavailable;return Copy(*record);}
    auto* store=Storage();const auto evicted=cache.EvictOriginal(std::span<const core::Tgi>(store->payload[batch.slot_].data(),record->count),record->owner.witness_);
    record->eviction=evicted.status;record->invalidated=evicted.invalidated;
    if(evicted.status==CacheEvictionStatus::Evicted||evicted.status==CacheEvictionStatus::EvictedDiagnosticPending)record->phase=Phase::Settled;
    else if(evicted.status!=CacheEvictionStatus::PendingAllocation)record->phase=Phase::Unqualified;
    return Copy(*record);
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
CacheSettlement::Result CacheSettlement::CopyActionCount(const Batch& batch,std::uint32_t& output) noexcept {
    output=0;
    if(batch.slot_>=Slots||!batch.incarnation_)return Result::Invalid;
    // @summary A valid retained Batch already initialized storage and pins occupied/incarnation against reclamation.
    auto* store=Storage();if(!store)return Result::Unavailable;
    const auto& record=store->records[batch.slot_];
    if(!record.occupied.load(std::memory_order_acquire)||record.incarnation!=batch.incarnation_)return Result::Invalid;
    output=record.actions.load(std::memory_order_acquire);return Result::Ok;
}
CacheSettlement::Result CacheSettlementTestAccess::CopyActionCount(const CacheSettlement::Batch& batch,std::uint32_t& output)noexcept{return CacheSettlement::CopyActionCount(batch,output);}
CacheSettlement::Result CacheSettlementTestAccess::IssueOwner(RuntimeResourceCache& cache,CacheSettlement::OwnerToken& output)noexcept{return CacheSettlement::IssueOwner(cache,output);}
CacheSettlement::Result CacheSettlementTestAccess::Inspect(Metrics& output)noexcept {
    output={};auto* store=CacheSettlement::Storage();if(!store)return CacheSettlement::Result::Unavailable;
    std::unique_lock gate(store->mutex,std::try_to_lock);if(!gate.owns_lock())return CacheSettlement::Result::Busy;
    output.ownerBytes=sizeof(CacheSettlement::OwnerToken);output.viewBytes=sizeof(CacheSettlement::Batch);output.recordBytes=sizeof(CacheSettlement::Record);output.storeBytes=sizeof(CacheSettlement::Store);output.scratchBytes=sizeof(std::array<CacheSettlement::OwnerToken,Slots>);
    for(auto& record:store->records){AtomicTurn turn(record.turn);if(!turn)return CacheSettlement::Result::Busy;if(record.occupied.load()){++output.occupied;output.payloadBytes+=record.count*sizeof(core::Tgi);output.externalViews+=record.references.load();}}
    return CacheSettlement::Result::Ok;
}
void CacheSettlementTestAccess::HoldAdmission(const std::function<void()>& callback){auto* store=CacheSettlement::Storage();if(!store)throw std::bad_alloc();const std::scoped_lock gate(store->mutex);callback();}
CacheSettlement::Result CacheSettlementTestAccess::CopyTargets(const CacheSettlement::Batch& batch,std::span<core::Tgi> output,std::size_t& copied)noexcept {
    copied=0;CacheSettlement::Record* record{};const auto result=CacheSettlement::Acquire(batch,record);if(result!=CacheSettlement::Result::Ok)return result;
    CacheSettlement::Turn action(record);if(output.size()<record->count)return CacheSettlement::Result::Capacity;
    // @summary A valid checked view and accepted turn retain the original immutable payload throughout copying.
    auto* store=CacheSettlement::Storage();const auto index=static_cast<std::size_t>(record-store->records.data());std::copy_n(store->payload[index].begin(),record->count,output.begin());copied=record->count;return CacheSettlement::Result::Ok;
}
#endif
}
