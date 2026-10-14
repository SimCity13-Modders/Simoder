#include "mod_fixture.hpp"
#include "test_harness.hpp"
#include <Windows.h>
#include <fstream>
namespace sc13::tests {
/** @summary Creates an empty uniquely named temporary directory for filesystem tests. */
[[nodiscard]] std::filesystem::path CreateTemporaryDirectory() {
    wchar_t temporaryDirectory[MAX_PATH]{};
    wchar_t temporaryName[MAX_PATH]{};
    SC13_EXPECT(GetTempPathW(MAX_PATH, temporaryDirectory) != 0);
    SC13_EXPECT(GetTempFileNameW(temporaryDirectory, L"s13", 0, temporaryName) != 0);
    SC13_EXPECT(DeleteFileW(temporaryName) != FALSE);
    SC13_EXPECT(CreateDirectoryW(temporaryName, nullptr) != FALSE);
    return temporaryName;
}

/** @summary Writes one complete UTF-8 fixture file. */
void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    SC13_EXPECT(static_cast<bool>(stream));
}

/** @summary Build the current installed manifest envelope for an owned fixture. */
std::string FixtureHeader(std::string_view id, std::string_view name) {
    return "schemaVersion=2\nformat='simoder.authoring'\n[metadata]\nid='" + std::string(id) +
        "'\nname='" + std::string(name) + "'\nversion='1.0.0'\nauthor='Simoder Tests'\n";
}

/** @summary Produce one canonical float patch with a configurable property identity. */
std::string FixturePatch(float value, std::string_view property) {
    return "[[patches]]\n[patches.target]\ntype=0x00B1B104\ngroup=0x61EFC000\ninstance=0x719436BD\n"
        "[[patches.properties]]\nid=" + std::string(property) +
        "\ntype='float'\noperation='set'\nvalue=" + std::to_string(value) + "\n";
}

/** @summary Write a current manifest fixture without retired sidecar inputs. */
void WriteModFixture(const std::filesystem::path& directory, std::string_view id,
    std::string_view name, float value) {
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    SC13_EXPECT(!directoryError);
    WriteTextFile(directory / L"mod.toml", FixtureHeader(id, name) + FixturePatch(value));
}

}
