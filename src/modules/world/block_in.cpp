#include "modules/world/block_in.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "config/settings.hpp"
#include "events/events.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "util/math.hpp"
#include "util/random.hpp"

// Block In: walls all around you (feet + head level) and a roof, placed with silent mouse-style aim and real
// right-clicks.
//
// Planner (every tick): the needed cells are the 4 lower walls, the 4 upper walls and the roof above your head.
// For each empty cell it looks for a solid neighbour whose shared face is visible from the eye, confirmed by a
// block raytrace (the crosshair will really hit that face). Cells without such a support get a helper block
// first: below a wall that floats over air, or - for the roof - on top of an upper wall, which the roof can
// then be placed against. Walls go first, then the roof support, then the roof; inside a group the cell needing
// the smallest rotation wins. A click is only sent when the crosshair is on the planned face (CPS limited), and
// success is read back from the world, so nothing is assumed placed.

namespace slinky::modules {

namespace {

// EnumFacing ordinals: 0=Down 1=Up 2=North(-Z) 3=South(+Z) 4=West(-X) 5=East(+X)
constexpr int32_t kFaceDown = 0;
constexpr int32_t kFaceUp = 1;
constexpr int32_t kFaceNorth = 2;
constexpr int32_t kFaceSouth = 3;
constexpr int32_t kFaceWest = 4;
constexpr int32_t kFaceEast = 5;

constexpr double kMaxPlaceReachSq = 20.1601; // 4.49^2
constexpr int32_t kPendingTicks = 6;         // a clicked cell is not clicked again for this long
constexpr int32_t kStuckTicks = 40;          // nothing placeable for this long -> give up

// 4 cardinal directions ordered clockwise from South (+Z).
constexpr int32_t kCardinalDx[4] = {0, -1, 0, 1};
constexpr int32_t kCardinalDz[4] = {1, 0, -1, 0};

// Air (0), flowing/still water (8, 9), flowing/still lava (10, 11), tall grass (31), fire (51), snow layer (78).
bool is_replaceable_block_id(int32_t id) noexcept {
    switch (id) {
        case 0: case 8: case 9: case 10: case 11: case 31: case 51: case 78:
            return true;
        default:
            return id < 0;
    }
}

bool is_centered_in_block(const math::Vec3d& pos) noexcept {
    const double frac_x = std::fabs(std::fmod(pos.x, 1.0));
    if (frac_x < 0.31 || frac_x > 0.69) {
        return false;
    }
    const double frac_z = std::fabs(std::fmod(pos.z, 1.0));
    return frac_z >= 0.31 && frac_z <= 0.69;
}

void on_game_tick(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void on_rotation_spoof_query(void* event) noexcept;
void on_pre_living_update(void* event) noexcept;
void on_post_living_update(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

enum class CenterMoveMode : int32_t {
    None = 0,
    WalkCenter = 1,
    StopKeys = 2,
};

struct PendingCell {
    mc::Vec3i cell;
    int32_t until_tick;
};

struct BlockInState {
    events::Module base{};

    // Settings
    config::BoolSetting require_ground{{}, "On ground", false, false, {}};
    config::FloatSetting speed{{}, "Speed", "%.0f%%", 0.0f, 100.0f, 0.3f, 1.0f, 0.7f, 0.7f};
    config::FloatSetting click_speed{{}, "Click speed", "%.0f CPS", 1.0f, 0.0f, 5.0f, 20.0f, 14.0f, 14.0f};
    config::BoolSetting roof{{}, "Roof", true, true, {}};

    // Planned placement (written by on_post_living_update, used by on_game_tick)
    bool has_target = false;
    mc::Vec3i target_cell{0, 0, 0};
    mc::Vec3i support{0, 0, 0};
    int32_t support_face = kFaceUp;

    bool spoof_rotation_active = false;
    CenterMoveMode movement_mode = CenterMoveMode::None;
    math::Vec2f center_walk_rot{0.0f, 0.0f};
    float click_cooldown_ticks = 0.0f;
    int32_t tick_counter = 0;
    int32_t stuck_ticks = 0;
    std::vector<PendingCell> pending;

    // The cell you stand in, fixed while airborne so a jump does not shift every planned cell.
    bool anchor_valid = false;
    mc::Vec3i anchor{0, 0, 0};
    // Roof: from inside a 1x1 the top of the upper walls is above eye level, so a roof support can only be
    // placed at the top of a jump (like a player does).
    bool want_jump = false;
    bool jump_overridden = false;
    int32_t jump_grace_ticks = 0; // our own jump: "On ground" does not cancel it

    random::DriftNoise aim_u_drift{};
    random::DriftNoise aim_v_drift{};
    random::DriftNoise yaw_speed_drift{};
    random::DriftNoise pitch_speed_drift{};

    BlockInState() noexcept {
        base.name = "Block In";
        base.description = "Places blocks around you.";
        base.category = events::ModuleCategory::Block;
        base.enabled = false;
        base.flags = events::kModuleFlagNoToggleSwitch | events::kModuleFlagHiddenFromHud;
        base.keybind = 0;
        base.hold_mode = false;
        base.events[events::kEventGameTick] = {on_game_tick, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.events[events::kEventRotationSpoofQuery] = {on_rotation_spoof_query, -2000, 0};
        base.events[events::kEventPreLivingUpdate] = {on_pre_living_update, -1000, 0};
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, -500, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        base.format_hud_tag = nullptr;
    }
};

BlockInState g_block_in{};

// Scores defensive hardness of placeable blocks: Obsidian > Wood/EndStone/Cobble > Clay/Stone/Glass > Wool
int score_block_in_item(int item_id) noexcept {
    switch (item_id) {
        case 49: // Obsidian
            return 4;
        case 4:   // Cobblestone
        case 5:   // Planks
        case 17:  // Log
        case 121: // End Stone
        case 162: // Log2
            return 3;
        case 1:   // Stone
        case 2:   // Grass
        case 3:   // Dirt
        case 20:  // Glass
        case 24:  // Sandstone
        case 45:  // Brick
        case 87:  // Netherrack
        case 159: // Stained Hardened Clay
            return 2;
        case 35: // Wool
            return 1;
        default:
            return -1;
    }
}

bool same_cell(const mc::Vec3i& a, const mc::Vec3i& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool is_pending(const mc::Vec3i& cell) noexcept {
    for (const PendingCell& p : g_block_in.pending) {
        if (same_cell(p.cell, cell) && p.until_tick > g_block_in.tick_counter) {
            return true;
        }
    }
    return false;
}

struct Candidate {
    mc::Vec3i cell{};
    mc::Vec3i support{};
    int32_t face = kFaceUp;
    math::Vec3d aim{};
    float angle = 0.0f;
    int32_t group = 0; // 0 = walls (+ their floor helpers), 1 = roof support, 2 = roof
};

struct Planner {
    events::JniContext* jni;
    jobject world;
    jobject mutable_pos;
    math::Vec3d eye;
    math::Vec2f cur_rot;
    math::AABB player_box;

    int32_t block_id(int32_t x, int32_t y, int32_t z) const noexcept {
        jobject blk = mc::world_get_block_mutable_pos(jni, world, mutable_pos, x, y, z);
        return blk != nullptr ? mc::block_get_id(jni, blk) : -1;
    }

    bool empty(const mc::Vec3i& c) const noexcept {
        return is_replaceable_block_id(block_id(c.x, c.y, c.z));
    }

    bool intersects_player(const mc::Vec3i& c) const noexcept {
        const math::Vec3d lo = math::make_vec3(c.x, c.y, c.z);
        return math::aabb_intersects(player_box, math::make_aabb(lo, math::vec3_add(lo, math::make_vec3(1.0, 1.0, 1.0))));
    }

    // Best visible, raytrace-confirmed support face for placing into `cell`.
    bool best_face(const mc::Vec3i& cell, Candidate* out) const noexcept {
        struct Neighbour {
            int32_t dx, dy, dz, face;
        };
        constexpr Neighbour kNeighbours[6] = {
            {0, -1, 0, kFaceUp},    {-1, 0, 0, kFaceEast},  {1, 0, 0, kFaceWest},
            {0, 0, -1, kFaceSouth}, {0, 0, 1, kFaceNorth},  {0, 1, 0, kFaceDown},
        };
        const double cx = cell.x;
        const double cy = cell.y;
        const double cz = cell.z;
        const double u = 0.5 + g_block_in.aim_u_drift.sample_timed({-0.2f, 0.2f}, 0.6);
        const double v = 0.5 + g_block_in.aim_v_drift.sample_timed({-0.2f, 0.2f}, 0.6);

        bool found = false;
        for (const Neighbour& n : kNeighbours) {
            const mc::Vec3i sup{cell.x + n.dx, cell.y + n.dy, cell.z + n.dz};
            if (is_replaceable_block_id(block_id(sup.x, sup.y, sup.z))) {
                continue;
            }
            math::Vec3d aim{};
            bool visible = false;
            switch (n.face) {
                case kFaceUp:    aim = math::make_vec3(cx + u, cy, cz + v);       visible = eye.y > cy; break;
                case kFaceDown:  aim = math::make_vec3(cx + u, cy + 1.0, cz + v); visible = eye.y < cy + 1.0; break;
                case kFaceEast:  aim = math::make_vec3(cx, cy + u, cz + v);       visible = eye.x > cx; break;
                case kFaceWest:  aim = math::make_vec3(cx + 1.0, cy + u, cz + v); visible = eye.x < cx + 1.0; break;
                case kFaceSouth: aim = math::make_vec3(cx + u, cy + v, cz);       visible = eye.z > cz; break;
                default:         aim = math::make_vec3(cx + u, cy + v, cz + 1.0); visible = eye.z < cz + 1.0; break;
            }
            if (!visible || math::vec3_dist_sq(eye, aim) >= kMaxPlaceReachSq) {
                continue;
            }
            // The first block the crosshair ray hits must be the support, on exactly this face.
            const math::Vec3d end = math::make_vec3(eye.x + (aim.x - eye.x) * 1.05, eye.y + (aim.y - eye.y) * 1.05,
                                                    eye.z + (aim.z - eye.z) * 1.05);
            jobject start_vec = mc::vec3_new(jni, eye);
            jobject end_vec = mc::vec3_new(jni, end);
            jobject ray = mc::world_ray_trace_blocks(jni, world, start_vec, end_vec, false, true, false);
            bool ok = false;
            if (ray != nullptr) {
                if (mc::mop_get_type_ordinal(jni, ray) == 1) {
                    const mc::Vec3i hit = mc::mop_get_block_pos_vec3i(jni, ray);
                    ok = same_cell(hit, sup) && mc::mop_get_side_hit(jni, ray) == n.face;
                }
                jni->env->DeleteLocalRef(ray);
            }
            jni->env->DeleteLocalRef(start_vec);
            jni->env->DeleteLocalRef(end_vec);
            if (!ok) {
                continue;
            }
            const math::Vec2f desired = math::calc_yaw_pitch(eye, aim);
            const float angle = std::fabs(math::wrap_degrees_180(desired.x - cur_rot.x)) + std::fabs(desired.y - cur_rot.y);
            if (!found || angle < out->angle) {
                out->cell = cell;
                out->support = sup;
                out->face = n.face;
                out->aim = aim;
                out->angle = angle;
                found = true;
            }
        }
        return found;
    }
};

void disable_self(events::GameContext* ctx) noexcept {
    ctx->module_manager->request_set_enabled(&g_block_in.base, false);
}

// Right-clicks (CPS limited) whenever the crosshair is on the planned support face.
void on_game_tick(void* event) noexcept {
    float cd = g_block_in.click_cooldown_ticks;
    if (cd > 0.0f) {
        cd -= 1.0f;
        g_block_in.click_cooldown_ticks = cd;
        if (cd > 0.0f) {
            return;
        }
    }

    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (!ctx->in_game_focus || !g_block_in.has_target) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    if (!mc::is_block_item(jni, mc::player_get_held_item(jni, ctx->local_player))) {
        return;
    }
    jobject mop = mc::mc_get_object_mouse_over(jni, ctx->minecraft);
    if (mop == nullptr || mc::mop_get_type_ordinal(jni, mop) != 1) {
        return;
    }
    const mc::Vec3i hit = mc::mop_get_block_pos_vec3i(jni, mop);
    if (!same_cell(hit, g_block_in.support) || mc::mop_get_side_hit(jni, mop) != g_block_in.support_face) {
        return;
    }

    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    mc::key_binding_increment_press_time(jni, mc::settings_get_key_use_item(jni, settings));
    g_block_in.click_cooldown_ticks = cd + 20.0f / g_block_in.click_speed.value;
    g_block_in.pending.push_back(PendingCell{g_block_in.target_cell, g_block_in.tick_counter + kPendingTicks});
    g_block_in.has_target = false; // re-plan before the next click
}

// Restores the physical WASD state on disable; resets the planner on enable.
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_block_in.base) {
        return;
    }
    g_block_in.has_target = false;
    g_block_in.pending.clear();
    g_block_in.stuck_ticks = 0;
    g_block_in.click_cooldown_ticks = 0.0f;
    g_block_in.anchor_valid = false;
    g_block_in.want_jump = false;
    if (ev->enabled || ev->ctx == nullptr) {
        g_block_in.jump_overridden = false;
        return;
    }
    events::GameContext* ctx = ev->ctx;
    events::JniContext* jni = ctx->jni;
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);
    jobject keys[5] = {
        mc::settings_get_key_forward(jni, settings),
        mc::settings_get_key_back(jni, settings),
        mc::settings_get_key_left(jni, settings),
        mc::settings_get_key_right(jni, settings),
        mc::settings_get_key_jump(jni, settings),
    };
    g_block_in.jump_overridden = false;
    for (jobject key : keys) {
        mc::key_binding_set_pressed(jni, key, mc::key_binding_is_physically_down(jni, key));
    }
}

// Silent rotation; while walking to the block centre the move-fix steers towards it.
void on_rotation_spoof_query(void* event) noexcept {
    if (g_block_in.spoof_rotation_active) {
        auto* ev = static_cast<events::RotationSpoofEvent*>(event);
        ev->active = true;
        ev->move_fix = true;
        if (g_block_in.movement_mode == CenterMoveMode::WalkCenter) {
            ev->rotation = g_block_in.center_walk_rot;
        }
    }
}

// Overrides WASD to walk to the block centre or to stand still while placing.
void on_pre_living_update(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    events::JniContext* jni = ctx->jni;
    jobject settings = mc::mc_get_game_settings(jni, ctx->minecraft);

    // Jump for the roof support; hand the key back afterwards.
    jobject jump_key = mc::settings_get_key_jump(jni, settings);
    if (g_block_in.want_jump) {
        mc::key_binding_set_pressed(jni, jump_key, true);
        g_block_in.jump_overridden = true;
    } else if (g_block_in.jump_overridden) {
        mc::key_binding_set_pressed(jni, jump_key, mc::key_binding_is_physically_down(jni, jump_key));
        g_block_in.jump_overridden = false;
    }

    if (g_block_in.movement_mode == CenterMoveMode::None) {
        return;
    }
    mc::key_binding_set_pressed(jni, mc::settings_get_key_forward(jni, settings),
                                g_block_in.movement_mode == CenterMoveMode::WalkCenter);
    mc::key_binding_set_pressed(jni, mc::settings_get_key_back(jni, settings), false);
    mc::key_binding_set_pressed(jni, mc::settings_get_key_left(jni, settings), false);
    mc::key_binding_set_pressed(jni, mc::settings_get_key_right(jni, settings), false);
}

// Plans the next placement and aims at it.
void on_post_living_update(void* event) noexcept {
    g_block_in.spoof_rotation_active = false;
    g_block_in.movement_mode = CenterMoveMode::None;
    ++g_block_in.tick_counter;

    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (!ctx->in_game_focus) {
        disable_self(ctx);
        return;
    }
    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    const bool on_ground = mc::entity_is_on_ground(jni, player);
    if (g_block_in.jump_grace_ticks > 0) {
        --g_block_in.jump_grace_ticks;
    }
    if (g_block_in.require_ground.value && !on_ground && g_block_in.jump_grace_ticks == 0) {
        disable_self(ctx);
        return;
    }

    // Drop expired pending cells.
    std::vector<PendingCell>& pending = g_block_in.pending;
    for (size_t i = 0; i < pending.size();) {
        if (pending[i].until_tick <= g_block_in.tick_counter) {
            pending[i] = pending.back();
            pending.pop_back();
        } else {
            ++i;
        }
    }

    math::Vec3d feet = mc::entity_get_position(jni, player);
    if (mc::is_version_1_7_10(jni)) {
        feet = math::vec3_sub_eye_height(feet);
    }
    const mc::Vec3i here{static_cast<int32_t>(std::floor(feet.x)), static_cast<int32_t>(std::floor(feet.y)),
                         static_cast<int32_t>(std::floor(feet.z))};
    if (on_ground || !g_block_in.anchor_valid) {
        g_block_in.anchor = here;
        g_block_in.anchor_valid = true;
    }
    const mc::Vec3i f = g_block_in.anchor;
    g_block_in.want_jump = false;

    // All planning JNI work (block lookups, raytraces) inside one local frame.
    if (jni->env->PushLocalFrame(512) != 0) {
        return;
    }
    jobject mutable_pos = (jni->mappings->version != 0) ? mc::block_pos_new(jni, 0, 0, 0) : nullptr;
    Planner planner{
        jni,
        ctx->world,
        mutable_pos,
        mc::vec3_to_vec3d(jni, mc::living_get_position_eyes(jni, player)),
        math::vec2_add(mc::entity_get_rotation(jni, player), mc::get_pending_aim_rotation_delta()),
        mc::aabb_to_aabb(jni, mc::entity_get_bounding_box(jni, player)),
    };

    bool any_missing = false;
    bool have = false;
    Candidate best{};
    const auto consider = [&](const mc::Vec3i& cell, int32_t group) noexcept {
        if (is_pending(cell) || planner.intersects_player(cell)) {
            return false;
        }
        Candidate c{};
        if (!planner.best_face(cell, &c)) {
            return false;
        }
        c.group = group;
        if (!have || c.group < best.group || (c.group == best.group && c.angle < best.angle)) {
            best = c;
            have = true;
        }
        return true;
    };

    // Walls: feet and head level in all 4 directions (a floor helper below a wall that floats over air).
    for (int32_t d = 0; d < 4; ++d) {
        for (int32_t level = 0; level < 2; ++level) {
            const mc::Vec3i cell{f.x + kCardinalDx[d], f.y + level, f.z + kCardinalDz[d]};
            if (!planner.empty(cell)) {
                continue;
            }
            any_missing = true;
            if (!consider(cell, 0) && level == 0) {
                const mc::Vec3i below{cell.x, cell.y - 1, cell.z};
                if (planner.empty(below)) {
                    consider(below, 0);
                }
            }
        }
    }

    // Roof above the head; without a neighbour to place against, first put a support on top of an upper wall.
    bool roof_needs_jump = false;
    if (g_block_in.roof.value) {
        const mc::Vec3i roof{f.x, f.y + 2, f.z};
        if (planner.empty(roof)) {
            any_missing = true;
            if (!consider(roof, 2)) {
                bool support_possible = false;
                bool support_placeable = false;
                for (int32_t d = 0; d < 4; ++d) {
                    const mc::Vec3i side{f.x + kCardinalDx[d], f.y + 2, f.z + kCardinalDz[d]};
                    const mc::Vec3i wall{side.x, f.y + 1, side.z};
                    if (planner.empty(side) && !planner.empty(wall)) {
                        support_possible = true;
                        support_placeable = consider(side, 1) || support_placeable;
                    }
                }
                roof_needs_jump = support_possible && !support_placeable;
            }
        }
    }

    jni->env->PopLocalFrame(nullptr);

    if (!any_missing) {
        // Done; finish once standing centred so the walls fit snugly.
        g_block_in.has_target = false;
        if (on_ground && is_centered_in_block(feet)) {
            disable_self(ctx);
            return;
        }
    } else if (!have) {
        g_block_in.has_target = false;
        if (roof_needs_jump && on_ground && is_centered_in_block(feet) && pending.empty()) {
            g_block_in.want_jump = true; // the roof support becomes visible at the top of the jump
            g_block_in.jump_grace_ticks = 15;
            g_block_in.stuck_ticks = 0;
        } else if (pending.empty() && ++g_block_in.stuck_ticks > kStuckTicks) {
            disable_self(ctx); // nothing reachable to place against
            return;
        }
    }

    // Walk to (or stay in) the block centre, so no wall cell is blocked by the player's hitbox.
    if (is_centered_in_block(feet)) {
        g_block_in.movement_mode = CenterMoveMode::StopKeys;
    } else {
        const math::Vec3d center = math::make_vec3(std::floor(feet.x) + 0.5, feet.y, std::floor(feet.z) + 0.5);
        g_block_in.movement_mode = CenterMoveMode::WalkCenter;
        g_block_in.center_walk_rot = math::calc_yaw_pitch(feet, center);
    }
    g_block_in.spoof_rotation_active = true;

    if (!have) {
        return;
    }
    g_block_in.stuck_ticks = 0;

    if (!mc::try_claim_aim_priority(500u)) {
        g_block_in.has_target = false;
        return;
    }
    mc::select_hotbar_slot_by_scorer(ctx, &g_block_in.base, true, score_block_in_item);
    if (!mc::is_block_item(jni, mc::player_get_held_item(jni, player))) {
        disable_self(ctx); // no blocks in the hotbar
        return;
    }

    g_block_in.has_target = true;
    g_block_in.target_cell = best.cell;
    g_block_in.support = best.support;
    g_block_in.support_face = best.face;

    // Mouse-style aim: proportional step blended with a clamped step (as before), faster with "Speed".
    const math::Vec2f desired = math::calc_yaw_pitch(planner.eye, best.aim);
    const math::Vec2f raw_diff = math::vec2_sub(desired, planner.cur_rot);
    const float yaw_diff = math::wrap_degrees_180(raw_diff.x);
    const float pitch_diff = raw_diff.y;
    const float speed = g_block_in.speed.value;
    const float prop_factor = speed * 0.5f + 0.5f;
    const math::Vec2f prop_step = math::vec2_scale({yaw_diff, pitch_diff}, prop_factor);
    const float max_yaw_step = g_block_in.yaw_speed_drift.sample_timed({0.8f, 1.2f}, 0.2) * 180.0f * speed;
    const float max_pitch_step = g_block_in.pitch_speed_drift.sample_timed({0.8f, 1.2f}, 0.2) * 180.0f * speed;
    const math::Vec2f clamped_step{
        math::clampf(-max_yaw_step, max_yaw_step, yaw_diff),
        math::clampf(-max_pitch_step, max_pitch_step, pitch_diff),
    };
    mc::queue_aim_rotation_delta(math::vec2_lerp(prop_step, clamped_step, 0.5f));
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_block_in.speed.render(gui, renderer);
    g_block_in.click_speed.render(gui, renderer);
    g_block_in.roof.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Conditions");
    g_block_in.require_ground.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_block_in.speed.visit(archive, "speed");
    g_block_in.require_ground.visit(archive, "require_ground");
    g_block_in.click_speed.visit(archive, "click_speed");
    g_block_in.roof.visit(archive, "roof");
}

} // namespace

events::Module* get_block_in_module() noexcept {
    return &g_block_in.base;
}

} // namespace slinky::modules
