#include "modules/adjust/hud/adjust_hud.hpp"

#include <cmath>

#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/gui_renderer.hpp"
#include "gui/widgets.hpp"

namespace slinky::adjust {

// ColorSetting.getColor(): Rainbow -> ColorUtil.rainbow(0, 0.45, 0.8) (keeps the alpha), Main -> HUD colour.
Argb ColorOption::get() const noexcept {
    switch (current_mode()) {
        case ColorMode::Rainbow:
            return with_alpha(rainbow(0.0f, 0.45f, 0.8f), alpha);
        case ColorMode::Main:
            return modules::adjust_hud::hud_color();
        case ColorMode::Plain:
        default:
            return plain();
    }
}

} // namespace slinky::adjust

namespace slinky::modules {

namespace {

using adjust::Argb;
using adjust_hud::HudElement;

constexpr const char* kHudColorModeOptions[2] = {
    "Plain",
    "Rainbow",
};

void on_game_tick(void* event) noexcept;
void on_gui_frame_poll(void* event) noexcept;
void on_gui_render_2d(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void on_pre_attack(void* event) noexcept;
void on_main_thread_packet(void* event) noexcept;
void on_profile_action(void* event) noexcept;
void on_error_notification(void* event) noexcept;
void on_client_shutdown(void* event) noexcept;
void on_incoming_packet(void* event) noexcept;
void on_render_world_2d(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct AdjustHudState {
    events::Module base{};

    // HUD > Color / Enable Secondary / Secondary / Radiance / Scale / Other > Watermark
    config::ModeSetting color_mode{{}, "Color mode", 2, 0, kHudColorModeOptions, 0, 0, false, {}};
    config::ColorSetting color{"Color", adjust::to_color3f(adjust::argb(67, 90, 209)),
                               adjust::to_color3f(adjust::argb(67, 90, 209))};
    config::BoolSetting enable_secondary{{}, "Enable Secondary", false, false, {}};
    config::ColorSetting secondary{"Secondary", adjust::to_color3f(adjust::argb(148, 81, 171)),
                                   adjust::to_color3f(adjust::argb(148, 81, 171))};
    config::FloatSetting radiance{{}, "Radiance", "%.0f", 1.0f, 0.0f, 0.0f, 345.0f, 0.0f, 0.0f};
    config::FloatSetting scale{{}, "Scale", "%.0f", 1.0f, 0.0f, 1.0f, 16.0f, 2.0f, 2.0f};
    config::BoolSetting watermark{{}, "Watermark", true, true, {}};

    // Colour transition (HUDModule.startColorTransition / updateColorTransition)
    Argb last_plain = 0;
    bool has_last_plain = false;
    Argb previous_color = 0;
    bool transition_active = false;
    adjust::Animation transition{};
    Argb animated = 0;

    int armor_offset = 0;
    float toggled_bottom = 0.0f;
    int64_t last_frame_ms = 0;

    AdjustHudState() noexcept {
        base.name = "HUD";
        base.description = "Adjust heads-up display: watermark, toggled list, information, target HUD and more.";
        base.category = events::ModuleCategory::Visual;
        base.enabled = true; // Adjust's HUD starts enabled
        base.flags = events::kModuleFlagStyleAdjust; // listed in the toggled list (HUDModule.setHidden(false))
        base.keybind = 0;
        base.hold_mode = false;
        base.events[events::kEventGameTick] = {on_game_tick, 0, 0};
        base.events[events::kEventGuiFramePoll] = {on_gui_frame_poll, 0, 0};
        base.events[events::kEventGuiRender2D] = {on_gui_render_2d, 100, 0}; // under the ClickGUI
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.events[events::kEventPreAttackOrUseEntity] = {on_pre_attack, 0, 0};
        base.events[events::kEventMainThreadPacket] = {on_main_thread_packet, 0, 0};
        base.events[events::kEventProfileAction] = {on_profile_action, 0, 0};
        base.events[events::kEventErrorNotification] = {on_error_notification, 0, 0};
        base.events[events::kEventClientShutdown] = {on_client_shutdown, 0, 0};
        base.events[events::kEventIncomingPacket] = {on_incoming_packet, 0, 0};
        base.events[events::kEventRenderWorld2DProj] = {on_render_world_2d, -100, 0}; // late: over ESP overlays
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        base.format_hud_tag = nullptr;
        events::set_module_config_key(&base, "Adjust HUD");
    }
};

AdjustHudState g_hud{};

// Element order = Adjust's HUD render order (HudElements + HUDModule.onRender2D).
const HudElement* const* elements() noexcept {
    static const HudElement* const s_elements[] = {
        &adjust_hud::element_watermark(),
        &adjust_hud::element_toggled(),
        &adjust_hud::element_information(),
        &adjust_hud::element_tabgui(),
        &adjust_hud::element_potions(),
        &adjust_hud::element_armor(),
        &adjust_hud::element_target(),
        &adjust_hud::element_widgets(),
        &adjust_hud::element_notifications(),
        nullptr,
    };
    return s_elements;
}

template <typename Fn>
void for_each_element(Fn&& fn) noexcept {
    for (const HudElement* const* it = elements(); *it != nullptr; ++it) {
        fn(**it);
    }
}

Argb plain_hud_color() noexcept {
    return adjust::from_color3f(g_hud.color.value);
}

// Starts the 185 ms transition whenever the plain HUD colour changes, then advances it.
void update_color_transition() noexcept {
    const Argb now_plain = plain_hud_color();
    if (!g_hud.has_last_plain) {
        g_hud.last_plain = now_plain;
        g_hud.has_last_plain = true;
    } else if (now_plain != g_hud.last_plain) {
        g_hud.previous_color = g_hud.transition_active ? g_hud.animated : g_hud.last_plain;
        g_hud.last_plain = now_plain;
        g_hud.transition = adjust::Animation(adjust::Easing::Linear, 185);
        g_hud.transition_active = true;
    }
    if (!g_hud.transition_active) {
        return;
    }
    const float progress = g_hud.transition.update(1.0f);
    if (g_hud.transition.finished) {
        g_hud.transition_active = false;
        return;
    }
    g_hud.animated = adjust::blend(now_plain, g_hud.previous_color, progress);
}

void on_game_tick(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (ctx == nullptr || ctx->local_player == nullptr || ctx->world == nullptr) {
        return;
    }
    for_each_element([&](const HudElement& e) noexcept {
        if (e.tick != nullptr) {
            e.tick(ctx);
        }
    });
}

// The Adjust HUD animates continuously (colour pulses, list slides), so redraw every frame while running.
void on_gui_frame_poll(void* event) noexcept {
    auto* ev = static_cast<events::GuiFramePollEvent*>(event);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->local_player == nullptr || ctx->world == nullptr) {
        return;
    }
    ev->render_flags |= 3u;
}

void on_gui_render_2d(void* event) noexcept {
    auto* ev = static_cast<events::GuiRender2DEvent*>(event);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->local_player == nullptr || ctx->world == nullptr || ev->renderer == nullptr) {
        return;
    }

    update_color_transition();

    const adjust::HudFrame frame = adjust::begin_hud(ev->renderer, adjust_hud::requested_hud_scale());
    if (!frame.valid) {
        return;
    }

    const int64_t now = adjust::now_ms();
    float dt = ev->delta_time;
    if (g_hud.last_frame_ms != 0 && dt <= 0.0f) {
        dt = static_cast<float>(now - g_hud.last_frame_ms) / 1000.0f;
    }
    g_hud.last_frame_ms = now;

    adjust_hud::HudContext hud_ctx{};
    hud_ctx.game = ctx;
    hud_ctx.renderer = ev->renderer;
    hud_ctx.frame = &frame;
    hud_ctx.delta_time = dt;

    for_each_element([&](const HudElement& e) noexcept {
        if (e.render != nullptr) {
            e.render(hud_ctx);
            adjust::set_global_alpha(1.0f);
        }
    });

    adjust::end_hud();
}

void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    for_each_element([&](const HudElement& e) noexcept {
        if (e.on_module_state_changed != nullptr) {
            e.on_module_state_changed(ev);
        }
    });
}

void on_pre_attack(void* event) noexcept {
    auto* ev = static_cast<events::AttackOrUseEntityEvent*>(event);
    for_each_element([&](const HudElement& e) noexcept {
        if (e.on_attack != nullptr) {
            e.on_attack(ev);
        }
    });
}

void on_main_thread_packet(void* event) noexcept {
    auto* ev = static_cast<events::MainThreadPacketEvent*>(event);
    for_each_element([&](const HudElement& e) noexcept {
        if (e.on_main_thread_packet != nullptr) {
            e.on_main_thread_packet(ev);
        }
    });
}

void on_profile_action(void* event) noexcept {
    auto* ev = static_cast<events::ProfileActionEvent*>(event);
    for_each_element([&](const HudElement& e) noexcept {
        if (e.on_profile_action != nullptr) {
            e.on_profile_action(ev);
        }
    });
}

void on_error_notification(void* event) noexcept {
    auto* ev = static_cast<events::ErrorNotificationEvent*>(event);
    for_each_element([&](const HudElement& e) noexcept {
        if (e.on_error_notification != nullptr) {
            e.on_error_notification(ev);
        }
    });
}

void on_client_shutdown(void* /*event*/) noexcept {
    for_each_element([&](const HudElement& e) noexcept {
        if (e.on_client_shutdown != nullptr) {
            e.on_client_shutdown();
        }
    });
}

void on_incoming_packet(void* event) noexcept {
    auto* ev = static_cast<events::NettyPacketEvent*>(event);
    for_each_element([&](const HudElement& e) noexcept {
        if (e.on_incoming_packet != nullptr) {
            e.on_incoming_packet(ev);
        }
    });
}

void on_render_world_2d(void* event) noexcept {
    auto* ev = static_cast<events::RenderWorld2DProjEvent*>(event);
    for_each_element([&](const HudElement& e) noexcept {
        if (e.on_render_world_2d != nullptr) {
            e.on_render_world_2d(ev);
        }
    });
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_hud.color_mode.render(gui, renderer);
    if (g_hud.color_mode.value == 0) {
        g_hud.color.render(gui, renderer);
        g_hud.enable_secondary.render(gui, renderer);
        if (g_hud.enable_secondary.value) {
            g_hud.secondary.render(gui, renderer);
        }
    }
    g_hud.radiance.render(gui, renderer);
    g_hud.scale.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Other");
    g_hud.watermark.render(gui, renderer);

    for_each_element([&](const HudElement& e) noexcept {
        if (e.render_settings != nullptr) {
            gui::widgets::render_section_header(gui, renderer, e.name != nullptr ? e.name : "");
            e.render_settings(gui, renderer);
        }
    });
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_hud.color_mode.visit(archive, "color_mode");
    g_hud.color.visit(archive, "color");
    g_hud.enable_secondary.visit(archive, "enable_secondary");
    g_hud.secondary.visit(archive, "secondary");
    g_hud.radiance.visit(archive, "radiance");
    g_hud.scale.visit(archive, "scale");
    g_hud.watermark.visit(archive, "watermark");
    for_each_element([&](const HudElement& e) noexcept {
        if (e.visit_config != nullptr) {
            e.visit_config(archive);
        }
    });
}

} // namespace

