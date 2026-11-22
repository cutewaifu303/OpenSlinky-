// Port of com.adjustclient.module.impl.render.TrueSightModule: "Allows you to see invisible players."
//
// Missing hook (phase 2) - Minecraft 1.8.9, class net.minecraft.client.renderer.entity.RendererLivingEntity
// (notch bjl), method renderModel(EntityLivingBase, float, float, float, float, float, float) (notch
// a(Lpr;FFFFFF)V, SRG func_77036_a):
//   at the head, replace `boolean flag = !entity.isInvisible();` by a call into the client that starts from
//   invisible = entity.isInvisible(), body_alpha = 1.0 and runs adjust_true_sight_apply(jni, entity, &invisible,
//   &body_alpha). Then:
//     - if !invisible, or vanilla's `!entity.isInvisibleToPlayer(mc.thePlayer)` path: bind the entity texture and
//       render mainModel;
//     - if body_alpha < 1 (or the vanilla translucent path, alpha 0.15): around mainModel.render(...) do
//       pushMatrix, enableColorMaterial, colorMaterial(1032, 5634), resetColor, color(1, 1, 1, alpha),
//       depthMask(false), enableBlend, tryBlendFuncSeparate(770, 771, 1, 0), alphaFunc(516, 0.003921569F),
//       and afterwards color(1, 1, 1, 1), disableBlend, alphaFunc(516, 0.1F), popMatrix, depthMask(true)
//       (this is exactly Adjust's patched renderModel).
// Until then the module only holds its settings.
//
// Adjust's AntiBot / WorldUtil.isTeammate checks map to the OpenSlinky++ AntiBot / Teams player tags.

#include "modules/adjust/render/true_sight/true_sight.hpp"

#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules {

namespace {

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct TrueSightState {
    events::Module base{};

    config::FloatSetting opacity{{}, "Opacity", "%.0f%%", 1.0f, 0.0f, 10.0f, 100.0f, 50.0f, 50.0f};
    config::BoolSetting ignore_teammates{{}, "Ignore teammates", false, false, {}};

    TrueSightState() noexcept {
        base.name = "True Sight";
        base.description = "Allows you to see invisible players.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust True Sight");
    }
};

TrueSightState g_state{};

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.opacity.render(gui, renderer);
    g_state.ignore_teammates.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.opacity.visit(archive, "opacity");
    g_state.ignore_teammates.visit(archive, "ignore_teammates");
}

} // namespace

events::Module* get_adjust_true_sight_module() noexcept {
    return &g_state.base;
}

// TrueSightModule.apply
bool adjust_true_sight_apply(events::JniContext* jni, jobject entity, bool* invisible, float* body_alpha) noexcept {
    if (!events::module_is_running(&g_state.base) || jni == nullptr || jni->env == nullptr || entity == nullptr ||
        invisible == nullptr || body_alpha == nullptr) {
        return false;
    }
    if (minecraft::as_entity_player(jni, entity) == nullptr) {
        return false;
    }
    if (minecraft::player_has_tag_flags(jni, entity, minecraft::kPlayerTagBot)) {
        return false; // AntiBotModule.isBot
    }
    if (g_state.ignore_teammates.value && minecraft::player_has_tag_flags(jni, entity, minecraft::kPlayerTagTeammate)) {
        return false; // WorldUtil.isTeammate
    }
    if (!*invisible) {
        return false;
    }
    *invisible = false;
    *body_alpha = g_state.opacity.value / 100.0f;
    return true;
}

} // namespace slinky::modules
