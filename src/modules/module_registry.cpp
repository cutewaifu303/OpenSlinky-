#include "modules/module_registry.hpp"

#include "modules/combat/aim_assist.hpp"
#include "modules/utility/anti_bot.hpp"
#include "modules/visual/anti_debuff.hpp"
#include "modules/combat/auto_block.hpp"
#include "modules/combat/auto_clicker.hpp"
#include "modules/utility/auto_pot.hpp"
#include "modules/utility/auto_rod.hpp"
#include "modules/utility/auto_soup.hpp"
#include "modules/world/auto_tool.hpp"
#include "modules/combat/auto_weapon.hpp"
#include "modules/combat/backtrack.hpp"
#include "modules/world/bed_breaker.hpp"
#include "modules/utility/blink.hpp"
#include "modules/visual/block_esp.hpp"
#include "modules/world/block_in.hpp"
#include "modules/world/bridge.hpp"
#include "modules/world/bridge_assist.hpp"
#include "modules/visual/chams.hpp"
#include "modules/visual/chest_esp.hpp"
#include "modules/utility/chest_stealer.hpp"
#include "modules/world/clutch.hpp"
#include "modules/combat/criticals.hpp"
#include "modules/utility/debug.hpp"
#include "modules/utility/fake_lag.hpp"
#include "modules/movement/fast_accel.hpp"
#include "modules/world/fast_mine.hpp"
#include "modules/world/fast_place.hpp"
#include "modules/visual/free_look.hpp"
#include "modules/utility/friends.hpp"
#include "modules/world/ghost_hand.hpp"
#include "modules/visual/gui.hpp"
#include "modules/combat/hitboxes.hpp"
#include "modules/combat/hit_select.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/visual/indicators.hpp"
#include "modules/movement/instant_stop.hpp"
#include "modules/utility/inv_manager.hpp"
#include "modules/movement/inv_walk.hpp"
#include "modules/visual/item_esp.hpp"
#include "modules/utility/item_use_fix.hpp"
#include "modules/movement/keep_sprint.hpp"
#include "modules/combat/knockback_delay.hpp"
#include "modules/combat/knockback_displace.hpp"
#include "modules/combat/lag_range.hpp"
#include "modules/visual/mod_overlay.hpp"
#include "modules/visual/name_tags.hpp"
#include "modules/utility/no_hit_delay.hpp"
#include "modules/visual/no_hurt_cam.hpp"
#include "modules/utility/no_item_release.hpp"
#include "modules/movement/no_jump_delay.hpp"
#include "modules/movement/no_slow.hpp"
#include "modules/visual/notifications.hpp"
#include "modules/utility/no_use_delay.hpp"
#include "modules/movement/null_move.hpp"
#include "modules/combat/piercing.hpp"
#include "modules/utility/ping_fix.hpp"
#include "modules/visual/player_esp.hpp"
#include "modules/visual/pointers.hpp"
#include "modules/combat/reach.hpp"
#include "modules/utility/refill.hpp"
#include "modules/movement/safe_walk.hpp"
#include "modules/movement/sprint.hpp"
#include "modules/combat/sprint_reset.hpp"
#include "modules/movement/strafe.hpp"
#include "modules/utility/teams.hpp"
#include "modules/utility/timer.hpp"
#include "modules/combat/timer_range.hpp"
#include "modules/visual/trajectories.hpp"
#include "modules/combat/velocity.hpp"
#include "modules/visual/watermark.hpp"
#include "modules/combat/weapons.hpp"
#include "modules/adjust/render/barrier_reveal/barrier_reveal.hpp"
#include "modules/adjust/render/block_places/block_places.hpp"
#include "modules/adjust/render/crosshair/crosshair.hpp"
#include "modules/adjust/render/damage_particles/damage_particles.hpp"
#include "modules/adjust/render/more_particles/more_particles.hpp"
#include "modules/adjust/render/streamer/streamer.hpp"
#include "modules/adjust/render/trails/trails.hpp"
#include "modules/adjust/render/true_sight/true_sight.hpp"
#include "modules/adjust/render/bedplates/bedplates.hpp"
#include "modules/adjust/render/block_overlay/block_overlay.hpp"
#include "modules/adjust/render/indicators/indicators.hpp"
#include "modules/adjust/render/trajectories/trajectories.hpp"
#include "modules/adjust/render/chams/adjust_chams.hpp"
#include "modules/adjust/render/esp/adjust_esp.hpp"
#include "modules/adjust/render/skeleton/adjust_skeleton.hpp"
#include "modules/visual/stats_overlay.hpp"
#include "modules/adjust/render/hurtcam/hurtcam.hpp"
#include "modules/adjust/mainmenu/main_menu.hpp"
#include "modules/adjust/render/item_animations/item_animations.hpp"
#include "modules/adjust/render/item_physics/item_physics.hpp"
#include "modules/adjust/render/motion_blur/motion_blur.hpp"
#include "modules/adjust/render/atmosphere/atmosphere.hpp"
#include "modules/adjust/render/third_person/third_person.hpp"
#include "modules/adjust/render/freecam/freecam.hpp"
#include "modules/adjust/render/tweaks/tweaks.hpp"
#include "modules/adjust/clickgui/adjust_click_gui.hpp"

