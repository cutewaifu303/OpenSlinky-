// "Click GUI" module: the settings Adjust keeps under HUD > Click GUI (HUDModule: Bind, Scale, Separators).
//
// Scale = UserConfiguration.secondaryScale (1..16, default 2, 16 = "Auto"; HUDModule.getClickGuiScale), Separators =
// HUDModule.clickGuiSeparators. The Click GUI bind is the OpenSlinky++ GUI module's keybind (not duplicated here).
// Global settings (like Adjust's user configuration they are not part of a profile).

#include "modules/adjust/clickgui/adjust_click_gui.hpp"

#include "adjust/kit.hpp"
#include "config/settings.hpp"
#include "modules/adjust/clickgui/internal.hpp"

namespace slinky::modules {

namespace {

struct ClickGuiState {
    events::Module base{};
    config::FloatSetting scale{{}, "Scale", "%.0f", 1.0f, 0.0f, 1.0f, 16.0f, 2.0f, 2.0f};
    config::BoolSetting separators{{}, "Separators", true, true, {}};

    ClickGuiState() noexcept {
        base.name = "Click GUI";
        base.description = "Settings of the Click GUI.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagNoToggleSwitch |
                     events::kModuleFlagNoKeybind | events::kModuleFlagHiddenFromHud |
                     events::kModuleFlagNoHoldMode | events::kModuleFlagGlobalSettings;
        base.render_settings = &render_settings;
        base.visit_config = &visit_config;
        events::set_module_config_key(&base, "Adjust Click GUI");
    }

    static void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
    static void visit_config(config::ConfigArchive* ar) noexcept;
};

ClickGuiState& state() noexcept {
    static ClickGuiState s{};
    return s;
}

void ClickGuiState::render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    state().scale.render(gui, renderer);
    state().separators.render(gui, renderer);
}

void ClickGuiState::visit_config(config::ConfigArchive* ar) noexcept {
    state().scale.visit(ar, "scale");
    state().separators.visit(ar, "separators");
}

} // namespace

events::Module* get_adjust_click_gui_module() noexcept {
    return &state().base;
}

namespace adjust_clickgui::detail {

const config::FloatSetting* click_gui_scale_setting() noexcept {
    return &state().scale;
}

float click_gui_scale() noexcept {
    const int value = static_cast<int>(state().scale.value + 0.5f);
    return adjust::hud_scale_from_setting(value < 1 ? 1 : value);
}

bool click_gui_separators() noexcept {
    return state().separators.value;
}

} // namespace adjust_clickgui::detail

} // namespace slinky::modules
