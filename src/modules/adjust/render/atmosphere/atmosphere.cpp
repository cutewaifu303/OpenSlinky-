// Port of com.adjustclient.module.impl.render.AtmosphereModule (+ AtmosphereFogCondition, AtmosphereTimeCondition,
// FogMode, LightingMode).
//
// Adjust injection points and their OpenSlinky++ equivalents (hooks in src/adjust/hooks/world_fx_hooks.cpp):
//   * World.getCelestialAngle / getMoonPhase / getCurrentMoonPhaseFactor use getEffectiveWorldTime()
//       -> the WorldInfo.getWorldTime() (Forge: World.getWorldTime()) call inside them is replaced;
//          adjust_atmosphere_time_override(). OpenSlinky++ only overrides the client world's time (Adjust patched
//          World itself, which also changed the integrated server's day cycle in singleplayer).
//   * EntityRenderer.updateLightmap: LightmapStateEvent (customColors / nightVision), GammaEvent, Custom colour
//       -> Config.isCustomColors() (OptiFine), EntityPlayerSP.isPotionActive(), GameSettings.gammaSetting and
//          DynamicTexture.updateDynamicTexture() are replaced inside updateLightmap.
//   * EntityRenderer.updateFogColor: night-vision brightening skip + FogRgbEvent
//       -> EntityLivingBase.isPotionActive() and the final GlStateManager.clearColor() (OptiFine:
//          Shaders.setClearColor()) are replaced inside updateFogColor.
//   * EntityRenderer.setupFog / applyFogParameters: FogParametersEvent
//       -> the final GlStateManager.enableFog() inside setupFog is replaced; the native reads the fog state the
//          branch just set (GlStateManager.fogState), runs adjust_atmosphere_apply_fog() and writes it back.
//
// Port notes: Adjust's ExpandSetting groups "Time" and "Fog" are section headers here. The Time slider shows raw
// ticks (Adjust formats them as h:mm, which the OpenSlinky++ slider format string cannot express).

#include "modules/adjust/render/atmosphere/atmosphere.hpp"

#include <algorithm>

#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"

namespace slinky::modules {

namespace {

using adjust::Argb;

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

// LightingMode
constexpr const char* kLightingModes[3] = {"Default", "Bright", "Custom"};
enum class LightingMode : int32_t { Default = 0, Bright = 1, Custom = 2 };

// FogMode
constexpr const char* kFogModes[3] = {"Default", "Soft", "Constant"};
enum class FogMode : int32_t { Vanilla = 0, Soft = 1, Constant = 2 };

constexpr int32_t kGlLinear = 9729;
constexpr int32_t kGlExp = 2048;

struct AtmosphereState {
    events::Module base{};

    // "Time" (ExpandSetting, enabledPredicate = AtmosphereTimeCondition -> timeEnabled)
    config::BoolSetting time_enabled{{}, "Enabled", true, true, {}};
    config::FloatSetting time_value{{}, "Time", "%.0f", 100.0f, 0.0f, 0.0f, 24000.0f, 12000.0f, 12000.0f};