namespace slinky::modules {

events::Module* const* get_module_registry() noexcept {
    static events::Module* const s_registry[kModuleRegistryCount] = {
        get_aim_assist_module(),              // [ 0] Aim Assist
        get_anti_bot_module(),                // [ 1] Anti Bot
        get_anti_debuff_module(),             // [ 2] Anti Debuff
        get_adjust_atmosphere_module(),       // [ 3] Atmosphere (Adjust)
        get_auto_block_module(),              // [ 4] Auto Block
        get_auto_clicker_module(),            // [ 5] Auto Clicker
        get_auto_pot_module(),                // [ 6] Auto Pot
        get_auto_rod_module(),                // [ 7] Auto Rod
        get_auto_soup_module(),               // [ 8] Auto Soup
        get_auto_tool_module(),               // [ 9] Auto Tool
        get_auto_weapon_module(),             // [10] Auto Weapon
        get_backtrack_module(),               // [11] Backtrack
        get_adjust_barrier_reveal_module(),   // [12] Barrier Reveal (Adjust)
        get_bed_breaker_module(),             // [13] Bed Breaker
        get_adjust_bedplates_module(),        // [14] Bed Plates (Adjust)
        get_blink_module(),                   // [15] Blink
        get_block_esp_module(),               // [16] Block ESP
        get_block_in_module(),                // [17] Block In
        get_adjust_block_overlay_module(),    // [18] Block Overlay (Adjust)
        get_adjust_block_places_module(),     // [19] Block Places (Adjust)
        get_bridge_module(),                  // [20] Bridge
        get_bridge_assist_module(),           // [21] Bridge Assist
        get_chams_module(),                   // [22] Chams
        get_adjust_chams_module(),            // [23] Chams (Adjust)
        get_chest_esp_module(),               // [24] Chest ESP
        get_chest_stealer_module(),           // [25] Chest Stealer
        get_adjust_click_gui_module(),        // [26] Click GUI (Adjust)
        get_clutch_module(),                  // [27] Clutch
        get_criticals_module(),               // [28] Criticals
        get_adjust_crosshair_module(),        // [29] Crosshair (Adjust)
        get_adjust_damage_particles_module(), // [30] Damage Particles (Adjust)
        get_debug_module(),                   // [31] Debug
        get_adjust_esp_module(),              // [32] ESP (Adjust)
        get_fake_lag_module(),                // [33] Fake Lag
        get_fast_accel_module(),              // [34] Fast Accel
        get_fast_mine_module(),               // [35] Fast Mine
        get_fast_place_module(),              // [36] Fast Place
        get_adjust_freecam_module(),          // [37] Freecam (Adjust)
        get_free_look_module(),               // [38] Free Look
        get_friends_module(),                 // [39] Friends
        get_ghost_hand_module(),              // [40] Ghost Hand
        get_gui_module(),                     // [41] GUI
        get_hitboxes_module(),                // [42] Hitboxes
        get_hit_select_module(),              // [43] Hit Select
        get_adjust_hud_module(),              // [44] HUD (Adjust)
        get_adjust_hurtcam_module(),          // [45] Hurtcam (Adjust)
        get_indicators_module(),              // [46] Indicators
        get_adjust_indicators_module(),       // [47] Indicators (Adjust)
        get_instant_stop_module(),            // [48] Instant Stop
        get_inv_manager_module(),             // [49] Inv Manager
        get_inv_walk_module(),                // [50] Inv Walk
        get_adjust_item_animations_module(),  // [51] Item Animations (Adjust)
        get_item_esp_module(),                // [52] Item ESP
        get_adjust_item_physics_module(),     // [53] Item Physics (Adjust)
        get_item_use_fix_module(),            // [54] Item Use Fix
        get_keep_sprint_module(),             // [55] Keep Sprint
        get_knockback_delay_module(),         // [56] Knockback Delay
        get_knockback_displace_module(),      // [57] Knockback Displace
        get_lag_range_module(),               // [58] Lag Range
        get_adjust_main_menu_module(),        // [59] Main Menu (Adjust)
        get_mod_overlay_module(),             // [60] Mod Overlay
        get_adjust_more_particles_module(),   // [61] More Particles (Adjust)
        get_adjust_motion_blur_module(),      // [62] Motion Blur (Adjust)
        get_name_tags_module(),               // [63] Name Tags
        get_no_hit_delay_module(),            // [64] No Hit Delay
        get_no_hurt_cam_module(),             // [65] No Hurt Cam
        get_no_item_release_module(),         // [66] No Item Release
        get_no_jump_delay_module(),           // [67] No Jump Delay
        get_no_slow_module(),                 // [68] No Slow
        get_notifications_module(),           // [69] Notifications
        get_no_use_delay_module(),            // [70] No Use Delay
        get_null_move_module(),               // [71] Null Move
        get_stats_overlay_module(),           // [72] Overlay
        get_piercing_module(),                // [73] Piercing
        get_ping_fix_module(),                // [74] Ping Fix
        get_player_esp_module(),              // [75] Player ESP
        get_pointers_module(),                // [76] Pointers
        get_reach_module(),                   // [77] Reach
        get_refill_module(),                  // [78] Refill
        get_safe_walk_module(),               // [79] Safe Walk
        get_adjust_skeleton_module(),         // [80] Skeleton (Adjust)
        get_sprint_module(),                  // [81] Sprint
        get_sprint_reset_module(),            // [82] Sprint Reset
        get_strafe_module(),                  // [83] Strafe
        get_adjust_streamer_module(),         // [84] Streamer (Adjust)
        get_teams_module(),                   // [85] Teams
        get_adjust_third_person_module(),     // [86] Third Person (Adjust)
        get_timer_module(),                   // [87] Timer
        get_timer_range_module(),             // [88] Timer Range
        get_adjust_trails_module(),           // [89] Trails (Adjust)
        get_trajectories_module(),            // [90] Trajectories
        get_adjust_trajectories_module(),     // [91] Trajectories (Adjust)
        get_adjust_true_sight_module(),       // [92] True Sight (Adjust)
        get_adjust_tweaks_module(),           // [93] Tweaks (Adjust)
        get_velocity_module(),                // [94] Velocity
        get_watermark_module(),               // [95] Watermark
        get_weapons_module(),                 // [96] Weapons
    };
    return s_registry;
}

} // namespace slinky::modules
