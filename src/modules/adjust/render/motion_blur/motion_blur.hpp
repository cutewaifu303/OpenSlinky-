#pragma once

// Adjust "Motion Blur" (com.adjustclient.module.impl.render.motionblur: MotionBlurModule, MotionBlurMode,
// MotionBlurRenderer, MonkeyBlurShader, DepthCapture, MoulberryBlur) for OpenSlinky++.
//
// Adjust drives MotionBlurRenderer from its patched EntityRenderer / RenderManager. OpenSlinky++ gets the same
// call-ins from the bytecode hooks in src/adjust/hooks/world_fx_hooks.cpp:
//   * EntityRenderer.updateCameraAndRender: renderWorld(...) is replaced ->
//       adjust_motion_blur_begin_frame() (MotionBlurRenderer.beginFrame), the original renderWorld, then
//       adjust_motion_blur_end_frame() (renderMonkey at the end of the world pass + renderPost).
//   * EntityRenderer.renderWorldPass: setupCameraTransform(...) is replaced -> original, then
//       adjust_motion_blur_capture_matrices() (MotionBlurRenderer.captureMatrices, Monkey mode).
//   * RenderGlobal.renderEntities: RenderManager.renderEntitySimple(...) is replaced -> entity mask around the
//       original (RenderManager.renderEntitySimple patch, Monkey mode keeps the local player sharp).
//
// The blur uses its own GL resources (GLSL programs, textures, framebuffers; GL 3.0 via glad) instead of
// Minecraft's ShaderGroup; all GL state it touches is saved and restored.
//
// Only available while the "Adjust" visual style is selected (kModuleFlagStyleAdjust).

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_motion_blur_module() noexcept;

// MotionBlurRenderer.beginFrame(). Call on the render thread right before EntityRenderer.renderWorld.
void adjust_motion_blur_begin_frame(events::JniContext* jni) noexcept;

// MotionBlurRenderer.renderMonkey() + renderPost(). Call right after EntityRenderer.renderWorld returned (only
// when no Java exception is pending). Uses raw GL only.
void adjust_motion_blur_end_frame() noexcept;

// True while Monkey mode is active this frame (cheap; no JNI).
bool adjust_motion_blur_wants_matrices() noexcept;

// MotionBlurRenderer.captureMatrices(partialTicks). Call right after EntityRenderer.setupCameraTransform.
void adjust_motion_blur_capture_matrices(events::JniContext* jni, float partial_ticks) noexcept;

// True while an entity mask could be captured (Monkey mode active, matrices captured this frame; no JNI).
bool adjust_motion_blur_mask_possible() noexcept;

// MotionBlurRenderer.beginEntityMask(entity). Returns true when the mask capture started; the caller must then call
// adjust_motion_blur_end_entity_mask() after rendering the entity (also when it threw).
bool adjust_motion_blur_begin_entity_mask(events::JniContext* jni, jobject entity) noexcept;

// DepthCapture.end(). Raw GL only (safe with a pending Java exception).
void adjust_motion_blur_end_entity_mask() noexcept;

} // namespace slinky::modules
