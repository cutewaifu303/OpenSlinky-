#pragma once

// Port of com.adjustclient.module.impl.render.esp.ModernItemName: a per item/metadata colour table (modern
// texture average colours) used by Adjust ESP to colour item nametags.

#include <jni.h>

#include "adjust/kit.hpp"
#include "events/events.hpp"

namespace slinky::modules::adjust_esp {

// ModernItemName.lookup(stack): colour for the stack's item + metadata (exact metadata first, else the entry
// with metadata -1). Returns false when the item has no entry (Adjust returns null).
bool modern_item_color(events::JniContext* jni, jobject stack, adjust::Argb* out_rgb) noexcept;

} // namespace slinky::modules::adjust_esp
