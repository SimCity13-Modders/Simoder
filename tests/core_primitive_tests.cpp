#include "test_harness.hpp"
#include "core/tgi.hpp"
#include "core/resource_filter.hpp"
#include "memory/signature.hpp"
#include <vector>
#include <span>

namespace sc13::tests {
namespace {
/** @summary Validates formatting and strict parsing of TGI identifiers. */
void TestTgi() {
    const sc13::core::Tgi expected{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    SC13_EXPECT(sc13::core::ToString(expected) == "00B1B104:61EFC000:719436BD");
    const auto parsed = sc13::core::ParseTgi("0x00B1B104:61EFC000:719436BD");
    SC13_EXPECT(parsed.has_value());
    SC13_EXPECT(parsed.value() == expected);
    SC13_EXPECT(!sc13::core::ParseTgi("00B1B104:61EFC000").has_value());
}

/** @summary Validates optional semantic TGI matching without dependence on runtime field order. */
void TestResourceFilter() {
    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const sc13::core::ResourceFilter exact{
        target.type, target.group, target.instance};
    SC13_EXPECT(exact.Matches(target));
    SC13_EXPECT(!exact.Matches(sc13::core::Tgi{target.type, target.group, 0x11111111U}));
    const sc13::core::ResourceFilter groupOnly{std::nullopt, target.group, std::nullopt};
    SC13_EXPECT(groupOnly.Matches(target));
    SC13_EXPECT(!groupOnly.Matches(sc13::core::Tgi{target.type, 0x22222222U, target.instance}));
}

/** @summary Validates wildcard signature parsing and all-match scanning. */
void TestSignature() {
    std::string error;
    const auto signature = sc13::memory::Signature::Parse("8B ?? 89", error);
    SC13_EXPECT(signature.has_value());
    const std::vector<std::byte> bytes{
        std::byte{0x8B}, std::byte{0x01}, std::byte{0x89}, std::byte{0x90},
        std::byte{0x8B}, std::byte{0xFF}, std::byte{0x89}};
    const auto matches = sc13::memory::FindAll(bytes, signature.value());
    SC13_EXPECT(matches.size() == 2);
    SC13_EXPECT(matches[0] == 0);
    SC13_EXPECT(matches[1] == 4);
    const auto ambiguous = sc13::memory::FindUnique(bytes, signature.value());
    SC13_EXPECT(ambiguous.status == sc13::memory::UniqueSignatureStatus::Ambiguous);
    SC13_EXPECT(ambiguous.matchCount == 2U);
    const auto absent = sc13::memory::Signature::Parse("DE AD BE EF", error);
    SC13_EXPECT(absent.has_value());
    SC13_EXPECT(sc13::memory::FindAll(bytes, absent.value()).empty());
    const auto noMatch = sc13::memory::FindUnique(bytes, absent.value());
    SC13_EXPECT(noMatch.status == sc13::memory::UniqueSignatureStatus::NotFound);
    SC13_EXPECT(noMatch.matchCount == 0U);

    const std::span<const std::byte> exactBytes(bytes.data(), 3U);
    const auto exact = sc13::memory::FindUnique(exactBytes, signature.value());
    SC13_EXPECT(exact.status == sc13::memory::UniqueSignatureStatus::Matched);
    SC13_EXPECT(exact.offset == 0U);
}

}
void TestCorePrimitives() {
    TestTgi();
    TestResourceFilter();
    TestSignature();
}
}
