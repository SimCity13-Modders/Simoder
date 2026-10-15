#include "runtime/runtime_scope.hpp"
#include "platform/runtime_owner.hpp"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

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
/** @summary Process-local generation allocator; zero means exhausted and always denies. */
std::atomic<std::uint64_t> nextGeneration{1};
/** @summary Thread-local linked reservations prevent undefined recursive std::mutex try-locking. */
struct ActiveCommit final {
    const void* domain;
    ActiveCommit* previous;
};
thread_local ActiveCommit* activeCommit{};
/** @summary Stack-only writer identity guard shared by byte and copied-value commits. */
struct ActiveReservation final {
    ActiveCommit entry;
    /** @summary Registers the already locked writer domain. @param domain Native domain identity. */
    explicit ActiveReservation(const void* domain) : entry{domain, activeCommit} { activeCommit = &entry; }
    /** @summary Restores the enclosing writer identity. */
    ~ActiveReservation() { activeCommit = entry.previous; }
};
/** @summary Allocates without wraparound reuse. @return Unique nonzero generation or zero. */
std::uint64_t AllocateGeneration() noexcept {
    auto value = nextGeneration.load();
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (nextGeneration.compare_exchange_weak(value, value + 1)) return value;
    }
    return 0;
}
}

/** @summary Small owned authority/queue state retained independently by executing callbacks. */
struct RuntimeScope::State final {
    /** @summary Internal fixture grant records; production store remains unavailable and empty. */
    struct Grant final {
        OperationContext context;
        bool sessionAllowed{};
        bool entityAllowed{};
        bool mandatoryAllowed{};
        bool revoked{};
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
        copied::Envelope valueInput, valueResult;
        copied::Reservation resultArena, resultWire, metadata;
        bool valueOperation{};
        bool committed{};
    };
    mutable std::mutex mutex;
    std::shared_ptr<RuntimeCommitDomain> domain;
    std::condition_variable drained;
    std::shared_ptr<platform::RuntimeOwner> owner;
    std::vector<std::shared_ptr<void>> resources;
    std::map<std::uint64_t, Grant> grants;
    std::map<std::uint64_t, Operation> operations;
    std::uint64_t generation{AllocateGeneration()};
    std::uint64_t epoch{1};
    std::uint64_t nextOperation{1};
    std::uint64_t nextSession{1};
    std::size_t running{};
    std::size_t registrations{};
    std::uint32_t readiness{};
    bool ownerAvailable{};
    bool authorityAvailable{};
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
            --state->domain->pending_;
            operation.phase = State::Operation::Phase::Cancelled;
            operation.outcome = ScopeResult::Cancelled;
            operation.input.clear();
        }
    }
    state->drained.notify_all();
}
bool RuntimeScope::DrainFor(std::chrono::milliseconds timeout) {
    const auto state = state_;
    std::vector<std::shared_ptr<void>> released;
    {
        std::unique_lock lock(state->mutex);
        if (!state->stopping || timeout.count() < 0) return false;
        if (!state->drained.wait_for(lock, timeout, [&] { return state->running == 0; })) return false;
        state->safelyDrained = true;
        released.swap(state->resources);
    }
    // @summary Resource destructors may reenter; release them after dropping the state lock.
    released.clear();
    return true;
}
bool RuntimeScope::RetainResource(std::shared_ptr<void> resource) {
    if (!resource) return false;
    const std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->domain->IsStopping() || state_->resources.size() + state_->registrations >= MaxRetainedResources) return false;
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
    return {state_->generation, state_->epoch, state_->domain->revision_.load(), state_->running, state_->operations.size(),
            state_->committed, state_->stopping, state_->safelyDrained};
}

