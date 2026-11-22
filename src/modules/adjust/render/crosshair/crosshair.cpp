// Port of com.adjustclient.module.impl.render.CrosshairModule (+ CrosshairMode, CrosshairFadeCondition):
// a custom NanoVG crosshair (Cross / Gapped / Dot) with a moving gap animation and an inactivity fade.
//
// Drawn on the OpenSlinky++ 2D layer (kEventGuiRender2D) in Adjust HUD units. Vanilla's crosshair cannot be
// hidden without a hook (see adjust_crosshair_replaces_vanilla()); until then both are visible.

#include "modules/adjust/render/crosshair/crosshair.hpp"

#include <algorithm>
#include <cmath>

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"

namespace slinky::modules {

namespace {

using adjust::Argb;

constexpr const char* kModeOptions[3] = {"Cross", "Gapped", "Dot"}; // CrosshairMode
enum CrosshairMode : int32_t { kModeCross = 0, kModeGapped = 1, kModeDot = 2 };

void on_gui_frame_poll(void* event) noexcept;
void on_gui_render_2d(void* event) noexcept;
void on_post_living_update(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct CrosshairState {
    events::Module base{};

    config::ModeSetting mode{{}, "Mode", 3, 0, kModeOptions, kModeGapped, kModeGapped, false, {}};
    adjust::ColorOption color{"Color", "Color mode", 0xFF435AD1u, adjust::ColorMode::Plain};
    // Inactivity Fade
    config::BoolSetting fade_enabled{{}, "Enabled", false, false, {}};
    config::FloatSetting fade_alpha{{}, "Alpha", "%.0f", 1.0f, 0.0f, 0.0f, 255.0f, 100.0f, 100.0f};
    config::FloatSetting fade_requirement{{}, "Requirement", "%.0f ticks", 5.0f, 0.0f, 5.0f, 60.0f, 10.0f, 10.0f};
    // Gap (Gapped mode only)
    config::FloatSetting gap_moving{{}, "Moving", "%.1f", 0.5f, 0.0f, 1.5f, 4.0f, 2.5f, 2.5f};
    config::FloatSetting gap_normal{{}, "Normal", "%.1f", 0.5f, 0.0f, 1.5f, 3.0f, 1.5f, 1.5f};
    config::FloatSetting width{{}, "Width", "%.1f", 0.5f, 0.0f, 1.0f, 5.0f, 3.0f, 3.0f};
    config::FloatSetting thickness{{}, "Thickness", "%.1f", 0.5f, 0.0f, 0.5f, 1.0f, 1.0f, 1.0f};
    config::BoolSetting dot{{}, "Dot", true, true, {}};
    config::BoolSetting outline{{}, "Outline", true, true, {}};

    adjust::Animation gap_animation{adjust::Easing::EaseOutSine, 100};
    adjust::Animation alpha_animation{adjust::Easing::Linear, 125};
    int inactive_ticks = 0; // CrosshairModule.inactiveSeconds (counts player updates)

    CrosshairState() noexcept {
        base.name = "Crosshair";
        base.description = "Replaces the crosshair with a customizable one.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust; // CrosshairModule.setHidden(false): listed in the HUD
        base.events[events::kEventGuiFramePoll] = {on_gui_frame_poll, 0, 0};
        base.events[events::kEventGuiRender2D] = {on_gui_render_2d, 110, 0}; // below the Adjust HUD
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Crosshair");
    }
};

CrosshairState g_state{};

// ---- Minecraft state ----------------------------------------------------------------------------------------

// CrosshairModule.isStationary(): no movement input or no horizontal motion.
bool is_stationary(events::JniContext* jni, jobject player) noexcept {
    if (jni == nullptr || player == nullptr) {
        return true;
    }
    const float forward = minecraft::living_get_move_forward(jni, player);
    const float strafe = minecraft::living_get_move_strafing(jni, player);
    if (forward == 0.0f && strafe == 0.0f) {
        return true;
    }
    const double mx = minecraft::entity_get_motion_x(jni, player);
    const double mz = minecraft::entity_get_motion_z(jni, player);
    return std::hypot(mx, mz) == 0.0;
}

int animated_opacity() noexcept {
    return std::clamp(static_cast<int>(g_state.alpha_animation.value), 0, 255);
}

// CrosshairModule.getGap()
float get_gap(events::JniContext* jni, jobject player) noexcept {
    if (!is_stationary(jni, player) && animated_opacity() >= 85) {
        return g_state.gap_moving.value;
    }
    return g_state.gap_normal.value;
}

struct OverlayState {
    bool draw = true;      // mc.ingameGUI.showCrosshair() and the in-game overlay is rendered
    int gui_scale = 0;     // mc.gameSettings.guiScale (0 = auto)
};

// Mirrors the conditions under which GuiIngame posts Adjust's Render2DEvent and the crosshair check:
// the overlay is drawn when !hideGUI || currentScreen != null, then `mc.ingameGUI.showCrosshair()`.
// OpenSlinky++ draws on top of everything, so a screen other than chat also hides the crosshair (in Adjust it is
// drawn underneath the screen).
OverlayState query_overlay(events::GameContext* ctx) noexcept {
    OverlayState out{};
    events::JniContext* jni = ctx->jni;
    if (jni == nullptr || jni->env == nullptr || ctx->minecraft == nullptr || !adjust::jni::is_1_8(jni)) {
        return out;
    }
    JNIEnv* env = jni->env;
    jclass mc_cls = jni->mappings->cls_minecraft;
    jclass settings_cls = jni->mappings->cls_game_settings;

    static const jfieldID fid_hide_gui =
        adjust::jni::field(jni, settings_cls, {"aA", "hideGUI", "field_74319_N"}, "Z", "Z");
    static const jfieldID fid_gui_scale =
        adjust::jni::field(jni, settings_cls, {"aL", "guiScale", "field_74335_Z"}, "I", "I");
    static const jfieldID fid_current_screen = adjust::jni::field(
        jni, mc_cls, {"m", "currentScreen", "field_71462_r"}, "Laxu;", "Lnet/minecraft/client/gui/GuiScreen;");
    static const jfieldID fid_ingame_gui = adjust::jni::field(
        jni, mc_cls, {"q", "ingameGUI", "field_71456_v"}, "Lavo;", "Lnet/minecraft/client/gui/GuiIngame;");
    static const jclass cls_ingame = adjust::jni::find_class(jni, "avo", "net/minecraft/client/gui/GuiIngame");
    static const jmethodID mid_show_crosshair =
        adjust::jni::method(jni, cls_ingame, {"b", "showCrosshair", "func_175183_b"}, "()Z", "()Z");
    static const jclass cls_gui_chat = adjust::jni::find_class(jni, "awv", "net/minecraft/client/gui/GuiChat");

    jobject settings = minecraft::mc_get_game_settings(jni, ctx->minecraft);
    bool hide_gui = false;
    if (settings != nullptr) {
        if (fid_hide_gui != nullptr) {
            hide_gui = env->GetBooleanField(settings, fid_hide_gui) != JNI_FALSE;
        }
        if (fid_gui_scale != nullptr) {
            out.gui_scale = env->GetIntField(settings, fid_gui_scale);
        }
        env->DeleteLocalRef(settings);
    }

    if (fid_current_screen != nullptr) {
        jobject screen = env->GetObjectField(ctx->minecraft, fid_current_screen);
        if (screen != nullptr) {
            const bool is_chat = cls_gui_chat != nullptr && env->IsInstanceOf(screen, cls_gui_chat);
            env->DeleteLocalRef(screen);
            if (!is_chat) {
                out.draw = false;
                return out;
            }
            hide_gui = false; // the overlay is rendered while a screen is open
        }
    }
    if (hide_gui) {
        out.draw = false;
        return out;
    }

    if (fid_ingame_gui != nullptr && mid_show_crosshair != nullptr) {
        jobject ingame = env->GetObjectField(ctx->minecraft, fid_ingame_gui);
        if (ingame != nullptr) {
            out.draw = env->CallBooleanMethod(ingame, mid_show_crosshair) != JNI_FALSE;
            if (adjust::jni::clear_exception(jni)) {
                out.draw = true;
            }
            env->DeleteLocalRef(ingame);
        }
    }
    return out;
}

// ---- NvgRenderer commands (rect / strokeRect in HUD units) ------------------------------------------------------

void rect(Argb color, float x, float y, float w, float h) noexcept {
    if (adjust::alpha_of(color) == 0) {
        return;
    }
    adjust::fill_rect(color, x, y, x + w, y + h);
}

void stroke(bool outside, Argb color, float x, float y, float w, float h, float line_width) noexcept {
    if (adjust::alpha_of(color) == 0) {
        return;
    }
    adjust::stroke_rect(outside, color, x, y, x + w, y + h, line_width);
}

void draw_crosshair(float x, float y, float animated_gap, Argb edge, Argb foreground) noexcept {
    const float length = g_state.width.value;
    const float thick = g_state.thickness.value;
    const float left = x - 0.5f;

    switch (g_state.mode.value) {
        case kModeDot:
            stroke(false, edge, x - 1.0f, y - 0.5f, thick + 1.0f, thick + 1.0f, 0.5f);
            rect(foreground, left, y, thick, thick);
            break;

        case kModeGapped: {
            float start = x - length - animated_gap - 0.5f;
            stroke(true, edge, start, y, length, thick, 0.5f);
            rect(foreground, start, y, length, thick);
            start = y - animated_gap - length;
            stroke(true, edge, left, start, thick, length, 0.5f);
            rect(foreground, left, start, thick, length);
            start = x + animated_gap + thick - 0.5f;
            stroke(true, edge, start, y, length, thick, 0.5f);
            rect(foreground, start, y, length, thick);
            start = y + animated_gap + thick;
            stroke(true, edge, left, start, thick, length, 0.5f);
            rect(foreground, left, start, thick, length);
            if (g_state.dot.value) {
                stroke(false, edge, x - 1.0f, y - 0.5f, thick + 1.0f, thick + 1.0f, 0.5f);
                rect(foreground, left, y, thick, thick);
            }
            break;
        }

        case kModeCross:
        default: {
            const float start_x = x - length - 0.5f;
            const float outer_x = start_x - 0.5f;
            rect(edge, outer_x, y, 0.5f, thick);
            rect(edge, outer_x, y - 0.5f, length, 0.5f);
            rect(edge, outer_x, y + thick, length, 0.5f);
            rect(foreground, start_x, y, length, thick);

            const float top = y - length;
            rect(edge, left, top - 0.5f, thick, 0.5f);
            rect(edge, x - 1.0f, top - 0.5f, 0.5f, length + 0.5f);
            rect(edge, x + thick - 0.5f, top - 0.5f, 0.5f, length);
            rect(foreground, left, top, thick, length + 1.0f);

            const float right = x + thick - 0.5f;
            rect(edge, right, y - 0.5f, length, 0.5f);
            rect(edge, right + length, y - 0.5f, 0.5f, thick + 1.0f);
            rect(edge, right, y + thick, length, 0.5f);
            rect(foreground, right, y, length, thick);

            rect(edge, x - 1.0f, y + thick, 0.5f, length);
            rect(edge, x - 1.0f, y + thick + length, thick + 0.5f, 0.5f);
            rect(edge, x + thick - 0.5f, y + thick, 0.5f, length + 0.5f);
            rect(foreground, left, y + thick, thick, length);
            break;
        }
    }
}

// ---- events -------------------------------------------------------------------------------------------------

void on_gui_frame_poll(void* event) noexcept {
    auto* ev = static_cast<events::GuiFramePollEvent*>(event);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->local_player == nullptr || ctx->world == nullptr) {
        return;
    }
    ev->render_flags |= 3u; // animated (gap / fade)
}

// CrosshairModule.onRender2D
void on_gui_render_2d(void* event) noexcept {
    auto* ev = static_cast<events::GuiRender2DEvent*>(event);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->local_player == nullptr || ctx->world == nullptr || ev->renderer == nullptr) {
        return;
    }
    const OverlayState overlay = query_overlay(ctx);
    if (!overlay.draw) {
        return;
    }

