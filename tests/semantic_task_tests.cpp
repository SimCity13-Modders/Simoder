#include "runtime/semantic_tasks.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

/** @summary Test-thread allocation failure injection; production allocation code is unchanged. */
thread_local bool failAllocation{};
/** @summary Test replacement allocation. @param bytes Requested bytes. @return Owned allocation. */
void* operator new(std::size_t bytes) {
    if (failAllocation) throw std::bad_alloc();
    if (void* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
/** @summary Test replacement deallocation. @param memory Owned allocation. */
void operator delete(void* memory) noexcept { std::free(memory); }
/** @summary Sized test deallocation. @param memory Owned allocation. */
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

using namespace sc13::runtime;
namespace {
constexpr std::string_view Wire = R"({"v":1,"schema":"task.v1","value":["u64","18446744073709551615"]})";
/** @summary Requires deterministic behavior. @param value Expected true. @param message Failure detail. */
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
/** @summary Owns one prepared synthetic authority and actual production task implementation. */
struct Fixture final {
    std::shared_ptr<RuntimeScope> scope;
    std::shared_ptr<copied::Storage> storage{std::make_shared<copied::Storage>()};
    SemanticTasks tasks;
    OperationContext context;
    copied::Envelope input;
    /** @summary Creates test-only authority in separately compiled support target. @param domain Optional shared writer domain. */
    explicit Fixture(std::shared_ptr<RuntimeCommitDomain> domain = {})
        : scope(RuntimeScopeTestAccess::Create(std::move(domain))), tasks(scope, storage) {
        Require(scope->CommitActivation(), "activation"); scope->SetReadiness(true);
        context = RuntimeScopeTestAccess::Issue(*scope);
        Require(storage->Decode(Wire, "task.v1", context.owner, input) == copied::Status::Ok, "input decode");
    }
    /** @summary Reserves one task. @param limit Output wire bound. @return Issued opaque handle. */
    TaskHandle Admit(std::size_t limit = 512) {
        TaskHandle task; Require(tasks.Admit(context, input, limit, task) == ScopeResult::Ok, "admission"); return task;
    }
};
/** @summary Writes exact copied input as verified fixture output. @param input Retained input. @param output Preallocated wire. @param size Written bytes. @return Verified postcondition. */
bool Echo(const copied::Envelope& input, std::span<char> output, std::size_t& size) {
    const auto wire = input.Encode();
    if (wire.size() > output.size()) return false;
    std::memcpy(output.data(), wire.data(), wire.size()); size = wire.size(); return true;
}
/** @summary Verifies state/commit distinction, no replay, copied exact64 result and cleanup retention. */
void Lifecycle() {
    Fixture f; const auto baseline = f.storage->Used(); auto task = f.Admit();
    TaskSnapshot snapshot;
    Require(f.tasks.Poll(task, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Queued && !snapshot.committed, "queued phase");
    Require(f.tasks.Begin(task) == ScopeResult::Ok, "begin");
    Require(f.tasks.Poll(task, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Running && !snapshot.committed, "preparation is not commit");
    Require(f.tasks.Commit(task, Echo) == ScopeResult::Ok, "commit");
    Require(f.tasks.Poll(task, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Succeeded && snapshot.committed && snapshot.value.At(0)->bits == UINT64_MAX, "exact64 success");
    int calls{};
    Require(f.tasks.Commit(task, [&](auto&, auto, auto&) { ++calls; return true; }) == ScopeResult::Stale && calls == 0, "terminal replay");
    Require(f.tasks.Cancel(task) == ScopeResult::Busy, "committed terminal cancellation cannot undo");
    Require(f.tasks.Release(task) == ScopeResult::Ok && f.storage->Used() > baseline, "snapshot retained charged result");
    snapshot = {}; Require(f.storage->Used() == baseline, "last snapshot release");
    Require(f.tasks.Release(task) == ScopeResult::Stale, "double release");
}
/** @summary Verifies cancelable preparation versus committed executing work with deterministic barriers. */
void Cancellation() {
    Fixture f; auto task = f.Admit(); Require(f.tasks.Begin(task) == ScopeResult::Ok, "cancel begin");
    Require(f.tasks.Cancel(task) == ScopeResult::Ok, "running-uncommitted cancel");
    Require(f.tasks.Commit(task, Echo) == ScopeResult::Cancelled, "cancelled effect rejected");
    TaskSnapshot snapshot; Require(f.tasks.Poll(task, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Cancelled && !snapshot.committed, "cancel phase");
    Require(f.tasks.Release(task) == ScopeResult::Ok, "cancel release");
    task = f.Admit(); Require(f.tasks.Begin(task) == ScopeResult::Ok, "commit begin");
    std::promise<void> entered, leave; auto release = leave.get_future().share();
    auto commit = std::async(std::launch::async, [&] { return f.tasks.Commit(task, [&](const auto& input, auto output, auto& size) {
        entered.set_value(); release.wait(); return Echo(input, output, size);
    }); });
    entered.get_future().wait();
    Require(f.tasks.Cancel(task) == ScopeResult::Busy && f.tasks.Release(task) == ScopeResult::Busy, "committed lifetime");
    Require(f.tasks.Poll(task, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Running && snapshot.committed, "committed marker");
    leave.set_value(); Require(commit.get() == ScopeResult::Ok, "concurrent completion");
    Require(f.tasks.Release(task) == ScopeResult::Ok, "completed release");
}
/** @summary Verifies revoked/conflicting/missing authority cannot commit or disclose retained results. */
void Authority() {
    Fixture f; auto task = f.Admit(); Require(f.tasks.Begin(task) == ScopeResult::Ok, "revoke begin");
    RuntimeScopeTestAccess::Revoke(*f.scope, f.context.grantGeneration);
    int calls{}; Require(f.tasks.Commit(task, [&](auto&, auto, auto&) { ++calls; return true; }) == ScopeResult::Denied && calls == 0, "revocation before effect");
    TaskSnapshot snapshot; Require(f.tasks.Poll(task, snapshot) == ScopeResult::Denied && !snapshot.value, "revoked queued delivery");
    Require(f.tasks.Release(task) == ScopeResult::Ok, "revoked cleanup");
    Fixture g; task = g.Admit(); Require(g.tasks.Begin(task) == ScopeResult::Ok && g.tasks.Commit(task, Echo) == ScopeResult::Ok, "successful before revoke");
    Require(g.tasks.Poll(task, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Succeeded, "terminal observed");
    RuntimeScopeTestAccess::Revoke(*g.scope, g.context.grantGeneration);
    Require(g.tasks.Poll(task, snapshot) == ScopeResult::Denied && !snapshot.value && !snapshot.committed, "revoked result suppressed");
    Require(g.tasks.Release(task) == ScopeResult::Ok, "successful revoked cleanup");
    Fixture foreign; Require(foreign.tasks.Poll(task, snapshot) == ScopeResult::Denied, "cross-generation handle");
    auto production = std::make_shared<RuntimeScope>(); SemanticTasks absent(production, g.storage); TaskHandle denied;
    Require(absent.Admit(g.context, g.input, 512, denied) == ScopeResult::Denied && !denied, "no production authority");
    Fixture h; task = h.Admit(); Require(h.tasks.Begin(task) == ScopeResult::Ok, "stale begin"); h.scope->RequestStop();
    Require(h.tasks.Commit(task, Echo) != ScopeResult::Ok && h.tasks.Poll(task, snapshot) == ScopeResult::Stale, "epoch invalidation");
    Require(h.tasks.Release(task) == ScopeResult::Ok, "stopped cleanup");
}
/** @summary Verifies uncertain postconditions and malformed/oversized results become immutable Indeterminate. */
void Uncertain() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f; auto task = f.Admit(); Require(f.tasks.Begin(task) == ScopeResult::Ok, "uncertain begin");
        const auto status = f.tasks.Commit(task, [&](const auto&, auto wire, auto& size) {
            if (mode == 0) return false;
            if (mode == 1) throw std::runtime_error("fixture failure");
            if (mode == 2) { size = wire.size() + 1; return true; }
            wire[0] = '!'; size = 1; return true;
        });
        TaskSnapshot snapshot;
        Require(status == ScopeResult::Indeterminate && f.tasks.Poll(task, snapshot) == ScopeResult::Ok && snapshot.state == TaskState::Indeterminate && snapshot.committed && !snapshot.value, "uncertain retained state");
        Require(f.tasks.Commit(task, Echo) == ScopeResult::Stale, "uncertain no replay");
        Require(f.tasks.Release(task) == ScopeResult::Ok, "uncertain cleanup");
    }
}
/** @summary Verifies physical preallocation permits completion while all test-thread heap allocation fails. */
void NoPostcommitAllocation() {
    Fixture f; auto task = f.Admit(); Require(f.tasks.Begin(task) == ScopeResult::Ok, "noalloc begin");
    const SemanticTasks::ValueCallback callback = [](const auto& input, auto output, auto& size) {
        const bool ok = Echo(input, output, size); failAllocation = true; return ok;
    };
    const auto status = f.tasks.Commit(task, callback);
    failAllocation = false;
    TaskSnapshot snapshot;
    Require(status == ScopeResult::Ok && f.tasks.Poll(task, snapshot) == ScopeResult::Ok && snapshot.value, "completion allocated after effect");
    Require(f.tasks.Release(task) == ScopeResult::Ok, "noalloc release");
}
/** @summary Verifies aggregate queue/retained/pool bounds, allocation failures and pool identity rejection before effects. */
void Bounds() {
    auto domain = std::make_shared<RuntimeCommitDomain>(); Fixture f(domain), g(domain);
    std::array<TaskHandle, 8> tasks;
    for (auto& task : tasks) task = f.Admit();
    TaskHandle excess;
    Require(g.tasks.Admit(g.context, g.input, 512, excess) == ScopeResult::Busy && !excess, "eight global pending");
    for (auto task : tasks) Require(f.tasks.Release(task) == ScopeResult::Ok, "pending release");
    Require(g.tasks.Admit(g.context, f.input, 512, excess) == ScopeResult::Invalid, "foreign pool input");
    const auto used = f.storage->Used(); copied::Reservation all;
    Require(f.storage->Allocate(f.context.owner, copied::Storage::OwnerLimit - used, all) == copied::Status::Ok, "quota fill");
    Require(f.tasks.Admit(f.context, f.input, 512, excess) == ScopeResult::Busy && !excess, "quota denial before queue");
    all = {}; Require(f.storage->Used() == used, "reservation release");
    failAllocation = true;
    const auto denied = f.tasks.Admit(f.context, f.input, 512, excess);
    failAllocation = false;
    Require(denied == ScopeResult::Busy && !excess && f.storage->Used() == used, "allocation rollback");
    std::array<TaskHandle, 128> retained;
    for (auto& task : retained) { task = f.Admit(); Require(f.tasks.FinishUncommitted(task, ScopeResult::NotReady) == ScopeResult::Ok, "failed terminal"); }
    Require(f.tasks.Admit(f.context, f.input, 512, excess) == ScopeResult::Busy, "128 retained bound");
    for (auto task : retained) Require(f.tasks.Release(task) == ScopeResult::Ok, "retained cleanup");
    Require(f.storage->Used() == used, "all native task storage released");
}
/** @summary Proves the private atomic preflight runs before commitment and shares the native dependency budget. */
void FencesAndResources() {
    Fixture f;
    auto task = f.Admit();
    Require(f.tasks.Begin(task) == ScopeResult::Ok, "guard begin");
    int effects{};
    const auto result = f.tasks.Commit(task, [&](const auto& input, auto output, auto& size) {
        ++effects; return Echo(input, output, size);
    }, [] { return ScopeResult::Stale; });
    Require(result == ScopeResult::Stale && effects == 0, "stale fence admitted effect");
    TaskSnapshot snapshot;
    Require(f.tasks.Poll(task, snapshot) == ScopeResult::Ok && !snapshot.committed, "fence marked commitment");
    Require(f.tasks.FinishUncommitted(task, result) == ScopeResult::Ok && f.tasks.Release(task) == ScopeResult::Ok, "fence cleanup");
    task = f.Admit();
    Require(f.tasks.Begin(task) == ScopeResult::Ok, "throwing fence begin");
    Require(f.tasks.Commit(task, Echo, []() -> ScopeResult { throw std::runtime_error("fixture fence"); }) == ScopeResult::Denied,
        "throwing fence escaped commit boundary");
    Require(f.tasks.Poll(task, snapshot) == ScopeResult::Ok && !snapshot.committed, "throwing fence marked commitment");
    Require(f.tasks.FinishUncommitted(task, ScopeResult::Denied) == ScopeResult::Ok && f.tasks.Release(task) == ScopeResult::Ok,
        "throwing fence cleanup");
    auto dependency = std::make_shared<int>(1);
    Require(f.scope->RetainResource(dependency), "native dependency reservation");
    for (int i = 0; i < 63; ++i) Require(f.scope->ReserveRegistration(), "shared continuation reservation");
    Require(!f.scope->ReserveRegistration() && !f.scope->RetainResource(dependency), "shared 64 resource cap");
    f.scope->ReleaseRegistration();
    Require(f.scope->ReserveRegistration(), "slot reuse");
    for (int i = 0; i < 63; ++i) f.scope->ReleaseRegistration();
    f.scope->RequestStop();
    Require(!f.scope->ReserveRegistration(), "registration after stop");
}
/** @summary Settle revoked native dispatch attempts without granting result access or pinning the shared pending lane. */
void RevokedPendingCleanup() {
    auto domain = std::make_shared<RuntimeCommitDomain>();
    Fixture revoked(domain), fresh(domain);
    std::array<TaskHandle, 8> retained;
    for (auto& task : retained) task = revoked.Admit();
    RuntimeScopeTestAccess::Revoke(*revoked.scope, revoked.context.grantGeneration);
    for (const auto task : retained) {
        Require(revoked.tasks.Begin(task) == ScopeResult::Denied, "revoked preparation admitted");
        Require(revoked.tasks.FinishUncommitted(task, ScopeResult::Denied) == ScopeResult::Ok, "revoked native cleanup rejected");
        TaskSnapshot hidden;
        Require(revoked.tasks.Poll(task, hidden) == ScopeResult::Denied && !hidden.value, "cleanup disclosed revoked payload");
    }
    auto admitted = fresh.Admit();
    Require(fresh.tasks.Release(admitted) == ScopeResult::Ok, "fresh shared pending cleanup");
    for (const auto task : retained) Require(revoked.tasks.Release(task) == ScopeResult::Ok, "revoked retained owner cleanup");
}
}
/** @summary Runs deterministic copied-task authority/lifetime/storage regression fixtures. @return Zero only for all passed groups. */
int main() {
    try { Lifecycle(); Cancellation(); Authority(); Uncertain(); NoPostcommitAllocation(); Bounds(); FencesAndResources(); RevokedPendingCleanup(); std::cout << "semantic tasks: 8 groups PASS\n"; return 0; }
    catch (const std::exception& error) { failAllocation = false; std::cerr << error.what() << '\n'; return 1; }
}
