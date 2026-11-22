#include "modules/world/bridge_assist.hpp"

#include <cmath>
#include <cstdint>

#include "config/settings.hpp"
#include "events/events.hpp"
#include "gui/gui_renderer.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/world/bridge.hpp"
#include "modules/world/bridge_common.hpp"
#include "modules/world/fast_place.hpp"
#include "util/math.hpp"
#include "util/random.hpp"
#include "util/time.hpp"

namespace slinky::modules {

namespace {

constexpr const char* kSelectBlocksOptions[3] = {
    "No",
    "On depletion",
    "Always",
};

// Mode selector: the original Slinky edge-sneak plus the Vape Legit and GodBridge scaffold techniques.
enum BridgeMode : int32_t {
    kModeLegitSlinky = 0, // original Bridge Assist behaviour
    kModeLegitVape = 1,   // Vape LegitScaffoldMode / edge-sneak
    kModeGodBridge = 2,   // Vape BlatantScaffoldMode (diagonal)
    kModeCount = 3,
};
constexpr const char* kModeOptions[kModeCount] = {"Legit (Slinky)", "Legit (Vape)", "GodBridge"};

// EnumFacing ordinal for the bottom face (mouseover side check).
constexpr int32_t kFaceDown = 0;

void on_module_state_changed(void* event) noexcept;
void on_pre_attack_or_use(void* event) noexcept;
void on_mid_living_update(void* event) noexcept;
void on_post_living_update(void* event) noexcept;
void on_game_tick(void* event) noexcept;
void on_rotation_spoof_query(void* event) noexcept;
void on_pre_move_with_heading(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct BridgeAssistState {
    events::Module base{};

    // Mode.
    config::ModeSetting mode{{}, "Mode", kModeCount, 0, kModeOptions, 0, 0, false, {}};

    // Settings (Legit (Slinky), @ 0x180306CD0 .. 0x180306E77)
    config::BoolSetting require_sneak{{}, "Sneak key pressed", false, false, {}};
    config::BoolSetting require_looking_down{{}, "Looking down", false, false, {}};
    config::BoolSetting require_not_forward{{}, "Not moving forward", false, false, {}};
    config::ModeSetting select_blocks{{}, "Select blocks", 3, 0, kSelectBlocksOptions, 1, 1, false, {}};
    config::BoolSetting require_holding_blocks{{}, "Holding blocks", false, false, {}};
    config::BoolSetting sneak_on_jump{{}, "Sneak on jump", false, false, {}};
    config::FloatSetting edge_offset{{}, "Edge offset", "%.2f blocks", 0.01f, 0.0f, 0.0f, 0.2999f, 0.2f, 0.2f};
    config::BoolSetting randomize{{}, "Randomize", true, true, {}};
    config::BoolSetting avoid_double_sneak{{}, "Avoid double-sneaking", false, false, {}};
    config::FloatSetting unsneak_delay{{}, "Unsneak delay", "%.0f ms", 0.1f, 50.0f, 1.0f, 6.0f, 3.0f, 3.0f};

    // Settings (Vape modes).
    config::FloatSetting aim_speed{{}, "Aim speed", "%.0f \xC2\xB0/s", 1.0f, 0.0f, 20.0f, 180.0f, 120.0f, 120.0f};
    config::BoolSetting pitch_check{{}, "Pitch check", false, false, {}};
    config::FloatSetting pitch_threshold{{}, "Pitch", "%.0f \xC2\xB0", 1.0f, 0.0f, 0.0f, 90.0f, 45.0f, 45.0f};
    config::BoolSetting legit_require_sneak{{}, "Require sneak", false, false, {}};
    config::FloatSetting legit_sneak_delay{{}, "Sneak delay", "%.0f ms", 10.0f, 0.0f, 100.0f, 500.0f, 200.0f, 200.0f};
    config::FloatSetting activation_blocks{{}, "Activation blocks", "%.0f", 1.0f, 0.0f, 1.0f, 8.0f, 2.0f, 2.0f};

    // Runtime state (Legit (Slinky), @ 0x180314FB8 .. 0x180315000)
    bool sneak_overridden = false;
    int32_t recent_place_ticks = 0;
    bool avoid_double_sneak_active = false;
    bool should_select_blocks = false;
    float unsneak_timer = 0.0f;
    int32_t holding_block_grace_ticks = 0;
    random::DriftNoise edge_offset_drift{};
    random::DriftNoise unsneak_delay_drift{};
    random::DriftNoise unsneak_spike_gate{};

    // Runtime state (Vape modes).
    float legit_sneak_timer = 0.0f; // ms countdown
    bool rotation_active = false;
    math::Vec2f target_rot{};
    bool steer_active = false;
    double steer_dx = 0.0;          // locked GodBridge axis (world direction)
    double steer_dz = 0.0;
    float locked_yaw = 0.0f;        // yaw of the locked axis
    bool dir_locked = false;
    bool place_armed = false;
    int32_t place_level = 0;        // y of the cell placements must fill this tick
    float click_cooldown = 0.0f;
    int32_t activated_blocks = 0;   // manual placements counted before taking over
    bool taken_over = false;
    int32_t idle_ticks = 0;
    int32_t ticks_since_manual_place = 0;
    bool bridged_since_takeover = false;
    int32_t last_held_slot = -1;
    int32_t last_held_size = 0;
    random::DriftNoise aim_jitter{};
    random::DriftNoise yaw_speed_drift{};

    BridgeAssistState() noexcept {
        base.name = "Bridge Assist";
        base.description = "Bridging assist: Legit edge-sneak (Slinky/Vape) and GodBridge.";
        base.category = events::ModuleCategory::Block;
        base.enabled = false;
        base.flags = events::kModuleFlagNone;
        base.keybind = 0;
        base.hold_mode = false;
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.events[events::kEventPreAttackOrUseEntity] = {on_pre_attack_or_use, 0, 0};
        base.events[events::kEventMidLivingUpdate] = {on_mid_living_update, 0, 0};
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, -900, 0};
        base.events[events::kEventGameTick] = {on_game_tick, 0, 0};
        base.events[events::kEventRotationSpoofQuery] = {on_rotation_spoof_query, -100, 0};
        base.events[events::kEventPreMoveWithHeading] = {on_pre_move_with_heading, -100, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        base.format_hud_tag = nullptr;
    }
};

BridgeAssistState g_bridge_assist{};

// ============================================================================================================
// Legit (Slinky) -- original Bridge Assist implementation.
// ============================================================================================================

// Predicts next-tick foot position and returns true if all overlapping support blocks are air (sub_18004FE40)
bool is_over_air_edge(
    events::JniContext* jni,
    jobject settings,
    jobject world,
    jobject player) noexcept
{
    jobject bbox_obj = mc::entity_get_bounding_box(jni, player);
    const math::AABB bbox = mc::aabb_to_aabb(jni, bbox_obj);
    const math::Vec3d feet_center = math::make_vec3(
        (bbox.min.x + bbox.max.x) * 0.5,
        bbox.min.y - 0.01,
        (bbox.min.z + bbox.max.z) * 0.5);

    const math::Vec3d motion = mc::entity_get_motion(jni, player);
    double offset = static_cast<double>(g_bridge_assist.edge_offset.value);
    if (g_bridge_assist.randomize.value) {
        const float drift = g_bridge_assist.edge_offset_drift.sample_timed({0.0f, 0.15f}, 5.0);
        offset -= static_cast<double>(drift);
        if (offset < 0.0) {
            offset = 0.0;
        }
    }

    jobject fwd_key = mc::settings_get_key_forward(jni, settings);
    jobject back_key = mc::settings_get_key_back(jni, settings);
    jobject left_key = mc::settings_get_key_left(jni, settings);
    jobject right_key = mc::settings_get_key_right(jni, settings);

    float move_forward = mc::key_binding_is_pressed(jni, fwd_key) ? 1.0f : 0.0f;
    if (mc::key_binding_is_pressed(jni, back_key)) {
        move_forward -= 1.0f;
    }
    float move_strafe = mc::key_binding_is_pressed(jni, left_key) ? 1.0f : 0.0f;
    if (mc::key_binding_is_pressed(jni, right_key)) {
        move_strafe -= 1.0f;
    }

    const math::Vec2f rot = mc::entity_get_rotation(jni, player);
    const float land_factor = mc::living_get_land_movement_factor(jni, player);
    const math::Vec3d strafe_delta = mc::compute_strafe_motion(rot, move_forward, move_strafe, land_factor);
    const math::Vec3d next_vel = math::vec3_add(motion, strafe_delta);
    const math::Vec3d pred_pos = math::vec3_add(feet_center, next_vel);

    const int32_t min_x = static_cast<int32_t>(std::floor(pred_pos.x - offset));
    const int32_t min_z = static_cast<int32_t>(std::floor(pred_pos.z - offset));
    const int32_t block_y = static_cast<int32_t>(std::floor(pred_pos.y));
    const int32_t max_x = static_cast<int32_t>(std::floor(pred_pos.x + offset));
    const int32_t max_z = static_cast<int32_t>(std::floor(pred_pos.z + offset));

    if (mc::block_get_id(jni, mc::world_get_block(jni, world, min_x, block_y, min_z)) != 0) {
        return false;
    }
    if (min_z != max_z &&
        mc::block_get_id(jni, mc::world_get_block(jni, world, min_x, block_y, max_z)) != 0) {
        return false;
    }
    if (min_x == max_x) {
        return true;
    }
    if (mc::block_get_id(jni, mc::world_get_block(jni, world, max_x, block_y, min_z)) != 0) {
        return false;
    }
    if (min_z == max_z) {
        return true;
    }
    return mc::block_get_id(jni, mc::world_get_block(jni, world, max_x, block_y, max_z)) == 0;
}

// Suppresses diagonal side-extension misplacements and tracks recent block placements (sub_18004F530)
void slinky_pre_attack_or_use(events::AttackOrUseEntityEvent* ev) noexcept {
    if (ev->is_attack || g_bridge_assist.recent_place_ticks == 20) {
        return;
    }

    events::GameContext* ctx = ev->ctx;
    events::JniContext* jni = ctx->jni;

    const auto record_held_block_place = [&]() noexcept {
        jobject held = mc::player_get_held_item(jni, ctx->local_player);
        if (mc::is_block_item(jni, held)) {
            g_bridge_assist.recent_place_ticks = 20;
        }
    };

    if (!g_bridge_assist.avoid_double_sneak_active) {
        record_held_block_place();
        return;
    }

    jobject mop = mc::mc_get_object_mouse_over(jni, ctx->minecraft);
    if (mop == nullptr ||
        mc::mop_get_type_ordinal(jni, mop) != 1 ||
        static_cast<uint32_t>(mc::mop_get_side_hit(jni, mop)) < 2u ||
        !mc::entity_is_on_ground(jni, ctx->local_player)) {
        record_held_block_place();
        return;
    }

    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    jobject back_key = mc::settings_get_key_back(jni, settings);
    jobject left_key = mc::settings_get_key_left(jni, settings);
    jobject right_key = mc::settings_get_key_right(jni, settings);
    if (!mc::key_binding_is_pressed(jni, back_key) ||
        mc::key_binding_is_pressed(jni, left_key) ||
        mc::key_binding_is_pressed(jni, right_key)) {
        record_held_block_place();
        return;
    }

    const math::Vec2f rot = mc::entity_get_rotation(jni, ctx->local_player);
    const float yaw_mod_90 = std::fabs(static_cast<float>(std::fmod(static_cast<double>(rot.x), 90.0)));
    if (yaw_mod_90 < 30.0f || yaw_mod_90 > 60.0f) {
        record_held_block_place();
        return;
    }

    math::Vec3d player_pos = mc::entity_get_position(jni, ctx->local_player);
    if (mc::is_version_1_7_10(jni)) {
        player_pos = math::vec3_sub_eye_height(player_pos);
    }

    const math::Vec3d hit_vec = mc::vec3_to_vec3d(jni, mc::mop_get_hit_vec(jni, mop));
    const double rel_y = hit_vec.y - player_pos.y;
    if (std::fabs(hit_vec.x - player_pos.x) > 1.0 ||
        rel_y < -1.0 ||
        rel_y > 0.0 ||
        std::fabs(hit_vec.z - player_pos.z) > 1.0) {
        record_held_block_place();
        return;
    }

    const math::Vec3d offset_pos = mc::mop_get_offset_block_pos_vec3d(jni, mop);
    const bool z_inside = (offset_pos.z < player_pos.z) && (player_pos.z < offset_pos.z + 1.0);
    const bool x_inside = (offset_pos.x < player_pos.x) && (player_pos.x < offset_pos.x + 1.0);
    const double margin = (x_inside && z_inside) ? 0.25 : 0.15;
    const double upper = 1.0 - margin;

    const double frac_x = std::fabs(std::fmod(hit_vec.x, 1.0));
    const double frac_z = std::fabs(std::fmod(hit_vec.z, 1.0));
    if ((frac_x >= margin && frac_x <= upper) || (frac_z >= margin && frac_z <= upper)) {
        ev->cancelled = true;
        return;
    }

    record_held_block_place();
}

// Updates automatic sneaking/unsneaking during EntityPlayerSP living update (sub_18004F8E0)
void slinky_mid_living_update(events::GameContext* ctx) noexcept {
    if (!ctx->in_game_focus) {
        return;
    }

    events::JniContext* jni = ctx->jni;
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    if (g_bridge_assist.recent_place_ticks > 0) {
        --g_bridge_assist.recent_place_ticks;
    }

    jobject sneak_key = mc::settings_get_key_sneak(jni, settings);
    const bool phys_sneak = mc::key_binding_is_physically_down(jni, sneak_key);

    const auto release_sneak_override = [&]() noexcept {
        if (g_bridge_assist.sneak_overridden) {
            mc::key_binding_set_pressed(jni, sneak_key, phys_sneak);
        }
        g_bridge_assist.sneak_overridden = false;
    };

    if (g_bridge_assist.require_sneak.value != phys_sneak) {
        release_sneak_override();
        return;
    }

    jobject player = ctx->local_player;
    if (g_bridge_assist.require_looking_down.value) {
        if (mc::entity_get_rotation(jni, player).y < 65.0f) {
            release_sneak_override();
            return;
        }
    }

    if (g_bridge_assist.require_not_forward.value) {
        if (mc::living_get_move_forward(jni, player) > 0.0f) {
            release_sneak_override();
            return;
        }
    }

    g_bridge_assist.should_select_blocks = (g_bridge_assist.select_blocks.value != 0);

    if (g_bridge_assist.require_holding_blocks.value) {
        jobject held = mc::player_get_held_item(jni, player);
        if (mc::is_block_item(jni, held)) {
            g_bridge_assist.holding_block_grace_ticks = 2;
        } else if (g_bridge_assist.holding_block_grace_ticks > 0) {
            --g_bridge_assist.holding_block_grace_ticks;
        } else {
            release_sneak_override();
            return;
        }
    }

    g_bridge_assist.sneak_overridden = true;
    const bool currently_sneaking = mc::key_binding_is_pressed(jni, sneak_key);
    jobject world = ctx->world;

    if (currently_sneaking) {
        if (g_bridge_assist.require_sneak.value) {
            if (mc::living_get_move_forward(jni, player) >= 0.0f || g_bridge_assist.recent_place_ticks <= 0) {
                g_bridge_assist.avoid_double_sneak_active = g_bridge_assist.avoid_double_sneak.value;
                return;
            }
            if (is_over_air_edge(jni, settings, world, player)) {
                g_bridge_assist.avoid_double_sneak_active = g_bridge_assist.avoid_double_sneak.value;
                return;
            }
        } else {
            const bool can_keep_sneak =
                mc::entity_is_on_ground(jni, player) ||
                (g_bridge_assist.sneak_on_jump.value && mc::entity_get_motion(jni, player).y >= 0.0);
            if (can_keep_sneak && is_over_air_edge(jni, settings, world, player)) {
                g_bridge_assist.avoid_double_sneak_active = g_bridge_assist.avoid_double_sneak.value;
                return;
            }
        }

        g_bridge_assist.unsneak_timer -= 1.0f;
        if (g_bridge_assist.unsneak_timer > 0.0f) {
            g_bridge_assist.avoid_double_sneak_active = g_bridge_assist.avoid_double_sneak.value;
            return;
        }

        mc::key_binding_set_pressed(jni, sneak_key, false);
        float next_delay = g_bridge_assist.unsneak_delay.value;
        if (g_bridge_assist.randomize.value) {
            next_delay += g_bridge_assist.unsneak_delay_drift.sample_timed({-0.5f, 0.5f}, 30.0);
            if (g_bridge_assist.unsneak_spike_gate.sample_chance_timed({0.04f, 0.08f}, 60.0)) {
                next_delay += random::uniform_range({0.0f, 3.0f});
            }
        }
        g_bridge_assist.unsneak_timer += next_delay;
        return;
    }

    const bool can_engage_sneak =
        mc::entity_is_on_ground(jni, player) ||
        (g_bridge_assist.sneak_on_jump.value && mc::entity_get_motion(jni, player).y >= 0.0);
    if (can_engage_sneak && is_over_air_edge(jni, settings, world, player)) {
        mc::key_binding_set_pressed(jni, sneak_key, true);
        g_bridge_assist.avoid_double_sneak_active = g_bridge_assist.avoid_double_sneak.value;
    }
}

// ============================================================================================================
// Vape modes -- shared helpers.
// ============================================================================================================

bool is_air(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) noexcept {
    return mc::block_get_id(jni, mc::world_get_block(jni, world, x, y, z)) == 0;
}

math::Vec3d feet_pos(events::JniContext* jni, jobject player) noexcept {
    math::Vec3d feet = mc::entity_get_position(jni, player);
    if (mc::is_version_1_7_10(jni)) {
        feet = math::vec3_sub_eye_height(feet);
    }
    return feet;
}

// Y level where blocks are placed under the player (handles the half-block edge case, Vape getPlacementY).
int32_t placement_y(const math::Vec3d& feet) noexcept {
    const double y = feet.y;
    const double frac = y - std::floor(y);
    if (std::fabs(frac - 0.5) < 1e-6) {
        return static_cast<int32_t>(std::floor(y));
    }
    return static_cast<int32_t>(std::floor(y - 1.0));
}

// Pitch of your actual view (camera), used for the pitch check.
float view_pitch(events::JniContext* jni, jobject player) noexcept {
    return mc::is_rotation_spoof_active() ? mc::get_server_spoof_rotation().y
                                          : mc::entity_get_rotation(jni, player).y;
}

// World yaw you are trying to walk towards (physical WASD in the movement frame). False when idle.
bool travel_yaw(events::JniContext* jni, jobject settings, jobject player, float* out_yaw) noexcept {
    const auto down = [&](jobject k) noexcept { return mc::key_binding_is_physically_down(jni, k); };
    const int32_t fwd = (down(mc::settings_get_key_forward(jni, settings)) ? 1 : 0) -
                        (down(mc::settings_get_key_back(jni, settings)) ? 1 : 0);
    const int32_t str = (down(mc::settings_get_key_left(jni, settings)) ? 1 : 0) -
                        (down(mc::settings_get_key_right(jni, settings)) ? 1 : 0);
    if (fwd == 0 && str == 0) {
        return false;
    }
    const float intent = static_cast<float>(
        std::atan2(static_cast<double>(-str), static_cast<double>(fwd)) * 180.0 / math::kPi);
    *out_yaw = bridge_common::frame_yaw(jni, player) + intent;
    return true;
}

bool pitch_ok(events::JniContext* jni, jobject player) noexcept {
    if (!g_bridge_assist.pitch_check.value) {
        return true;
    }
    return view_pitch(jni, player) >= g_bridge_assist.pitch_threshold.value;
}

bool holding_block(events::JniContext* jni, jobject player) noexcept {
    return mc::is_block_item(jni, mc::player_get_held_item(jni, player));
}

// Current hotbar slot and its stack size (0 when empty).
void held_slot_and_size(events::JniContext* jni, jobject player, int32_t* out_slot, int32_t* out_size) noexcept {
    jobject inv = mc::player_get_inventory(jni, player);
    const int32_t slot = mc::inv_player_get_current_item(jni, inv);
    *out_slot = slot;
    *out_size = 0;
    if (slot < 0 || slot > 8) {
        return;
    }
    jobject stack = jni->env->GetObjectArrayElement(mc::inv_player_get_main_inventory(jni, inv), slot);
    if (stack != nullptr) {
        *out_size = mc::item_stack_get_size(jni, stack);
        jni->env->DeleteLocalRef(stack);
    }
}

void restore_keys(events::JniContext* jni, jobject settings) noexcept {
    if (g_bridge_assist.sneak_overridden) {
        jobject sneak_key = mc::settings_get_key_sneak(jni, settings);
        mc::key_binding_set_pressed(jni, sneak_key, mc::key_binding_is_physically_down(jni, sneak_key));
        g_bridge_assist.sneak_overridden = false;
    }
}

void reset_takeover() noexcept {
    g_bridge_assist.taken_over = false;
    g_bridge_assist.activated_blocks = 0;
    g_bridge_assist.dir_locked = false;
    g_bridge_assist.bridged_since_takeover = false;
}

// Silently slews the aim rotation toward target_rot at the configured aim speed (Bridge pattern).
void slew_rotation_to_target(events::JniContext* jni, jobject player) noexcept {
    if (!mc::try_claim_aim_priority(1000u)) {
        return;
    }
    const math::Vec2f cur = math::vec2_add(mc::entity_get_rotation(jni, player),
                                           mc::get_pending_aim_rotation_delta());
    const float yaw_diff = math::wrap_degrees_180(g_bridge_assist.target_rot.x - cur.x);
    const float pitch_diff = g_bridge_assist.target_rot.y - cur.y;
    const float rand = g_bridge_assist.randomize.value
                           ? g_bridge_assist.yaw_speed_drift.sample_timed({0.85f, 1.15f}, 0.2)
                           : 1.0f;
    const float step = g_bridge_assist.aim_speed.value * rand;
    mc::queue_aim_rotation_delta({math::clampf(-step, step, yaw_diff), math::clampf(-step, step, pitch_diff)});
    g_bridge_assist.rotation_active = true;
}

// Aims at the visible placement face for `level` when there is one, otherwise at the given pose.
void aim_pose_or_face(events::GameContext* ctx, math::Vec2f pose, int32_t level) noexcept {
    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    const math::Vec3d eye = mc::vec3_to_vec3d(jni, mc::living_get_position_eyes(jni, player));
    const math::Vec2f cur = math::vec2_add(mc::entity_get_rotation(jni, player),
                                           mc::get_pending_aim_rotation_delta());
    math::Vec3d aim{};
    g_bridge_assist.target_rot = bridge_find_place_aim(ctx, eye, cur, level, g_bridge_assist.randomize.value, &aim)
                                     ? math::calc_yaw_pitch(eye, aim)
                                     : pose;
    slew_rotation_to_target(jni, player);
    g_bridge_assist.place_armed = true;
    g_bridge_assist.place_level = level;
}

// Vape LegitScaffoldMode / ScaffoldEdgeSneakHelper.
void vape_legit_mid(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);

    jobject sneak_key = mc::settings_get_key_sneak(jni, settings);
    const bool phys_sneak = mc::key_binding_is_physically_down(jni, sneak_key);
    if (g_bridge_assist.legit_require_sneak.value && !phys_sneak) {
        restore_keys(jni, settings);
        return;
    }

    const auto down = [&](jobject k) noexcept { return mc::key_binding_is_physically_down(jni, k); };
    const int32_t fwd = (down(mc::settings_get_key_forward(jni, settings)) ? 1 : 0) -
                        (down(mc::settings_get_key_back(jni, settings)) ? 1 : 0);
    const bool on_ground = mc::entity_is_on_ground(jni, player);

    bool should_sneak = false;
    if (fwd <= 0 && on_ground && bridge_common::over_air_edge_vape(jni, ctx->world, player)) {
        should_sneak = true;
    }

    g_bridge_assist.sneak_overridden = true;
    if (should_sneak) {
        mc::key_binding_set_pressed(jni, sneak_key, true);
        float delay = g_bridge_assist.legit_sneak_delay.value;
        if (g_bridge_assist.randomize.value) {
            delay += random::uniform_range({-40.0f, 40.0f});
        }
        g_bridge_assist.legit_sneak_timer = delay;
    } else if (g_bridge_assist.legit_sneak_timer > 0.0f) {
        g_bridge_assist.legit_sneak_timer -= 50.0f; // ~one tick
        mc::key_binding_set_pressed(jni, sneak_key, true);
    } else {
        mc::key_binding_set_pressed(jni, sneak_key, phys_sneak);
    }
}

// Counts your own block placements (held stack shrinks while you hold right click) until the mode takes over.
void track_manual_placements(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    int32_t slot = -1;
    int32_t size = 0;
    held_slot_and_size(jni, ctx->local_player, &slot, &size);

    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    const bool use_down = mc::key_binding_is_physically_down(jni, mc::settings_get_key_use_item(jni, settings));
    if (!g_bridge_assist.taken_over && g_bridge_assist.activated_blocks > 0 &&
        ++g_bridge_assist.ticks_since_manual_place > 60) {
        g_bridge_assist.activated_blocks = 0;
    }
    if (!g_bridge_assist.taken_over && use_down && slot == g_bridge_assist.last_held_slot &&
        size < g_bridge_assist.last_held_size && holding_block(jni, ctx->local_player)) {
        g_bridge_assist.ticks_since_manual_place = 0;
        ++g_bridge_assist.activated_blocks;
        if (g_bridge_assist.activated_blocks >= static_cast<int32_t>(g_bridge_assist.activation_blocks.value)) {
            g_bridge_assist.taken_over = true;
        }
    }
    g_bridge_assist.last_held_slot = slot;
    g_bridge_assist.last_held_size = size;
}

// GodBridge (Vape BlatantScaffold): after the activation blocks it locks your bridge axis, faces back
// diagonally (45 deg) looking down, keeps you walking straight along the axis and places at the edge.
void update_god_bridge(events::GameContext* ctx, float travel) noexcept {
    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;

    if (!g_bridge_assist.dir_locked) {
        g_bridge_assist.locked_yaw = bridge_common::snap_cardinal(travel, &g_bridge_assist.steer_dx, &g_bridge_assist.steer_dz);
        g_bridge_assist.dir_locked = true;
    }

    const float yaw_jit = g_bridge_assist.randomize.value ? g_bridge_assist.aim_jitter.sample_timed({-2.0f, 2.0f}, 0.4) : 0.0f;
    const float pitch_jit = g_bridge_assist.randomize.value ? g_bridge_assist.aim_jitter.sample_timed({-1.5f, 1.5f}, 0.5) : 0.0f;
    const math::Vec2f pose = math::make_vec2(g_bridge_assist.locked_yaw + 180.0f + 45.0f + yaw_jit, 80.5f + pitch_jit);
    aim_pose_or_face(ctx, pose, placement_y(feet_pos(jni, player)));
    g_bridge_assist.steer_active = true;
}

// ============================================================================================================
// Event handlers (dispatch on mode).
// ============================================================================================================

void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_bridge_assist.base || ev->enabled) {
        return;
    }
    events::GameContext* ctx = ev->ctx;
    jobject settings = mc::mc_get_game_settings(ctx->jni, ctx->minecraft);
    restore_keys(ctx->jni, settings);
    reset_takeover();
    g_bridge_assist.steer_active = false;
    g_bridge_assist.rotation_active = false;
    g_bridge_assist.place_armed = false;
}

