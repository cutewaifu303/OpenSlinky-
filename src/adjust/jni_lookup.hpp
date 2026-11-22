#pragma once

// Self-contained JNI member lookup for ported Adjust visuals.
//
// The central Mappings table (minecraft/mappings.hpp + mc_wrappers) already covers ~360 accessors - use those
// first. When a port needs a field/method that is not mapped yet, resolve it locally with these helpers instead
// of editing mappings.cpp, and cache the result in a function-local static:
//
//     static const jclass cls = adjust::jni::find_class(jni, "pr", "net/minecraft/entity/EntityLivingBase");
//     static const jmethodID mid = adjust::jni::method(jni, cls, {"bn", "getHealth", "func_110143_aJ"},
//                                                      "()F", "()F");
//     if (mid == nullptr) return;   // unsupported version / mapping -> degrade gracefully
//
// Names are for Minecraft 1.8.9 (the version Adjust targets): {vanilla/notch, MCP, SRG}. The flavour in use is
// picked from Mappings::mapping_flavor (0 = vanilla, 1 = MCP/Lunar, 2 = Forge/SRG). Class names: vanilla uses the
// notch name ("pk"), MCP and SRG both use the deobfuscated path ("net/minecraft/entity/Entity").
// Descriptors: pass the vanilla descriptor (notch class names, e.g. "(Lpk;)F") and the named descriptor
// ("(Lnet/minecraft/entity/Entity;)F"); they are identical for primitives.
//
// Look names up in the generated 1.8.9 table (kind, mcp_class, mcp_name, srg_name, notch_class, notch_name,
// notch_desc, mcp_desc) - see the porting brief.
//
// All helpers return nullptr on failure (and clear the pending Java exception); on 1.7.10 they always fail.

#include <jni.h>

#include "events/events.hpp"

namespace slinky::adjust::jni {

struct Names {
    const char* vanilla;
    const char* mcp;
    const char* srg;
};

bool is_1_8(events::JniContext* jni) noexcept;
const char* pick(events::JniContext* jni, const Names& names) noexcept;

// Returns a global reference (never freed - cache it).
jclass find_class(events::JniContext* jni, const char* vanilla_name, const char* mcp_name) noexcept;

jfieldID field(events::JniContext* jni, jclass cls, const Names& names,
               const char* vanilla_desc, const char* named_desc) noexcept;
jfieldID static_field(events::JniContext* jni, jclass cls, const Names& names,
                      const char* vanilla_desc, const char* named_desc) noexcept;
jmethodID method(events::JniContext* jni, jclass cls, const Names& names,
                 const char* vanilla_desc, const char* named_desc) noexcept;
jmethodID static_method(events::JniContext* jni, jclass cls, const Names& names,
                        const char* vanilla_desc, const char* named_desc) noexcept;

// Clears and reports a pending Java exception (true if one was pending). Call after JNI calls that may throw.
bool clear_exception(events::JniContext* jni) noexcept;

} // namespace slinky::adjust::jni
