// Port of com.adjustclient.module.impl.render.hud.widget.HudWidgets (HUD > Widgets): Radar, Session Information,
// Click Speed and Memory Usage. The widgets live in widgets/*.cpp; this element wires them into the Adjust HUD.
//
// HUDModule creates the widget settings with widget.createSettings(widgets) inside the "Widgets" expand group:
// the Radar gets its own group ("Enabled", Size, Distance, Smoothing and the six RadarGroups), the other widgets a
// single On/Off toggle named after the widget (all default off). Settings keys are prefixed "widgets_".
//
// Adjust widgets can be dragged in Adjust's HUD editor; OpenSlinky++ has no HUD editor, so they stay at the default
// positions of HudWidgets.createMovable (Radar 2,150 / Session Information 2,250 / Click Speed 350,250 /
// Memory Usage 350,280, HUD units from the top-left corner).
//
// The Player List and Event Timers groups Adjust shows in the same settings group are separate features (not
// HudWidgets) and are not part of this element.

#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/hud/widgets/widgets.hpp"

namespace slinky::modules::adjust_hud {

namespace {

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    // The single-toggle widgets first, then the Radar with its sub sections (Adjust lists the Radar first; it is
    // moved last here because its group headers would otherwise swallow the following toggles).
    widgets::session_render_settings(gui, renderer);
    widgets::click_speed_render_settings(gui, renderer);
    widgets::memory_render_settings(gui, renderer);
    widgets::radar_render_settings(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    widgets::radar_visit_config(ar);
    widgets::session_visit_config(ar);
    widgets::click_speed_visit_config(ar);
    widgets::memory_visit_config(ar);
}

void tick(events::GameContext* ctx) noexcept {
    widgets::radar_tick(ctx);
    widgets::session_tick(ctx);
}

// Same order as HudWidgets.widgets (each widget is its own Render2D listener in Adjust).
void render(const HudContext& ctx) noexcept {
    if (ctx.game == nullptr || ctx.game->local_player == nullptr) {
        return;
    }
    widgets::radar_render(ctx);
    adjust::set_global_alpha(1.0f);
    widgets::session_render(ctx);
    widgets::click_speed_render(ctx);
    widgets::memory_render(ctx);
}

void on_attack(events::AttackOrUseEntityEvent* ev) noexcept {
    widgets::click_speed_on_attack(ev);
}

void on_client_shutdown() noexcept {
    widgets::radar_shutdown();
    widgets::session_shutdown();
}

} // namespace

const HudElement& element_widgets() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Widgets";
        e.render_settings = render_settings;
        e.visit_config = visit_config;
        e.tick = tick;
        e.render = render;
        e.on_attack = on_attack;
        e.on_client_shutdown = on_client_shutdown;
        return e;
    }();
    return s_element;
}

} // namespace slinky::modules::adjust_hud
