#include "scripting/semantic_bridge.hpp"
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
    std::string_view method, const runtime::copied::Envelope& value, runtime::TaskHandle& output,SemanticContextLease& context) {
    using runtime::ScopeResult;
    output = {};
    const std::scoped_lock lock(mutex_);
    if (closed_ || !readyGeneration_) return ScopeResult::NotReady;
    if (!endpoint || endpoint->bridge.get() != this || endpoint->closed || !endpoint->generation) return ScopeResult::Stale;
    std::size_t index{};
    while (index < methods_.size() && methods_[index].id != method) ++index;
    if (index == methods_.size()) return ScopeResult::NotReady;
    if (!value || value.Schema() != methods_[index].inputSchema) return ScopeResult::Invalid;
    const auto resolved=ResolveContext(*endpoint,methods_[index],value,context);
    if (resolved!=ScopeResult::Ok) return resolved;
    const auto size = value.Encode().size();
    if (count_ == queue_.size() || size > 1024U * 1024U - bytes_) return ScopeResult::Busy;
    if(nextOperation_==UINT64_MAX)return ScopeResult::Busy;
    context.operation=nextOperation_++;
    const auto admitted = endpoint->tasks->Admit(endpoint->context, value, methods_[index].maxResultBytes, output);
    if (admitted != ScopeResult::Ok) return admitted;
    queue_[(first_ + count_) % queue_.size()] = {endpoint, output, value, endpoint->generation, epoch_, index,context};
    ++count_; bytes_ += size;
    return ScopeResult::Ok;
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
