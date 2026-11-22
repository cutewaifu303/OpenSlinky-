// Port of com.adjustclient.module.impl.render.hud.widget.MemoryUsageWidget.
//
// Java heap usage of the game (java.lang.Runtime, read through JNI): "used/maxMB" right aligned, the bold
// percentage on the left and a thin bar filled in the HUD colour.

#include "modules/adjust/hud/widgets/widgets.hpp"

#include <cinttypes>
#include <cstdio>
#include <jni.h>

#include "config/settings.hpp"

namespace slinky::modules::adjust_hud::widgets {

namespace {

// HudWidget.createSettings: BooleanSetting labelled with the widget name, default off.
config::BoolSetting g_enabled{{}, "Memory Usage", false, false, {}};

struct RuntimeIds {
    jclass cls = nullptr;
    jmethodID get_runtime = nullptr;
    jmethodID max_memory = nullptr;
    jmethodID total_memory = nullptr;
    jmethodID free_memory = nullptr;
    bool resolved = false;
    bool ok = false;
};

const RuntimeIds& runtime_ids(JNIEnv* env) noexcept {
    static RuntimeIds ids{};
    if (ids.resolved) {
        return ids;
    }
    ids.resolved = true;
    jclass local = env->FindClass("java/lang/Runtime");
    if (local == nullptr) {
        env->ExceptionClear();
        return ids;
    }
    ids.cls = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    ids.get_runtime = env->GetStaticMethodID(ids.cls, "getRuntime", "()Ljava/lang/Runtime;");
    ids.max_memory = env->GetMethodID(ids.cls, "maxMemory", "()J");
    ids.total_memory = env->GetMethodID(ids.cls, "totalMemory", "()J");
    ids.free_memory = env->GetMethodID(ids.cls, "freeMemory", "()J");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    ids.ok = ids.get_runtime != nullptr && ids.max_memory != nullptr && ids.total_memory != nullptr &&
             ids.free_memory != nullptr;
    return ids;
}

// Runtime.maxMemory() and totalMemory() - freeMemory(). False if JNI failed.
bool read_memory(JNIEnv* env, int64_t* out_max, int64_t* out_used) noexcept {
    const RuntimeIds& ids = runtime_ids(env);
    if (!ids.ok) {
        return false;
    }
    jobject runtime = env->CallStaticObjectMethod(ids.cls, ids.get_runtime);
    if (env->ExceptionCheck() || runtime == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const jlong max = env->CallLongMethod(runtime, ids.max_memory);
    const jlong total = env->CallLongMethod(runtime, ids.total_memory);
    const jlong free = env->CallLongMethod(runtime, ids.free_memory);
    env->DeleteLocalRef(runtime);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    *out_max = static_cast<int64_t>(max);
    *out_used = static_cast<int64_t>(total - free);
    return true;
}

} // namespace

void memory_render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_enabled.render(gui, renderer);
}

void memory_visit_config(config::ConfigArchive* ar) noexcept {
    g_enabled.visit(ar, "widgets_memory_usage");
}

void memory_render(const HudContext& ctx) noexcept {
    if (!g_enabled.value || ctx.game == nullptr || ctx.game->jni == nullptr || ctx.game->jni->env == nullptr) {
        return;
    }
    const WidgetBox box = draw_frame("Memory Usage", kMemoryX, kMemoryY, 65.0f, 15.0f);

    int64_t max = 0;
    int64_t used = 0;
    if (!read_memory(ctx.game->jni->env, &max, &used)) {
        return;
    }
    const int64_t used_mb = used / 1024 / 1024;
    const int64_t max_mb = max / 1024 / 1024;
    char usage[64]{};
    std::snprintf(usage, sizeof(usage), "%" PRId64 "/%" PRId64 "MB", used_mb, max_mb);

    const float x = box.x;
    const float width = box.width;
    const float y = box.y;
    const adjust::Argb text_color = adjust::argb(210, 210, 210, 200);
    adjust::kTahoma11.draw(usage, (x + width) - 3.0f - adjust::kTahoma11.width(usage), y + 14.0f, text_color);

    if (max == 0) {
        return; // Adjust divides by zero here (ArithmeticException); just skip the rest.
    }
    const int64_t percent = 100 * used / max;
    char percent_text[48]{};
    std::snprintf(percent_text, sizeof(percent_text), ADJUST_FMT "l% 2" PRId64 "%%", percent);
    adjust::kTahoma12.draw(percent_text, x + 2.0f, y + 14.0f, text_color);

    // Bar: grey track, HUD coloured fill.
    const float bar_left = x + 3.5f;
    const float bar_top = y + 21.0f;
    const float bar_width = width - 7.0f;
    const float bar_bottom = bar_top + 1.0f;
    adjust::fill_rounded_rect(adjust::argb(210, 210, 210, 150), bar_left, bar_top, bar_left + bar_width, bar_bottom,
                              1.0f);
    const float fill_right =
        (static_cast<float>(static_cast<int>(percent)) / 100.0f) * bar_width + bar_left;
    const adjust::Argb fill_color = (hud_color() & 0xFFFFFFu) | 0x96000000u;
    adjust::fill_rounded_rect(fill_color, bar_left, bar_top, fill_right, bar_bottom, 1.0f);
}

} // namespace slinky::modules::adjust_hud::widgets
