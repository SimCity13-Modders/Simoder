#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include "runtime/cache_settlement.hpp"

namespace sc13::mods {
class PublicationOrigin;
class ModManager;
class OwnerLifecycle;
class ManagerClose;
class ManagerCache;
class ManagerCacheAction;
/** @summary One typed current-stamp participant shared by canonical entries and isolated lifecycle fixtures. */
class OwnerParticipant {
    friend class ManagerCommands;
    friend struct ManagerCommandTicket;
    friend class ModManager;
    friend class OwnerLifecycle;
    friend class ManagerClose;
    friend class ManagerPublication;
    friend class ManagerCache;
    friend class ManagerCacheAction;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerLifetimeTestAccess;
    friend struct OwnerLifecycleTestAccess;
    friend struct ManagerPublicationTestAccess;
    friend struct ManagerCommandsTestAccess;
#endif
    /** @summary Immutable once published; predecessors retain observations, never another writable current slot. */
    struct Stamp final {
        std::shared_ptr<PublicationOrigin> current;
        std::shared_ptr<const void> tombstone;
        std::uint64_t revision{};
        bool cleanupPending{};
    };
    /** @summary Classify only an exact canonical empty slot; callers must separately prove actual installed membership and inactive runtime state.
     * @return True for pristine initial state or retained successful-removal history, never cleanup/reset/exhausted state. */
    bool CanonicalEmpty() const noexcept {
        if(!stamp || stamp->current || stamp->cleanupPending || stamp->revision==UINT64_MAX)return false;
        return stamp->tombstone ? static_cast<bool>(jointBinding) && stamp->revision!=0 : stamp->revision==0;
    }
    /** @summary Nonreusable entry incarnation survives erase/recreate while a plan retains it. */
    std::shared_ptr<const void> identity{std::make_shared<const std::byte>()};
    std::shared_ptr<Stamp> stamp{std::make_shared<Stamp>()};
    std::shared_ptr<const void> jointBinding;
    /** @summary Stable original native cache lineage across stamp/origin handoff and removal. */
    runtime::CacheSettlement::OwnerToken cacheOwner;
};
}
