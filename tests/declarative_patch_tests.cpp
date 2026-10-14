#include "mods/mod_definition_parser.hpp"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
/** @summary Stop on invalid behavior with a focused assertion. */
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** @summary Write one temporary current-format manifest. */
void Write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc); file << text;
    Check(bool(file), "Cannot write patch fixture");
}
}

/** @summary Verify current TOML patch semantics without a worker, game or shipped example dependency. */
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("simoder-patch-test-" + std::to_string(GetCurrentProcessId()));
    try {
        Check(!std::filesystem::exists(root), "Fixture already exists");
        std::filesystem::create_directory(root);
        const std::string header = "schemaVersion=2\nformat='simoder.authoring'\n[metadata]\nid='fixture.patch'\nname='Patch'\nversion='1'\n";
        const std::string symbols = "[[symbols.resources]]\nname='target'\ntype=1\ngroup=2\ninstance=3\nsource='fixture'\n"
            "[[symbols.properties]]\nname='cost'\nid=0x09AE19D7\ntype='float'\nsource='verified fixture'\nunit='simoleons'\n";
        const std::string patch = "[[patches]]\n[patches.target]\nname='target'\n"
            "[[patches.properties]]\nname='cost'\ntype='float'\noperation='set'\nvalue=345\n";
        Write(root / "mod.toml", header + symbols + patch);
        sc13::mods::ModDefinition definition; std::string error;
        Check(sc13::mods::LoadModDefinition(root, definition, error), error.c_str());
        Check(!definition.script && definition.patches.size() == 1, "Patch-only mod was not prepared directly");
        const auto& property = definition.patches.front().properties.front();
        Check(property.propertyId == 0x09AE19D7 && property.value == 345 && property.name == "cost" &&
            property.source == "verified fixture" && property.unit == "simoleons", "Named patch lost canonical identity or provenance");
        std::string catalog;
        Check(sc13::mods::ListSymbolsJson(root, "cost", catalog, error), error.c_str());
        Check(catalog.find("cost") != std::string::npos && catalog.find("simoleons") != std::string::npos,
            "Current TOML catalog is missing from IDE completion");
        Write(root / "mod.toml", header + symbols + patch + patch);
        Check(!sc13::mods::LoadModDefinition(root, definition, error), "Duplicate TGI/property was accepted");
        auto unknown = patch;
        unknown.replace(unknown.find("name='cost'"), std::string("name='cost'").size(), "name='unknown'");
        Write(root / "mod.toml", header + symbols + unknown);
        Check(!sc13::mods::LoadModDefinition(root, definition, error), "Unknown alias was accepted");
        Check(error.find("Unknown property name") != std::string::npos, "Unknown alias diagnostic is missing");
        std::filesystem::remove_all(root);
        std::cout << "TOML patches: direct preparation, identity/provenance and rejection checks passed.\n";
        return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
