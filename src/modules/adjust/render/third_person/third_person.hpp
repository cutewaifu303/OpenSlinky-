#pragma once

// Adjust "Third Person" (com.adjustclient.module.impl.render.ThirdPersonModule + ThirdPersonViewClipCondition).
//
// Free Look (hold/toggle a bind to orbit the camera around the player without turning the player), View Clip (the
// third person camera passes through blocks) and an extra third person Distance.
//
// Minecraft integration (Minecraft 1.8.9):
//   * mouse input while free looking: SlinkyHooks13.adjustCameraSetAngles (src/adjust/hooks/camera_hooks.cpp)
//   * View Clip: SlinkyHooks13.adjustCameraViewClipDistance (EntityRenderer.orientCamera)
//   * camera rotation / distance, ActiveRenderInfo and RenderManager.playerViewX/Y: the existing base hooks
//     (hooks::hook_orient_camera / hook_update_active_render_info / hook_set_render_x / hook_set_render_y) call the
//     bridge functions declared in src/adjust/hooks/camera_hooks.hpp, which read the state below.

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"
#include "util/math.hpp"

namespace slinky::modules {

events::Module* get_adjust_third_person_module() noexcept;

// ThirdPersonModule.isEnabled() && freeLooking (false while the module is not running).
bool adjust_third_person_free_looking() noexcept;

// Free look camera rotation (yaw = x, pitch = y). Only meaningful while adjust_third_person_free_looking().
math::Vec2f adjust_third_person_free_look_rotation() noexcept;

// ThirdPersonModule.distance (added to the vanilla third person distance), 0 while the module is not running.
float adjust_third_person_extra_distance() noexcept;

// ThirdPersonModule.isViewClipEnabled(): true = the third person camera ignores blocks.
bool adjust_third_person_view_clip(events::JniContext* jni) noexcept;

// ThirdPersonModule.onMouseInput(MouseDeltaEvent): while free looking, the vanilla mouse look
// `thePlayer.setAngles(yaw_arg, pitch_arg)` (EntityRenderer.updateCameraAndRender) turns the free look camera instead
// of the player. Returns true if the call was consumed (the player must then not be rotated).
bool adjust_third_person_consume_mouse(events::JniContext* jni, float yaw_arg, float pitch_arg) noexcept;

} // namespace slinky::modules
