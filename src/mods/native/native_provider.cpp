#include "mods/native/native_provider.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace sc13::mods::native {
namespace {
/** @summary Fixed invocation and task capacity for each retained provider. */
constexpr std::uint32_t Slots = 128;
/** @summary Each provider receives disjoint public slots; exhaustion never wraps. */
std::atomic_uint32_t nextBlock{1};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Fixture-thread scheduling cut only; absent from production and never consulted for identity or authority. */
thread_local std::function<void()> afterOriginalPoll;
/** @summary Fixture-only pending reservation cut, absent from production and original admission decisions. */
thread_local std::function<void()> afterTaskReserve;
#endif
/** @summary Compare opaque identities without reinterpretation. @param a First identity. @param b Second identity. @return Exact equality. */
bool Same(SdHandle a, SdHandle b) noexcept { return a.slot == b.slot && a.generation == b.generation; }
/** @summary Validate identity shape only, never establish authority. @param value Opaque identity. @return Nonzero components. */
bool Present(SdHandle value) noexcept { return value.slot && value.generation; }
/** @summary Reserved public width covers invocation, task and informational execution-context kinds. */
constexpr std::uint32_t BlockWidth = 3*Slots;
/** @summary Allocate disjoint typed ranges before exposing any reference. @return First slot, or throw on exhaustion. */
std::uint32_t Block() {
    auto value=nextBlock.load();
    do { if(value>UINT32_MAX-BlockWidth) throw std::runtime_error("Native provider identity exhausted"); }
    while(!nextBlock.compare_exchange_weak(value,value+BlockWidth));
    return value;
}
/** @summary Clear supplied valid native storage; not a sandbox for hostile native pointers.
 * @param output Caller buffer. @param capacity Bounded size. @param required Cleared required bytes. @return Valid buffer shape. */
bool Buffer(std::uint8_t* output,std::uint32_t capacity,std::uint32_t* required) noexcept {
    if(required)*required=0;
    if(capacity>runtime::copied::Storage::EnvelopeLimit)return false;
    if(output&&capacity)std::memset(output,0,capacity);
    return required && (!capacity||output);
}
/** @summary Preserve only negotiation header on rejected observation. @param output Caller structure. @return Negotiation status. */
template<class T> SdResult Clear(T* output) noexcept {
    if(!output)return SD_INVALID;
    const auto header=output->header;*output={};output->header=header;
    return header.size==sizeof(T)&&header.version==1?SD_OK:SD_VERSION;
}
}
/** @summary Fixed bounded public references retain original endpoints, task issuers and copied pool charges. */
struct NativeProvider::State final {
    /** @summary Retain the exact typed outer lifetime before any protected operation.
     * @param participant Genuine original VM or State. */
    explicit State(ProviderParticipant participant):participant(std::move(participant)){}
    /** @summary Retains a single immutable route, independent of caller-selected IDs. */
    struct Invocation final { std::size_t route{}; };
    /** @summary Retains the original task issuer/context and its original invocation through cleanup. */
    struct Task final {
        std::shared_ptr<Invocation> invocation;
        runtime::TaskHandle task;
        scripting::SemanticContextLease context;
        std::atomic_bool pending{true};
        std::atomic_bool releasing{};
    };
    /** @summary A retired generation is never reused after its uint32 capacity is exhausted. */
    template<class T> struct Slot final { std::uint32_t generation{1};std::shared_ptr<T> value; };
    SdHandle owner{},session{};
    std::shared_ptr<scripting::BaseHost> base;
    std::shared_ptr<scripting::SemanticEndpoint> endpoint;
    ProviderParticipant participant;
    std::shared_ptr<runtime::RuntimeLifecycle> lifecycle;
    scripting::SemanticEndpointBinding binding;
    std::vector<ProviderRoute> routes;
    runtime::copied::Reservation accounting;
    std::mutex mutex;
    std::array<Slot<Invocation>,Slots> invocations;
    std::array<Slot<Task>,Slots> tasks;
    std::uint32_t block{};
    std::atomic_bool closed{};
    /** @summary Resolve under bookkeeping only; lease survives lock release.
     * @param table Fixed kind table. @param handle Public identity. @param offset Kind range. @return Original entry or empty. */
    template<class T> std::shared_ptr<T> Find(std::array<Slot<T>,Slots>& table,SdHandle handle,std::uint32_t offset) {
        const std::lock_guard lock(mutex);
        if(handle.slot<block+offset||handle.slot>=block+offset+Slots)return {};
        const auto& slot=table[handle.slot-block-offset];
        return slot.generation==handle.generation?slot.value:nullptr;
    }
    /** @summary Reserve the shared live invocation/task ceiling before publishing any kind, without entering scope or VM.
     * @param table Fixed kind table. @param value Prepared lease. @param offset Kind range. @return Issued identity or zero at capacity. */
    template<class T> SdHandle Add(std::array<Slot<T>,Slots>& table,const std::shared_ptr<T>& value,std::uint32_t offset) {
        const std::lock_guard lock(mutex);
        const auto retained=[](const auto& slots){return static_cast<std::size_t>(std::count_if(slots.begin(),slots.end(),[](const auto& slot){return bool(slot.value);}));};
        if(retained(invocations)+retained(tasks)>=Slots)return {};
        for(std::uint32_t i=0;i<Slots;++i)if(!table[i].value&&table[i].generation){table[i].value=value;return {block+offset+i,table[i].generation};}
        return {};
    }
    /** @summary Detach exact storage; caller retains lease outside bookkeeping lock.
     * @param table Fixed kind table. @param handle Original identity. @param offset Kind range.
     * @param value Original lease. @return Whether exact entry was removed. */
    template<class T> bool Remove(std::array<Slot<T>,Slots>& table,SdHandle handle,std::uint32_t offset,const std::shared_ptr<T>& value) {
        const std::lock_guard lock(mutex);
        if(handle.slot<block+offset||handle.slot>=block+offset+Slots)return false;
        auto& slot=table[handle.slot-block-offset];
        if(slot.generation!=handle.generation||slot.value!=value)return false;
        slot.value.reset();slot.generation=slot.generation==UINT32_MAX?0:slot.generation+1;return true;
    }
    /** @summary Verify captured role/session; agent cannot select native role with zero session.
     * @param candidate Caller session. @return Matching original role and session. */
    bool Role(SdHandle candidate) const {
        if(std::holds_alternative<runtime::AgentOrigin>(binding.authority.origin))return Present(session)&&Same(candidate,session);
        return std::holds_alternative<runtime::OrdinaryScriptOrigin>(binding.authority.origin)&&!candidate.slot&&!candidate.generation&&!session.slot&&!session.generation;
    }
};

