#include "runtime/runtime_scope.hpp"
#include "platform/runtime_owner.hpp"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <cstdio>
#include <algorithm>
#include <array>
#include <new>
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY) && defined(SIMODER_SCRIPT_LANE_TEST_WITNESS)
#include "scripting/script_runtime.hpp"
#endif

namespace sc13::runtime {
namespace {
/** @summary Generic envelope ceiling; existing consumer-specific limits remain mandatory. */
constexpr std::size_t MaxOperationBytes = 1024 * 1024;
/** @summary Bound retained handles independently from the shared pending queue. */
constexpr std::size_t MaxRetainedOperations = 128;
/** @summary Bound queued and executing requests across a shared writer domain. */
constexpr std::size_t MaxPendingOperations = 8;
/** @summary Bound dependency references owned by one module generation. */
constexpr std::size_t MaxRetainedResources = 64;
/** @summary Combined native incarnation/phase rejects ABA without separate nonce/phase races. */
enum class TicketPhase : std::uint64_t { Free,Live,Claimed,Queued,Acknowledged };
/** @summary Encode one nonreused release-only identity. @param serial Native sequence. @param phase Native phase. @return Atomic word. */
constexpr std::uint64_t TicketWord(std::uint64_t serial,TicketPhase phase) noexcept {return (serial<<3)|static_cast<std::uint64_t>(phase);}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fixture-thread interleavings never run while Q is held. */
thread_local std::function<void()> commandReservedBarrier,commandWritingBarrier;
/** @summary A fixture-only negative cut cannot install a source or grant. */
thread_local RuntimeScopeTestAccess::AllocationCut authorityAllocationCut{};
/** @summary Throw once at an original allocation boundary in a separately compiled fixture.
 * @param cut Allocation being attempted. */
void FailAuthorityAllocation(RuntimeScopeTestAccess::AllocationCut cut) {
    if(authorityAllocationCut!=cut)return;
    authorityAllocationCut=RuntimeScopeTestAccess::AllocationCut::None;
    throw std::bad_alloc{};
}
#endif
/** @summary Process-local generation allocator; zero means exhausted and always denies. */
std::atomic<std::uint64_t> nextGeneration{1};
/** @summary Allocates without wraparound reuse. @return Unique nonzero generation or zero. */
std::uint64_t AllocateGeneration() noexcept {
    auto value = nextGeneration.load();
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (nextGeneration.compare_exchange_weak(value, value + 1)) return value;
    }
    return 0;
}
}

thread_local RuntimeCommitDomain::Writer* RuntimeCommitDomain::activeWriter_{};
RuntimeCommitDomain::Writer::Writer(RuntimeCommitDomain& domain, bool wait)
    : domain_(domain), lock_(domain.lane_, std::defer_lock) {
    if (domain.WriterActiveOnCurrentThread()) return;
    if (wait) lock_.lock(); else static_cast<void>(lock_.try_lock());
    if (lock_.owns_lock()) { previous_ = activeWriter_; activeWriter_ = this; }
}
bool RuntimeCommitDomain::WriterActiveOnCurrentThread() const noexcept {
    for (auto* active = activeWriter_; active; active = active->previous_)
        if (&active->domain_ == this) return true;
    return false;
}
RuntimeCommitDomain::Writer::~Writer() {
    if (lock_.owns_lock()) activeWriter_ = previous_;
}
bool RuntimeCommitDomain::Writer::CanAdvance() const noexcept {
    return Acquired() && !domain_.IsStopping() && domain_.revision_.load() != UINT64_MAX;
}
std::uint64_t RuntimeCommitDomain::Writer::Advance() noexcept {
    return CanAdvance() ? ++domain_.revision_ : 0;
}
std::uint64_t RuntimeCommitDomain::Writer::AdvanceRollback() noexcept {
    return Acquired() && domain_.revision_.load() != UINT64_MAX ? ++domain_.revision_ : 0;
}

/** @summary Small owned authority/queue state retained independently by executing callbacks. */
struct RuntimeScope::State final {
    /** @summary Fixed exact retirement mailbox; phase zero is free, one writing, two ready and three draining. */
    struct CommandRelease final {
        std::atomic_uint8_t phase{};
        OperationContext context;
        std::uint64_t ticketSerial{};
        std::size_t ticketSlot{};
    };
    std::array<CommandRelease,MaxRetainedOperations> commandReleases{};
    /** @summary Sticky impossible-capacity fault keeps remaining reservations retained and closes native admission. */
    std::atomic_bool commandReleaseOverflow{};
    /** @summary Fixed original-context tickets share the existing retained reservation admission budget. */
    struct CommandRecord final {std::atomic_uint64_t word{};OperationContext context;};
    std::array<CommandRecord,MaxRetainedOperations> commandTickets{};
    static_assert(sizeof(commandTickets)<=32U*1024U,"Reviewed fixed command-ticket metadata bound");
    /** @summary Fixed provenance beside the unchanged bounded resource vector; generic opaque resources stay unqualified. */
    std::array<bool,MaxRetainedResources> resourceScopeOwned{};
    /** @summary Native scope retirement sequence retained across off-guard destruction. */
    struct Retirement final {
        std::atomic_bool turn{};
        std::atomic_uint64_t completed{};
        std::uint64_t sequence{},joined{};
        std::size_t outstandingTickets{};
        std::uint64_t nextTicket{1};
        bool invariant{};
    } retirement;
    /** @summary Sole original grant records; production remains empty without an actual private source/proof. */
    struct Grant final {
        OperationContext context;
        std::map<std::uint64_t, OperationContext> reservations;
        std::uint64_t capabilities{};
        std::uint64_t required{};
        bool restricted{};
        bool allParticipants{};
        bool sessionAllowed{};
        bool entityAllowed{};
        bool mandatoryAllowed{};
        bool revoked{};
        std::uint64_t revision{1};
    };
    /** @summary Retained copied operation with explicit nonreplayable completion. */
    struct Operation final {
        enum class Phase { Queued, Preparing, Running, Completed, Cancelled };
        OperationContext context;
        std::vector<std::byte> input;
        std::vector<std::byte> result;
        Phase phase{Phase::Queued};
        ScopeResult outcome{ScopeResult::Busy};
        std::shared_ptr<copied::Storage> storage;
        copied::Envelope valueInput, valueResult, preparedResult;
        copied::Reservation resultArena, resultWire, metadata;
        bool valueOperation{};
        bool committed{};
        std::uint64_t sequence{},observationRevision{},terminalSequence{};
        /** @summary Check publication arithmetic without wrap. @param count Required observations. @return Available capacity. */
        bool CanPublish(std::uint64_t count) const noexcept {return !valueOperation||count<=UINT64_MAX-sequence;}
        /** @summary Reserve the factual terminal observation before an irreversible effect.
         * @param count Marker plus terminal publications, or one compound prepared publication.
         * @return Capacity; failure leaves the original task unchanged. */
        bool ReserveTerminal(std::uint64_t count) noexcept {
            if(!valueOperation||terminalSequence||!count||!CanPublish(count))return false;
            terminalSequence=sequence+count;return true;
        }
        /** @summary Publish the sole original tuple under Q; terminal observations cannot be replaced.
         * @param next New phase. @param irreversible Factual commit flag. @param status Original outcome.
         * @param revision Original admission or actual irreversible revision. @param value Copied retained result.
         * @return False without mutation when terminal or publication capacity is unavailable. */
        bool Publish(Phase next,bool irreversible,ScopeResult status,std::uint64_t revision,copied::Envelope value={}) noexcept {
            if(valueOperation){
                if(sequence&&(phase==Phase::Completed||phase==Phase::Cancelled))return false;
                if(!CanPublish(1))return false;
                const bool terminal=next==Phase::Completed||next==Phase::Cancelled;
                if(terminal&&terminalSequence&&terminalSequence!=sequence+1)return false;
                ++sequence;observationRevision=revision;
            }
            phase=next;committed=irreversible;outcome=status;valueResult=std::move(value);return true;
        }
    };
    using OperationMap=std::map<std::uint64_t,Operation>;
    /** @summary Fixed off-Q transfer holders duplicate no payload and keep original State alive through acknowledgement. */
    struct DisposalStorage final {
        OperationMap::node_type operation;
        std::shared_ptr<void> resource;
        std::shared_ptr<State> owner;
        std::uint64_t sequence{};
    };
    static_assert(sizeof(DisposalStorage)+sizeof(Retirement)+sizeof(std::unique_lock<std::mutex>)<=1024,
        "Reviewed additional native disposal holder/turn metadata bound");
    mutable std::mutex mutex;
    std::shared_ptr<RuntimeCommitDomain> domain;
    std::condition_variable drained;
    std::shared_ptr<platform::RuntimeOwner> owner;
    std::vector<std::shared_ptr<void>> resources;
    std::map<std::uint64_t, Grant> grants;
    OperationMap operations;
    std::vector<std::uint64_t> disconnectedChannels;
    std::uint64_t generation{AllocateGeneration()};
    std::uint64_t epoch{1};
    std::uint64_t nextOperation{1};
    std::uint64_t nextGrant{1};
    std::uint64_t nextReservation{1};
    std::size_t retainedReservations{};
    bool admissionClosed{};
    /** @summary Once-bound immutable source; no production constructor exists in C0. */
    std::optional<RuntimeAuthoritySource> authoritySource;
    std::map<std::uint64_t, AuthorityReview> challenges;
    std::optional<AuthorityReview> currentReview;
    std::uint64_t nextChallenge{1};
    std::size_t running{};
    std::size_t registrations{};
    std::uint32_t readiness{};
    bool ownerAvailable{};
    bool committed{};
    bool stopping{};
    bool safelyDrained{};
    StopReason reason{StopReason::ExplicitShutdown};
};

