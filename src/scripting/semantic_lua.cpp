#include "scripting/semantic_lua.hpp"
#include "scripting/script_runtime.hpp"
#include <lua.h>
#include <lualib.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace sc13::scripting {
namespace {
constexpr int valueTag = 64, taskTag = 65, invocationTag = 66,subscriptionTag=67;
static_assert(subscriptionTag < LUA_UTAG_LIMIT,"Private semantic userdata tags must remain inside the pinned Luau destructor table");
/** @summary Immutable native subscription cursor, not a public generation identifier. */
struct SubscriptionToken final {std::size_t slot;std::uint64_t serial;};
/** @summary Emit bounded host-assigned codes rather than arbitrary script diagnostic strings. */
const char* Code(runtime::ScopeResult result) {
    using runtime::ScopeResult;
    switch (result) {
    case ScopeResult::Ok: return "Ok"; case ScopeResult::Denied: return "Denied";
    case ScopeResult::Stale: return "Stale"; case ScopeResult::NotReady: return "NotReady";
    case ScopeResult::Busy: return "Busy"; case ScopeResult::Conflict: return "Conflict";
    case ScopeResult::Cancelled: return "Cancelled"; case ScopeResult::Indeterminate: return "Indeterminate";
    case ScopeResult::BufferSmall: return "BufferSmall"; default: return "Invalid";
    }
}
/** @summary Preserve native 64-bit identities as decimal text rather than rounded doubles. */
void Identity(lua_State* state, const char* field, std::uint64_t value) {
    char bytes[21]{}; const auto length = std::snprintf(bytes, sizeof(bytes), "%llu", static_cast<unsigned long long>(value));
    lua_pushlstring(state, bytes, static_cast<std::size_t>(length)); lua_setfield(state, -2, field);
}
}
struct SemanticLua::Value final {
    runtime::copied::Envelope value;std::uint64_t generation{};
    SemanticContextLease references;runtime::ScopeResult referenceStatus{runtime::ScopeResult::Ok};bool hasReferences{};
};
struct SemanticLua::TaskLease final {
    std::shared_ptr<SemanticEndpoint> origin;
    runtime::TaskHandle handle;
    SemanticContextLease context;
    runtime::copied::Reservation accounting;
    /** @summary Release owner-bound retained state even after revocation, without disclosing its result. */
    ~TaskLease() { if (origin && handle) { try { origin->tasks->Release(handle); } catch (...) {} } }
};
struct SemanticLua::Task final { std::shared_ptr<TaskLease> lease; std::uint64_t generation{}; };
struct SemanticLua::Invocation final { SemanticInvocation value; bool consumed{}; };
SemanticLua::SemanticLua(ScriptRuntime& runtime,runtime::copied::Reservation accounting)
    : owner_(runtime), endpoint_(runtime.host_.semantic), accounting_(std::move(accounting)) {
    if (!endpoint_ || endpoint_->generation || endpoint_->closed) throw std::runtime_error("Semantic endpoint already bound");
    endpoint_->generation = owner_.vmGeneration_;
}
SemanticLua::~SemanticLua() { Stop(); }
SemanticLua& SemanticLua::Self(lua_State* state) {
    auto* owner = static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
    if (!owner || !owner->semantics_ || owner->semantics_->stopped_) luaL_error(state, "Semantic endpoint unavailable");
    return *owner->semantics_;
}
void SemanticLua::PushValue(lua_State* state, const runtime::copied::Envelope& value,const SemanticContextLease* captured) {
    auto* wrapper=new (lua_newuserdatatagged(state, sizeof(Value), valueTag)) Value{};
    wrapper->value=value;wrapper->generation=owner_.vmGeneration_;
    for(std::uint32_t i=0;const auto* node=value.At(i);++i)if(node->tag==runtime::copied::Tag::Ref){wrapper->hasReferences=true;break;}
    if(wrapper->hasReferences){
        if(captured){wrapper->references=*captured;
            for(std::uint32_t i=0;const auto* node=value.At(i);++i)if(node->tag==runtime::copied::Tag::Ref){
                const auto kind=value.Text(*node);const std::size_t index=kind=="runtime"?0:kind=="region"?1:kind=="city"?2:3;
                const auto& fact=captured->facts[index];
                if(!(captured->mask&(1U<<index))||fact.slot!=node->bits||fact.generation!=node->generation)wrapper->referenceStatus=runtime::ScopeResult::Stale;
            }
        }else wrapper->referenceStatus=endpoint_->bridge->BindReferences(*endpoint_,value,wrapper->references);
    }
}
void SemanticLua::PushTask(lua_State* state, runtime::TaskHandle task,const SemanticContextLease& context) {
    runtime::copied::Reservation accounting;
    if(endpoint_->bridge->Values()->Account(endpoint_->storageOwner,sizeof(TaskLease)+64,accounting)!=runtime::copied::Status::Ok)
        throw std::runtime_error("Semantic task presentation storage exhausted");
    auto lease = std::make_shared<TaskLease>(); lease->origin = endpoint_; lease->handle = task;lease->accounting=std::move(accounting);
    lease->context=context;
    new (lua_newuserdatatagged(state, sizeof(Task), taskTag)) Task{std::move(lease), owner_.vmGeneration_};
}
SemanticLua::Task& SemanticLua::CheckTask(lua_State* state, int index) {
    auto* task = static_cast<Task*>(lua_touserdatatagged(state, index, taskTag));
    if (!task || task->generation != owner_.vmGeneration_ || !task->lease || task->lease->origin != endpoint_)
        luaL_error(state, "Invalid owner task");
    return *task;
}
int SemanticLua::Validate(lua_State* state) {
    auto& self = Self(state); std::size_t length{}, schemaLength{};
    const auto* wire = luaL_checklstring(state, 1, &length);
    const auto* schema = luaL_checklstring(state, 2, &schemaLength);
    runtime::copied::Envelope value;
    const auto status = self.endpoint_->bridge->Values()->Decode({wire,length}, {schema,schemaLength}, self.endpoint_->storageOwner, value);
    if (status != runtime::copied::Status::Ok) { lua_pushnil(state); lua_pushstring(state, status == runtime::copied::Status::Invalid ? "Invalid" : "ResourceLimit"); return 2; }
    self.PushValue(state,value); return 1;
}
int SemanticLua::Wire(lua_State* state) {
    auto& self = Self(state);
    const auto* value = static_cast<Value*>(lua_touserdatatagged(state,1,valueTag));
    if (!value || value->generation != self.owner_.vmGeneration_) luaL_error(state,"Invalid owner value");
    const auto bytes = value->value.Encode(); lua_pushlstring(state,bytes.data(),bytes.size()); return 1;
}
int SemanticLua::Request(lua_State* state) {
    auto& self = Self(state); std::size_t length{};
    const auto* method = luaL_checklstring(state,1,&length);
    const auto* value = static_cast<Value*>(lua_touserdatatagged(state,2,valueTag));
    if (!value || value->generation != self.owner_.vmGeneration_) luaL_error(state,"Invalid owner value");
    if (self.owner_.bootstrap_) {
        auto* invocation = static_cast<Invocation*>(lua_touserdatatagged(state,3,invocationTag));
        if (!invocation || invocation->consumed || invocation->value.epoch != self.endpoint_->bridge->Epoch())
            luaL_error(state,"Missing private invocation");
        invocation->consumed = true;
        const auto result = self.endpoint_->bridge->Dispatch(invocation->value,{method,length},value->value);
        lua_pushstring(state,Code(result)); return 1;
    }
    if (!self.owner_.committed_ || !self.owner_.running_ || self.owner_.stopping_) {
        lua_pushnil(state); lua_pushstring(state,"NotReady"); return 2;
    }
    if(value->hasReferences){auto reference=value->referenceStatus;if(reference==runtime::ScopeResult::Ok)reference=self.endpoint_->bridge->ValidateContext(*self.endpoint_,value->references);
        if(reference!=runtime::ScopeResult::Ok){lua_pushnil(state);lua_pushstring(state,Code(reference));return 2;}}
    runtime::TaskHandle task; SemanticContextLease context;
    const auto result = self.endpoint_->bridge->Request(self.endpoint_,{method,length},value->value,task,context);
    self.Record(SemanticPhase::Admit,result,&context);
    if (result != runtime::ScopeResult::Ok) { lua_pushnil(state); lua_pushstring(state,Code(result)); return 2; }
    try {self.PushTask(state,task,context);}catch(...){self.endpoint_->tasks->Release(task);throw;} return 1;
}
int SemanticLua::PushPoll(lua_State* state, const TaskLease& task) {
    const auto context=endpoint_->bridge->ValidateContext(*endpoint_,task.context);
    if(context!=runtime::ScopeResult::Ok){lua_pushnil(state);lua_pushstring(state,Code(context));return 2;}
    runtime::TaskSnapshot snapshot;
    const auto result = endpoint_->tasks->Poll(task.handle,snapshot);
    Record(SemanticPhase::Delivery,result,&task.context);
    if (result != runtime::ScopeResult::Ok) { lua_pushnil(state); lua_pushstring(state,Code(result)); return 2; }
    lua_newtable(state);
    const auto* phase = "queued";
    switch (snapshot.state) {
    case runtime::TaskState::Running: phase="running"; break;
    case runtime::TaskState::Succeeded: phase="succeeded"; break;
    case runtime::TaskState::Failed: phase="failed"; break;
    case runtime::TaskState::Cancelled: phase="cancelled"; break;
    case runtime::TaskState::Indeterminate: phase="indeterminate"; break;
    default: break;
    }
    lua_pushstring(state,phase); lua_setfield(state,-2,"state");
    lua_pushboolean(state,snapshot.committed); lua_setfield(state,-2,"committed");
    if (snapshot.value) { PushValue(state,snapshot.value,&task.context); lua_setfield(state,-2,"result"); }
    lua_pushstring(state,Code(snapshot.outcome)); lua_setfield(state,-2,"error");
    lua_setreadonly(state,-1,true); return 1;
}
int SemanticLua::Poll(lua_State* state) { auto& self=Self(state); return self.PushPoll(state,*self.CheckTask(state,1).lease); }
int SemanticLua::Cancel(lua_State* state) {
    auto& self=Self(state);const auto& lease=*self.CheckTask(state,1).lease;
    auto result=self.endpoint_->bridge->ValidateContext(*self.endpoint_,lease.context);
    if(result==runtime::ScopeResult::Ok)result=self.endpoint_->tasks->Cancel(lease.handle);
    lua_pushstring(state,Code(result)); return 1;
}
int SemanticLua::Observe(lua_State* state) {
    auto& self=Self(state); auto lease=self.CheckTask(state,1).lease;
    luaL_checktype(state,2,LUA_TFUNCTION);
    if (!self.AcquireRegistration()) { lua_pushnil(state); lua_pushstring(state,"ResourceLimit"); return 2; }
    for (auto& callback:self.callbacks_) if (callback.reference == -1) {
        callback={std::move(lease),lua_ref(state,2),self.turn_+1};
        lua_pushboolean(state,true); return 1;
    }
    luaL_error(state,"ResourceLimit");
}
int SemanticLua::Await(lua_State* state) {
    auto& self=Self(state); auto lease=self.CheckTask(state,1).lease;
    for (auto& coroutine:self.coroutines_) if (coroutine.state == state && coroutine.reference != -1 && !coroutine.waiting) {
        if (!lua_isyieldable(state)) luaL_error(state,"Await boundary is not yieldable");
        coroutine.waiting=std::move(lease); coroutine.turn=self.turn_+1;self.Record(SemanticPhase::Await,runtime::ScopeResult::Busy,&coroutine.waiting->context); return lua_yield(state,0);
    }
    luaL_error(state,"Await requires a host-registered coroutine");
}
int SemanticLua::Run(lua_State* state) {
    auto& self=Self(state); luaL_checktype(state,1,LUA_TFUNCTION);
    if (self.owner_.bootstrap_ || !self.owner_.committed_ || !self.AcquireRegistration()) {
        lua_pushnil(state); lua_pushstring(state,"NotReady"); return 2;
    }
    for (auto& coroutine:self.coroutines_) if (coroutine.reference == -1) {
        auto* thread=lua_newthread(state); const int reference=lua_ref(state,-1); lua_pop(state,1);
        lua_pushvalue(state,1); lua_xmove(state,thread,1);
        coroutine={thread,reference,{},self.turn_+1,false};
        lua_pushboolean(state,true); return 1;
    }
    luaL_error(state,"ResourceLimit");
}
SemanticLua::LifecycleRecord SemanticLua::CaptureSnapshot() {
    return CaptureSnapshot(endpoint_->bridge->CurrentPublication());
}
SemanticLua::LifecycleRecord SemanticLua::CaptureSnapshot(const SemanticPublication& publication) {
    const auto scope=endpoint_->scope->Current();
    return {publication.baseEpoch,publication.baseGeneration,owner_.vmGeneration_,scope.epoch,scope.revision,
        publication.sequence,publication.monotonicNs,publication.baseGeneration!=0,publication.contexts,publication.singlePlayerKnown};
}
void SemanticLua::PushSnapshot(lua_State* state,const LifecycleRecord& record) {
    lua_newtable(state); Identity(state,"baseEpoch",record.epoch);
    Identity(state,"baseGeneration",record.baseGeneration); Identity(state,"generation",record.generation);
    Identity(state,"runtimeEpoch",record.scopeEpoch); Identity(state,"revision",record.revision);
    Identity(state,"sequence",record.sequence); Identity(state,"monotonicNs",record.monotonicNs);
    lua_pushboolean(state,record.ready); lua_setfield(state,-2,"ready");
    lua_pushboolean(state,record.singlePlayerKnown);lua_setfield(state,-2,"singlePlayerKnown");
    lua_newtable(state);constexpr const char* kinds[]{"runtime","region","city","entity"};
    for(std::size_t i=0;i<record.contexts.size();++i){lua_newtable(state);Identity(state,"slot",record.contexts[i].slot);Identity(state,"generation",record.contexts[i].generation);Identity(state,"epoch",record.contexts[i].epoch);lua_setreadonly(state,-1,true);lua_setfield(state,-2,kinds[i]);}
    lua_setreadonly(state,-1,true);lua_setfield(state,-2,"contexts");
    lua_setreadonly(state,-1,true);
}
void SemanticLua::PushSnapshot(lua_State* state) { PushSnapshot(state,CaptureSnapshot()); }
int SemanticLua::Snapshot(lua_State* state) { auto& self=Self(state); self.PushSnapshot(state); return 1; }
int SemanticLua::Subscribe(lua_State* state) {
    auto& self=Self(state); luaL_checktype(state,1,LUA_TFUNCTION);
    if (self.retained_>=64 || self.mailboxCount_==self.mailbox_.size() || !self.AcquireRegistration()) { lua_pushnil(state); lua_pushstring(state,"ResourceLimit"); return 2; }
    for (std::size_t index=0;index<self.subscriptions_.size();++index) {auto& subscription=self.subscriptions_[index];if (subscription.reference == -1) {
        if (self.mailboxCount_==self.mailbox_.size()) {lua_pushnil(state);lua_pushstring(state,"ResourceLimit");return 2;}
        if(self.nextSubscription_==UINT64_MAX)luaL_error(state,"Subscription identity exhausted");
        subscription.serial=self.nextSubscription_++;subscription.reference=lua_ref(state,1); subscription.last=self.CaptureSnapshot();subscription.cursor=subscription.last.sequence;
        self.Enqueue(subscription,subscription.last);
        lua_newtable(state);
        new(lua_newuserdatatagged(state,sizeof(SubscriptionToken),subscriptionTag))SubscriptionToken{index,subscription.serial};
        lua_pushcclosure(state,&Unsubscribe,"unsubscribe",1);lua_setfield(state,-2,"unsubscribe");lua_setreadonly(state,-1,true);return 1;
    }}
    luaL_error(state,"ResourceLimit");
}
int SemanticLua::Unsubscribe(lua_State* state){
    auto& self=Self(state);const auto* token=static_cast<SubscriptionToken*>(lua_touserdatatagged(state,lua_upvalueindex(1),subscriptionTag));
    if(!token||token->slot>=self.subscriptions_.size()){lua_pushstring(state,"Stale");return 1;}
    auto& subscription=self.subscriptions_[token->slot];
    if(subscription.reference==-1||subscription.serial!=token->serial){lua_pushstring(state,"Stale");return 1;}
    subscription.cancelled=true;
    if(subscription.executing){lua_pushstring(state,"Busy");return 1;}
    lua_unref(state,subscription.reference);self.Clear(subscription);subscription={};self.ReleaseRegistration();
    lua_pushstring(state,"Ok");return 1;
}
int SemanticLua::Descriptors(lua_State* state) {
    const auto& methods=Self(state).endpoint_->bridge->Methods(); lua_newtable(state); int index=1;
    for (const auto& method:methods) {
        lua_newtable(state);
        for (const auto& field: {std::pair{"id",&method.id}, {"inputSchema",&method.inputSchema}, {"lane",&method.lane},
            {"context",&method.context}, {"capability",&method.capability}, {"postcondition",&method.postcondition}}) {
            lua_pushlstring(state,field.second->data(),field.second->size()); lua_setfield(state,-2,field.first);
        }
        lua_pushstring(state,"task"); lua_setfield(state,-2,"kind");
        lua_pushnumber(state,static_cast<double>(method.maxResultBytes)); lua_setfield(state,-2,"maxResultBytes");
        lua_setreadonly(state,-1,true); lua_rawseti(state,-2,index++);
    }
    lua_setreadonly(state,-1,true); return 1;
}
void SemanticLua::Install() {
    auto* state=owner_.state_;
    lua_setuserdatadtor(state,valueTag,[](lua_State*,void* value){static_cast<Value*>(value)->~Value();});
    lua_setuserdatadtor(state,taskTag,[](lua_State*,void* value){static_cast<Task*>(value)->~Task();});
    lua_setuserdatadtor(state,invocationTag,[](lua_State*,void* value){static_cast<Invocation*>(value)->~Invocation();});
    lua_newtable(state);
    const auto group=[&](const char* name,std::initializer_list<std::pair<const char*,lua_CFunction>> functions) {
        lua_newtable(state); for (const auto& [field,function]:functions) {
            lua_pushcfunction(state,function,field); lua_setfield(state,-2,field);
        } lua_setreadonly(state,-1,true); lua_setfield(state,-2,name);
    };
    group("values",{{"validate",Validate},{"wire",Wire}});
    group("tasks",{{"poll",Poll},{"cancel",Cancel},{"observe",Observe},{"await",Await}});
    group("lifecycle",{{"snapshot",Snapshot},{"subscribe",Subscribe}});
    group("registry",{{"descriptors",Descriptors}});
    group("runtime",{{"request",Request},{"run",Run}});
    lua_setreadonly(state,-1,true);
    if (owner_.bootstrap_) lua_setglobal(state,"__simoder_base");
    else {
        // @summary Presentation source is copied only from the already hash-verified base generation.
        lua_pushstring(state,"scripts/.simoder-base/init.luau");
        lua_pushcclosure(state,&ScriptRuntime::Require,"require",1); lua_pushstring(state,"./facade"); owner_.Call(1,1);
        lua_getfield(state,-1,"create"); lua_remove(state,-2); lua_insert(state,-2); owner_.Call(1,1);
        lua_setglobal(state,"base");
    }
}
bool SemanticLua::Qualify(int tableRef) {
    if (!owner_.bootstrap_) return false;
    auto* state=owner_.state_; lua_getref(state,tableRef); lua_getfield(state,-1,"services"); lua_remove(state,-2);
    if (!lua_istable(state,-1) || !lua_getreadonly(state,-1)) { lua_pop(state,1); return false; }
    const std::array<std::pair<const char*,std::vector<const char*>>,5> required{{
        {"values",{"validate","wire"}}, {"tasks",{"wrap","stop"}}, {"lifecycle",{"snapshot","subscribe","stop"}},
        {"registry",{"descriptors","resolve","manager","stop"}}, {"runtime-manager",{"dispatch","snapshot","subscribe","stop"}}}};
    for (const auto& [name,methods]:required) {
        lua_getfield(state,-1,name);
        if (!lua_istable(state,-1) || !lua_getreadonly(state,-1)) { lua_pop(state,2); return false; }
        for (const auto* method:methods) { lua_getfield(state,-1,method); const bool valid=lua_isfunction(state,-1); lua_pop(state,1); if (!valid) {lua_pop(state,2);return false;} }
        lua_pop(state,1);
    }
    servicesRef_=lua_ref(state,-1);
    lua_getfield(state,-1,"runtime-manager"); lua_getfield(state,-1,"dispatch"); dispatchRef_=lua_ref(state,-1); lua_pop(state,3);
    // @summary Verify actual singleton wiring and exercise authored services against the real native value/lifecycle bindings.
    const auto serviceFunction=[&](const char* service,const char* method){lua_getref(state,servicesRef_);lua_getfield(state,-1,service);lua_getfield(state,-1,method);lua_remove(state,-2);lua_remove(state,-2);};
    serviceFunction("registry","manager");lua_pushstring(state,"runtime");owner_.Call(1,1);
    lua_getref(state,servicesRef_);lua_getfield(state,-1,"runtime-manager");
    const bool singleton=lua_rawequal(state,-1,-3)!=0;lua_pop(state,3);
    if(!singleton)throw std::runtime_error("Base runtime manager is not its registered singleton");
    serviceFunction("registry","descriptors");owner_.Call(0,1);
    if(!lua_istable(state,-1)||!lua_getreadonly(state,-1)||lua_objlen(state,-1)!=static_cast<int>(endpoint_->bridge->Methods().size()))
        throw std::runtime_error("Base catalog qualification failed");
    lua_pop(state,1);
    serviceFunction("runtime-manager","snapshot");owner_.Call(0,1);
    if(!lua_istable(state,-1)||!lua_getreadonly(state,-1))throw std::runtime_error("Base lifecycle qualification failed");
    lua_getfield(state,-1,"baseEpoch");const auto epoch=std::to_string(endpoint_->bridge->Epoch());
    const auto* epochText=lua_tostring(state,-1);const bool matchingEpoch=epochText&&epoch==epochText;lua_pop(state,2);
    if(!matchingEpoch)throw std::runtime_error("Base snapshot did not preserve its actual epoch");
    constexpr const char* wire="{\"v\":1,\"schema\":\"simoder.readiness@1\",\"value\":[\"i64\",\"-9223372036854775808\"]}";
    serviceFunction("values","validate");lua_pushstring(state,wire);lua_pushstring(state,"simoder.readiness@1");owner_.Call(2,1);
    auto* copied=static_cast<Value*>(lua_touserdatatagged(state,-1,valueTag));
    if(!copied||copied->generation!=owner_.vmGeneration_||copied->value.Encode()!=wire)
        throw std::runtime_error("Base copied-value qualification failed");
    const int valueRef=lua_ref(state,-1);lua_pop(state,1);
    serviceFunction("values","wire");lua_getref(state,valueRef);owner_.Call(1,1);
    const auto* roundtrip=lua_tostring(state,-1);const bool exact=roundtrip&&std::strcmp(roundtrip,wire)==0;
    lua_unref(state,valueRef);lua_pop(state,1);
    if(!exact)throw std::runtime_error("Base exact-value roundtrip failed");
    // @summary A private readiness lease remains pending until this qualified VM's native postcommit.
    return true;
}
void SemanticLua::Pump() {
    if (stopped_ || !owner_.running_ || !owner_.committed_) return;
    ++turn_;
    auto* state=owner_.state_;
    if (owner_.bootstrap_ && dispatchRef_!=-1) {
        SemanticInvocation invocation;
        if (endpoint_->bridge->Take(invocation)) {
            Record(SemanticPhase::Dispatch,runtime::ScopeResult::Ok,&invocation.context);
            lua_getref(state,dispatchRef_);
            const auto& method=endpoint_->bridge->Methods()[invocation.method];
            lua_pushlstring(state,method.id.data(),method.id.size()); PushValue(state,invocation.value);
            auto* token=new (lua_newuserdatatagged(state,sizeof(Invocation),invocationTag)) Invocation{invocation,false};
            const int reference=lua_ref(state,-1);
            try { owner_.Call(3,0); if (!token->consumed) invocation.origin->tasks->FinishUncommitted(invocation.task,runtime::ScopeResult::Invalid); }
            catch (...) { invocation.origin->tasks->FinishUncommitted(invocation.task,runtime::ScopeResult::Invalid); }
            lua_unref(state,reference);
            // @summary External copied arenas are much larger than their Lua wrappers; reclaim unreachable dispatch wrappers before another native admission can exhaust the shared pool.
            lua_gc(state,LUA_GCCOLLECT,0);
            return;
        }
    }
    // @summary Capture transitions before delivering any observer; the initial snapshot cannot be overtaken.
    for (auto& subscription:subscriptions_) if (subscription.reference!=-1 && !subscription.faulted) {
        SemanticPublication publication;
        while(true){const auto next=endpoint_->bridge->NextPublication(subscription.cursor,publication);if(next==runtime::ScopeResult::NotReady)break;
            if(next!=runtime::ScopeResult::Ok){subscription.faulted=true;break;}
            const auto current=CaptureSnapshot(publication);if(!Enqueue(subscription,current)){subscription.faulted=true;break;}subscription.last=current;
        }
    }
    for(unsigned attempt=0;attempt<3;++attempt){const auto category=(workCategory_+attempt)%3;
    if(category==0)for(std::size_t n=0;n<callbacks_.size();++n){const auto index=(callbackCursor_+n)%callbacks_.size();auto& callback=callbacks_[index];if (callback.reference!=-1 && callback.turn<=turn_) {
        runtime::TaskSnapshot snapshot; const auto access=endpoint_->tasks->Poll(callback.task->handle,snapshot);
        if (access==runtime::ScopeResult::Ok && endpoint_->bridge->ValidateContext(*endpoint_,callback.task->context)==runtime::ScopeResult::Ok && (snapshot.state==runtime::TaskState::Queued || snapshot.state==runtime::TaskState::Running)) continue;
        const int reference=callback.reference; auto lease=std::move(callback.task); callback={}; ReleaseRegistration();
        Record(SemanticPhase::Callback,access,&lease->context);
        lua_getref(state,reference); lua_unref(state,reference); const int count=PushPoll(state,*lease);
        try { owner_.Call(count,0); } catch (...) { Failure("SEMANTIC_CALLBACK_FAILED"); }
        callbackCursor_=(index+1)%callbacks_.size();workCategory_=1;
        return;
    }}
    if(category==1)for(std::size_t n=0;n<coroutines_.size();++n){const auto index=(coroutineCursor_+n)%coroutines_.size();auto& coroutine=coroutines_[index];if (coroutine.reference!=-1 && coroutine.turn<=turn_) {
        int arguments{};
        if (coroutine.waiting) {
            runtime::TaskSnapshot snapshot; const auto access=endpoint_->tasks->Poll(coroutine.waiting->handle,snapshot);
            if (access==runtime::ScopeResult::Ok && endpoint_->bridge->ValidateContext(*endpoint_,coroutine.waiting->context)==runtime::ScopeResult::Ok && (snapshot.state==runtime::TaskState::Queued || snapshot.state==runtime::TaskState::Running)) continue;
            Record(SemanticPhase::Await,access,&coroutine.waiting->context);arguments=PushPoll(coroutine.state,*coroutine.waiting); coroutine.waiting.reset();
        } else if (coroutine.started) continue;
        coroutine.started=true; owner_.deadline_=std::chrono::steady_clock::now()+std::chrono::milliseconds(owner_.budgetMs_);
        const int status=lua_resume(coroutine.state,state,arguments);
        if (status!=LUA_OK && (status!=LUA_YIELD || !coroutine.waiting)) Failure("SEMANTIC_COROUTINE_FAILED");
        if (coroutine.reference!=-1 && (status!=LUA_YIELD || !coroutine.waiting)) { lua_unref(state,coroutine.reference); coroutine={}; ReleaseRegistration(); }
        coroutineCursor_=(index+1)%coroutines_.size();workCategory_=2;
        return;
    }}
    if(category==2)for(std::size_t n=0;n<subscriptions_.size();++n){const auto index=(subscriptionCursor_+n)%subscriptions_.size();auto& subscription=subscriptions_[index];if (subscription.reference!=-1 && (subscription.count || subscription.faulted)) {
        lua_getref(state,subscription.reference); int arguments=1;
        if (subscription.faulted) { lua_pushnil(state); lua_pushstring(state,"MailboxOverflow"); arguments=2; lua_unref(state,subscription.reference); Clear(subscription); subscription={}; ReleaseRegistration(); }
        else { auto& event=mailbox_[subscription.first]; PushSnapshot(state,event.record); subscription.first=event.next; event={}; --subscription.count; --mailboxCount_; if (!subscription.count) subscription.tail=128; }
        subscription.executing=true;
        try { owner_.Call(arguments,0); } catch (...) { Failure("SEMANTIC_SUBSCRIPTION_FAILED"); }
        subscription.executing=false;
        if(subscription.reference!=-1&&subscription.cancelled){lua_unref(state,subscription.reference);Clear(subscription);subscription={};ReleaseRegistration();}
        subscriptionCursor_=(index+1)%subscriptions_.size();workCategory_=0;
        return;
    }}}
}
void SemanticLua::Stop() noexcept {
    if (stopped_) return;Record(SemanticPhase::Stop,runtime::ScopeResult::Stale); stopped_=true; endpoint_->closed=true;
    auto* state=owner_.state_; if (!state) return;
    for (auto& callback:callbacks_) if (callback.reference!=-1) { lua_unref(state,callback.reference); callback={}; }
    for (auto& coroutine:coroutines_) if (coroutine.reference!=-1) { lua_unref(state,coroutine.reference); coroutine={}; }
    for (auto& subscription:subscriptions_) if (subscription.reference!=-1) { lua_unref(state,subscription.reference); Clear(subscription); subscription={}; }
    if (servicesRef_!=-1) lua_unref(state,servicesRef_);
    if (dispatchRef_!=-1) lua_unref(state,dispatchRef_);
    servicesRef_=dispatchRef_=-1;while(retained_)ReleaseRegistration();
    if (owner_.bootstrap_) endpoint_->bridge->Close();
}
bool SemanticLua::Enqueue(Subscription& subscription,const LifecycleRecord& record) noexcept {
    static_assert(sizeof(LifecycleRecord)*128<=1024U*1024U);
    if (mailboxCount_==mailbox_.size()) return false;
    for (std::size_t i=0;i<mailbox_.size();++i) if (!mailbox_[i].used) {
        mailbox_[i]={record,128,true};
        if (!subscription.count) subscription.first=i; else mailbox_[subscription.tail].next=i;
        subscription.tail=i; ++subscription.count; ++mailboxCount_; return true;
    }
    return false;
}
void SemanticLua::Clear(Subscription& subscription) noexcept {
    while (subscription.count) {auto& event=mailbox_[subscription.first];subscription.first=event.next;event={};--subscription.count;--mailboxCount_;}
    subscription.first=subscription.tail=128;
}
void SemanticLua::Failure(const char* code) noexcept {
    Record(SemanticPhase::Fault,runtime::ScopeResult::Invalid);
    owner_.CaptureDiagnostic(code);
    try {if(owner_.logs_.size()<128)owner_.logs_.emplace_back(code);else if(owner_.droppedLogs_!=UINT64_MAX)++owner_.droppedLogs_;}catch(...){}
    if(owner_.bootstrap_||owner_.host_.errorMode==ScriptHost::ErrorMode::Regular){owner_.running_=false;Stop();}
    owner_.PublishStatus();
}
bool SemanticLua::AcquireRegistration() noexcept {
    if(retained_>=64||!endpoint_->scope->ReserveRegistration())return false;++retained_;return true;
}
void SemanticLua::ReleaseRegistration() noexcept {
    if(retained_){--retained_;endpoint_->scope->ReleaseRegistration();}
}
void SemanticLua::Record(SemanticPhase phase,runtime::ScopeResult reason,const SemanticContextLease* context) noexcept {
    try{
        if(diagnosticSequence_==UINT64_MAX)return;
        const auto scope=endpoint_->scope->Current();SemanticDiagnostic record;
#if defined(SC13_SEMANTIC_BUILD_ID)
        constexpr const char* build=SC13_SEMANTIC_BUILD_ID;
#else
        constexpr const char* build="unqualified-source-build";
#endif
        const auto length=std::min(std::strlen(build),record.buildIdentity.size()-1);std::memcpy(record.buildIdentity.data(),build,length);
        record.sequence=++diagnosticSequence_;record.monotonicNs=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        record.vmGeneration=owner_.vmGeneration_;record.scopeGeneration=endpoint_->storageOwner;record.runtimeEpoch=scope.epoch;record.baseEpoch=endpoint_->bridge->Epoch();record.phase=phase;record.reason=reason;
        if(context){record.operation=context->operation;record.vmGeneration=context->vmGeneration;record.scopeGeneration=context->scopeGeneration;record.runtimeEpoch=context->runtimeEpoch;for(std::size_t i=0;i<record.contextEpochs.size();++i)record.contextEpochs[i]=context->facts[i].epoch;}
        if(diagnosticCount_==diagnostics_.size()){diagnosticFirst_=(diagnosticFirst_+1)%diagnostics_.size();--diagnosticCount_;if(diagnosticDropped_!=UINT64_MAX)++diagnosticDropped_;}
        record.dropped=diagnosticDropped_;diagnostics_[(diagnosticFirst_+diagnosticCount_)%diagnostics_.size()]=record;++diagnosticCount_;
    }catch(...){if(diagnosticDropped_!=UINT64_MAX)++diagnosticDropped_;}
}
std::vector<SemanticDiagnostic> SemanticLua::Diagnostics() const {
    std::vector<SemanticDiagnostic> output;output.reserve(diagnosticCount_);
    for(std::size_t i=0;i<diagnosticCount_;++i)output.push_back(diagnostics_[(diagnosticFirst_+i)%diagnostics_.size()]);return output;
}
}
