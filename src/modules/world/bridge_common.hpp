#pragma once
// Helpers shared by Bridge and Bridge Assist (GodBridge axis lock + steering, Vape edge detection).

#include <cmath>
#include <cstdint>

#include "events/events.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "util/math.hpp"

namespace slinky::modules::bridge_common {

// Yaw of the movement frame (what WASD is relative to). Free Look locks it, otherwise it is your view;
// never the rotation a module aims with, so bridge directions cannot feed back on themselves.
inline float frame_yaw(events::JniContext* jni, jobject player) noexcept {
    return mc::is_rotation_spoof_active() ? mc::get_target_spoof_rotation().x
                                          : mc::entity_get_rotation(jni, player).x;
}

// Snaps a yaw to the nearest cardinal axis; returns that axis' yaw and its world direction.
inline float snap_cardinal(float yaw, double* out_dx, double* out_dz) noexcept {
    const float snapped = std::round(yaw / 90.0f) * 90.0f;
    const double rad = static_cast<double>(snapped) * math::kPi / 180.0;
    *out_dx = std::round(-std::sin(rad));
    *out_dz = std::round(std::cos(rad));
    return snapped;
}

// Movement input (forward/strafe) that makes moveFlying walk toward world direction (dx,dz) for the given
// yaw. Inverse of Minecraft's moveFlying rotation.
inline void world_dir_to_input(double dx, double dz, float yaw_deg, float* out_forward, float* out_strafe) noexcept {
    const double len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-6) {
        *out_forward = 0.0f;
        *out_strafe = 0.0f;
        return;
    }
    dx /= len;
    dz /= len;
    const double yaw = static_cast<double>(yaw_deg) * math::kPi / 180.0;
    const double s = std::sin(yaw);
    const double c = std::cos(yaw);
    *out_strafe = static_cast<float>(c * dx + s * dz);
    *out_forward = static_cast<float>(-s * dx + c * dz);
}

// Re-points a moveWithHeading input along world direction (dx,dz), keeping the player's own input strength
// (no input = no movement, sneak speed kept). Uses the aim yaw moveFlying actually rotates with.
inline void steer_move_input(events::MoveWithHeadingEvent* ev, double dx, double dz) noexcept {
    const float magnitude = std::sqrt(ev->move_strafe * ev->move_strafe + ev->move_forward * ev->move_forward);
    if (magnitude < 1e-4f) {
        return;
    }
    float forward = 0.0f;
    float strafe = 0.0f;
    world_dir_to_input(dx, dz, mc::entity_get_rotation(ev->ctx->jni, ev->ctx->local_player).x, &forward, &strafe);
    ev->move_forward = forward * magnitude;
    ev->move_strafe = strafe * magnitude;
}

// Vape ScaffoldEdgeSneakHelper: predicts the next-tick hitbox (inset 0.2) and returns true when every
// supporting block beneath it is air.
inline bool over_air_edge_vape(events::JniContext* jni, jobject world, jobject player) noexcept {
    const math::AABB bbox = mc::aabb_to_aabb(jni, mc::entity_get_bounding_box(jni, player));
    const math::Vec3d motion = mc::entity_get_motion(jni, player);
    const double cx = (bbox.min.x + bbox.max.x) * 0.5;
    const double cz = (bbox.min.z + bbox.max.z) * 0.5;
    const double half_x = (bbox.max.x - bbox.min.x) * 0.5;
    const double half_z = (bbox.max.z - bbox.min.z) * 0.5;
    constexpr double kInset = 0.2;
    const int32_t y = static_cast<int32_t>(std::floor(bbox.min.y - 1.0));
    const int32_t min_x = static_cast<int32_t>(std::floor(cx + motion.x - half_x + kInset));
    const int32_t max_x = static_cast<int32_t>(std::floor(cx + motion.x + half_x - kInset));
    const int32_t min_z = static_cast<int32_t>(std::floor(cz + motion.z - half_z + kInset));
    const int32_t max_z = static_cast<int32_t>(std::floor(cz + motion.z + half_z - kInset));
    for (int32_t x = min_x; x <= max_x; ++x) {
        for (int32_t z = min_z; z <= max_z; ++z) {
            if (mc::block_get_id(jni, mc::world_get_block(jni, world, x, y, z)) != 0) {
                return false;
            }
        }
    }
    return true;
}

} // namespace slinky::modules::bridge_common
