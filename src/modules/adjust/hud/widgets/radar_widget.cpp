// Port of com.adjustclient.module.impl.render.hud.widget.radar.RadarWidget, RadarGroup and RadarTarget.
//
// A square radar (panel + title) rotated with the camera yaw, with one dot per nearby living entity coloured by its
// group and the local player's arrow (Adjust's direction_arrow.png, embedded) in the centre.
//
// Entity groups: Adjust classifies the entities of its EntityTracker with TrackedEntity.matchesType() using these
// masks, in this order; the port reproduces the same classification (including the group labels of the original):
//   0x800 "Enemy"    players tagged TARGET in Adjust's tag manager - OpenSlinky++ has no such tag, never matched
//   0x002 "Players"  other players that are not teammates
//   0x020 "Passive"  teammates (Adjust: WorldUtil.isTeammate or WHITELIST tag -> OpenSlinky++ teammate/friend tag)
//   0x004 "Friendly" EntityMob / EntityIronGolem (non-player living entities)
//   0x008 "Hostile"  every other non-player living entity
// Invisible entities never match (their masks lack the 0x40 "invisibles" bit) and the local player is excluded.

#include "modules/adjust/hud/widgets/widgets.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/widgets/direction_arrow_png.hpp"

namespace slinky::modules::adjust_hud::widgets {

namespace {

constexpr double kRadToDeg = 57.29577951308232;
constexpr double kDegToRad = 0.0174532925199433;

// MathHelper.wrapAngleTo180_float
float wrap_degrees(float value) noexcept {
    value = std::fmod(value, 360.0f);
    if (value >= 180.0f) {
        value -= 360.0f;
    }
    if (value < -180.0f) {
        value += 360.0f;
    }
    return value;
}

// RadarGroup: ExpandSetting with Enabled / Color / Alpha (70..255) / Size (1..3).
struct RadarGroup {
    const char* name;
    config::BoolSetting enabled;
    adjust::ColorOption color;
    config::FloatSetting alpha;
    config::FloatSetting size;

    RadarGroup(const char* group_name, bool enabled_default, adjust::Argb default_color, int default_alpha,
               int default_size) noexcept
        : name(group_name),
          enabled{{}, "Enabled", enabled_default, enabled_default, {}},
          color("Color", "Color mode", default_color),
          alpha{{}, "Alpha", "%.0f", 1.0f, 0.0f, 70.0f, 255.0f, static_cast<float>(default_alpha),
                static_cast<float>(default_alpha)},
          size{{}, "Size", "%.0f", 1.0f, 0.0f, 1.0f, 3.0f, static_cast<float>(default_size),
               static_cast<float>(default_size)} {}

