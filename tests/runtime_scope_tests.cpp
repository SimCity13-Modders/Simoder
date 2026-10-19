#include "runtime/runtime_scope.hpp"
#include "platform/runtime_owner.hpp"

#include <array>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <string>

using namespace sc13::runtime;
using namespace std::chrono_literals;
namespace {
/** @summary Fails an exact behavioral expectation. @param value Observed condition. @param message Failure diagnostic. */
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
/** @summary Constructs a locally prepared synthetic scope, without any production credential issuer.
 * @return Active test-only scope with factual runtime readiness. */
std::unique_ptr<RuntimeScope> Active() {
    auto scope = RuntimeScopeTestAccess::Create();
    Require(scope->CommitActivation(), "synthetic postcommit failed");
    scope->SetReadiness(true);
    return scope;
}
/** @summary Returns copied input as deterministic bounded result. @param input Borrowed owned input. @return Copied bytes. */
std::vector<std::byte> Echo(std::span<const std::byte> input) { return {input.begin(), input.end()}; }
/** @summary Verifies precommit, mandatory, session/entity, context integrity and deadline gates. */
void Gates() {
    RuntimeScope production;
    ScopeHandle handle{123, 456};
    Require(!production.CommitActivation(), "missing owner activation accepted");
    const auto name = L"Global\\Simoder.TEST.U1.Scope." +
        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
    auto owner = sc13::platform::RuntimeOwnerTestAccess::Acquire(name);
    Require(owner && owner->IsAcquired(), "provider fixture owner unavailable");
    RuntimeScope missingProvider(owner);
    Require(missingProvider.CommitActivation(), "actual owner postcommit failed");
    missingProvider.SetReadiness(true);
    const auto observed = missingProvider.Current();
    OperationContext untrusted;
    untrusted.owner = observed.generation; untrusted.epoch = observed.epoch; untrusted.grantGeneration = 1;
    untrusted.origin = AgentOrigin{observed.generation, 1, 7, 13, 17, 11, 1};
    untrusted.opcode = 1; untrusted.deadline = std::chrono::steady_clock::now() + 1min;
    Require(missingProvider.Queue(untrusted, {}, handle) == ScopeResult::Denied, "missing production authority admitted claimed principal");
    auto probe = RuntimeScopeTestAccess::Create();
    auto context = RuntimeScopeTestAccess::Issue(*probe);
    Require(probe->Queue(context, {}, handle) == ScopeResult::NotReady && handle == ScopeHandle{}, "precommit admission");
    Require(probe->CommitActivation(), "postcommit failed");
    Require(probe->Queue(context, {}, handle) == ScopeResult::NotReady, "readiness bypass");
    probe->SetReadiness(true);
    Require(probe->Queue(context, {}, handle) == ScopeResult::Ok, "entity grant denied");
    Require(probe->Release(context.owner, handle) == ScopeResult::Ok, "entity cleanup");
    const auto session = RuntimeScopeTestAccess::Issue(*probe, true, false);
    Require(probe->Queue(session, {}, handle) == ScopeResult::Ok, "session grant denied");
    Require(probe->Release(session.owner, handle) == ScopeResult::Ok, "session cleanup");
    const auto noPermission = RuntimeScopeTestAccess::Issue(*probe, false, false);
    Require(probe->Queue(noPermission, {}, handle) == ScopeResult::Denied, "absent permission accepted");
    const auto missingMandatory = RuntimeScopeTestAccess::Issue(*probe, true, true, false);
    Require(probe->Queue(missingMandatory, {}, handle) == ScopeResult::Denied, "full permission bypassed mandatory gates");
    const auto expired = RuntimeScopeTestAccess::Issue(*probe, true, false, true, 1, -1ms);
    Require(probe->Queue(expired, {}, handle) == ScopeResult::Denied, "expired lease accepted");
    const auto city = RuntimeScopeTestAccess::Issue(*probe, true, false, true, 1, 1min, CityReady);
    Require(probe->Queue(city, {}, handle) == ScopeResult::NotReady, "runtime inferred city readiness");
    probe->SetReadiness(true, false, true);
    Require(probe->Queue(city, {}, handle) == ScopeResult::Ok, "observed city denied");
    Require(probe->Release(city.owner, handle) == ScopeResult::Ok, "city cleanup");
    for (int field = 0; field != 11; ++field) {
        auto forged = context;
        switch (field) {
        case 0: ++forged.owner; break; case 1: ++forged.epoch; break;
        case 2: ++forged.grantGeneration; break; case 3: ++std::get<AgentOrigin>(forged.origin).actor; break;
        case 4: ++std::get<AgentOrigin>(forged.origin).consentSession; break; case 5: ++std::get<AgentOrigin>(forged.origin).entity; break;
        case 6: ++forged.revision; break; case 7: ++forged.opcode; break;
        case 8: forged.readiness = CityReady; break; case 9: forged.deadline += 1s; break;
        default: forged.readiness = 0; break;
        }
        Require(probe->Queue(forged, {}, handle) != ScopeResult::Ok && handle == ScopeHandle{}, "forged context accepted");
    }
    // @summary Even test code cannot turn a production-constructed scope into an issuer.
    Require(RuntimeScopeTestAccess::Issue(production).owner == 0, "test issuer adopted production scope");
}
/** @summary Verifies commit revision, no replay, confidentiality, short buffers and post-revocation cleanup. */
void Commits() {
    auto scope = Active();
    const auto context = RuntimeScopeTestAccess::Issue(*scope);
    const std::array bytes{std::byte{42}, std::byte{17}};
    ScopeHandle first{}, conflict{};
    Require(scope->Queue(context, bytes, first) == ScopeResult::Ok, "first queue");
    Require(scope->Queue(context, bytes, conflict) == ScopeResult::Ok, "conflict queue");
    int calls{};
    Require(scope->Commit(context, first, [&](auto input) { ++calls; return Echo(input); }) == ScopeResult::Ok, "first commit");
    Require(scope->Commit(context, conflict, Echo) == ScopeResult::Conflict, "conflicting writer accepted");
    auto forged = context;
    forged.revision = scope->Current().revision;
    ScopeHandle unauthorized{};
    Require(scope->Queue(forged, {}, unauthorized) == ScopeResult::Denied, "old context revision forged");
    Require(scope->Commit(context, first, [&](auto input) { ++calls; return Echo(input); }) == ScopeResult::Stale && calls == 1, "commit replay");
    std::vector<std::byte> output{std::byte{99}};
    Require(scope->Poll(context, first, 1, output) == ScopeResult::BufferSmall && output.empty(), "short buffer leaked result");
    auto foreign = context; ++std::get<AgentOrigin>(foreign.origin).actor;
    Require(scope->Poll(foreign, first, 100, output) == ScopeResult::Denied && output.empty(), "foreign principal read result");
    Require(scope->Poll(context, first, 2, output) == ScopeResult::Ok && output == std::vector<std::byte>(bytes.begin(), bytes.end()), "owned result mismatch");
    const auto fresh = RuntimeScopeTestAccess::Issue(*scope);
    ScopeHandle freshOperation{};
    Require(scope->Queue(fresh, {}, freshOperation) == ScopeResult::Ok, "fresh issued revision denied");
    RuntimeScopeTestAccess::Revoke(*scope, fresh.grantGeneration);
    Require(scope->Commit(fresh, freshOperation, Echo) == ScopeResult::Denied, "revoke before commit ignored");
    RuntimeScopeTestAccess::Revoke(*scope, context.grantGeneration);
    Require(scope->Poll(context, first, 100, output) == ScopeResult::Denied && output.empty(), "revoked result leaked");
    Require(scope->Release(context.owner + 1, first) == ScopeResult::Denied, "foreign result cleanup");
    Require(scope->Release(context.owner, first) == ScopeResult::Ok, "revoked owner cleanup blocked");
    Require(scope->Release(context.owner, first) == ScopeResult::Stale, "released result reused");
    auto other = Active();
    Require(other->Current().generation != context.owner, "owner ABA generation reuse");
    Require(other->Release(other->Current().generation, conflict) == ScopeResult::Denied, "cross-owner operation reused");
}
/** @summary Verifies stop/reentrant commit safety, bounded drain and retained callback/dependency lifetime. */
void RunningStop() {
    auto scope = Active();
    const auto context = RuntimeScopeTestAccess::Issue(*scope);
    ScopeHandle operation{}, queued{};
    Require(scope->Queue(context, {}, operation) == ScopeResult::Ok, "running queue");
    Require(scope->Queue(context, {}, queued) == ScopeResult::Ok, "queued stop case");
    auto resource = std::make_shared<int>(12);
    const std::weak_ptr<int> retained = resource;
    Require(scope->RetainResource(resource), "resource retain"); resource.reset();
    std::promise<void> entered, finish;
    auto finishSignal = finish.get_future().share();
    auto worker = std::async(std::launch::async, [&] {
        return scope->Commit(context, operation, [&](auto) {
            Require(RuntimeScopeTestAccess::Revoke(*scope, context.grantGeneration) == ScopeResult::Busy,
                "recursive revocation claimed success or blocked writer");
            Require(scope->Commit(context, queued, Echo) == ScopeResult::Busy, "reentrant commit lane did not reject");
            Require(scope->Release(context.owner, operation) == ScopeResult::Busy, "running result prematurely released");
            entered.set_value();
            Require(finishSignal.wait_for(10s) == std::future_status::ready, "test release timed out");
            return std::vector<std::byte>{std::byte{21}};
        });
    });
    Require(entered.get_future().wait_for(5s) == std::future_status::ready, "worker did not enter");
    Require(scope->Cancel(context, operation) == ScopeResult::Busy, "running mutation falsely cancelled");
    Require(scope->Commit(context, queued, Echo) == ScopeResult::Busy, "concurrent commit lane admitted");
    const auto epoch = scope->Current().epoch;
    scope->RequestStop(); scope->RequestStop();
    Require(scope->Current().epoch == epoch + 1, "stop not idempotent");
    Require(!scope->DrainFor(0ms) && !retained.expired(), "timeout released running dependencies");
    Require(!scope->CommitActivation() && !scope->RetainResource(std::make_shared<int>(1)), "stop reopened admission");
    finish.set_value();
    Require(worker.get() == ScopeResult::Ok, "stop represented committed work as undone");
    Require(scope->Commit(context, queued, Echo) == ScopeResult::Cancelled, "queued work not cancelled");
    std::vector<std::byte> output;
    Require(scope->Poll(context, operation, 100, output) != ScopeResult::Ok && output.empty(), "stopped result leaked");
    Require(scope->Release(context.owner, operation) == ScopeResult::Ok, "stopped cleanup failed");
    Require(scope->DrainFor(0ms) && retained.expired() && scope->Current().drained, "safe drain did not release dependency");
}
/** @summary Verifies bounded queued/retained storage, copied payloads and ambiguous committed failures. */
void BoundsAndFailures() {
    auto scope = Active();
    const auto context = RuntimeScopeTestAccess::Issue(*scope);
    std::array<ScopeHandle, 8> pending{};
    for (auto& handle : pending) Require(scope->Queue(context, {}, handle) == ScopeResult::Ok, "pending capacity too small");
    ScopeHandle extra{};
    Require(scope->Queue(context, {}, extra) == ScopeResult::Busy, "pending capacity exceeded");
    for (const auto handle : pending) Require(scope->Release(context.owner, handle) == ScopeResult::Ok, "pending cleanup");
    std::vector<std::byte> oversized(1024 * 1024 + 1);
    Require(scope->Queue(context, oversized, extra) == ScopeResult::Invalid, "oversized input retained");
    std::vector<std::byte> copied{std::byte{1}};
    Require(scope->Queue(context, copied, extra) == ScopeResult::Ok, "copied queue"); copied[0] = std::byte{2};
    Require(scope->Commit(context, extra, [](auto input) -> std::vector<std::byte> {
        Require(input[0] == std::byte{1}, "caller input retained by reference");
        throw std::runtime_error("synthetic operation exception");
    }) == ScopeResult::Indeterminate, "exception falsely undone");
    Require(scope->Current().revision == 1 && scope->Commit(context, extra, Echo) == ScopeResult::Stale, "exception replay");
    const auto fullContext = RuntimeScopeTestAccess::Issue(*scope);
    std::vector<std::byte> envelope(1024 * 1024, std::byte{35});
    Require(scope->Queue(fullContext, envelope, extra) == ScopeResult::Ok, "valid envelope silently narrowed");
    Require(scope->Commit(fullContext, extra, Echo) == ScopeResult::Ok, "bounded envelope commit");
    std::vector<std::byte> result;
    Require(scope->Poll(fullContext, extra, envelope.size(), result) == ScopeResult::Ok && result == envelope, "bounded envelope result");
    const auto overflowContext = RuntimeScopeTestAccess::Issue(*scope);
    Require(scope->Queue(overflowContext, {}, extra) == ScopeResult::Ok, "result overflow queue");
    Require(scope->Commit(overflowContext, extra, [](auto) { return std::vector<std::byte>(1024 * 1024 + 1); }) == ScopeResult::Indeterminate,
            "oversized committed result falsely successful");
    Require(scope->Poll(overflowContext, extra, 1024 * 1024 + 1, result) == ScopeResult::Indeterminate && result.empty(), "oversized result retained");
    auto bounded = Active();
    const auto allowed = RuntimeScopeTestAccess::Issue(*bounded);
    for (int index = 0; index != 128; ++index) {
        Require(bounded->Queue(allowed, {}, extra) == ScopeResult::Ok, "retained capacity too small");
        Require(bounded->Cancel(allowed, extra) == ScopeResult::Ok, "queued cancel failed");
    }
    Require(bounded->Queue(allowed, {}, extra) == ScopeResult::Busy, "retained capacity exceeded");
}
/** @summary Verifies callback-initiated stop and reentrant dependency destruction without authority-lock deadlock. */
void ReentrantStop() {
    auto scope = Active();
    const auto context = RuntimeScopeTestAccess::Issue(*scope);
    ScopeHandle operation{};
    Require(scope->Queue(context, {}, operation) == ScopeResult::Ok, "reentrant stop queue");
    bool destroyed{}, observedStopped{};
    auto resource = std::shared_ptr<void>(new int(1), [&](void* value) {
        observedStopped = scope->Current().stopping;
        destroyed = true;
        delete static_cast<int*>(value);
    });
    Require(scope->RetainResource(resource), "reentrant dependency retain"); resource.reset();
    Require(scope->Commit(context, operation, [&](auto) {
        scope->RequestStop();
        Require(!scope->DrainFor(0ms) && !destroyed, "callback drained its executing dependency");
        return std::vector<std::byte>{};
    }) == ScopeResult::Ok, "callback stop failed");
    Require(scope->DrainFor(0ms) && destroyed && observedStopped, "resource destructor could not reenter safely");
}
/** @summary Checks that a legacy reservation shares revision and recursive exclusion with native tasks. */
void LegacyWriterDomain() {
    auto domain = std::make_shared<RuntimeCommitDomain>();
    auto scope = RuntimeScopeTestAccess::Create(domain);
    Require(scope->CommitActivation(), "legacy-domain fixture activation");
    scope->SetReadiness(true);
    const auto context = RuntimeScopeTestAccess::Issue(*scope);
    ScopeHandle queued{};
    Require(scope->Queue(context, {}, queued) == ScopeResult::Ok, "legacy-domain queue");
    {
        auto writer = domain->ReserveWriter(true);
        Require(writer.Acquired() && writer.CanAdvance(), "legacy lane admission");
        auto nested = domain->ReserveWriter(true);
        Require(!nested.Acquired(), "blocking legacy reentry deadlocked or admitted");
        Require(scope->Commit(context, queued, Echo) == ScopeResult::Busy, "task bypassed legacy reservation");
        auto competing = std::async(std::launch::async, [&] {
            auto reservation = domain->ReserveWriter();
            return reservation.Acquired();
        });
        Require(!competing.get(), "new operational admission waited or bypassed legacy lane");
        Require(writer.Advance() == 1, "legacy revision was not exact");
    }
    Require(scope->Commit(context, queued, Echo) == ScopeResult::Conflict,
        "task admitted stale revision after legacy publication");
    domain->RequestStop();
    auto stopped = domain->ReserveWriter(true);
    Require(stopped.Acquired() && !stopped.CanAdvance() && stopped.Advance() == 0,
        "stopped domain admitted ordinary publication");
    Require(stopped.AdvanceRollback() == 2 && !stopped.CanAdvance() && stopped.Advance() == 0,
        "rollback either lost revision capacity or reopened ordinary admission");
}

/** @summary Verifies one shared core writer domain rejects concurrent/conflicting cross-module writers. */
void SharedDomain() {
    auto domain = std::make_shared<RuntimeCommitDomain>();
    auto first = RuntimeScopeTestAccess::Create(domain);
    auto second = RuntimeScopeTestAccess::Create(domain);
    Require(first->CommitActivation() && second->CommitActivation(), "shared domain activation");
    first->SetReadiness(true); second->SetReadiness(true);
    const auto left = RuntimeScopeTestAccess::Issue(*first);
    const auto right = RuntimeScopeTestAccess::Issue(*second);
    ScopeHandle a{}, b{};
    Require(first->Queue(left, {}, a) == ScopeResult::Ok && second->Queue(right, {}, b) == ScopeResult::Ok, "shared writer queue");
    std::promise<void> entered, release;
    const auto released = release.get_future().share();
    auto writer = std::async(std::launch::async, [&] {
        return first->Commit(left, a, [&](auto input) {
            Require(second->Commit(right, b, Echo) == ScopeResult::Busy, "same-thread cross-scope domain reentry admitted");
            entered.set_value();
            Require(released.wait_for(10s) == std::future_status::ready, "shared writer release timeout");
            return Echo(input);
        });
    });
    Require(entered.get_future().wait_for(5s) == std::future_status::ready, "shared writer never entered");
    const auto concurrent = second->Commit(right, b, Echo);
    release.set_value();
    Require(writer.get() == ScopeResult::Ok && concurrent == ScopeResult::Busy, "cross-scope writer not serialized");
    Require(second->Commit(right, b, Echo) == ScopeResult::Conflict, "cross-scope old revision accepted");
    const auto fresh = RuntimeScopeTestAccess::Issue(*second);
    ScopeHandle next{};
    Require(second->Queue(fresh, {}, next) == ScopeResult::Ok && second->Commit(fresh, next, Echo) == ScopeResult::Ok,
            "fresh shared revision rejected");
    Require(first->Current().revision == 2 && second->Current().revision == 2, "writer domain revision diverged");
    Require(second->Release(right.owner, b) == ScopeResult::Ok, "conflicting queued cleanup");
    auto boundedDomain = std::make_shared<RuntimeCommitDomain>();
    auto one = RuntimeScopeTestAccess::Create(boundedDomain);
    auto two = RuntimeScopeTestAccess::Create(boundedDomain);
    Require(one->CommitActivation() && two->CommitActivation(), "bounded domain activation");
    one->SetReadiness(true); two->SetReadiness(true);
    const auto oneContext = RuntimeScopeTestAccess::Issue(*one);
    const auto twoContext = RuntimeScopeTestAccess::Issue(*two);
    std::array<ScopeHandle, 8> pending{};
    for (std::size_t index = 0; index != pending.size(); ++index) {
        auto& scope = index % 2 == 0 ? one : two;
        const auto& context = index % 2 == 0 ? oneContext : twoContext;
        Require(scope->Queue(context, {}, pending[index]) == ScopeResult::Ok, "shared pending reservation");
    }
    Require(two->Queue(twoContext, {}, next) == ScopeResult::Busy, "shared pending cap exceeded");
    one->RequestStop();
    Require(two->Queue(twoContext, {}, next) == ScopeResult::Ok, "stopped owner did not release shared queued capacity");
    boundedDomain->RequestStop(); boundedDomain->RequestStop();
    Require(!two->CommitActivation(), "root domain stop allowed child activation");
    Require(two->Queue(twoContext, {}, next) == ScopeResult::NotReady, "root domain stop allowed child queue");
    Require(two->Commit(twoContext, pending[1], Echo) == ScopeResult::NotReady, "root domain stop allowed queued child commit");
    Require(two->Release(twoContext.owner, pending[1]) == ScopeResult::Ok, "root domain stop blocked owned cleanup");
}
/** @summary Creates one exact ordinary-script fixture review without fabricated agent credentials.
 * @param scope Native fixture owner. @return Review bound to its isolated loaded-content pin. */
RuntimeScopeTestAccess::Review ScriptReview(RuntimeScope& scope) {
    RuntimeScopeTestAccess::Review review;
    review.origin = OrdinaryScriptOrigin{scope.Current().generation, 1};
    return review;
}
/** @summary Verifies one-shot immutable prompts, roles, effective restrictions and nonrenewable reservations. */
void TypedDecisions() {
    auto scope = Active();
    auto review = ScriptReview(*scope);
    const auto prompt = RuntimeScopeTestAccess::ChallengeDecision(*scope, review);
    const auto ordinary = RuntimeScopeTestAccess::Decide(*scope, prompt, review);
    ScopeHandle task{};
    Require(ordinary.owner && std::holds_alternative<OrdinaryScriptOrigin>(ordinary.origin), "ordinary origin fabricated agent login");
    Require(scope->Queue(ordinary, {}, task) == ScopeResult::Ok, "ordinary script requires agent consent");
    Require(!RuntimeScopeTestAccess::Decide(*scope, prompt, review).owner, "human action replay issued grant");
    Require(scope->Commit(ordinary, task, Echo) == ScopeResult::Ok, "ordinary typed publication failed");
    const auto fresh = RuntimeScopeTestAccess::Reserve(*scope, ordinary);
    Require(fresh.owner && fresh.revision == ordinary.revision + 1 && fresh.deadline == ordinary.deadline &&
        fresh.decision == ordinary.decision && fresh.grantGeneration == ordinary.grantGeneration &&
        fresh.reservation != ordinary.reservation, "fresh reservation renewed consent or rewrote grant");
    Require(scope->Queue(ordinary, {}, task) == ScopeResult::Conflict, "old reservation silently refreshed");
    Require(scope->Queue(fresh, {}, task) == ScopeResult::Ok, "fresh same-decision reservation rejected");
    Require(scope->Release(fresh.owner, task) == ScopeResult::Ok, "typed fresh cleanup");
    for (int change = 0; change != 10; ++change) {
        auto altered = review;
        const auto challenge = RuntimeScopeTestAccess::ChallengeDecision(*scope, review);
        switch (change) {
        case 0: ++std::get<OrdinaryScriptOrigin>(altered.origin).owner; break;
        case 1: ++std::get<OrdinaryScriptOrigin>(altered.origin).content; break;
        case 2: ++altered.revision; break;
        case 3: ++altered.action; break;
        case 4: altered.approved = 0; break;
        case 5: altered.requested = 3; break;
        case 6: altered.required = 0; break;
        case 7: altered.expires += 1s; break;
        case 8: altered.origin = AgentOrigin{ordinary.owner, 1, 7, 13, 17, 11, 1}; break;
        default: altered.readiness = CityReady; break;
        }
        Require(!RuntimeScopeTestAccess::Decide(*scope, challenge, altered).owner, "changed review accepted");
        Require(!RuntimeScopeTestAccess::Decide(*scope, challenge, review).owner, "failed prompt replay accepted");
    }
    auto forged = fresh;
    forged.origin = AgentOrigin{fresh.owner, 1, 7, 13, 17, 11, 1};
    Require(scope->Queue(forged, {}, task) == ScopeResult::Denied, "role substitution accepted");
    forged = fresh; ++forged.grantRevision;
    Require(scope->Queue(forged, {}, task) == ScopeResult::Denied, "grant revision forged");
    forged = fresh; ++forged.decision.action;
    Require(scope->Queue(forged, {}, task) == ScopeResult::Denied, "decision action forged");
    auto invalid = review; invalid.origin = UnqualifiedOrigin{};
    Require(!RuntimeScopeTestAccess::ChallengeDecision(*scope, invalid).serial, "unqualified role admitted");
    invalid = review; invalid.sessionAllowed = true;
    Require(!RuntimeScopeTestAccess::ChallengeDecision(*scope, invalid).serial, "ordinary role accepted agent-only predicate");
    invalid = review; std::get<OrdinaryScriptOrigin>(invalid.origin).content = 2;
    Require(!RuntimeScopeTestAccess::ChallengeDecision(*scope, invalid).serial, "foreign loaded pin admitted");
    const auto superseded = RuntimeScopeTestAccess::ChallengeDecision(*scope, review);
    auto revised = review; ++revised.revision;
    const auto latest = RuntimeScopeTestAccess::ChallengeDecision(*scope, revised);
    Require(!RuntimeScopeTestAccess::ChallengeDecision(*scope, review).serial, "review revision rolled backward");
    Require(latest.serial && !RuntimeScopeTestAccess::Decide(*scope, superseded, review).owner,
        "superseded unchanged prompt accepted");
    Require(RuntimeScopeTestAccess::Decide(*scope, latest, revised).owner != 0, "current prompt rejected");
    auto optional = Active();
    auto restricted = ScriptReview(*optional); restricted.required = 0; restricted.approved = 0; restricted.supportsRestricted = true;
    const auto limited = RuntimeScopeTestAccess::Decide(*optional,
        RuntimeScopeTestAccess::ChallengeDecision(*optional, restricted), restricted);
    const auto view = RuntimeScopeTestAccess::Inspect(*optional, limited);
    Require(limited.owner && view.restricted && !view.capabilities && !view.admissionClosed, "restricted inspection lied");
    Require(optional->Queue(limited, {}, task) == ScopeResult::Denied, "optional denied capability executed");
    auto required = ScriptReview(*optional); required.approved = 0;
    Require(!RuntimeScopeTestAccess::Decide(*optional,
        RuntimeScopeTestAccess::ChallengeDecision(*optional, required), required).owner, "required denial activated owner");
    Require(RuntimeScopeTestAccess::Inspect(*optional, limited).admissionClosed, "required denial left owner open");
    auto expiry = Active(); auto brief = ScriptReview(*expiry);
    brief.expires = std::chrono::steady_clock::now() + 30ms;
    const auto stale = RuntimeScopeTestAccess::ChallengeDecision(*expiry, brief);
    std::this_thread::sleep_for(40ms);
    Require(!RuntimeScopeTestAccess::Decide(*expiry, stale, brief).owner, "expired prompt issued authority");
}
/** @summary Verifies retained agent role, scoped later participants, channel loss and session/entity intersection. */
void AgentDecisions() {
    auto scope = Active();
    auto review = ScriptReview(*scope);
    review.origin = AgentOrigin{scope->Current().generation, 1, 7, 13, 17, 11, 1};
    review.required = 0; review.sessionAllowed = true; review.entityAllowed = true; review.allParticipants = true;
    const auto original = RuntimeScopeTestAccess::Decide(*scope,
        RuntimeScopeTestAccess::ChallengeDecision(*scope, review), review);
    const auto joined = RuntimeScopeTestAccess::Join(*scope, original, 23, 29);
    ScopeHandle operation{};
    Require(joined.owner && std::get<AgentOrigin>(joined.origin).consentSession == 17,
        "later participant got replacement consent session");
    Require(scope->Queue(joined, {}, operation) == ScopeResult::Ok, "reviewed future participant denied");
    Require(scope->Release(joined.owner, operation) == ScopeResult::Ok, "participant cleanup");
    for (int field = 0; field != 5; ++field) {
        auto forged = joined; auto& origin = std::get<AgentOrigin>(forged.origin);
        switch (field) { case 0: ++origin.actor; break; case 1: ++origin.channel; break;
        case 2: ++origin.consentSession; break; case 3: ++origin.extent; break; default: ++origin.entity; break; }
        Require(scope->Queue(forged, {}, operation) == ScopeResult::Denied, "forged authenticated agent origin");
    }
    auto downgraded = joined; downgraded.origin = OrdinaryScriptOrigin{joined.owner, 1};
    Require(scope->Queue(downgraded, {}, operation) == ScopeResult::Denied, "agent causal downgrade accepted");
    auto secondReview = review; std::get<AgentOrigin>(secondReview.origin).actor = 37;
    secondReview.entityAllowed = false;
    const auto sameSession = RuntimeScopeTestAccess::Decide(*scope,
        RuntimeScopeTestAccess::ChallengeDecision(*scope, secondReview), secondReview);
    auto unrelatedReview = review; std::get<AgentOrigin>(unrelatedReview.origin).consentSession = 41;
    const auto unrelated = RuntimeScopeTestAccess::Decide(*scope,
        RuntimeScopeTestAccess::ChallengeDecision(*scope, unrelatedReview), unrelatedReview);
    Require(RuntimeScopeTestAccess::RevokeSession(*scope, 17) == ScopeResult::Ok, "session revoke failed");
    Require(scope->Queue(sameSession, {}, operation) == ScopeResult::Denied &&
        !RuntimeScopeTestAccess::Reserve(*scope, sameSession).owner, "sibling grant retained revoked consent session");
    Require(scope->Queue(unrelated, {}, operation) == ScopeResult::Ok, "unrelated consent session revoked");
    Require(scope->Release(unrelated.owner, operation) == ScopeResult::Ok, "unrelated session cleanup");
    Require(scope->Queue(joined, {}, operation) == ScopeResult::Denied, "joined actor borrowed another entity approval");
    Require(scope->Queue(original, {}, operation) == ScopeResult::Denied, "old effective grant revision retained admission");
    const auto fallback = RuntimeScopeTestAccess::Reserve(*scope, original);
    Require(fallback.owner && fallback.grantRevision == original.grantRevision + 1 && fallback.deadline == original.deadline,
        "entity fallback failed to reserve without consent renewal");
    Require(scope->Queue(fallback, {}, operation) == ScopeResult::Ok, "independent entity consent lost to session revoke");
    Require(scope->Release(fallback.owner, operation) == ScopeResult::Ok, "entity fallback cleanup");
    Require(RuntimeScopeTestAccess::Disconnect(*scope, 13) == ScopeResult::Ok, "disconnect failed");
    Require(scope->Queue(fallback, {}, operation) == ScopeResult::Denied, "disconnected channel retained admission");
    Require(!RuntimeScopeTestAccess::Join(*scope, original, 7, 31).owner, "disconnect inherited replacement channel");
    Require(!RuntimeScopeTestAccess::Inspect(*scope, fallback).capabilities, "disconnected inspection advertised access");
}
/** @summary Verifies bounded decision/reservation retention, denial variants and actual consent expiry. */
void DecisionBudgetsAndExpiry() {
    auto pending = Active(); auto review = ScriptReview(*pending);
    for (int index = 0; index != 128; ++index)
        Require(RuntimeScopeTestAccess::ChallengeDecision(*pending, review).serial != 0, "challenge bound too small");
    Require(!RuntimeScopeTestAccess::ChallengeDecision(*pending, review).serial, "challenge capacity exceeded");
    auto replacement = review; ++replacement.revision;
    Require(RuntimeScopeTestAccess::ChallengeDecision(*pending, replacement).serial != 0,
        "capacity prevented prompt supersession");
    auto scope = Active(); review = ScriptReview(*scope);
    const auto context = RuntimeScopeTestAccess::Decide(*scope,
        RuntimeScopeTestAccess::ChallengeDecision(*scope, review), review);
    for (int index = 1; index != 128; ++index)
        Require(RuntimeScopeTestAccess::Reserve(*scope, context).owner != 0, "reservation bound too small");
    Require(!RuntimeScopeTestAccess::Reserve(*scope, context).owner, "aggregate reservation capacity exceeded");
    auto denied = review; denied.approved = 0;
    Require(!RuntimeScopeTestAccess::Decide(*scope,
        RuntimeScopeTestAccess::ChallengeDecision(*scope, denied), denied).owner &&
        RuntimeScopeTestAccess::Inspect(*scope, context).admissionClosed,
        "exhausted reservation capacity prevented required denial");
    auto unsupported = Active(); review = ScriptReview(*unsupported);
    review.required = 0; review.approved = 0;
    Require(!RuntimeScopeTestAccess::Decide(*unsupported,
        RuntimeScopeTestAccess::ChallengeDecision(*unsupported, review), review).owner,
        "unsupported restricted mode activated owner");
    auto required = Active(); review = ScriptReview(*required);
    const auto requiredContext = RuntimeScopeTestAccess::Decide(*required,
        RuntimeScopeTestAccess::ChallengeDecision(*required, review), review);
    Require(RuntimeScopeTestAccess::Revoke(*required, requiredContext.grantGeneration) == ScopeResult::Ok,
        "required withdrawal failed");
    Require(RuntimeScopeTestAccess::Inspect(*required, requiredContext).admissionClosed,
        "required withdrawal left operational owner open");
    auto expires = Active(); review = ScriptReview(*expires);
    review.expires = std::chrono::steady_clock::now() + 60ms;
    const auto brief = RuntimeScopeTestAccess::Decide(*expires,
        RuntimeScopeTestAccess::ChallengeDecision(*expires, review), review);
    ScopeHandle task{}; int effects{};
    Require(expires->Queue(brief, {}, task) == ScopeResult::Ok, "short consent queue failed");
    std::this_thread::sleep_for(80ms);
    Require(expires->Commit(brief, task, [&](auto) { ++effects; return std::vector<std::byte>{}; }) == ScopeResult::Denied && !effects,
        "expired consent executed effects");
    Require(!RuntimeScopeTestAccess::Reserve(*expires, brief).owner, "expired consent renewed");
    Require(expires->Release(brief.owner, task) == ScopeResult::Ok, "expired task owner cleanup blocked");
}
/** @summary Verifies revocation closes outstanding prompts before they can resurrect withdrawn authority. */
void PendingDecisionRevocation() {
    auto scope = Active(); auto review = ScriptReview(*scope); review.required = 0;
    const auto granted = RuntimeScopeTestAccess::Decide(*scope,
        RuntimeScopeTestAccess::ChallengeDecision(*scope, review), review);
    const auto old = RuntimeScopeTestAccess::ChallengeDecision(*scope, review);
    Require(RuntimeScopeTestAccess::Revoke(*scope, granted.grantGeneration) == ScopeResult::Ok, "pending exact revoke failed");
    Require(!RuntimeScopeTestAccess::Decide(*scope, old, review).owner, "old prompt resurrected revoked grant");
    Require(RuntimeScopeTestAccess::Decide(*scope,
        RuntimeScopeTestAccess::ChallengeDecision(*scope, review), review).owner != 0, "fresh explicit review denied");
    auto agent = Active(); review = ScriptReview(*agent); review.required = 0;
    review.origin = AgentOrigin{agent->Current().generation, 1, 7, 13, 17, 11, 1};
    review.sessionAllowed = true;
    const auto sessionPrompt = RuntimeScopeTestAccess::ChallengeDecision(*agent, review);
    Require(RuntimeScopeTestAccess::RevokeSession(*agent, 17) == ScopeResult::Ok, "pending-only session not revoked");
    Require(!RuntimeScopeTestAccess::Decide(*agent, sessionPrompt, review).owner, "pending session prompt resurrected consent");
    const auto channelPrompt = RuntimeScopeTestAccess::ChallengeDecision(*agent, review);
    Require(RuntimeScopeTestAccess::Disconnect(*agent, 13) == ScopeResult::Ok, "pending channel not closed");
    Require(!RuntimeScopeTestAccess::Decide(*agent, channelPrompt, review).owner, "pending channel prompt resurrected authentication");
    Require(!RuntimeScopeTestAccess::ChallengeDecision(*agent, review).serial, "fresh prompt reused disconnected channel");
}
}
/** @summary Executes native generic foundation checks without loading the game or a transport. */
int main() {
    try { Gates(); Commits(); RunningStop(); BoundsAndFailures(); ReentrantStop(); SharedDomain(); LegacyWriterDomain(); TypedDecisions(); AgentDecisions(); DecisionBudgetsAndExpiry(); PendingDecisionRevocation(); std::cout << "Runtime scope gates, revisions, drain, ownership and typed provenance passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
