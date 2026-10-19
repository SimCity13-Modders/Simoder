#include "scripting/base_host.hpp"
#include "scripting/semantic_bridge.hpp"
#include "mods/owned_publication.hpp"
#include "scripting/luau_compiler.hpp"
#include "scripting/source_project.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <barrier>
#include <cstring>
#include <future>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace sc13::scripting;
using namespace sc13::runtime;
int checks{};
/** @summary Fail one behavioral expectation. @param value Actual condition. @param message Fixed test diagnostic. */
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
/** @summary Pin every actual executable package source, using test-host trusted local input. @param directory Actual Base checkout. @return Complete content pin. */
BasePackagePin Pin(const std::filesystem::path& directory) {
    BasePackagePin pin{"0.1.0-alpha", {}};
    pin.files.emplace("mod.toml", HashText(ReadBounded(directory / "mod.toml", 4096)));
    for (const auto& file : std::filesystem::recursive_directory_iterator(directory / "scripts")) {
        if (!file.is_regular_file()) continue;
        const auto name = file.path().lexically_relative(directory).generic_string();
        pin.files.emplace(name, HashText(ReadBounded(file.path(), 1024U * 1024U)));
    }
    return pin;
}
/** @summary Prepare isolated fixture authority; no issuer exists in the production core. @return Active exact scope. */
std::shared_ptr<RuntimeScope> Scope(std::shared_ptr<RuntimeCommitDomain> domain = {}) {
    auto scope = std::shared_ptr<RuntimeScope>(RuntimeScopeTestAccess::Create(std::move(domain)));
    Check(scope->CommitActivation(), "fixture scope activation failed"); scope->SetReadiness(true);
    return scope;
}
/** @summary Compile a real isolated mod VM with native-bound origin. @param host Actual verified Base host. @param scope Original authority scope. @param source Actual test mod source. @return Committed isolated runtime. */
std::unique_ptr<ScriptRuntime> Mod(BaseHost& host, const std::shared_ptr<RuntimeScope>& scope, std::string_view source,
    ScriptHost::ErrorMode mode = ScriptHost::ErrorMode::Regular, std::size_t memory = 8U * 1024U * 1024U, unsigned budget = 20) {
    auto binding = host.Bind(scope);
    binding.errorMode = mode;
    SemanticBridgeTestAccess::Issue(*binding.semantic, RuntimeScopeTestAccess::Issue(*scope));
    ScriptDefinition definition;
    definition.entry = "scripts/init.luau"; definition.language = "luau"; definition.budgetMs = budget; definition.memoryBytes = memory;
    definition.bytecode = CompileLuauSource(std::string(source));
    auto runtime = std::make_unique<ScriptRuntime>("fixture.same-description", definition, std::move(binding));
    runtime->Commit(); return runtime;
}
/** @summary Require a marker emitted by real script assertions without equating source loading to success. @param mod Script runtime. @param marker Expected fixed marker. */
void Marker(ScriptRuntime& mod, std::string_view marker) {
    const auto status = mod.Status();
    if (!status.running) throw std::runtime_error("mod failed: " + status.diagnostic);
    const auto logs = mod.DrainLogs();
    Check(std::any_of(logs.begin(), logs.end(), [&](const auto& log) { return log.find(marker) != std::string::npos; }), "script completion marker absent");
}
/** @summary Advance deterministic separate script/base turns without entering another VM inline. @param host Base runtime. @param mods Isolated mod VMs. */
void Turns(BaseHost& host, std::initializer_list<ScriptRuntime*> mods) {
    for (int turn = 0; turn != 8; ++turn) {
        for (auto* mod : mods) mod->Tick(0);
        host.Pump();
    }
}
/** @summary Define reviewed fixture-only echo behavior using the actual native callback and reserved result buffer. @param calls Observable effect count. @param context Required factual context. @return Native descriptor. */
SemanticMethod Echo(int& calls, std::string context = "runtime") {
    return {"fixture.echo", "test.echo@1", "script", std::move(context), "fixture-only", "echo-confirmed", 1024,
        [&calls](const copied::Envelope& input, std::span<char> output, std::size_t& size) {
            ++calls; const auto wire = input.Encode(); if (wire.size() > output.size()) return false;
            std::memcpy(output.data(), wire.data(), wire.size()); size = wire.size(); return true;
        }};
}
/** @summary Verify atomic source publications preserve all intermediate transitions and explicitly fault overflow. */
void Publications() {
    auto bridge = std::make_shared<SemanticBridge>();
    const auto initial = bridge->CurrentPublication();
    const auto repeated = bridge->CurrentPublication();
    Check(initial.sequence && initial.sequence == repeated.sequence && initial.monotonicNs == repeated.monotonicNs,
        "snapshot reads invented new publication identity");
    auto cursor = initial.sequence;
    for (std::uint64_t slot = 1; slot != 4; ++slot)
        SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, slot, 7);
    SemanticPublication publication;
    for (std::uint64_t slot = 1; slot != 4; ++slot) {
        Check(bridge->NextPublication(cursor, publication) == ScopeResult::Ok, "intermediate publication lost");
        Check(publication.contexts[2].slot == slot && publication.contexts[2].epoch == slot,
            "publication ordering/context mismatch");
    }
    Check(bridge->NextPublication(cursor, publication) == ScopeResult::NotReady && !publication.sequence,
        "empty stream replayed or retained stale output");
    const auto old = cursor;
    for (int index = 0; index != 129; ++index)
        SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 3, 7);
    Check(bridge->NextPublication(cursor, publication) == ScopeResult::Conflict && cursor == old && !publication.sequence,
        "overflow silently discarded source transitions");
}
/** @summary Run the actual Base graph and two isolated mod VMs through copied codec/task/callback/await primitives. @param package Actual pinned package root. */
void CrossVm(const std::filesystem::path& package) {
    int calls{};
    auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    Check(host.Ready() && bridge->ReadyGeneration() == host.Status().generation, "actual critical graph did not qualify");
    auto firstScope = Scope(), secondScope = Scope();
    constexpr auto source = R"lua(
        assert(__simoder_base == nil)
        assert(table.isfrozen(base) and table.isfrozen(base.values))
        local wire = '{"v":1,"schema":"test.echo@1","value":["list",[["u64","18446744073709551615"],["i64","-9223372036854775808"],["null"],["f64","8000000000000000"]]]}'
        local value = assert(base.values.validate(wire, "test.echo@1"))
        assert(base.values.wire(value) == wire)
        local invalid, reason = base.values.validate('{"v":1,"schema":"test.echo@1","value":["u64","18446744073709551616"]}', "test.echo@1")
        assert(invalid == nil and reason == "Invalid")
        local snapshot = base.snapshot()
        local repeated = base.snapshot()
        assert(snapshot.sequence == repeated.sequence and snapshot.monotonicNs == repeated.monotonicNs)
        assert(type(snapshot.baseEpoch) == "string" and table.isfrozen(snapshot))
        local task, completed, awaited, late, initial = nil, 0, false, false, false
        local started, finished = false, false
        return {
            onEnable = function()
                local denied, code = base.request("fixture.echo", value)
                assert(denied == nil and code == "NotReady")
            end,
            onTick = function()
                if not started then
                    started = true
                    assert(base.subscribe(function(record)
                        assert(record ~= nil and type(record.sequence) == "string")
                        initial = true
                    end))
                    assert(not initial)
                    task = assert(base.request("fixture.echo", value))
                    assert(task.poll().state == "queued")
                    assert(task.onCompleted(function(record)
                        assert(record.state == "succeeded" and record.committed and base.values.wire(record.result) == wire)
                        completed += 1
                    end))
                    local raw = coroutine.create(function() task.await() end)
                    assert(not coroutine.resume(raw))
                    assert(base.run(function()
                        local record = task.await()
                        assert(record and record.state == "succeeded" and base.values.wire(record.result) == wire)
                        awaited = true
                    end))
                elseif not finished and completed == 1 and awaited and initial then
                    assert(task.poll().state == "succeeded")
                    assert(task.cancel() == "Busy")
                    assert(task.onCompleted(function() late = true end))
                    assert(not late)
                    finished = true
                elseif finished and late then
                    assert(completed == 1)
                    print("CROSS_VM_COMPLETE")
                    late = false
                end
            end,
        }
    )lua";
    auto first = Mod(host, firstScope, source), second = Mod(host, secondScope, source);
    Check(first->Status().generation != second->Status().generation, "same textual mod description reused VM identity");
    Turns(host, {first.get(), second.get()});
    Marker(*first, "CROSS_VM_COMPLETE"); Marker(*second, "CROSS_VM_COMPLETE");
    Check(calls == 2, "cross-VM task effects duplicated or lost");
    first->Stop(); second->Stop();
}
/** @summary Verify cancellation prevents effects and native callback slots enforce the shared registration ceiling. @param package Actual pinned package. */
void CancellationAndBounds(const std::filesystem::path& package) {
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    auto mod = Mod(host, Scope(), R"lua(
        local value = assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["null"]}', "test.echo@1"))
        local started, cancelled = false, false
        return {onTick = function()
            if not started then
                started = true
                local task = assert(base.request("fixture.echo", value))
                assert(task.cancel() == "Ok")
                task.onFailed(function(record) assert(record.state == "cancelled" and not record.committed); cancelled = true end)
            elseif cancelled then print("CANCEL_COMPLETE"); cancelled = false end
        end}
    )lua");
    Turns(host, {mod.get()}); Marker(*mod, "CANCEL_COMPLETE"); Check(calls == 0, "cancelled task executed"); mod->Stop();
    auto bounded = Mod(host, Scope(), R"lua(
        local started = false
        return {onTick = function()
            if started then return end
            started = true
            for index = 1, 64 do assert(base.subscribe(function() end)) end
            local admitted, reason = base.subscribe(function() end)
            assert(admitted == nil and reason == "ResourceLimit")
            print("BOUND_COMPLETE")
        end}
    )lua");
    bounded->Tick(0); Marker(*bounded, "BOUND_COMPLETE"); bounded->Stop();
}
/** @summary Revalidate permission immediately before native commit and suppress prior results after revocation. @param package Actual pinned package. */
void Revocation(const std::filesystem::path& package) {
    for (const bool replaceIngress : {false, true}) {
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    auto scope = Scope();
    auto binding = host.Bind(scope);
    const auto endpoint = binding.semantic;
    const auto context = RuntimeScopeTestAccess::Issue(*scope);
    SemanticBridgeTestAccess::Issue(*binding.semantic, context);
    ScriptDefinition definition; definition.entry = "scripts/init.luau"; definition.budgetMs = 20;
    definition.bytecode = CompileLuauSource(R"lua(
        local started, task = false, nil
        return {onTick = function()
            if not started then
                started = true
                local value = assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["null"]}', "test.echo@1"))
                task = assert(base.request("fixture.echo", value))
            else
                local result, reason = task.poll()
                assert(result == nil and reason == "Denied")
                print("REVOKE_COMPLETE")
            end
        end}
    )lua");
    ScriptRuntime mod("fixture.revoked", definition, std::move(binding)); mod.Commit(); mod.Tick(0);
    if (replaceIngress) {
        RuntimeScopeTestAccess::Review review;
        review.origin = OrdinaryScriptOrigin{scope->Current().generation, 1}; review.required = 0;
        const auto ordinary = RuntimeScopeTestAccess::Decide(*scope,
            RuntimeScopeTestAccess::ChallengeDecision(*scope, review), review);
        Check(ordinary.owner != 0, "independent ordinary ingress fixture failed");
        ScopeHandle proof;
        Check(scope->Queue(ordinary, {}, proof) == ScopeResult::Ok &&
            scope->Release(ordinary.owner, proof) == ScopeResult::Ok, "replacement ordinary ingress was not independently valid");
        // @summary Even a host-only endpoint replacement cannot relabel the original retained agent task.
        endpoint->context = ordinary;
    }
    RuntimeScopeTestAccess::Revoke(*scope, context.grantGeneration);
    host.Pump(); mod.Tick(0); Marker(mod, "REVOKE_COMPLETE"); Check(calls == 0, "revoked queued task committed");
    mod.Stop();
    }
}
/** @summary Preserve single native outcomes across conflicting commits, foreign access and post-success revocation. @param package Actual pinned package. */
void NativeOutcomeGates(const std::filesystem::path& package) {
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    auto scope = Scope(); auto binding = host.Bind(scope); auto endpoint = binding.semantic;
    const auto issued = RuntimeScopeTestAccess::Issue(*scope);
    SemanticBridgeTestAccess::Issue(*endpoint, issued);
    ScriptDefinition definition; definition.entry = "scripts/init.luau"; definition.bytecode = CompileLuauSource("return {}");
    ScriptRuntime mod("fixture.native-gates", definition, std::move(binding)); mod.Commit();
    copied::Envelope value;
    Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["null"]})", "test.echo@1",
        endpoint->storageOwner, value) == copied::Status::Ok, "native gate fixture codec failed");
    TaskHandle first, second; SemanticContextLease firstContext, secondContext;
    Check(bridge->Request(endpoint, "fixture.echo", value, first, firstContext) == ScopeResult::Ok, "first writer rejected");
    Check(bridge->Request(endpoint, "fixture.echo", value, second, secondContext) == ScopeResult::Ok, "second queued writer rejected prematurely");
    SemanticInvocation one, two;
    Check(bridge->Take(one) && bridge->Take(two), "native queued invocations missing");
    Check(bridge->Dispatch(one, "fixture.echo", value) == ScopeResult::Ok, "first writer failed");
    Check(bridge->Dispatch(two, "fixture.echo", value) == ScopeResult::Conflict && calls == 1, "revision conflict executed second effect");
    TaskSnapshot snapshot;
    Check(endpoint->tasks->Poll(first, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Succeeded && snapshot.committed,
        "successful native outcome missing");
    Check(endpoint->tasks->Poll(second, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Failed && !snapshot.committed
        && snapshot.outcome == ScopeResult::Conflict, "conflict terminal outcome fabricated commit");
    auto foreign = host.Bind(Scope());
    Check(foreign.semantic->tasks->Poll(first, snapshot) != ScopeResult::Ok && !snapshot.value,
        "foreign native task handle disclosed a result");
    Check(bridge->Dispatch(one, "fixture.echo", value) != ScopeResult::Ok && calls == 1, "completed task replayed effect");
    RuntimeScopeTestAccess::Revoke(*scope, issued.grantGeneration);
    Check(endpoint->tasks->Poll(first, snapshot) == ScopeResult::Denied && !snapshot.value, "post-success revoke disclosed retained result");
    Check(endpoint->tasks->Release(first) == ScopeResult::Ok && endpoint->tasks->Release(second) == ScopeResult::Ok,
        "revoked owner could not release native outcomes");
    mod.Stop();
}
/** @summary Enforce pending-operation admission before effects through the actual bridge. @param package Actual pinned package. */
void PendingBound(const std::filesystem::path& package) {
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    auto scope = Scope(); auto binding = host.Bind(scope); auto endpoint = binding.semantic;
    SemanticBridgeTestAccess::Issue(*endpoint, RuntimeScopeTestAccess::Issue(*scope));
    ScriptDefinition definition; definition.entry = "scripts/init.luau"; definition.bytecode = CompileLuauSource("return {}");
    ScriptRuntime mod("fixture.pending-bound", definition, std::move(binding)); mod.Commit();
    copied::Envelope value;
    Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["null"]})", "test.echo@1",
        endpoint->storageOwner, value) == copied::Status::Ok, "pending fixture codec failed");
    std::array<TaskHandle, 8> admitted;
    SemanticContextLease context;
    for (auto& task : admitted)
        Check(bridge->Request(endpoint, "fixture.echo", value, task, context) == ScopeResult::Ok, "inclusive pending capacity rejected");
    TaskHandle rejected;
    Check(bridge->Request(endpoint, "fixture.echo", value, rejected, context) == ScopeResult::Busy && !rejected && calls == 0,
        "pending overflow admitted or executed effects");
    for (auto& task : admitted) {
        Check(endpoint->tasks->Cancel(task) == ScopeResult::Ok, "bounded task cleanup cancellation failed");
        Check(endpoint->tasks->Release(task) == ScopeResult::Ok, "bounded task release failed");
    }
    mod.Stop();
}
/** @summary Settle an actual removed bridge invocation when another native writer holds the same commit domain. @param package Actual package. */
void BusyWriterSettlement(const std::filesystem::path& package) {
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    auto domain = std::make_shared<RuntimeCommitDomain>();
    auto blockingScope = Scope(domain);
    SemanticTasks blocker(blockingScope, bridge->Values());
    const auto blockingContext = RuntimeScopeTestAccess::Issue(*blockingScope);
    copied::Envelope blockingValue;
    constexpr auto wire = R"({"v":1,"schema":"test.echo@1","value":["null"]})";
    Check(bridge->Values()->Decode(wire, "test.echo@1", blockingContext.owner, blockingValue) == copied::Status::Ok, "busy writer codec");
    TaskHandle blockingTask;
    Check(blocker.Admit(blockingContext, blockingValue, 1024, blockingTask) == ScopeResult::Ok && blocker.Begin(blockingTask) == ScopeResult::Ok,
        "busy writer preparation");
    auto scope = Scope(domain); auto binding = host.Bind(scope); auto endpoint = binding.semantic;
    const auto initialContext = RuntimeScopeTestAccess::Issue(*scope);
    SemanticBridgeTestAccess::Issue(*endpoint, initialContext);
    ScriptDefinition definition; definition.entry = "scripts/init.luau"; definition.bytecode = CompileLuauSource("return {}");
    ScriptRuntime mod("fixture.busy-writer", definition, std::move(binding)); mod.Commit();
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    std::future<ScopeResult> writer;
    /** @summary Ensure a failing fixture cannot strand the separate trusted native writer. */
    struct ReleaseOnExit final {
        std::promise<void>& signal;
        /** @summary Release even when a test assertion throws. */
        ~ReleaseOnExit() { try { signal.set_value(); } catch (...) {} }
    } guard{release};
    writer = std::async(std::launch::async, [&] {
        return blocker.Commit(blockingTask, [&](const auto& input, auto output, auto& size) {
            entered.set_value(); released.wait();
            const auto bytes = input.Encode(); std::memcpy(output.data(), bytes.data(), bytes.size()); size = bytes.size(); return true;
        });
    });
    entered.get_future().wait();
    // @summary Consent predates the writer; a new reservation captures its revision without issuing authority under contention.
    endpoint->context = RuntimeScopeTestAccess::Reserve(*scope, initialContext);
    Check(endpoint->context.owner != 0 && endpoint->context.deadline == initialContext.deadline &&
        endpoint->context.decision == initialContext.decision, "busy writer reservation renewed or lost consent");
    copied::Envelope value;
    Check(bridge->Values()->Decode(wire, "test.echo@1", endpoint->storageOwner, value) == copied::Status::Ok, "busy invocation codec");
    TaskHandle task; SemanticContextLease context;
    Check(bridge->Request(endpoint, "fixture.echo", value, task, context) == ScopeResult::Ok, "busy invocation admission");
    SemanticInvocation invocation; Check(bridge->Take(invocation), "busy invocation queue");
    Check(bridge->Dispatch(invocation, "fixture.echo", value) == ScopeResult::Busy && calls == 0, "busy writer executed bridge effect");
    TaskSnapshot snapshot;
    Check(endpoint->tasks->Poll(task, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Failed && !snapshot.committed && snapshot.outcome == ScopeResult::Busy,
        "busy dispatch left an orphaned preparing task");
    Check(endpoint->tasks->Release(task) == ScopeResult::Ok, "busy terminal cleanup");
    release.set_value();
    Check(writer.get() == ScopeResult::Ok && blocker.Release(blockingTask) == ScopeResult::Ok, "blocking writer completion");
    mod.Stop();
}
/** @summary Exercise exact-key native lifecycle without production permission or manager wiring.
 * @param package Actual pinned Base package used by isolated VM fixtures. */
void OwnerLifecycleTransitions(const std::filesystem::path& package) {
    using namespace sc13::mods;
    for (int scenario = 0; scenario != 23; ++scenario) {
        int calls{};
        auto bridge = std::make_shared<SemanticBridge>();
        auto method = Echo(calls, "city"); method.registryPublication = true;
        SemanticBridgeTestAccess::Configure(*bridge, {std::move(method)});
        SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
        SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
        BaseHost host(package, Pin(package), {}, bridge);
        auto domain = std::make_shared<RuntimeCommitDomain>();
        auto registry = std::make_shared<PatchRegistry>();
        auto oldScope = Scope(domain); auto oldBinding = host.Bind(oldScope); auto oldEndpoint = oldBinding.semantic;
        const auto originalDecision = RuntimeScopeTestAccess::Issue(*oldScope);
        SemanticBridgeTestAccess::Issue(*oldEndpoint, originalDecision);
        ScriptDefinition definition; definition.entry = "scripts/init.luau"; definition.language = "luau";
        definition.bytecode = CompileLuauSource("return {}"); definition.revision = "old-loaded";
        auto oldVm = std::make_shared<ScriptRuntime>("fixture.lifecycle", definition, std::move(oldBinding)); oldVm->Commit();
        ResourcePatchDefinition declared; declared.target = {1,2,3};
        declared.properties.push_back({4, PropertyType::Float, PatchOperation::Set, 1.0F});
        auto old = OwnedPublicationTestAccess::Issue(oldVm, oldEndpoint, "old-source", {declared});
        Check(OwnedPublicationTestAccess::Transition(*old, PublicationOrigin::Phase::Active), "lifecycle old phase");
        std::string error; std::set<sc13::core::Tgi> affected;
        Check(registry->Activate("foreign-before", {&declared,1}, affected, error), "lifecycle foreign-before");
        copied::Envelope value;
        Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]})",
            "test.echo@1", oldEndpoint->storageOwner, value) == copied::Status::Ok, "lifecycle input");
        TaskHandle task; SemanticContextLease oldLease;
        Check(bridge->Request(oldEndpoint,"fixture.echo",value,task,oldLease)==ScopeResult::Ok,"lifecycle old request");
        SemanticInvocation invocation; Check(bridge->Take(invocation),"lifecycle old dequeue");
        OwnedPublication initial;
        Check(OwnedPublicationTestAccess::Prepare(old,*registry,invocation,{},initial,error),"lifecycle old preparation");
        Check(bridge->DispatchPrepared(invocation,*registry,initial)==ScopeResult::Ok,"lifecycle old publication");
        Check(oldEndpoint->tasks->Release(task)==ScopeResult::Ok,"lifecycle old task cleanup");
        Check(registry->Activate("foreign-after", {&declared,1}, affected, error), "lifecycle foreign-after");
        auto oldDecision = RuntimeScopeTestAccess::Reserve(*oldScope,originalDecision);
        if (scenario==17) oldDecision=RuntimeScopeTestAccess::Issue(*oldScope,false,true,true,1,std::chrono::milliseconds(100));
        Check(oldDecision.owner != 0,"lifecycle current old decision");
        auto record = OwnerLifecycleTestAccess::Issue(old,registry,domain);
        Check(static_cast<bool>(record),"lifecycle record issuance");
        Check(!OwnerLifecycleTestAccess::Issue(old,std::make_shared<PatchRegistry>(),domain),"foreign registry accepted old key");
        Check(!OwnerLifecycleTestAccess::Issue(old,registry,std::make_shared<RuntimeCommitDomain>()),"foreign domain accepted old key");
        auto nextScope = std::shared_ptr<RuntimeScope>(RuntimeScopeTestAccess::Create(domain));
        nextScope->SetReadiness(true);
        auto nextBinding = host.Bind(nextScope); auto nextEndpoint = nextBinding.semantic;
        const auto nextDecision = RuntimeScopeTestAccess::Issue(*nextScope,false,true,true,1,
            scenario==22 ? std::chrono::milliseconds(100) : std::chrono::minutes(1));
        SemanticBridgeTestAccess::Issue(*nextEndpoint,nextDecision);
        definition.revision = "new-loaded"; definition.modules.emplace("new-module","new-content");
        auto nextVm = std::make_shared<ScriptRuntime>("fixture.lifecycle",definition,std::move(nextBinding)); nextVm->Commit();
        auto successorPatch = declared; successorPatch.properties.front().value = 9.0F;
        std::vector<ResourcePatchDefinition> replacements{successorPatch};
        if (scenario == 9) replacements.resize(4097, successorPatch);
        auto successor = OwnedPublicationTestAccess::Issue(nextVm,nextEndpoint,"new-source",std::move(replacements));
        copied::Envelope nextValue;
        Check(bridge->Values()->Decode(value.Encode(),"test.echo@1",nextEndpoint->storageOwner,nextValue)==copied::Status::Ok,"lifecycle next input");
        SemanticContextLease nextLease;
        Check(bridge->BindReferences(*nextEndpoint,nextValue,nextLease)==ScopeResult::Ok,"lifecycle candidate facts");
        Check(!nextScope->Current().activationCommitted,"candidate operational activation escaped preparation");
        ScopeHandle deniedCandidate;
        Check(nextScope->Queue(nextDecision,{},deniedCandidate)==ScopeResult::NotReady,"candidate queued ordinary work");
        const auto before = registry->Current(); const auto revision = domain->Revision();
        if (scenario == 15) ++nextLease.runtimeEpoch;
        struct Reclamation final {
            bool destroyed{}, outside{};
        };
        auto reclamation=std::make_shared<Reclamation>();
        if (scenario==0) {
            // @summary The actual retained dependency probes every relevant lock without blocking or dangling references.
            const std::weak_ptr<RuntimeScope> weakScope=oldScope;
            auto retained=std::shared_ptr<void>(new std::byte{},[reclamation,domain,registry,bridge,weakScope](void* pointer) {
                auto writer=domain->ReserveWriter();
                auto publication=registry->TryReservePublication();
                auto observation=SemanticBridgeTestAccess::ReserveObservation(*bridge);
                auto scope=weakScope.lock();
                auto state=scope ? RuntimeScopeTestAccess::ReserveState(*scope) : std::unique_lock<std::mutex>{};
                reclamation->outside=writer.Acquired() && publication.owns_lock() && observation.owns_lock() && state.owns_lock();
                reclamation->destroyed=true; delete static_cast<std::byte*>(pointer);
            });
            Check(oldScope->RetainResource(retained),"old dependency retention"); retained.reset();
        }
        OwnerLifecycle::Plan plan;
        if (scenario == 8) {
            Check(OwnerLifecycleTestAccess::Remove(record,successor,plan,error)==ScopeResult::Conflict,
                "same-ID candidate selected predecessor removal");
            Check(registry->Current()==before && !oldScope->Current().stopping,"foreign removal touched predecessor");
        } else if (scenario == 11 || scenario == 12 || scenario == 16) {
            Check(OwnerLifecycleTestAccess::Remove(record,old,plan,error)==ScopeResult::Ok,"release-only preparation");
            if (scenario == 11) Check(RuntimeScopeTestAccess::Revoke(*oldScope,oldDecision.grantGeneration)==ScopeResult::Ok,"release old revocation");
            if (scenario == 12) oldScope->RequestStop();
            if (scenario == 16) domain->RequestStop();
            const auto outcome = OwnerLifecycleTestAccess::Commit(record,plan);
            if (scenario == 16) {
                Check(outcome==ScopeResult::NotReady && registry->Current()==before,"global stop allowed maintenance bypass");
            } else {
                Check(outcome==ScopeResult::Ok && !registry->IsActive("fixture.lifecycle") && oldScope->Current().stopping,
                    "revoked owner release-only cleanup failed");
                Check(OwnerLifecycleTestAccess::Commit(record,plan)==ScopeResult::Conflict,"removal plan replay");
                std::shared_ptr<PublicationOrigin> current; std::uint64_t stamp{};
                Check(OwnerLifecycleTestAccess::Inspect(record,current,stamp)==ScopeResult::Ok && !current && stamp==2,
                    "removal record stamp mismatch");
                const auto removed = registry->Current(); const auto removedRevision=domain->Revision();
                OwnerLifecycle::Plan again;
                Check(OwnerLifecycleTestAccess::Remove(record,old,again,error)==ScopeResult::Ok &&
                    OwnerLifecycleTestAccess::Commit(record,again)==ScopeResult::Ok && registry->Current()==removed &&
                    domain->Revision()==removedRevision,"absent exact removal was not idempotent");
                Check(registry->IsActive("foreign-before") && registry->IsActive("foreign-after"),"removal changed other owners");
            }
        } else {
            const auto prepared=OwnerLifecycleTestAccess::Handoff(record,old,successor,oldDecision,nextDecision,
                oldLease,nextLease,plan,error);
            if (scenario == 9) {
                Check(prepared==ScopeResult::Busy && registry->Current()==before && !oldScope->Current().stopping,
                    "candidate capacity failure changed predecessor");
            } else {
                Check(prepared==ScopeResult::Ok,"handoff preparation");
                OwnerLifecycle::Plan delayedOldRemoval;
                if (scenario==0) Check(OwnerLifecycleTestAccess::Remove(record,old,delayedOldRemoval,error)==ScopeResult::Ok,
                    "preprepared old removal");
                if (scenario == 1) RuntimeScopeTestAccess::Revoke(*oldScope,oldDecision.grantGeneration);
                if (scenario == 2) RuntimeScopeTestAccess::Revoke(*nextScope,nextDecision.grantGeneration);
                if (scenario == 3) SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::City,7,3);
                if (scenario == 4) Check(registry->Replace("foreign-after",{},affected,error),"stale-generation fixture");
                if (scenario == 6) domain->RequestStop();
                if (scenario == 10) Check(nextScope->CommitActivation(),"premature candidate fixture");
                if (scenario == 13) oldScope->RequestStop();
                std::vector<ScopeHandle> queued;
                const auto storageBaseline=bridge->Values()->Used(oldEndpoint->storageOwner);
                std::size_t retainedStorage{};
                if (scenario == 0) {
                    for (int i=0;i!=8;++i) {
                        ScopeHandle handle;
                        Check(oldScope->QueueValue(oldDecision,value,bridge->Values(),512,handle)==ScopeResult::Ok,
                            "old pending copied task fixture"); queued.push_back(handle);
                    }
                    Check(oldScope->BeginValue(queued.front())==ScopeResult::Ok,"old preparing task fixture");
                    retainedStorage=bridge->Values()->Used(oldEndpoint->storageOwner);
                    Check(retainedStorage>storageBaseline,"copied task buffers not actually retained");
                    ScopeHandle excess;
                    Check(oldScope->Queue(oldDecision,{},excess)==ScopeResult::Busy,"shared pending bound not reached");
                }
                if (scenario==17) std::this_thread::sleep_until(oldDecision.deadline+std::chrono::milliseconds(1));
                if (scenario==22) std::this_thread::sleep_until(nextDecision.deadline+std::chrono::milliseconds(1));
                ScopeResult outcome;
                if (scenario == 5) {
                    auto busy=domain->ReserveWriter();
                    Check(busy.Acquired(),"busy domain fixture");
                    outcome=OwnerLifecycleTestAccess::Commit(record,plan);
                } else if (scenario == 7) {
                    auto foreignRecord=OwnerLifecycleTestAccess::Issue(old,registry,domain);
                    outcome=OwnerLifecycleTestAccess::Commit(foreignRecord,plan);
                } else if (scenario==14) {
                    std::barrier start(3);
                    auto commit=std::async(std::launch::async,[&] { start.arrive_and_wait(); return OwnerLifecycleTestAccess::Commit(record,plan); });
                    auto revoke=std::async(std::launch::async,[&] { start.arrive_and_wait(); return RuntimeScopeTestAccess::Revoke(*oldScope,oldDecision.grantGeneration); });
                    start.arrive_and_wait(); outcome=commit.get(); const auto revoked=revoke.get();
                    Check((outcome==ScopeResult::Ok || outcome==ScopeResult::Denied || outcome==ScopeResult::Busy) &&
                        (revoked==ScopeResult::Ok || revoked==ScopeResult::Busy),"revocation/handoff race produced invalid outcome");
                    if (outcome!=ScopeResult::Ok) {
                        ScopeHandle denied;
                        Check(revoked==ScopeResult::Ok && oldScope->Queue(oldDecision,{},denied)!=ScopeResult::Ok,
                            "losing handoff revived revoked old authority");
                    }
                } else if (scenario==18 || scenario==19 || scenario==20 || scenario==21) {
                    std::promise<void> acquired, release;
                    auto released=release.get_future();
                    auto blocker=std::async(std::launch::async,[&] {
                        if (scenario==18) {
                            auto guard=registry->TryReservePublication(); const bool locked=guard.owns_lock();
                            acquired.set_value(); released.wait(); return locked;
                        }
                        if (scenario==19) {
                            auto guard=SemanticBridgeTestAccess::ReserveObservation(*bridge); const bool locked=guard.owns_lock();
                            acquired.set_value(); released.wait(); return locked;
                        }
                        auto guard=RuntimeScopeTestAccess::ReserveState(scenario==20 ? *oldScope : *nextScope);
                        const bool locked=guard.owns_lock(); acquired.set_value(); released.wait(); return locked;
                    });
                    acquired.get_future().wait();
                    outcome=OwnerLifecycleTestAccess::Commit(record,plan);
                    release.set_value(); Check(blocker.get() && outcome==ScopeResult::Busy,"contended lifecycle lock did not fail boundedly");
                } else outcome=OwnerLifecycleTestAccess::Commit(record,plan);
                if (scenario == 0 || (scenario == 14 && outcome==ScopeResult::Ok)) {
                    Check(outcome==ScopeResult::Ok && registry->Current()->generation()==before->generation()+1 &&
                        domain->Revision()==revision+1,"handoff did not publish exactly once");
                    std::shared_ptr<PublicationOrigin> current; std::uint64_t stamp{};
                    Check(OwnerLifecycleTestAccess::Inspect(record,current,stamp)==ScopeResult::Ok && current==successor && stamp==2,
                        "handoff registry and record stamp diverged");
                    Check(oldScope->Current().stopping && nextScope->Current().activationCommitted,"handoff owner admission mismatch");
                    if (scenario==0) Check(bridge->Values()->Used(oldEndpoint->storageOwner)==retainedStorage,
                        "retirement released copied task buffers under fences");
                    for (const auto& handle:queued) Check(oldScope->Release(handle.owner,handle)==ScopeResult::Ok,"retired queued task not releasable");
                    if (scenario==0) Check(bridge->Values()->Used(oldEndpoint->storageOwner)==storageBaseline,
                        "explicit retired task cleanup leaked copied envelopes or reserved buffers");
                    Check(OwnerLifecycleTestAccess::Commit(record,plan)==ScopeResult::Conflict,"handoff replay");
                    Check(!OwnedPublicationTestAccess::Transition(*old,PublicationOrigin::Phase::Active),"predecessor revived after handoff");
                    const auto* patches=registry->Current()->Find(declared.target);
                    Check(patches && patches->size()==3 && patches->at(0).owner=="foreign-before" &&
                        patches->at(1).owner=="fixture.lifecycle" && patches->at(1).patch.properties.front().value==9.0F &&
                        patches->at(2).owner=="foreign-after","handoff changed order or foreign owners");
                    OwnerLifecycle::Plan staleCleanup;
                    Check(OwnerLifecycleTestAccess::Remove(record,old,staleCleanup,error)==ScopeResult::Conflict &&
                        registry->IsActive("fixture.lifecycle"),"delayed old cleanup removed successor");
                    if (scenario==0) Check(OwnerLifecycleTestAccess::Commit(record,delayedOldRemoval)==ScopeResult::Conflict,
                        "previously prepared old removal erased committed successor");
                    const auto newFresh = RuntimeScopeTestAccess::Reserve(*nextScope,nextDecision);
                    Check(newFresh.owner!=0,"successor current decision inaccessible");
                    std::vector<ScopeHandle> successorTasks;
                    for (int i=0;i!=8;++i) {
                        ScopeHandle handle;
                        Check(nextScope->Queue(newFresh,{},handle)==ScopeResult::Ok,"retirement leaked shared pending capacity");
                        successorTasks.push_back(handle);
                    }
                    for (const auto& handle:successorTasks) Check(nextScope->Cancel(newFresh,handle)==ScopeResult::Ok &&
                        nextScope->Release(handle.owner,handle)==ScopeResult::Ok,"successor task cleanup");
                    if (scenario==0) {
                        Check(!reclamation->destroyed,"retirement reclaimed dependency under publication guard");
                        Check(oldScope->DrainFor(std::chrono::milliseconds(0)) && reclamation->destroyed && reclamation->outside,
                            "retired dependency destructor ran under a lifecycle fence");
                    }
                } else {
                    Check(outcome!=ScopeResult::Ok,"invalid handoff published");
                    if (scenario!=4) Check(registry->Current()==before,"failed handoff changed registry");
                    std::shared_ptr<PublicationOrigin> retained; std::uint64_t retainedStamp{};
                    Check(OwnerLifecycleTestAccess::Inspect(record,retained,retainedStamp)==ScopeResult::Ok && retained==old &&
                        retainedStamp==1,"failed handoff changed authoritative record");
                    if (scenario!=10) Check(!nextScope->Current().activationCommitted,"failed handoff exposed candidate");
                    if (scenario==1) {
                        ScopeHandle stale;
                        Check(oldScope->Queue(oldDecision,{},stale)!=ScopeResult::Ok,"failed handoff resurrected revoked decision");
                        OwnerLifecycle::Plan cleanup;
                        Check(OwnerLifecycleTestAccess::Remove(record,old,cleanup,error)==ScopeResult::Ok &&
                            OwnerLifecycleTestAccess::Commit(record,cleanup)==ScopeResult::Ok && !registry->IsActive("fixture.lifecycle"),
                            "revocation denied narrow cleanup after failed handoff");
                    } else if (scenario!=6 && scenario!=13) Check(!oldScope->Current().stopping,"failed candidate retired old owner");
                }
            }
        }
        Check(calls==0,"lifecycle called operational effect callback");
        oldVm->Stop(); nextVm->Stop();
    }
}