    adjust::Argb argb() const noexcept { return color.get(); }
    int alpha_value() const noexcept { return static_cast<int>(alpha.value); }
    int size_value() const noexcept { return static_cast<int>(size.value); }
    bool is_enabled() const noexcept { return enabled.value; }
};

enum GroupIndex : int {
    kLocal = 0,
    kPlayers = 1,
    kEnemy = 2,
    kFriendly = 3,
    kHostile = 4,
    kPassive = 5,
    kGroupCount = 6,
};

struct RadarSettings {
    config::BoolSetting enabled{{}, "Enabled", false, false, {}};
    config::FloatSetting size{{}, "Size", "%.0f", 5.0f, 0.0f, 50.0f, 100.0f, 80.0f, 80.0f};
    config::FloatSetting distance{{}, "Distance", "%.0f", 5.0f, 0.0f, 10.0f, 120.0f, 30.0f, 30.0f};
    // Adjust formats it with NumberSetting.DEGREES ("Off" at 0).
    config::FloatSetting smoothing{{}, "Smoothing", "%.1f\xC2\xB0", 0.1f, 0.0f, 0.0f, 1.0f, 0.3f, 0.3f};
    RadarGroup groups[kGroupCount] = {
        RadarGroup("Local", true, adjust::argb(67, 90, 209), 220, 2),
        RadarGroup("Players", true, adjust::argb(232, 73, 73, 255), 200, 2),
        RadarGroup("Enemy", true, adjust::argb(183, 138, 40, 255), 220, 2),
        RadarGroup("Friendly", true, adjust::argb(91, 121, 178, 255), 170, 2),
        RadarGroup("Hostile", false, adjust::argb(122, 122, 122, 255), 130, 1),
        RadarGroup("Passive", false, adjust::argb(192, 187, 187, 255), 120, 1),
    };
};

RadarSettings g_settings{};

// RadarTarget: tracked entity (global ref) + its group.
struct RadarTarget {
    jobject entity = nullptr;
    int group = 0;
};

struct RadarState {
    std::vector<RadarTarget> targets;
    JNIEnv* env = nullptr; // env of the client thread (ticks + rendering), used to free the global refs
    float smoothed_yaw = 0.0f;
    double last_frame_time = 0.0;
    bool has_last_frame = false;
    NVGcontext* arrow_vg = nullptr;
    int arrow_image = 0;
};

RadarState g_state{};

void clear_targets(JNIEnv* env) noexcept {
    if (env != nullptr) {
        for (RadarTarget& t : g_state.targets) {
            if (t.entity != nullptr) {
                env->DeleteGlobalRef(t.entity);
            }
        }
    }
    g_state.targets.clear();
}

bool is_invisible(events::JniContext* jni, jobject entity) noexcept {
    static const jmethodID mid =
        adjust::jni::method(jni, jni->mappings->cls_entity, {"ax", "isInvisible", "func_82150_aj"}, "()Z", "()Z");
    if (mid == nullptr) {
        return false;
    }
    const bool invisible = jni->env->CallBooleanMethod(entity, mid) != JNI_FALSE;
    adjust::jni::clear_exception(jni);
    return invisible;
}

// Entity.lastTickPosX / lastTickPosZ (RadarWidget interpolates the local player from them).
bool last_tick_pos(events::JniContext* jni, jobject entity, double* out_x, double* out_z) noexcept {
    static const jfieldID fx =
        adjust::jni::field(jni, jni->mappings->cls_entity, {"P", "lastTickPosX", "field_70142_S"}, "D", "D");
    static const jfieldID fz =
        adjust::jni::field(jni, jni->mappings->cls_entity, {"R", "lastTickPosZ", "field_70136_U"}, "D", "D");
    if (fx == nullptr || fz == nullptr) {
        return false;
    }
    *out_x = jni->env->GetDoubleField(entity, fx);
    *out_z = jni->env->GetDoubleField(entity, fz);
    return true;
}

// TrackedEntity.matchesType() classification (see the file comment). Returns -1 when no group matches.
int classify(events::JniContext* jni, jobject entity) noexcept {
    static const jclass mob_cls = adjust::jni::find_class(jni, "vv", "net/minecraft/entity/monster/EntityMob");
    static const jclass golem_cls =
        adjust::jni::find_class(jni, "ty", "net/minecraft/entity/monster/EntityIronGolem");
    if (is_invisible(jni, entity)) {
        return -1;
    }
    JNIEnv* env = jni->env;
    if (minecraft::as_entity_player(jni, entity) != nullptr) {
        const uint16_t tags = minecraft::player_get_tag_flags(jni, entity);
        const bool teammate = (tags & (minecraft::kPlayerTagTeammate | minecraft::kPlayerTagFriend)) != 0;
        return teammate ? kPassive : kPlayers;
    }
    const bool mob = (mob_cls != nullptr && env->IsInstanceOf(entity, mob_cls) != JNI_FALSE) ||
                     (golem_cls != nullptr && env->IsInstanceOf(entity, golem_cls) != JNI_FALSE);
    return mob ? kFriendly : kHostile;
}

int arrow_image(NVGcontext* vg) noexcept {
    if (vg == nullptr) {
        return 0;
    }
    if (g_state.arrow_vg != vg) {
        g_state.arrow_vg = vg;
        g_state.arrow_image = nvgCreateImageMem(vg, 0, const_cast<unsigned char*>(kDirectionArrowPng),
                                                static_cast<int>(kDirectionArrowPngSize));
    }
    return g_state.arrow_image;
}

// RadarWidget.drawArrow: texture rotated by -angle / 90 (sic, Adjust does not convert to radians), tinted.
void draw_arrow(adjust::Argb color, float x, float y, float angle) noexcept {
    NVGcontext* vg = adjust::vg();
    const int image = arrow_image(vg);
    if (vg == nullptr || image == 0) {
        return;
    }
    const float arrow_size = static_cast<float>(g_settings.groups[kLocal].size_value()) + 3.0f;
    const float half = arrow_size * 0.5f;
    nvgSave(vg);
    nvgTranslate(vg, x, y);
    nvgRotate(vg, -angle / 90.0f);
    NVGpaint paint = nvgImagePattern(vg, -half, -half, arrow_size, arrow_size, 0.0f, image, 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, -half, -half, arrow_size, arrow_size);
    paint.innerColor = adjust::to_nvg(color);
    nvgFillPaint(vg, paint);
    nvgFill(vg);
    nvgRestore(vg);
}

void render_group_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer, RadarGroup& group) noexcept {
    char header[48]{};
    std::snprintf(header, sizeof(header), "Radar: %s", group.name);
    gui::widgets::render_section_header(gui, renderer, header);
    group.enabled.render(gui, renderer);
    if (group.enabled.value) {
        group.color.render(gui, renderer);
        group.alpha.render(gui, renderer);
        group.size.render(gui, renderer);
    }
}

} // namespace

