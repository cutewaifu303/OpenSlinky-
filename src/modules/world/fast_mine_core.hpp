#pragma once

// Shared Fast Mine algorithm (originally sub_180062E90 / sub_180062FD0), used by the Fast Mine module and by
// Bed Breaker. It only adjusts the vanilla PlayerControllerMP fields, vanilla still does the actual mining:
//
//   pre_tick (kEventGameTick, before vanilla mines this tick):
//       caps blockHitDelay (the cooldown between two broken blocks) at `break_delay` ticks - optionally counting a
//       stuck delay down by one - and records curBlockDamageMP before vanilla advances it.
//   post_living (kEventPostLivingUpdate, after vanilla advanced the progress):
//       on the first damage tick of a block (progress went 0 -> >0) it multiplies the progress so that the block
//       breaks after exactly ceil((1/speed) / damage_per_tick) ticks instead of ceil(1 / damage_per_tick).

#include <cstdint>
#include <jni.h>

#include "events/events.hpp"

namespace slinky::modules::fast_mine_core {

struct Params {
    float speed = 1.0f;                // 1.0 = vanilla, 2.0 = half the ticks
    float break_delay = 5.0f;          // max blockHitDelay in ticks (vanilla sets 5 after each break)
    bool decr_block_hit_delay = false; // count a stuck delay down by one per tick
};

struct State {
    int32_t prev_block_hit_delay = 0;
    float pre_tick_block_damage = -1.0f; // -1 = not eligible this tick (no boost)
};

// Call in kEventGameTick when the block under the crosshair is eligible.
void pre_tick(events::JniContext* jni, jobject controller, const Params& params, State& state) noexcept;

// Call in kEventGameTick when nothing eligible is being mined (disables the boost for this tick).
inline void skip_tick(State& state) noexcept {
    state.pre_tick_block_damage = -1.0f;
}

// Call when there is no player controller.
inline void reset(State& state) noexcept {
    state.prev_block_hit_delay = 0;
    state.pre_tick_block_damage = -1.0f;
}

// Call in kEventPostLivingUpdate.
void post_living(events::JniContext* jni, jobject controller, const Params& params, const State& state) noexcept;

} // namespace slinky::modules::fast_mine_core
