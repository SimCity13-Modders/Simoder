#include "scripting/base_host.hpp"
#include "scripting/semantic_bridge.hpp"
#include "scripting/luau_compiler.hpp"
#include "scripting/source_project.hpp"
#include <algorithm>
#include <cstring>
#include <future>
#include <fstream>
#include <iostream>
#include <stdexcept>

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
    int calls{}; auto bridge = std::make_shared<SemanticBridge>();
    SemanticBridgeTestAccess::Configure(*bridge, {Echo(calls)});
    SemanticBridgeTestAccess::ObserveContext(*bridge, SemanticContextKind::Runtime, 1, 1);
    BaseHost host(package, Pin(package), {}, bridge);
    auto scope = Scope();
    auto binding = host.Bind(scope);
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
    RuntimeScopeTestAccess::Revoke(*scope, context.session);
    host.Pump(); mod.Tick(0); Marker(mod, "REVOKE_COMPLETE"); Check(calls == 0, "revoked queued task committed");
    mod.Stop();
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
    RuntimeScopeTestAccess::Revoke(*scope, issued.session);
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
    auto scope = Scope(domain); auto binding = host.Bind(scope); auto endpoint = binding.semantic;
    SemanticBridgeTestAccess::Issue(*endpoint, RuntimeScopeTestAccess::Issue(*scope));
    ScriptDefinition definition; definition.entry = "scripts/init.luau"; definition.bytecode = CompileLuauSource("return {}");
    ScriptRuntime mod("fixture.busy-writer", definition, std::move(binding)); mod.Commit();
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
        LuaStreams(package); ReferenceAndRuntimeContexts(package); StoppedContinuations(package); FaultModes(package);
        InterruptAndMemory(package); ShapeOnlyReadiness(package); ContextAndRestart(package);
        std::cout << checks << " semantic bridge checks passed\n"; return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