NativeProvider::NativeProvider(SdHandle owner,std::shared_ptr<scripting::BaseHost> base,
    std::shared_ptr<scripting::SemanticEndpoint> endpoint,ProviderParticipant participant,
    std::shared_ptr<runtime::RuntimeLifecycle> lifecycle,SdHandle session,std::vector<ProviderRoute> routes)
    :state_(std::make_unique<State>(std::move(participant))) {
    if(!Present(owner)||!base||!endpoint||!endpoint->bridge||base->bridge_!=endpoint->bridge||routes.size()>64)
        throw std::invalid_argument("Invalid native provider capture");
    auto& s=*state_;s.owner=owner;s.session=session;s.base=std::move(base);s.endpoint=std::move(endpoint);
    if(const auto* script=std::get_if<std::shared_ptr<scripting::ScriptRuntime>>(&s.participant.lifetime_)) {
        if(!*script || (*script)->host_.semantic!=s.endpoint || !(*script)->vmGeneration_ ||
            (*script)->stopRequested_.load() || !lifecycle)throw std::invalid_argument("Invalid script participant");
    } else {
        const auto& native=std::get<NativeParticipant>(s.participant.lifetime_);
        std::shared_ptr<runtime::RuntimeScope> originalScope;std::shared_ptr<runtime::RuntimeLifecycle> originalLifecycle;
        const auto identity=native.ObserveLocalFence().identity;
        if(!native.StageLifetime(originalScope,originalLifecycle) || originalScope!=s.endpoint->scope ||
            !s.endpoint->Participant().Matches(native) || !Same(owner,identity.owner) ||
            (lifecycle && lifecycle!=originalLifecycle) || Present(session) || session.slot || session.generation || !routes.empty())
            throw std::invalid_argument("Invalid native participant");
        lifecycle=std::move(originalLifecycle);
    }
    s.lifecycle=std::move(lifecycle);
    if(s.endpoint->bridge->CaptureBinding(s.endpoint,s.binding)!=runtime::ScopeResult::Ok)
        throw std::invalid_argument("Unavailable native provider binding");
    if(const auto* script=std::get_if<std::shared_ptr<scripting::ScriptRuntime>>(&s.participant.lifetime_)) {
        if(s.binding.participant.ScriptGeneration()!=(*script)->vmGeneration_)throw std::invalid_argument("Mismatched script identity");
    } else if(s.binding.participant.Type()!=scripting::SemanticParticipantKey::Kind::RetainedNativeModule ||
        !std::holds_alternative<runtime::UnqualifiedOrigin>(s.binding.authority.origin) || s.binding.authority.reservation)
        throw std::invalid_argument("Native authority is unavailable");
    std::size_t bytes=sizeof(State)+Slots*(sizeof(State::Invocation)+sizeof(State::Task)+128)+routes.capacity()*sizeof(ProviderRoute);
    for(std::size_t i=0;i<routes.size();++i){
        const auto& route=routes[i];
        if(!route.method||route.name.empty()||route.name.size()>128||!Present(route.target)||!route.opcode||route.opcode!=s.binding.authority.opcode)
            throw std::invalid_argument("Invalid native provider route");
        for(std::size_t j=0;j<i;++j)if(routes[j].method==route.method)throw std::invalid_argument("Duplicate native provider method");
        const auto& methods=s.endpoint->bridge->Methods();
        if(std::none_of(methods.begin(),methods.end(),[&](const auto& method){return method.id==route.name;}))throw std::invalid_argument("Unreviewed native provider route");
        bytes+=route.name.capacity();
    }
    if(s.endpoint->bridge->Values()->Account(s.binding.storageOwner,bytes,s.accounting)!=runtime::copied::Status::Ok)
        throw std::runtime_error("Native provider storage exhausted");
    s.routes=std::move(routes);s.block=Block();
}
std::shared_ptr<scripting::SemanticEndpoint> NativeProvider::NativeEndpoint(const std::shared_ptr<scripting::BaseHost>& base,
    const NativeParticipant& participant) {
    if(!base || !base->bridge_)throw std::invalid_argument("Unavailable native Base");
    return base->bridge_->BindNative(participant);
}
NativeProvider::NativeProvider(std::shared_ptr<scripting::BaseHost> base,const NativeParticipant& participant)
    :NativeProvider(participant.ObserveLocalFence().identity.owner,base,NativeEndpoint(base,participant),participant,{}, {}, {}){}