void radar_render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Radar");
    g_settings.enabled.render(gui, renderer);
    if (!g_settings.enabled.value) {
        return;
    }
    g_settings.size.render(gui, renderer);
    g_settings.distance.render(gui, renderer);
    g_settings.smoothing.render(gui, renderer);
    for (RadarGroup& group : g_settings.groups) {
        render_group_settings(gui, renderer, group);
    }
}

void radar_visit_config(config::ConfigArchive* ar) noexcept {
    g_settings.enabled.visit(ar, "widgets_radar_enabled");
    g_settings.size.visit(ar, "widgets_radar_size");
    g_settings.distance.visit(ar, "widgets_radar_distance");
    g_settings.smoothing.visit(ar, "widgets_radar_smoothing");
    static constexpr const char* kKeys[kGroupCount][5] = {
        {"widgets_radar_local_enabled", "widgets_radar_local_color", "widgets_radar_local_color_mode",
         "widgets_radar_local_alpha", "widgets_radar_local_size"},
        {"widgets_radar_players_enabled", "widgets_radar_players_color", "widgets_radar_players_color_mode",
         "widgets_radar_players_alpha", "widgets_radar_players_size"},
        {"widgets_radar_enemy_enabled", "widgets_radar_enemy_color", "widgets_radar_enemy_color_mode",
         "widgets_radar_enemy_alpha", "widgets_radar_enemy_size"},
        {"widgets_radar_friendly_enabled", "widgets_radar_friendly_color", "widgets_radar_friendly_color_mode",
         "widgets_radar_friendly_alpha", "widgets_radar_friendly_size"},
        {"widgets_radar_hostile_enabled", "widgets_radar_hostile_color", "widgets_radar_hostile_color_mode",
         "widgets_radar_hostile_alpha", "widgets_radar_hostile_size"},
        {"widgets_radar_passive_enabled", "widgets_radar_passive_color", "widgets_radar_passive_color_mode",
         "widgets_radar_passive_alpha", "widgets_radar_passive_size"},
    };
    for (int i = 0; i < kGroupCount; ++i) {
        RadarGroup& group = g_settings.groups[i];
        group.enabled.visit(ar, kKeys[i][0]);
        group.color.visit(ar, kKeys[i][1], kKeys[i][2]);
        group.alpha.visit(ar, kKeys[i][3]);
        group.size.visit(ar, kKeys[i][4]);
    }
}