/** @summary Exercise prepared registry publication through the original retained native task.
 * @param package Actual pinned Base package. */
void PreparedRegistryPublications(const std::filesystem::path& package) {
    using Registry = sc13::mods::PatchRegistry;
    for (const int scenario : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 10, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19}) {
        int forbiddenCalls{};
        auto bridge = std::make_shared<SemanticBridge>();
        auto method = Echo(forbiddenCalls, "city");
        method.maxResultBytes = scenario == 8 ? 64 : 512; method.registryPublication = scenario != 9;
        SemanticBridgeTestAccess::Configure(*bridge, {std::move(method)});
        SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
        SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
        BaseHost host(package, Pin(package), {}, bridge);
        auto domain = std::make_shared<RuntimeCommitDomain>();
        auto scope = Scope(domain); auto binding = host.Bind(scope); auto endpoint = binding.semantic;
        const auto issued = RuntimeScopeTestAccess::Issue(*scope);
        SemanticBridgeTestAccess::Issue(*endpoint, issued);
        ScriptDefinition definition; definition.entry = "scripts/init.luau";
        definition.bytecode = CompileLuauSource("return {}");
        definition.language="luau";
        definition.resourceAliases.emplace("fixture.resource",sc13::core::Tgi{1,2,3});
        sc13::mods::PropertyPatch alias; alias.propertyId=4; alias.value=1.0F;
        definition.propertyAliases.emplace("fixture.property",alias);
        definition.modules.emplace("fixture", "immutable-module"); definition.revision="loaded-revision";
        auto mod = std::make_shared<ScriptRuntime>("fixture.registry-owner", definition, std::move(binding));
        auto owner = sc13::mods::OwnedPublicationTestAccess::Issue(mod, endpoint, "fixture-source-correlation");
        Check(!sc13::mods::OwnedPublicationTestAccess::Transition(*owner, sc13::mods::PublicationOrigin::Phase::Active), "uncommitted VM became active owner");
        mod->Commit();
        if (scenario != 16) Check(sc13::mods::OwnedPublicationTestAccess::Transition(*owner, sc13::mods::PublicationOrigin::Phase::Active), "owner activation failed");
        definition.modules["fixture"]="changed-disk-successor"; definition.revision="pending-revision";
        definition.entry="changed-entry"; definition.language="changed-language";
        definition.bytecode=CompileLuauSource("return {changed=true}"); definition.memoryBytes=10U*1024U*1024U; definition.budgetMs=3;
        definition.resourceAliases["fixture.resource"]={9,9,9}; definition.propertyAliases["fixture.property"].propertyId=99;
        const auto loaded = sc13::mods::OwnedPublicationTestAccess::Snapshot(*owner);
        Check(loaded.revision=="loaded-revision" && loaded.modules.at("fixture")=="immutable-module", "source mutation changed loaded origin snapshot");
        Check(loaded.entry=="scripts/init.luau" && loaded.language=="luau" && loaded.bytecode!=definition.bytecode &&
            loaded.memoryBytes==8U*1024U*1024U && loaded.budgetMs==2 &&
            loaded.resourceAliases.at("fixture.resource")==sc13::core::Tgi{1,2,3} &&
            loaded.propertyAliases.at("fixture.property").propertyId==4,"aliases or execution limits escaped immutable loaded pin");
        if (scenario==19) {
            bool rejected{};
            auto foreignScope=Scope(domain); auto foreignBinding=host.Bind(foreignScope); auto foreignEndpoint=foreignBinding.semantic;
            SemanticBridgeTestAccess::Issue(*foreignEndpoint,RuntimeScopeTestAccess::Issue(*foreignScope));
            auto foreignVm=std::make_shared<ScriptRuntime>("fixture.registry-owner",definition,std::move(foreignBinding)); foreignVm->Commit();
            try {
                sc13::mods::OwnedPublicationTestAccess::Issue(foreignVm,endpoint,"same-source-text"); }
            catch(const std::invalid_argument&) { rejected=true; }
            Check(rejected,"same-ID foreign VM selected another endpoint");
            auto failedCandidate=sc13::mods::OwnedPublicationTestAccess::Issue(foreignVm,foreignEndpoint,"failed-successor");
            Check(sc13::mods::OwnedPublicationTestAccess::Transition(*failedCandidate,sc13::mods::PublicationOrigin::Phase::Retired),"candidate retirement");
            Check(!sc13::mods::OwnedPublicationTestAccess::Transition(*failedCandidate,sc13::mods::PublicationOrigin::Phase::Active),"failed candidate revival");
            Check(sc13::mods::OwnedPublicationTestAccess::Transition(*owner,sc13::mods::PublicationOrigin::Phase::Active),"failed candidate retired original owner");
            foreignVm->Stop();
        }
        copied::Envelope value;
        Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["null"]})",
            "test.echo@1", endpoint->storageOwner, value) == copied::Status::Ok, "registry input codec failed");
        Registry registry, foreign;
        sc13::mods::OwnedPublication plan;
        auto& sourceRegistry = scenario == 4 ? foreign : registry;
        sc13::mods::ResourcePatchDefinition patch;
        patch.target = {1, 2, 3};
        patch.properties.push_back({4, sc13::mods::PropertyType::Float, sc13::mods::PatchOperation::Set, 5.0F});
        std::string error;
        TaskHandle task; SemanticContextLease context;
        copied::Reservation pressure;
        if (scenario == 11) {
            const auto used = bridge->Values()->Used(endpoint->storageOwner);
            Check(bridge->Values()->Account(endpoint->storageOwner, copied::Storage::OwnerLimit - used, pressure)
                == copied::Status::Ok, "pre-admission quota fixture failed");
            Check(bridge->Request(endpoint, "fixture.echo", value, task, context) == ScopeResult::Busy && !task
                && registry.Current()->generation() == 0 && domain->Revision() == 0 && forbiddenCalls == 0,
                "exhausted result allocation admitted task or changed registry");
            mod->Stop(); continue;
        }
        Check(bridge->Request(endpoint, "fixture.echo", value, task, context) == ScopeResult::Ok, "registry request failed");
        SemanticInvocation invocation; Check(bridge->Take(invocation), "registry invocation missing");
        Check(sc13::mods::OwnedPublicationTestAccess::Prepare(owner, sourceRegistry, invocation,
            {{patch.target,4,5.0F}},plan,error), "owned registry preparation failed");
        if(scenario==17) {
            Check(sc13::mods::OwnedPublicationTestAccess::Transition(*owner, sc13::mods::PublicationOrigin::Phase::Retired),"owner retirement failed");
            Check(!sc13::mods::OwnedPublicationTestAccess::Transition(*owner, sc13::mods::PublicationOrigin::Phase::Active),"retired incarnation revived");
        }
        if(scenario==18) ++invocation.context.operation;
        if(scenario==19) ++invocation.method;
        if (scenario == 12) {
            const auto used = bridge->Values()->Used(endpoint->storageOwner);
            Check(bridge->Values()->Account(endpoint->storageOwner, copied::Storage::OwnerLimit - used, pressure)
                == copied::Status::Ok, "post-admission quota fixture failed");
        }
        if (scenario == 0) Check(endpoint->tasks->Cancel(task) == ScopeResult::Ok, "registry cancellation failed");
        if (scenario == 1) RuntimeScopeTestAccess::Revoke(*scope, issued.grantGeneration);
        if (scenario == 2) SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
        if (scenario == 3) bridge->Close();
        if (scenario == 5) {
            std::set<sc13::core::Tgi> affected;
            Check(registry.Activate("legacy-writer", {}, affected, error), "legacy registry publication failed");
        }
        if (scenario == 7) {
            auto writer = domain->ReserveWriter();
            Check(writer.Acquired() && writer.Advance() == 1, "competing domain writer failed");
        }
        if (scenario == 13) scope->RequestStop();
        if (scenario == 15) endpoint->scope = Scope();
        const auto before = registry.Current();
        ScopeResult result;
        if (scenario == 10) {
            std::barrier start(2);
            auto invalidator = std::async(std::launch::async, [&] {
                start.arrive_and_wait();
                SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
            });
            start.arrive_and_wait();
            result = bridge->DispatchPrepared(invocation, registry, plan);
            invalidator.get();
        } else if (scenario == 14) {
            auto heldWriter = domain->ReserveWriter();
            Check(heldWriter.Acquired(), "held domain fixture could not reserve writer");
            result = bridge->DispatchPrepared(invocation, registry, plan);
        } else result = bridge->DispatchPrepared(invocation, registry, plan);
        Check(forbiddenCalls == 0, "typed registry publication invoked arbitrary descriptor callback");
        if (scenario != 6 && scenario != 12 && !(scenario == 10 && result == ScopeResult::Ok)) {
            Check(result != ScopeResult::Ok && registry.Current() == before && domain->Revision() == (scenario == 7 ? 1 : 0),
                "cancelled, revoked, stale or foreign publication changed registry/domain");
            if (scenario == 1) Check(result == ScopeResult::Denied, "revocation lost explicit denial");
            if (scenario == 2) Check(result == ScopeResult::Stale, "context ABA lost stale result");
            if (scenario == 3) Check(result == ScopeResult::Stale || result == ScopeResult::NotReady,
                "retired bridge admitted prepared publication");
            if (scenario == 4 || scenario == 5 || scenario == 7) Check(result == ScopeResult::Conflict, "registry identity/revision lost conflict");
            if (scenario == 8) Check(result == ScopeResult::BufferSmall, "receipt capacity was not rejected before publication");
            if (scenario == 9) Check(result == ScopeResult::Denied, "unreviewed descriptor reached typed registry commit");
            if (scenario == 10) Check(result == ScopeResult::Busy || result == ScopeResult::Stale,
                "context contention produced an unclassified outcome");
            if (scenario == 13) Check(result == ScopeResult::Stale || result == ScopeResult::NotReady,
                "stopped scope admitted prepared publication");
            if (scenario == 14) Check(result == ScopeResult::Busy, "held domain did not fail nonblocking admission");
            if (scenario == 15) Check(result == ScopeResult::Denied, "foreign scope writer bypassed original task domain");
            if (scenario == 16 || scenario == 17) Check(result == ScopeResult::NotReady,"candidate/retired owner gained operational publication");
            if (scenario == 18 || scenario == 19) Check(result == ScopeResult::Conflict,"prepared owner accepted altered original request");
            Check(foreign.Current()->generation() == 0, "wrong registry consumed foreign candidate");
            TaskSnapshot rejected;
            const auto delivery = endpoint->tasks->Poll(task, rejected);
            if (delivery == ScopeResult::Ok) {
                Check(!rejected.committed && !rejected.value
                    && (rejected.state == TaskState::Failed || rejected.state == TaskState::Cancelled),
                    "rejected publication retained an orphan or disclosed a receipt");
            } else Check((scenario == 1 && delivery == ScopeResult::Denied)
                || ((scenario == 3 || scenario == 13) && (delivery == ScopeResult::Stale || delivery == ScopeResult::NotReady)),
                "rejected original task lost owner delivery semantics");
        } else {
            const auto published = registry.Current();
            Check(result == ScopeResult::Ok && published->generation() == 1 && published->domain_revision() == 1
                && domain->Revision() == 1 && registry.IsActive("fixture.registry-owner"), "registry/domain publication mismatch");
            TaskSnapshot outcome;
            Check(endpoint->tasks->Poll(task, outcome) == ScopeResult::Ok && outcome.committed
                && outcome.state == TaskState::Succeeded && outcome.value, "original task lost successful publication receipt");
            const auto wire = outcome.value.Encode();
            bool exactRegistry{}, exactDomain{};
            const auto* root = outcome.value.At(0);
            Check(root && root->tag == copied::Tag::Map, "publication receipt is not a copied map");
            auto child = root->first;
            for (std::uint32_t index = 0; index < root->count; ++index) {
                const auto* node = outcome.value.At(child);
                Check(node != nullptr, "publication receipt child missing");
                if (outcome.value.Key(*node) == "registryGeneration") exactRegistry = node->tag == copied::Tag::U64 && node->bits == 1;
                if (outcome.value.Key(*node) == "domainRevision") exactDomain = node->tag == copied::Tag::U64 && node->bits == 1;
                child = node->next;
            }
            Check(outcome.value.Schema() == "simoder.registry-publication.v1"
                && exactRegistry && exactDomain
                && wire.find("registry-committed") != std::string::npos && wire.find("not-requested") != std::string::npos,
                "registry receipt omitted exact revisions or fabricated live verification");
            Check(endpoint->tasks->Cancel(task) == ScopeResult::Busy, "committed cancellation claimed undo");
            if (scenario == 6) {
                auto freshScope = Scope(domain);
                auto freshBinding = host.Bind(freshScope); auto freshEndpoint = freshBinding.semantic;
                SemanticBridgeTestAccess::Issue(*freshEndpoint, RuntimeScopeTestAccess::Issue(*freshScope));
                ScriptRuntime freshMod("fixture.consumed-plan", definition, std::move(freshBinding)); freshMod.Commit();
                copied::Envelope freshValue;
                Check(bridge->Values()->Decode(value.Encode(), "test.echo@1", freshEndpoint->storageOwner, freshValue)
                    == copied::Status::Ok, "fresh candidate task value copy failed");
                TaskHandle replayTask; SemanticContextLease replayContext;
                Check(bridge->Request(freshEndpoint, "fixture.echo", freshValue, replayTask, replayContext) == ScopeResult::Ok,
                    "fresh original task could not test consumed candidate");
                SemanticInvocation replay; Check(bridge->Take(replay), "fresh consumed candidate invocation missing");
                Check(bridge->DispatchPrepared(replay, registry, plan) == ScopeResult::Conflict
                    && registry.Current() == published && domain->Revision() == 1,
                    "consumed candidate published through a fresh task");
                Check(freshEndpoint->tasks->Release(replayTask) == ScopeResult::Ok, "consumed candidate task cleanup failed");
            }
            SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
            Check(bridge->DispatchPrepared(invocation, registry, plan) != ScopeResult::Ok
                && registry.Current() == published, "completed task replayed registry publication");
            RuntimeScopeTestAccess::Revoke(*scope, issued.grantGeneration);
            Check(endpoint->tasks->Poll(task, outcome) == ScopeResult::Denied && !outcome.value
                && registry.Current() == published, "revocation disclosed receipt or undid immutable outcome");
        }
        Check(endpoint->tasks->Release(task) == ScopeResult::Ok, "registry owner cleanup failed");
        mod->Stop();
    }
}
/** @summary Successive owner-local reductions preserve declarative/foreign patches and reject stale competing baselines.
 * @param package Actual pinned Base package. */
