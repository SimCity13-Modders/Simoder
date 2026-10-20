#pragma once

#include "mods/native/contract.h"
#include "runtime/runtime_lifecycle.hpp"
#include "runtime/runtime_scope.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace sc13::mods::native {
class NativeParticipant;
/** @summary Core-reviewed registration, never deserialized from a native module's own claims. */
struct ReviewedModule final {
    std::string moduleId;
    std::filesystem::path absolutePath;
    std::array<unsigned char, 32> sha256{};
    bool nativeConsentApproved{};
};
/** @summary Observable cooperative generation phase; images remain mapped in every phase. */
enum class ModulePhase { Prepared, Active, Stopping, Drained, Faulted };
/** @summary Conservative process-wide retained image budget, independent of module resource quotas. */
struct ImageUsage final { std::size_t count{}; std::size_t mappedBytes{}; std::size_t generations{}; };

/** @summary Owns a logical generation while the host retains all native code and borrowed tables. */
class NativeModule final {
public:
    struct State;
    /** @summary Production admission remains unavailable until the real base provider qualifies. @return NOT_READY, or STALE after stop. */
    SdResult Admit();
    /** @summary Closes host admission before invoking the cooperative module without host locks. @return Explicit module outcome. */
    SdResult Quiesce();
    /** @summary Observes bounded nonblocking drain; never unloads an image. @return BUSY while callbacks/resources remain. */
    SdResult Drain();
    /** @summary Returns the permanently retained borrowed table. @return Core-owned immutable table reference. */
    const SdHostApiV2& Host() const noexcept;
    /** @summary Copies the logical generation phase. @return Current phase. */
    ModulePhase Phase() const noexcept;
    /** @summary Stage a retained original native identity outside all publication guards.
     * @param output Cleared outside guards on rejection; receives a prepared lifetime only.
     * @return OK for successfully prepared locally live identity, STALE after observed stop.
     * @pre Caller holds no D/M/S/V/R/B/Q/T guard. This grants no runtime authority. */
    SdResult CaptureParticipant(std::optional<NativeParticipant>& output) const;
#if defined(SIMODER_NATIVE_TEST_HOST)
    /** @summary Synthetic postcommit base gate, absent from every production target. @return Fixture admission outcome. */
    SdResult AdmitSynthetic();
#endif
private:
    /** @summary Wraps a process-retained state. @param state Host-owned generation. */
    explicit NativeModule(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    friend class NativeModuleLoader;
#if defined(SIMODER_NATIVE_TEST_HOST)
    friend struct NativeParticipantTestAccess;
#endif
};

#if defined(SIMODER_NATIVE_TEST_HOST)
/** @summary Isolated observation cut on the original retained scope; no production identity or authority factory. */
struct NativeParticipantTestAccess final {
    /** @summary Request real external scope stop without manufacturing State-local retirement.
     * @param module Successfully returned original module retained by the caller.
     * @pre Caller holds no publication or loader/State/Registry guard. */
    static void StopOriginalScope(NativeModule& module) noexcept;
    /** @summary Reads actual foreign close invocation attempts on the original retained State.
     * @param module Original module retained by the caller.
     * @return Quiesce then Drain attempt counts; coherent after a completed serialized close call. */
    static std::array<std::uint32_t, 2> ForeignCloseCalls(const NativeModule& module) noexcept;
};
#endif

/** @summary Bounded trusted-code loader; each instance requires serialized core callers, while the process registry serializes images. Native DLLs are not sandboxed. */
class NativeModuleLoader final {
public:
    /** @summary Binds existing core ownership and one explicit staging location.
     * @param stagingRoot Existing absolute plain directory under a trusted user-controlled location.
     * @param owner Actual fresh U1 runtime owner. @param domain Shared U1 commit domain.
     * @param lifecycle Existing factual lifecycle publisher. */
    NativeModuleLoader(std::filesystem::path stagingRoot, std::shared_ptr<platform::RuntimeOwner> owner,
        std::shared_ptr<runtime::RuntimeCommitDomain> domain, std::shared_ptr<runtime::RuntimeLifecycle> lifecycle);
    /** @summary Verifies exact trust/hash/PE/dependencies before executing; prepares a fresh owner.
     * @param registration Core-reviewed exact content and consent. @param output Cleared on every error.
     * @return Explicit status; diagnostics contain no credential or game data. */
    SdResult Prepare(const ReviewedModule& registration, std::shared_ptr<NativeModule>& output);
    /** @summary Returns a copied process-wide budget observation. @return All retained images and generations, including failures. */
    static ImageUsage Usage();
    /** @summary Returns the most recent local failure description. @return Diagnostic without source bytes. */
    const std::string& LastError() const noexcept { return error_; }
private:
    std::filesystem::path stagingRoot_;
    std::shared_ptr<platform::RuntimeOwner> owner_;
    std::shared_ptr<runtime::RuntimeCommitDomain> domain_;
    std::shared_ptr<runtime::RuntimeLifecycle> lifecycle_;
    std::string error_;
};
}