RuntimeScope::RuntimeScope(std::shared_ptr<platform::RuntimeOwner> owner, std::shared_ptr<RuntimeCommitDomain> domain)
    : state_(std::make_shared<State>()) {
    state_->domain = domain ? std::move(domain) : std::make_shared<RuntimeCommitDomain>();
    state_->ownerAvailable = owner && owner->IsAcquired();
    state_->owner = std::move(owner);
}
RuntimeScope::~RuntimeScope() { RequestStop(); }
RuntimeCommitDomain& RuntimeScope::stateDomain() noexcept { return *state_->domain; }

bool RuntimeScope::CommitActivation() {
    const std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->domain->IsStopping() || !state_->ownerAvailable || !state_->generation) return false;
    state_->committed = true;
    return true;
}
void RuntimeScope::SetReadiness(bool runtimePrepared, bool menuObserved, bool cityObserved) {
    const std::lock_guard lock(state_->mutex);
    if (state_->stopping) return;
    state_->readiness = (runtimePrepared ? RuntimeReady : 0u) |
                        (menuObserved ? MenuReady : 0u) | (cityObserved ? CityReady : 0u);
}
void RuntimeScope::RequestStop(StopReason reason) noexcept {
    const auto state = state_;
    const std::lock_guard lock(state->mutex);
    if (state->stopping) return;
    state->stopping = true;
    state->reason = reason;
    state->readiness = 0;
    if (state->epoch != std::numeric_limits<std::uint64_t>::max()) ++state->epoch;
    for (auto& [id, grant] : state->grants) { (void)id; grant.revoked = true; }
    for (auto& [id, operation] : state->operations) {
        (void)id;
        if (operation.phase == State::Operation::Phase::Queued || operation.phase == State::Operation::Phase::Preparing) {
            if(operation.Publish(State::Operation::Phase::Cancelled,false,ScopeResult::Cancelled,operation.observationRevision))
                --state->domain->pending_;
            operation.input.clear();
        }
    }
    state->drained.notify_all();
}
bool RuntimeScope::DrainFor(std::chrono::milliseconds timeout) {
    const auto state = state_;
    decltype(state->resources) released;
    {
        std::unique_lock lock(state->mutex);
        if (!state->stopping || timeout.count() < 0) return false;
        if (!state->drained.wait_for(lock, timeout, [&] { return state->running == 0; })) return false;
        state->safelyDrained = true;
        released.swap(state->resources);
        state->resourceScopeOwned.fill(false);
    }
    // @summary Resource destructors may reenter; release them after dropping the state lock.
    released.clear();
    return true;
}
bool RuntimeScope::RetainResource(std::shared_ptr<void> resource) {
    if (!resource) return false;
    const std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->domain->IsStopping() || state_->resources.size() + state_->registrations >= MaxRetainedResources) return false;
    state_->resourceScopeOwned[state_->resources.size()]=false;
    state_->resources.push_back(std::move(resource));
    return true;
}
bool RuntimeScope::ReserveRegistration() {
    const std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->domain->IsStopping() ||
        state_->resources.size() + state_->registrations >= MaxRetainedResources) return false;
    ++state_->registrations;
    return true;
}
void RuntimeScope::ReleaseRegistration() noexcept {
    const std::lock_guard lock(state_->mutex);
    if (state_->registrations) --state_->registrations;
}
ScopeSnapshot RuntimeScope::Current() const {
    const std::lock_guard lock(state_->mutex);
    return SnapshotLocked(*state_);
}
void RuntimeScope::DrainCommandReleasesLocked(State& state) noexcept {
    for(auto& slot:state.commandReleases) {
        std::uint8_t ready=2;
        if(!slot.phase.compare_exchange_strong(ready,3,std::memory_order_acquire))continue;
        if(slot.ticketSlot>=state.commandTickets.size() ||
            state.commandTickets[slot.ticketSlot].context!=slot.context ||
            AcknowledgeCommandLocked(state,slot.ticketSlot,slot.ticketSerial)!=ScopeResult::Ok)
            {state.retirement.invariant=true;state.admissionClosed=true;state.domain->RequestStop();}
        slot.phase.store(0,std::memory_order_release);
    }
}
ScopeResult RuntimeScope::AcknowledgeCommandLocked(State& state,std::size_t slot,std::uint64_t serial) noexcept {
    if(slot>=state.commandTickets.size() || !serial)return ScopeResult::Stale;
    auto& record=state.commandTickets[slot];const auto word=record.word.load(std::memory_order_acquire);
    if(word==TicketWord(serial,TicketPhase::Acknowledged))return ScopeResult::Ok;
    if(word!=TicketWord(serial,TicketPhase::Claimed) && word!=TicketWord(serial,TicketPhase::Queued))return ScopeResult::Stale;
    const auto& context=record.context;const auto grant=state.grants.find(context.grantGeneration);
    if(grant==state.grants.end() || context==grant->second.context){state.retirement.invariant=true;state.admissionClosed=true;state.domain->RequestStop();return ScopeResult::Indeterminate;}
    const auto reservation=grant->second.reservations.find(context.reservation);
    if(reservation==grant->second.reservations.end() || reservation->second!=context || !state.retainedReservations || !state.retirement.outstandingTickets){
        state.retirement.invariant=true;state.admissionClosed=true;state.domain->RequestStop();return ScopeResult::Indeterminate;
    }
    grant->second.reservations.erase(reservation);--state.retainedReservations;--state.retirement.outstandingTickets;
    record.word.store(TicketWord(serial,TicketPhase::Acknowledged),std::memory_order_release);return ScopeResult::Ok;
}
ScopeResult RuntimeScope::ReleaseCommandTicket(const CommandTicket& ticket) noexcept {
    const auto state=state_;
    if(ticket.owner_!=state->generation || !ticket.serial_ || ticket.slot_>=state->commandTickets.size())return ScopeResult::Stale;
    auto& record=state->commandTickets[ticket.slot_];auto expected=TicketWord(ticket.serial_,TicketPhase::Live);
    if(!record.word.compare_exchange_strong(expected,TicketWord(ticket.serial_,TicketPhase::Claimed),std::memory_order_acq_rel)){
        if(expected==TicketWord(ticket.serial_,TicketPhase::Acknowledged))return ScopeResult::Ok;
        if((expected>>3)!=ticket.serial_)return ScopeResult::Stale;
        return ScopeResult::Busy;
    }
    std::unique_lock lock(state->mutex,std::try_to_lock);
    if(lock.owns_lock())return AcknowledgeCommandLocked(*state,ticket.slot_,ticket.serial_);
    for(auto& slot:state->commandReleases) {
        std::uint8_t empty=0;
        if(!slot.phase.compare_exchange_strong(empty,1,std::memory_order_acquire))continue;
        slot.context=record.context;slot.ticketSerial=ticket.serial_;slot.ticketSlot=ticket.slot_;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        try{if(commandWritingBarrier)commandWritingBarrier();}catch(...){state->commandReleaseOverflow.store(true);state->domain->RequestStop();return ScopeResult::Indeterminate;}
#endif
        // @summary Publish exact transfer ownership before Ready, so a reducer never observes an unowned ready record.
        record.word.store(TicketWord(ticket.serial_,TicketPhase::Queued),std::memory_order_release);
        slot.phase.store(2,std::memory_order_release);
        return ScopeResult::NotReady;
    }
    // @summary At most 127 fresh tickets coexist with the retained root grant in the 128-slot scope budget.
    // Writing/ready/draining slots each still represent one distinct retained reservation; ordinary code cannot fill this mailbox.
    // On native invariant failure retain the original map/context for final State cleanup and stop admission, never terminate or wait.
    state->commandReleaseOverflow.store(true);state->domain->RequestStop();return ScopeResult::Indeterminate;
}
ScopeSnapshot RuntimeScope::SnapshotLocked(const State& state) noexcept {
    return {state.generation,state.epoch,state.domain->revision_.load(),state.running,state.operations.size(),
        state.committed,state.stopping,state.safelyDrained,state.readiness};
}
RuntimeScope::RetirementObservation RuntimeScope::ObserveRetirement() noexcept {
    const auto state=state_;RetirementObservation output;output.generation=state->generation;
    output.ticketBytes=sizeof(state->commandTickets);output.disposalBytes=sizeof(State::DisposalStorage)+sizeof(State::Retirement)+sizeof(std::unique_lock<std::mutex>);
    /** @summary Compare the existing mailbox layout to expose all added fixed metadata, not opaque resource payload bytes. */
    struct OriginalMailbox final {std::atomic_uint8_t phase{};OperationContext context;};
    output.metadataBytes=sizeof(State::Retirement)+sizeof(state->resourceScopeOwned)+
        state->commandReleases.size()*(sizeof(State::CommandRelease)-sizeof(OriginalMailbox));
    bool idle=false;if(!state->retirement.turn.compare_exchange_strong(idle,true)){output.result=ScopeResult::Busy;return output;}
    /** @summary Releases the native turn without running any resource destructor. */
    struct Turn final {State& state;/** @summary Release only this native observation turn. */~Turn(){state.retirement.turn.store(false,std::memory_order_release);}} turn{*state};
    std::unique_lock lock(state->mutex,std::try_to_lock);if(!lock.owns_lock()){output.result=ScopeResult::Busy;return output;}
    auto& retirement=state->retirement;
    const auto completed=retirement.completed.load(std::memory_order_acquire);
    if(completed==retirement.sequence)retirement.joined=completed;
    output.disposalSequence=retirement.sequence;output.disposalAcknowledged=retirement.joined;
    output.disposalPending=retirement.sequence!=retirement.joined;
    output.running=state->running;output.registrations=state->registrations;output.operations=state->operations.size();output.resources=state->resources.size();
    for(std::size_t index=0;index<state->resources.size();++index)if(!state->resourceScopeOwned[index])++output.unknownResources;
    output.commandTickets=retirement.outstandingTickets;output.stopping=state->stopping;
    output.invariant=retirement.invariant || state->commandReleaseOverflow.load();
    for(const auto& mailbox:state->commandReleases){const auto phase=mailbox.phase.load(std::memory_order_acquire);if(phase==1)++output.mailboxWriting;else if(phase==2)++output.mailboxReady;else if(phase==3)++output.mailboxDraining;}
    for(const auto& [grantId,grant]:state->grants){
        static_cast<void>(grantId);
        for(const auto& [serial,context]:grant.reservations){
            static_cast<void>(serial);if(context==grant.context){++output.rootReservations;continue;}
            bool tracked=false;
            for(const auto& ticket:state->commandTickets){const auto phase=static_cast<TicketPhase>(ticket.word.load(std::memory_order_acquire)&7U);
                if(phase!=TicketPhase::Free && phase!=TicketPhase::Acknowledged && ticket.context==context){tracked=true;break;}}
            if(!tracked)++output.nonCommandFreshReservations;
        }
    }
    output.scopeReady=state->generation && state->stopping && state->admissionClosed && !output.running && !output.registrations &&
        !output.operations && !output.resources && !output.commandTickets && !output.nonCommandFreshReservations &&
        !output.mailboxWriting && !output.mailboxReady && !output.mailboxDraining && !output.disposalPending && !output.invariant;
    output.result=output.scopeReady?ScopeResult::Ok:ScopeResult::NotReady;return output;
}
ScopeResult RuntimeScope::DrainRetirementOne() noexcept {
    const auto state=state_;
    if(state->domain->WriterActiveOnCurrentThread())return ScopeResult::Denied;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY) && defined(SIMODER_SCRIPT_LANE_TEST_WITNESS)
    if(scripting::ScriptRuntime::OnScriptLane())return ScopeResult::Denied;