void OwnedBaselineSequence(const std::filesystem::path& package) {
    using namespace sc13::mods;
    int calls{}; auto bridge=std::make_shared<SemanticBridge>();
    auto method=Echo(calls); method.registryPublication=true;
    SemanticBridgeTestAccess::Configure(*bridge,{std::move(method)});
    SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::Runtime,1,1);
    BaseHost host(package,Pin(package),{},bridge);
    auto scope=Scope(); auto binding=host.Bind(scope); auto endpoint=binding.semantic;
    SemanticBridgeTestAccess::Issue(*endpoint,RuntimeScopeTestAccess::Issue(*scope));
    ScriptDefinition definition; definition.bytecode=CompileLuauSource("return {}");
    {
        const auto before=bridge->Values()->Used(endpoint->storageOwner);
        auto oversized=definition; oversized.bytecode.assign(copied::Storage::OwnerLimit,'x');
        bool rejected{};
        try {ScriptRuntime excessive("fixture.owner",oversized,binding);}
        catch(const std::runtime_error&) {rejected=true;}
        Check(rejected && endpoint->generation==0 && bridge->Values()->Used(endpoint->storageOwner)==before,
            "failed semantic snapshot leaked quota or bound a VM generation");
    }
    auto vm=std::make_shared<ScriptRuntime>("fixture.owner",definition,std::move(binding)); vm->Commit();
    ResourcePatchDefinition declared; declared.target={1,2,3};
    declared.properties.push_back({90,PropertyType::Float,PatchOperation::Set,9.0F});
    auto owner=OwnedPublicationTestAccess::Issue(vm,endpoint,"correlation",{declared});
    Check(OwnedPublicationTestAccess::Transition(*owner,PublicationOrigin::Phase::Active),"sequence owner activation");
    PatchRegistry registry; std::set<sc13::core::Tgi> affected; std::string error;
    Check(registry.Activate("foreign-owner",{&declared,1},affected,error),"foreign baseline setup");
    copied::Envelope value;
    Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["null"]})","test.echo@1",endpoint->storageOwner,value)==copied::Status::Ok,"sequence input");
    const auto request=[&] {
        TaskHandle task; SemanticContextLease context;
        Check(bridge->Request(endpoint,"fixture.echo",value,task,context)==ScopeResult::Ok,"sequence admission");
        SemanticInvocation invocation; Check(bridge->Take(invocation),"sequence dequeue"); return invocation;
    };
    /** @summary Fixture-only refresh to the next original-scope revision; never changes VM or scope identity. */
    const auto refresh=[&] {endpoint->context=RuntimeScopeTestAccess::Issue(*scope);};
    {
        // @summary A valid same-ID VM on a different writer domain cannot lend its owned plan to this endpoint.
        auto otherScope=Scope(); auto otherBinding=host.Bind(otherScope); auto otherEndpoint=otherBinding.semantic;
        SemanticBridgeTestAccess::Issue(*otherEndpoint,RuntimeScopeTestAccess::Issue(*otherScope));
        auto otherVm=std::make_shared<ScriptRuntime>("fixture.owner",definition,std::move(otherBinding)); otherVm->Commit();
        auto otherOwner=OwnedPublicationTestAccess::Issue(otherVm,otherEndpoint,"same-description");
        Check(OwnedPublicationTestAccess::Transition(*otherOwner,PublicationOrigin::Phase::Active),"foreign real owner activation");
        copied::Envelope otherValue;
        Check(bridge->Values()->Decode(value.Encode(),"test.echo@1",otherEndpoint->storageOwner,otherValue)==copied::Status::Ok,"foreign owned input");
        TaskHandle otherTask; SemanticContextLease otherContext;
        Check(bridge->Request(otherEndpoint,"fixture.echo",otherValue,otherTask,otherContext)==ScopeResult::Ok,"foreign owned admission");
        SemanticInvocation otherInvocation; Check(bridge->Take(otherInvocation),"foreign owned dequeue");
        OwnedPublication otherPlan;
        Check(OwnedPublicationTestAccess::Prepare(otherOwner,registry,otherInvocation,{{declared.target,4,7.0F}},otherPlan,error),"foreign owned preparation");
        auto original=request();
        Check(bridge->DispatchPrepared(original,registry,otherPlan)==ScopeResult::Conflict,"foreign owned plan crossed native endpoint/domain");
        Check(registry.Current()->generation()==1,"foreign owned plan changed registry");
        Check(endpoint->tasks->Release(original.task)==ScopeResult::Ok,"foreign rejection cleanup");
        Check(otherEndpoint->tasks->Cancel(otherTask)==ScopeResult::Ok && otherEndpoint->tasks->Release(otherTask)==ScopeResult::Ok,"foreign task cleanup");
        otherVm->Stop();
    }
    // @summary No task is recreated for preparation or publication; each retained request completes once.
    auto first=request(); OwnedPublication firstPlan;
    Check(OwnedPublicationTestAccess::Prepare(owner,registry,first,{{declared.target,4,1.0F}},firstPlan,error),"first owner preparation");
    Check(bridge->DispatchPrepared(first,registry,firstPlan)==ScopeResult::Ok,"first owner publication");
    Check(endpoint->tasks->Release(first.task)==ScopeResult::Ok,"first task cleanup");
    {
        PatchRegistry::PreparedPublication raw;
        Check(!registry.Prepare(PatchRegistry::ChangeKind::Replace,"fixture.owner",{},raw,error),
            "legacy raw plan adopted a sealed owner incarnation");
        auto otherScope=Scope(); auto otherBinding=host.Bind(otherScope); auto otherEndpoint=otherBinding.semantic;
        SemanticBridgeTestAccess::Issue(*otherEndpoint,RuntimeScopeTestAccess::Issue(*otherScope));
        auto otherVm=std::make_shared<ScriptRuntime>("fixture.owner",definition,std::move(otherBinding)); otherVm->Commit();
        auto otherOwner=OwnedPublicationTestAccess::Issue(otherVm,otherEndpoint,"same-source-and-ID");
        Check(OwnedPublicationTestAccess::Transition(*otherOwner,PublicationOrigin::Phase::Active),"second legitimate same-ID owner activation");
        copied::Envelope otherValue;
        Check(bridge->Values()->Decode(value.Encode(),"test.echo@1",otherEndpoint->storageOwner,otherValue)==copied::Status::Ok,"second same-ID input");
        TaskHandle otherTask; SemanticContextLease context;
        Check(bridge->Request(otherEndpoint,"fixture.echo",otherValue,otherTask,context)==ScopeResult::Ok,"second same-ID admission");
        SemanticInvocation otherInvocation; Check(bridge->Take(otherInvocation),"second same-ID dequeue");
        OwnedPublication conflicting;
        const auto before=registry.Current();
        Check(!OwnedPublicationTestAccess::Prepare(otherOwner,registry,otherInvocation,{{declared.target,4,std::nullopt}},conflicting,error)
            && registry.Current()==before,"legitimate same-ID runtime cleared another incarnation's state");
        Check(otherEndpoint->tasks->Cancel(otherTask)==ScopeResult::Ok && otherEndpoint->tasks->Release(otherTask)==ScopeResult::Ok,"second owner cleanup");
        otherVm->Stop();
        PatchRegistry legacy;
        Check(legacy.Activate("fixture.owner",{},affected,error),"legacy owner setup");
        OwnedPublication adoption;
        Check(!OwnedPublicationTestAccess::Prepare(owner,legacy,first,{{declared.target,5,1.0F}},adoption,error),
            "owned origin adopted an unsealed legacy owner");
    }
    // @summary Grants retain the original shared revision; fixture reissuance models the next reviewed operation only.
    refresh();
    auto second=request(); OwnedPublication secondPlan;
    Check(OwnedPublicationTestAccess::Prepare(owner,registry,second,{{declared.target,5,2.0F}},secondPlan,error),"second owner preparation");
    auto stale=request(); OwnedPublication stalePlan;
    Check(OwnedPublicationTestAccess::Prepare(owner,registry,stale,{{declared.target,6,3.0F}},stalePlan,error),"competing owner preparation");
    Check(bridge->DispatchPrepared(second,registry,secondPlan)==ScopeResult::Ok,"second owner publication");
    Check(bridge->DispatchPrepared(stale,registry,stalePlan)==ScopeResult::Conflict,"stale owner baseline publication");
    Check(endpoint->tasks->Release(second.task)==ScopeResult::Ok && endpoint->tasks->Release(stale.task)==ScopeResult::Ok,"competing task cleanup");
    refresh();
    auto third=request(); OwnedPublication thirdPlan;
    Check(OwnedPublicationTestAccess::Prepare(owner,registry,third,{{declared.target,4,std::nullopt}},thirdPlan,error),"clear preparation");
    Check(bridge->DispatchPrepared(third,registry,thirdPlan)==ScopeResult::Ok,"clear publication");
    const auto* patches=registry.Current()->Find(declared.target);
    bool foreign{},declarative{},kept{},cleared=true,failedAbsent=true;
    Check(patches!=nullptr,"owner patch sequence missing");
    for(const auto& patch:*patches) for(const auto& field:patch.patch.properties) {
        if(patch.owner=="foreign-owner") foreign=true;
        if(patch.owner=="fixture.owner") {
            if(field.propertyId==90) declarative=true;
            if(field.propertyId==5) kept=true;
            if(field.propertyId==4) cleared=false;
            if(field.propertyId==6) failedAbsent=false;
        }
    }
    Check(foreign&&declarative&&kept&&cleared&&failedAbsent,"owner clear lost foreign/declarative/previous state or applied failed baseline");
    Check(endpoint->tasks->Release(third.task)==ScopeResult::Ok,"clear task cleanup"); vm->Stop();
}
/** @summary Exercise the actual Lua subscription interface including current-state ordering, busy cancellation and overflow. @param package Actual package. */
void LuaStreams(const std::filesystem::path& package) {
    auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    auto ordered = Mod(host, Scope(), R"lua(
        local started, events, finished = false, {}, false
        return {onTick = function()
            if not started then
                started = true
                assert(base.subscribe(function(record, failure)
                    assert(record and failure == nil)
                    table.insert(events, record.contexts.city.slot)
                end))
                assert(#events == 0)
            elseif #events == 3 and not finished then
                assert(table.concat(events, ',') == '0,10,11')
                print('STREAM_ORDER_COMPLETE'); finished = true
            end
        end}
    )lua");
    ordered->Tick(0);
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 10, 1);
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 11, 1);
    Turns(host, {ordered.get()}); Marker(*ordered, "STREAM_ORDER_COMPLETE"); ordered->Stop();
    auto busy = Mod(host, Scope(), R"lua(
        local started, subscription, calls = false, nil, 0
        return {onTick = function()
            if not started then
                started = true
                subscription = assert(base.subscribe(function(record)
                    assert(record)
                    calls += 1
                    assert(subscription.unsubscribe() == 'Busy')
                end))
            elseif calls == 1 then
                assert(subscription.unsubscribe() == 'Stale')
                print('UNSUBSCRIBE_BUSY_COMPLETE')
            else assert(calls == 0) end
        end}
    )lua");
    busy->Tick(0); busy->Tick(0);
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 12, 1);
    busy->Tick(0); Marker(*busy, "UNSUBSCRIBE_BUSY_COMPLETE"); busy->Stop();
    auto overflow = Mod(host, Scope(), R"lua(
        local started, faulted = false, false
        return {onTick = function()
            if not started then
                started = true
                base.subscribe(function(record, failure)
                    assert(record == nil and failure == 'MailboxOverflow')
                    assert(not faulted); faulted = true
                end)
            elseif faulted then print('STREAM_OVERFLOW_COMPLETE') end
        end}
    )lua");
    overflow->Tick(0);
    for (int index = 0; index != 129; ++index)
        SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 12, 1);
    overflow->Tick(0); Marker(*overflow, "STREAM_OVERFLOW_COMPLETE"); overflow->Tick(0); overflow->Stop();
}
/** @summary Keep runtime-only leases valid across city transitions while stale decoded references never rebind. @param package Actual package. */
void ReferenceAndRuntimeContexts(const std::filesystem::path& package) {
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
    BaseHost host(package, Pin(package), {}, bridge);
    auto stale = Mod(host, Scope(), R"lua(
        local value = assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]}', 'test.echo@1'))
        local started = false
        return {onTick = function()
            if started then return end
            started = true
            local task, reason = base.request('fixture.echo', value)
            assert(task == nil and reason == 'Stale')
            print('REFERENCE_ABA_COMPLETE')
        end}
    )lua");
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
    stale->Tick(0); Marker(*stale, "REFERENCE_ABA_COMPLETE"); stale->Stop(); Check(calls == 0, "decoded stale reference admitted an effect");
    auto runtime = Mod(host, Scope(), R"lua(
        local started, task = false, nil
        return {onTick = function()
            if not started then
                started = true
                local value = assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["null"]}', 'test.echo@1'))
                task = assert(base.request('fixture.echo', value))
            elseif task.poll().state == 'succeeded' then print('RUNTIME_CONTEXT_COMPLETE') end
        end}
    )lua");
    runtime->Tick(0);
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 99, 4);
    host.Pump(); runtime->Tick(0); Marker(*runtime, "RUNTIME_CONTEXT_COMPLETE");
    Check(calls == 1, "runtime-only operation incorrectly expired with city context"); runtime->Stop();
}
/** @summary Stop suspended code and observers before dispatch without resuming an old generation. @param package Actual package. */
void StoppedContinuations(const std::filesystem::path& package) {
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    auto scope = Scope();
    auto mod = Mod(host, scope, R"lua(
        local started = false
        return {onTick = function()
            if started then return end
            started = true
            local value = assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["null"]}', 'test.echo@1'))
            local task = assert(base.request('fixture.echo', value))
            task.onCompleted(function() print('FORBIDDEN_CALLBACK') end)
            base.run(function()
                print('AWAIT_ENTERED')
                task.await()
                print('FORBIDDEN_RESUME')
            end)
        end}
    )lua");
    mod->Tick(0); mod->Tick(0);
    const auto entered = mod->DrainLogs();
    Check(mod->Status().running && std::any_of(entered.begin(), entered.end(), [](const auto& log) { return log.find("AWAIT_ENTERED") != std::string::npos; }),
        "host-registered coroutine did not enter await before stop");
    Check(std::none_of(entered.begin(), entered.end(), [](const auto& log) { return log.find("FORBIDDEN_") != std::string::npos; }),
        "pending coroutine or callback completed before native dispatch");
    mod->Stop();
    host.Pump(); mod->Tick(0);
    const auto logs = mod->DrainLogs();
    Check(std::none_of(logs.begin(), logs.end(), [](const auto& log) { return log.find("FORBIDDEN_") != std::string::npos; }),
        "stopped generation executed a continuation");
    Check(!mod->Status().running && calls == 0, "stopped queued task executed effects");
    mod.reset(); Check(bridge->Values()->Used(scope->Current().generation) == 0, "stopped generation retained native value storage");
}
/** @summary One finite real selected-lane pause, with observable expiry and exit before native continuation. */
struct ReloadGate final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{},released{},exited{},expired{};
    /** @summary Pause outside L/V; expiry is recorded and never treated as successful test progress. */
    void Hold() {
        std::unique_lock lock(mutex);entered=true;changed.notify_all();
        expired=!changed.wait_for(lock,std::chrono::seconds(5),[&]{return released;});
        exited=true;changed.notify_all();
    }
    /** @summary Observe a genuine bounded entry. @return Whether actual callback entered before the deadline. */
    bool Entered() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(2),[&]{return entered;});
    }
    /** @summary Release before any synchronous lane join or owning-object destruction. */
    void Release() noexcept {
        {const std::lock_guard lock(mutex);released=true;}changed.notify_all();
    }
    /** @summary Observe whether the actual callback is still holding its cut. @return Live entered pause. */
    bool Held() {const std::lock_guard lock(mutex);return entered&&!exited&&!expired;}
    /** @summary Observe a completed explicit release. @return Exit without timeout. */
    bool CleanExit() {const std::lock_guard lock(mutex);return exited&&!expired&&released;}
};
/** @summary Last-declared unwind guard releases every callback-owned gate before any native join. */
struct ReloadRelease final {
    std::shared_ptr<ReloadGate> first,await,next;
    /** @summary No callback-held shared owner or gate destructor is relied on to release a wait. */
    ~ReloadRelease(){first->Release();await->Release();next->Release();}
};
/** @summary Actual native lifecycle callbacks, retained independently of the fixture's stack lifetime. */
struct ReloadPhases final {std::atomic_uint disable{},close{};};
/** @summary One real VM and its constructor-bound native incarnation; no mutable replacement authority. */
struct ReloadCandidate final {
    std::shared_ptr<RuntimeScope> scope;
    std::shared_ptr<SemanticEndpoint> endpoint;
    std::shared_ptr<ReloadPhases> phases;
    std::shared_ptr<ScriptRuntime> vm;
    std::shared_ptr<sc13::mods::PublicationOrigin> origin;
    OperationContext root;
    SemanticContextLease facts;
};
/** @summary Verified same-Base host with an actual enrolled joint scheduler and isolated original domain. */
struct ReloadFixture final {
    int calls{};
    std::shared_ptr<RuntimeCommitDomain> domain=std::make_shared<RuntimeCommitDomain>();
    std::shared_ptr<sc13::mods::PatchRegistry> registry=std::make_shared<sc13::mods::PatchRegistry>();
    std::shared_ptr<SemanticBridge> bridge=std::make_shared<SemanticBridge>();
    std::unique_ptr<BaseHost> base;
    ScriptScheduler scheduler;
    /** @summary Configure only the existing fixture descriptor and bind a real Base. @param package Verified package. */
    explicit ReloadFixture(const std::filesystem::path& package) {
        SemanticBridgeTestAccess::Configure(*bridge,{Echo(calls)});
        SemanticBridgeTestAccess::ObserveContext(*bridge,SemanticContextKind::Runtime,1,1);
        base=std::make_unique<BaseHost>(package,Pin(package),std::function<void(ScriptPhase,std::uint64_t)>{},bridge);
        Check(base->Ready(),"reload actual Base readiness");
        Check(SchedulerPublicationTestAccess::Enroll(scheduler,domain)==ScopeResult::Ok,"reload actual scheduler enrollment");
    }
    /** @summary Join original native callbacks before Base/captured effect counters are destroyed. */
    ~ReloadFixture(){scheduler.Shutdown();}
    /** @summary Construct an inactive actual VM; the only grant issue happens before VM binding.
     * @param source Authored Luau. @param lifetime Original fixture consent lifetime. @return Genuine inactive candidate. */
    ReloadCandidate Make(std::string_view source,std::chrono::milliseconds lifetime=std::chrono::minutes(1)) {
        ReloadCandidate result;result.scope=std::shared_ptr<RuntimeScope>(RuntimeScopeTestAccess::Create(domain));
        result.scope->SetReadiness(true);auto host=base->Bind(result.scope);result.endpoint=host.semantic;
        result.phases=std::make_shared<ReloadPhases>();
        host.lifecycle=[phases=result.phases](ScriptPhase phase,std::uint64_t){
            if(phase==ScriptPhase::Disable)++phases->disable;if(phase==ScriptPhase::Close)++phases->close;
        };
        result.root=RuntimeScopeTestAccess::Issue(*result.scope,false,true,true,1,lifetime);
        Check(result.root.owner!=0,"reload original grant issue before VM");
        SemanticBridgeTestAccess::Issue(*result.endpoint,result.root);
        ScriptDefinition definition;definition.entry="scripts/init.luau";definition.language="luau";
        definition.memoryBytes=8U*1024U*1024U;definition.budgetMs=20;
        definition.bytecode=CompileLuauSource(std::string(source));
        result.vm=std::make_shared<ScriptRuntime>("fixture.canonical-reload",definition,std::move(host));
        result.origin=sc13::mods::OwnedPublicationTestAccess::Issue(result.vm,result.endpoint,"canonical-reload-fixture");
        Facts(result);return result;
    }
    /** @summary Capture actual unchanged runtime facts for original lifecycle preparation. @param candidate Actual VM. */
    void Facts(ReloadCandidate& candidate) {
        copied::Envelope value;
        Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","runtime","1","1"]})",
            "test.echo@1",candidate.endpoint->storageOwner,value)==copied::Status::Ok,"reload actual factual input");
        Check(bridge->BindReferences(*candidate.endpoint,value,candidate.facts)==ScopeResult::Ok,"reload actual factual lease");
    }
};
/** @summary Reattempt only pre-marker Busy on the same authentic plan within the existing fixture bound.
 * @param record Original joint record. @param plan Original retained preparation. @return Actual final result. */
