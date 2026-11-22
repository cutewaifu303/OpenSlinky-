// Port of com.adjustclient.module.impl.render.HurtcamModule.
//
// Settings (Adjust):
//   Local (expand)        Disabled   bool  true
//   Model color (expand)  Enabled    bool  false
//                         Color      color 0xFF435AD1 (Plain), hidden while "Enabled" is off
//   Armor                            bool  true
//
// Behaviour lives in Adjust's patched Minecraft classes, ported as hooks in src/adjust/hooks/hurtcam_hooks.cpp.

#include "modules/adjust/render/hurtcam/hurtcam.hpp"

#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"

namespace slinky::modules {

namespace {

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct HurtcamState {
    events::Module base{};

    // Local
    config::BoolSetting local_disabled{{}, "Disabled", true, true, {}};
    // Model color
    config::BoolSetting model_color_enabled{{}, "Enabled", false, false, {}};
    adjust::ColorOption model_color_color{"Color", "Color mode", 0xFF435AD1u};
    // Armor
    config::BoolSetting armor{{}, "Armor", true, true, {}};

    HurtcamState() noexcept {
        base.name = "Hurtcam";
        base.description = "Changes the hurt camera shake and the hurt tint of entities.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Hurtcam");
    }
};

HurtcamState g_state{};

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Local");
    g_state.local_disabled.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Model color");
    g_state.model_color_enabled.render(gui, renderer);
    if (g_state.model_color_enabled.value) { // hideIf: !modelColorEnabled
        g_state.model_color_color.render(gui, renderer);
    }

    gui::widgets::render_section_header(gui, renderer, "General");
    g_state.armor.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.local_disabled.visit(archive, "local_disabled");
    g_state.model_color_enabled.visit(archive, "model_color_enabled");
    g_state.model_color_color.visit(archive, "model_color_color", "model_color_color_mode");
    g_state.armor.visit(archive, "armor");
}

} // namespace

events::Module* get_adjust_hurtcam_module() noexcept {
    return &g_state.base;
}

// EntityRenderer.hurtCameraEffect: INSTANCE.isEnabled() && localDisabled.isEnabled() -> return.
bool adjust_hurtcam_disables_local() noexcept {
    return events::module_is_running(&g_state.base) && g_state.local_disabled.value;
}

// RendererLivingEntity.setBrightness: INSTANCE.isEnabled() && modelColorEnabled.isEnabled() -> modelColorColor.getColor().
bool adjust_hurtcam_model_color(adjust::Argb* out_color) noexcept {
    if (out_color == nullptr || !events::module_is_running(&g_state.base) || !g_state.model_color_enabled.value) {
        return false;
    }
    *out_color = g_state.model_color_color.get();
    return true;
}

// LayerArmorBase.shouldCombineTextures: INSTANCE.isEnabled() && armor.isEnabled().
bool adjust_hurtcam_tints_armor() noexcept {
    return events::module_is_running(&g_state.base) && g_state.armor.value;
}

} // namespace slinky::modules
