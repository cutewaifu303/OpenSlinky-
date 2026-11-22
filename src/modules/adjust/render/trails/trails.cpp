// Port of com.adjustclient.module.impl.render.TrailsModule: a fading line along your last N tick positions.
//
// "Blink only" follows OpenSlinky++'s Blink module (Adjust: BlinkModule). The trail is cleared on
// S01PacketJoinGame / S07PacketRespawn and on S08PacketPlayerPosLook teleports further than 3 blocks; those
// packets are inspected on the Netty thread with a few field reads only, the clear itself happens on the main
// thread.

#include "modules/adjust/render/trails/trails.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/utility/blink.hpp"
#include "util/math.hpp"

namespace slinky::modules {

namespace {

using adjust::Argb;

constexpr double kYOffset = 0.00625;

void on_post_living_update(void* event) noexcept;
void on_render_world_3d(void* event) noexcept;
void on_incoming_packet(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct TrailsState {
    events::Module base{};

    config::BoolSetting blink_only{{}, "Blink only", false, false, {}};
    config::FloatSetting moves{{}, "Moves", "%.0f", 1.0f, 0.0f, 3.0f, 60.0f, 10.0f, 10.0f};
    config::FloatSetting width{{}, "Width", "%.1f", 0.5f, 0.0f, 1.0f, 5.0f, 2.0f, 2.0f};
    adjust::ColorOption color{"Color", "Color mode", 0xDC9090FFu, adjust::ColorMode::Plain};

    std::deque<math::Vec3d> trail; // BoundedList<Vec3>(moves)
    int bound = 10;
    std::atomic<bool> clear_requested{false};

    TrailsState() noexcept {
        base.name = "Trails";
        base.description = "Draws a trail behind you.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, 0, 0};
        base.events[events::kEventRenderWorld3D] = {on_render_world_3d, 0, 0};
        base.events[events::kEventIncomingPacket] = {on_incoming_packet, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Trails");
    }
};

TrailsState g_state{};

int moves_value() noexcept {
    return std::clamp(static_cast<int>(g_state.moves.value), 3, 60);
}

// BoundedList.add: drops the oldest entry when full.
void add_point(const math::Vec3d& point) noexcept {
    while (static_cast<int>(g_state.trail.size()) >= g_state.bound && !g_state.trail.empty()) {
        g_state.trail.pop_front();
    }
    g_state.trail.push_back(point);
}

// TrailsModule.onTick (PlayerUpdateTailEvent)
void on_post_living_update(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    // moves.addListener: a new BoundedList whenever the setting changes.
    if (moves_value() != g_state.bound) {
        g_state.bound = moves_value();
        g_state.trail.clear();
    }
    if (g_state.clear_requested.exchange(false)) {
        g_state.trail.clear();
    }
    if (ctx == nullptr || ctx->jni == nullptr || ctx->local_player == nullptr) {
        return;
    }
    if (g_state.blink_only.value && !events::module_is_running(get_blink_module())) {
        g_state.trail.clear();
        return;
    }
    const math::Vec3d last_tick = minecraft::entity_get_prev_position(ctx->jni, ctx->local_player);
    add_point(math::make_vec3(last_tick.x, last_tick.y + kYOffset, last_tick.z));
}

void gl_color(Argb argb) noexcept {
    glColor4f(static_cast<float>(adjust::red_of(argb)) / 255.0f, static_cast<float>(adjust::green_of(argb)) / 255.0f,
              static_cast<float>(adjust::blue_of(argb)) / 255.0f, static_cast<float>(adjust::alpha_of(argb)) / 255.0f);
}

// TrailsModule.onRender + RenderUtil.drawTrail
void on_render_world_3d(void* event) noexcept {
    if (g_state.trail.empty()) {
        return;
    }
    auto* ev = static_cast<events::RenderWorld3DEvent*>(event);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->local_player == nullptr) {
        return;
    }

    std::vector<math::Vec3d> points(g_state.trail.begin(), g_state.trail.end());
    const math::Vec3d last = points.back();
    const double pt = static_cast<double>(ev->partial_ticks);
    const math::Vec3d pos = minecraft::entity_get_position(ctx->jni, ctx->local_player);
    const math::Vec3d last_tick = minecraft::entity_get_prev_position(ctx->jni, ctx->local_player);
    points.push_back(math::make_vec3(last_tick.x + (pos.x - last_tick.x) * pt,
                                     last.y + (pos.y + kYOffset - last.y) * pt,
                                     last_tick.z + (pos.z - last_tick.z) * pt));

    const Argb color = g_state.color.get();
    const int base_alpha = adjust::alpha_of(color);

    // GlStateManager.disableDepth (already off in the world pass)
    glPushMatrix();
    glEnable(GL_BLEND);
    glEnable(GL_LINE_SMOOTH);
    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(g_state.width.value);
    glBegin(GL_LINE_STRIP);
    float fraction = 0.0f;
    const float step = 1.0f / static_cast<float>(points.size());
    for (const math::Vec3d& point : points) {
        const int alpha = std::clamp(static_cast<int>(static_cast<float>(base_alpha) * fraction), 0, 255);
        gl_color(adjust::with_alpha(color, alpha));
        fraction += step;
        glVertex3d(point.x - ev->render_pos.x, point.y - ev->render_pos.y, point.z - ev->render_pos.z);
    }
    glEnd();
    glPopMatrix();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// TrailsModule.onPacket (Netty thread: field reads only).
void on_incoming_packet(void* event) noexcept {
    auto* ev = static_cast<events::NettyPacketEvent*>(event);
    events::JniContext* jni = ev->jni;
    if (jni == nullptr || jni->env == nullptr || ev->packet == nullptr || ev->elapsed_ms != 0 ||
        !adjust::jni::is_1_8(jni)) {
        return; // elapsed_ms != 0: re-dispatch of a packet held by another module, already inspected
    }
    JNIEnv* env = jni->env;
    static const jclass cls_pos_look =
        adjust::jni::find_class(jni, "fi", "net/minecraft/network/play/server/S08PacketPlayerPosLook");
    static const jclass cls_join_game =
        adjust::jni::find_class(jni, "gt", "net/minecraft/network/play/server/S01PacketJoinGame");
    static const jclass cls_respawn =
        adjust::jni::find_class(jni, "he", "net/minecraft/network/play/server/S07PacketRespawn");
    static const jfieldID fid_x = adjust::jni::field(jni, cls_pos_look, {"a", "x", "field_148940_a"}, "D", "D");
    static const jfieldID fid_y = adjust::jni::field(jni, cls_pos_look, {"b", "y", "field_148938_b"}, "D", "D");
    static const jfieldID fid_z = adjust::jni::field(jni, cls_pos_look, {"c", "z", "field_148939_c"}, "D", "D");

    if (cls_pos_look != nullptr && env->IsInstanceOf(ev->packet, cls_pos_look)) {
        if (fid_x == nullptr || fid_y == nullptr || fid_z == nullptr) {
            return;
        }
        const math::Vec3d target = math::make_vec3(env->GetDoubleField(ev->packet, fid_x),
                                                   env->GetDoubleField(ev->packet, fid_y),
                                                   env->GetDoubleField(ev->packet, fid_z));
        jobject mc = minecraft::mc_get_instance(jni);
        jobject player = mc != nullptr ? minecraft::mc_get_player(jni, mc) : nullptr;
        if (player == nullptr || math::vec3_dist_sq(minecraft::entity_get_position(jni, player), target) > 9.0) {
            g_state.clear_requested.store(true);
        }
        if (player != nullptr) {
            env->DeleteLocalRef(player);
        }
        if (mc != nullptr) {
            env->DeleteLocalRef(mc);
        }
    } else if ((cls_join_game != nullptr && env->IsInstanceOf(ev->packet, cls_join_game)) ||
               (cls_respawn != nullptr && env->IsInstanceOf(ev->packet, cls_respawn))) {
        g_state.clear_requested.store(true);
    }
}

// onEnable: trail = new BoundedList(moves); onDisable: trail = null
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_state.base) {
        return;
    }
    g_state.bound = moves_value();
    g_state.trail.clear();
    g_state.clear_requested.store(false);
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.blink_only.render(gui, renderer);
    g_state.moves.render(gui, renderer);
    g_state.width.render(gui, renderer);
    g_state.color.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.blink_only.visit(archive, "blink_only");
    g_state.moves.visit(archive, "moves");
    g_state.width.visit(archive, "width");
    g_state.color.visit(archive, "color", "color_mode");
}

} // namespace

events::Module* get_adjust_trails_module() noexcept {
    return &g_state.base;
}

} // namespace slinky::modules
