#pragma once

// Adjust camera hooks (Third Person + Freecam), Minecraft 1.8.9.
//
// camera_hooks() lists the extra bytecode hooks (natives in camera_hooks.cpp). The remaining camera behaviour sits
// on instructions the base hook table already owns, so the existing handlers in src/hooks/jni_hooks.cpp call the
// bridge functions below:
//
//   hook_orient_camera (EntityRenderer.setupCameraTransform -> orientCamera), around EVERY
//   `minecraft::entity_renderer_orient_camera(...)` call (both the spoof and the plain branch, inside the spoof
//   swap):
//       adjust::hooks::CameraOrientState cam{};
//       adjust::hooks::camera_orient_begin(&jni, entity_renderer, &cam);
//       minecraft::entity_renderer_orient_camera(&jni, entity_renderer, partial_ticks);
//       adjust::hooks::camera_orient_end(&jni, entity_renderer, &cam);
//
//   hook_update_active_render_info (renderWorldPass -> ActiveRenderInfo.updateRenderInfo), around EVERY
//   `minecraft::active_render_info_update(...)` call (inside the spoof rotation swap):
//       adjust::hooks::CameraRenderInfoState info{};
//       const bool front = adjust::hooks::camera_render_info_begin(&jni, player, third_person_front != JNI_FALSE,
//                                                                  &info);
//       minecraft::active_render_info_update(&jni, player, front);
//       adjust::hooks::camera_render_info_end(&jni, player, &info);
//
//   hook_set_render_x / hook_set_render_y (RenderManager.cacheActiveRenderInfo putfield playerViewX / playerViewY):
//   build the JniContext first, then
//       float view_x = angle_x;
//       if (!adjust::hooks::camera_override_player_view_x(&jni, &view_x) && minecraft::is_rotation_spoof_active()) {
//           view_x = minecraft::get_server_spoof_rotation().y;
//       }
//   (same for Y with camera_override_player_view_y and .x).

#include <jni.h>

#include "bytecode/extra_hooks.hpp"
#include "events/events.hpp"
#include "util/math.hpp"

namespace slinky::adjust::hooks {

// Domain hook list (nullptr-terminated).
const bytecode::ExtraHookSpec* const* camera_hooks() noexcept;

// ---- orientCamera -----------------------------------------------------------------------------------------------

struct CameraOrientState {
    bool distance_applied = false;
    float saved_distance = 0.0f;      // EntityRenderer.thirdPersonDistance
    float saved_distance_temp = 0.0f; // EntityRenderer.thirdPersonDistanceTemp
    bool rotation_applied = false;
    jobject entity = nullptr;         // local ref of the entity whose rotation was replaced
    math::Vec2f saved_rotation{};
    math::Vec2f saved_prev_rotation{};
};

// Third Person: adds Distance to the vanilla third person distance and, while free looking (view entity = local
// player), renders with the free look rotation (Adjust's patched EntityRenderer.orientCamera).
void camera_orient_begin(events::JniContext* jni, jobject entity_renderer, CameraOrientState* state) noexcept;
void camera_orient_end(events::JniContext* jni, jobject entity_renderer, CameraOrientState* state) noexcept;

// ---- ActiveRenderInfo.updateRenderInfo ----------------------------------------------------------------------------

struct CameraRenderInfoState {
    bool rotation_applied = false;
    math::Vec2f saved_rotation{};
    math::Vec2f saved_prev_rotation{};
};

// Vanilla passes mc.thePlayer: Freecam substitutes the camera rotation, free look the free look rotation (and
// forces the non-mirrored variant, as Adjust's `thirdPersonView == 2 && !freeLooking`). Returns the boolean to pass.
bool camera_render_info_begin(events::JniContext* jni, jobject player, bool third_person_front,
                              CameraRenderInfoState* state) noexcept;
void camera_render_info_end(events::JniContext* jni, jobject player, const CameraRenderInfoState* state) noexcept;

// ---- RenderManager.playerViewX / playerViewY ----------------------------------------------------------------------

// True if the camera modules own the value (the caller must then skip its rotation-spoof override and store
// *inout): Freecam keeps vanilla's value (computed from the camera), free look substitutes its pitch / yaw
// (Adjust's patched RenderManager.cacheActiveRenderInfo, no +180 for the front view while free looking).
bool camera_override_player_view_x(events::JniContext* jni, float* inout_pitch) noexcept;
bool camera_override_player_view_y(events::JniContext* jni, float* inout_yaw) noexcept;

} // namespace slinky::adjust::hooks