events::Module* get_adjust_hud_module() noexcept {
    return &g_hud.base;
}

namespace adjust_hud {

Argb hud_color() noexcept {
    if (g_hud.color_mode.value == 1) {
        return adjust::rainbow(0.0f, 0.45f, 0.8f);
    }
    if (g_hud.transition_active) {
        return g_hud.animated;
    }
    return plain_hud_color();
}

Argb hud_color(int alpha) noexcept {
    return adjust::with_alpha(hud_color(), alpha);
}

bool hud_color_rainbow() noexcept {
    return g_hud.color_mode.value == 1;
}

bool secondary_active() noexcept {
    return g_hud.enable_secondary.value && g_hud.color_mode.value == 0;
}

Argb secondary_color() noexcept {
    return adjust::from_color3f(g_hud.secondary.value);
}

Argb secondary_for(Argb primary) noexcept {
    if (!secondary_active()) {
        return primary;
    }
    const Argb second = secondary_color();
    return adjust::alpha_of(primary) == 255 ? second : adjust::with_alpha(second, adjust::alpha_of(primary));
}

Argb secondary_or_darker(Argb primary) noexcept {
    if (secondary_active()) {
        return secondary_for(primary);
    }
    return adjust::argb(static_cast<int>(adjust::red_of(primary) * 0.8f),
                        static_cast<int>(adjust::green_of(primary) * 0.8f),
                        static_cast<int>(adjust::blue_of(primary) * 0.8f), adjust::alpha_of(primary));
}

float radiance() noexcept {
    return g_hud.radiance.value;
}

Argb theme_color_or(Argb fallback) noexcept {
    const int shift = static_cast<int>(g_hud.radiance.value);
    if (shift < 2) {
        return fallback;
    }
    const Argb base_color = plain_hud_color();
    float hsl[3]{};
    adjust::to_hsl(base_color, hsl);
    hsl[0] = std::fmod(hsl[0] + static_cast<float>(shift), 360.0f);
    return adjust::hsl_to_argb(hsl, adjust::alpha_of(base_color));
}

float requested_hud_scale() noexcept {
    return adjust::hud_scale_from_setting(static_cast<int>(g_hud.scale.value));
}

bool watermark_enabled() noexcept {
    return g_hud.watermark.value;
}

int armor_offset() noexcept {
    return g_hud.armor_offset;
}

void set_armor_offset(int offset) noexcept {
    g_hud.armor_offset = offset;
}

float toggled_list_bottom() noexcept {
    return g_hud.toggled_bottom;
}

void set_toggled_list_bottom(float bottom) noexcept {
    g_hud.toggled_bottom = bottom;
}

} // namespace adjust_hud
} // namespace slinky::modules