    config::ModeSetting lighting{{}, "Lighting", 3, 0, kLightingModes, 0, 0, false, {}};
    adjust::ColorOption lighting_color{"Lighting color", "Lighting color mode", adjust::argb(166, 166, 166, 255)};
    config::FloatSetting brightness{{}, "Brightness", "%.2f", 0.01f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    config::BoolSetting no_night_vision{{}, "No Night Vision", false, false, {}};

    // "Fog" (ExpandSetting, enabledPredicate = AtmosphereFogCondition -> fogEnabled)
    config::BoolSetting fog_enabled{{}, "Enabled", false, false, {}};
    config::BoolSetting custom_fog_color{{}, "Custom color", true, true, {}};
    adjust::ColorOption fog_color{"Color", "Color mode", adjust::argb(77, 6, 6, 255)};
    config::FloatSetting fog_strength{{}, "Color Strength", "%.0f", 1.0f, 0.0f, 0.0f, 100.0f, 25.0f, 25.0f};
    config::BoolSetting custom_density{{}, "Custom Density", true, true, {}};
    config::FloatSetting density{{}, "Density", "%.2f", 0.05f, 0.0f, 0.0f, 20.0f, 1.8f, 1.8f};
    config::ModeSetting fog_mode{{}, "Mode", 3, 0, kFogModes, 0, 0, false, {}};

    AtmosphereState() noexcept {
        base.name = "Atmosphere";
        base.description = "Changes the time, lighting and fog of the world.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust; // AtmosphereModule calls setHidden(false)
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Atmosphere");
    }

    bool running() const noexcept { return events::module_is_running(&base); }
    LightingMode lighting_mode() const noexcept { return static_cast<LightingMode>(lighting.value); }
    FogMode fog_mode_value() const noexcept { return static_cast<FogMode>(fog_mode.value); }
};

AtmosphereState g_state{};

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Time");
    g_state.time_enabled.render(gui, renderer);
    g_state.time_value.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Lighting");
    g_state.lighting.render(gui, renderer);
    if (g_state.lighting_mode() == LightingMode::Custom) {
        g_state.lighting_color.render(gui, renderer);
    }
    if (g_state.lighting_mode() == LightingMode::Bright) {
        g_state.brightness.render(gui, renderer);
    }
    g_state.no_night_vision.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Fog");
    g_state.fog_enabled.render(gui, renderer);
    g_state.custom_fog_color.render(gui, renderer);
    if (g_state.custom_fog_color.value) {
        g_state.fog_color.render(gui, renderer);
        g_state.fog_strength.render(gui, renderer);
    }
    g_state.custom_density.render(gui, renderer);
    if (g_state.custom_density.value) {
        g_state.density.render(gui, renderer);
        g_state.fog_mode.render(gui, renderer);
    }
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.time_enabled.visit(archive, "time_enabled");
    g_state.time_value.visit(archive, "time");
    g_state.lighting.visit(archive, "lighting");
    g_state.lighting_color.visit(archive, "lighting_color", "lighting_color_mode");
    g_state.brightness.visit(archive, "brightness");
    g_state.no_night_vision.visit(archive, "no_night_vision");
    g_state.fog_enabled.visit(archive, "fog_enabled");
    g_state.custom_fog_color.visit(archive, "fog_custom_color");
    g_state.fog_color.visit(archive, "fog_color", "fog_color_mode");
    g_state.fog_strength.visit(archive, "fog_color_strength");
    g_state.custom_density.visit(archive, "fog_custom_density");
    g_state.density.visit(archive, "fog_density");
    g_state.fog_mode.visit(archive, "fog_mode");
}

} // namespace

events::Module* get_adjust_atmosphere_module() noexcept {
    return &g_state.base;
}

// World.getEffectiveWorldTime(): (long) atmosphere.timeValue.getInt()
bool adjust_atmosphere_time_override(int64_t* out_time) noexcept {
    if (out_time == nullptr || !g_state.running() || !g_state.time_enabled.value) {
        return false;
    }
    *out_time = static_cast<int64_t>(static_cast<int32_t>(g_state.time_value.value));
    return true;
}

// AtmosphereModule.onLighting (nightVision) / isNightVisionDisabled()
bool adjust_atmosphere_disables_night_vision() noexcept {
    return g_state.running() && g_state.no_night_vision.value;
}

// AtmosphereModule.onLighting (customColors)
bool adjust_atmosphere_disables_custom_colors() noexcept {
    return g_state.running() && g_state.lighting_mode() != LightingMode::Default;
}

// AtmosphereModule.onBrightness
bool adjust_atmosphere_gamma(float* out_gamma) noexcept {
    if (out_gamma == nullptr || !g_state.running() || g_state.lighting_mode() != LightingMode::Bright) {
        return false;
    }
    const float value = g_state.brightness.value;
    *out_gamma = value < 0.005f ? 0.0375f : value * 7.5f;
    return true;
}

// EntityRenderer.updateLightmap: lightingRgb = atmosphere.lightingColor.getColor() while Lighting = Custom
bool adjust_atmosphere_lighting_color(Argb* out_color) noexcept {
    if (out_color == nullptr || !g_state.running() || g_state.lighting_mode() != LightingMode::Custom) {
        return false;
    }
    *out_color = g_state.lighting_color.get();
    return true;
}

// AtmosphereModule.onFogColor
bool adjust_atmosphere_fog_color(float* red, float* green, float* blue) noexcept {
    if (red == nullptr || green == nullptr || blue == nullptr || !g_state.running() || !g_state.fog_enabled.value ||
        !g_state.custom_fog_color.value) {
        return false;
    }
    const float strength = g_state.fog_strength.value / 100.0f;
    const Argb color = g_state.fog_color.get();
    *red += (static_cast<float>(adjust::red_of(color)) / 255.0f - *red) * strength;
    *green += (static_cast<float>(adjust::green_of(color)) / 255.0f - *green) * strength;
    *blue += (static_cast<float>(adjust::blue_of(color)) / 255.0f - *blue) * strength;
    return true;
}

bool adjust_atmosphere_fog_enabled() noexcept {
    return g_state.running() && g_state.fog_enabled.value;
}

// AtmosphereModule.onFog
bool adjust_atmosphere_apply_fog(AtmosphereFogParams* params) noexcept {
    if (params == nullptr || !adjust_atmosphere_fog_enabled()) {
        return false;
    }
    if (!g_state.custom_fog_color.value && !g_state.custom_density.value) {
        return false;
    }
    // event.fogStandard = true (OptiFine only; applyFogParameters overrides it with false while fog is enabled)
    if (!g_state.custom_density.value) {
        return false;
    }
    const float value = g_state.density.value;
    const float range = std::max(1.0f, params->far_plane_distance);
    if (value <= 0.0f) {
        params->mode = kGlLinear;
        params->start = range * 4.0f;
        params->end = params->start + 1.0f;
    } else if (params->radial) {
        params->density *= value;
    } else {
        switch (g_state.fog_mode_value()) {
            case FogMode::Vanilla:
                params->start /= value;
                params->end /= value;
                break;
            case FogMode::Soft: {
                const float scale = range * 1024.0f;
                const float end = scale * (1.0f - value / 20.0f);
                params->start = end - scale;
                params->end = end;
                break;
            }
            case FogMode::Constant:
                params->mode = kGlExp;
                params->density = value * 3.0f / range;
                break;
        }
    }
    return true;
}

} // namespace slinky::modules
