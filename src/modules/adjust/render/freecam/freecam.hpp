#pragma once

// Adjust "Freecam" (com.adjustclient.module.impl.render.freecam.FreecamModule / FreecamCamera).
//
// The camera is a client-only EntityArmorStand (not added to the world, sized 0.6 x 1.8 so its eye height is
// 1.62) that becomes Minecraft's render view entity; Adjust uses its own `Minecraft.cameraOverride` field and
// patches every renderer to read it - the render view entity is the vanilla equivalent.
//
// Minecraft integration (Minecraft 1.8.9, natives in src/adjust/hooks/camera_hooks.cpp):
//   * mouse look turns the camera instead of the player       (EntityRenderer.updateCameraAndRender: setAngles)
//   * the player gets no movement input                       (MovementInputFromOptions: KeyBinding.isKeyDown)
//   * the player still behaves as the view entity (C03 packets with its unchanged position/rotation, movement
//     input applied)                                          (EntityPlayerSP: isCurrentViewEntity)
//   * no first person hand                                    (EntityRenderer.renderWorldPass: renderHand)
//   * hotbar and player stats keep showing the player's       (GuiIngame.renderTooltip / renderPlayerStats:
//                                                              Minecraft.getRenderViewEntity)
//   * ActiveRenderInfo / RenderManager.playerViewX/Y use the camera rotation (bridge functions in
//     src/adjust/hooks/camera_hooks.hpp, called by the existing base hooks)

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"
#include "util/math.hpp"

namespace slinky::modules {

events::Module* get_adjust_freecam_module() noexcept;

// True while the Freecam camera exists (module running). Tears a stale camera down when the module stopped
// running (e.g. visual style switched) and returns false.
bool adjust_freecam_active(events::JniContext* jni) noexcept;

// The camera entity (JNI global reference) or nullptr.
jobject adjust_freecam_camera() noexcept;

// Camera rotation (yaw = x, pitch = y); only meaningful while adjust_freecam_active().
math::Vec2f adjust_freecam_rotation(events::JniContext* jni) noexcept;

// EntityRenderer.updateMouseInput with Minecraft.cameraOverride: the vanilla mouse look
// `thePlayer.setAngles(yaw_arg, pitch_arg)` turns the camera instead. Returns true if consumed.
bool adjust_freecam_consume_mouse(events::JniContext* jni, float yaw_arg, float pitch_arg) noexcept;

} // namespace slinky::modules
