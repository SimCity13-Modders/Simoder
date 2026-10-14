#include "mod_fixture.hpp"
#include "test_harness.hpp"
#include "mods/mod_definition_parser.hpp"
namespace sc13::tests {
/** @summary Validate current manifest aliases, canonical identity and unsafe-input rejection. */
void TestModDefinitionParser() {
    const auto directory = CreateTemporaryDirectory();
    const auto header = FixtureHeader("dawid.better-sanitizer", "Better Sanitizer");
    WriteModFixture(directory, "dawid.better-sanitizer", "Better Sanitizer", 345.0F);
    sc13::mods::ModDefinition definition;
    std::string error;
    SC13_EXPECT(sc13::mods::LoadModDefinition(directory, definition, error));
    SC13_EXPECT(definition.manifest.id == "dawid.better-sanitizer");
    SC13_EXPECT(definition.manifest.name == "Better Sanitizer");
    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    SC13_EXPECT(definition.patches.size() == 1U);
    SC13_EXPECT(definition.patches.front().target == target);
    SC13_EXPECT(definition.patches.front().properties.front().value == 345.0F);
    SC13_EXPECT(definition.patches.front().properties.front().operation == sc13::mods::PatchOperation::Set);
    const std::string symbols =
        "[[symbols.resources]]\nname='sanitizer'\ntype=0x00B1B104\ngroup=0x61EFC000\ninstance=0x719436BD\nsource='test fixture'\n"
        "[[symbols.properties]]\nname='cost'\nid=0x09AE19D7\nsource='test fixture'\n";
    const std::string named = "[[patches]]\n[patches.target]\nname='sanitizer'\n"
        "[[patches.properties]]\nname='cost'\ntype='float'\noperation='set'\nvalue=345\n";
    WriteTextFile(directory / L"mod.toml", header + symbols + named);
    SC13_EXPECT(sc13::mods::LoadModDefinition(directory, definition, error));
    SC13_EXPECT(definition.patches.front().name == "sanitizer");
    SC13_EXPECT(definition.patches.front().properties.front().propertyId == 0x09AE19D7U);
    SC13_EXPECT(definition.patches.front().properties.front().name == "cost");
    SC13_EXPECT(definition.patches.front().properties.front().source == "test fixture");
    /** @summary Keep negative semantic scenarios independent of the installed serialization format. */
    const auto rejects = [&](const std::string& manifest, std::string_view diagnostic) {
        WriteTextFile(directory / L"mod.toml", manifest);
        SC13_EXPECT(!sc13::mods::LoadModDefinition(directory, definition, error));
        SC13_EXPECT(error.find(diagnostic) != std::string::npos);
    };
    rejects(header + named, "Unknown resource");
    auto mismatch = named;
    mismatch.insert(mismatch.find("type='float'"), "id=1\n");
    rejects(header + symbols + mismatch, "mismatch");
    mismatch = named;
    mismatch.insert(mismatch.find("[[patches.properties]]"), "type=1\ngroup=0x61EFC000\ninstance=0x719436BD\n");
    rejects(header + symbols + mismatch, "mismatch");
    rejects(header + symbols + "[[symbols.properties]]\nname='cost'\nid=1\nsource='fixture'\n" + named, "Duplicate symbol");
    auto unknown = named;
    unknown.replace(unknown.find("name='cost'"), 11, "name='unknown'");
    rejects(header + symbols + unknown, "Unknown property");
    rejects(header + "[[symbols.properties]]\nname='cost'\nid=1\n" + named, "source");
    rejects(header + FixturePatch(345) +
        "[[patches.properties]]\nid=0x09AE19D7\ntype='float'\noperation='set'\nvalue=346\n", "same property twice");
    rejects(FixtureHeader("Invalid.Id", "Invalid") + FixturePatch(1), "Invalid source mod ID");
    auto divide = FixturePatch(0);
    divide.replace(divide.find("operation='set'"), 15, "operation='divide'");
    rejects(header + divide, "cannot use zero");
    /** @summary Retired installed manifests provide migration guidance rather than compatibility loading. */
    SC13_EXPECT(std::filesystem::remove(directory / L"mod.toml"));
    WriteTextFile(directory / L"mod.toon", "id: retired\nname: Retired\nversion: 1\n");
    SC13_EXPECT(!sc13::mods::LoadModDefinition(directory, definition, error));
    SC13_EXPECT(error.find("migrate-toon") != std::string::npos);
    std::filesystem::remove_all(directory);
}

}
