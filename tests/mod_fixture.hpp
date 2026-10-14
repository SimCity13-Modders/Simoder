#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace sc13::tests {
/** @summary Create an isolated temporary directory for owned test inputs. */
[[nodiscard]] std::filesystem::path CreateTemporaryDirectory();
/** @summary Replace one owned fixture file with complete UTF-8 text. */
void WriteTextFile(const std::filesystem::path& path, std::string_view text);
/** @summary Produce current manifest metadata for a synthetic mod. */
std::string FixtureHeader(std::string_view id, std::string_view name);
/** @summary Produce a canonical scalar patch for a synthetic observed resource. */
std::string FixturePatch(float value, std::string_view property = "0x09AE19D7");
/** @summary Create a complete current-format mod without shipped examples. */
void WriteModFixture(const std::filesystem::path& directory, std::string_view id,
    std::string_view name, float value);
}
