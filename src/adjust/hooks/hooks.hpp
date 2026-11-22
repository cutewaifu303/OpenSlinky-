#pragma once

// OpenSlinky++ extra Minecraft hooks for the Adjust visual style.
//
// Each domain file (render_hooks.cpp, ...) defines its natives and a nullptr-terminated list of
// bytecode::ExtraHookSpec; hook_registry.cpp concatenates the lists for the JVMTI installer. Every native must also
// be declared in java/src/SlinkyHooks13.java with the same name and descriptor.

#include <jni.h>

#include "bytecode/extra_hooks.hpp"
#include "events/events.hpp"

namespace slinky::adjust::hooks {

// JniContext for a hook native (mappings of the running client + the calling thread's env).
events::JniContext jni_for(JNIEnv* env) noexcept;

// Domain hook lists (nullptr-terminated).
const bytecode::ExtraHookSpec* const* render_hooks() noexcept;
const bytecode::ExtraHookSpec* const* tweaks_hooks() noexcept;
const bytecode::ExtraHookSpec* const* camera_hooks() noexcept;
const bytecode::ExtraHookSpec* const* world_fx_hooks() noexcept;
const bytecode::ExtraHookSpec* const* item_hooks() noexcept;
const bytecode::ExtraHookSpec* const* mainmenu_hooks() noexcept;
const bytecode::ExtraHookSpec* const* streamer_hooks() noexcept;
const bytecode::ExtraHookSpec* const* hurtcam_hooks() noexcept;

} // namespace slinky::adjust::hooks