ScopeResult RuntimeScope::Validate(const State& state, const OperationContext& context, bool checkRevision) {
    if (!state.generation || context.owner != state.generation || context.epoch != state.epoch) return ScopeResult::Stale;
    if (state.stopping || state.domain->IsStopping() || !state.committed || !state.ownerAvailable) return ScopeResult::NotReady;
    if (!state.authorityAvailable) return ScopeResult::Denied;
    if (!context.principal || !context.session || !context.entity || !context.opcode ||
        !context.readiness || (context.readiness & ~(RuntimeReady | MenuReady | CityReady)) != 0 ||
        std::chrono::steady_clock::now() >= context.deadline) return ScopeResult::Denied;
    const auto grant = state.grants.find(context.session);
    if (grant == state.grants.end() || grant->second.revoked) return ScopeResult::Denied;
    const auto& authorized = grant->second.context;
    if (context.owner != authorized.owner || context.epoch != authorized.epoch ||
        context.grantGeneration != authorized.grantGeneration || context.principal != authorized.principal ||
        context.session != authorized.session || context.entity != authorized.entity ||
        context.revision != authorized.revision ||
        context.opcode != authorized.opcode || context.readiness != authorized.readiness ||
        context.deadline != authorized.deadline || !grant->second.mandatoryAllowed ||
        !(grant->second.sessionAllowed || grant->second.entityAllowed)) return ScopeResult::Denied;
    if ((state.readiness & context.readiness) != context.readiness) return ScopeResult::NotReady;
    if (checkRevision && context.revision != state.domain->revision_.load()) return ScopeResult::Conflict;
    return ScopeResult::Ok;
}

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
    for (const auto* active = activeCommit; active; active = active->previous)
        if (active->domain == state->domain.get()) return ScopeResult::Busy;
    std::unique_lock lane(state->domain->lane_, std::try_to_lock);
    if (!lane.owns_lock()) return ScopeResult::Busy;
    ActiveReservation reservation(state->domain.get());
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
    --state_->domain->pending_;
    found->second.phase = State::Operation::Phase::Cancelled;
    found->second.outcome = ScopeResult::Cancelled;
    found->second.input.clear();
    return ScopeResult::Ok;
}
ScopeResult RuntimeScope::Release(std::uint64_t owner, ScopeHandle operation) {
    const std::lock_guard lock(state_->mutex);
    if (owner != state_->generation || operation.owner != owner) return ScopeResult::Denied;
    const auto found = state_->operations.find(operation.serial);
    if (found == state_->operations.end()) return ScopeResult::Stale;
    if (found->second.phase == State::Operation::Phase::Running) return ScopeResult::Busy;
    if (found->second.phase == State::Operation::Phase::Queued) --state_->domain->pending_;
    if (found->second.phase == State::Operation::Phase::Preparing) --state_->domain->pending_;
    state_->operations.erase(found);
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
    try { state_->operations.emplace(serial, std::move(operation)); }
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
    retained.phase = State::Operation::Phase::Preparing;
    return ScopeResult::Ok;
}
ScopeResult RuntimeScope::CommitValue(ScopeHandle operation, const ValueCallback& callback,
                                    const std::function<ScopeResult()>& precommit) {
    if (!callback) return ScopeResult::Invalid;
    const auto state = state_;
    for (const auto* active = activeCommit; active; active = active->previous)
        if (active->domain == state->domain.get()) return ScopeResult::Busy;
    std::unique_lock lane(state->domain->lane_, std::try_to_lock);
    if (!lane.owns_lock()) return ScopeResult::Busy;
    ActiveReservation guard(state->domain.get());
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
        retained->phase = State::Operation::Phase::Running;
        retained->committed = true;
        ++state->running; ++state->domain->revision_;
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
        retained->valueResult = std::move(result);
        retained->outcome = outcome;
        retained->phase = State::Operation::Phase::Completed;
        retained->valueInput = {}; retained->resultArena = {}; retained->resultWire = {};
        --state->running; --state->domain->pending_;
        state->drained.notify_all();
    }
    return outcome;
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
    retained.outcome = reason;
    retained.phase = reason == ScopeResult::Cancelled ? State::Operation::Phase::Cancelled : State::Operation::Phase::Completed;
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
std::size_t RuntimeScopeTestAccess::Registrations(const RuntimeScope& scope) {
    const std::lock_guard lock(scope.state_->mutex);
    return scope.state_->registrations;
}
std::unique_ptr<RuntimeScope> RuntimeScopeTestAccess::Create(std::shared_ptr<RuntimeCommitDomain> domain) {
    auto scope = std::make_unique<RuntimeScope>(nullptr, std::move(domain));
    scope->state_->ownerAvailable = true;
    scope->state_->authorityAvailable = true;
    return scope;
}
OperationContext RuntimeScopeTestAccess::Issue(RuntimeScope& scope, bool sessionAllowed, bool entityAllowed,
                                              bool mandatoryAllowed, std::uint32_t opcode,
                                              std::chrono::milliseconds lease, std::uint32_t readiness) {
    const std::lock_guard lock(scope.state_->mutex);
    auto& state = *scope.state_;
    if (!state.authorityAvailable || state.grants.size() >= MaxRetainedOperations || state.stopping || state.domain->IsStopping() ||
        state.nextSession == std::numeric_limits<std::uint64_t>::max()) return {};
    const auto session = state.nextSession++;
    OperationContext context{state.generation, state.epoch, session, 7, session, 11, state.domain->revision_.load(), opcode,
                             readiness, std::chrono::steady_clock::now() + lease};
    state.grants.emplace(session, RuntimeScope::State::Grant{context, sessionAllowed, entityAllowed, mandatoryAllowed, false});
    return context;
}
void RuntimeScopeTestAccess::Revoke(RuntimeScope& scope, std::uint64_t session) {
    const std::lock_guard lock(scope.state_->mutex);
    const auto found = scope.state_->grants.find(session);
    if (found != scope.state_->grants.end()) found->second.revoked = true;
}
#endif
}