// RadarWidget.onTick: collect the entities within Distance (horizontal) of the player.
void radar_tick(events::GameContext* ctx) noexcept {
    if (ctx == nullptr || ctx->jni == nullptr || ctx->jni->env == nullptr) {
        return;
    }
    JNIEnv* env = ctx->jni->env;
    g_state.env = env;
    clear_targets(env);
    if (!g_settings.enabled.value || ctx->local_player == nullptr || ctx->world == nullptr ||
        !adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    if (env->PushLocalFrame(16) != 0) {
        env->ExceptionClear();
        return;
    }
    const math::Vec3d player_pos = minecraft::entity_get_position(jni, ctx->local_player);
    const double max_distance = static_cast<double>(static_cast<int>(g_settings.distance.value));
    jobject list = minecraft::world_get_loaded_entity_list(jni, ctx->world);
    const int32_t count = list != nullptr ? minecraft::java_list_size(jni, list) : 0;
    for (int32_t i = 0; i < count; ++i) {
        jobject entity = minecraft::java_list_get(jni, list, i);
        if (entity == nullptr) {
            adjust::jni::clear_exception(jni);
            continue;
        }
        if (minecraft::as_entity_living_base(jni, entity) != nullptr &&
            env->IsSameObject(entity, ctx->local_player) == JNI_FALSE) {
            const int group = classify(jni, entity);
            if (group >= 0 && g_settings.groups[group].is_enabled()) {
                const math::Vec3d pos = minecraft::entity_get_position(jni, entity);
                const double dx = player_pos.x - pos.x;
                const double dz = player_pos.z - pos.z;
                const double distance = std::sqrt(dx * dx + 0.0 + dz * dz);
                if (distance <= max_distance) {
                    jobject global = env->NewGlobalRef(entity);
                    if (global != nullptr) {
                        g_state.targets.push_back(RadarTarget{global, group});
                    }
                }
            }
        }
        env->DeleteLocalRef(entity);
    }
    env->PopLocalFrame(nullptr);
}

// RadarWidget.onRender2D: panel, optional cross hair, entity dots, local arrow.
void radar_render(const HudContext& ctx) noexcept {
    if (!g_settings.enabled.value || ctx.game == nullptr || ctx.game->jni == nullptr ||
        ctx.game->local_player == nullptr || !adjust::jni::is_1_8(ctx.game->jni)) {
        return;
    }
    events::GameContext* game = ctx.game;
    events::JniContext* jni = game->jni;
    JNIEnv* env = jni->env;

    const int size = static_cast<int>(g_settings.size.value);
    const WidgetBox box = draw_frame("Radar", kRadarX, kRadarY, static_cast<float>(size), static_cast<float>(size));
    const float left = box.x;
    const float top = box.y;
    const float content_top = top + 9.0f;
    const float content_height = box.height - 8.0f; // = size + 1
    const float center_y = content_height * 0.5f + content_top;
    const float bottom = content_top + content_height;
    const float right = left + box.width;
    const float center_x = box.width * 0.5f + left;

    const RadarGroup& local = g_settings.groups[kLocal];
    if (!local.is_enabled()) {
        // No local arrow: cross hair (two 0.5 wide lines) in (230, 230, 230, 120).
        adjust::push_even_scale();
        const adjust::Argb line_color = adjust::argb(230, 230, 230, 120);
        const math::Vec4f vertical = adjust::to_pixels(center_x, content_top, center_x + 0.5f, bottom);
        adjust::fill_rect(line_color, vertical.x, vertical.y, vertical.w, vertical.h - 0.5f);
        const math::Vec4f horizontal = adjust::to_pixels(left, center_y, right, center_y + 0.5f);
        adjust::fill_rect(line_color, 0.5f + horizontal.x, horizontal.y, horizontal.w - 0.5f, horizontal.h);
        adjust::pop_even_scale();
    }

    // Interpolated position of the local player.
    jobject timer = minecraft::mc_get_timer(jni, game->minecraft);
    const float partial_ticks = timer != nullptr ? minecraft::timer_get_render_partial_ticks(jni, timer) : 1.0f;
    if (timer != nullptr) {
        env->DeleteLocalRef(timer);
    }
    const math::Vec3d pos = minecraft::entity_get_position(jni, game->local_player);
    double last_x = pos.x;
    double last_z = pos.z;
    last_tick_pos(jni, game->local_player, &last_x, &last_z);
    const double player_x = (pos.x - last_x) * partial_ticks + last_x;
    const double player_z = (pos.z - last_z) * partial_ticks + last_z;
    const float half_size = static_cast<float>(size) * 0.5f;
    adjust::push_scissor(left + 0.5f, content_top, static_cast<float>(size) - 1.0f, content_height);

    // Frame time.
    const double now = static_cast<double>(adjust::now_ms()) / 1000.0;
    if (!g_state.has_last_frame) {
        g_state.last_frame_time = now;
        g_state.has_last_frame = true;
    }
    const double delta = now - g_state.last_frame_time;
    g_state.last_frame_time = now;

    // Yaw the radar is rotated by (ThirdPersonModule.getYaw: the camera entity's yaw), optionally smoothed.
    float target_yaw = 0.0f;
    jobject view = minecraft::mc_get_render_view_entity(jni, game->minecraft);
    if (view != nullptr) {
        target_yaw = minecraft::entity_get_rotation(jni, view).x;
        env->DeleteLocalRef(view);
    } else {
        target_yaw = minecraft::entity_get_rotation(jni, game->local_player).x;
    }
    const float smoothing = g_settings.smoothing.value;
    float shown_yaw = target_yaw;
    if (smoothing == 0.0f) {
        g_state.smoothed_yaw = target_yaw;
    } else {
        const float previous = g_state.smoothed_yaw;
        const float diff = wrap_degrees(target_yaw - previous);
        const float step = static_cast<float>(delta) * (3.0f / smoothing);
        float current = previous;
        // Keeps adding the same increment until the smoothed yaw is within 150 degrees of the target (Adjust).
        for (int guard = 0; guard < 100000; ++guard) {
            current = current + diff * step;
            g_state.smoothed_yaw = current;
            if (previous == current) {
                break;
            }
            if (std::fabs(wrap_degrees(current - target_yaw)) <= 150.0f) {
                break;
            }
        }
        shown_yaw = previous; // the value from before this frame's update is drawn
    }

    const int max_distance = static_cast<int>(g_settings.distance.value);
    for (const RadarTarget& target : g_state.targets) {
        if (target.entity == nullptr) {
            continue;
        }
        const math::Vec3d epos = minecraft::entity_get_position(jni, target.entity);
        const double dx = epos.x - player_x;
        const double dz = epos.z - player_z;
        const double distance = static_cast<float>(std::sqrt(dx * dx + 0.0 + dz * dz));
        const auto angle = static_cast<float>(std::atan2(dx, dz) * kRadToDeg);
        const float relative = wrap_degrees((angle - 180.0f) + shown_yaw);
        const double radians = static_cast<double>(relative) * kDegToRad;
        const double hypot = std::hypot(distance, distance);
        const double scaled = hypot / static_cast<double>(max_distance) * static_cast<double>(half_size);
        const float dot_x = static_cast<float>(std::sin(radians) * scaled) + center_x;
        const float dot_y = static_cast<float>(std::cos(radians) * scaled) + center_y;
        const RadarGroup& group = g_settings.groups[target.group];
        const int dot_size = group.size_value() - 1;
        const float dot_radius = (static_cast<float>(dot_size) / 2.0f) * 0.5f + 0.5f;
        if (left - dot_radius < dot_x && content_top - dot_radius < dot_y && dot_x <= right + dot_radius &&
            dot_y <= bottom + dot_radius) {
            const adjust::Argb color = (static_cast<adjust::Argb>(group.alpha_value() & 0xFF) << 24) |
                                       (group.argb() & 0xFFFFFFu);
            adjust::fill_circle(color, dot_x, dot_y, dot_radius);
        }
    }

    if (local.is_enabled()) {
        const float arrow_angle = wrap_degrees(shown_yaw - target_yaw);
        draw_arrow((static_cast<adjust::Argb>(local.alpha_value() & 0xFF) << 24) | (local.argb() & 0xFFFFFFu),
                   center_x, center_y, arrow_angle);
    }
    adjust::pop_scissor();
}

void radar_shutdown() noexcept {
    clear_targets(g_state.env);
    // The NanoVG context (and its images) is destroyed with the renderer.
    g_state.arrow_vg = nullptr;
    g_state.arrow_image = 0;
}

} // namespace slinky::modules::adjust_hud::widgets
