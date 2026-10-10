// The Input window: connected controllers with their live state, and the project's
// action bindings, edited in place and saved to Config/Input.swinput.
#include "Editor.h"

#include <Input/Input.h>
#include <Input/InputBinding.h>
#include <UI/Gui.h>

#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <string>

namespace editor {

using shockwave::InputBinding;
using shockwave::InputDevice;

namespace {

const char* typeName(shockwave::GamepadType type) {
    switch (type) {
    case shockwave::GamepadType::Xbox: return "Xbox";
    case shockwave::GamepadType::PlayStation5: return "DualSense (PS5)";
    case shockwave::GamepadType::PlayStation4: return "DualShock 4 (PS4)";
    case shockwave::GamepadType::PlayStation3: return "DualShock 3 (PS3)";
    case shockwave::GamepadType::Switch: return "Switch";
    case shockwave::GamepadType::Steam: return "Steam";
    case shockwave::GamepadType::Other: return "Gamepad";
    case shockwave::GamepadType::Unknown: break;
    }
    return "Gamepad";
}

// A -1..1 or 0..1 value as a bar with the number on it.
void axisBar(const char* label, float value, bool centered) {
    const std::string text = std::format("{} {:+.2f}", label, value);
    const float fraction = centered ? (value + 1.0f) * 0.5f : value;
    ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), text.c_str());
}

// A small label that lights up in the accent color while held.
void buttonChip(const char* label, bool held) {
    const ImVec4 color =
        held ? shockwave::gui::accent() : shockwave::gui::semanticColor(shockwave::gui::currentTheme().textDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

}   // namespace

void Editor::loadProjectInput() {
    shockwave::Input& input = engine().input();
    const auto path = shockwave::inputBindingsPath(m_project.root);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::string err;
        if (!input.loadBindingsFile(path, err)) error(err);
    } else {
        input.clearBindingsFile();
    }
    m_inputDirty = false;
    m_inputSeenVersion = 0;   // pick up the new bindings on the next draw
}

void Editor::applyInputEdit() {
    engine().input().setBindings(m_inputEdit);
    m_inputSeenVersion = engine().input().bindingsVersion();
    m_inputDirty = true;
}

void Editor::saveInputBindings() {
    shockwave::Input& input = engine().input();
    const auto path = shockwave::inputBindingsPath(m_project.root);
    std::string err;
    if (!shockwave::saveInputBindings(path, m_inputEdit, err)) {
        error("Couldn't save input bindings: " + err);
        return;
    }
    input.loadBindingsFile(path, err);
    m_inputSeenVersion = input.bindingsVersion();
    m_inputDirty = false;
    log("Saved " + m_project.relative(path));
}

