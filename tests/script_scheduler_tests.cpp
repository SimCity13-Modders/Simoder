#include "scripting/script_runtime.hpp"
#include <Luau/Compiler.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstdlib>
#include <new>

namespace {
std::atomic_bool rejectHostAllocation{};
std::atomic_size_t rejectedHostAllocations{};
}
/** @summary Injects deterministic test-binary host allocation failure, leaving the Lua realloc allocator intact. */
void* operator new(std::size_t bytes) {
    if (rejectHostAllocation.load()) { ++rejectedHostAllocations; throw std::bad_alloc(); }
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
/** @summary Routes array allocations through the same deterministic host failure fixture. */
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
/** @summary Releases test-binary host storage without requiring allocation. */
void operator delete(void* pointer) noexcept { std::free(pointer); }
/** @summary Releases array storage through the same test-binary allocator. */
void operator delete[](void* pointer) noexcept { std::free(pointer); }
/** @summary Matches sized host deletion emitted by the selected compiler. */
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
/** @summary Matches sized array deletion emitted by the selected compiler. */
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
using namespace sc13::scripting;
using namespace std::chrono_literals;
/** @summary Fails a behavioral expectation. @param value Observed condition. @param message Failure detail. */
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
/** @summary Compiles trusted synthetic source using the production linked compiler. */
ScriptDefinition Definition(const std::string& source) {
    ScriptDefinition result;
    result.bytecode = Luau::compile(source); result.entry = "scripts/main.luau";
    result.language = "luau"; result.memoryBytes = 2U * 1024U * 1024U;
    return result;
}
/** @summary Observes every real lifecycle phase, failure cleanup and nonreused host generations. */
void ThreadIdentity() {
    std::mutex mutex;
    std::vector<std::pair<ScriptPhase, std::thread::id>> phases;
    std::vector<std::uint64_t> generations;
    ScriptHost host;
    host.lifecycle = [&](ScriptPhase phase, std::uint64_t generation) {
        std::lock_guard lock(mutex); phases.emplace_back(phase, std::this_thread::get_id()); generations.push_back(generation);
    };
    auto definition = Definition("local h=require('./helper'); return {onEnable=function() h() end,onTick=function() end,onDisable=function() end}");
    definition.modules["scripts/helper.luau"] = Luau::compile("return function() end");
    std::uint64_t first{};
    {
        ScriptRuntime runtime("fixture.identity", definition, host);
        runtime.Commit(); runtime.Tick(0.05); runtime.Stop(); runtime.Stop();
        first = runtime.Status().generation;
    }
    const auto lane = phases.front().second;
    Check(lane != std::this_thread::get_id(), "Lua ran on the caller thread");
    for (const auto& phase : phases) Check(phase.second == lane, "Lifecycle escaped the sole script lane");
    for (const auto expected : {ScriptPhase::Create, ScriptPhase::Require, ScriptPhase::Entry, ScriptPhase::Enable, ScriptPhase::Tick, ScriptPhase::Disable, ScriptPhase::Close}) {
        unsigned count{}; for (const auto& phase : phases) if (phase.first == expected) ++count;
        Check(count == 1, "Lifecycle phase missing or repeated");
    }
    for (const auto generation : generations) Check(generation == first && generation != 0, "Generation evidence changed during VM lifetime");
    phases.clear(); generations.clear();
    bool failed{};
    try { ScriptRuntime runtime("fixture.failed", Definition("error('fixture init rejection')"), host); }
    catch (const std::exception&) { failed = true; }
    Check(failed && phases.back().first == ScriptPhase::Close, "Failed initialization did not close on the lane");
    Check(generations.front() > first, "Generation reused after lane recreation");
}
/** @summary Preserves aggregate memory admission, recovers reservations and exposes bounded dropped-log evidence. */
void LimitsAndLogs() {
    auto definition = Definition("for i=1,140 do simoder.log('bounded') end; return {}");
    {
        ScriptRuntime runtime("fixture.logs", definition); runtime.Commit();
        Check(runtime.DrainLogs().size() == 128 && runtime.Status().droppedLogs == 12, "Log overflow lost its bounded drop count");
    }
    std::vector<std::unique_ptr<ScriptRuntime>> runtimes;
    definition = Definition("return {}");
    for (unsigned i = 0; i < 64; ++i) runtimes.push_back(std::make_unique<ScriptRuntime>("fixture.capacity", definition));
    bool rejected{};
    try { ScriptRuntime excess("fixture.excess", definition); } catch (const std::exception&) { rejected = true; }
    Check(rejected, "Aggregate memory admission exceeded 128 MiB");
    runtimes.clear();
    ScriptRuntime recovered("fixture.recovered", definition);
    Check(recovered.Status().memoryBytes > 0, "Failed admission leaked a VM reservation");
}
/** @summary Checks fair visits, bounded interruption and prepublication host binding. */
void Fairness() {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned ticks{};
    ScriptScheduler scheduler([&](const std::string&, const std::string& message) {
        if (message == "tick") { std::lock_guard lock(mutex); ++ticks; changed.notify_all(); }
    });
    auto loop = std::make_shared<ScriptRuntime>("fixture.loop", Definition("return {onTick=function() while true do end end}"));
    auto healthy = std::make_shared<ScriptRuntime>("fixture.healthy", Definition("return {onTick=function() simoder.log('tick') end}"));
    scheduler.Reserve("fixture.loop"); scheduler.Reserve("fixture.healthy");
    scheduler.Prepare(loop); scheduler.Prepare(healthy);
    bool rejected{};
    try { scheduler.Prepare(healthy); } catch (const std::exception&) { rejected = true; }
    Check(rejected, "An already bound VM accepted another host");
    loop->Commit(); healthy->Commit(); scheduler.Put("fixture.loop", loop); scheduler.Put("fixture.healthy", healthy);
    std::vector<std::shared_ptr<ScriptRuntime>> flood;
    std::vector<std::jthread> writers;
    for (unsigned i = 0; i < 4; ++i) {
        auto runtime = std::make_shared<ScriptRuntime>("fixture.control-flood", Definition("return {onTick=function() end}"));
        runtime->Commit(); flood.push_back(runtime);
        writers.emplace_back([runtime](std::stop_token stop) { while (!stop.stop_requested()) runtime->Tick(0.001); });
    }
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, 3s, [&] { return ticks >= 3; }), "Faulty VM starved a healthy generation");
    }
    for (auto& writer : writers) writer.request_stop();
    writers.clear();
    Check(!loop->Status().running && !loop->Status().diagnostic.empty(), "Loop was not interrupted and isolated");
    scheduler.Shutdown();
    Check(!healthy->Status().running && healthy->Status().memoryBytes == 0, "Shutdown did not close its retained VMs");
}
/** @summary Reads a copied status while a native observation suspends a real Lua callback under its VM lock. */
void CopiedStatus() {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{}, release{};
    ScriptHost host;
    host.readFloat = [&](const sc13::core::Tgi&, std::uint32_t) -> std::optional<FloatObservation> {
        std::unique_lock lock(mutex); entered = true; changed.notify_all(); changed.wait(lock, [&] { return release; }); return {};
    };
    ScriptRuntime runtime("fixture.snapshot", Definition("return {onTick=function() simoder.props.getFloat('00B1B104:0:1',0) end}"), host);
    runtime.Commit();
    auto tick = std::async(std::launch::async, [&] { runtime.Tick(0.05); });
    {
        std::unique_lock lock(mutex); Check(changed.wait_for(lock, 3s, [&] { return entered; }), "Native observation never entered Lua callback");
    }
    const auto before = std::chrono::steady_clock::now();
    Check(runtime.Status().running && std::chrono::steady_clock::now() - before < 100ms, "Status retained the Lua callback mutex");
    { std::lock_guard lock(mutex); release = true; } changed.notify_all();
    Check(tick.wait_for(3s) == std::future_status::ready, "Native observation did not drain"); tick.get();
}
/** @summary Proves copied status and off-lane shutdown remain safe when a sink retains the last dispatch owner. */
void InFlightOwnership() {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{}, release{};
    std::atomic_bool reentryRejected{};
    auto scheduler = std::make_unique<ScriptScheduler>();
    ScriptScheduler* observed{};
    scheduler = std::make_unique<ScriptScheduler>([&](const std::string&, const std::string&) {
        try { observed->Remove("fixture.owner"); } catch (const std::exception&) { reentryRejected = true; }
        std::unique_lock lock(mutex); entered = true; changed.notify_all(); changed.wait(lock, [&] { return release; });
    });
    observed = scheduler.get();
    auto runtime = std::make_shared<ScriptRuntime>("fixture.owner", Definition("return {onTick=function() simoder.log('hold') end}"));
    scheduler->Reserve("fixture.owner"); scheduler->Prepare(runtime); runtime->Commit(); scheduler->Put("fixture.owner", runtime);
    {
        std::unique_lock lock(mutex); Check(changed.wait_for(lock, 3s, [&] { return entered; }), "Sink dispatch never started");
    }
    const auto before = std::chrono::steady_clock::now();
    Check(runtime->Status().running && std::chrono::steady_clock::now() - before < 100ms, "Status waited for a sink/VM lane");
    std::weak_ptr<ScriptRuntime> lifetime = runtime;
    runtime.reset(); scheduler->Remove("fixture.owner");
    Check(!lifetime.expired(), "Removal released an executing VM");
    auto shutdown = std::async(std::launch::async, [&] { scheduler->Shutdown(); });
    Check(shutdown.wait_for(20ms) == std::future_status::timeout, "Shutdown did not fence active dispatch");
    { std::lock_guard lock(mutex); release = true; } changed.notify_all();
    Check(shutdown.wait_for(3s) == std::future_status::ready, "Shutdown deadlocked after dispatch release");
    shutdown.get(); scheduler.reset();
    Check(lifetime.expired() && reentryRejected, "Dispatch owner leaked or same-lane mutation was allowed");
}
/** @summary Forces host allocation failure during failing retirement and with retained long diagnostics; VM close must still run. */
void TeardownAllocationFailure() {
    std::atomic_uint closes{};
    ScriptHost host;
    host.lifecycle = [&](ScriptPhase phase, std::uint64_t) { if (phase == ScriptPhase::Close) ++closes; };
    const std::string longError(4096, 'x');
    {
        auto runtime = std::make_unique<ScriptRuntime>("fixture.stop-oom",
            Definition("return {onDisable=function() error('" + longError + "') end}"), host);
        runtime->Commit();
        rejectedHostAllocations = 0; rejectHostAllocation = true;
        runtime->Stop(); runtime.reset();
        rejectHostAllocation = false;
        Check(rejectedHostAllocations > 0 && closes == 1, "Failing onDisable did not exercise allocation failure and close exactly once");
    }
    {
        auto runtime = std::make_unique<ScriptRuntime>("fixture.retained-error",
            Definition("return {onTick=function() error('" + longError + "') end,onDisable=function() end}"), host);
        runtime->Commit(); runtime->Tick(0.05);
        Check(!runtime->Status().running && runtime->Status().diagnostic.size() == 2048, "Long script diagnostics were not captured within their fixed bound");
        rejectedHostAllocations = 0; rejectHostAllocation = true;
        runtime->Stop(); runtime.reset();
        rejectHostAllocation = false;
        Check(closes == 2 && rejectedHostAllocations == 0, "Retained diagnostic reporting allocated during Stop/close");
    }
    {
        auto runtime = std::make_unique<ScriptRuntime>("fixture.destructor-oom",
            Definition("return {onDisable=function() error('" + longError + "') end}"), host);
        runtime->Commit();
        rejectedHostAllocations = 0; rejectHostAllocation = true;
        runtime.reset();
        rejectHostAllocation = false;
        Check(closes == 3 && rejectedHostAllocations > 0, "Destructor stopped before forced VM close on reporting failure");
    }
    // @summary Every reservation must be returned even when error reporting fails during retirement.
    std::vector<std::unique_ptr<ScriptRuntime>> recovered;
    const auto definition = Definition("return {}");
    for (unsigned i = 0; i < 64; ++i) recovered.push_back(std::make_unique<ScriptRuntime>("fixture.oom-recovered", definition));
}
}
/** @summary Executes scheduler/lifecycle tests without installed game resources. @return Zero on success. */
int main() {
    try { ThreadIdentity(); LimitsAndLogs(); Fairness(); CopiedStatus(); InFlightOwnership(); TeardownAllocationFailure(); std::cout << "script scheduler PASS\n"; return 0; }
    catch (const std::exception& error) { rejectHostAllocation = false; std::cerr << error.what() << '\n'; return 1; }
}