ScopeResult ReloadCommit(const sc13::mods::OwnerLifecycleTestAccess::Record& record,sc13::mods::OwnerLifecycle::Plan& plan) {
    auto result=ScopeResult::Busy;
    for(unsigned attempt=0;attempt!=4096&&result==ScopeResult::Busy;++attempt){
        result=sc13::mods::OwnerLifecycleTestAccess::CommitJoint(record,plan);
        if(result==ScopeResult::Busy)std::this_thread::yield();
    }
    return result;
}
/** @summary Accumulate real generation-local logs while refusing any retired continuation execution.
 * @param vm Actual original VM. @param logs Retained actual log bytes. */
void ReloadLogs(ScriptRuntime& vm,std::vector<std::string>& logs) {
    auto added=vm.DrainLogs();logs.insert(logs.end(),added.begin(),added.end());
    Check(std::none_of(logs.begin(),logs.end(),[](const auto& line){return line.find("FORBIDDEN_RELOAD")!=std::string::npos;}),
        "canonical handoff delivered original Lua continuation");
}
/** @summary Search actual accumulated script diagnostics. @param logs Original log bytes. @param marker Fixed authored marker.
 * @return Whether a real script emitted the marker. */
bool ReloadHas(const std::vector<std::string>& logs,std::string_view marker) {
    return std::any_of(logs.begin(),logs.end(),[&](const auto& line){return line.find(marker)!=std::string::npos;});
}
/** @summary Prove queued and genuinely committed-undelivered old Lua work across SAME-Base canonical joint handoff.
 * @param package Verified separately supplied Base package. */
