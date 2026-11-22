// Port of com.adjustclient.module.impl.render.ThirdPersonModule and ThirdPersonViewClipCondition.
//
// Settings (names, defaults and ranges as in Adjust):
//   Free Look  (ExpandSetting)  Bind (KeybindSetting "Free Look", unbound), Hold (On), Invert yaw (Off),
//                               Invert pitch (Off)
//   View Clip  (ExpandSetting, condition = ThirdPersonViewClipCondition)  Enabled (Yes), Sneak only (Off)
//   Distance   0 m, -2 .. 5, step 0.25 (NumberSetting.METERS)
//
// Behaviour:
//   * setFreeLooking(true) snapshots the rendered rotation (RenderRotationTracker -> OpenSlinky++ server spoof
//     rotation when a silent rotation is active, else the player rotation), reloads the entity shader for the view
//     entity and marks the entity list dirty; setFreeLooking(false) unloads the shader.
//   * Bind: on press (in game, no screen) -> Hold ? start : toggle; on release with Hold -> stop. Every tick:
//     no player / any screen open -> stop; Hold -> follow the bind state; while free looking the entity list is
//     marked dirty each tick (RenderGlobal.displayListEntitiesDirty).
//   * onMouseInput consumes the mouse delta into yaw/pitch (pitch clamped to +-90, optional inversion).
//   * isViewClipEnabled() = enabled && View Clip Enabled && (!Sneak only || sneak key down).
//
// Adjust patches EntityRenderer / ActiveRenderInfo / RenderManager / RenderGlobal / GuiIngame so that, while free
// looking, Minecraft behaves as in third person even when gameSettings.thirdPersonView is 0 (camera behind the
// player, player model rendered, no hand, no crosshair, no pumpkin overlay, ActiveRenderInfo not mirrored). The
// port gets the same result by switching thirdPersonView from 0 to 1 while free looking (restored afterwards unless
// the user changed the perspective meanwhile); only the camera rotation / distance / view clip need hooks (see
// camera_hooks.cpp / camera_hooks.hpp).
//
// The keybind is polled from the client key table (GameContext::key_states, OpenSlinky++ key codes) every frame
// and every tick, like the other ported Adjust key handlers (Adjust's BindManager has no OpenSlinky++ equivalent).

#include "modules/adjust/render/third_person/third_person.hpp"

#include <algorithm>

#include "adjust/jni_lookup.hpp"
#include "cJSON.h"
#include "config/profile_manager.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/click_gui.hpp"
#include "gui/gui_internal.hpp"
#include "gui/gui_renderer.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/tabgui/setting_capture.hpp"
#include "util/keycodes.hpp"

namespace slinky::modules {

namespace {

namespace ajni = adjust::jni;

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;
void on_module_state_changed(void* raw) noexcept;
void on_game_tick(void* raw) noexcept;
void on_frame_update(void* raw) noexcept;
void on_client_shutdown(void* raw) noexcept;

struct ThirdPersonState {
    events::Module base{};

    // Free Look
    int32_t free_look_bind = 0; // KeybindSetting "Bind" (OpenSlinky++ key code, 0 = none)
    int32_t free_look_bind_default = 0;
    config::BoolSetting free_look_hold{{}, "Hold", true, true, {}};
    config::BoolSetting free_look_invert_yaw{{}, "Invert yaw", false, false, {}};
    config::BoolSetting free_look_invert_pitch{{}, "Invert pitch", false, false, {}};
    // View Clip
    config::BoolSetting view_clip_enabled{{}, "Enabled", true, true, {}};
    config::BoolSetting view_clip_sneak_only{{}, "Sneak only", false, false, {}};
    // Distance (NumberSetting.METERS: "#.##" + "m")
    config::FloatSetting distance{{}, "Distance", "%.2fm", 0.25f, 0.0f, -2.0f, 5.0f, 0.0f, 0.0f};

    // Runtime state
    float yaw = 0.0f;
    float pitch = 0.0f;
    float previous_yaw = 0.0f;
    float previous_pitch = 0.0f;
    bool free_looking = false;
    bool forced_third_person = false; // thirdPersonView switched 0 -> 1 by setFreeLooking(true)
    bool bind_was_down = false;