#endif
    bool idle=false;if(!state->retirement.turn.compare_exchange_strong(idle,true))return ScopeResult::Busy;
    /** @summary Keeps arbitrary destruction outside Q while retaining exact turn ownership. */
    struct Turn final {State& state;/** @summary Release the turn after off-guard destruction and native acknowledgement. */~Turn(){state.retirement.turn.store(false,std::memory_order_release);}} turn{*state};
    State::DisposalStorage disposal;disposal.owner=state;
    {
        std::unique_lock lock(state->mutex,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
        auto& retirement=state->retirement;
        if(retirement.sequence!=retirement.joined)return ScopeResult::NotReady;
        RetireLocked(*state,StopReason::ExplicitShutdown);DrainCommandReleasesLocked(*state);
        if(retirement.invariant || state->commandReleaseOverflow.load())return ScopeResult::Indeterminate;
        if(state->running || state->registrations)return ScopeResult::NotReady;
        auto operation=state->operations.begin();
        while(operation!=state->operations.end() && operation->second.phase==State::Operation::Phase::Running)++operation;
        std::size_t resource=0;while(resource<state->resources.size() && !state->resourceScopeOwned[resource])++resource;
        if(operation==state->operations.end() && resource==state->resources.size())return ScopeResult::NotReady;
        if(retirement.sequence==UINT64_MAX)return ScopeResult::Denied;
        if(operation!=state->operations.end())disposal.operation=state->operations.extract(operation);
        if(resource<state->resources.size()){
            disposal.resource=std::move(state->resources[resource]);
            if(resource+1!=state->resources.size()){state->resources[resource]=std::move(state->resources.back());state->resourceScopeOwned[resource]=state->resourceScopeOwned[state->resources.size()-1];}
            state->resourceScopeOwned[state->resources.size()-1]=false;state->resources.pop_back();
        }
        disposal.sequence=++retirement.sequence;
    }
    // @summary Copied operation pools are native-owned; resource provenance was explicitly enrolled by a closed native issuer.
    disposal.operation={};disposal.resource.reset();
    state->retirement.completed.store(disposal.sequence,std::memory_order_release);
    return ScopeResult::Ok;
}

ScopeResult RuntimeScope::Validate(const State& state, const OperationContext& context, bool checkRevision,
    bool checkGrantRevision, bool checkCapability, bool candidate) {
    if (!state.generation || context.owner != state.generation || context.epoch != state.epoch) return ScopeResult::Stale;
    if (state.stopping || state.domain->IsStopping() || (!candidate && !state.committed) ||
        (candidate && state.committed) || !state.ownerAvailable) return ScopeResult::NotReady;
    if (!SourceMatches(state,context.origin)) return ScopeResult::Denied;
    if (state.admissionClosed || !context.opcode ||
        !context.readiness || (context.readiness & ~(RuntimeReady | MenuReady | CityReady)) != 0 ||
        std::chrono::steady_clock::now() >= context.deadline) return ScopeResult::Denied;
    const auto grant = state.grants.find(context.grantGeneration);
    if (grant == state.grants.end() || grant->second.revoked) return ScopeResult::Denied;
    const auto& effective = grant->second;
    if(effective.context.origin.index()!=context.origin.index())return ScopeResult::Denied;
    const auto reservation = effective.reservations.find(context.reservation);
    if (reservation == effective.reservations.end() || reservation->second != context ||
        (checkGrantRevision && context.grantRevision != effective.revision) ||
        !effective.mandatoryAllowed || context.opcode > 64 ||
        (checkCapability && !(effective.capabilities & (std::uint64_t{1} << (context.opcode - 1)))) ||
        (effective.required & effective.capabilities) != effective.required) return ScopeResult::Denied;
    if (const auto* script = std::get_if<OrdinaryScriptOrigin>(&context.origin)) {
        if (script->owner != state.generation || !script->content) return ScopeResult::Denied;
    } else if (const auto* agent = std::get_if<AgentOrigin>(&context.origin)) {
        if (agent->owner != state.generation || !agent->content || !agent->actor || !agent->channel ||
            !agent->consentSession || !agent->entity || !agent->extent ||
            !(effective.sessionAllowed || (effective.entityAllowed &&
                std::get<AgentOrigin>(effective.context.origin).actor == agent->actor &&
                std::get<AgentOrigin>(effective.context.origin).entity == agent->entity)) ||
            std::find(state.disconnectedChannels.begin(), state.disconnectedChannels.end(), agent->channel) != state.disconnectedChannels.end())
            return ScopeResult::Denied;
    } else return ScopeResult::Denied;
    if ((state.readiness & context.readiness) != context.readiness) return ScopeResult::NotReady;
    if (checkRevision && context.revision != state.domain->revision_.load()) return ScopeResult::Conflict;
    return ScopeResult::Ok;
}

