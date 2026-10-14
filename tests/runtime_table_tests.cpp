#include "test_harness.hpp"
#include "core/runtime_property_table.hpp"
#include <array>
#include <bit>

namespace sc13::tests {
namespace {
/** @summary Validates runtime-table structure, float reads, copy isolation, and one-field patching. */
void TestRuntimePropertyTable() {
    constexpr std::uint32_t floatMetadata = 0x000D0000U;
    std::array<sc13::core::RuntimePropertyRecord, 4> source{
        sc13::core::RuntimePropertyRecord{
            0x01000000U, std::bit_cast<std::uint32_t>(1.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x0AFB9882U, std::bit_cast<std::uint32_t>(72.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x0FD16C15U, std::bit_cast<std::uint32_t>(200.0F), {}, floatMetadata}};
    SC13_EXPECT(sc13::core::ValidateRuntimePropertyTable(source, source.size()) ==
                sc13::core::RuntimeTableStatus::Valid);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(source, 0x09AE19D7U).value() == 300.0F);

    auto ownedCopy = source;
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    ownedCopy, ownedCopy.size(), 0x09AE19D7U, 300.0F, 345.0F) ==
                sc13::core::RuntimeFloatPatchStatus::Applied);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(source, 0x09AE19D7U).value() == 300.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(ownedCopy, 0x09AE19D7U).value() == 345.0F);
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (index == 1U) {
            SC13_EXPECT(source[index].id == ownedCopy[index].id);
            SC13_EXPECT(source[index].auxiliary == ownedCopy[index].auxiliary);
            SC13_EXPECT(source[index].metadata == ownedCopy[index].metadata);
            SC13_EXPECT(source[index].valueBits != ownedCopy[index].valueBits);
        } else {
            SC13_EXPECT(std::memcmp(&source[index], &ownedCopy[index], sizeof(source[index])) == 0);
        }
    }
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    ownedCopy, ownedCopy.size(), 0x09AE19D7U, 300.0F, 345.0F) ==
                sc13::core::RuntimeFloatPatchStatus::AlreadyApplied);
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    source, source.size(), 0x09AE19D7U, 301.0F, 345.0F) ==
                sc13::core::RuntimeFloatPatchStatus::UnexpectedValue);
    SC13_EXPECT(sc13::core::ValidateRuntimePropertyTable(source, source.size() + 1U) ==
                sc13::core::RuntimeTableStatus::WrongCount);
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    source, source.size(), 0x11111111U, 0.0F, 1.0F) ==
                sc13::core::RuntimeFloatPatchStatus::PropertyMissing);

    auto wrongType = source;
    wrongType[1].metadata = 0x000C0000U;
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    wrongType, wrongType.size(), 0x09AE19D7U, 300.0F, 345.0F) ==
                sc13::core::RuntimeFloatPatchStatus::PropertyNotFloat);

    auto malformed = source;
    std::swap(malformed[1], malformed[2]);
    SC13_EXPECT(sc13::core::ValidateRuntimePropertyTable(malformed, malformed.size()) ==
                sc13::core::RuntimeTableStatus::NotStrictlySorted);
}

}
void TestRuntimeTable() { TestRuntimePropertyTable(); }
}