void CanonicalReload(const std::filesystem::path& package) {
    using Access=sc13::mods::OwnerLifecycleTestAccess;
    using Decision=sc13::mods::LifecycleDecision;
    for(const bool committed:{false,true}) {
        std::cout<<"canonical reload "<<(committed?"committed-undelivered":"queued")<<std::endl;
        // @summary These owners outlive fixture shutdown and every captured task on all exception paths.
        Decision oldBinding,nextBinding,oldDecision,nextDecision;
        ReloadFixture fixture(package);ReloadCandidate old,next;
        sc13::mods::OwnerLifecycle::Plan activation,handoff,removal;
        SemanticInvocation invocation;TaskSnapshot before,after;
        std::vector<std::string> oldLogs,nextLogs;
        const auto first=std::make_shared<ReloadGate>(),await=std::make_shared<ReloadGate>(),second=std::make_shared<ReloadGate>();
        const auto firstVisit=std::make_shared<std::atomic_bool>(false),secondVisit=std::make_shared<std::atomic_bool>(false);
        const auto armed=std::make_shared<std::atomic_bool>(false);
        old=fixture.Make(R"lua(
            local started,events=false,0
            return {onTick=function()
                if started then return end
                started=true
                assert(base.subscribe(function(record)
                    assert(record and type(record.sequence)=='string')
                    events+=1
                    if events==1 then print('RELOAD_A_SUBSCRIBED') else print('FORBIDDEN_RELOAD_SUBSCRIPTION') end
                end))
                local value=assert(base.values.validate('{"v":1,"schema":"test.echo@1","value":["null"]}','test.echo@1'))
                local task=assert(base.request('fixture.echo',value))
                assert(task.onCompleted(function() print('FORBIDDEN_RELOAD_CALLBACK') end))
                assert(base.run(function()
                    print('RELOAD_A_AWAIT_ENTERED')
                    task.await()
                    print('FORBIDDEN_RELOAD_AWAIT')
                end))
            end,onDisable=function() end}
        )lua");
        SchedulerPublicationTestAccess::Barriers(old.vm,[first,await,firstVisit,armed]{
            if(!firstVisit->exchange(true)){first->Hold();return;}
            if(armed->exchange(false))await->Hold();
        },{});
        const auto oldScope=old.scope;const auto oldStorage=old.endpoint->storageOwner;
        const auto epoch=fixture.bridge->Epoch(),ready=fixture.bridge->ReadyGeneration();
        auto record=Access::IssueCandidate(old.origin,fixture.registry,fixture.domain);
        Check(bool(record),"reload original joint record");
        std::string error;
        Check(Access::Joint(fixture.scheduler,record,nullptr,old.origin,{},old.root,{},old.facts,activation,error)==ScopeResult::Ok,
            "reload actual initial joint preparation");
        // @summary Construct LAST: release all callbacks before Plan/VM/scheduler joins on every failure.
        ReloadRelease release{first,await,second};
        Check(ReloadCommit(record,activation)==ScopeResult::Ok,"reload actual initial joint marker");
        Check(first->Entered()&&first->Held(),"reload genuine A first selection before first tick");
        Check(SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,old.origin,oldBinding)==ScopeResult::Ok,
            "reload authentic post-activation owned binding");
        first->Release();
        const auto awaitDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do {
            old.vm->Tick(0);ReloadLogs(*old.vm,oldLogs);
            if(ReloadHas(oldLogs,"RELOAD_A_AWAIT_ENTERED")&&ReloadHas(oldLogs,"RELOAD_A_SUBSCRIBED"))break;
        }while(std::chrono::steady_clock::now()<awaitDeadline);
        Check(first->CleanExit()&&old.vm->Status().running&&ReloadHas(oldLogs,"RELOAD_A_AWAIT_ENTERED")&&
            ReloadHas(oldLogs,"RELOAD_A_SUBSCRIBED"),"reload actual old coroutine and initial subscription ready");
        next=fixture.Make(R"lua(
            local started,completed,awaited,subscribed,finished=false,0,false,false,false
            local wire='{"v":1,"schema":"test.echo@1","value":["null"]}'
            return {onTick=function()
                if not started then
                    started=true
                    assert(base.subscribe(function(record) assert(record and type(record.sequence)=='string');subscribed=true end))
                    local value=assert(base.values.validate(wire,'test.echo@1'))
                    local task=assert(base.request('fixture.echo',value))
                    assert(task.onCompleted(function(record)
                        assert(record and record.state=='succeeded' and record.committed and base.values.wire(record.result)==wire)
                        completed+=1
                    end))
                    assert(base.run(function()
                        local record=task.await()
                        assert(record and record.state=='succeeded' and record.committed and base.values.wire(record.result)==wire)
                        awaited=true
                    end))
                elseif not finished and completed==1 and awaited and subscribed then
                    print('RELOAD_B_COMPLETE');finished=true
                end
            end,onDisable=function() end}
        )lua");
        SchedulerPublicationTestAccess::Barriers(next.vm,[second,secondVisit]{if(!secondVisit->exchange(true))second->Hold();},{});
        Check(next.root.revision==fixture.domain->Revision()&&next.scope!=old.scope&&
            next.vm->Status().generation!=old.vm->Status().generation,"reload successor original current root and distinct VM");
        armed->store(true);
        Check(await->Entered()&&await->Held(),"reload genuine armed selected cut after await and B construction");
        Check(fixture.bridge->Take(invocation)&&invocation.origin==old.endpoint,"reload exact original queued task");
        Check(old.endpoint->tasks->Poll(invocation.task,before)==ScopeResult::Ok&&before.state==TaskState::Queued&&!before.committed,
            "reload genuine queued original outcome before cut");
        if(committed) {
            Check(fixture.bridge->Dispatch(invocation,"fixture.echo",invocation.value)==ScopeResult::Ok,"reload genuine old native commit before delivery");
            Check(old.endpoint->tasks->Poll(invocation.task,before)==ScopeResult::Ok&&before.state==TaskState::Succeeded&&before.committed&&
                before.outcome==ScopeResult::Ok&&fixture.calls==1,"reload actual committed snapshot before retirement");
        }
        Check(fixture.calls==(committed?1:0)&&await->Held(),"reload native effect distinct from held Lua delivery");
        Check(sc13::mods::LifecycleDecisionTestAccess::FreshDecision(old.scope,old.root,oldDecision)==ScopeResult::Ok&&
            sc13::mods::LifecycleDecisionTestAccess::FreshDecision(next.scope,next.root,nextDecision)==ScopeResult::Ok,
            "reload current existing-grant tracked lifecycle reservations");
        fixture.Facts(old);fixture.Facts(next);const auto revision=fixture.domain->Revision();
        Check(Access::Joint(fixture.scheduler,record,old.origin,next.origin,oldDecision.Context(),nextDecision.Context(),
            old.facts,next.facts,handoff,error)==ScopeResult::Ok,"reload original-record handoff preparation");
        Check(ReloadCommit(record,handoff)==ScopeResult::Ok&&fixture.domain->Revision()==revision+1,
            "reload one authentic original-record handoff marker");
        Check(old.scope->Current().stopping&&!old.vm->Status().running&&next.scope->Current().activationCommitted&&
            fixture.bridge->Epoch()==epoch&&fixture.bridge->ReadyGeneration()==ready,"reload retired A and same-Base active successor");
        Check(old.endpoint->tasks->Poll(invocation.task,after)==ScopeResult::Stale&&!after.committed&&!after.value,
            "reload post-retirement delivery denies without disclosing committed snapshot");
        await->Release();
        Check(second->Entered()&&second->Held(),"reload B genuine first selection before successor request");
        Check(SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,next.origin,nextBinding)==ScopeResult::Ok,
            "reload authentic post-handoff successor binding");
        second->Release();
        // @summary B's first gate is released before ANY synchronous lane operation, including old reclaim.
        old.vm->Tick(0);ReloadLogs(*old.vm,oldLogs);
        Check(first->CleanExit()&&await->CleanExit()&&second->CleanExit(),"reload all selected cuts explicitly exited without expiry");
        Check(Access::Reclaim(handoff)==ScopeResult::Ok&&Access::Reclaim(handoff)==ScopeResult::Ok&&
            old.phases->disable==1&&old.phases->close==1&&SchedulerPublicationTestAccess::Closed(old.vm),
            "reload original close and disable exactly once through genuine reclaim");
        Check(old.endpoint->tasks->Release(invocation.task)==ScopeResult::Stale&&old.scope->Current().retainedOperations==0&&
            RuntimeScopeTestAccess::Registrations(*old.scope)==0,"reload actual Lua task and continuation ownership released once");
        TaskHandle denied;SemanticContextLease deniedFacts;
        Check(fixture.bridge->Request(old.endpoint,"fixture.echo",invocation.value,denied,deniedFacts)==ScopeResult::Stale&&!denied&&
            fixture.bridge->Dispatch(invocation,"fixture.echo",invocation.value)==ScopeResult::Stale&&fixture.calls==(committed?1:0),
            "reload closed original facade cannot target equal-ID successor or replay effect");
        SemanticBridgeTestAccess::ObserveContext(*fixture.bridge,SemanticContextKind::City,99,1);
        const auto successorDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do {
            next.vm->Tick(0);fixture.base->Pump();ReloadLogs(*old.vm,oldLogs);ReloadLogs(*next.vm,nextLogs);
            if(ReloadHas(nextLogs,"RELOAD_B_COMPLETE"))break;
        }while(std::chrono::steady_clock::now()<successorDeadline);
        Check(next.vm->Status().running&&ReloadHas(nextLogs,"RELOAD_B_COMPLETE")&&fixture.calls==(committed?2:1)&&
            fixture.bridge->Epoch()==epoch&&fixture.bridge->ReadyGeneration()==ready,"reload successor real facade callback await subscription usable");
        Check(Access::Joint(fixture.scheduler,record,next.origin,nullptr,next.root,{},next.facts,{},removal,error)==ScopeResult::Ok&&
            ReloadCommit(record,removal)==ScopeResult::Ok,"reload genuine successor release-only removal");
        Check(Access::Reclaim(removal)==ScopeResult::Ok&&Access::Reclaim(removal)==ScopeResult::Ok&&
            next.phases->disable==1&&next.phases->close==1,"reload successor genuine once-only cleanup");
        ReloadLogs(*old.vm,oldLogs);Check(fixture.calls==(committed?2:1),"reload cleanup never replayed native effects");
        const auto nextScope=next.scope;const auto nextStorage=next.endpoint->storageOwner;
        invocation={};before={};after={};activation={};handoff={};removal={};record.reset();old={};next={};
        oldBinding={};nextBinding={};oldDecision={};nextDecision={};
        const auto oldRetirement=ScopeRetirementTestAccess::Observe(*oldScope),nextRetirement=ScopeRetirementTestAccess::Observe(*nextScope);
        Check(oldRetirement.result==ScopeResult::Ok&&nextRetirement.result==ScopeResult::Ok&&
            oldRetirement.commandTickets==0&&nextRetirement.commandTickets==0&&
            oldRetirement.nonCommandFreshReservations==0&&nextRetirement.nonCommandFreshReservations==0,
            "reload actual tracked reservation owners settled without raw fresh leak");
        Check(fixture.bridge->Values()->Used(oldStorage)==0&&fixture.bridge->Values()->Used(nextStorage)==0,
            "reload original and successor copied storage fully released after retained owners");
    }
}

