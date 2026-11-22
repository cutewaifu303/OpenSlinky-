#pragma once

// Adjust "True Sight" (com.adjustclient.module.impl.render.TrueSightModule).
//
// INERT WITHOUT A HOOK. Adjust handles EntityOpacityEvent, posted by its patched RendererLivingEntity.doRender
// before the model is drawn; OpenSlinky++ has no equivalent call-in yet. The module, its settings and the decision
// logic are complete: a phase-2 hook only has to call adjust_true_sight_apply() (see the port notes in
// true_sight.cpp for the exact injection point).

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_true_sight_module() noexcept;

// TrueSightModule.apply(EntityOpacityEvent). `entity` is the EntityLivingBase being rendered; `invisible` and
// `body_alpha` are the event fields (initially entity.isInvisible() and 1.0). Returns true if it changed them:
// an invisible player (not a bot, not a teammate when "Ignore teammates" is on) becomes visible with
// body_alpha = Opacity / 100.
bool adjust_true_sight_apply(events::JniContext* jni, jobject entity, bool* invisible, float* body_alpha) noexcept;

} // namespace slinky::modules
