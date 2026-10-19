#include "mods/manager_cache.hpp"
#include <exception>

namespace sc13::mods {
namespace {
/** @summary Map bounded native admission without turning pending into an accepted no-op. */
runtime::ScopeResult Admission(runtime::CacheSettlement::Result result)noexcept {using R=runtime::CacheSettlement::Result;using S=runtime::ScopeResult;if(result==R::Ok)return S::Ok;if(result==R::Busy||result==R::Conflict)return S::Busy;return S::NotReady;}
}
bool ManagerCacheAction::Matches(const OwnerParticipant& participant)const noexcept {return prepared_&&identity_==participant.identity&&runtime::CacheSettlement::SameOwner(owner_,participant.cacheOwner);}
void ManagerCacheAction::Acknowledge(std::uint64_t revision)noexcept {if(!prepared_||revision_)std::terminate();marker_.Acknowledge(revision);revision_=revision;}
runtime::ScopeResult ManagerCacheAction::Finish()noexcept {marker_.Release();if(!revision_)return runtime::ScopeResult::Ok;const auto observed=runtime::CacheSettlement::Evict(*cache_,batch_);return observed.result==runtime::CacheSettlement::Result::Ok&&observed.phase==runtime::CacheSettlement::Phase::Settled?runtime::ScopeResult::Ok:runtime::ScopeResult::Indeterminate;}
runtime::ScopeResult ManagerCache::Prepare(const ModManager::Lease& lease,const std::shared_ptr<OwnerLifecycle::Binding>& binding,const PatchRegistry::PreparedPublication& publication,ManagerCacheAction& output) {
    using S=runtime::ScopeResult;static_assert(sizeof(ManagerCacheAction)<=512);
    if(!lease||!binding||!binding->state||output.prepared_)return S::Invalid;
    auto* manager=lease.Manager();if(!manager||manager->state_!=binding->state)return S::Conflict;
    {
        std::unique_lock lock(manager->mutex_,std::try_to_lock);if(!lock.owns_lock())return S::Busy;
        const auto found=manager->records_.find(binding->owner);if(found==manager->records_.end()||found->second.participant.identity!=binding->identity)return S::Stale;
        const auto& participant=found->second.participant;if(!participant.cacheOwner.sequence_)return S::NotReady;
        output.owner_=participant.cacheOwner;output.identity_=participant.identity;output.cache_=&manager->cache_;
    }
    // @summary Reject an already-known reset/exhaustion before any marker; a later concurrent reset remains an explicit unsettled outcome.
    const auto current=output.cache_->Witness();const auto& original=output.owner_.witness_;
    if(!current.nonce_||current.nonce_!=original.nonce_||current.reset_!=original.reset_||current.control_.owner_before(original.control_)||original.control_.owner_before(current.control_))return S::NotReady;
    const auto result=runtime::CacheSettlement::ReserveMarker(output.owner_,publication.Affected(),output.batch_,output.marker_);if(result!=runtime::CacheSettlement::Result::Ok)return Admission(result);
    output.prepared_=true;return S::Ok;
}
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
runtime::ScopeResult ManagerCache::Ensure(ModManager& manager,const ModId& id) {
    using S=runtime::ScopeResult;const auto state=manager.state_;const ModManager::Lease lease(state);if(!lease||lease.Manager()!=&manager)return S::Cancelled;
    std::shared_ptr<const void> identity;runtime::CacheSettlement::OwnerToken token;
    {std::unique_lock lock(manager.mutex_,std::try_to_lock);if(!lock.owns_lock())return S::Busy;const auto found=manager.records_.find(id);if(found==manager.records_.end())return S::Stale;identity=found->second.participant.identity;token=found->second.participant.cacheOwner;}
    if(!manager.cache_.BindCanonicalState(state))return S::Denied;
    if(token.sequence_)return S::Ok;
    const auto issued=runtime::CacheSettlement::IssueOwner(manager.cache_,token);if(issued!=runtime::CacheSettlement::Result::Ok)return Admission(issued);
    {std::unique_lock lock(manager.mutex_,std::try_to_lock);if(!lock.owns_lock())return S::Busy;const auto found=manager.records_.find(id);if(found==manager.records_.end()||found->second.participant.identity!=identity)return S::Stale;
        if(!found->second.participant.cacheOwner.sequence_)found->second.participant.cacheOwner=token;}
    return S::Ok;
}
runtime::CacheSettlement::Observation ManagerCache::Inspect(ModManager& manager,const ModId& id,bool repair) {
    runtime::CacheSettlement::Observation observed;const auto state=manager.state_;const ModManager::Lease lease(state);if(!lease||lease.Manager()!=&manager){observed.result=runtime::CacheSettlement::Result::Unavailable;return observed;}
    runtime::CacheSettlement::OwnerToken token;
    const auto lookup=LookupToken(manager,lease,id,token);if(lookup!=runtime::CacheSettlement::Result::Ok){observed.result=lookup;return observed;}
    runtime::CacheSettlement::Batch batch;const auto found=runtime::CacheSettlement::FindPending(token,batch);if(found!=runtime::CacheSettlement::Result::Ok){observed.result=found;observed.ownerSequence=token.sequence_;return observed;}
    return repair?runtime::CacheSettlement::Evict(manager.cache_,batch):runtime::CacheSettlement::Observe(batch);
}
runtime::CacheSettlement::Result ManagerCache::Pending(ModManager& manager,const ModId& id,runtime::CacheSettlement::Batch& output) {
    using R=runtime::CacheSettlement::Result;
    const auto state=manager.state_;const ModManager::Lease lease(state);
    if(!lease||lease.Manager()!=&manager)return R::Unavailable;
    runtime::CacheSettlement::OwnerToken token;
    const auto lookup=LookupToken(manager,lease,id,token);if(lookup!=R::Ok)return lookup;
    // @summary Existing API validates exact empty output and retains only its checked record view; M is gone.
    return runtime::CacheSettlement::FindPending(token,output);
}
runtime::CacheSettlement::Result ManagerCache::LookupToken(ModManager& manager,const ModManager::Lease& lease,const ModId& id,runtime::CacheSettlement::OwnerToken& output) {
    using R=runtime::CacheSettlement::Result;
    if(!lease||lease.Manager()!=&manager)return R::Unavailable;
    {
        std::unique_lock lock(manager.mutex_,std::try_to_lock);if(!lock.owns_lock())return R::Busy;
        const auto found=manager.records_.find(id);if(found==manager.records_.end())return R::NotFound;
        output=found->second.participant.cacheOwner;
    }
    return R::Ok;
}
#endif
}
