#pragma once

// Adjust "Chams" (com.adjustclient.module.impl.render.chams.ChamsModule) for OpenSlinky++.
//
// Adjust patches RendererLivingEntity.doRender: Normal mode renders the model + layers with a huge negative
// polygon offset (visible through walls), Colored mode renders the model with a flat colour (texture-combiner
// interpolation towards the colour, optional "Through walls" pass with depth testing disabled) and the layers
// normally. OpenSlinky++ already replaces the super.doRender call inside RenderPlayer.doRender with a native hook
// (hooks/jni_hooks.cpp hook_render_player -> modules::chams_render_living_model); adjust_chams_render_living is
// meant to be called from there, so no new bytecode hook is needed for players.
//
// Only available while the "Adjust" visual style is selected (kModuleFlagStyleAdjust).

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_chams_module() noexcept;

// Renders `entity` with `renderer` (a RendererLivingEntity) in place of RendererLivingEntity.doRender(entity, x, y,
// z, entity_yaw, partial_ticks) when Adjust Chams applies to it. Returns true when it rendered the entity (the
// vanilla doRender is called inside), false when the caller must render it as usual.
bool adjust_chams_render_living(
    events::JniContext* jni,
    jobject renderer,
    jobject entity,
    double x,
    double y,
    double z,
    float entity_yaw,
    float partial_ticks) noexcept;

} // namespace slinky::modules
