#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sc13::tests {
/** @summary Append a big-endian uint16 to synthetic binary input. */
void AppendBigU16(std::vector<std::byte>& bytes, std::uint16_t value);
/** @summary Append a big-endian uint32 to synthetic binary input. */
void AppendBigU32(std::vector<std::byte>& bytes, std::uint32_t value);
/** @summary Write a little-endian uint16 at an existing fixture offset. */
void WriteLittleU16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value);
/** @summary Write a little-endian uint32 at an existing fixture offset. */
void WriteLittleU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value);
}