NativeProvider::~NativeProvider(){Close();}
SdResult NativeProvider::Map(runtime::ScopeResult result) noexcept {
    using runtime::ScopeResult;
    switch(result){case ScopeResult::Ok:return SD_OK;case ScopeResult::Denied:return SD_DENIED;case ScopeResult::Stale:return SD_STALE;
    case ScopeResult::NotReady:return SD_NOT_READY;case ScopeResult::Busy:return SD_BUSY;case ScopeResult::Conflict:return SD_CONFLICT;
    case ScopeResult::Cancelled:return SD_CANCELLED;case ScopeResult::Indeterminate:return SD_INDETERMINATE;
    case ScopeResult::BufferSmall:return SD_BUFFER_SMALL;case ScopeResult::Invalid:return SD_INVALID;}
    return SD_OPERATION_FAILED;
}
runtime::ScopeResult NativeProvider::CheckAuthority() const {
    const auto facts=CheckFacts();if(facts!=runtime::ScopeResult::Ok)return facts;
    if(state_->binding.participant.Type()==scripting::SemanticParticipantKey::Kind::RetainedNativeModule)return runtime::ScopeResult::NotReady;
    runtime::RuntimeScope::LifecycleGuard guard(*state_->binding.scope);
    return guard.Check(state_->binding.authority,false);
}
runtime::ScopeResult NativeProvider::CheckFacts() const {
    const auto& s=*state_;
    if(s.closed||s.endpoint->closed||s.lifecycle->Current().stopping)return runtime::ScopeResult::Stale;
    if(!s.base->Ready())return runtime::ScopeResult::Stale;
    scripting::SemanticEndpointBinding current;
    const auto captured=s.endpoint->bridge->CaptureBinding(s.endpoint,current);
    if(captured!=runtime::ScopeResult::Ok)return captured;
    return current==s.binding?runtime::ScopeResult::Ok:runtime::ScopeResult::Stale;
}
SdResult NativeProvider::Bind(SdHandle owner,SdHandle session,std::uint32_t method,SdHandle target,std::uint64_t revision,SdHandle* output) noexcept try {
    if(output)*output={};if(!output)return SD_INVALID;auto& s=*state_;
    if(!Same(owner,s.owner))return SD_DENIED;
    if(s.binding.participant.Type()==scripting::SemanticParticipantKey::Kind::RetainedNativeModule)return Map(CheckAuthority());
    if(!s.Role(session))return SD_DENIED;
    if(target.slot>=s.block+2*Slots&&target.slot<s.block+BlockWidth)return SD_STALE;
    const auto auth=CheckAuthority();if(auth!=runtime::ScopeResult::Ok)return Map(auth);
    if(revision!=s.binding.authority.revision)return SD_CONFLICT;
    for(std::size_t i=0;i<s.routes.size();++i)if(s.routes[i].method==method){
        if(!Same(target,s.routes[i].target))return SD_STALE;
        const auto context=s.endpoint->bridge->ValidateContext(*s.endpoint,s.routes[i].context);if(context!=runtime::ScopeResult::Ok)return Map(context);
        auto invocation=std::make_shared<State::Invocation>();invocation->route=i;
        *output=s.Add(s.invocations,invocation,0);return Present(*output)?SD_OK:SD_RESOURCE_LIMIT;
    }return SD_NOT_READY;
}catch(...){if(output)*output={};return SD_OPERATION_FAILED;}
SdResult NativeProvider::Invoke(SdHandle owner,SdHandle invocation,std::uint32_t method,SdBytes input,SdHandle* output) noexcept try {
    if(output)*output={};if(!output||(!input.data&&input.size))return SD_INVALID;auto& s=*state_;
    if(!Same(owner,s.owner))return SD_DENIED;
    if(s.binding.participant.Type()==scripting::SemanticParticipantKey::Kind::RetainedNativeModule)return Map(CheckAuthority());
    auto origin=s.Find(s.invocations,invocation,0);if(!origin)return SD_STALE;
    const auto auth=CheckAuthority();if(auth!=runtime::ScopeResult::Ok)return Map(auth);
    const auto& route=s.routes[origin->route];if(route.method!=method)return SD_DENIED;
    const auto facts=s.endpoint->bridge->ValidateContext(*s.endpoint,route.context);if(facts!=runtime::ScopeResult::Ok)return Map(facts);
    const auto& methods=s.endpoint->bridge->Methods();const auto descriptor=std::find_if(methods.begin(),methods.end(),[&](const auto& value){return value.id==route.name;});
    if(descriptor==methods.end())return SD_NOT_READY;
    runtime::copied::Envelope value;const auto decoded=s.endpoint->bridge->Values()->Decode(
        {reinterpret_cast<const char*>(input.data),input.size},descriptor->inputSchema,s.binding.storageOwner,value);
    if(decoded!=runtime::copied::Status::Ok)return decoded==runtime::copied::Status::ResourceLimit?SD_RESOURCE_LIMIT:SD_INVALID;
    auto task=std::make_shared<State::Task>();task->invocation=origin;const auto handle=s.Add(s.tasks,task,Slots);if(!Present(handle))return SD_RESOURCE_LIMIT;
    runtime::ScopeResult admitted;
    try {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        if(afterTaskReserve)afterTaskReserve();
#endif
        admitted=s.endpoint->bridge->Request(s.endpoint,route.name,value,task->task,task->context,&route.context,&s.binding);
    }
    catch (...) {s.Remove(s.tasks,handle,Slots,task);throw;}
    if(admitted!=runtime::ScopeResult::Ok){s.Remove(s.tasks,handle,Slots,task);return Map(admitted);}
    task->pending.store(false);*output=handle;return SD_OK;
}catch(...){if(output)*output={};return SD_OPERATION_FAILED;}
SdResult NativeProvider::Poll(SdHandle owner,SdHandle invocation,SdHandle task,SdTaskInfo* info,std::uint8_t* output,std::uint32_t capacity,std::uint32_t* required) noexcept try {
    const auto header=Clear(info);const bool buffer=Buffer(output,capacity,required);if(header!=SD_OK)return header;if(!buffer)return SD_INVALID;
    auto& s=*state_;if(!Same(owner,s.owner))return SD_DENIED;
    auto origin=s.Find(s.invocations,invocation,0);auto work=s.Find(s.tasks,task,Slots);if(!origin||!work)return SD_STALE;
    if(work->invocation!=origin)return SD_DENIED;if(work->pending||work->releasing)return SD_BUSY;
    const auto facts=CheckFacts();if(facts!=runtime::ScopeResult::Ok)return Map(facts);
    const auto context=s.endpoint->bridge->ValidateContext(*s.endpoint,work->context);if(context!=runtime::ScopeResult::Ok)return Map(context);
    runtime::TaskSnapshot observed;const auto auth=s.binding.tasks->Poll(work->task,observed);
    if(auth!=runtime::ScopeResult::Ok)return Map(auth);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    if(afterOriginalPoll)afterOriginalPoll();
#endif
    // @summary Delivery follows revalidation of the same full original binding and input-augmented execution lease.
    const auto current=CheckFacts();if(current!=runtime::ScopeResult::Ok)return Map(current);
    const auto fence=s.endpoint->bridge->ValidateContext(*s.endpoint,work->context);if(fence!=runtime::ScopeResult::Ok)return Map(fence);
    // @summary Reuse the original task's delivery evaluator, which preserves admitted revision across committed effects.
    const auto delivery=s.binding.tasks->Poll(work->task,observed);if(delivery!=runtime::ScopeResult::Ok)return Map(delivery);
    // @summary Encode borrows this one final charged immutable snapshot; no task/result is reconstructed or rerun.
    const auto bytes=observed.value.Encode();
    if(bytes.size()>runtime::copied::Storage::EnvelopeLimit)return SD_OPERATION_FAILED;
    const auto finalFacts=CheckFacts();if(finalFacts!=runtime::ScopeResult::Ok)return Map(finalFacts);
    const auto finalContext=s.endpoint->bridge->ValidateContext(*s.endpoint,work->context);if(finalContext!=runtime::ScopeResult::Ok)return Map(finalContext);
    if(bytes.size()>capacity){*required=static_cast<std::uint32_t>(bytes.size());return SD_BUFFER_SMALL;}
    SdTaskInfo delivered{{sizeof(SdTaskInfo),1}};delivered.task=task;
    // @summary The same task slot/generation maps the retained full lease into a disjoint non-authoritative kind.
    delivered.context={task.slot+Slots,task.generation};
    switch(observed.state){
    case runtime::TaskState::Queued:delivered.phase=SD_TASK_QUEUED;break;
    case runtime::TaskState::Running:delivered.phase=SD_TASK_RUNNING;break;
    case runtime::TaskState::Succeeded:delivered.phase=SD_TASK_SUCCEEDED;break;
    case runtime::TaskState::Failed:delivered.phase=SD_TASK_FAILED;break;
    case runtime::TaskState::Cancelled:delivered.phase=SD_TASK_CANCELLED;break;
    case runtime::TaskState::Indeterminate:delivered.phase=SD_TASK_INDETERMINATE;break;
    }
    delivered.error=Map(observed.outcome);delivered.flags=observed.committed?SD_TASK_COMMITTED:0;
    delivered.revision=observed.revision;delivered.sequence=observed.sequence;
    if(!bytes.empty())std::memcpy(output,bytes.data(),bytes.size());
    *required=static_cast<std::uint32_t>(bytes.size());*info=delivered;return SD_OK;
}catch(...){return SD_OPERATION_FAILED;}
SdResult NativeProvider::Snapshot(SdHandle owner,SdHandle invocation,std::uint32_t method,SdSnapshotInfo* info,std::uint8_t* output,std::uint32_t capacity,std::uint32_t* required) noexcept try {
    const auto header=Clear(info);const bool buffer=Buffer(output,capacity,required);if(header!=SD_OK)return header;if(!buffer)return SD_INVALID;
    auto& s=*state_;if(!Same(owner,s.owner))return SD_DENIED;auto origin=s.Find(s.invocations,invocation,0);if(!origin)return SD_STALE;
    if(s.routes[origin->route].method!=method)return SD_DENIED;
    const auto auth=CheckAuthority();if(auth!=runtime::ScopeResult::Ok)return Map(auth);
    const auto context=s.endpoint->bridge->ValidateContext(*s.endpoint,s.routes[origin->route].context);
    return context==runtime::ScopeResult::Ok?SD_NOT_READY:Map(context);
}catch(...){return SD_OPERATION_FAILED;}
SdResult NativeProvider::Cancel(SdHandle owner,SdHandle invocation,SdHandle task) noexcept try {
    auto& s=*state_;if(!Same(owner,s.owner))return SD_DENIED;auto origin=s.Find(s.invocations,invocation,0);auto work=s.Find(s.tasks,task,Slots);
    if(!origin||!work)return SD_STALE;if(work->invocation!=origin)return SD_DENIED;if(work->pending||work->releasing)return SD_BUSY;
    const auto facts=CheckFacts();if(facts!=runtime::ScopeResult::Ok)return Map(facts);
    const auto context=s.endpoint->bridge->ValidateContext(*s.endpoint,work->context);if(context!=runtime::ScopeResult::Ok)return Map(context);
    return Map(s.binding.tasks->Cancel(work->task));
}catch(...){return SD_OPERATION_FAILED;}
SdResult NativeProvider::Release(SdHandle owner,SdHandle task) noexcept try {
    auto& s=*state_;if(!Same(owner,s.owner))return SD_DENIED;auto work=s.Find(s.tasks,task,Slots);if(!work)return SD_STALE;
    if(work->pending||work.use_count()>2||work->releasing.exchange(true))return SD_BUSY;
    const auto result=s.binding.tasks->Release(work->task);
    if(result!=runtime::ScopeResult::Ok){work->releasing=false;return Map(result);}
    return s.Remove(s.tasks,task,Slots,work)?SD_OK:SD_STALE;
}catch(...){return SD_OPERATION_FAILED;}
SdResult NativeProvider::ReleaseInvocation(SdHandle owner,SdHandle invocation) noexcept try {
    auto& s=*state_;if(!Same(owner,s.owner))return SD_DENIED;auto origin=s.Find(s.invocations,invocation,0);if(!origin)return SD_STALE;
    // @summary Recheck shared lease count while admission bookkeeping is reserved; final destruction is off-lock.
    {const std::lock_guard lock(s.mutex);if(origin.use_count()>2)return SD_BUSY;
    const auto index=invocation.slot-s.block;auto& slot=s.invocations[index];if(slot.value!=origin||slot.generation!=invocation.generation)return SD_STALE;
    slot.value.reset();slot.generation=slot.generation==UINT32_MAX?0:slot.generation+1;}
    return SD_OK;
}catch(...){return SD_OPERATION_FAILED;}
void NativeProvider::Close() noexcept {if(state_&&!state_->closed.exchange(true))state_->binding.scope->RequestStop();}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
std::shared_ptr<scripting::SemanticEndpoint> NativeProviderTestAccess::Endpoint(const NativeProvider& provider){return provider.state_->endpoint;}
bool NativeProviderTestAccess::ExhaustBlockRange() noexcept {
    std::uint32_t unused=1;
    return nextBlock.compare_exchange_strong(unused,UINT32_MAX-BlockWidth+1);
}
void NativeProviderTestAccess::AfterPoll(std::function<void()> callback){afterOriginalPoll=std::move(callback);}
void NativeProviderTestAccess::AfterTaskReserve(std::function<void()> callback){afterTaskReserve=std::move(callback);}
#endif
}
