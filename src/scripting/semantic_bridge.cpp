#include "scripting/semantic_bridge.hpp"
#include "mods/owned_publication.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace sc13::scripting {
namespace {
/** @summary Base epochs never reuse an identity within the process. */
std::atomic_uint64_t nextEpoch{1};
}
SemanticBridge::SemanticBridge() {
    // @summary Every base epoch shares one process-lifetime pool, including retained old-epoch values.
    static const auto pool=std::make_shared<runtime::copied::Storage>(); values_=pool;
    auto candidate = nextEpoch.load();
    do { if (candidate == UINT64_MAX) throw std::runtime_error("Base epoch exhausted"); }
    while (!nextEpoch.compare_exchange_weak(candidate, candidate + 1));
    epoch_ = candidate;
    PublishObservation();
}
std::shared_ptr<SemanticEndpoint> SemanticBridge::Bind(std::shared_ptr<runtime::RuntimeScope> scope) {
    if (!scope) scope = std::make_shared<runtime::RuntimeScope>();
    runtime::copied::Reservation accounting;
    if(values_->Account(scope->Current().generation,sizeof(SemanticEndpoint)+sizeof(runtime::SemanticTasks)+128,accounting)!=runtime::copied::Status::Ok)
        throw std::runtime_error("Semantic endpoint storage exhausted");
    auto endpoint = std::make_shared<SemanticEndpoint>();
    endpoint->accounting=std::move(accounting);
    endpoint->bridge = shared_from_this();
    endpoint->storageOwner = scope->Current().generation;
    endpoint->scope=scope;
    endpoint->tasks = std::make_shared<runtime::SemanticTasks>(std::move(scope), values_);
    const std::scoped_lock lock(mutex_);
    if (closed_) throw std::runtime_error("Base transport is closed");
    if(!accounting_){std::size_t metadata=methods_.capacity()*sizeof(SemanticMethod);
        for(const auto& method:methods_)metadata+=method.id.capacity()+method.inputSchema.capacity()+method.lane.capacity()+method.context.capacity()+method.capability.capacity()+method.postcondition.capacity();
        if(values_->Account(endpoint->storageOwner,sizeof(SemanticBridge)+metadata+64,accounting_)!=runtime::copied::Status::Ok)throw std::runtime_error("Base transport storage exhausted");}
    for (auto& slot:endpoints_) if (slot.expired()) {slot=endpoint;return endpoint;}
    throw std::runtime_error("Base endpoint capacity exhausted");
}
runtime::ScopeResult SemanticBridge::Request(const std::shared_ptr<SemanticEndpoint>& endpoint,
    std::string_view method, const runtime::copied::Envelope& value, runtime::TaskHandle& output,SemanticContextLease& context,
    const SemanticContextLease* expected,const SemanticEndpointBinding* binding) {
    using runtime::ScopeResult;
    output = {}; context = {};
    const std::scoped_lock lock(mutex_);
    if (closed_ || !readyGeneration_) return ScopeResult::NotReady;
    if (!endpoint || endpoint->bridge.get() != this || endpoint->closed || !endpoint->generation) return ScopeResult::Stale;
    std::size_t index{};
    while (index < methods_.size() && methods_[index].id != method) ++index;
    if (index == methods_.size()) return ScopeResult::NotReady;
    if (!value || value.Schema() != methods_[index].inputSchema) return ScopeResult::Invalid;
    if(binding){const auto exact=ValidateBindingLocked(endpoint,*binding);if(exact!=ScopeResult::Ok)return exact;}
    SemanticContextLease captured;
    const auto resolved=ResolveContext(*endpoint,methods_[index],value,captured);
    if (resolved!=ScopeResult::Ok) return resolved;
    if(expected){
        // @summary Preserve the original invocation's facts atomically with native admission, rather than silently rebinding newer observations.
        const auto current=ValidateContextLocked(*endpoint,*expected);
        if(current!=ScopeResult::Ok)return current;
        if(captured.baseEpoch!=expected->baseEpoch||captured.vmGeneration!=expected->vmGeneration||
            captured.scopeGeneration!=expected->scopeGeneration||captured.runtimeEpoch!=expected->runtimeEpoch||
            (captured.mask&expected->mask)!=expected->mask)return ScopeResult::Stale;
        for(std::size_t i=0;i<captured.facts.size();++i)
            if((expected->mask&(1U<<i))&&!(captured.facts[i]==expected->facts[i]))return ScopeResult::Stale;
    }
    const auto size = value.Encode().size();
    if (count_ == queue_.size() || size > 1024U * 1024U - bytes_) return ScopeResult::Busy;
    if(nextOperation_==UINT64_MAX)return ScopeResult::Busy;
    captured.operation=nextOperation_++;
    const auto admitted = endpoint->tasks->Admit(endpoint->context, value, methods_[index].maxResultBytes, output);
    if (admitted != ScopeResult::Ok) return admitted;
    queue_[(first_ + count_) % queue_.size()] = {endpoint, output, value, endpoint->generation, epoch_, index,captured};
    ++count_; bytes_ += size;
    context=captured;
    return ScopeResult::Ok;
}
runtime::ScopeResult SemanticBridge::CaptureBinding(const std::shared_ptr<SemanticEndpoint>& endpoint,SemanticEndpointBinding& output) const {
    output={};
    SemanticEndpointBinding captured;
    {
        const std::scoped_lock lock(mutex_);
        if(closed_)return runtime::ScopeResult::Stale;
        if(!readyGeneration_)return runtime::ScopeResult::NotReady;
        if(!endpoint||endpoint->closed||endpoint->bridge.get()!=this||!endpoint->scope||!endpoint->tasks||!endpoint->generation||
            !std::any_of(endpoints_.begin(),endpoints_.end(),[&](const auto& entry){return !entry.owner_before(endpoint)&&!endpoint.owner_before(entry);}))
            return runtime::ScopeResult::Stale;
        captured={endpoint,endpoint->scope,endpoint->tasks,endpoint->context,endpoint->generation,endpoint->storageOwner,epoch_,readyGeneration_};
    }
    output=std::move(captured);return runtime::ScopeResult::Ok;
}
runtime::ScopeResult SemanticBridge::ValidateBindingLocked(const std::shared_ptr<SemanticEndpoint>& endpoint,const SemanticEndpointBinding& expected) const {
    if(closed_||!endpoint||endpoint->closed)return runtime::ScopeResult::Stale;
    if(!readyGeneration_)return runtime::ScopeResult::NotReady;
    if(expected.endpoint!=endpoint||endpoint->bridge.get()!=this||expected.scope!=endpoint->scope||expected.tasks!=endpoint->tasks||
        expected.authority!=endpoint->context||expected.generation!=endpoint->generation||expected.storageOwner!=endpoint->storageOwner||
        expected.baseEpoch!=epoch_||expected.baseGeneration!=readyGeneration_)return runtime::ScopeResult::Stale;
    return runtime::ScopeResult::Ok;
}
bool SemanticBridge::Take(SemanticInvocation& output) {
    const std::scoped_lock lock(mutex_);
    if (closed_ || !readyGeneration_ || !count_) return false;
    output = std::move(queue_[first_]); queue_[first_] = {};
    first_ = (first_ + 1) % queue_.size(); --count_; bytes_ -= output.value.Encode().size();
    return true;
}
runtime::ScopeResult SemanticBridge::Dispatch(const SemanticInvocation& invocation, std::string_view method,
    const runtime::copied::Envelope& value) {
    using runtime::ScopeResult;
    {
        const std::scoped_lock lock(mutex_);
        if (closed_ || !readyGeneration_ || invocation.epoch != epoch_) return ScopeResult::Stale;
    }
    const auto& origin = invocation.origin;
    if (!origin || origin->closed || origin->bridge.get() != this || !origin->generation ||
        invocation.generation != origin->generation || invocation.method >= methods_.size()) return ScopeResult::Stale;
    const auto& descriptor = methods_[invocation.method];
    if (descriptor.registryPublication) {
        origin->tasks->FinishUncommitted(invocation.task, ScopeResult::Denied);
        return ScopeResult::Denied;
    }
    const auto liveContext=ValidateContext(*origin,invocation.context);
    if (liveContext!=ScopeResult::Ok) {origin->tasks->FinishUncommitted(invocation.task,liveContext);return liveContext;}
    if (descriptor.id != method || value.Encode() != invocation.value.Encode() || value.Schema() != descriptor.inputSchema) {
        origin->tasks->FinishUncommitted(invocation.task, ScopeResult::Invalid);return ScopeResult::Invalid;
    }
    const auto begin = origin->tasks->Begin(invocation.task);
    if (begin != ScopeResult::Ok) {origin->tasks->FinishUncommitted(invocation.task,begin);return begin;}
    const auto result=origin->tasks->Commit(invocation.task, descriptor.execute,[this,origin,context=invocation.context]{return ContextFence(*origin,context);});
    if(result!=ScopeResult::Ok&&result!=ScopeResult::Indeterminate)origin->tasks->FinishUncommitted(invocation.task,result);
    return result;
}
runtime::ScopeResult SemanticBridge::DispatchPrepared(const SemanticInvocation& invocation,
    mods::PatchRegistry& registry, mods::OwnedPublication& plan) {
    using runtime::ScopeResult;
    const auto& origin = invocation.origin;
    if (!origin || origin->bridge.get() != this || !origin->scope || !origin->tasks ||
        origin->closed || !origin->generation || invocation.generation != origin->generation)
        return ScopeResult::Stale;
    if (origin->tasks->scope_.get() != origin->scope.get()) {
        // @summary Consume the original issuer-bound task without touching the mismatched scope or disclosing its data.
        origin->tasks->FinishUncommitted(invocation.task, ScopeResult::Denied);
        return ScopeResult::Denied;
    }
    const auto begin = origin->tasks->Begin(invocation.task);
    if (begin != ScopeResult::Ok) {
        origin->tasks->FinishUncommitted(invocation.task, begin);
        return begin;
    }
    ScopeResult result = ScopeResult::Busy;
    {
        auto writer = origin->scope->ReserveWriter();
        if (writer.Acquired()) {
            const auto owned = plan.Validate(invocation);
            if (owned != ScopeResult::Ok) result=owned;
            else {
            auto publication = registry.TryReservePublication();
            if (publication.owns_lock()) {
                // @summary The same mutex serializes ObserveContext/Close; no observation-only check can race the typed publication.
                const std::unique_lock contextLock(mutex_, std::try_to_lock);
                if (contextLock.owns_lock()) {
                    result = ValidateContextLocked(*origin, invocation.context);
                    if (result == ScopeResult::Ok && (invocation.epoch != epoch_ || invocation.method >= methods_.size()))
                        result = ScopeResult::Stale;
                    if (result == ScopeResult::Ok && (!invocation.value ||
                        invocation.value.Schema() != methods_[invocation.method].inputSchema)) result = ScopeResult::Invalid;
                    if (result == ScopeResult::Ok && !methods_[invocation.method].registryPublication) result = ScopeResult::Denied;
                    if (result == ScopeResult::Ok) {
                        result = origin->tasks->CommitPrepared(invocation.task, writer,
                            publication, plan.plan_, invocation.context.scopeGeneration, invocation.context.runtimeEpoch, invocation.value);
                        // @summary Swap preallocated owner baseline only after the exact publication; retired storage remains in the plan beyond all fences.
                        if(result==ScopeResult::Ok) plan.origin_->scripted_.swap(plan.next_);
                    }
                }
            }
            }
        }
    }
    if (result != ScopeResult::Ok && result != ScopeResult::Indeterminate)
        origin->tasks->FinishUncommitted(invocation.task, result);
    return result;
}
void SemanticBridge::Close() noexcept {
    std::array<SemanticInvocation, 128> retired;
    std::array<std::weak_ptr<SemanticEndpoint>,128> endpoints;
    {
        const std::scoped_lock lock(mutex_);
        if (closed_) return;
        retiredStamp_.store(true);closed_ = true; readyGeneration_ = 0; retired.swap(queue_); endpoints=endpoints_; count_ = bytes_ = first_ = 0;
        PublishObservation();
    }
    // @summary Existing scope validation is the commit boundary; stopping does not invent a second permission store.
    for (auto& weak:endpoints) if (const auto endpoint=weak.lock()) endpoint->scope->RequestStop(runtime::StopReason::OwnerLost);
    for (auto& request : retired) if (request.origin) {
        try { request.origin->tasks->FinishUncommitted(request.task, runtime::ScopeResult::Stale); }
        catch (...) { /* @summary Stop remains sticky even if an observer's owned task was already released. */ }
    }
}
void SemanticBridge::Publish(std::uint64_t generation) noexcept {
    const std::scoped_lock lock(mutex_);
    if (!closed_ && generation && !readyGeneration_) { readyGeneration_ = generation; PublishObservation(); }
}
std::uint64_t SemanticBridge::ReadyGeneration() const noexcept {
    const std::scoped_lock lock(mutex_); return closed_ ? 0 : readyGeneration_;
}
void SemanticBridge::PublishObservation() noexcept {
    if (publicationSequence_ == UINT64_MAX) {
        retiredStamp_.store(true);
        closed_ = true;
        readyGeneration_ = 0;
        return;
    }
    const auto sequence = ++publicationSequence_;
    publications_[(sequence - 1) % publications_.size()] = {
        sequence,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()),
        epoch_, readyGeneration_, contexts_, singlePlayerKnown_
    };
}
SemanticPublication SemanticBridge::CurrentPublication() const noexcept {
    const std::scoped_lock lock(mutex_);
    return publicationSequence_ ? publications_[(publicationSequence_ - 1) % publications_.size()] : SemanticPublication{};
}
runtime::ScopeResult SemanticBridge::NextPublication(std::uint64_t& cursor, SemanticPublication& output) const noexcept {
    output = {};
    const std::scoped_lock lock(mutex_);
    if (cursor > publicationSequence_ || publicationSequence_ - cursor > publications_.size())
        return runtime::ScopeResult::Conflict;
    if (cursor == publicationSequence_) return runtime::ScopeResult::NotReady;
    const auto& next = publications_[cursor % publications_.size()];
    if (next.sequence != cursor + 1) return runtime::ScopeResult::Conflict;
    output = next;
    cursor = next.sequence;
    return runtime::ScopeResult::Ok;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
runtime::ScopeResult SemanticBridgeTestAccess::CaptureBinding(const SemanticBridge& bridge,const std::shared_ptr<SemanticEndpoint>& endpoint,SemanticEndpointBinding& output){
    return bridge.CaptureBinding(endpoint,output);
}
std::unique_lock<std::mutex> SemanticBridgeTestAccess::ReserveObservation(SemanticBridge& bridge) {
    return std::unique_lock<std::mutex>(bridge.mutex_, std::try_to_lock);
}
void SemanticBridgeTestAccess::Configure(SemanticBridge& bridge, std::vector<SemanticMethod> methods) {
    if (bridge.readyGeneration_ || bridge.closed_ || methods.size() > 64) throw std::runtime_error("Invalid fixture catalog");
    for (std::size_t i = 0; i < methods.size(); ++i) {
        const auto& method = methods[i];
        if (method.id.empty() || method.id.size() > 128 || method.inputSchema.empty() || method.inputSchema.size() > 128 ||
            method.lane.empty() || method.context.empty() || method.capability.empty() || method.postcondition.empty() ||
            !method.maxResultBytes || method.maxResultBytes > runtime::copied::Storage::EnvelopeLimit || !method.execute)
            throw std::runtime_error("Invalid fixture descriptor");
        for (std::size_t j = 0; j < i; ++j) if (methods[j].id == method.id) throw std::runtime_error("Duplicate fixture descriptor");
    }
    bridge.methods_ = std::move(methods);
}
void SemanticBridgeTestAccess::Issue(SemanticEndpoint& endpoint, runtime::OperationContext context) {
    if (endpoint.generation || endpoint.closed) throw std::runtime_error("Endpoint already bound");
    endpoint.context = context;
}
/** @summary Refresh only an existing original owner's binding through the authentic tracked reservation evaluator.
 * @param origin Constructor-bound actual native incarnation. @param output Empty typed lifetime owner.
 * @return Exact bounded admission; failed local ownership is released after all guards. */
runtime::ScopeResult SemanticBridge::RefreshFixtureReservation(
    const std::shared_ptr<mods::PublicationOrigin>& origin, mods::LifecycleDecision& output) {
    using runtime::ScopeResult;
    if (!origin || output.Context().owner) return ScopeResult::Invalid;
    // @summary Current reads constructor-complete identities; mutable admission is checked under D/B/Q below.
    if (!origin->Current()) return ScopeResult::Stale;
    const auto endpoint=origin->endpoint_;
    const auto scope=origin->scope_;
    if (scope->stateDomain().WriterActiveOnCurrentThread()) return ScopeResult::Busy;
    mods::LifecycleDecision fresh;
    runtime::OperationContext source;
    std::uint64_t ready{};
    const auto matches=[&] {
        return origin->Current() && origin->endpoint_==endpoint && origin->scope_==scope &&
            endpoint->bridge.get()==this && endpoint->scope==scope && !endpoint->closed.load() &&
            endpoint->generation && endpoint->generation==origin->generation_ &&
            endpoint->storageOwner && endpoint->context.owner==endpoint->storageOwner &&
            !closed_ && readyGeneration_ &&
            std::any_of(endpoints_.begin(),endpoints_.end(),[&](const auto& entry) {
                return !entry.owner_before(endpoint) && !endpoint.owner_before(entry);
            });
    };
    {
        std::unique_lock observation(mutex_,std::try_to_lock);
        if (!observation.owns_lock()) return ScopeResult::Busy;
        if (!matches()) return ScopeResult::Stale;
        source=endpoint->context;ready=readyGeneration_;
    }
    // @summary Existing allocation/reservation callbacks run outside every publication/bridge/scope guard.
    const auto reserved=mods::LifecycleDecisionTestAccess::FreshDecision(scope,source,fresh);
    if (reserved!=ScopeResult::Ok) return reserved;
    {
        auto writer=scope->ReserveWriter();
        if (!writer.Acquired()) return ScopeResult::Busy;
        std::unique_lock observation(mutex_,std::try_to_lock);
        if (!observation.owns_lock()) return ScopeResult::Busy;
        if (!matches() || readyGeneration_!=ready) return ScopeResult::Stale;
        if (endpoint->context!=source) return ScopeResult::Conflict;
        runtime::RuntimeScope::LifecycleGuard authority(*scope);
        if (!authority.Acquired()) return ScopeResult::Busy;
        if (!authority.InDomain(scope->stateDomain())) return ScopeResult::Stale;
        const auto context=fresh.Context();
        const auto admitted=authority.Check(context,false);
        if (admitted!=ScopeResult::Ok) return admitted;
        const auto facts=authority.Snapshot();
        if (facts.generation!=endpoint->storageOwner || context.owner!=facts.generation ||
            context.epoch!=facts.epoch || !context.reservation || context.reservation==source.reservation)
            return ScopeResult::Stale;
        auto provenance=context;
        // @summary Only compare immutable provenance; this normalized temporary is never installed or authorized.
        provenance.reservation=source.reservation;provenance.revision=source.revision;
        provenance.grantRevision=source.grantRevision;
        if (provenance!=source || endpoint->closed.load()) return ScopeResult::Stale;
        endpoint->context=context;
        output=std::move(fresh);
    }
    // @summary A failed local lease is destroyed only after Q/B/D; success transfers that same owner to the caller.
    return ScopeResult::Ok;
}
/** @summary Fixture-only forwarding never creates a grant or accepts a replacement context.
 * @param bridge Exact original transport. @param origin Actual native incarnation. @param output Empty typed owner.
 * @return Original guarded refresh result. */
runtime::ScopeResult SemanticBridgeTestAccess::RefreshReservation(SemanticBridge& bridge,
    const std::shared_ptr<mods::PublicationOrigin>& origin, mods::LifecycleDecision& output) {
    return bridge.RefreshFixtureReservation(origin,output);
}

void SemanticBridgeTestAccess::ObserveContext(SemanticBridge& bridge,SemanticContextKind kind,std::uint64_t slot,std::uint32_t generation,bool singlePlayerKnown) {
    const std::scoped_lock lock(bridge.mutex_); auto& fact=bridge.contexts_[static_cast<std::size_t>(kind)];
    if (fact.epoch==UINT64_MAX) throw std::runtime_error("Context epoch exhausted");
    fact={slot,fact.epoch+1,generation};bridge.singlePlayerKnown_=singlePlayerKnown;
    bridge.contextStamps_[static_cast<std::size_t>(kind)].store(fact.epoch);bridge.singlePlayerStamp_.store(singlePlayerKnown);
    bridge.PublishObservation();
}
#endif
runtime::ScopeResult SemanticBridge::ResolveContext(const SemanticEndpoint& endpoint,const SemanticMethod& method,
    const runtime::copied::Envelope& value,SemanticContextLease& output) const {
    using runtime::ScopeResult;
    output={}; output.baseEpoch=epoch_;output.vmGeneration=endpoint.generation;
    const auto scope=endpoint.scope->Current();output.scopeGeneration=scope.generation;output.runtimeEpoch=scope.epoch;
    // @summary This unit has no qualified game observer; fixture facts cannot turn into a production mutation provider.
    if (!method.context.empty()&&!singlePlayerKnown_) return ScopeResult::NotReady;
    const auto kindIndex=[](std::string_view kind)->int {
        if(kind=="runtime")return 0;if(kind=="region")return 1;if(kind=="city")return 2;if(kind=="entity")return 3;return -1;
    };
    const int required=kindIndex(method.context);
    if(required<0&&!method.context.empty())return ScopeResult::Invalid;
    output.mask=required<0?0:static_cast<std::uint8_t>(1U<<required);
    for(std::uint32_t i=0;const auto* node=value.At(i);++i)if(node->tag==runtime::copied::Tag::Ref){
        const int index=kindIndex(value.Text(*node));if(index<0)return ScopeResult::Invalid;
        const auto& fact=contexts_[static_cast<std::size_t>(index)];
        if(!fact.epoch||!fact.slot||fact.slot!=node->bits||fact.generation!=node->generation)return ScopeResult::Stale;
        output.mask|=static_cast<std::uint8_t>(1U<<index);
    }
    for(std::size_t i=0;i<contexts_.size();++i)if(output.mask&(1U<<i)){
        if(!contexts_[i].epoch||!contexts_[i].slot||!contexts_[i].generation)return ScopeResult::NotReady;
        output.facts[i]=contexts_[i];
    }
    return ScopeResult::Ok;
}
runtime::ScopeResult SemanticBridge::ValidateContext(const SemanticEndpoint& endpoint,const SemanticContextLease& lease) const {
    const std::scoped_lock lock(mutex_);
    return ValidateContextLocked(endpoint, lease);
}
runtime::ScopeResult SemanticBridge::ValidateContextLocked(const SemanticEndpoint& endpoint,const SemanticContextLease& lease) const noexcept {
    if(closed_||!readyGeneration_||endpoint.closed||lease.baseEpoch!=epoch_||lease.vmGeneration!=endpoint.generation||!lease.mask)return runtime::ScopeResult::Stale;
    if(!singlePlayerKnown_)return runtime::ScopeResult::NotReady;
    for(std::size_t i=0;i<contexts_.size();++i)if((lease.mask&(1U<<i))&&!(lease.facts[i]==contexts_[i]))return runtime::ScopeResult::Stale;
    return runtime::ScopeResult::Ok;
}
runtime::ScopeResult SemanticBridge::ContextFence(const SemanticEndpoint& endpoint,const SemanticContextLease& lease) const noexcept {
    if(retiredStamp_.load()||endpoint.closed||lease.baseEpoch!=epoch_||lease.vmGeneration!=endpoint.generation||!lease.mask)return runtime::ScopeResult::Stale;
    if(!singlePlayerStamp_.load())return runtime::ScopeResult::NotReady;
    for(std::size_t i=0;i<contextStamps_.size();++i)if((lease.mask&(1U<<i))&&lease.facts[i].epoch!=contextStamps_[i].load())return runtime::ScopeResult::Stale;
    return runtime::ScopeResult::Ok;
}
runtime::ScopeResult SemanticBridge::BindReferences(const SemanticEndpoint& endpoint,const runtime::copied::Envelope& value,SemanticContextLease& output) const {
    const std::scoped_lock lock(mutex_);return ResolveContext(endpoint,SemanticMethod{},value,output);
}
}