void on_pre_attack_or_use(void* event) noexcept {
    if (g_bridge_assist.mode.value != kModeLegitSlinky) {
        return;
    }
    slinky_pre_attack_or_use(static_cast<events::AttackOrUseEntityEvent*>(event));
}

void on_mid_living_update(void* event) noexcept {
    g_bridge_assist.avoid_double_sneak_active = false;
    g_bridge_assist.should_select_blocks = false;

    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;

    if (g_bridge_assist.mode.value == kModeLegitSlinky) {
        slinky_mid_living_update(ctx);
        return;
    }
    if (g_bridge_assist.mode.value != kModeLegitVape) {
        return; // GodBridge works in post-update
    }

    events::JniContext* jni = ctx->jni;
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    if (!ctx->in_game_focus || !pitch_ok(jni, ctx->local_player)) {
        restore_keys(jni, settings);
        return;
    }
    vape_legit_mid(ctx);
}

void on_post_living_update(void* event) noexcept {
    const int32_t mode = g_bridge_assist.mode.value;
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;

    if (mode == kModeLegitSlinky) {
        if (g_bridge_assist.should_select_blocks) {
            mc::select_hotbar_block_slot(ctx, &g_bridge_assist.base, g_bridge_assist.select_blocks.value == 2);
        }
        return;
    }

    g_bridge_assist.rotation_active = false;
    g_bridge_assist.steer_active = false;
    g_bridge_assist.place_armed = false;

    if (mode == kModeLegitVape) {
        return;
    }

    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    if (!ctx->in_game_focus || !pitch_ok(jni, player)) {
        reset_takeover();
        return;
    }
    if (!holding_block(jni, player)) {
        mc::select_hotbar_block_slot(ctx, &g_bridge_assist.base, true);
        if (!holding_block(jni, player)) {
            reset_takeover();
            return;
        }
    }

    if (mode == kModeGodBridge) {
        float travel = 0.0f;
        if (!travel_yaw(jni, settings, player, &travel)) {
            ++g_bridge_assist.idle_ticks;
            const int32_t idle_limit = g_bridge_assist.bridged_since_takeover ? 10 : 60;
            if (g_bridge_assist.taken_over && g_bridge_assist.idle_ticks > idle_limit) {
                reset_takeover(); // stopped bridging: place the activation blocks again to restart
            }
            return;
        }
        g_bridge_assist.idle_ticks = 0;
        if (g_bridge_assist.taken_over) {
            g_bridge_assist.bridged_since_takeover = true;
            update_god_bridge(ctx, travel);
        }
    }
}

