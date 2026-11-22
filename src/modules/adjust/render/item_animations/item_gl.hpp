#pragma once

// GlStateManager (Minecraft 1.8.9) through JNI + Minecraft's MathHelper float math, shared by the Adjust Item
// Animations / Item Physics ports and their hooks (src/adjust/hooks/item_hooks.cpp).
//
// Every matrix operation goes through GlStateManager (as Adjust's patched Minecraft does), so its state cache stays
// correct. If GlStateManager cannot be resolved (1.7.10 / unknown mappings) the matrix functions fall back to the
// identical raw GL calls (GlStateManager does not cache matrix state) and the state functions are no-ops.

#include <jni.h>

#include "events/events.hpp"

namespace slinky::adjust::item_gl {

// True if GlStateManager and all methods used below were resolved.
bool available(events::JniContext* jni) noexcept;

void push_matrix(events::JniContext* jni) noexcept;
void pop_matrix(events::JniContext* jni) noexcept;
void translate(events::JniContext* jni, float x, float y, float z) noexcept;
void rotate(events::JniContext* jni, float angle, float x, float y, float z) noexcept;
void scale(events::JniContext* jni, float x, float y, float z) noexcept;
void enable_rescale_normal(events::JniContext* jni) noexcept;
void disable_rescale_normal(events::JniContext* jni) noexcept;
void alpha_func(events::JniContext* jni, int func, float ref) noexcept;
void enable_blend(events::JniContext* jni) noexcept;
void disable_blend(events::JniContext* jni) noexcept;
void try_blend_func_separate(events::JniContext* jni, int src, int dst, int src_alpha, int dst_alpha) noexcept;
void color(events::JniContext* jni, float r, float g, float b, float a) noexcept;

// net.minecraft.util.MathHelper.sin / cos (65536-entry table lookup, same values as Minecraft) and sqrt_float.
float mc_sin(float value) noexcept;
float mc_cos(float value) noexcept;
float mc_sqrt(float value) noexcept;

// (float) Math.PI
inline constexpr float kPi = 3.1415927f;

} // namespace slinky::adjust::item_gl
