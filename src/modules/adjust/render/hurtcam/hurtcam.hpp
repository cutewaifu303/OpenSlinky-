#pragma once

// Adjust "Hurtcam" (com.adjustclient.module.impl.render.HurtcamModule; HurtcamArmorCondition and
// HurtcamColorCondition are unused BooleanSupplier helpers in the restored source).
//
// Driven by the hooks in src/adjust/hooks/hurtcam_hooks.cpp:
//   * EntityRenderer.hurtCameraEffect          -> skipped while "Local > Disabled" is on
//   * RendererLivingEntity.setBrightness       -> hurt tint uses "Model color > Color" while enabled
//   * LayerArmorBase.renderLayer (per slot)    -> armour also gets the hurt tint while "Armor" is on
//     (Adjust: LayerArmorBase.shouldCombineTextures() returns true)
// Every query returns false while the module is not running, so the hooks keep vanilla behaviour.

#include "adjust/kit.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_hurtcam_module() noexcept;

// EntityRenderer.hurtCameraEffect patch: true = skip the local hurt camera shake (and the death roll).
bool adjust_hurtcam_disables_local() noexcept;

// RendererLivingEntity.setBrightness patch: true = replace the hurt tint (vanilla 1, 0, 0) by *out_color's RGB
// (alpha stays 0.3).
bool adjust_hurtcam_model_color(adjust::Argb* out_color) noexcept;

// LayerArmorBase.shouldCombineTextures patch: true = armour layers are rendered with the hurt tint.
bool adjust_hurtcam_tints_armor() noexcept;

} // namespace slinky::modules