// Placement: counts manual activation blocks, then right-clicks (Fast Place CPS when on, else every 4
// ticks) whenever the crosshair would put a block into the empty cell at the armed level.
void on_game_tick(void* event) noexcept {
    const int32_t mode = g_bridge_assist.mode.value;
    if (mode == kModeLegitSlinky || mode == kModeLegitVape) {
        return; // Legit does not auto-place; the player clicks.
    }

    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (!ctx->in_game_focus || ctx->local_player == nullptr) {
        return;
    }
    track_manual_placements(ctx);

    if (g_bridge_assist.click_cooldown > 0.0f) {
        g_bridge_assist.click_cooldown -= 1.0f;
        if (g_bridge_assist.click_cooldown > 0.0f) {
            return;
        }
    }
    if (!g_bridge_assist.place_armed) {
        g_bridge_assist.click_cooldown = 0.0f;
        return;
    }
    const float interval = get_fast_place_module()->enabled ? 20.0f / fast_place_get_max_cps() : 4.0f;

    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    if (!holding_block(jni, player)) {
        return;
    }
    jobject mop = mc::mc_get_object_mouse_over(jni, ctx->minecraft);
    if (mop == nullptr || mc::mop_get_type_ordinal(jni, mop) != 1 || mc::mop_get_side_hit(jni, mop) == kFaceDown) {
        return;
    }
    const math::Vec3d place = mc::mop_get_offset_block_pos_vec3d(jni, mop);
    const int32_t px = static_cast<int32_t>(std::floor(place.x));
    const int32_t py = static_cast<int32_t>(std::floor(place.y));
    const int32_t pz = static_cast<int32_t>(std::floor(place.z));
    if (py != g_bridge_assist.place_level || !is_air(jni, ctx->world, px, py, pz)) {
        return;
    }

    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    mc::key_binding_increment_press_time(jni, mc::settings_get_key_use_item(jni, settings));
    g_bridge_assist.click_cooldown += interval;
}