RuntimeScope::LifecycleGuard::LifecycleGuard(RuntimeScope& scope) noexcept
    : scope_(scope), lock_(scope.state_->mutex, std::try_to_lock) {}
ScopeResult RuntimeScope::LifecycleGuard::Check(const OperationContext& context, bool candidate) const {
    return Acquired() ? Validate(*scope_.state_, context, true, true, true, candidate) : ScopeResult::Busy;
}
bool RuntimeScope::LifecycleGuard::InDomain(const RuntimeCommitDomain& domain) const noexcept {
    return Acquired() && scope_.state_->domain.get() == &domain;
}
ScopeSnapshot RuntimeScope::LifecycleGuard::Snapshot() const noexcept {
    return Acquired() ? RuntimeScope::SnapshotLocked(*scope_.state_) : ScopeSnapshot{};
}
void RuntimeScope::LifecycleGuard::Retire() noexcept {
    RuntimeScope::RetireLocked(*scope_.state_,StopReason::OwnerLost);
}
void RuntimeScope::RetireLocked(State& state,StopReason reason) noexcept {
    state.admissionClosed=true;
    if (state.stopping) return;
    state.stopping = true; state.admissionClosed = true; state.readiness = 0;
    state.reason = reason;
    if (state.epoch != UINT64_MAX) ++state.epoch;
    for (auto& [id, grant] : state.grants) { (void)id; grant.revoked = true; }
    for (auto& [id, operation] : state.operations) {
        (void)id;
        if (operation.phase == State::Operation::Phase::Queued || operation.phase == State::Operation::Phase::Preparing) {
            if(operation.Publish(State::Operation::Phase::Cancelled,false,ScopeResult::Cancelled,operation.observationRevision))
                --state.domain->pending_;
            // @summary Keep copied envelopes, buffers and dependency graphs retained until cleanup outside every fence.
        }
    }
    state.drained.notify_all();
}
void RuntimeScope::LifecycleGuard::Activate() noexcept { scope_.state_->committed = true; }

ScopeResult RuntimeScope::Queue(const OperationContext& context, std::span<const std::byte> input, ScopeHandle& output) {
    output = {};
    if (input.size() > MaxOperationBytes) return ScopeResult::Invalid;
    const std::lock_guard lock(state_->mutex);
    const auto valid = Validate(*state_, context, true);
    if (valid != ScopeResult::Ok) return valid;
    if (state_->operations.size() >= MaxRetainedOperations ||
        state_->nextOperation == std::numeric_limits<std::uint64_t>::max()) return ScopeResult::Busy;
    const auto serial = state_->nextOperation++;
    State::Operation operation;
    operation.context = context;
    operation.input.assign(input.begin(), input.end());
    auto pending = state_->domain->pending_.load();
    do { if (pending >= MaxPendingOperations) return ScopeResult::Busy; }
    while (!state_->domain->pending_.compare_exchange_weak(pending, pending + 1));
    try { state_->operations.emplace(serial, std::move(operation)); }
    catch (...) { --state_->domain->pending_; throw; }
    output = {state_->generation, serial};
    return ScopeResult::Ok;
}

ScopeResult RuntimeScope::Commit(const OperationContext& context, ScopeHandle operation, CommitCallback callback) {
    if (!callback) return ScopeResult::Invalid;
    const auto state = state_;
    auto lane = state->domain->ReserveWriter();
    if (!lane.Acquired()) return ScopeResult::Busy;
    std::vector<std::byte> input;
    {
        const std::lock_guard lock(state->mutex);
        if (operation.owner != state->generation) return ScopeResult::Stale;
        const auto found = state->operations.find(operation.serial);
        if (found == state->operations.end()) return ScopeResult::Stale;
        if (!(found->second.context == context)) return ScopeResult::Denied;
        if (found->second.valueOperation) return ScopeResult::Invalid;
        if (found->second.phase == State::Operation::Phase::Cancelled) return ScopeResult::Cancelled;
        if (found->second.phase != State::Operation::Phase::Queued) return ScopeResult::Stale;
        const auto valid = Validate(*state, context, true);
        if (valid != ScopeResult::Ok) return valid;
        if (state->domain->revision_.load() == std::numeric_limits<std::uint64_t>::max()) return ScopeResult::Conflict;
        found->second.phase = State::Operation::Phase::Running;
        input = std::move(found->second.input);
        ++state->running;
        // @summary Reserve the revision at the irreversible commit point; completion never promises undo.
        ++state->domain->revision_;
    }
    ScopeResult outcome = ScopeResult::Ok;
    std::vector<std::byte> result;
    try {
        result = callback(input);
        if (result.size() > MaxOperationBytes) { result.clear(); outcome = ScopeResult::Indeterminate; }
    } catch (...) { outcome = ScopeResult::Indeterminate; }
    {
        const std::lock_guard lock(state->mutex);
        auto& retained = state->operations.at(operation.serial);
        retained.result = std::move(result);
        retained.outcome = outcome;
        retained.phase = State::Operation::Phase::Completed;
        --state->running;
        --state->domain->pending_;
        state->drained.notify_all();
    }
    return outcome;
}

ScopeResult RuntimeScope::Poll(const OperationContext& context, ScopeHandle operation, std::size_t capacity,
                               std::vector<std::byte>& output) const {
    output.clear();
    const std::lock_guard lock(state_->mutex);
    if (operation.owner != state_->generation) return ScopeResult::Stale;
    const auto found = state_->operations.find(operation.serial);
    if (found == state_->operations.end()) return ScopeResult::Stale;
    if (!(found->second.context == context)) return ScopeResult::Denied;
    const auto valid = Validate(*state_, context, false);
    if (valid != ScopeResult::Ok) return valid;
    if (found->second.phase == State::Operation::Phase::Cancelled) return ScopeResult::Cancelled;
    if (found->second.phase != State::Operation::Phase::Completed) return ScopeResult::Busy;
    if (found->second.result.size() > capacity) return ScopeResult::BufferSmall;
    output = found->second.result;
    return found->second.outcome;
}
ScopeResult RuntimeScope::Cancel(const OperationContext& context, ScopeHandle operation) {
    const std::lock_guard lock(state_->mutex);
    if (operation.owner != state_->generation) return ScopeResult::Stale;
    const auto found = state_->operations.find(operation.serial);
    if (found == state_->operations.end()) return ScopeResult::Stale;
    if (!(found->second.context == context)) return ScopeResult::Denied;
    const auto valid = Validate(*state_, context, false);
    if (valid != ScopeResult::Ok) return valid;
    if (found->second.phase == State::Operation::Phase::Running) return ScopeResult::Busy;
    if (found->second.phase == State::Operation::Phase::Completed) return ScopeResult::Stale;
    if (found->second.phase == State::Operation::Phase::Cancelled) return ScopeResult::Cancelled;
    if(!found->second.Publish(State::Operation::Phase::Cancelled,false,ScopeResult::Cancelled,found->second.observationRevision))return ScopeResult::Busy;
    --state_->domain->pending_;
    found->second.input.clear();
    return ScopeResult::Ok;
}
ScopeResult RuntimeScope::Release(std::uint64_t owner, ScopeHandle operation) {
    // @summary Extract without allocation, then reclaim retained payloads after releasing the scope guard.
    decltype(state_->operations)::node_type retired;
    {
        const std::lock_guard lock(state_->mutex);
        if (owner != state_->generation || operation.owner != owner) return ScopeResult::Denied;
        const auto found = state_->operations.find(operation.serial);
        if (found == state_->operations.end()) return ScopeResult::Stale;
        if (found->second.phase == State::Operation::Phase::Running) return ScopeResult::Busy;
        if (found->second.phase == State::Operation::Phase::Queued ||
            found->second.phase == State::Operation::Phase::Preparing) --state_->domain->pending_;
        retired = state_->operations.extract(found);
    }
    return ScopeResult::Ok;
}

