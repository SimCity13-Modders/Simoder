#include "test_harness.hpp"
#include "binary_fixture.hpp"
#include "formats/dbpf/refpack.hpp"
#include "formats/prop/prop_document.hpp"
#include "formats/toon/toon_document.hpp"
#include <array>
#include <bit>
#include <span>
#include <string>

namespace sc13::tests {
namespace {
/** @summary Validates literal-only RefPack decoding and truncated-input rejection. */
void TestRefPack() {
    const std::vector<std::byte> compressed{
        std::byte{0x10}, std::byte{0xFB}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04},
        std::byte{0xE0}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
        std::byte{0xFC}};
    std::vector<std::byte> output;
    std::string error;
    SC13_EXPECT(sc13::formats::dbpf::DecompressRefPack(compressed, 4, output, error));
    SC13_EXPECT(output == std::vector<std::byte>(
                              {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}}));
    SC13_EXPECT(!sc13::formats::dbpf::DecompressRefPack(
        std::span<const std::byte>(compressed.data(), 7), 4, output, error));
}

/** @summary Builds and validates a byte-preserving synthetic PROP document. */
void TestProp() {
    std::vector<std::byte> bytes;
    AppendBigU32(bytes, 3);
    AppendBigU32(bytes, 0x09AE19D7U);
    AppendBigU16(bytes, sc13::formats::prop::kTypeFloat);
    AppendBigU16(bytes, 0x8000U);
    AppendBigU32(bytes, std::bit_cast<std::uint32_t>(300.0F));
    AppendBigU32(bytes, 0xDEADBEEFU);
    AppendBigU16(bytes, sc13::formats::prop::kTypeUInt32);
    AppendBigU16(bytes, 0x8000U);
    AppendBigU32(bytes, 0x12345678U);
    AppendBigU32(bytes, 0xCAFEBABEU);
    AppendBigU16(bytes, 0x007FU);
    AppendBigU16(bytes, 0x8020U);
    AppendBigU32(bytes, 2);
    AppendBigU32(bytes, 3);
    bytes.insert(bytes.end(), {
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
        std::byte{0x40}, std::byte{0x50}, std::byte{0x60}});

    const std::vector<std::byte> original = bytes;
    sc13::formats::prop::PropDocument document;
    std::string error;
    SC13_EXPECT(document.Parse(bytes, error));
    SC13_EXPECT(std::vector<std::byte>(document.bytes().begin(), document.bytes().end()) == original);
    const auto* opaque = document.Find(0xCAFEBABEU);
    SC13_EXPECT(opaque != nullptr);
    SC13_EXPECT(opaque->isArray);
    SC13_EXPECT(opaque->type == 0x7FU);
    SC13_EXPECT(opaque->valueSize == 6);
    SC13_EXPECT(document.GetFloat(0x09AE19D7U).value() == 300.0F);
    SC13_EXPECT(document.SetFloat(0x09AE19D7U, 345.0F, error));
    SC13_EXPECT(document.GetFloat(0x09AE19D7U).value() == 345.0F);
    SC13_EXPECT(document.bytes()[12] == std::byte{0x43});
    SC13_EXPECT(document.bytes()[13] == std::byte{0xAC});
    SC13_EXPECT(document.bytes()[14] == std::byte{0x80});
    SC13_EXPECT(document.bytes()[15] == std::byte{0x00});
    for (std::size_t index = 0; index < document.bytes().size(); ++index) {
        if (index < 12 || index > 15) {
            SC13_EXPECT(document.bytes()[index] == original[index]);
        }
    }
}

/** @summary Validates the bounded TOON document format retained for configuration and offline migration. */
void TestToon() {
    constexpr std::string_view source =
        "id: dawid.better-sanitizer\n"
        "name: Better Sanitizer\n"
        "version: 1.0.0\n"
        "developerConsole:\n"
        "  enabled: false\n"
        "patches[1]:\n"
        "  - target:\n"
        "      type: 0x00B1B104\n"
        "      group: 0x61EFC000\n"
        "      instance: 0x719436BD\n"
        "    properties[1]{id,type,operation,value}:\n"
        "      0x09AE19D7,float,set,345\n";
    sc13::formats::toon::Document document;
    sc13::formats::toon::ParseError error;
    SC13_EXPECT(sc13::formats::toon::Parse(source, document, error));
    const auto id = document.root().find("id");
    SC13_EXPECT(id != document.root().end());
    SC13_EXPECT(id->second.AsString() != nullptr);
    SC13_EXPECT(*id->second.AsString() == "dawid.better-sanitizer");

    const auto developerConsole = document.root().find("developerConsole");
    SC13_EXPECT(developerConsole != document.root().end());
    const auto* enabled = developerConsole->second.Find("enabled");
    SC13_EXPECT(enabled != nullptr);
    SC13_EXPECT(enabled->AsBoolean() != nullptr);
    SC13_EXPECT(!*enabled->AsBoolean());

    const auto patches = document.root().find("patches");
    SC13_EXPECT(patches != document.root().end());
    SC13_EXPECT(patches->second.AsArray() != nullptr);
    SC13_EXPECT(patches->second.AsArray()->size() == 1U);
    const auto* target = patches->second.AsArray()->front().Find("target");
    SC13_EXPECT(target != nullptr);
    const auto* type = target->Find("type");
    SC13_EXPECT(type != nullptr);
    SC13_EXPECT(type->AsString() != nullptr);
    SC13_EXPECT(*type->AsString() == "0x00B1B104");
    const auto* properties = patches->second.AsArray()->front().Find("properties");
    SC13_EXPECT(properties != nullptr);
    SC13_EXPECT(properties->AsArray() != nullptr);
    SC13_EXPECT(properties->AsArray()->front().Find("value")->AsNumber() != nullptr);
    SC13_EXPECT(*properties->AsArray()->front().Find("value")->AsNumber() == 345.0);

    SC13_EXPECT(!sc13::formats::toon::Parse(
        "items[2]: one\n", document, error));
    SC13_EXPECT(error.line == 1U);
    SC13_EXPECT(!sc13::formats::toon::Parse(
        "root:\n   child: bad-indent\n", document, error));
    SC13_EXPECT(error.line == 2U);
    SC13_EXPECT(!sc13::formats::toon::Parse(
        "key: one\nkey: two\n", document, error));
    SC13_EXPECT(error.line == 2U);
}

}
void TestFormats() { TestRefPack(); TestProp(); TestToon(); }
}
