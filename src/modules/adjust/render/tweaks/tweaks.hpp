#pragma once

// Adjust "Tweaks" (com.adjustclient.module.impl.render.tweaks.*: TweaksModule, Tweak, ScoreboardTweak, ChatTweak
// (+ ChatLineKey, ChatStackEntry, StackedChatComponent), BowTweak, TabOverlayTweak, ZoomTweak, NoRenderTweak,
// SmoothRotationYawTweak, MinimalBobbingTweak, DisconnectConfirmTweak, NoHotbarScrollTweak, FullscreenTweak).
//
// The module owns the settings and the tick-driven parts (Bow, No lobby messages, fullscreen refresh, disconnect
// timer). Everything Minecraft has to ask about lives in src/adjust/hooks/tweaks_hooks.cpp, which queries the
// functions below. Every query returns false (vanilla behaviour) while the module is not running.

#include <cstdint>
#include <jni.h>

#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_tweaks_module() noexcept;

namespace adjust_tweaks {

// ScoreboardTweak (GuiIngame.renderScoreboard)
bool scoreboard_hidden() noexcept;        // "No scoreboard"
bool scoreboard_transparent() noexcept;   // "Transparent"
bool scoreboard_remove_scores() noexcept; // "Remove scores"

// ChatTweak (GuiNewChat)
bool chat_transparent() noexcept;      // GuiPersistentChat.drawChat background
bool chat_player_heads() noexcept;     // ChatTweak.isStackingEnabled() (sic: gates the player heads)
bool chat_stacking() noexcept;         // stackingEnabled
int64_t chat_stack_timeout_ms() noexcept;
// Bumped whenever Adjust would rebuild its stack map (enable / disable / stacking toggled / world reset).
uint32_t chat_reset_generation() noexcept;

// NoRenderTweak
bool hide_title() noexcept;
bool hide_boss_bar() noexcept;
bool hide_achievements() noexcept;

// TabOverlayTweak
bool tab_remove_npcs() noexcept;

// SmoothRotationYawTweak / MinimalBobbingTweak / NoHotbarScrollTweak / FullscreenTweak
bool smooth_rotation_yaw() noexcept;
bool minimal_bobbing() noexcept;
bool no_hotbar_scroll() noexcept;
bool borderless_fullscreen() noexcept;
// Reported by the Display.setFullscreen hook: true while the window is the borderless fullscreen window.
void set_borderless_applied(bool applied) noexcept;

// DisconnectConfirmTweak: called for GuiIngameMenu button 1 ("Disconnect"). True = swallow this click
// (first click, or more than 1.5 s since the last one).
bool disconnect_confirm_swallow(JNIEnv* env, jobject menu) noexcept;

// ZoomTweak (needs OptiFine's zoom key, GameSettings.ofKeyBindZoom)
bool zoom_enabled() noexcept;         // module && (No cinematic || Scroll zoom)
bool zoom_no_cinematic() noexcept;
bool zoom_scroll() noexcept;
float zoom_factor() noexcept;         // ZoomTweak.getZoom()
void zoom_on_wheel(int delta) noexcept; // ZoomTweak.updateZoom (zoom key / screen checks done by the caller)
void zoom_reset() noexcept;           // ZoomTweak.resetZoom

} // namespace adjust_tweaks
} // namespace slinky::modules
