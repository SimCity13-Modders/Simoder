#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
namespace sc13::core {
/** @summary Exact SHA-256 output bytes; digest contents convey no authority. */
using Sha256Digest=std::array<std::byte,32>;
/** @summary Compute SHA-256 using the original Windows CNG implementation and an owned temporary object.
 * @param data Borrowed immutable input. @param digest Full output on success.
 * @param error Best-effort nonsecret diagnostic. @return False on hashing/allocation failure. */
[[nodiscard]] bool Sha256(std::span<const std::byte> data,Sha256Digest& digest,std::string& error) noexcept;
/** @summary Compute the SAME SHA-256 using only caller-owned precharged object workspace, without a heap fallback.
 * @param data Borrowed original immutable bytes. @param workspace Original reserved writable CNG object storage.
 * @param digest Full output on success. @return False if actual CNG object length exceeds this original capacity or hashing fails.
 * @remarks Caller excludes overlap, holds the workspace through return and accounts its complete capacity before effects.
 * Internal Windows CNG provider/handle working set remains a separately qualified platform dependency. */
[[nodiscard]] bool Sha256(std::span<const std::byte> data,std::span<std::byte> workspace,Sha256Digest& digest) noexcept;
/** @summary Hash a finite ordered set of borrowed original spans without concatenation or a second payload allocation.
 * @param parts Exact ordered bytes. @param workspace Original precharged CNG object storage.
 * @param digest Complete digest on success. @return Factual hashing outcome. */
[[nodiscard]] bool Sha256(std::initializer_list<std::span<const std::byte>> parts,std::span<std::byte> workspace,Sha256Digest& digest) noexcept;
/** @summary Read only a caller-bounded exact extent into the provided original buffer; no source or native authority is issued. */
using Sha256ReadCallback=bool (*)(void*,std::span<std::byte>,std::size_t&)noexcept;
/** @summary Hash a known exact original extent with caller-owned IO and CNG storage, without a full payload copy or heap fallback.
 * @param reader Synchronous trusted original reader; must report actual bytes and never retain spans.
 * @param context Original borrowed reader owner, live through return. @param expectedBytes Exact observed input extent.
 * @param input Nonempty caller-owned bounded read buffer. @param workspace Original CNG object workspace.
 * @param digest Published only after all exact original bytes hash successfully.
 * @return False on size/read/hash/alias refusal; caller still owns and must positively close its original input.
 * @pre Reader effects occur outside all host publication/native borrow guards; provider internal memory is separately qualified. */
[[nodiscard]] bool Sha256Read(Sha256ReadCallback reader,void* context,std::uint64_t expectedBytes,
    std::span<std::byte> input,std::span<std::byte> workspace,Sha256Digest& digest)noexcept;
/** @summary Compute SHA-256 over a file using bounded original streaming reads.
 * @param path Exact input file. @param digest Full digest. @param error Best-effort diagnostic. @return Hash outcome. */
[[nodiscard]] bool Sha256File(const std::filesystem::path& path,Sha256Digest& digest,std::string& error) noexcept;
/** @summary Convert the exact digest to uppercase hexadecimal text. @param digest Original bytes. @return64characters. */
[[nodiscard]] std::string ToHex(const Sha256Digest& digest);
}