    ThirdPersonState() noexcept {
        base.name = "Third Person";
        base.description = "Free look, view clip and third person distance.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust; // setHidden(false)
        base.events[events::kEventModuleStateChanged] = {&on_module_state_changed, 0};
        base.events[events::kEventGameTick] = {&on_game_tick, 250}; // PreClientTickEvent priority 250
        base.events[events::kEventFrameUpdate] = {&on_frame_update, 0};
        base.events[events::kEventClientShutdown] = {&on_client_shutdown, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Third Person");
    }
};

ThirdPersonState g_state{};

// ---------------------------------------------------------------------------------------------------------------
// JNI members not covered by the central mapping table (Minecraft 1.8.9)
// ---------------------------------------------------------------------------------------------------------------

struct Ids {
    jmethodID load_entity_shader = nullptr; // EntityRenderer.loadEntityShader(Entity)
    jfieldID invert_mouse = nullptr;        // GameSettings.invertMouse
};

const Ids& ids(events::JniContext* jni) noexcept {
    static const Ids s_ids = [&] {
        Ids out{};
        const jclass renderer = ajni::find_class(jni, "bfk", "net/minecraft/client/renderer/EntityRenderer");
        const jclass settings = ajni::find_class(jni, "avh", "net/minecraft/client/settings/GameSettings");
        out.load_entity_shader = ajni::method(jni, renderer, {"a", "loadEntityShader", "func_175066_a"}, "(Lpk;)V",
                                              "(Lnet/minecraft/entity/Entity;)V");
        out.invert_mouse = ajni::field(jni, settings, {"b", "invertMouse", "field_74338_d"}, "Z", "Z");
        return out;
    }();
    return s_ids;
}

bool running() noexcept {
    return events::module_is_running(&g_state.base);
}

// RenderRotationTracker.INSTANCE.rotation (null -> player rotation)
math::Vec2f rendered_rotation(events::JniContext* jni, jobject player) noexcept {
    if (minecraft::is_rotation_spoof_active()) {
        return minecraft::get_server_spoof_rotation();
    }
    return minecraft::entity_get_rotation(jni, player);
}

void load_entity_shader(events::JniContext* jni, jobject mc, jobject entity) noexcept {
    const Ids& id = ids(jni);
    if (id.load_entity_shader == nullptr) {
        return;
    }
    jobject renderer = minecraft::mc_get_entity_renderer(jni, mc);
    if (renderer == nullptr) {
        return;
    }
    jni->env->CallVoidMethod(renderer, id.load_entity_shader, entity);
    ajni::clear_exception(jni);
    jni->env->DeleteLocalRef(renderer);
}

void mark_entities_dirty(events::JniContext* jni, jobject mc) noexcept {
    jobject render_global = minecraft::mc_get_render_global(jni, mc);
    if (render_global != nullptr) {
        minecraft::render_global_set_entities_dirty(jni, render_global, true);
        jni->env->DeleteLocalRef(render_global);
    }
}

// ThirdPersonModule.setFreeLooking
void set_free_looking(events::JniContext* jni, bool active) noexcept {
    if (g_state.free_looking == active) {
        return;
    }
    if (active && (jni == nullptr || jni->env == nullptr || !ajni::is_1_8(jni))) {
        return; // Adjust targets 1.8.9 only (the free look hooks do not exist on 1.7.10)
    }
    g_state.free_looking = active;
    if (jni == nullptr || jni->env == nullptr) {
        g_state.forced_third_person = false;
        return;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        ajni::clear_exception(jni);
        return;
    }
    jobject mc = minecraft::mc_get_instance(jni);
    jobject player = mc != nullptr ? minecraft::mc_get_player(jni, mc) : nullptr;
    jobject settings = mc != nullptr ? minecraft::mc_get_game_settings(jni, mc) : nullptr;
    if (active) {
        if (player == nullptr) {
            g_state.free_looking = false;
            env->PopLocalFrame(nullptr);
            return;
        }
        const math::Vec2f rot = rendered_rotation(jni, player);
        g_state.previous_yaw = g_state.yaw = rot.x;
        g_state.previous_pitch = g_state.pitch = rot.y;
        jobject camera = minecraft::mc_get_render_view_entity(jni, mc);
        load_entity_shader(jni, mc, camera);
        // OpenSlinky++: Adjust's "free looking counts as third person" patches -> thirdPersonView 0 becomes 1.
        if (settings != nullptr && minecraft::settings_get_third_person_view(jni, settings) == 0) {
            minecraft::settings_set_third_person_view(jni, settings, 1);
            g_state.forced_third_person = true;
        }
    } else if (mc != nullptr) {
        load_entity_shader(jni, mc, nullptr);
        if (g_state.forced_third_person && settings != nullptr &&
            minecraft::settings_get_third_person_view(jni, settings) == 1) {
            minecraft::settings_set_third_person_view(jni, settings, 0);
        }
        g_state.forced_third_person = false;
    } else {
        g_state.forced_third_person = false;
    }
    if (mc != nullptr) {
        mark_entities_dirty(jni, mc);
    }
    ajni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

bool bind_down(const events::GameContext* ctx) noexcept {
    const int32_t key = g_state.free_look_bind;
    return key > 0 && key < input::KEY_COUNT && ctx->key_states != nullptr && (ctx->key_states[key] & 1u) != 0;
}

// Bind callback (BindMode.PRESSED / RELEASED), edge-detected from the key table.
void poll_bind(events::GameContext* ctx) noexcept {
    const bool down = bind_down(ctx);
    const bool pressed = down && !g_state.bind_was_down;
    const bool released = !down && g_state.bind_was_down;
    g_state.bind_was_down = down;
    if (released) {
        if (g_state.free_look_hold.value) {
            set_free_looking(ctx->jni, false);
        }
    } else if (pressed && ctx->local_player != nullptr && ctx->in_game_focus) {
        set_free_looking(ctx->jni, g_state.free_look_hold.value || !g_state.free_looking);
    }
}

void on_module_state_changed(void* raw) noexcept {
    const auto* ev = static_cast<const events::ModuleStateChangedEvent*>(raw);
    if (ev->module != &g_state.base) {
        return;
    }
    if (!ev->enabled) {
        set_free_looking(ev->ctx != nullptr ? ev->ctx->jni : nullptr, false); // onDisable
    }
    g_state.bind_was_down = false;
}

// onGameLoop(PreClientTickEvent)
void on_game_tick(void* raw) noexcept {
    const auto* ev = static_cast<const events::ContextEvent*>(raw);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->jni == nullptr) {
        return;
    }
    poll_bind(ctx);
    if (ctx->local_player == nullptr || !ctx->in_game_focus) {
        set_free_looking(ctx->jni, false);
    } else if (g_state.free_look_hold.value) {
        set_free_looking(ctx->jni, bind_down(ctx));
    }
    if (g_state.free_looking && ctx->minecraft != nullptr) {
        mark_entities_dirty(ctx->jni, ctx->minecraft);
    }
}

void on_frame_update(void* raw) noexcept {
    const auto* ev = static_cast<const events::FrameUpdateEvent*>(raw);
    if (ev->ctx != nullptr && ev->ctx->jni != nullptr) {
        poll_bind(ev->ctx);
    }
}

void on_client_shutdown(void* raw) noexcept {
    const auto* ev = static_cast<const events::ContextEvent*>(raw);
    set_free_looking(ev->ctx != nullptr ? ev->ctx->jni : nullptr, false);
}

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

void set_free_look_bind(void* target, uint32_t key_code) noexcept {
    auto* bind = static_cast<int32_t*>(target);
    const int32_t key = (key_code < static_cast<uint32_t>(input::KEY_COUNT)) ? static_cast<int32_t>(key_code) : 0;
    if (*bind != key) {
        *bind = key;
        config::mark_profile_dirty(true);
    }
}

// KeybindSetting row: label on the left, OpenSlinky++ keybind pill on the right.
void render_bind_row(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    if (gui == nullptr || renderer == nullptr || adjust_hud::tabgui::capture_active()) {
        return; // the Tab GUI capture has no keybind item kind
    }
    const math::Vec4f row = gui->next_cell(20.0f);
    NVGcontext* vg = renderer->vg;
    nvgFillColor(vg, gui::detail::color_text_primary());
    nvgFontSize(vg, 15.0f);
    renderer->draw_text(row, gui::kTextAlignLeftMiddle, gui::kFontRegular, "Bind");
    const math::Vec4f pill_bounds = math::make_rect(row.x + row.w * 0.5f, row.y, row.w * 0.5f, row.h);
    gui::widgets::render_keybind_pill(gui, renderer, &set_free_look_bind, &g_state.free_look_bind,
                                      static_cast<uint32_t>(g_state.free_look_bind), nullptr, pill_bounds);
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Free Look");
    render_bind_row(gui, renderer);
    g_state.free_look_hold.render(gui, renderer);
    g_state.free_look_invert_yaw.render(gui, renderer);
    g_state.free_look_invert_pitch.render(gui, renderer);
    gui::widgets::render_section_header(gui, renderer, "View Clip");
    g_state.view_clip_enabled.render(gui, renderer);
    if (g_state.view_clip_enabled.value) { // ThirdPersonViewClipCondition
        g_state.view_clip_sneak_only.render(gui, renderer);
    }
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.distance.render(gui, renderer);
}

void visit_bind(config::ConfigArchive* ar, const char* key) noexcept {
    if (ar->failed) {
        return;
    }
    switch (ar->action) {
        case config::ConfigAction::Load: {
            int32_t value = g_state.free_look_bind_default;
            const cJSON* item = cJSON_GetObjectItemCaseSensitive(ar->json, key);
            if (cJSON_IsNumber(item)) {
                const double parsed = cJSON_GetNumberValue(item);
                if (parsed >= 0.0 && parsed < static_cast<double>(input::KEY_COUNT)) {
                    value = static_cast<int32_t>(parsed);
                }
            }
            g_state.free_look_bind = value;
            break;
        }
        case config::ConfigAction::Save:
            if (cJSON_AddNumberToObject(ar->json, key, static_cast<double>(g_state.free_look_bind)) == nullptr) {
                ar->failed = true;
            }
            break;
        case config::ConfigAction::CaptureDefaults:
            g_state.free_look_bind_default = g_state.free_look_bind;
            break;
        case config::ConfigAction::ResetDefaults:
            g_state.free_look_bind = g_state.free_look_bind_default;
            break;
    }
}

void visit_config(config::ConfigArchive* archive) noexcept {
    visit_bind(archive, "free_look_bind");
    g_state.free_look_hold.visit(archive, "free_look_hold");
    g_state.free_look_invert_yaw.visit(archive, "free_look_invert_yaw");
    g_state.free_look_invert_pitch.visit(archive, "free_look_invert_pitch");
    g_state.view_clip_enabled.visit(archive, "view_clip_enabled");
    g_state.view_clip_sneak_only.visit(archive, "view_clip_sneak_only");
    g_state.distance.visit(archive, "distance");
}

} // namespace

events::Module* get_adjust_third_person_module() noexcept {
    return &g_state.base;
}

bool adjust_third_person_free_looking() noexcept {
    return g_state.free_looking && running();
}

math::Vec2f adjust_third_person_free_look_rotation() noexcept {
    return math::make_vec2(g_state.yaw, g_state.pitch);
}

float adjust_third_person_extra_distance() noexcept {
    return running() ? g_state.distance.value : 0.0f;
}

// ThirdPersonModule.isViewClipEnabled
bool adjust_third_person_view_clip(events::JniContext* jni) noexcept {
    if (!running() || !g_state.view_clip_enabled.value) {
        return false;
    }
    if (!g_state.view_clip_sneak_only.value) {
        return true;
    }
    if (jni == nullptr || jni->env == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(8) != 0) {
        ajni::clear_exception(jni);
        return false;
    }
    bool sneak = false;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject settings = mc != nullptr ? minecraft::mc_get_game_settings(jni, mc) : nullptr;
    jobject key = settings != nullptr ? minecraft::settings_get_key_sneak(jni, settings) : nullptr;
    if (key != nullptr) {
        sneak = minecraft::key_binding_is_pressed(jni, key); // keyBindSneak.isKeyDown()
    }
    env->PopLocalFrame(nullptr);
    return sneak;
}

// ThirdPersonModule.onMouseInput(MouseDeltaEvent)
bool adjust_third_person_consume_mouse(events::JniContext* jni, float yaw_arg, float pitch_arg) noexcept {
    if (!adjust_third_person_free_looking() || jni == nullptr || jni->env == nullptr) {
        return false;
    }
    // Entity.setAngles receives (sensitivity * deltaX, sensitivity * deltaY * (invertMouse ? -1 : 1)); Adjust's
    // MouseDeltaEvent carries the deltas before vanilla's invertMouse, so undo it.
    bool invert_mouse = false;
    const Ids& id = ids(jni);
    if (id.invert_mouse != nullptr) {
        jobject mc = minecraft::mc_get_instance(jni);
        jobject settings = mc != nullptr ? minecraft::mc_get_game_settings(jni, mc) : nullptr;
        if (settings != nullptr) {
            invert_mouse = jni->env->GetBooleanField(settings, id.invert_mouse) != JNI_FALSE;
            jni->env->DeleteLocalRef(settings);
        }
        if (mc != nullptr) {
            jni->env->DeleteLocalRef(mc);
        }
    }
    float dx = yaw_arg;
    float dy = invert_mouse ? -pitch_arg : pitch_arg;
    if (g_state.free_look_invert_yaw.value) {
        dx = -dx;
    }
    if (!g_state.free_look_invert_pitch.value) {
        dy = -dy;
    }
    const float old_yaw = g_state.yaw;
    const float old_pitch = g_state.pitch;
    g_state.yaw += static_cast<float>(static_cast<double>(dx) * 0.15);
    g_state.pitch = std::clamp(g_state.pitch + static_cast<float>(static_cast<double>(dy) * 0.15), -90.0f, 90.0f);
    g_state.previous_pitch += g_state.pitch - old_pitch;
    g_state.previous_yaw += g_state.yaw - old_yaw;
    return true;
}

} // namespace slinky::modules
