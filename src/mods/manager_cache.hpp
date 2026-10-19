#pragma once
#include "mods/owned_publication.hpp"

namespace sc13::mods {
/** @summary One synchronous original cache action; no operational borrow survives its caller's Lease. */
class ManagerCacheAction final {
    friend class ManagerCache;
    friend class ManagerCommands;
    friend class OwnerLifecycle;
    friend class ManagerPublication;
    runtime::CacheSettlement::OwnerToken owner_;
    std::shared_ptr<const void> identity_;
    runtime::RuntimeResourceCache* cache_{};
    runtime::CacheSettlement::Batch batch_;
    // @summary Marker dies before Batch; its original view protects exact incarnation throughout cancellation/ack.
    runtime::CacheSettlement::MarkerReservation marker_;
    std::uint64_t revision_{};
    bool prepared_{};
    /** @summary Validate actual participant identity/token under original M, without new locks. */
    bool Matches(const OwnerParticipant& participant)const noexcept;
    /** @summary Acknowledge actual original nonzero Advance before every later memory mutation. */
    void Acknowledge(std::uint64_t revision)noexcept;
    /** @summary Finish acknowledged original cache work off all compound guards under caller's live Lease. */
    runtime::ScopeResult Finish()noexcept;
public:
    /** @summary Construct empty action bookkeeping before native admission. */
    ManagerCacheAction()=default;
    ManagerCacheAction(const ManagerCacheAction&)=delete;
    ManagerCacheAction& operator=(const ManagerCacheAction&)=delete;
    /** @summary Cancel only own unacknowledged preparation; arbitrary native disposal remains off guards. */
    ~ManagerCacheAction()=default;
};
/** @summary Shared canonical State/cache binder, pending gate and original release-only repair. */
class ManagerCache final {
    friend class ManagerCommands;
    friend class OwnerLifecycle;
    friend class ManagerPublication;
    /** @summary Reserve complete immutable targets off all guards using admitted original lifetime. @param lease Original action lifetime. @param binding Exact original State/entry. @param publication Immutable prepared union. @param output Empty synchronous action. @return Bounded original admission. */
    static runtime::ScopeResult Prepare(const ModManager::Lease& lease,const std::shared_ptr<OwnerLifecycle::Binding>& binding,const PatchRegistry::PreparedPublication& publication,ManagerCacheAction& output);
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerCommandsTestAccess;
    /** @summary Bind exactly once through existing trusted canonical fixture ingress, absent from production. @param manager Original admitted owner. @param id Actual map entry key. @return Original lineage bind, never authority. */
    static runtime::ScopeResult Ensure(ModManager& manager,const ModId& id);
    /** @summary Copy original native pending state through an action-local manager lifetime. @param manager Original live owner. @param id Actual participant. @param repair Permit only original cache eviction. @return Exact native observation without route-status mutation. */
    static runtime::CacheSettlement::Observation Inspect(ModManager& manager,const ModId& id,bool repair);
    /** @summary Retain only an existing original pending record view after fresh action admission; never issue or replace its token.
     * @param manager Original live manager. @param id Actual participant key. @param output Empty checked view, unchanged on rejection.
     * @return Original FindPending result or actual admission/lookup failure. */
    static runtime::CacheSettlement::Result Pending(ModManager& manager,const ModId& id,runtime::CacheSettlement::Batch& output);
    /** @summary Share fixture-only exact participant token lookup while the caller retains a fresh original Lease.
     * @param manager Original live manager. @param lease Actual admitted original lifetime. @param id Existing participant key.
     * @param output Empty local token copy, never returned through public fixture API. @return Actual admission/try-M lookup result. */
    static runtime::CacheSettlement::Result LookupToken(ModManager& manager,const ModManager::Lease& lease,const ModId& id,runtime::CacheSettlement::OwnerToken& output);
#endif
};
}
