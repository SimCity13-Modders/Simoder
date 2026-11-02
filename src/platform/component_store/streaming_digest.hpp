/** @file @summary Hash an already retained StoreIO file without reopening, closing or manufacturing installer authority. */
#pragma once
#include "platform/component_store/store_io.hpp"
#include "core/sha256.hpp"
namespace sc13::platform::component_store {
/** @summary Data-only bounded hash disposition; short successful reads remain Unknown, unlike hash failure. */
enum class DigestStatus {Complete,SizeMismatch,ReadFailure,ShortRead,HashFailure};
/** @summary Preserve actual last native read status separately from data/hash validation. */
struct DigestResult final {DigestStatus status{DigestStatus::HashFailure};Result read{Status::Ok};};
/** @summary Hash exactly expected bytes from the same original retained file starting at its current initial position.
 * @param io Same native backend instance that owns file. @param file Caller-retained original read file.
 * @param observed Original OpenRead observation; data only, never an owner issuer.
 * @param expected Expected complete byte count. @param digest Full output only on Complete.
 * @remarks Uses 128 KiB stack scratch. Caller MUST retain and explicitly Close file after every result,
 * interpret Close Unknown before accepting output, and retain uncertain custody. This helper never closes,
 * retries, reopens or changes source permissions. A second invocation does not rewind the original file. */
[[nodiscard]] DigestResult ReadRetainedDigest(StoreIO& io,File& file,const Observation& observed,
    std::uint64_t expected,core::Sha256Digest& digest)noexcept;
}