void Editor::drawInputWindow() {
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Input", &m_showInput)) {
        ImGui::End();
        return;
    }
    shockwave::Input& input = engine().input();
    // Follow the live bindings (the file changed on disk, a project opened) unless the
    // user has edits in progress.
    if (!m_inputDirty && m_inputSeenVersion != input.bindingsVersion()) {
        m_inputEdit = input.currentBindings();
        m_inputSeenVersion = input.bindingsVersion();
    }

    // ---- Header: the file and its state, Save / Revert trailing ----
    const auto path = shockwave::inputBindingsPath(m_project.root);
    ImGui::TextDisabled("%s", m_project.relative(path).c_str());
    ImGui::SameLine();
    if (!input.bindingsError().empty()) {
        ImGui::TextColored(shockwave::gui::semanticColor(shockwave::gui::currentTheme().error), "file has errors");
        ImGui::SetItemTooltip("%s", input.bindingsError().c_str());
    } else if (m_inputDirty) {
        ImGui::TextColored(shockwave::gui::semanticColor(shockwave::gui::currentTheme().warning), "unsaved");
    }
    shockwave::gui::alignTrailing(shockwave::gui::controlGroupWidth({"Revert", "Save"}));
    ImGui::BeginDisabled(!m_inputDirty);
    if (shockwave::gui::button("Revert")) loadProjectInput();
    ImGui::SameLine();
    if (shockwave::gui::button("Save")) saveInputBindings();
    ImGui::EndDisabled();
    ImGui::Separator();

    // ---- Controllers ----
    const auto& devices = input.devices();
    char header[64];
    std::snprintf(header, sizeof header, "Controllers (%zu)###controllers", devices.size());
    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (devices.empty()) {
            ImGui::TextDisabled("No controllers connected. Plug one in and it shows up here.");
        }
        for (const InputDevice& d : devices) {
            ImGui::PushID(static_cast<int>(d.id));
            const bool pad = d.kind == InputDevice::Kind::Gamepad;
            std::string title =
                d.name + (pad ? "  ·  " + std::string(typeName(d.type)) + ", player " + std::to_string(d.player + 1)
                              : "  ·  joystick");
            if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (pad) {
                    static const char* axisNames[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
                    for (size_t a = 0; a < d.axes.size(); ++a) axisBar(axisNames[a], d.axes[a], a < 4);
                    for (size_t b = 0; b < d.buttons.size(); ++b) {
                        if (b) ImGui::SameLine();
                        buttonChip(d.buttonLabel(static_cast<shockwave::GamepadButton>(b)).c_str(), d.buttons[b]);
                    }
                    if (d.hasGyro) {
                        ImGui::TextDisabled(
                            "Gyro %+.2f %+.2f %+.2f", static_cast<double>(d.gyro.x), static_cast<double>(d.gyro.y),
                            static_cast<double>(d.gyro.z));
                    }
                } else {
                    for (size_t a = 0; a < d.axes.size(); ++a) {
                        char label[32];
                        std::snprintf(label, sizeof label, "Axis %zu", a);
                        axisBar(label, d.axes[a], true);
                    }
                    for (size_t b = 0; b < d.buttons.size(); ++b) {
                        if (b % 12) ImGui::SameLine();
                        char label[24];
                        std::snprintf(label, sizeof label, "%zu", b);
                        buttonChip(label, d.buttons[b]);
                    }
                    for (size_t h = 0; h < d.hats.size(); ++h) {
                        const uint8_t v = d.hats[h];
                        ImGui::TextDisabled(
                            "Hat %zu: %s%s%s%s", h, v & 1 ? "up " : "", v & 4 ? "down " : "", v & 8 ? "left " : "",
                            v & 2 ? "right" : "");
                    }
                }
                if (d.hasRumble && shockwave::gui::button("Test rumble"))
                    input.rumble(0.6f, 0.6f, 0.4, pad ? d.player : -1);
                if (d.hasTriggerRumble) {
                    ImGui::SameLine();
                    if (shockwave::gui::button("Test triggers")) input.rumbleTriggers(0.6f, 0.6f, 0.4, d.player);
                }
                if (d.hasLight) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::ColorEdit3("Light", &m_padLight.x, ImGuiColorEditFlags_NoInputs))
                        input.setLightColor(m_padLight, d.player);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    // ---- Bindings ----
    if (ImGui::CollapsingHeader("Bindings", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Listening: the first input pressed becomes a binding. Escape cancels; the
        // click that started listening is ignored.
        if (!m_inputListenAction.empty()) {
            if (m_inputListenFrames > 0) {
                --m_inputListenFrames;
            } else if (const auto& pressed = input.lastPressed()) {
                const bool escape = pressed->source == InputBinding::Source::Key &&
                                    pressed->code == static_cast<uint16_t>(shockwave::Key::Escape);
                if (!escape) {
                    m_inputEdit.binds.emplace_back(m_inputListenAction, *pressed);
                    applyInputEdit();
                }
                m_inputListenAction.clear();
            }
        }

        if (shockwave::gui::beginPropertyGrid("input_settings")) {
            shockwave::gui::propertyLabel("Stick deadzone");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##deadzone", &m_inputEdit.deadzone, 0.0f, 0.5f, "%.2f")) applyInputEdit();
            ImGui::EndTable();
        }

        ImGui::SetNextItemWidth(-shockwave::gui::buttonWidth("Add action") - ImGui::GetStyle().ItemSpacing.x);
        const bool enter = ImGui::InputTextWithHint(
            "##newaction", "new action name, e.g. jump", &m_inputNewAction, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((shockwave::gui::button("Add action") || enter) && !m_inputNewAction.empty()) {
            m_inputListenAction = m_inputNewAction;   // listen for its first input straight away
            m_inputListenFrames = 1;
            m_inputNewAction.clear();
        }

        // Group by action, keeping file order within each.
        std::vector<std::string> actions;
        for (const auto& [action, binding] : m_inputEdit.binds)
            if (std::find(actions.begin(), actions.end(), action) == actions.end()) actions.push_back(action);
        if (!m_inputListenAction.empty() &&
            std::find(actions.begin(), actions.end(), m_inputListenAction) == actions.end())
            actions.push_back(m_inputListenAction);
        std::sort(actions.begin(), actions.end());

        int removeIndex = -1;
        if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Inputs");
            for (const std::string& action : actions) {
                ImGui::PushID(action.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(action.c_str());
                ImGui::TableNextColumn();
                // Chips wrap inside the cell rather than running off its edge.
                const float cellRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                bool first = true;
                auto next = [&](const char* label) {
                    const float end = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x +
                                      shockwave::gui::buttonWidth(label);
                    if (!first && end <= cellRight) ImGui::SameLine();
                    first = false;
                };
                for (size_t i = 0; i < m_inputEdit.binds.size(); ++i) {
                    if (m_inputEdit.binds[i].first != action) continue;
                    ImGui::PushID(static_cast<int>(i));
                    const std::string label = shockwave::formatBinding(m_inputEdit.binds[i].second) + "  x";
                    next(label.c_str());
                    if (shockwave::gui::button(label.c_str())) removeIndex = static_cast<int>(i);
                    ImGui::SetItemTooltip("Remove");
                    ImGui::PopID();
                }
                const bool listening = m_inputListenAction == action;
                const char* listenLabel = listening ? "Press an input... (Esc cancels)" : "+ Listen";
                next(listenLabel);
                if (shockwave::gui::toolButton(listenLabel, listening)) {
                    if (listening) {
                        m_inputListenAction.clear();
                    } else {
                        m_inputListenAction = action;
                        m_inputListenFrames = 1;
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (removeIndex >= 0) {
            m_inputEdit.binds.erase(m_inputEdit.binds.begin() + removeIndex);
            applyInputEdit();
        }

        // Typed bindings, for inputs that are awkward to press (or when no device is here).
        ImGui::SetNextItemWidth(-shockwave::gui::buttonWidth("Add") - ImGui::GetStyle().ItemSpacing.x);
        const bool typed = ImGui::InputTextWithHint(
            "##typed", "action input, e.g.  jump pad south   or   steer joy \"G29\" axis 0 +", &m_inputTyped,
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (shockwave::gui::button("Add") || typed) {
            const size_t space = m_inputTyped.find(' ');
            InputBinding binding;
            std::string err;
            if (space == std::string::npos) {
                m_inputTypedError = "write the action, then the input";
            } else if (!shockwave::parseBinding(m_inputTyped.substr(space + 1), binding, err)) {
                m_inputTypedError = err;
            } else {
                m_inputEdit.binds.emplace_back(m_inputTyped.substr(0, space), binding);
                applyInputEdit();
                m_inputTyped.clear();
                m_inputTypedError.clear();
            }
        }
        if (!m_inputTypedError.empty())
            ImGui::TextColored(
                shockwave::gui::semanticColor(shockwave::gui::currentTheme().error), "%s", m_inputTypedError.c_str());
        ImGui::TextDisabled("Changes apply straight away; Save writes them to the file.");
    }
    ImGui::End();
}

}   // namespace editor
