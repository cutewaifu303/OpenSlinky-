#pragma once

// Adjust "Item Physics" (com.adjustclient.module.impl.render.ItemPhysicsModule).
//
// Dropped items lie flat on the ground and spin while falling. Adjust's patched RenderEntityItem.doRender calls
// ItemPhysicsModule.renderItem instead of the vanilla body while the module is enabled; here the
// RenderEntityItem.doRender bridge call is hooked (src/adjust/hooks/item_hooks.cpp) and calls render_item().

#include <jni.h>

#include "events/events.hpp"
#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_item_physics_module() noexcept;

namespace item_physics {

// module_is_running(Item Physics). Adjust's RenderUtil (ESP screen bounds) only adds the vanilla item bobbing
// offset while this is false.
bool running() noexcept;

// ItemPhysicsModule.renderItem(renderer, item, x, y, z). `renderer` = RenderEntityItem, `entity_item` = EntityItem.
void render_item(events::JniContext* jni, jobject renderer, jobject entity_item, double x, double y, double z) noexcept;

} // namespace item_physics

} // namespace slinky::modules
