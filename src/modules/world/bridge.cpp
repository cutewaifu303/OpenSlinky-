#include "modules/world/bridge.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config/settings.hpp"
#include "events/events.hpp"
#include "gui/gui_renderer.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/world/fast_place.hpp"
#include "util/math.hpp"
#include "util/random.hpp"

namespace slinky::modules {

namespace {

// EnumFacing ordinals: 0=Down(-Y) 1=Up(+Y) 2=North(-Z) 3=South(+Z) 4=West(-X) 5=East(+X)
constexpr int32_t kFaceDown  = 0;
constexpr int32_t kFaceUp    = 1;
constexpr int32_t kFaceNorth = 2;
constexpr int32_t kFaceSouth = 3;
constexpr int32_t kFaceWest  = 4;
constexpr int32_t kFaceEast  = 5;

constexpr double kMaxPlaceReachSq = 20.1601; // 4.49^2

void on_game_tick(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void on_rotation_spoof_query(void* event) noexcept;
void on_mid_living_update(void* event) noexcept;
void on_post_living_update(void* event) noexcept;
void on_gui_frame_poll(void* event) noexcept;
void on_gui_render_2d(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct BridgeState {
    events::Module base{};

    // Settings
    config::FloatSetting edge_offset{{}, "Edge offset", "%.2f blocks", 0.01f, 0.0f, 0.0f, 0.2999f, 0.2f, 0.2f};
    config::BoolSetting show_block_count{{}, "Show block count", true, true, {}};
    config::BoolSetting tower{{}, "Tower (hold jump)", true, true, {}};
    config::BoolSetting randomize{{}, "Randomize", true, true, {}};
    config::FloatSetting pitch{{}, "Pitch", "%.1f \xC2\xB0", 0.1f, 0.0f, 75.0f, 90.0f, 82.5f, 82.5f};
    config::FloatSetting aim_speed{{}, "Aim speed", "%.0f \xC2\xB0/s", 1.0f, 20.0f, 20.0f, 180.0f, 90.0f, 90.0f};
    config::BoolSetting safe_edge{{}, "Never fall off edges", true, true, {}};
    config::BoolSetting edge_sneak{{}, "Auto sneak at edges", true, true, {}};
    config::BoolSetting sneak_on_jump{{}, "Sneak on jump", false, false, {}};
    config::FloatSetting unsneak_delay{{}, "Unsneak delay", "%.0f ms", 0.1f, 50.0f, 1.0f, 6.0f, 3.0f, 3.0f};

    // Runtime state
    float click_cooldown_ticks = 0.0f;
    bool targeting_active = false;
    bool spoof_rotation_active = false;
    bool sneak_overridden = false;
    float unsneak_timer = 0.0f;
    char hud_text[32]{};      // text shown next to the crosshair (empty = hidden)
    char hud_drawn[32]{};     // text currently baked into the offscreen GUI buffer
    random::DriftNoise edge_offset_drift{};
    random::DriftNoise aim_jitter_a{};
    random::DriftNoise aim_jitter_b{};
    random::DriftNoise yaw_speed_drift{};
    random::DriftNoise pitch_speed_drift{};
    random::DriftNoise unsneak_delay_drift{};

    BridgeState() noexcept {
        base.name = "Bridge";
        base.description = "Legit Auto Scaffold";
        base.category = events::ModuleCategory::Block;
        base.enabled = false;
        base.flags = events::kModuleFlagNone;
        base.keybind = 0;
        base.hold_mode = false;
        base.events[events::kEventGameTick] = {on_game_tick, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.events[events::kEventRotationSpoofQuery] = {on_rotation_spoof_query, -100, 0};
        base.events[events::kEventMidLivingUpdate] = {on_mid_living_update, 0, 0};
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, -900, 0};
        base.events[events::kEventGuiFramePoll] = {on_gui_frame_poll, 0, 0};
        base.events[events::kEventGuiRender2D] = {on_gui_render_2d, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        base.format_hud_tag = nullptr;
    }
};

BridgeState g_bridge{};

// Version-agnostic block queries (mc::world_get_block branches on 1.7 vs 1.8 internally).
bool is_air_block(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) noexcept {
    return mc::block_get_id(jni, mc::world_get_block(jni, world, x, y, z)) == 0;
}

bool is_solid_block(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) noexcept {
    return mc::block_get_id(jni, mc::world_get_block(jni, world, x, y, z)) != 0;
}

// Predicts next-tick foot cell and returns true when the supporting blocks beneath it are all air.
// Ported from Bridge Assist (sub_18004FE40) so Bridge can hold a sneak at open edges itself.
bool is_over_air_edge(
    events::JniContext* jni,
    jobject settings,
    jobject world,
    jobject player) noexcept
{
    const math::AABB bbox = mc::aabb_to_aabb(jni, mc::entity_get_bounding_box(jni, player));
    const math::Vec3d feet_center = math::make_vec3(
        (bbox.min.x + bbox.max.x) * 0.5,
        bbox.min.y - 0.01,
        (bbox.min.z + bbox.max.z) * 0.5);

    const math::Vec3d motion = mc::entity_get_motion(jni, player);
    double offset = static_cast<double>(g_bridge.edge_offset.value);
    if (g_bridge.randomize.value) {
        const float drift = g_bridge.edge_offset_drift.sample_timed({0.0f, 0.15f}, 5.0);
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
    const math::Vec3d pred_pos = math::vec3_add(feet_center, math::vec3_add(motion, strafe_delta));

    const int32_t min_x = static_cast<int32_t>(std::floor(pred_pos.x - offset));
    const int32_t min_z = static_cast<int32_t>(std::floor(pred_pos.z - offset));
    const int32_t block_y = static_cast<int32_t>(std::floor(pred_pos.y));
    const int32_t max_x = static_cast<int32_t>(std::floor(pred_pos.x + offset));
    const int32_t max_z = static_cast<int32_t>(std::floor(pred_pos.z + offset));

    if (is_solid_block(jni, world, min_x, block_y, min_z)) {
        return false;
    }
    if (min_z != max_z && is_solid_block(jni, world, min_x, block_y, max_z)) {
        return false;
    }
    if (min_x == max_x) {
        return true;
    }
    if (is_solid_block(jni, world, max_x, block_y, min_z)) {
        return false;
    }
    if (min_z == max_z) {
        return true;
    }
    return is_air_block(jni, world, max_x, block_y, max_z);
}

struct PlaceCandidate {
    mc::Vec3i support{};
    int32_t side = kFaceUp;
    math::Vec3d aim{};
    float angle = 0.0f;
};

// For an air cell, evaluates every solid neighbour (below + 4 horizontal) and keeps the face that is
// actually visible from the eye (verified by a block raytrace) with the smallest rotation change.
bool find_best_face_for_cell(
    events::JniContext* jni,
    jobject world,
    const math::Vec3d& eye,
    math::Vec2f cur_rot,
    const math::Vec3d& feet,
    const mc::Vec3i& cell,
    double jx,
    double jz,
    PlaceCandidate* best) noexcept
{
    struct Neighbour {
        int32_t dx, dy, dz; // offset from cell to support block
        int32_t side;       // face of the support block that touches the cell
    };
    constexpr Neighbour kNeighbours[5] = {
        { 0, -1,  0, kFaceUp},
        {-1,  0,  0, kFaceEast},
        { 1,  0,  0, kFaceWest},
        { 0,  0, -1, kFaceSouth},
        { 0,  0,  1, kFaceNorth},
    };

    const double cx = static_cast<double>(cell.x);
    const double cy = static_cast<double>(cell.y);
    const double cz = static_cast<double>(cell.z);
    const auto clamp_in = [](double lo, double v) noexcept {
        return static_cast<double>(math::clampf(static_cast<float>(lo + 0.2), static_cast<float>(lo + 0.8), static_cast<float>(v)));
    };

    bool found = false;
    for (const Neighbour& n : kNeighbours) {
        const mc::Vec3i sup{cell.x + n.dx, cell.y + n.dy, cell.z + n.dz};
        if (!is_solid_block(jni, world, sup.x, sup.y, sup.z)) {
            continue;
        }

        // Aim point on the shared face + visibility (eye must be on the cell side of the face plane).
        math::Vec3d aim{};
        bool visible = false;
        const double mid_y = clamp_in(cy, cy + 0.5 + jx);
        switch (n.side) {
            case kFaceUp:
                aim = math::make_vec3(clamp_in(cx, feet.x + jx), cy, clamp_in(cz, feet.z + jz));
                visible = eye.y > cy;
                break;
            case kFaceEast: // support at -X, face plane x = cell.x
                aim = math::make_vec3(cx, mid_y, clamp_in(cz, feet.z + jz));
                visible = eye.x > cx;
                break;
            case kFaceWest: // support at +X, face plane x = cell.x + 1
                aim = math::make_vec3(cx + 1.0, mid_y, clamp_in(cz, feet.z + jz));
                visible = eye.x < cx + 1.0;
                break;
            case kFaceSouth: // support at -Z, face plane z = cell.z
                aim = math::make_vec3(clamp_in(cx, feet.x + jx), mid_y, cz);
                visible = eye.z > cz;
                break;
            default: // kFaceNorth: support at +Z, face plane z = cell.z + 1
                aim = math::make_vec3(clamp_in(cx, feet.x + jx), mid_y, cz + 1.0);
                visible = eye.z < cz + 1.0;
                break;
        }
        if (!visible || math::vec3_dist_sq(eye, aim) >= kMaxPlaceReachSq) {
            continue;
        }

        // Raytrace slightly past the face: the first block hit must be the support on the right face.
        const math::Vec3d end = math::make_vec3(
            eye.x + (aim.x - eye.x) * 1.05,
            eye.y + (aim.y - eye.y) * 1.05,
            eye.z + (aim.z - eye.z) * 1.05);
        jobject ray = mc::world_ray_trace_blocks(
            jni, world, mc::vec3_new(jni, eye), mc::vec3_new(jni, end), false, true, false);
        if (ray == nullptr || mc::mop_get_type_ordinal(jni, ray) != 1) {
            continue;
        }
        const mc::Vec3i hit = mc::mop_get_block_pos_vec3i(jni, ray);
        if (hit.x != sup.x || hit.y != sup.y || hit.z != sup.z || mc::mop_get_side_hit(jni, ray) != n.side) {
            continue;
        }

        const math::Vec2f desired = math::calc_yaw_pitch(eye, aim);
        const math::Vec2f diff = math::vec2_sub(desired, cur_rot);
        const float angle = std::fabs(math::wrap_degrees_180(diff.x)) + std::fabs(diff.y);
        if (!found || angle < best->angle) {
            best->support = sup;
            best->side = n.side;
            best->aim = aim;
            best->angle = angle;
            found = true;
        }
    }
    return found;
}

// Picks the support block to click, the face to hit, and a humanized aim point on that face.
// Target = the air cell under the player (tower: under the feet while rising; bridge: under the
// current or next-tick centre). Returns false when nothing needs to be placed this tick.
bool compute_place_target(
    events::GameContext* ctx,
    const math::Vec3d& eye,
    math::Vec2f cur_rot,
    mc::Vec3i* out_support,
    int32_t* out_side,
    math::Vec3d* out_aim) noexcept
{
    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    jobject world = ctx->world;

    math::Vec3d feet = mc::entity_get_position(jni, player);
    if (mc::is_version_1_7_10(jni)) {
        feet = math::vec3_sub_eye_height(feet);
    }
    const int32_t support_y = static_cast<int32_t>(std::floor(feet.y)) - 1;

    const float rand_scale = g_bridge.randomize.value ? 1.0f : 0.0f;
    const double jx = static_cast<double>(g_bridge.aim_jitter_a.sample_timed({-0.12f, 0.12f}, 0.5) * rand_scale);
    const double jz = static_cast<double>(g_bridge.aim_jitter_b.sample_timed({-0.12f, 0.12f}, 0.5) * rand_scale);

    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    const bool jump_held = mc::key_binding_is_physically_down(jni, mc::settings_get_key_jump(jni, settings));

    // Candidate cells at foot-support level: under the centre, under the next-tick centre, then
    // under every corner of the hitbox. The corners matter when building up while moving: the
    // column above the block you jumped from is only under the back of your hitbox.
    const math::Vec3d motion = mc::entity_get_motion(jni, player);
    const math::AABB box = mc::aabb_to_aabb(jni, mc::entity_get_bounding_box(jni, player));
    const auto cell_at = [&](double x, double z) noexcept {
        return mc::Vec3i{static_cast<int32_t>(std::floor(x)), support_y, static_cast<int32_t>(std::floor(z))};
    };
    constexpr double kInset = 0.001;
    const mc::Vec3i raw_cells[6] = {
        cell_at(feet.x, feet.z),
        cell_at(feet.x + motion.x, feet.z + motion.z),
        cell_at(box.min.x + kInset, box.min.z + kInset),
        cell_at(box.max.x - kInset, box.min.z + kInset),
        cell_at(box.min.x + kInset, box.max.z - kInset),
        cell_at(box.max.x - kInset, box.max.z - kInset),
    };

    for (int32_t i = 0; i < 6; ++i) {
        const mc::Vec3i& cell = raw_cells[i];
        bool duplicate = false;
        for (int32_t j = 0; j < i; ++j) {
            duplicate = duplicate || (raw_cells[j].x == cell.x && raw_cells[j].z == cell.z);
        }
        if (duplicate || !is_air_block(jni, world, cell.x, cell.y, cell.z)) {
            continue;
        }
        PlaceCandidate best{};
        if (find_best_face_for_cell(jni, world, eye, cur_rot, feet, cell, jx, jz, &best)) {
            if (best.side == kFaceUp && jump_held && !g_bridge.tower.value) {
                continue;
            }
            *out_support = best.support;
            *out_side = best.side;
            *out_aim = best.aim;
            return true;
        }
    }
    return false;
}

math::Vec3d get_feet_pos(events::JniContext* jni, jobject player) noexcept {
    math::Vec3d feet = mc::entity_get_position(jni, player);
    if (mc::is_version_1_7_10(jni)) {
        feet = math::vec3_sub_eye_height(feet);
    }
    return feet;
}

// Right-clicks like a player spamming use while looking at the edge: fires (CPS-limited) whenever the
// crosshair is on a block face whose placement cell is the empty cell at foot-support level.
void on_game_tick(void* event) noexcept {
    float cd = g_bridge.click_cooldown_ticks;
    if (cd > 0.0f) {
        cd -= 1.0f;
        g_bridge.click_cooldown_ticks = cd;
        if (cd > 0.0f) {
            return;
        }
    }
    // Click rate: Fast Place's CPS when it is on, otherwise vanilla held-right-click (every 4 ticks).
    const float interval = get_fast_place_module()->enabled
        ? 20.0f / fast_place_get_max_cps()
        : 4.0f;
    g_bridge.click_cooldown_ticks = cd + interval;

    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (!ctx->in_game_focus || !g_bridge.targeting_active) {
        return;
    }

    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    if (!mc::is_block_item(jni, mc::player_get_held_item(jni, player))) {
        return;
    }

    // Sneak-bridge like a player: on the ground only place once actually sneaking at the edge.
    // (Towering skips this - sneaking would cancel the jump.)
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    const bool towering = g_bridge.tower.value &&
                          mc::key_binding_is_physically_down(jni, mc::settings_get_key_jump(jni, settings));
    if (g_bridge.edge_sneak.value && !towering && mc::entity_is_on_ground(jni, player) &&
        !mc::entity_is_sneaking(jni, player)) {
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
    const int32_t support_y = static_cast<int32_t>(std::floor(get_feet_pos(jni, player).y)) - 1;
    if (py != support_y || !is_air_block(jni, ctx->world, px, py, pz)) {
        return;
    }

    mc::key_binding_increment_press_time(jni, mc::settings_get_key_use_item(jni, settings));
}

// Restores the physical sneak key state when Bridge is disabled.
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module == &g_bridge.base && !ev->enabled && g_bridge.sneak_overridden) {
        events::GameContext* ctx = ev->ctx;
        jobject settings = mc::mc_get_game_settings(ctx->jni, ctx->minecraft);
        jobject sneak_key = mc::settings_get_key_sneak(ctx->jni, settings);
        mc::key_binding_set_pressed(ctx->jni, sneak_key, mc::key_binding_is_physically_down(ctx->jni, sneak_key));
        g_bridge.sneak_overridden = false;
    }
}

// Enables silent rotation + WASD move-fix while Bridge is aiming at a placement.
void on_rotation_spoof_query(void* event) noexcept {
    if (g_bridge.spoof_rotation_active) {
        auto* ev = static_cast<events::RotationSpoofEvent*>(event);
        ev->active = true;
        ev->move_fix = true;
    }
}

// Holds/releases an automatic sneak at open edges so walking straight does not drop you off the bridge.
void on_mid_living_update(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    events::JniContext* jni = ctx->jni;
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    jobject sneak_key = mc::settings_get_key_sneak(jni, settings);
    const bool phys_sneak = mc::key_binding_is_physically_down(jni, sneak_key);

    const auto release_override = [&]() noexcept {
        if (g_bridge.sneak_overridden) {
            mc::key_binding_set_pressed(jni, sneak_key, phys_sneak);
        }
        g_bridge.sneak_overridden = false;
    };

    jobject player = ctx->local_player;
    jobject jump_key = mc::settings_get_key_jump(jni, settings);
    const bool towering = g_bridge.tower.value && mc::key_binding_is_physically_down(jni, jump_key);

    // Never sneak while towering (sneak cancels the jump) or when disabled/out of focus.
    if (!ctx->in_game_focus || !g_bridge.edge_sneak.value || towering) {
        release_override();
        return;
    }

    const bool can_sneak =
        mc::entity_is_on_ground(jni, player) ||
        (g_bridge.sneak_on_jump.value && mc::entity_get_motion(jni, player).y >= 0.0);
    if (!can_sneak) {
        release_override();
        return;
    }

    g_bridge.sneak_overridden = true;
    const bool currently_sneaking = mc::key_binding_is_pressed(jni, sneak_key);

    if (currently_sneaking) {
        // Keep sneaking while still over an edge.
        if (is_over_air_edge(jni, settings, ctx->world, player)) {
            return;
        }
        // Edge is filled: release after a short humanized delay so we step forward.
        g_bridge.unsneak_timer -= 1.0f;
        if (g_bridge.unsneak_timer > 0.0f) {
            return;
        }
        mc::key_binding_set_pressed(jni, sneak_key, false);
        float next_delay = g_bridge.unsneak_delay.value;
        if (g_bridge.randomize.value) {
            next_delay += g_bridge.unsneak_delay_drift.sample_timed({-0.5f, 0.5f}, 30.0);
        }
        g_bridge.unsneak_timer += next_delay;
        return;
    }

    // Not sneaking yet: engage the moment we are heading over air.
    if (is_over_air_edge(jni, settings, ctx->world, player)) {
        mc::key_binding_set_pressed(jni, sneak_key, true);
    }
}

// Holds the classic sneak-bridge pose (silently facing away from where you walk, looking steeply down),
// fine-tuning onto a visible placement face when one exists. Move-fix turns your W into S in that pose,
// so you walk backwards exactly like manual Bridge Assist bridging.
void on_post_living_update(void* event) noexcept {
    g_bridge.targeting_active = false;
    g_bridge.spoof_rotation_active = false;

    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (!ctx->in_game_focus) {
        return;
    }

    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);

    // Always keep a block in hand: switch to the best block in the hotbar whenever you hold none.
    if (!mc::is_block_item(jni, mc::player_get_held_item(jni, player))) {
        mc::select_hotbar_block_slot(ctx, &g_bridge.base, true);
    }

    // Raw movement intent from the physical keys (the bound keys are remapped by move-fix).
    const auto down = [&](jobject key) noexcept { return mc::key_binding_is_physically_down(jni, key); };
    const int32_t fwd_in = (down(mc::settings_get_key_forward(jni, settings)) ? 1 : 0) -
                           (down(mc::settings_get_key_back(jni, settings)) ? 1 : 0);
    const int32_t str_in = (down(mc::settings_get_key_left(jni, settings)) ? 1 : 0) -
                           (down(mc::settings_get_key_right(jni, settings)) ? 1 : 0);
    const bool moving = fwd_in != 0 || str_in != 0;
    const bool towering = g_bridge.tower.value && down(mc::settings_get_key_jump(jni, settings));
    if (!moving && !towering) {
        return; // idle: release the spoof so your view snaps back
    }

    if (!mc::try_claim_aim_priority(1000u)) {
        return;
    }
    if (!mc::is_block_item(jni, mc::player_get_held_item(jni, player))) {
        return;
    }

    const math::Vec3d eye_pos = mc::vec3_to_vec3d(jni, mc::living_get_position_eyes(jni, player));
    const math::Vec2f cur_rot = math::vec2_add(
        mc::entity_get_rotation(jni, player),
        mc::get_pending_aim_rotation_delta());

    // Pose: back towards the walking direction (snapped to 45 deg), steep pitch.
    // The walking direction comes from the movement frame move-fix uses (Free Look locks it), never
    // from the camera - so Free Look / F5 camera movement does not steer the bridge.
    const float move_yaw = mc::is_rotation_spoof_active()
        ? mc::get_target_spoof_rotation().x
        : mc::entity_get_rotation(jni, player).x;
    const float rand_scale = g_bridge.randomize.value ? 1.0f : 0.0f;
    math::Vec2f desired_rot{cur_rot.x, 89.5f};
    if (moving) {
        const float intent = static_cast<float>(
            std::atan2(static_cast<double>(-str_in), static_cast<double>(fwd_in)) * 180.0 / math::kPi);
        const float back_yaw = move_yaw + intent + 180.0f;
        desired_rot.x = std::round(back_yaw / 45.0f) * 45.0f;
        if (!towering) {
            desired_rot.y = g_bridge.pitch.value +
                            g_bridge.aim_jitter_a.sample_timed({-0.6f, 0.6f}, 0.7) * rand_scale;
        }
    }

    // When hanging over the edge (or rising while towering) aim exactly at the visible face.
    mc::Vec3i support{};
    int32_t side = kFaceUp;
    math::Vec3d aim_point{};
    if (compute_place_target(ctx, eye_pos, cur_rot, &support, &side, &aim_point)) {
        desired_rot = math::calc_yaw_pitch(eye_pos, aim_point);
    }

    const math::Vec2f raw_diff = math::vec2_sub(desired_rot, cur_rot);
    const float yaw_diff = math::wrap_degrees_180(raw_diff.x);
    const float pitch_diff = raw_diff.y;
    const float max_yaw_step =
        g_bridge.yaw_speed_drift.sample_timed({0.8f, 1.2f}, 0.2) * g_bridge.aim_speed.value;
    const float max_pitch_step =
        g_bridge.pitch_speed_drift.sample_timed({0.8f, 1.2f}, 0.2) * g_bridge.aim_speed.value;
    mc::queue_aim_rotation_delta({
        math::clampf(-max_yaw_step, max_yaw_step, yaw_diff),
        math::clampf(-max_pitch_step, max_pitch_step, pitch_diff),
    });

    g_bridge.targeting_active = true;
    g_bridge.spoof_rotation_active = true;
}

// Counts the blocks in the hotbar and decides whether the crosshair label needs a redraw.
void on_gui_frame_poll(void* event) noexcept {
    auto* ev = static_cast<events::GuiFramePollEvent*>(event);
    events::GameContext* ctx = ev->ctx;

    g_bridge.hud_text[0] = '\0';
    if (g_bridge.show_block_count.value && ctx != nullptr && ctx->in_game_focus &&
        ctx->local_player != nullptr && ctx->world != nullptr) {
        events::JniContext* jni = ctx->jni;
        jobjectArray main_inv = mc::inv_player_get_main_inventory(jni, mc::player_get_inventory(jni, ctx->local_player));
        int32_t total = 0;
        for (jsize slot = 0; slot < 9; ++slot) {
            jobject stack = jni->env->GetObjectArrayElement(main_inv, slot);
            if (stack == nullptr) {
                continue;
            }
            if (mc::is_block_item(jni, mc::item_stack_get_item(jni, stack))) {
                total += mc::item_stack_get_size(jni, stack);
            }
            jni->env->DeleteLocalRef(stack);
        }
        std::snprintf(g_bridge.hud_text, sizeof(g_bridge.hud_text), "%d blocks", total);
    }

    const bool changed = std::strcmp(g_bridge.hud_text, g_bridge.hud_drawn) != 0;
    if (g_bridge.hud_text[0] != '\0') {
        ev->render_flags |= 1u;
    }
    if (changed) {
        ev->render_flags |= 3u; // redraw once so the old label is replaced / cleared
    }
}

// Draws "<n> blocks" just right of the crosshair.
void on_gui_render_2d(void* event) noexcept {
    auto* ev = static_cast<events::GuiRender2DEvent*>(event);
    std::memcpy(g_bridge.hud_drawn, g_bridge.hud_text, sizeof(g_bridge.hud_drawn));
    if (g_bridge.hud_text[0] == '\0') {
        return;
    }

    gui::GuiRenderer* renderer = ev->renderer;
    NVGcontext* vg = renderer->vg;
    const float x = renderer->logical_width * 0.5f + 8.0f;
    const float y = renderer->logical_height * 0.5f;

    nvgFontSize(vg, 10.0f);
    renderer->select_font(gui::kFontBold);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 160));
    nvgText(vg, x + 0.75f, y + 0.75f, g_bridge.hud_text, nullptr);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
    nvgText(vg, x, y, g_bridge.hud_text, nullptr);
}

// Renders Bridge settings in ClickGUI.
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_bridge.edge_offset.render(gui, renderer);
    g_bridge.show_block_count.render(gui, renderer);
    g_bridge.tower.render(gui, renderer);
    g_bridge.randomize.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Aim");
    g_bridge.pitch.render(gui, renderer);
    g_bridge.aim_speed.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Sneak");
    g_bridge.safe_edge.render(gui, renderer);
    g_bridge.edge_sneak.render(gui, renderer);
    g_bridge.sneak_on_jump.render(gui, renderer);
    g_bridge.unsneak_delay.render(gui, renderer);
}

// Serializes/deserializes Bridge config.
void visit_config(config::ConfigArchive* archive) noexcept {
    g_bridge.edge_offset.visit(archive, "edge_offset");
    g_bridge.show_block_count.visit(archive, "show_block_count");
    g_bridge.tower.visit(archive, "tower");
    g_bridge.randomize.visit(archive, "randomize");
    g_bridge.pitch.visit(archive, "pitch");
    g_bridge.aim_speed.visit(archive, "aim_speed");
    g_bridge.safe_edge.visit(archive, "safe_edge");
    g_bridge.edge_sneak.visit(archive, "edge_sneak");
    g_bridge.sneak_on_jump.visit(archive, "sneak_on_jump");
    g_bridge.unsneak_delay.visit(archive, "unsneak_delay");
}

} // namespace

events::Module* get_bridge_module() noexcept {
    return &g_bridge.base;
}

bool bridge_wants_ledge_clip() noexcept {
    return g_bridge.base.enabled && g_bridge.safe_edge.value;
}

} // namespace slinky::modules