    // Adjust draws at the HUD scale when it equals Minecraft's GUI scale, otherwise at a fixed NanoVG scale of 2
    // (NvgContext.setScale(2.0f)). The crosshair is always centred on the framebuffer here.
    const float hud_scale = adjust_hud::requested_hud_scale();
    const bool custom_scale = static_cast<double>(hud_scale) != static_cast<double>(overlay.gui_scale);
    const adjust::HudFrame frame = adjust::begin_hud(ev->renderer, custom_scale ? 2.0f : hud_scale);
    if (!frame.valid) {
        return;
    }

    const bool faded = g_state.fade_enabled.value &&
                       g_state.inactive_ticks >= static_cast<int>(g_state.fade_requirement.value);
    const float alpha = faded ? g_state.fade_alpha.value : 255.0f;
    g_state.alpha_animation.update(alpha);
    g_state.gap_animation.update(get_gap(ctx->jni, ctx->local_player));
    const float animated_gap = g_state.gap_animation.value;
    const int opacity = animated_opacity();

    const Argb edge = g_state.outline.value ? adjust::argb(19, 19, 19, opacity) : adjust::argb(0, 0, 0, 0);
    const Argb foreground = adjust::with_alpha(g_state.color.get(), opacity);

    const float x = frame.width / 2.0f;
    const float y = frame.height / 2.0f;
    draw_crosshair(x, y, animated_gap, edge, foreground);

