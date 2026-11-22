#pragma once

// OpenSlinky++ Alt Manager - live Minecraft session injection.
//
// Builds a new net.minecraft.util.Session from an Account and writes it into the Minecraft singleton's
// session field, so the in-game username / UUID / access token change immediately (no restart). The session
// field name is obfuscation-dependent, so it is located by reflection (the only Session-typed field on the
// Minecraft class) rather than by a hard-coded name. Must be called on the JNI/render thread.

#include <jni.h>

#include "events/events.hpp"
#include "modules/altmanager/account_store.hpp"

namespace slinky::altmanager {

// Returns true if the session was replaced. 1.8.x only; no-op (false) on 1.7.10 or if reflection fails.
bool inject_session(events::JniContext* jni, jobject minecraft, const Account& account) noexcept;

} // namespace slinky::altmanager
