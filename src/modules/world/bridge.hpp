#pragma once
#include "events/events.hpp"
#include "events/module.hpp"
#include "util/math.hpp"

namespace slinky::modules {
events::Module* get_bridge_module() noexcept;

// True while Bridge wants vanilla's sneak ledge-clip applied to the local player (queried by
// hook_should_clip_ledge, same path Safe Walk uses). Stops you at the edge in the same move.
bool bridge_wants_ledge_clip() noexcept;

// Finds a humanized aim point on a block face that is visible from `eye` (raytrace-verified) and would
// place a block into an empty cell at height `place_y` under the player (centre, next-tick centre or a
// hitbox corner). Shared with Bridge Assist's GodBridge / TellyBridge modes.
bool bridge_find_place_aim(
    events::GameContext* ctx,
    const math::Vec3d& eye,
    math::Vec2f cur_rot,
    int32_t place_y,
    bool randomize,
    math::Vec3d* out_aim) noexcept;
} // namespace slinky::modules