    adjust::end_hud();
}

// CrosshairModule.onTick (CancellablePlayerUpdateEvent)
void on_post_living_update(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (ctx == nullptr || ctx->local_player == nullptr) {
        return;
    }
    if (is_stationary(ctx->jni, ctx->local_player)) {
        ++g_state.inactive_ticks;
    } else {
        g_state.inactive_ticks = 0;
    }
}

// CrosshairModule.onEnable
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_state.base || !ev->enabled) {
        return;
    }
    events::JniContext* jni = ev->ctx != nullptr ? ev->ctx->jni : nullptr;
    jobject player = ev->ctx != nullptr ? ev->ctx->local_player : nullptr;
    g_state.gap_animation.value = get_gap(jni, player);
    g_state.inactive_ticks = 0;
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.mode.render(gui, renderer);
    g_state.color.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Inactivity Fade");
    g_state.fade_enabled.render(gui, renderer);
    g_state.fade_alpha.render(gui, renderer);
    g_state.fade_requirement.render(gui, renderer);

    if (g_state.mode.value == kModeGapped) {
        gui::widgets::render_section_header(gui, renderer, "Gap");
        g_state.gap_moving.render(gui, renderer);
        g_state.gap_normal.render(gui, renderer);
    }

    gui::widgets::render_section_header(gui, renderer, "Shape");
    if (g_state.mode.value != kModeDot) {
        g_state.width.render(gui, renderer);
    }
    g_state.thickness.render(gui, renderer);
    if (g_state.mode.value == kModeGapped) {
        g_state.dot.render(gui, renderer);
    }
    g_state.outline.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.mode.visit(archive, "mode");
    g_state.color.visit(archive, "color", "color_mode");
    g_state.fade_enabled.visit(archive, "inactivity_fade_enabled");
    g_state.fade_alpha.visit(archive, "inactivity_fade_alpha");
    g_state.fade_requirement.visit(archive, "inactivity_fade_requirement");
    g_state.gap_moving.visit(archive, "gap_moving");
    g_state.gap_normal.visit(archive, "gap_normal");
    g_state.width.visit(archive, "width");
    g_state.thickness.visit(archive, "thickness");
    g_state.dot.visit(archive, "dot");
    g_state.outline.visit(archive, "outline");
}

} // namespace

events::Module* get_adjust_crosshair_module() noexcept {
    return &g_state.base;
}

bool adjust_crosshair_replaces_vanilla() noexcept {
    return events::module_is_running(&g_state.base);
}

} // namespace slinky::modules