/** @summary Restore only the current fixture thread's existing reservation cuts before any future join. */
struct ReloadClearReservationCut final {
    /** @summary No callback remains installed after stack references become invalid. */
    ~ReloadClearReservationCut(){ScopeRetirementTestAccess::Barriers({},{});}
};
/** @summary Dispose a non-scheduled native adapter fixture before its authentic reservation owners.
 * @param candidate Original unregistered VM. */
void ReloadCloseNative(ReloadCandidate& candidate) {
    candidate.vm->Stop();candidate.origin.reset();candidate.vm.reset();candidate.endpoint.reset();
}
/** @summary Drain genuine bounded ticket acknowledgements and require no lost fresh reservation.
 * @param scope Actual original scope. @param roots Original retained root count. */
void ReloadSettled(RuntimeScope& scope,std::size_t roots=1) {
    for(unsigned attempt=0;attempt!=128;++attempt){
        const auto observed=ScopeRetirementTestAccess::Observe(scope);
        if(observed.result==ScopeResult::Ok&&!observed.commandTickets&&!observed.mailboxWriting&&!observed.mailboxReady&&
            !observed.mailboxDraining&&!observed.disposalPending)break;
        const auto drained=ScopeRetirementTestAccess::DrainOne(scope);
        Check(drained==ScopeResult::Ok||drained==ScopeResult::NotReady,"refresh genuine off-guard ticket acknowledgement");
    }
    const auto observed=ScopeRetirementTestAccess::Observe(scope);
    Check(observed.result==ScopeResult::Ok&&!observed.commandTickets&&!observed.nonCommandFreshReservations&&
        observed.rootReservations==roots&&!observed.mailboxWriting&&!observed.mailboxReady&&!observed.mailboxDraining&&!observed.disposalPending,
        "refresh authentic ticket and reservation settlement");
}
/** @summary Qualify exact denied ingress, real lock contention, original task preservation and finite owned capacity.
 * @param package Real Base package, with unchanged script and target budgets. */
