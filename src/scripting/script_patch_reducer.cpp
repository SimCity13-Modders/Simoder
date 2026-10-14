#include "scripting/script_patch_reducer.hpp"
#include <map>
#include <stdexcept>

namespace sc13::scripting {
using mods::ResourcePatchDefinition;
using mods::PropertyPatch;
std::vector<ResourcePatchDefinition> ReduceScriptPatches(
    const std::vector<ResourcePatchDefinition>& previous,
    const std::vector<ScriptPatchCommand>& commands) {
    std::map<core::Tgi, std::map<std::uint32_t, PropertyPatch>> fields;
    for (const auto& patch : previous)
        for (const auto& field : patch.properties) fields[patch.target][field.propertyId] = field;
    for (const auto& command : commands) {
        auto& target = fields[command.target];
        if (!command.value) { target.erase(command.property); continue; }
        PropertyPatch field;
        field.propertyId = command.property; field.value = *command.value;
        field.name = command.propertyName;
        field.source = command.source.empty() ? "Luau host API" : command.source;
        target[command.property] = std::move(field);
    }
    std::vector<ResourcePatchDefinition> result;
    std::size_t count{};
    for (auto& [target, properties] : fields) {
        if (properties.empty()) continue;
        ResourcePatchDefinition patch; patch.target = target; patch.source = "Luau host API";
        for (auto& [id, property] : properties) { static_cast<void>(id); patch.properties.push_back(std::move(property)); }
        count += patch.properties.size();
        if (count > 256) throw std::runtime_error("At most 256 persistent PROP overrides per script");
        result.push_back(std::move(patch));
    }
    return result;
}

} // namespace sc13::scripting