void on_rotation_spoof_query(void* event) noexcept {
    if (g_bridge_assist.rotation_active) {
        auto* ev = static_cast<events::RotationSpoofEvent*>(event);
        ev->active = true;
        ev->move_fix = true;
    }
}

// GodBridge steering: keeps your own input strength (so no walking by itself, sneak speed kept) but points
// it exactly along the locked bridge axis, converted with the aim yaw moveFlying actually uses.
void on_pre_move_with_heading(void* event) noexcept {
    if (!g_bridge_assist.steer_active) {
        return;
    }
    bridge_common::steer_move_input(static_cast<events::MoveWithHeadingEvent*>(event),
                                    g_bridge_assist.steer_dx, g_bridge_assist.steer_dz);
}

// ============================================================================================================
// ClickGUI + config.
// ============================================================================================================

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_bridge_assist.mode.render(gui, renderer);
    g_bridge_assist.randomize.render(gui, renderer);

    if (g_bridge_assist.mode.value == kModeLegitSlinky) {
        g_bridge_assist.edge_offset.render(gui, renderer);
        g_bridge_assist.unsneak_delay.render(gui, renderer);
        g_bridge_assist.select_blocks.render(gui, renderer);
        g_bridge_assist.sneak_on_jump.render(gui, renderer);
        g_bridge_assist.avoid_double_sneak.render(gui, renderer);

        gui::widgets::render_section_header(gui, renderer, "Conditions");
        g_bridge_assist.require_sneak.render(gui, renderer);
        g_bridge_assist.require_holding_blocks.render(gui, renderer);
        g_bridge_assist.require_looking_down.render(gui, renderer);
        g_bridge_assist.require_not_forward.render(gui, renderer);
        return;
    }

    // Vape modes.
    g_bridge_assist.aim_speed.render(gui, renderer);
    g_bridge_assist.pitch_check.render(gui, renderer);
    if (g_bridge_assist.pitch_check.value) {
        g_bridge_assist.pitch_threshold.render(gui, renderer);
    }

    if (g_bridge_assist.mode.value == kModeLegitVape) {
        gui::widgets::render_section_header(gui, renderer, "Legit (Vape)");
        g_bridge_assist.legit_require_sneak.render(gui, renderer);
        g_bridge_assist.legit_sneak_delay.render(gui, renderer);
    } else {
        gui::widgets::render_section_header(gui, renderer, "GodBridge");
        g_bridge_assist.activation_blocks.render(gui, renderer);
    }
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_bridge_assist.mode.visit(archive, "mode");
    if (g_bridge_assist.mode.value < 0 || g_bridge_assist.mode.value >= kModeCount) {
        g_bridge_assist.mode.value = kModeLegitSlinky; // old configs may still hold a removed mode
    }
    g_bridge_assist.randomize.visit(archive, "randomize");
    // Legit (Slinky).
    g_bridge_assist.edge_offset.visit(archive, "edge_offset");
    g_bridge_assist.unsneak_delay.visit(archive, "unsneak_delay");
    g_bridge_assist.select_blocks.visit(archive, "select_blocks");
    g_bridge_assist.sneak_on_jump.visit(archive, "sneak_on_jump");
    g_bridge_assist.avoid_double_sneak.visit(archive, "avoid_double_sneak");
    g_bridge_assist.require_sneak.visit(archive, "require_sneak");
    g_bridge_assist.require_holding_blocks.visit(archive, "require_holding_blocks");
    g_bridge_assist.require_looking_down.visit(archive, "require_looking_down");
    g_bridge_assist.require_not_forward.visit(archive, "require_not_forward");
    // Vape modes.
    g_bridge_assist.aim_speed.visit(archive, "aim_speed");
    g_bridge_assist.pitch_check.visit(archive, "pitch_check");
    g_bridge_assist.pitch_threshold.visit(archive, "pitch");
    g_bridge_assist.legit_require_sneak.visit(archive, "legit_require_sneak");
    g_bridge_assist.legit_sneak_delay.visit(archive, "legit_sneak_delay");
    g_bridge_assist.activation_blocks.visit(archive, "activation_blocks");
}

} // namespace

bool bridge_assist_wants_ledge_clip() noexcept {
    return g_bridge_assist.base.enabled && g_bridge_assist.mode.value == kModeGodBridge &&
           g_bridge_assist.taken_over;
}

events::Module* get_bridge_assist_module() noexcept {
    return &g_bridge_assist.base;
}

} // namespace slinky::modules
