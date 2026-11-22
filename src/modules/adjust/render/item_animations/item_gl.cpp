#include "modules/adjust/render/item_animations/item_gl.hpp"

#include <cmath>
#include <cstdint>

#include <glad/glad.h>

#include "adjust/jni_lookup.hpp"

namespace slinky::adjust::item_gl {

namespace {

struct GlIds {
    jclass cls = nullptr;
    jmethodID push_matrix = nullptr;
    jmethodID pop_matrix = nullptr;
    jmethodID translate = nullptr;
    jmethodID rotate = nullptr;
    jmethodID scale = nullptr;
    jmethodID enable_rescale_normal = nullptr;
    jmethodID disable_rescale_normal = nullptr;
    jmethodID alpha_func = nullptr;
    jmethodID enable_blend = nullptr;
    jmethodID disable_blend = nullptr;
    jmethodID blend_func_separate = nullptr;
    jmethodID color = nullptr;
    bool complete = false;
};

const GlIds& ids(events::JniContext* jni) noexcept {
    static const GlIds s_ids = [&] {
        GlIds g{};
        g.cls = jni::find_class(jni, "bfl", "net/minecraft/client/renderer/GlStateManager");
        if (g.cls == nullptr) {
            return g;
        }
        g.push_matrix = jni::static_method(jni, g.cls, {"E", "pushMatrix", "func_179094_E"}, "()V", "()V");
        g.pop_matrix = jni::static_method(jni, g.cls, {"F", "popMatrix", "func_179121_F"}, "()V", "()V");
        g.translate = jni::static_method(jni, g.cls, {"b", "translate", "func_179109_b"}, "(FFF)V", "(FFF)V");
        g.rotate = jni::static_method(jni, g.cls, {"b", "rotate", "func_179114_b"}, "(FFFF)V", "(FFFF)V");
        g.scale = jni::static_method(jni, g.cls, {"a", "scale", "func_179152_a"}, "(FFF)V", "(FFF)V");
        g.enable_rescale_normal =
            jni::static_method(jni, g.cls, {"B", "enableRescaleNormal", "func_179091_B"}, "()V", "()V");
        g.disable_rescale_normal =
            jni::static_method(jni, g.cls, {"C", "disableRescaleNormal", "func_179101_C"}, "()V", "()V");
        g.alpha_func = jni::static_method(jni, g.cls, {"a", "alphaFunc", "func_179092_a"}, "(IF)V", "(IF)V");
        g.enable_blend = jni::static_method(jni, g.cls, {"l", "enableBlend", "func_179147_l"}, "()V", "()V");
        g.disable_blend = jni::static_method(jni, g.cls, {"k", "disableBlend", "func_179084_k"}, "()V", "()V");
        g.blend_func_separate =
            jni::static_method(jni, g.cls, {"a", "tryBlendFuncSeparate", "func_179120_a"}, "(IIII)V", "(IIII)V");
        g.color = jni::static_method(jni, g.cls, {"c", "color", "func_179131_c"}, "(FFFF)V", "(FFFF)V");
        g.complete = g.push_matrix != nullptr && g.pop_matrix != nullptr && g.translate != nullptr &&
                     g.rotate != nullptr && g.scale != nullptr && g.enable_rescale_normal != nullptr &&
                     g.disable_rescale_normal != nullptr && g.alpha_func != nullptr && g.enable_blend != nullptr &&
                     g.disable_blend != nullptr && g.blend_func_separate != nullptr && g.color != nullptr;
        return g;
    }();
    return s_ids;
}

// Returns false if the GlStateManager method is unavailable (the caller may fall back to raw GL).
template <typename... Args>
bool call(events::JniContext* jni, jmethodID GlIds::*member, Args... args) noexcept {
    if (jni == nullptr || jni->env == nullptr) {
        return false;
    }
    const GlIds& g = ids(jni);
    if (g.cls == nullptr || g.*member == nullptr) {
        return false;
    }
    jni->env->CallStaticVoidMethod(g.cls, g.*member, args...);
    return true;
}

// Java (int) cast of a float: truncation, NaN -> 0, saturating.
int32_t java_f2i(float v) noexcept {
    if (std::isnan(v)) {
        return 0;
    }
    if (v >= 2147483647.0f) {
        return INT32_MAX;
    }
    if (v <= -2147483648.0f) {
        return INT32_MIN;
    }
    return static_cast<int32_t>(v);
}

// MathHelper.SIN_TABLE[index] = (float) Math.sin(index * Math.PI * 2.0 / 65536.0)
float sin_table(int32_t index) noexcept {
    constexpr double kJavaPi = 3.141592653589793;
    return static_cast<float>(std::sin(static_cast<double>(index & 65535) * kJavaPi * 2.0 / 65536.0));
}

} // namespace

bool available(events::JniContext* jni) noexcept {
    return jni != nullptr && jni->env != nullptr && ids(jni).complete;
}

// Matrix functions: GlStateManager's versions are plain GL11 forwards (no cached state), so raw GL is an exact
// fallback - it keeps push/pop balanced inside Minecraft's render code even if a lookup failed.
void push_matrix(events::JniContext* jni) noexcept {
    if (!call(jni, &GlIds::push_matrix) && glPushMatrix != nullptr) {
        glPushMatrix();
    }
}

void pop_matrix(events::JniContext* jni) noexcept {
    if (!call(jni, &GlIds::pop_matrix) && glPopMatrix != nullptr) {
        glPopMatrix();
    }
}

void translate(events::JniContext* jni, float x, float y, float z) noexcept {
    if (!call(jni, &GlIds::translate, static_cast<jfloat>(x), static_cast<jfloat>(y), static_cast<jfloat>(z)) &&
        glTranslatef != nullptr) {
        glTranslatef(x, y, z);
    }
}

void rotate(events::JniContext* jni, float angle, float x, float y, float z) noexcept {
    if (!call(jni, &GlIds::rotate, static_cast<jfloat>(angle), static_cast<jfloat>(x), static_cast<jfloat>(y),
              static_cast<jfloat>(z)) &&
        glRotatef != nullptr) {
        glRotatef(angle, x, y, z);
    }
}

void scale(events::JniContext* jni, float x, float y, float z) noexcept {
    if (!call(jni, &GlIds::scale, static_cast<jfloat>(x), static_cast<jfloat>(y), static_cast<jfloat>(z)) &&
        glScalef != nullptr) {
        glScalef(x, y, z);
    }
}

void enable_rescale_normal(events::JniContext* jni) noexcept { call(jni, &GlIds::enable_rescale_normal); }
void disable_rescale_normal(events::JniContext* jni) noexcept { call(jni, &GlIds::disable_rescale_normal); }

void alpha_func(events::JniContext* jni, int func, float ref) noexcept {
    call(jni, &GlIds::alpha_func, static_cast<jint>(func), static_cast<jfloat>(ref));
}

void enable_blend(events::JniContext* jni) noexcept { call(jni, &GlIds::enable_blend); }
void disable_blend(events::JniContext* jni) noexcept { call(jni, &GlIds::disable_blend); }

void try_blend_func_separate(events::JniContext* jni, int src, int dst, int src_alpha, int dst_alpha) noexcept {
    call(jni, &GlIds::blend_func_separate, static_cast<jint>(src), static_cast<jint>(dst),
         static_cast<jint>(src_alpha), static_cast<jint>(dst_alpha));
}

void color(events::JniContext* jni, float r, float g, float b, float a) noexcept {
    call(jni, &GlIds::color, static_cast<jfloat>(r), static_cast<jfloat>(g), static_cast<jfloat>(b),
         static_cast<jfloat>(a));
}

float mc_sin(float value) noexcept {
    return sin_table(java_f2i(value * 10430.378f));
}

float mc_cos(float value) noexcept {
    return sin_table(java_f2i(value * 10430.378f + 16384.0f));
}

float mc_sqrt(float value) noexcept {
    return static_cast<float>(std::sqrt(static_cast<double>(value)));
}

} // namespace slinky::adjust::item_gl
