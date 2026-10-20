#pragma once
#include <atomic>
#include <cstdint>

namespace sc13::runtime {
class RuntimeLifecycle;
/** @summary Close acknowledgement is distinct from original factual event delivery. */
enum class LifecycleClosePhase : std::uint8_t { Unbound, Open, Closing, Closed, AbandonedPending };
/** @summary First bounded close cause, never permission, readiness or publisher trust. */
enum class LifecycleCloseReason : std::uint8_t { None, ExplicitStop, Capacity, Sequence, Allocation, Destruction, Invariant };
/** @summary One coherent close phase/reason sample; contains no owner or permission. */
struct LifecycleSourceObservation final {LifecycleClosePhase phase;LifecycleCloseReason reason;};
/** @summary One acyclic private protocol cell; it retains no publisher, domain, receiver or scope. */
class LifecycleAuthoritySource final {
public:
    /** @summary Observe close protocol only; Open never grants an operation. @return Current monotonic phase. */
    LifecycleClosePhase Phase() const noexcept { return static_cast<LifecycleClosePhase>(word_.load(std::memory_order_acquire)&0xffU); }
    /** @summary Observe the first close cause. @return Bounded diagnostic only. */
    LifecycleCloseReason Reason() const noexcept { return static_cast<LifecycleCloseReason>(word_.load(std::memory_order_acquire)>>8); }
    /** @summary Decode one original atomic word rather than combine independently sampled fields.
     * @return Coherent protocol phase and first cause. */
    LifecycleSourceObservation Observe() const noexcept {
        const auto word=word_.load(std::memory_order_acquire);
        return {static_cast<LifecycleClosePhase>(word&0xffU),static_cast<LifecycleCloseReason>(word>>8)};
    }
    LifecycleAuthoritySource(const LifecycleAuthoritySource&)=delete;
    LifecycleAuthoritySource& operator=(const LifecycleAuthoritySource&)=delete;
private:
    /** @summary Only an actual domain-bound original publisher constructs this cell. */
    LifecycleAuthoritySource()=default;
    /** @summary Coherent monotonic phase/first reason; no availability or lifecycle snapshot mirror. */
    std::atomic<std::uint32_t> word_{static_cast<std::uint32_t>(LifecycleClosePhase::Open)};
    /** @summary Publish close intent once without acquiring any lock. @param reason First close cause. */
    void Request(LifecycleCloseReason reason) noexcept {
        auto expected=static_cast<std::uint32_t>(LifecycleClosePhase::Open);
        static_cast<void>(word_.compare_exchange_strong(expected,static_cast<std::uint32_t>(LifecycleClosePhase::Closing)|
            (static_cast<std::uint32_t>(reason)<<8),std::memory_order_acq_rel));
    }
    /** @summary Preserve first reason while closing an actual pending protocol. @param phase Closed or abandoned outcome. */
    void Complete(LifecycleClosePhase phase) noexcept {
        auto original=word_.load(std::memory_order_acquire);
        while(static_cast<LifecycleClosePhase>(original&0xffU)==LifecycleClosePhase::Closing){
            if(word_.compare_exchange_weak(original,(original&~0xffU)|static_cast<std::uint32_t>(phase),std::memory_order_acq_rel))return;
        }
    }
    friend class RuntimeLifecycle;
};
}
