#pragma once

// Adjust "Atmosphere" (com.adjustclient.module.impl.render.AtmosphereModule + AtmosphereFogCondition,
// AtmosphereTimeCondition, FogMode, LightingMode).
//
// Overrides the world time seen by the renderer, the lightmap (Bright gamma / Custom colour / no night vision) and
// the fog (colour blend + density modes). Adjust posts LightmapStateEvent / GammaEvent / FogRgbEvent /
// FogParametersEvent from its patched EntityRenderer and reads the time in its patched World; OpenSlinky++ gets the
// same call-ins from the bytecode hooks in src/adjust/hooks/world_fx_hooks.cpp, which call the functions below.
// Every function returns false (= leave vanilla alone) while the module is not running.
//
// Only available while the "Adjust" visual style is selected (kModuleFlagStyleAdjust).

#include <cstdint>

#include "adjust/kit.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_atmosphere_module() noexcept;

// World.getEffectiveWorldTime(): Time > Enabled -> the configured time (ticks).
bool adjust_atmosphere_time_override(int64_t* out_time) noexcept;

// LightmapStateEvent.nightVision = false / EntityRenderer.updateFogColor night vision skip ("No Night Vision").
bool adjust_atmosphere_disables_night_vision() noexcept;

// LightmapStateEvent.customColors = false (OptiFine custom lightmaps) while Lighting != Default.
bool adjust_atmosphere_disables_custom_colors() noexcept;

// GammaEvent: Lighting = Bright -> gamma from "Brightness".
bool adjust_atmosphere_gamma(float* out_gamma) noexcept;

// Lighting = Custom: every lightmap texel becomes this colour (alpha 255).
bool adjust_atmosphere_lighting_color(adjust::Argb* out_color) noexcept;

// FogRgbEvent: blends the fog colour towards "Color" by "Color Strength". Modifies in place.
bool adjust_atmosphere_fog_color(float* red, float* green, float* blue) noexcept;

// FogParametersEvent (fields as in Adjust). mode = GL_LINEAR (9729) / GL_EXP (2048).
struct AtmosphereFogParams {
    float far_plane_distance = 0.0f;
    float density = 0.0f;
    float start = 0.0f;
    float end = 0.0f;
    int32_t mode = 9729;
    bool radial = false;
};

// Module running and Fog > Enabled (EntityRenderer.applyFogParameters forces fogStandard = false then).
bool adjust_atmosphere_fog_enabled() noexcept;

// AtmosphereModule.onFog. Returns true when it changed `params`.
bool adjust_atmosphere_apply_fog(AtmosphereFogParams* params) noexcept;

} // namespace slinky::modules