void RefreshReservationNegatives(const std::filesystem::path& package) {
    using Decision=sc13::mods::LifecycleDecision;
    // @summary Owning decisions survive every candidate close and fixture shutdown on exceptions.
    Decision owner,replacement,denied;
    std::vector<Decision> retained;retained.reserve(127);
    ReloadFixture fixture(package);
    for(unsigned mode=0;mode!=14;++mode) {
        std::cout<<"refresh admission negative "<<mode<<std::endl;
        auto candidate=fixture.Make("return {}",mode==4?std::chrono::milliseconds(20):std::chrono::minutes(1));
        if(mode!=7) {Check(candidate.scope->CommitActivation(),"refresh native adapter active scope");candidate.vm->Commit();}
        const auto source=candidate.endpoint->context;const auto revision=fixture.domain->Revision();
        const auto count=RuntimeScopeTestAccess::Reservations(*candidate.scope);
        auto result=ScopeResult::Invalid;auto expected=ScopeResult::Invalid;
        if(mode==0)result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,{},denied);
        if(mode==1){SemanticBridge foreign;result=SemanticBridgeTestAccess::RefreshReservation(foreign,candidate.origin,denied);expected=ScopeResult::Stale;}
        if(mode==2){
            Check(SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,owner)==ScopeResult::Ok,
                "refresh original owned positive before occupied-output negative");
            const auto binding=candidate.endpoint->context;const auto occupied=RuntimeScopeTestAccess::Reservations(*candidate.scope);
            result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,owner);
            Check(result==ScopeResult::Invalid&&candidate.endpoint->context==binding&&
                RuntimeScopeTestAccess::Reservations(*candidate.scope)==occupied,"refresh occupied output does not overwrite owner or binding");
        }
        if(mode==3){
            Check(RuntimeScopeTestAccess::Revoke(*candidate.scope,candidate.root.grantGeneration)==ScopeResult::Ok,"refresh actual grant revocation");
            result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);expected=ScopeResult::Denied;
        }
        if(mode==4){
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(std::chrono::steady_clock::now()<candidate.root.deadline&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
            Check(std::chrono::steady_clock::now()>=candidate.root.deadline,"refresh actual original grant expiry");
            result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);expected=ScopeResult::Denied;
        }
        if(mode==5){candidate.scope->RequestStop();result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);expected=ScopeResult::Denied;}
        if(mode==6){candidate.scope->SetReadiness(false);result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);expected=ScopeResult::NotReady;}
        if(mode==7){result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);expected=ScopeResult::NotReady;}
        if(mode>=8&&mode<=10){
            const auto held=std::make_shared<ReloadGate>();std::atomic_bool acquired{};
            auto worker=std::async(std::launch::async,[&,held]{
                if(mode==8){auto guard=SemanticBridgeTestAccess::ReserveObservation(*fixture.bridge);acquired=guard.owns_lock();held->Hold();}
                if(mode==9){auto guard=RuntimeScopeTestAccess::ReserveState(*candidate.scope);acquired=guard.owns_lock();held->Hold();}
                if(mode==10){auto guard=candidate.scope->ReserveWriter();acquired=guard.Acquired();held->Hold();}
            });
            // @summary Destroy release BEFORE worker future on every failure, including failed native admission.
            ReloadRelease release{held,held,held};
            Check(held->Entered()&&acquired&&held->Held(),"refresh actual foreign-thread guard contention");
            result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);expected=ScopeResult::Busy;
            held->Release();Check(worker.wait_for(std::chrono::seconds(2))==std::future_status::ready,"refresh contention worker joined");worker.get();
            Check(held->CleanExit(),"refresh contention did not expire");
        }
        if(mode==11){
            bool called{};ReloadClearReservationCut clear;
            ScopeRetirementTestAccess::Barriers([&]{called=true;},{});
            auto writer=candidate.scope->ReserveWriter();Check(writer.Acquired(),"refresh same-domain reentry setup");
            result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);expected=ScopeResult::Busy;
            Check(!called,"refresh writer reentry never invokes reservation callback under D");
        }
        if(mode==12){candidate.vm->Stop();result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);expected=ScopeResult::Stale;}
        if(mode==13){
            auto foreign=fixture.Make("return {}");bool rejected{};
            try{static_cast<void>(sc13::mods::OwnedPublicationTestAccess::Issue(candidate.vm,foreign.endpoint,"invalid-native-pair"));}
            catch(const std::invalid_argument&){rejected=true;}
            Check(rejected,"refresh genuine foreign VM and endpoint cannot construct an original incarnation");
            result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,{},denied);
            ReloadCloseNative(foreign);
        }
        if(mode!=2)Check(result==expected&&!denied.Context().owner&&candidate.endpoint->context==source&&
            RuntimeScopeTestAccess::Reservations(*candidate.scope)==count,"refresh exact failed admission preserves binding and capacity");
        Check(fixture.domain->Revision()==revision&&fixture.calls==0,"refresh never publishes domain or native effect");
        const auto scope=candidate.scope;ReloadCloseNative(candidate);owner={};replacement={};denied={};ReloadSettled(*scope);
    }
    {
        std::cout<<"refresh original task and capacity"<<std::endl;
        auto candidate=fixture.Make("return {}");Check(candidate.scope->CommitActivation(),"refresh task fixture active scope");candidate.vm->Commit();
        const auto source=candidate.root;const auto revision=fixture.domain->Revision();
        Check(SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,owner)==ScopeResult::Ok,"refresh first owned real reservation");
        const auto first=owner.Context();
        Check(first.reservation!=source.reservation&&first.owner==source.owner&&first.epoch==source.epoch&&
            first.grantGeneration==source.grantGeneration&&first.decision==source.decision&&first.origin==source.origin&&
            first.deadline==source.deadline&&first.opcode==source.opcode&&first.readiness==source.readiness&&
            first.revision==fixture.domain->Revision(),"refresh same original provenance and exact current revision");
        copied::Envelope value;Check(fixture.bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["null"]})",
            "test.echo@1",candidate.endpoint->storageOwner,value)==copied::Status::Ok,"refresh actual task input");
        TaskHandle task;SemanticContextLease facts;
        Check(fixture.bridge->Request(candidate.endpoint,"fixture.echo",value,task,facts)==ScopeResult::Ok,"refresh original real native task admission");
        TaskSnapshot before,after;Check(candidate.endpoint->tasks->Poll(task,before)==ScopeResult::Ok&&before.state==TaskState::Queued&&!before.committed,
            "refresh original queued task before binding replacement");
        Check(SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,replacement)==ScopeResult::Ok&&
            replacement.Context().reservation!=first.reservation&&candidate.endpoint->tasks->Poll(task,after)==ScopeResult::Ok&&
            after.state==before.state&&after.committed==before.committed&&after.outcome==before.outcome,
            "refresh replacement preserves original captured task context and outcome");
        SemanticInvocation invocation;Check(fixture.bridge->Take(invocation)&&invocation.origin==candidate.endpoint,
            "refresh original task native identity retained");
        Check(fixture.bridge->Dispatch(invocation,"fixture.echo",invocation.value)==ScopeResult::Ok&&fixture.calls==1&&
            fixture.domain->Revision()==revision+1,"refresh actual native effect advances D once");
        Check(candidate.endpoint->tasks->Poll(task,after)==ScopeResult::Ok&&after.state==TaskState::Succeeded&&after.committed,
            "refresh committed original task remains truthful after D advance");
        const auto stale=candidate.endpoint->context;TaskHandle rejected;SemanticContextLease rejectedFacts;
        Check(fixture.bridge->Request(candidate.endpoint,"fixture.echo",value,rejected,rejectedFacts)==ScopeResult::Conflict&&!rejected&&
            candidate.endpoint->context==stale&&fixture.calls==1,"refresh stale bound revision never silently renewed or replayed");
        Check(candidate.endpoint->tasks->Release(task)==ScopeResult::Ok,"refresh original task released before dropping its reservation owner");
        invocation={};before={};after={};owner={};
        Check(SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,owner)==ScopeResult::Ok,
            "refresh separately explicit current existing-grant binding after native advance");
        replacement={};
        Check(fixture.bridge->Request(candidate.endpoint,"fixture.echo",value,task,facts)==ScopeResult::Ok&&
            candidate.endpoint->tasks->Cancel(task)==ScopeResult::Ok&&candidate.endpoint->tasks->Release(task)==ScopeResult::Ok,
            "refresh legitimate new reservation supports new task without replay");
        Check(fixture.bridge->Take(invocation)&&invocation.origin==candidate.endpoint,"refresh cancelled invocation original queue identity");invocation={};
        retained.push_back(std::move(owner));
        for(unsigned index=1;index!=127;++index){
            Decision next;Check(SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,next)==ScopeResult::Ok,
                "refresh each owned reservation within unchanged native capacity");retained.push_back(std::move(next));
        }
        const auto full=candidate.endpoint->context;const auto fullCount=RuntimeScopeTestAccess::Reservations(*candidate.scope);
        Check(fullCount==128&&SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied)==ScopeResult::NotReady&&
            !denied.Context().owner&&candidate.endpoint->context==full&&RuntimeScopeTestAccess::Reservations(*candidate.scope)==fullCount,
            "refresh full authentic reservation budget denies without owner or binding loss");
        const auto scope=candidate.scope;ReloadCloseNative(candidate);retained.clear();ReloadSettled(*scope);
        Check(fixture.calls==1,"refresh capacity and disposal never replay effects");
    }
    {
        std::cout<<"refresh post-reservation Q mailbox cleanup"<<std::endl;
        auto candidate=fixture.Make("return {}");Check(candidate.scope->CommitActivation(),"refresh mailbox fixture active scope");candidate.vm->Commit();
        const auto source=candidate.endpoint->context;
        const auto held=std::make_shared<ReloadGate>();std::atomic_bool start{},acquired{};
        auto worker=std::async(std::launch::async,[&,held]{
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!start.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
            if(!start.load())return;
            auto guard=RuntimeScopeTestAccess::ReserveState(*candidate.scope);acquired=guard.owns_lock();held->Hold();
        });
        ReloadRelease release{held,held,held};ReloadClearReservationCut clear;
        ScopeRetirementTestAccess::Barriers([&]{start=true;Check(held->Entered()&&acquired&&held->Held(),
            "refresh fresh reservation callback outside guards allows actual Q contender");},{});
        const auto result=SemanticBridgeTestAccess::RefreshReservation(*fixture.bridge,candidate.origin,denied);
        ScopeRetirementTestAccess::Barriers({},{});
        Check(result==ScopeResult::Busy&&!denied.Context().owner&&candidate.endpoint->context==source&&held->Held(),
            "refresh post-reservation Q denial preserves binding while exact release is pending");
        held->Release();Check(worker.wait_for(std::chrono::seconds(2))==std::future_status::ready,"refresh mailbox Q contender joined");worker.get();
        Check(held->CleanExit(),"refresh mailbox contention did not expire");
        auto pending=ScopeRetirementTestAccess::Observe(*candidate.scope);
        Check(pending.result==ScopeResult::NotReady&&!pending.scopeReady&&pending.commandTickets==1&&pending.mailboxReady==1&&
            pending.rootReservations==1&&pending.nonCommandFreshReservations==0,
            "refresh failed prepared lease transfers exact native ticket to real mailbox");
        ReloadCloseNative(candidate);
        ReloadSettled(*candidate.scope);
        Check(RuntimeScopeTestAccess::Reservations(*candidate.scope)==1,"refresh actual acknowledgement restores original reservation count");
    }
}

