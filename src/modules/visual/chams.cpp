#include "modules/visual/chams.hpp"

#include <glad/glad.h>

#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "events/module_manager.hpp"
#include "modules/adjust/render/chams/adjust_chams.hpp"
#include "modules/adjust/render/skeleton/adjust_skeleton.hpp"
#include "modules/module_interop.hpp"

namespace slinky::modules {

namespace {

config::BoolSetting s_hide_friendlies{{}, "Hide friendlies", false, false};

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    s_hide_friendlies.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    s_hide_friendlies.visit(ar, "hide_friendlies");
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Chams";
    mod.description = "Renders players behind blocks.";
    mod.category = events::ModuleCategory::Visual;
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleSlinky; // OpenSlinky++: Slinky++ visual style
    return mod;
}

} // namespace

// Matches sub_18005FB30
void chams_render_living_model(
    events::JniContext* jni,
    jobject renderer,
    jobject entity,
    double x,
    double y,
    double z,
    float entity_yaw,
    float partial_ticks) noexcept
{
    // Adjust Chams (Adjust visual style) renders the entity itself when it applies.
    if (!adjust_chams_render_living(jni, renderer, entity, x, y, z, entity_yaw, partial_ticks)) {
        const uint32_t exclude_mask = s_hide_friendlies.value
            ? minecraft::kPlayerTagExcludedMask
            : minecraft::kPlayerTagBot;

        if (events::module_is_running(get_chams_module()) &&
            !minecraft::player_has_tag_flags(jni, entity, exclude_mask))
        {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, -3000000.0f);
            minecraft::renderer_living_do_render_super(
                jni, renderer, entity, x, y, z, entity_yaw, partial_ticks);
            glPolygonOffset(0.0f, 0.0f); // reset so later polygon offsets in Minecraft are unaffected
            glDisable(GL_POLYGON_OFFSET_FILL);
        } else {
            minecraft::renderer_living_do_render_super(
                jni, renderer, entity, x, y, z, entity_yaw, partial_ticks);
        }
    }

    // Adjust Skeleton draws over the freshly animated player model.
    adjust_skeleton_render_player(jni, renderer, entity, partial_ticks);
}

events::Module* get_chams_module() noexcept {
    static events::Module s_module = create_module();
    return &s_module;
}

} // namespace slinky::modules
