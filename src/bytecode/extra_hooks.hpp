#pragma once

// OpenSlinky++ extra bytecode hooks (Adjust visual style, Minecraft 1.8.9).
//
// The original client patches 9 Minecraft classes with a fixed table (jvmti_installer.cpp). Extra hooks are plain
// data: "inside <class>.<method>, replace the instruction that touches <owner>.<member> with an invokestatic of
// SlinkyHooks13.<native_name><native_desc>", plus the C++ function registered for that native. At install time they
// are merged with the base table (same class -> one ClassPatchSpec; same method -> one MethodPatchSpec) and their
// natives are registered next to the 23 base natives.
//
// Rules for a hook:
//   * native_desc must be the stack-compatible static form of the replaced instruction: receiver (if any) and all
//     reference parameters as Ljava/lang/Object;, primitives unchanged, same return type (reference returns must be
//     JDK types such as Ljava/lang/String; or Ljava/util/Collection;, never Minecraft types).
//   * The native must be declared in java/src/SlinkyHooks13.java with exactly that name and descriptor.
//   * The native usually calls the original member through JNI when the feature is not active - the call site was
//     replaced, not the member itself, so this does not recurse.
//   * Only applied on 1.8.9; classes that cannot be resolved (e.g. Forge-only classes on vanilla) are skipped.

#include <cstddef>
#include <jni.h>

#include "bytecode/classfile_rewriter.hpp"

namespace slinky::bytecode {

// {vanilla name, vanilla desc, MCP name, MCP desc, SRG name, SRG desc}. All nullptr in `method` = every method.
struct ExtraMemberNames {
    const char* vanilla_name = nullptr;
    const char* vanilla_desc = nullptr;
    const char* mcp_name = nullptr;
    const char* mcp_desc = nullptr;
    const char* srg_name = nullptr;
    const char* srg_desc = nullptr;
};

struct ExtraHookSpec {
    const char* class_vanilla = nullptr; // class whose bytecode is patched (notch name, e.g. "avo")
    const char* class_named = nullptr;   // MCP/SRG name ("net/minecraft/client/gui/GuiIngame")
    ExtraMemberNames method{};           // method to patch (all nullptr = all methods of the class)
    const char* owner_vanilla = nullptr; // owner of the replaced member
    const char* owner_named = nullptr;
    ExtraMemberNames target{};           // replaced member
    HookInsnKind kind = HookInsnKind::InvokeInstance;
    const char* native_name = nullptr;   // SlinkyHooks13 native
    const char* native_desc = nullptr;
    void* native_fn = nullptr;           // JNICALL function (JNIEnv*, jclass, ...)
};

// All extra hooks compiled into the client (src/adjust/hooks/hook_registry.cpp). nullptr-terminated.
const ExtraHookSpec* const* all_extra_hooks() noexcept;

} // namespace slinky::bytecode
