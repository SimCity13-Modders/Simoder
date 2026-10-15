#include "dev/authoring_tool_manifest.hpp"
#include "scripting/source_project.hpp"
#include <stdexcept>

namespace sc13::dev {
AuthoringToolPaths ParseAuthoringToolManifest(const toml::table& document,
    const std::filesystem::path& directory) {
    AuthoringToolPaths result;
    if (const auto tools = document["tools"].as_table()) {
        for (const auto& [name, node] : *tools) {
            const std::string capability(name.str());
            if (!IsAuthoringCapability(capability))
                throw std::runtime_error("Unknown authoring capability: " + capability);
            const auto relative = node.value<std::string>();
            if (!relative || relative->empty()) throw std::runtime_error("Invalid authoring tool path");
            const auto path = scripting::SourcePath(directory, *relative);
            if (capability == "typescript-toolchain") {
                const auto entry = scripting::SourcePath(directory, *relative + "/build.mjs");
                if (!std::filesystem::is_directory(path) || !std::filesystem::is_regular_file(entry))
                    throw std::runtime_error("TypeScript toolchain must contain build.mjs");
            } else if (!std::filesystem::is_regular_file(path) || path.extension() != ".exe")
                throw std::runtime_error("Authoring tool must be a local executable");
            result.emplace(capability, path);
        }
    } else if (document.contains("tools")) throw std::runtime_error("tools must be a table");
    return result;
}
}