ScopeResult RuntimeScope::QueueValue(const OperationContext& context, const copied::Envelope& input,
                                    const std::shared_ptr<copied::Storage>& storage,
                                    std::size_t maxResultWire, ScopeHandle& output) {
    output = {};
    if (!storage || !storage->Owns(input, context.owner) || !maxResultWire || maxResultWire > copied::Storage::EnvelopeLimit)
        return ScopeResult::Invalid;
    const std::lock_guard lock(state_->mutex);
    const auto valid = Validate(*state_, context, true);
    if (valid != ScopeResult::Ok) return valid;
    if (state_->operations.size() >= MaxRetainedOperations || state_->nextOperation == UINT64_MAX) return ScopeResult::Busy;
    State::Operation operation;
    // @summary Map node payload plus links/alignment is conservatively accounted before emplace.
    if (storage->Account(context.owner, sizeof(State::Operation) + 128, operation.metadata) != copied::Status::Ok ||
        storage->Allocate(context.owner, copied::Storage::DecodeCapacity(maxResultWire), operation.resultArena) != copied::Status::Ok ||
        storage->Allocate(context.owner, maxResultWire, operation.resultWire) != copied::Status::Ok) return ScopeResult::Busy;
    operation.context = context; operation.valueInput = input; operation.storage = storage; operation.valueOperation = true;
    auto pending = state_->domain->pending_.load();
    do { if (pending >= MaxPendingOperations) return ScopeResult::Busy; }
    while (!state_->domain->pending_.compare_exchange_weak(pending, pending + 1));
    const auto serial = state_->nextOperation++;
    try { auto inserted=state_->operations.emplace(serial, std::move(operation));
        // @summary Sequence one exists only after successful retained insertion; Q keeps this initial publication atomic.
        static_cast<void>(inserted.first->second.Publish(State::Operation::Phase::Queued,false,ScopeResult::Busy,context.revision)); }
    catch (...) { --state_->domain->pending_; return ScopeResult::Busy; }
    output = {state_->generation, serial};
    return ScopeResult::Ok;
}
ScopeResult RuntimeScope::BeginValue(ScopeHandle operation) {
    const std::lock_guard lock(state_->mutex);
    if (operation.owner != state_->generation) return ScopeResult::Stale;
    const auto found = state_->operations.find(operation.serial);
    if (found == state_->operations.end() || !found->second.valueOperation) return ScopeResult::Stale;
    auto& retained = found->second;
    const auto valid = Validate(*state_, retained.context, true);
    if (valid != ScopeResult::Ok) return valid;
    if (retained.phase == State::Operation::Phase::Cancelled) return ScopeResult::Cancelled;
    if (retained.phase != State::Operation::Phase::Queued) return ScopeResult::Stale;
    // @summary Preparation must preserve one final cancellation/failure publication even at synthetic near-exhaustion.
    if(!retained.CanPublish(2))return ScopeResult::Busy;
    if(!retained.Publish(State::Operation::Phase::Preparing,false,retained.outcome,retained.observationRevision))return ScopeResult::Busy;
    return ScopeResult::Ok;
}
ScopeResult RuntimeScope::CommitValue(ScopeHandle operation, const ValueCallback& callback,
                                    const std::function<ScopeResult()>& precommit) {
    if (!callback) return ScopeResult::Invalid;
    const auto state = state_;
    auto lane = state->domain->ReserveWriter();
    if (!lane.Acquired()) return ScopeResult::Busy;
    State::Operation* retained{};
    {
        const std::lock_guard lock(state->mutex);
        if (operation.owner != state->generation) return ScopeResult::Stale;
        const auto found = state->operations.find(operation.serial);
        if (found == state->operations.end() || !found->second.valueOperation) return ScopeResult::Stale;
        retained = &found->second;
        if (retained->phase == State::Operation::Phase::Cancelled) return ScopeResult::Cancelled;
        if (retained->phase != State::Operation::Phase::Preparing) return ScopeResult::Stale;
        const auto valid = Validate(*state, retained->context, true);
        if (valid != ScopeResult::Ok) return valid;
        // @summary Trusted preflight reads only native atomic fences; no Lua, allocation or reentrant lock acquisition is permitted.
        if (precommit) {
            ScopeResult fenced = ScopeResult::Denied;
            try { fenced = precommit(); } catch (...) { return ScopeResult::Denied; }
            if (fenced != ScopeResult::Ok) return fenced;
        }
        if (state->domain->revision_.load() == UINT64_MAX) return ScopeResult::Conflict;
        if(!retained->ReserveTerminal(2))return ScopeResult::Busy;
        const auto committedRevision=++state->domain->revision_;
        // @summary Under Q, the checked two-publication reservation guarantees both this marker and its terminal record.
        static_cast<void>(retained->Publish(State::Operation::Phase::Running,true,ScopeResult::Busy,committedRevision));
        ++state->running;
    }
    ScopeResult outcome = ScopeResult::Indeterminate;
    copied::Envelope result;
    try {
        const auto bytes = retained->resultWire.Bytes();
        std::span<char> wire(reinterpret_cast<char*>(bytes.data()), bytes.size());
        std::size_t written{};
        const bool verified = callback(retained->valueInput, wire, written);
        if (verified && written <= wire.size() &&
            retained->storage->DecodeReserved({wire.data(), written}, retained->valueInput.Schema(), retained->resultArena, result, wire.size()) == copied::Status::Ok)
            outcome = ScopeResult::Ok;
    } catch (...) { outcome = ScopeResult::Indeterminate; }
    {
        const std::lock_guard lock(state->mutex);
        static_cast<void>(retained->Publish(State::Operation::Phase::Completed,true,outcome,retained->observationRevision,std::move(result)));
        retained->valueInput = {}; retained->resultArena = {}; retained->resultWire = {};
        --state->running; --state->domain->pending_;
        state->drained.notify_all();
    }
    return outcome;
}
ScopeResult RuntimeScope::CommitPreparedValue(ScopeHandle operation, RuntimeCommitDomain::Writer& writer,
    mods::PatchRegistry::PublicationGuard& publication, mods::PatchRegistry::PreparedPublication& plan,
    std::uint64_t scopeGeneration, std::uint64_t runtimeEpoch, const copied::Envelope& input) {
    const auto state = state_;
    if (&writer.domain_ != state->domain.get() || !writer.Acquired()) return ScopeResult::Denied;
    if (!publication.owns_lock()) return ScopeResult::Busy;
    if (!publication.CanPublish(plan)) return ScopeResult::Conflict;
    const std::unique_lock lock(state->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return ScopeResult::Busy;
    if (operation.owner != state->generation || scopeGeneration != state->generation || runtimeEpoch != state->epoch)
        return ScopeResult::Stale;
    const auto found = state->operations.find(operation.serial);
    if (found == state->operations.end() || !found->second.valueOperation) return ScopeResult::Stale;
    auto& retained = found->second;
    if (retained.phase == State::Operation::Phase::Cancelled) return ScopeResult::Cancelled;
    if (retained.phase != State::Operation::Phase::Preparing) return ScopeResult::Stale;
    if (!input || input.Schema() != retained.valueInput.Schema() || input.Encode() != retained.valueInput.Encode())
        return ScopeResult::Invalid;
    const auto valid = Validate(*state, retained.context, true);
    if (valid != ScopeResult::Ok) return valid;
    if (!writer.CanAdvance()) return ScopeResult::NotReady;
    if (retained.preparedResult) return ScopeResult::Stale;
    const auto revision = state->domain->Revision();
    if (revision == UINT64_MAX || plan.ExpectedGeneration() == UINT64_MAX) return ScopeResult::Conflict;
    if(!retained.CanPublish(1))return ScopeResult::Busy;
    // @summary Bounded formatting and the sole codec finish before the irreversible marker; no result allocation follows publication.
    const auto bytes = retained.resultWire.Bytes();
    auto* wire = reinterpret_cast<char*>(bytes.data());
    const int written = std::snprintf(wire, bytes.size(),
        "{\"v\":1,\"schema\":\"simoder.registry-publication.v1\",\"value\":[\"map\",["
        "[\"domainRevision\",[\"u64\",\"%llu\"]],[\"live\",[\"string\",\"not-requested\"]],"
        "[\"publication\",[\"string\",\"registry-committed\"]],[\"registryGeneration\",[\"u64\",\"%llu\"]]]]}",
        static_cast<unsigned long long>(revision + 1),
        static_cast<unsigned long long>(plan.ExpectedGeneration() + 1));
    if (written < 0 || static_cast<std::size_t>(written) >= bytes.size()) return ScopeResult::BufferSmall;
    if (retained.storage->DecodeReserved({wire, static_cast<std::size_t>(written)},
        "simoder.registry-publication.v1", retained.resultArena, retained.preparedResult, bytes.size()) != copied::Status::Ok)
        return ScopeResult::Invalid;
    // @summary Advance rechecks sticky stop. If stop wins, the hidden prepared receipt remains undisclosed until owner cleanup.
    if(!retained.ReserveTerminal(1))return ScopeResult::Busy;
    const auto committedRevision = writer.Advance();
    if (!committedRevision) return ScopeResult::NotReady;
    const bool published = publication.Publish(plan, committedRevision);
    static_cast<void>(retained.Publish(State::Operation::Phase::Completed,true,
        published?ScopeResult::Ok:ScopeResult::Indeterminate,committedRevision,published?std::move(retained.preparedResult):copied::Envelope{}));
    --state->domain->pending_;
    // @summary Input, reserved buffers and abandoned receipts retire only on later owner cleanup, outside all publication reservations.
    return retained.outcome;
}
ScopeResult RuntimeScope::FinishValue(ScopeHandle operation, ScopeResult reason) {
    if (reason == ScopeResult::Ok || reason == ScopeResult::Indeterminate) return ScopeResult::Invalid;
    const std::lock_guard lock(state_->mutex);
    if (operation.owner != state_->generation) return ScopeResult::Stale;
    const auto found = state_->operations.find(operation.serial);
    if (found == state_->operations.end() || !found->second.valueOperation) return ScopeResult::Stale;
    auto& retained = found->second;
    // @summary Owner-bound native failure cleanup cannot disclose payloads or execute effects; revoked work must release pending capacity.
    if (reason == ScopeResult::Cancelled && retained.committed) return ScopeResult::Busy;
    if (retained.phase == State::Operation::Phase::Running) return ScopeResult::Busy;
    if (retained.phase == State::Operation::Phase::Completed || retained.phase == State::Operation::Phase::Cancelled) return ScopeResult::Stale;
    if(!retained.Publish(reason==ScopeResult::Cancelled?State::Operation::Phase::Cancelled:State::Operation::Phase::Completed,
        false,reason,retained.observationRevision))return ScopeResult::Busy;
    retained.valueInput = {}; retained.resultArena = {}; retained.resultWire = {};
    --state_->domain->pending_;
    return ScopeResult::Ok;
}
ScopeResult RuntimeScope::PollValue(ScopeHandle operation, TaskSnapshot& output) const {
    output = {};
    const std::lock_guard lock(state_->mutex);
    if (operation.owner != state_->generation) return ScopeResult::Stale;
    const auto found = state_->operations.find(operation.serial);
    if (found == state_->operations.end() || !found->second.valueOperation) return ScopeResult::Stale;
    const auto& retained = found->second;
    const auto valid = Validate(*state_, retained.context, false);
    if (valid != ScopeResult::Ok) return valid;
    output.committed = retained.committed; output.outcome = retained.outcome;
    output.sequence=retained.sequence;output.revision=retained.observationRevision;
    switch (retained.phase) {
    case State::Operation::Phase::Queued: output.state = TaskState::Queued; break;
    case State::Operation::Phase::Preparing: case State::Operation::Phase::Running: output.state = TaskState::Running; break;
    case State::Operation::Phase::Cancelled: output.state = TaskState::Cancelled; break;
    case State::Operation::Phase::Completed:
        output.state = retained.outcome == ScopeResult::Ok ? TaskState::Succeeded : retained.outcome == ScopeResult::Indeterminate ? TaskState::Indeterminate : TaskState::Failed;
        output.value = retained.valueResult; break;
    }
    return ScopeResult::Ok;
}
ScopeResult RuntimeScope::CancelValue(ScopeHandle operation) {
    TaskSnapshot snapshot;
    const auto access = PollValue(operation, snapshot);
    if (access != ScopeResult::Ok) return access;
    return FinishValue(operation, ScopeResult::Cancelled);
}

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
bool RuntimeScopeTestAccess::SeedTaskSequence(RuntimeScope& scope,ScopeHandle task,std::uint64_t sequence){
    const std::lock_guard lock(scope.state_->mutex);
    if(task.owner!=scope.state_->generation||(sequence!=UINT64_MAX-2&&sequence!=UINT64_MAX-1))return false;
    const auto found=scope.state_->operations.find(task.serial);
    if(found==scope.state_->operations.end()||!found->second.valueOperation||found->second.committed||found->second.terminalSequence||
        (found->second.phase!=RuntimeScope::State::Operation::Phase::Queued&&found->second.phase!=RuntimeScope::State::Operation::Phase::Preparing))return false;
    found->second.sequence=sequence;return true;
}
std::unique_lock<std::mutex> RuntimeScopeTestAccess::ReserveState(RuntimeScope& scope) {
    return std::unique_lock<std::mutex>(scope.state_->mutex, std::try_to_lock);
}
#endif
bool RuntimeScope::SourceMatches(const State& state,const OperationOrigin& origin) noexcept {
    if(!state.authoritySource || state.authoritySource->owner_!=state.generation || !state.authoritySource->content_)return false;
    if (const auto* script = std::get_if<OrdinaryScriptOrigin>(&origin))
        return script->owner == state.generation && script->content==state.authoritySource->content_;
    if (const auto* agent = std::get_if<AgentOrigin>(&origin))
        return agent->owner == state.generation && agent->content==state.authoritySource->content_ && agent->actor && agent->channel &&
            agent->consentSession && agent->entity && agent->extent;
    // @summary C0 has no actual Native State/current-content source constructor; scalar native claims always deny.
    return false;
}
RuntimeScope::AuthorityChallenge RuntimeScope::ChallengeAuthority(const AuthorityReview& review) {
    const std::lock_guard lock(state_->mutex);
    auto& state = *state_;
    if (state.stopping || state.admissionClosed || state.domain->IsStopping() ||
        state.nextChallenge == UINT64_MAX ||
        !SourceMatches(state,review.origin) ||
        (std::holds_alternative<OrdinaryScriptOrigin>(review.origin) &&
            (review.sessionAllowed || review.entityAllowed || review.allParticipants)) ||
        !review.revision || !review.action ||
        !review.requested || (review.required & ~review.requested) || (review.approved & ~review.requested) ||
        !review.opcode || review.opcode > 64 || !review.readiness ||
        (review.readiness & ~(RuntimeReady | MenuReady | CityReady)) ||
        review.expires <= std::chrono::steady_clock::now() ||
        review.expires > std::chrono::steady_clock::now() + std::chrono::hours(24)) return {};
    if (const auto* agent = std::get_if<AgentOrigin>(&review.origin))
        if (std::find(state.disconnectedChannels.begin(), state.disconnectedChannels.end(), agent->channel) !=
            state.disconnectedChannels.end()) return {};
    if (state.currentReview && review.revision < state.currentReview->revision) return {};
    if (state.currentReview && *state.currentReview != review) state.challenges.clear();
    if (state.challenges.size() >= MaxRetainedOperations) return {};
    const auto serial = state.nextChallenge++;
    try {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        FailAuthorityAllocation(RuntimeScopeTestAccess::AllocationCut::Challenge);
#endif
        state.challenges.emplace(serial, review);
    }
    catch(...){return {};}
    state.currentReview = review;
    return {state.generation, serial};
}
OperationContext RuntimeScope::DecideAuthority(const RuntimeAuthoritySource::Decision& decision) {
    auto writer = state_->domain->ReserveWriter();
    if (!writer.Acquired()) return {};
    const std::lock_guard lock(state_->mutex);
    auto& state = *state_;
    const auto& review=decision.review_;const auto challenge=decision.challenge_;
    if(!state.authoritySource || decision.owner_!=state.generation || decision.content_!=state.authoritySource->content_)return {};
    if (challenge.owner != state.generation) return {};
    const auto found = state.challenges.find(challenge.serial);
    if (found == state.challenges.end()) return {};
    const auto reviewed = found->second;
    state.challenges.erase(found);
    if (reviewed != review || !SourceMatches(state,review.origin) || state.stopping || state.admissionClosed ||
        state.domain->IsStopping() || review.expires <= std::chrono::steady_clock::now())
        return {};
    if ((review.required & review.approved) != review.required ||
        (review.approved != review.requested && !review.supportsRestricted)) {
        // @summary Close admission before deferred retirement; authority bookkeeping never performs physical patch cleanup.
        state.admissionClosed = true;
        for (auto& [id, grant] : state.grants) { (void)id; grant.revoked = true; }
        return {};
    }
    if (state.grants.size() >= MaxRetainedOperations || state.retainedReservations >= MaxRetainedOperations ||
        state.nextGrant == UINT64_MAX || state.nextReservation == UINT64_MAX) return {};
    const auto serial = state.nextGrant++;
    OperationContext context;
    context.owner = state.generation; context.epoch = state.epoch; context.grantGeneration = serial;
    context.grantRevision = 1;
    context.origin = review.origin; context.decision = {challenge.serial, review.revision, review.action};
    context.reservation = state.nextReservation++; context.revision = state.domain->Revision();
    context.opcode = review.opcode; context.readiness = review.readiness; context.deadline = review.expires;
    RuntimeScope::State::Grant grant;
    grant.context = context;
    grant.capabilities = review.approved; grant.required = review.required;
    grant.restricted = review.approved != review.requested; grant.allParticipants = review.allParticipants;
    grant.sessionAllowed = review.sessionAllowed; grant.entityAllowed = review.entityAllowed;
    grant.mandatoryAllowed = review.mandatoryAllowed;
    try {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        FailAuthorityAllocation(RuntimeScopeTestAccess::AllocationCut::RootReservation);
#endif
        grant.reservations.emplace(context.reservation,context);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        FailAuthorityAllocation(RuntimeScopeTestAccess::AllocationCut::Grant);
#endif
        state.grants.emplace(serial,std::move(grant));
    }
    catch(...){return {};}
    ++state.retainedReservations;
    return context;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
OperationContext RuntimeScopeTestAccess::Reserve(RuntimeScope& scope, const OperationContext& context) {
    const std::lock_guard lock(scope.state_->mutex);
    return ReserveLocked(scope,context);
}
OperationContext RuntimeScopeTestAccess::TryReserve(RuntimeScope& scope,const OperationContext& context) {
    std::unique_lock lock(scope.state_->mutex,std::try_to_lock);if(!lock.owns_lock())return {};
    return ReserveLocked(scope,context);
}
#endif
OperationContext RuntimeScope::TryReserveCommand(const OperationContext& context,CommandTicket& ticket) {
    if(ticket.serial_ || context.opcode!=1)return {};
    return TryReserveTracked(context,ticket,ReservationRole::Command);
}
ScopeResult RuntimeScope::CheckRootDecision(const OperationContext& context) noexcept {
    std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock())return ScopeResult::Busy;
    if(state_->stopping || state_->admissionClosed || state_->domain->IsStopping())return ScopeResult::Denied;
    const auto result=Validate(*state_,context,false,false,true,!state_->committed);if(result!=ScopeResult::Ok)return result;
    const auto grant=state_->grants.find(context.grantGeneration);
    return grant!=state_->grants.end() && grant->second.context==context?ScopeResult::Ok:ScopeResult::Denied;
}
OperationContext RuntimeScope::TryReserveTracked(const OperationContext& context,CommandTicket& ticket,ReservationRole role,ScopeResult* status) {
    if(status)*status=ScopeResult::NotReady;
    if(ticket.serial_){if(status)*status=ScopeResult::Invalid;return {};}
    OperationContext fresh;
    {
        std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock()){if(status)*status=ScopeResult::Busy;return {};}
        if(status && (state_->stopping || state_->admissionClosed || state_->domain->IsStopping())){*status=ScopeResult::Denied;return {};}
        const bool candidate=role==ReservationRole::LifecycleDecision && !state_->committed;
        const auto valid=Validate(*state_,context,false,false,true,candidate);if(valid!=ScopeResult::Ok){if(status)*status=valid;return {};}
        if(state_->retirement.invariant || state_->commandReleaseOverflow.load() || state_->retirement.nextTicket>(UINT64_MAX>>3))return {};
        std::size_t slot=state_->commandTickets.size();
        for(std::size_t index=0;index<state_->commandTickets.size();++index){
            const auto phase=static_cast<TicketPhase>(state_->commandTickets[index].word.load(std::memory_order_acquire)&7U);
            if(phase==TicketPhase::Free || phase==TicketPhase::Acknowledged){slot=index;break;}
        }
        if(slot==state_->commandTickets.size())return {};
        fresh=ReserveFreshLocked(context,role);
        if(!fresh.reservation || fresh.reservation==context.reservation)return {};
        auto& record=state_->commandTickets[slot];record.context=fresh;
        ticket.owner_=state_->generation;ticket.slot_=slot;ticket.serial_=state_->retirement.nextTicket++;
        ++state_->retirement.outstandingTickets;
        record.word.store(TicketWord(ticket.serial_,TicketPhase::Live),std::memory_order_release);
    }
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    try{if(commandReservedBarrier)commandReservedBarrier();}
    catch(...){static_cast<void>(ReleaseCommandTicket(ticket));ticket={};return {};}
#endif
    if(status)*status=ScopeResult::Ok;
    return fresh;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
ScopeRetirementTestAccess::Reservation ScopeRetirementTestAccess::Reserve(RuntimeScope& scope,const OperationContext& context){
    Reservation result;result.context=scope.TryReserveCommand(context,result.ticket);return result;
}
ScopeResult ScopeRetirementTestAccess::Release(RuntimeScope& scope,const Ticket& ticket){return scope.ReleaseCommandTicket(ticket);}
ScopeRetirementTestAccess::Observation ScopeRetirementTestAccess::Observe(RuntimeScope& scope){return scope.ObserveRetirement();}
ScopeResult ScopeRetirementTestAccess::DrainOne(RuntimeScope& scope){return scope.DrainRetirementOne();}
bool ScopeRetirementTestAccess::RetainSafe(RuntimeScope& scope,std::shared_ptr<void> resource){
    if(!resource)return false;const std::scoped_lock lock(scope.state_->mutex);
    if(scope.state_->stopping || scope.state_->domain->IsStopping() || scope.state_->resources.size()+scope.state_->registrations>=MaxRetainedResources)return false;
    scope.state_->resources.push_back(std::move(resource));scope.state_->resourceScopeOwned[scope.state_->resources.size()-1]=true;return true;
}
void ScopeRetirementTestAccess::Barriers(std::function<void()> reserved,std::function<void()> writing){
    commandReservedBarrier=std::move(reserved);commandWritingBarrier=std::move(writing);
}
OperationContext RuntimeScopeTestAccess::ReserveLocked(RuntimeScope& scope,const OperationContext& context) {
    return scope.ReserveFreshLocked(context,RuntimeScope::ReservationRole::Command);
}
#endif
OperationContext RuntimeScope::ReserveFreshLocked(const OperationContext& context,ReservationRole role) {
    DrainCommandReleasesLocked(*state_);
    auto& state = *state_;
    const bool candidate=role==ReservationRole::LifecycleDecision && !state.committed;
    if (Validate(state, context, false, false,true,candidate) != ScopeResult::Ok || state.nextReservation == UINT64_MAX ||
        state.retainedReservations >= MaxRetainedOperations) return {};
    auto& grant = state.grants.at(context.grantGeneration);
    auto fresh = context;
    fresh.grantRevision = grant.revision;
    fresh.reservation = state.nextReservation++; fresh.revision = state.domain->Revision();
    try{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        FailAuthorityAllocation(RuntimeScopeTestAccess::AllocationCut::FreshReservation);
#endif
        grant.reservations.emplace(fresh.reservation, fresh);
    }catch(...){return {};}
    ++state.retainedReservations;
    return fresh;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
std::size_t RuntimeScopeTestAccess::Reservations(const RuntimeScope& scope) {
    const std::lock_guard lock(scope.state_->mutex);
    RuntimeScope::DrainCommandReleasesLocked(*scope.state_);return scope.state_->retainedReservations;
}
#endif
OperationContext RuntimeScope::JoinAuthority(const OperationContext& context,const RuntimeAuthoritySource::Participant& participant) {
    const std::lock_guard lock(state_->mutex);
    auto& state = *state_;
    const auto actor=participant.actor_,channel=participant.channel_;
    if(!state.authoritySource || participant.owner_!=state.generation || participant.content_!=state.authoritySource->content_)return {};
    if (!actor || !channel || RuntimeScope::Validate(state, context, false) != ScopeResult::Ok ||
        state.nextReservation == UINT64_MAX || state.retainedReservations >= MaxRetainedOperations) return {};
    auto& grant = state.grants.at(context.grantGeneration);
    if (!grant.allParticipants || !grant.sessionAllowed || grant.reservations.size() >= MaxRetainedOperations ||
        !std::holds_alternative<AgentOrigin>(context.origin) ||
        std::find(state.disconnectedChannels.begin(), state.disconnectedChannels.end(), channel) != state.disconnectedChannels.end()) return {};
    auto joined = context;
    auto& origin = std::get<AgentOrigin>(joined.origin);
    origin.actor = actor; origin.channel = channel;
    // @summary Entity consent belongs to its originally reviewed actor; session membership alone cannot borrow it.
    joined.reservation = state.nextReservation++; joined.revision = state.domain->Revision();
    try{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        FailAuthorityAllocation(RuntimeScopeTestAccess::AllocationCut::JoinedReservation);
#endif
        grant.reservations.emplace(joined.reservation, joined);
    }catch(...){return {};}
    ++state.retainedReservations;
    return joined;
}
ScopeResult RuntimeScope::RevokeAuthoritySession(const RuntimeAuthoritySource::Control& control) {
    auto writer = state_->domain->ReserveWriter();
    if (!writer.Acquired()) return ScopeResult::Busy;
    const std::lock_guard lock(state_->mutex);
    if(!state_->authoritySource || control.owner_!=state_->generation || control.content_!=state_->authoritySource->content_)return ScopeResult::Denied;
    const auto identity=control.subject_;
    bool matched{};
    for (auto pending = state_->challenges.begin(); pending != state_->challenges.end();) {
        const auto* origin = std::get_if<AgentOrigin>(&pending->second.origin);
        if (origin && origin->consentSession == identity) {
            matched = true; pending = state_->challenges.erase(pending);
        } else ++pending;
    }
    for (auto& [id, grant] : state_->grants) {
        (void)id;
        const auto* origin = std::get_if<AgentOrigin>(&grant.context.origin);
        if (!origin || origin->consentSession != identity) continue;
        matched = true; grant.sessionAllowed = false;
        if (grant.revision != UINT64_MAX) ++grant.revision;
        else grant.revoked = true;
    }
    return matched ? ScopeResult::Ok : ScopeResult::Stale;
}
ScopeResult RuntimeScope::DisconnectAuthority(const RuntimeAuthoritySource::Control& control) {
    auto writer = state_->domain->ReserveWriter();
    if (!writer.Acquired()) return ScopeResult::Busy;
    const std::lock_guard lock(state_->mutex);
    if(!state_->authoritySource || control.owner_!=state_->generation || control.content_!=state_->authoritySource->content_)return ScopeResult::Denied;
    const auto channel=control.subject_;
    if (!channel) return ScopeResult::Invalid;
    for (auto pending = state_->challenges.begin(); pending != state_->challenges.end();) {
        const auto* origin = std::get_if<AgentOrigin>(&pending->second.origin);
        if (origin && origin->channel == channel) pending = state_->challenges.erase(pending);
        else ++pending;
    }
    auto& closed = state_->disconnectedChannels;
    if (std::find(closed.begin(), closed.end(), channel) != closed.end()) return ScopeResult::Ok;
    if (closed.size() < MaxRetainedOperations){try{
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        FailAuthorityAllocation(RuntimeScopeTestAccess::AllocationCut::Disconnect);
#endif
        closed.push_back(channel);
    }catch(...){state_->admissionClosed=true;}}
    else state_->admissionClosed = true;
    return ScopeResult::Ok;
}
RuntimeScope::AuthorityPermission RuntimeScope::InspectAuthority(const OperationContext& context) const {
    const std::lock_guard lock(state_->mutex);
    const auto& state = *state_;
    if (RuntimeScope::Validate(state, context, false, true, false) != ScopeResult::Ok)
        return {0, false, state.admissionClosed};
    const auto& grant = state.grants.at(context.grantGeneration);
    return {grant.capabilities, grant.restricted, false};
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
std::size_t RuntimeScopeTestAccess::Registrations(const RuntimeScope& scope) {
    const std::lock_guard lock(scope.state_->mutex);
    return scope.state_->registrations;
}
std::unique_ptr<RuntimeScope> RuntimeScopeTestAccess::Create(std::shared_ptr<RuntimeCommitDomain> domain) {
    auto scope = std::make_unique<RuntimeScope>(nullptr, std::move(domain));
    scope->state_->ownerAvailable = true;
    // @summary Constructor is present only in this independent fixture build, never a production source factory.
    scope->state_->authoritySource=RuntimeAuthoritySource(scope->state_->generation,1);
    return scope;
}
OperationContext RuntimeScopeTestAccess::Issue(RuntimeScope& scope, bool sessionAllowed, bool entityAllowed,
                                              bool mandatoryAllowed, std::uint32_t opcode,
                                              std::chrono::milliseconds lease, std::uint32_t readiness) {
    Review review;
    // @summary Legacy fixture grants are independent optional operations; dedicated reviews test required-owner retirement.
    review.required = 0;
    review.origin = AgentOrigin{scope.Current().generation, 1, 7, 13, 17, 11, 1};
    review.sessionAllowed = sessionAllowed; review.entityAllowed = entityAllowed;
    review.mandatoryAllowed = mandatoryAllowed; review.opcode = opcode; review.readiness = readiness;
    review.expires = std::chrono::steady_clock::now() + lease;
    auto context = Decide(scope, ChallengeDecision(scope, review), review);
    if (!context.owner && lease.count() <= 0) {
        const auto current = scope.Current(); context.owner = current.generation; context.epoch = current.epoch;
    }
    return context;
}
#endif
ScopeResult RuntimeScope::RevokeAuthority(const RuntimeAuthoritySource::Control& control) {
    auto writer = state_->domain->ReserveWriter();
    if (!writer.Acquired()) return ScopeResult::Busy;
    const std::lock_guard lock(state_->mutex);
    if(!state_->authoritySource || control.owner_!=state_->generation || control.content_!=state_->authoritySource->content_)return ScopeResult::Denied;
    const auto found = state_->grants.find(control.subject_);
    if (found != state_->grants.end()) {
        // @summary A fresh reviewed action is required after revocation; an older pending prompt cannot restore admission.
        state_->challenges.clear();
        found->second.revoked = true;
        if (found->second.revision != UINT64_MAX) ++found->second.revision;
        if (found->second.required) state_->admissionClosed = true;
        return ScopeResult::Ok;
    }
    return ScopeResult::Stale;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
void RuntimeScopeTestAccess::FailAllocation(AllocationCut cut) noexcept {authorityAllocationCut=cut;}
RuntimeScopeTestAccess::AuthorityObservation RuntimeScopeTestAccess::ObserveAuthority(const RuntimeScope& scope){
    const std::lock_guard lock(scope.state_->mutex);
    const auto& state=*scope.state_;
    return {state.challenges.size(),state.grants.size(),state.retainedReservations,
        state.nextChallenge,state.nextGrant,state.nextReservation,state.authoritySource.has_value(),state.admissionClosed};
}
RuntimeScopeTestAccess::Challenge RuntimeScopeTestAccess::ChallengeDecision(RuntimeScope& scope,const Review& review){return scope.ChallengeAuthority(review);}
OperationContext RuntimeScopeTestAccess::Decide(RuntimeScope& scope,Challenge challenge,const Review& review){
    if(!scope.state_->authoritySource)return {};
    return scope.DecideAuthority(RuntimeAuthoritySource::Decision(*scope.state_->authoritySource,challenge,review));
}
OperationContext RuntimeScopeTestAccess::Join(RuntimeScope& scope,const OperationContext& context,std::uint64_t actor,std::uint64_t channel){
    if(!scope.state_->authoritySource)return {};
    return scope.JoinAuthority(context,RuntimeAuthoritySource::Participant(*scope.state_->authoritySource,actor,channel));
}
ScopeResult RuntimeScopeTestAccess::RevokeSession(RuntimeScope& scope,std::uint64_t session){
    if(!scope.state_->authoritySource)return ScopeResult::Denied;
    return scope.RevokeAuthoritySession(RuntimeAuthoritySource::Control(*scope.state_->authoritySource,session));
}
ScopeResult RuntimeScopeTestAccess::Disconnect(RuntimeScope& scope,std::uint64_t channel){
    if(!scope.state_->authoritySource)return ScopeResult::Denied;
    return scope.DisconnectAuthority(RuntimeAuthoritySource::Control(*scope.state_->authoritySource,channel));
}
ScopeResult RuntimeScopeTestAccess::Revoke(RuntimeScope& scope,std::uint64_t grant){
    if(!scope.state_->authoritySource)return ScopeResult::Denied;
    return scope.RevokeAuthority(RuntimeAuthoritySource::Control(*scope.state_->authoritySource,grant));
}
RuntimeScopeTestAccess::PermissionView RuntimeScopeTestAccess::Inspect(RuntimeScope& scope,const OperationContext& context){return scope.InspectAuthority(context);}
#endif
}
