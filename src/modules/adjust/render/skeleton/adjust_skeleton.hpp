#pragma once

// Adjust "Skeleton" (com.adjustclient.module.impl.render.skeleton.SkeletonModule) for OpenSlinky++.
//
// Adjust draws the skeleton from RenderPlayer.doRender right after super.doRender, using the limb rotations the
// ModelPlayer was just animated with. OpenSlinky++ replaces exactly that super.doRender call with a native hook
// (hooks/jni_hooks.cpp hook_render_player -> modules::chams_render_living_model), so
// adjust_skeleton_render_player must be called there after the model has been rendered; the limb angles are read
// through JNI from RendererLivingEntity.getMainModel() (ModelBiped.bipedHead/RightArm/... rotateAngleX/Y/Z).
//
// Only available while the "Adjust" visual style is selected (kModuleFlagStyleAdjust).

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_skeleton_module() noexcept;

// Draws the skeleton of `entity` (an AbstractClientPlayer) after `renderer` (its RenderPlayer) rendered it this
// frame. Safe to call for every player render; does nothing when the module is off or the player is not a target.
void adjust_skeleton_render_player(
    events::JniContext* jni,
    jobject renderer,
    jobject entity,
    float partial_ticks) noexcept;

} // namespace slinky::modules
