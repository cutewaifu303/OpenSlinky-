#include "modules/world/bed_breaker.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "config/settings.hpp"
#include "events/events.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "util/math.hpp"
#include "events/module_manager.hpp"
#include "modules/world/fast_mine_core.hpp"
#include "util/random.hpp"

// Bed Breaker (legit only) - works like a player with a mouse, same scheme as Bridge:
//   * aim:   silent rotation in mouse-sized steps (deg/tick, GCD-quantized by the aim system),
//            only onto bed faces a block raytrace confirms the crosshair can actually hit
//   * click: real left mouse button - one press edge, then held while the crosshair is on the
//            bed (or on the block covering it), released the moment it is not
// Vanilla PlayerControllerMP does the actual mining, so Fast Mine speeds it up on its own.
// Tool switching shares Auto Tool's hotbar lock, so the two never fight over the slot.

namespace slinky::modules {

namespace {

constexpr int32_t kBlockBed      = 26;
constexpr int32_t kBlockBedrock  = 7;
constexpr int32_t kBlockBarrier  = 166;
constexpr double  kBedHeight     = 0.5625;

// EnumFacing ordinals: 1=Up(+Y) 2=North(-Z) 3=South(+Z) 4=West(-X) 5=East(+X)
constexpr int32_t kFaceUp    = 1;
constexpr int32_t kFaceNorth = 2;
constexpr int32_t kFaceSouth = 3;
constexpr int32_t kFaceWest  = 4;
constexpr int32_t kFaceEast  = 5;

constexpr uint32_t kAimPriority         = 800u; // yields to Bridge / Clutch (1000)
constexpr int32_t  kRescanIntervalTicks = 5;    // throttle for the block scan while no bed is found
constexpr int32_t  kMaxBedCandidates    = 8;
constexpr int32_t  kLockMissTicks       = 12;   // crosshair off the locked block this long -> pick a new block
constexpr int32_t  kSwitchBackGraceTicks = 10;  // target lost this long before the tool is switched back

void on_game_tick(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void on_rotation_spoof_query(void* event) noexcept;
void on_post_living_update(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

struct BedBreakerState {
    events::Module base{};

    // Settings (no mode selection - always legit)
    config::FloatSetting range{{}, "Range", "%.2f blocks", 0.05f, 0.0f, 3.0f, 4.5f, 4.5f, 4.5f};
    config::FloatSetting fov{{}, "FOV", "%.0f\xC2\xB0", 1.0f, 0.0f, 30.0f, 360.0f, 360.0f, 360.0f};
    config::FloatSetting aim_speed{{}, "Aim speed", "%.0f \xC2\xB0/s", 1.0f, 20.0f, 5.0f, 180.0f, 30.0f, 30.0f};
    config::BoolSetting randomize{{}, "Randomize", true, true, {}};
    config::BoolSetting break_cover{{}, "Break cover blocks", true, true, {}};
    config::BoolSetting swap_tool{{}, "Swap tool", true, true, {}};
    config::BoolSetting switch_back{{}, "Switch back", true, true, {}};

    // Built-in Fast Mine (same algorithm and ranges as the Fast Mine module), applied only to the blocks Bed
    // Breaker itself is breaking (the bed and its cover blocks), independent of the Fast Mine module.
    config::BoolSetting fast_mine{{}, "Fast mine", true, true, {}};
    config::FloatSetting fm_speed{{}, "Speed", "%.2fx", 0.01f, 0.0f, 1.0f, 2.0f, 1.0f, 1.0f};
    config::FloatSetting fm_break_delay{{}, "Break delay", "%.0f ms", 1.0f, 50.0f, 0.0f, 5.0f, 5.0f, 5.0f};
    config::BoolSetting fm_decr_block_hit_delay{{}, "Always decrease break delay", false, false, {}};
    fast_mine_core::State fm_core{};

    // Target (owned by on_post_living_update, read by on_game_tick)
    bool has_bed = false;
    mc::Vec3i bed{0, 0, 0};          // targeted bed half
    int32_t face = -1;               // face of the bed being aimed at (hysteresis)
    bool mine_is_cover = false;      // crosshair ray hits a block covering the bed
    mc::Vec3i mine_target{0, 0, 0};  // that cover block
    math::Vec3d aim_point{0.0, 0.0, 0.0};
    bool aim_active = false;         // aimed this tick -> clicking allowed
    bool spoof_rotation_active = false;
    int32_t rescan_cooldown = 0;

    // Mining lock: once a block is being mined it stays the target (fixed aim point, no re-evaluation, no
    // jitter) until it is broken, out of reach or off the crosshair for kLockMissTicks. Without this the target
    // face / cover block was re-picked every tick and the crosshair hopped between blocks, restarting progress.
    bool locked = false;
    mc::Vec3i locked_block{0, 0, 0};
    int32_t locked_id = -1;
    math::Vec3d locked_aim{0.0, 0.0, 0.0};
    int32_t lock_miss_ticks = 0;
    int32_t lost_ticks = 0;          // ticks without any target (tool switch-back grace)

    // Side effects we own and must undo
    bool attack_overridden = false;
    int32_t saved_prev_slot = -1;
    int32_t selected_tool_slot = -1;

    random::DriftNoise jitter_x{};
    random::DriftNoise jitter_y{};
    random::DriftNoise jitter_z{};
    random::DriftNoise yaw_speed_drift{};
    random::DriftNoise pitch_speed_drift{};

    BedBreakerState() noexcept {
        base.name = "Bed Breaker";
        base.description = "Legit BedBreaker";
        base.category = events::ModuleCategory::Block;
        base.enabled = false;
        base.flags = events::kModuleFlagNone;
        base.keybind = 0;
        base.hold_mode = false;
        base.events[events::kEventGameTick] = {on_game_tick, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.events[events::kEventRotationSpoofQuery] = {on_rotation_spoof_query, -100, 0};
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, -1000, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        base.format_hud_tag = nullptr;
    }
};

BedBreakerState g_bed_breaker{};

int32_t block_id_at(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) noexcept {
    jobject block = mc::world_get_block(jni, world, x, y, z);
    return (block != nullptr) ? mc::block_get_id(jni, block) : -1;
}

bool same_pos(const mc::Vec3i& a, const mc::Vec3i& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// True if pos is the targeted bed half or the horizontally adjacent other half (caller checks the id).
bool is_same_bed(const mc::Vec3i& pos, const mc::Vec3i& bed) noexcept {
    return pos.y == bed.y && (std::abs(pos.x - bed.x) + std::abs(pos.z - bed.z)) <= 1;
}

void unlock_target() noexcept {
    g_bed_breaker.locked = false;
    g_bed_breaker.locked_id = -1;
    g_bed_breaker.lock_miss_ticks = 0;
}

void clear_target() noexcept {
    g_bed_breaker.has_bed = false;
    g_bed_breaker.face = -1;
    g_bed_breaker.mine_is_cover = false;
    unlock_target();
}

// ---------------------------------------------------------------------------------------------
// Mouse: left button
// ---------------------------------------------------------------------------------------------

// Presses / keeps holding the left mouse button. A press edge (pressTime++) is only generated when
// the button is not already down - exactly like a physical mouse press followed by holding it.
void press_attack(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    jobject attack_key = mc::settings_get_key_attack(jni, mc::mc_get_game_settings(jni, ctx->minecraft));
    if (!mc::key_binding_is_pressed(jni, attack_key)) {
        mc::key_binding_increment_press_time(jni, attack_key);
    }
    mc::key_binding_set_pressed(jni, attack_key, true);
    g_bed_breaker.attack_overridden = true;
}

// Releases the left mouse button back to its physical state - only if we were the one holding it,
// so other modules (Auto Clicker, ...) and the real mouse are never overwritten.
void release_attack(events::GameContext* ctx) noexcept {
    if (!g_bed_breaker.attack_overridden) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    jobject attack_key = mc::settings_get_key_attack(jni, mc::mc_get_game_settings(jni, ctx->minecraft));
    mc::key_binding_set_pressed(jni, attack_key, mc::key_binding_is_physically_down(jni, attack_key));
    g_bed_breaker.attack_overridden = false;
}

// ---------------------------------------------------------------------------------------------
// Tool (shares Auto Tool's hotbar lock)
// ---------------------------------------------------------------------------------------------

void restore_tool(events::GameContext* ctx) noexcept {
    mc::release_hotbar_lock(&g_bed_breaker.base);
    if (g_bed_breaker.saved_prev_slot != -1) {
        jobject inv = mc::player_get_inventory(ctx->jni, ctx->local_player);
        mc::inv_player_set_current_item(ctx->jni, inv, g_bed_breaker.saved_prev_slot);
    }
    g_bed_breaker.saved_prev_slot = -1;
    g_bed_breaker.selected_tool_slot = -1;
}

// Switches to the hotbar slot that breaks `block` (the block under the crosshair) fastest.
void swap_to_best_tool(events::GameContext* ctx, jobject block) noexcept {
    events::JniContext* jni = ctx->jni;
    jobject inv = mc::player_get_inventory(jni, ctx->local_player);
    const int32_t current_slot = mc::inv_player_get_current_item(jni, inv);

    // The player changed the slot themselves: hand control back without switching it again.
    if (g_bed_breaker.selected_tool_slot != -1 && g_bed_breaker.selected_tool_slot != current_slot) {
        mc::release_hotbar_lock(&g_bed_breaker.base);
        g_bed_breaker.saved_prev_slot = -1;
        g_bed_breaker.selected_tool_slot = -1;
        return;
    }

    float best_strength = 1.0f;
    int32_t best_slot = current_slot;
    for (int32_t offset = 0; offset < 9; ++offset) {
        const int32_t slot = (current_slot + offset) % 9;
        jobject stack = mc::inventory_get_stack_in_slot(jni, inv, slot);
        if (stack == nullptr) {
            continue;
        }
        const float strength = mc::item_stack_get_str_vs_block(jni, stack, block);
        if (strength > best_strength) {
            best_strength = strength;
            best_slot = slot;
        }
    }

    if (best_slot != current_slot && mc::try_acquire_hotbar_lock(&g_bed_breaker.base)) {
        if (g_bed_breaker.saved_prev_slot == -1 && g_bed_breaker.switch_back.value) {
            g_bed_breaker.saved_prev_slot = current_slot;
        }
        mc::inv_player_set_current_item(jni, inv, best_slot);
        g_bed_breaker.selected_tool_slot = best_slot;
    }
}

// ---------------------------------------------------------------------------------------------
// Targeting
// ---------------------------------------------------------------------------------------------

struct FaceChoice {
    int32_t face = -1;
    bool cover = false;
    mc::Vec3i mine{};
    math::Vec3d aim{};
    float angle = 0.0f;
};

// Picks the exposed bed face to aim at. Every face is verified with a crosshair-equivalent block
// raytrace: the first block hit must be this bed (direct) or - with "Break cover blocks" - a
// breakable block in front of it (cover). Direct beats cover; then smallest rotation change, with a
// bonus for the face we were already aiming at so the aim does not flip between faces.
bool evaluate_bed(
    events::GameContext* ctx,
    const math::Vec3d& eye,
    math::Vec2f cur_rot,
    const mc::Vec3i& bed,
    int32_t prefer_face,
    FaceChoice* out) noexcept
{
    events::JniContext* jni = ctx->jni;
    jobject world = ctx->world;
    if (jni->env->PushLocalFrame(64) != 0) {
        return false;
    }

    struct Face {
        int32_t side;
        int32_t dx;
        int32_t dz;
    };
    constexpr Face kFaces[5] = {
        {kFaceUp,     0,  0},
        {kFaceNorth,  0, -1},
        {kFaceSouth,  0,  1},
        {kFaceWest,  -1,  0},
        {kFaceEast,   1,  0},
    };

    const double bx = static_cast<double>(bed.x);
    const double by = static_cast<double>(bed.y);
    const double bz = static_cast<double>(bed.z);
    const double range = static_cast<double>(g_bed_breaker.range.value);
    const double range_sq = range * range;

    const float rand_scale = g_bed_breaker.randomize.value ? 1.0f : 0.0f;
    const double jx = static_cast<double>(g_bed_breaker.jitter_x.sample_timed({-0.15f, 0.15f}, 0.5) * rand_scale);
    const double jy = static_cast<double>(g_bed_breaker.jitter_y.sample_timed({-0.08f, 0.08f}, 0.5) * rand_scale);
    const double jz = static_cast<double>(g_bed_breaker.jitter_z.sample_timed({-0.15f, 0.15f}, 0.5) * rand_scale);
    const auto clamp_d = [](double lo, double hi, double v) noexcept { return v < lo ? lo : (v > hi ? hi : v); };

    const double face_x = clamp_d(bx + 0.2, bx + 0.8, bx + 0.5 + jx);
    const double face_z = clamp_d(bz + 0.2, bz + 0.8, bz + 0.5 + jz);
    const double side_y = clamp_d(by + 0.12, by + 0.45, by + 0.28 + jy);

    bool found = false;
    float best_score = 0.0f;
    for (const Face& f : kFaces) {
        math::Vec3d aim{};
        bool visible = false;
        if (f.side == kFaceUp) {
            aim = math::make_vec3(face_x, by + kBedHeight, face_z);
            visible = eye.y > by + kBedHeight;
        } else {
            // The side touching the other bed half is not exposed.
            if (block_id_at(jni, world, bed.x + f.dx, bed.y, bed.z + f.dz) == kBlockBed) {
                continue;
            }
            switch (f.side) {
                case kFaceNorth:
                    aim = math::make_vec3(face_x, side_y, bz);
                    visible = eye.z < bz;
                    break;
                case kFaceSouth:
                    aim = math::make_vec3(face_x, side_y, bz + 1.0);
                    visible = eye.z > bz + 1.0;
                    break;
                case kFaceWest:
                    aim = math::make_vec3(bx, side_y, face_z);
                    visible = eye.x < bx;
                    break;
                default: // kFaceEast
                    aim = math::make_vec3(bx + 1.0, side_y, face_z);
                    visible = eye.x > bx + 1.0;
                    break;
            }
        }
        // Must be inside vanilla block reach, otherwise the crosshair can never land on it.
        if (!visible || math::vec3_dist_sq(eye, aim) > range_sq) {
            continue;
        }

        // Same raytrace flags as vanilla getMouseOver (selectable blocks like torches/grass count).
        const math::Vec3d end = math::make_vec3(
            eye.x + (aim.x - eye.x) * 1.05,
            eye.y + (aim.y - eye.y) * 1.05,
            eye.z + (aim.z - eye.z) * 1.05);
        jobject ray = mc::world_ray_trace_blocks(
            jni, world, mc::vec3_new(jni, eye), mc::vec3_new(jni, end), false, false, false);
        if (ray == nullptr || mc::mop_get_type_ordinal(jni, ray) != 1) {
            continue;
        }
        const mc::Vec3i hit = mc::mop_get_block_pos_vec3i(jni, ray);
        const int32_t hit_id = block_id_at(jni, world, hit.x, hit.y, hit.z);

        bool cover = false;
        if (hit_id == kBlockBed && is_same_bed(hit, bed)) {
            cover = false;
        } else if (g_bed_breaker.break_cover.value && hit_id > 0 &&
                   hit_id != kBlockBedrock && hit_id != kBlockBarrier && hit_id != kBlockBed) {
            cover = true;
        } else {
            continue;
        }

        const math::Vec2f desired = math::calc_yaw_pitch(eye, aim);
        const float angle = std::fabs(math::wrap_degrees_180(desired.x - cur_rot.x)) +
                            std::fabs(desired.y - cur_rot.y);
        const float score = angle + (cover ? 1000.0f : 0.0f) - (f.side == prefer_face ? 15.0f : 0.0f);
        if (!found || score < best_score) {
            best_score = score;
            out->face = f.side;
            out->cover = cover;
            out->mine = cover ? hit : bed;
            out->aim = aim;
            out->angle = angle;
            found = true;
        }
    }

    jni->env->PopLocalFrame(nullptr);
    return found;
}

// Collects up to `max_count` bed blocks within reach, nearest first. One JNI local frame per Y-layer
// keeps the live reference count bounded; cells outside the reach sphere are skipped without JNI.
int32_t scan_beds(events::GameContext* ctx, const math::Vec3d& eye, mc::Vec3i* out, int32_t max_count) noexcept {
    events::JniContext* jni = ctx->jni;
    const double reach = static_cast<double>(g_bed_breaker.range.value) + 0.9;
    const double reach_sq = reach * reach;
    const int32_t radius = static_cast<int32_t>(std::ceil(reach));
    const int32_t ex = static_cast<int32_t>(std::floor(eye.x));
    const int32_t ey = static_cast<int32_t>(std::floor(eye.y));
    const int32_t ez = static_cast<int32_t>(std::floor(eye.z));

    double dists[kMaxBedCandidates]{};
    int32_t count = 0;

    for (int32_t dy = -radius; dy <= radius; ++dy) {
        if (jni->env->PushLocalFrame(512) != 0) {
            continue;
        }
        jobject mutable_pos = (jni->mappings->version != 0) ? mc::block_pos_new(jni, 0, 0, 0) : nullptr;

        for (int32_t dx = -radius; dx <= radius; ++dx) {
            for (int32_t dz = -radius; dz <= radius; ++dz) {
                const int32_t bx = ex + dx;
                const int32_t by = ey + dy;
                const int32_t bz = ez + dz;
                const math::Vec3d center = math::make_vec3(
                    static_cast<double>(bx) + 0.5, static_cast<double>(by) + 0.28, static_cast<double>(bz) + 0.5);
                const double dist_sq = math::vec3_dist_sq(eye, center);
                if (dist_sq > reach_sq) {
                    continue;
                }
                if (count == max_count && dist_sq >= dists[count - 1]) {
                    continue;
                }

                jobject block = mc::world_get_block_mutable_pos(jni, ctx->world, mutable_pos, bx, by, bz);
                if (block == nullptr || mc::block_get_id(jni, block) != kBlockBed) {
                    continue;
                }

                // Sorted insert (nearest first).
                int32_t i = (count < max_count) ? count++ : count - 1;
                while (i > 0 && dists[i - 1] > dist_sq) {
                    dists[i] = dists[i - 1];
                    out[i] = out[i - 1];
                    --i;
                }
                dists[i] = dist_sq;
                out[i] = mc::Vec3i{bx, by, bz};
            }
        }

        jni->env->PopLocalFrame(nullptr);
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

fast_mine_core::Params fast_mine_params() noexcept {
    return {g_bed_breaker.fm_speed.value, g_bed_breaker.fm_break_delay.value,
            g_bed_breaker.fm_decr_block_hit_delay.value};
}

// Target upkeep + mouse-style silent aim (runs after the player update, like Bridge).
void on_post_living_update(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;

    // Built-in Fast Mine: vanilla just advanced the progress of the block we mined this tick (if any).
    // post_living() only acts when on_game_tick armed it for an eligible block this tick.
    if (g_bed_breaker.fast_mine.value) {
        jobject controller = mc::mc_get_player_controller(ctx->jni, ctx->minecraft);
        if (controller != nullptr) {
            fast_mine_core::post_living(ctx->jni, controller, fast_mine_params(), g_bed_breaker.fm_core);
        }
    }

    g_bed_breaker.aim_active = false;
    g_bed_breaker.spoof_rotation_active = false;
    if (!ctx->in_game_focus) {
        clear_target();
        restore_tool(ctx);
        return;
    }
    if (g_bed_breaker.rescan_cooldown > 0) {
        --g_bed_breaker.rescan_cooldown;
    }

    events::JniContext* jni = ctx->jni;
    jobject player = ctx->local_player;
    const math::Vec3d eye = mc::vec3_to_vec3d(jni, mc::living_get_position_eyes(jni, player));
    const math::Vec2f cur_rot =
        math::vec2_add(mc::entity_get_rotation(jni, player), mc::get_pending_aim_rotation_delta());

    // Mining lock: keep the block that is being mined until it is gone (broken -> id changes) or out of reach.
    if (g_bed_breaker.locked) {
        const mc::Vec3i& lb = g_bed_breaker.locked_block;
        const double range = static_cast<double>(g_bed_breaker.range.value);
        if (!g_bed_breaker.has_bed || block_id_at(jni, ctx->world, lb.x, lb.y, lb.z) != g_bed_breaker.locked_id ||
            math::vec3_dist_sq(eye, g_bed_breaker.locked_aim) > range * range) {
            unlock_target();
        }
    }

    FaceChoice choice{};
    bool have = false;
    if (g_bed_breaker.locked) {
        have = true; // keep target, face, cover block and aim point exactly as they are
    } else if (g_bed_breaker.has_bed &&
               block_id_at(jni, ctx->world, g_bed_breaker.bed.x, g_bed_breaker.bed.y, g_bed_breaker.bed.z) ==
                   kBlockBed) {
        // Keep the current bed while it still exists and has a hittable face.
        have = evaluate_bed(ctx, eye, cur_rot, g_bed_breaker.bed, g_bed_breaker.face, &choice);
    }

    // Otherwise look for a new one (throttled), nearest first, inside the FOV.
    if (!have) {
        clear_target();
        if (g_bed_breaker.rescan_cooldown == 0) {
            g_bed_breaker.rescan_cooldown = kRescanIntervalTicks;
            mc::Vec3i beds[kMaxBedCandidates]{};
            const int32_t n = scan_beds(ctx, eye, beds, kMaxBedCandidates);
            const float fov = g_bed_breaker.fov.value;
            for (int32_t i = 0; i < n && !have; ++i) {
                FaceChoice c{};
                if (evaluate_bed(ctx, eye, cur_rot, beds[i], -1, &c) && (fov >= 360.0f || c.angle <= fov)) {
                    g_bed_breaker.bed = beds[i];
                    g_bed_breaker.has_bed = true;
                    choice = c;
                    have = true;
                }
            }
        }
    }

    if (!have) {
        // Done (or nothing to do): switch back - after a short grace so a one-tick gap does not flip the slot.
        if (++g_bed_breaker.lost_ticks > kSwitchBackGraceTicks) {
            restore_tool(ctx);
        }
        return;
    }
    g_bed_breaker.lost_ticks = 0;

    if (g_bed_breaker.locked) {
        g_bed_breaker.aim_point = g_bed_breaker.locked_aim; // fixed point on the block being mined
    } else {
        g_bed_breaker.face = choice.face;
        g_bed_breaker.mine_is_cover = choice.cover;
        g_bed_breaker.mine_target = choice.mine;
        g_bed_breaker.aim_point = choice.aim;
    }

    // A higher-priority aim owner (Bridge, Clutch, ...) is active: don't fight it this tick.
    if (!mc::try_claim_aim_priority(kAimPriority)) {
        return;
    }

    // Mouse-style aim: per-tick step limited by Aim speed (with drift), like Bridge.
    const math::Vec2f desired = math::calc_yaw_pitch(eye, g_bed_breaker.aim_point);
    const float yaw_diff = math::wrap_degrees_180(desired.x - cur_rot.x);
    const float pitch_diff = desired.y - cur_rot.y;
    const float drift_lo = g_bed_breaker.randomize.value ? 0.8f : 1.0f;
    const float drift_hi = g_bed_breaker.randomize.value ? 1.2f : 1.0f;
    const float max_yaw_step =
        g_bed_breaker.yaw_speed_drift.sample_timed({drift_lo, drift_hi}, 0.2) * g_bed_breaker.aim_speed.value;
    const float max_pitch_step =
        g_bed_breaker.pitch_speed_drift.sample_timed({drift_lo, drift_hi}, 0.2) * g_bed_breaker.aim_speed.value;
    mc::queue_aim_rotation_delta({
        math::clampf(-max_yaw_step, max_yaw_step, yaw_diff),
        math::clampf(-max_pitch_step, max_pitch_step, pitch_diff),
    });

    g_bed_breaker.aim_active = true;
    g_bed_breaker.spoof_rotation_active = true;
}

// Fires right after vanilla getMouseOver and before vanilla processes the mouse: holds the left
// button while the crosshair is on the bed (or its cover block), releases it otherwise. Because the
// release happens before vanilla mines, nothing behind/under the bed is ever touched.
void on_game_tick(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    fast_mine_core::skip_tick(g_bed_breaker.fm_core); // armed again below only while mining the bed / cover
    if (!ctx->in_game_focus || !g_bed_breaker.aim_active) {
        release_attack(ctx);
        return;
    }

    events::JniContext* jni = ctx->jni;
    jobject mop = mc::mc_get_object_mouse_over(jni, ctx->minecraft);
    if (mop == nullptr || mc::mop_get_type_ordinal(jni, mop) != 1) {
        release_attack(ctx);
        return;
    }

    const mc::Vec3i hit = mc::mop_get_block_pos_vec3i(jni, mop);
    jobject block = mc::world_get_block(jni, ctx->world, hit.x, hit.y, hit.z);
    const int32_t hit_id = (block != nullptr) ? mc::block_get_id(jni, block) : -1;

    if (g_bed_breaker.locked) {
        // Only the locked block (or the other half of the same bed) may be mined until it is gone.
        const bool on_locked = same_pos(hit, g_bed_breaker.locked_block) ||
                               (g_bed_breaker.locked_id == kBlockBed && hit_id == kBlockBed &&
                                is_same_bed(hit, g_bed_breaker.locked_block));
        if (!on_locked) {
            if (++g_bed_breaker.lock_miss_ticks > kLockMissTicks) {
                unlock_target(); // something moved in the way: re-plan next tick
            }
            release_attack(ctx);
            return;
        }
        g_bed_breaker.lock_miss_ticks = 0;
    } else {
        const bool on_bed = hit_id == kBlockBed && is_same_bed(hit, g_bed_breaker.bed);
        const bool on_cover =
            g_bed_breaker.mine_is_cover && hit_id > 0 && same_pos(hit, g_bed_breaker.mine_target);
        if (!on_bed && !on_cover) {
            release_attack(ctx);
            return;
        }
        // Start mining this block: lock it and freeze the aim point until it is broken.
        g_bed_breaker.locked = true;
        g_bed_breaker.locked_block = hit;
        g_bed_breaker.locked_id = hit_id;
        g_bed_breaker.locked_aim = g_bed_breaker.aim_point;
        g_bed_breaker.lock_miss_ticks = 0;
    }

    if (g_bed_breaker.swap_tool.value) {
        swap_to_best_tool(ctx, block);
    }

    // Built-in Fast Mine for the bed / cover block (the Fast Mine module yields while we own the mining).
    if (g_bed_breaker.fast_mine.value) {
        jobject controller = mc::mc_get_player_controller(jni, ctx->minecraft);
        if (controller == nullptr) {
            fast_mine_core::reset(g_bed_breaker.fm_core);
        } else {
            fast_mine_core::pre_tick(jni, controller, fast_mine_params(), g_bed_breaker.fm_core);
        }
    }

    press_attack(ctx);
}

// Undoes every side effect (mouse button, hotbar slot) when the module is toggled.
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_bed_breaker.base) {
        return;
    }

    if (!ev->enabled && ev->ctx != nullptr) {
        release_attack(ev->ctx);
        restore_tool(ev->ctx);
    }
    clear_target();
    g_bed_breaker.aim_active = false;
    g_bed_breaker.spoof_rotation_active = false;
    g_bed_breaker.rescan_cooldown = 0;
    g_bed_breaker.attack_overridden = false;
    g_bed_breaker.saved_prev_slot = -1;
    g_bed_breaker.selected_tool_slot = -1;
    g_bed_breaker.lost_ticks = 0;
    fast_mine_core::reset(g_bed_breaker.fm_core);
}

// Silent rotation + WASD move-fix while aiming at a bed.
void on_rotation_spoof_query(void* event) noexcept {
    if (g_bed_breaker.spoof_rotation_active) {
        auto* ev = static_cast<events::RotationSpoofEvent*>(event);
        ev->active = true;
        ev->move_fix = true;
    }
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_bed_breaker.range.render(gui, renderer);
    g_bed_breaker.fov.render(gui, renderer);
    g_bed_breaker.break_cover.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Aim");
    g_bed_breaker.aim_speed.render(gui, renderer);
    g_bed_breaker.randomize.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Tool");
    g_bed_breaker.swap_tool.render(gui, renderer);
    if (g_bed_breaker.swap_tool.value) {
        g_bed_breaker.switch_back.render(gui, renderer);
    }

    gui::widgets::render_section_header(gui, renderer, "Fast mine");
    g_bed_breaker.fast_mine.render(gui, renderer);
    if (g_bed_breaker.fast_mine.value) {
        g_bed_breaker.fm_speed.render(gui, renderer);
        g_bed_breaker.fm_break_delay.render(gui, renderer);
        g_bed_breaker.fm_decr_block_hit_delay.render(gui, renderer);
    }
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_bed_breaker.range.visit(archive, "range");
    g_bed_breaker.fov.visit(archive, "fov");
    g_bed_breaker.break_cover.visit(archive, "break_cover");
    g_bed_breaker.aim_speed.visit(archive, "aim_speed");
    g_bed_breaker.randomize.visit(archive, "randomize");
    g_bed_breaker.swap_tool.visit(archive, "swap_tool");
    g_bed_breaker.switch_back.visit(archive, "switch_back");
    g_bed_breaker.fast_mine.visit(archive, "fast_mine");
    g_bed_breaker.fm_speed.visit(archive, "fast_mine_speed");
    g_bed_breaker.fm_break_delay.visit(archive, "fast_mine_break_delay");
    g_bed_breaker.fm_decr_block_hit_delay.visit(archive, "fast_mine_decr_block_hit_delay");
}

} // namespace

events::Module* get_bed_breaker_module() noexcept {
    return &g_bed_breaker.base;
}

bool bed_breaker_owns_fast_mine() noexcept {
    return events::module_is_running(&g_bed_breaker.base) && g_bed_breaker.fast_mine.value &&
           g_bed_breaker.aim_active;
}

} // namespace slinky::modules
