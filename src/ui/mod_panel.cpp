#include "ui/mod_panel.hpp"
#include "mods/mod_manager.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "core/runtime_property_table.hpp"
#include "core/tgi.hpp"
#include <imgui.h>
#include <algorithm>
#include <vector>

namespace sc13::ui {
void DrawModPanel(mods::ModManager* manager, ModPanelState& state) {
    ImGui::SetNextWindowBgAlpha(0.88F);
    ImGui::Begin(
        "##SimoderLauncher", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Simoder")) {
        state.open = !state.open;
    }
    ImGui::End();

    if (state.open) {
        ImGui::SetNextWindowSize(ImVec2(960.0F, 640.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Simoder", &state.open)) {
            if (ImGui::Button("Refresh") && manager != nullptr) {
                static_cast<void>(manager->RequestAction("refresh"));
            }
            if (manager != nullptr) state.feedback = manager->ActionStatus();
            if (!state.feedback.empty()) {
                ImGui::SameLine();
                ImGui::TextWrapped("%s", state.feedback.c_str());
            }
            ImGui::Separator();

            const std::vector<mods::ModUiEntry> entries =
                manager == nullptr ? std::vector<mods::ModUiEntry>{}
                                     : manager->Snapshot(false);
            const auto runtimeObservations = manager == nullptr ? std::vector<runtime::RuntimeResourceObservation>{}
                : manager->RuntimeObservations();
            if (ImGui::BeginTable(
                    "ModTable", 5,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Version");
                ImGui::TableSetupColumn("Author");
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 80.0F);
                ImGui::TableHeadersRow();
                for (const mods::ModUiEntry& entry : entries) {
                    ImGui::PushID(entry.id.empty() ? entry.name.c_str() : entry.id.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(entry.name.c_str());
                    if (!entry.diagnostic.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", entry.diagnostic.c_str());
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(entry.version.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(entry.author.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(mods::ModStateName(entry.state));
                    ImGui::TableSetColumnIndex(4);
                    bool enabled = entry.canDisable;
                    const bool interactive = entry.canEnable || entry.canDisable;
                    ImGui::BeginDisabled(!interactive || (manager && manager->OperationBusy()));
                    if (ImGui::Checkbox("##Enabled", &enabled) && manager != nullptr) {
                        static_cast<void>(manager->RequestAction(enabled ? "enable" : "disable", entry.id));
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            /** @summary Displays inspection and reload controls at the full panel width. */
            ImGui::SeparatorText("Mod details");
            for (const auto& entry : entries) {
                ImGui::PushID(entry.id.empty() ? entry.name.c_str() : entry.id.c_str());
                if (ImGui::CollapsingHeader(entry.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextWrapped("ID: %s", entry.id.c_str());
                    if (!entry.scriptLanguage.empty()) {
                        ImGui::TextWrapped("Script: %s; %s; memory: %llu KiB", entry.scriptLanguage.c_str(),
                            entry.scriptRunning ? "running" : "stopped", static_cast<unsigned long long>(entry.scriptMemoryBytes / 1024));
                        ImGui::TextWrapped("Generation: %s", entry.scriptRevision.c_str());
                        if (!entry.scriptDiagnostic.empty()) ImGui::TextWrapped("Script error: %s", entry.scriptDiagnostic.c_str());
                    }
                    if (!entry.diagnostic.empty()) ImGui::TextWrapped("%s", entry.diagnostic.c_str());
                    if (ImGui::TreeNode("Resolved overrides")) {
                        for (const auto& patch : entry.patches) {
                            ImGui::TextWrapped("%s TGI=%s", patch.name.c_str(),
                                core::ToString(patch.target).c_str());
                            if (!patch.source.empty()) ImGui::TextWrapped("Source: %s", patch.source.c_str());
                            const auto observedCount = std::count_if(runtimeObservations.begin(), runtimeObservations.end(),
                                [&patch](const auto& observation) { return observation.key.target == patch.target; });
                            const auto appliedCount = std::count_if(runtimeObservations.begin(), runtimeObservations.end(),
                                [&patch](const auto& observation) { return observation.key.target == patch.target && observation.status == "applied"; });
                            ImGui::TextWrapped("Retained observations: %llu; applied: %llu (historical)",
                                static_cast<unsigned long long>(observedCount), static_cast<unsigned long long>(appliedCount));
                            for (const auto& property : patch.properties) {
                                ImGui::TextWrapped("%s [0x%08X] %s %.9g", property.name.c_str(),
                                    property.propertyId, mods::PatchOperationName(property.operation), property.value);
                                if (!property.source.empty()) ImGui::TextWrapped("Source: %s", property.source.c_str());
                                if (!property.description.empty()) ImGui::TextWrapped("%s", property.description.c_str());
                                if (!property.unit.empty()) ImGui::TextWrapped("Unit: %s", property.unit.c_str());
                                if (!property.origin.empty()) ImGui::TextWrapped("Origin: %s", property.origin.c_str());
                                for (const auto& observation : runtimeObservations) {
                                    if (observation.key.target != patch.target) continue;
                                    const auto vanilla = core::ReadRuntimeFloat(observation.vanilla, property.propertyId);
                                    const auto applied = core::ReadRuntimeFloat(observation.applied, property.propertyId);
                                    ImGui::TextWrapped("Observed instance=%llu generation=%llu %s: %s",
                                        static_cast<unsigned long long>(observation.key.resourceAddress),
                                        static_cast<unsigned long long>(observation.generation),
                                        observation.status.c_str(), observation.diagnostic.c_str());
                                    if (vanilla && applied) ImGui::TextWrapped("Historical vanilla %.9g -> committed %.9g", *vanilla, *applied);
                                }
                            }
                            if (std::none_of(runtimeObservations.begin(), runtimeObservations.end(),
                                [&patch](const auto& observation) { return observation.key.target == patch.target; }))
                                ImGui::TextUnformatted("No retained observation for this resource.");
                        }
                        ImGui::TreePop();
                    }
                    if (entry.canReload && ImGui::TreeNode("Pending changes (current -> pending)")) {
                        if (!entry.pendingScriptRevision.empty())
                            ImGui::TextWrapped("Script generation: %s -> %s (fresh VM, local state resets)",
                                entry.scriptRevision.c_str(), entry.pendingScriptRevision.c_str());
                        ImGui::TextUnformatted("Current operations:");
                        for (const auto& patch : entry.patches) for (const auto& property : patch.properties)
                            ImGui::TextWrapped("%s %s [0x%08X] %s %.9g", core::ToString(patch.target).c_str(),
                                property.name.c_str(), property.propertyId, mods::PatchOperationName(property.operation), property.value);
                        ImGui::TextUnformatted("Pending operations:");
                        for (const auto& patch : entry.pendingPatches) for (const auto& property : patch.properties)
                            ImGui::TextWrapped("%s %s [0x%08X] %s %.9g", core::ToString(patch.target).c_str(),
                                property.name.c_str(), property.propertyId, mods::PatchOperationName(property.operation), property.value);
                        ImGui::BeginDisabled(manager->OperationBusy());
                        if (ImGui::Button("Apply validated reload"))
                            static_cast<void>(manager->RequestAction("reload", entry.id, entry.pendingRevision));
                        ImGui::EndDisabled();
                        ImGui::TreePop();
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::End();
    }

}
}
