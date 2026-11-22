#include "modules/world/fast_mine_core.hpp"

#include <cmath>

#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules::fast_mine_core {

// Caps blockHitDelay and records pre-tick block damage (sub_180062E90)
void pre_tick(events::JniContext* jni, jobject controller, const Params& params, State& state) noexcept {
    state.pre_tick_block_damage = minecraft::controller_get_cur_block_damage(jni, controller);

    const int32_t hit_delay = minecraft::controller_get_block_hit_delay(jni, controller);
    if (static_cast<float>(hit_delay) > params.break_delay) {
        minecraft::controller_set_block_hit_delay(jni, controller, static_cast<int32_t>(params.break_delay));
    } else if (hit_delay > 0 && params.decr_block_hit_delay && hit_delay == state.prev_block_hit_delay) {
        minecraft::controller_set_block_hit_delay(jni, controller, hit_delay - 1);
    }
    state.prev_block_hit_delay = hit_delay;
}

// Advances initial curBlockDamageMP to achieve exact ceil-quantized tick speedup (sub_180062FD0)
void post_living(events::JniContext* jni, jobject controller, const Params& params, const State& state) noexcept {
    const float cur_damage = minecraft::controller_get_cur_block_damage(jni, controller);
    if (cur_damage == 0.0f || state.pre_tick_block_damage != 0.0f || params.speed <= 0.0f) {
        return;
    }
    const auto vanilla_ticks = static_cast<int64_t>(std::ceil(1.0f / cur_damage));
    const auto boosted_ticks_f = static_cast<float>(
        (1.0 / static_cast<double>(params.speed)) / static_cast<double>(cur_damage));
    const auto boosted_ticks = static_cast<int64_t>(std::ceil(boosted_ticks_f));
    const int32_t multiplier_ticks = static_cast<int32_t>(vanilla_ticks) - static_cast<int32_t>(boosted_ticks) + 1;
    minecraft::controller_set_cur_block_damage(jni, controller, static_cast<float>(multiplier_ticks) * cur_damage);
}

} // namespace slinky::modules::fast_mine_core
