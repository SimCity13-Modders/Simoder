#pragma once
#include "mods/mod_types.hpp"
#include "scripting/base_host.hpp"
#include <functional>
#include <map>
#include <memory>

namespace sc13::mods {
/** @summary Opaque native process-local installation binding; not durable identity or authorization. */
class InstallationBinding final {
public:
    /** @summary Create a distinct modeled installation, independent of path or content.
     * @return Opaque binding which the native caller may retain across captures.
     */
    [[nodiscard]] static std::shared_ptr<const InstallationBinding> Create();
private:
    /** @summary Prevent caller-authored textual installation identities. */
    InstallationBinding() = default;
};
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
/** @summary Typed fixture-only capture boundaries; never compiled into production. */
enum class CandidateCapturePhase { AfterInventory, AfterLeases, AfterRead };
class CapturedCandidateTestAccess;
#endif
/** @summary Own exact immutable selected inputs and compiler output without production consumers. */
class CapturedCandidate final {
public:
    /** @summary Capture dependency-free declarative/Luau inputs under retained native leases.
     * @param directory Source root. @param installation Native modeled installation binding.
     * @param base Structurally validated full Base pin; authenticity is not inferred.
     * @return Complete immutable candidate; throws bounded reason codes on failure.
     */
    [[nodiscard]] static std::shared_ptr<const CapturedCandidate> Capture(const std::filesystem::path& directory,
        std::shared_ptr<const InstallationBinding> installation, scripting::BasePackagePin base);
    /** @summary Access the exact parsed definition. @return Owned immutable definition. */
    [[nodiscard]] const ModDefinition& Definition() const noexcept { return definition_; }
    /** @summary Access exact consumed sources. @return Canonically sorted owned bytes. */
    [[nodiscard]] const std::map<std::string,std::string>& Inventory() const noexcept { return inventory_; }
    /** @summary Access the full copied Base pin. @return Structurally validated pin, not trust. */
    [[nodiscard]] const scripting::BasePackagePin& BasePin() const noexcept { return base_; }
    /** @summary Access deterministic content equality. @return Uppercase SHA256, never consent. */
    [[nodiscard]] const std::string& ContentDigest() const noexcept { return digest_; }
    /** @summary Compare native modeled installation identity. @param other Other capture. @return Exact binding equality. */
    [[nodiscard]] bool SameInstallation(const CapturedCandidate& other) const noexcept { return installation_ == other.installation_; }
    /** @summary Compare an original native entry binding without interpreting paths as identity.
     * @param binding Exact retained native installation. @return Whether capture belongs to that installation. */
    [[nodiscard]] bool SameInstallation(const std::shared_ptr<const InstallationBinding>& binding) const noexcept { return installation_ == binding; }
private:
    /** @summary Construction occurs only through complete native capture. */
    CapturedCandidate() = default;
    /** @summary Perform bounded capture with a fixture-only observer when compiled for acceptance.
     * @param directory Source root. @param installation Native binding. @param base Copied pin.
     * @return Complete immutable candidate.
     */
    static std::shared_ptr<const CapturedCandidate> CaptureImpl(const std::filesystem::path& directory,
        std::shared_ptr<const InstallationBinding> installation, scripting::BasePackagePin base
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
        , const std::function<void(CandidateCapturePhase)>& observer
#endif
    );
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
    friend class CapturedCandidateTestAccess;
#endif
    ModDefinition definition_;
    std::map<std::string,std::string> inventory_;
    scripting::BasePackagePin base_;
    std::shared_ptr<const InstallationBinding> installation_;
    std::string digest_;
};
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
/** @summary Exercise native capture races in the non-installed fixture archive only. */
class CapturedCandidateTestAccess final {
public:
    /** @summary Capture with deterministic fixture phases.
     * @param directory Root. @param installation Binding. @param base Pin. @param observer Fixture phase observer.
     * @return Complete candidate or bounded rejection.
     */
    static std::shared_ptr<const CapturedCandidate> Capture(const std::filesystem::path& directory,
        std::shared_ptr<const InstallationBinding> installation, scripting::BasePackagePin base,
        const std::function<void(CandidateCapturePhase)>& observer) {
        return CapturedCandidate::CaptureImpl(directory,std::move(installation),std::move(base),observer);
    }
};
#endif
}