/** @summary Distinguish function isolation in DEV from whole-mod isolation in REGULAR using real callback/coroutine failures. @param package Actual package. */
void FaultModes(const std::filesystem::path& package) {
    for (const auto mode : {ScriptHost::ErrorMode::Development, ScriptHost::ErrorMode::Regular})
        for (const bool coroutine : {false, true}) {
            int calls{}; auto bridge = std::make_shared<SemanticBridge>();
            SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
            SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
            BaseHost host(package, Pin(package), {}, bridge);
            auto scope = Scope();
            const auto action = coroutine ? "base.run(function() error('fixture-coroutine-failure') end)"
                : "local value=assert(base.values.validate('{\"v\":1,\"schema\":\"test.echo@1\",\"value\":[\"null\"]}','test.echo@1')); local task=assert(base.request('fixture.echo',value)); task.onCompleted(function() error('fixture-callback-failure') end)";
            const auto source = "local started=false; return {onTick=function() if not started then started=true; " + std::string(action) + " end end}";
            auto mod = Mod(host, scope, source, mode);
            Turns(host, {mod.get()});
            const auto status = mod->Status();
            Check(status.diagnostic.find(coroutine ? "SEMANTIC_COROUTINE_FAILED" : "SEMANTIC_CALLBACK_FAILED") != std::string::npos,
                "real semantic fault did not produce bounded diagnostic");
            Check(status.running == (mode == ScriptHost::ErrorMode::Development), "DEV/REGULAR failure isolation mismatch");
            Check(calls == (coroutine ? 0 : 1), "fault handling replayed or lost native task effect");
            mod->Stop(); mod.reset();
            Check(bridge->Values()->Used(scope->Current().generation) == 0, "faulted generation retained native storage");
        }
}
/** @summary Exercise actual Luau interrupt and allocator caps with retained-registration cleanup, without a production injection switch. @param package Actual package. */
void InterruptAndMemory(const std::filesystem::path& package) {
    for (const bool memory : {false, true}) {
        auto bridge = std::make_shared<SemanticBridge>();
        SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
        BaseHost host(package, Pin(package), {}, bridge);
        auto scope = Scope();
        const auto body = memory ? "local allocation = string.rep('x', 4*1024*1024); print(#allocation)" : "while true do end";
        const auto source = "local started=false; return {onTick=function() if not started then started=true; base.run(function() " +
            std::string(body) + " end) end end}";
        auto mod = Mod(host, scope, source, ScriptHost::ErrorMode::Regular, 2U * 1024U * 1024U, memory ? 20U : 2U);
        const auto before = std::chrono::steady_clock::now();
        mod->Tick(0); mod->Tick(0);
        Check(std::chrono::steady_clock::now() - before < std::chrono::seconds(1), "semantic fault exceeded interrupt containment deadline");
        Check(!mod->Status().running && mod->Status().diagnostic.find("SEMANTIC_COROUTINE_FAILED") != std::string::npos,
            "actual allocator/deadline fault escaped coroutine isolation");
        mod->Stop(); mod.reset();
        Check(bridge->Values()->Used(scope->Current().generation) == 0, "allocator/deadline fault orphaned native copied storage");
        Check(RuntimeScopeTestAccess::Registrations(*scope) == 0, "fault leaked registration capacity");
        Check(!scope->ReserveRegistration(), "faulted stopped owner reopened registration admission");
    }
}
/** @summary Reject an adversarial shape-only critical graph despite complete function names and frozen service tables. @param package Trusted source for disposable fixture copy. */
void ShapeOnlyReadiness(const std::filesystem::path& package) {
    const auto root = std::filesystem::temp_directory_path() /
        ("simoder-shape-base-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    try {
        std::filesystem::copy(package / "scripts", root / "scripts", std::filesystem::copy_options::recursive);
        std::filesystem::copy_file(package / "mod.toml", root / "mod.toml");
        std::ofstream source(root / "scripts/init.luau", std::ios::binary | std::ios::trunc);
        source << R"lua(
            local function forged() return nil end
            local service = table.freeze({validate=forged,wire=forged,wrap=forged,stop=forged,
                snapshot=forged,subscribe=forged,descriptors=forged,resolve=forged,manager=forged,dispatch=forged})
            local exports = {}
            exports.onEnable = function()
                exports.services = table.freeze({values=service,tasks=service,lifecycle=service,
                    registry=service,['runtime-manager']=service})
            end
            return exports
        )lua";
        source.close(); Check(bool(source), "shape-only fixture write failed");
        auto bridge = std::make_shared<SemanticBridge>();
        bool rejected{};
        try { BaseHost host(root, Pin(root), {}, bridge); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected && !bridge->ReadyGeneration(), "shape-only fake critical services manufactured readiness");
        std::filesystem::remove_all(root);
    } catch (...) { std::filesystem::remove_all(root); throw; }
}
/** @summary Deny stale factual leases, unknown SP/MP, foreign owner data and old-base epochs without effects. @param package Actual pinned package. */
void ContextAndRestart(const std::filesystem::path& package) {
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls, "city")});
    auto host = std::make_unique<BaseHost>(package, Pin(package), std::function<void(ScriptPhase,std::uint64_t)>{}, bridge);
    auto scope = Scope(); auto binding = host->Bind(scope); auto endpoint = binding.semantic;
    SemanticBridgeTestAccess::Issue(*endpoint, RuntimeScopeTestAccess::Issue(*scope));
    ScriptDefinition definition; definition.entry = "scripts/init.luau";
    definition.bytecode = CompileLuauSource("return {}");
    ScriptRuntime mod("fixture.context", definition, std::move(binding)); mod.Commit();
    copied::Envelope value;
    Check(bridge->Values()->Decode(R"({"v":1,"schema":"test.echo@1","value":["ref","city","7","3"]})",
        "test.echo@1", endpoint->storageOwner, value) == copied::Status::Ok, "context fixture codec failed");
    TaskHandle task; SemanticContextLease context;
    Check(bridge->Request(endpoint, "fixture.echo", value, task, context) == ScopeResult::NotReady, "unknown SP/MP admitted mutation");
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
    Check(bridge->Request(endpoint, "fixture.echo", value, task, context) == ScopeResult::Ok, "live context request rejected");
    SemanticInvocation invocation; Check(bridge->Take(invocation), "context invocation absent");
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::City, 7, 3);
    Check(bridge->ValidateContext(*endpoint, context) == ScopeResult::Stale, "same-slot context ABA silently rebound");
    Check(bridge->Dispatch(invocation, "fixture.echo", value) == ScopeResult::Stale && calls == 0, "stale-context task executed");
    Check(endpoint->tasks->Release(task) == ScopeResult::Ok, "stale-context owner cleanup failed");
    const auto oldEpoch = bridge->Epoch(); host.reset();
    Check(!bridge->ReadyGeneration(), "base retirement retained readiness");
    Check(bridge->Request(endpoint, "fixture.echo", value, task, context) != ScopeResult::Ok, "old base facade admitted work");
    auto replacement = std::make_shared<SemanticBridge>();
    BaseHost restarted(package, Pin(package), {}, replacement);
    Check(restarted.Ready() && replacement->Epoch() != oldEpoch, "base restart reused epoch or failed real graph");
    Check(bridge->Dispatch(invocation, "fixture.echo", value) == ScopeResult::Stale && calls == 0, "old invocation replayed after restart");
    mod.Stop();
}
}

/** @summary Execute real native/Base/mod integration against an explicit separately checked-out source package. @param argc Argument count. @param argv Package root argument. @return Zero only if all executed expectations pass. */
int main(int argc, char** argv) {
    try {
        Check(argc == 2, "actual Base package argument required");
        const std::filesystem::path package(argv[1]);
        Publications(); CrossVm(package); CancellationAndBounds(package); Revocation(package); NativeOutcomeGates(package); PendingBound(package); BusyWriterSettlement(package);
        PreparedRegistryPublications(package); OwnedBaselineSequence(package); OwnerLifecycleTransitions(package); LuaStreams(package); ReferenceAndRuntimeContexts(package); StoppedContinuations(package); FaultModes(package);
        InterruptAndMemory(package); ShapeOnlyReadiness(package); ContextAndRestart(package);
        CanonicalReload(package); RefreshReservationNegatives(package);
        std::cout << checks << " semantic bridge checks passed\n"; return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
