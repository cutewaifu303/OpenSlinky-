// Adjust main menu hooks (Minecraft 1.8.9) - see modules/adjust/mainmenu/main_menu.hpp:
//   * EntityRenderer.updateCameraAndRender -> GuiScreen.drawScreen(IIF)V         (vanilla / OptiFine / MCP)
//   * EntityRenderer.updateCameraAndRender -> ForgeHooksClient.drawScreen(...)   (Forge)
//       the vanilla GuiMainMenu is not drawn while the Adjust menu replaces it (drawn on the NanoVG GUI layer)
//   * Minecraft.runTick -> GuiScreen.handleInput()V
//       mouse input goes to the Adjust menu buttons / quit dialog instead of vanilla's (invisible) buttons
//   * Minecraft.runGameLoop -> Minecraft.getLimitFramerate()I
//       AdjustMainMenu.getMaxFps(): 175 (60 unfocused) instead of vanilla's 30 fps title screen cap
//
// Each native calls the original member when the Adjust menu is not active.

#include "adjust/hooks/hooks.hpp"

#include "adjust/jni_lookup.hpp"
#include "minecraft/mappings.hpp"
#include "modules/adjust/mainmenu/main_menu.hpp"

namespace slinky::adjust::hooks {

namespace {

using bytecode::ExtraHookSpec;
using bytecode::HookInsnKind;

// RAII local frame: hook natives may run every frame / tick.
struct LocalFrame {
    JNIEnv* env;
    bool pushed;
    explicit LocalFrame(JNIEnv* e) noexcept : env(e), pushed(e->PushLocalFrame(32) == 0) {
        if (!pushed) {
            env->ExceptionClear();
        }
    }
    ~LocalFrame() {
        if (pushed) {
            env->PopLocalFrame(nullptr); // allowed with a pending exception
        }
    }
    LocalFrame(const LocalFrame&) = delete;
    LocalFrame& operator=(const LocalFrame&) = delete;
};

// ---------------------------------------------------------------------------------------------------------------
// GuiScreen.drawScreen(IIF)V (instance call in EntityRenderer.updateCameraAndRender)
// ---------------------------------------------------------------------------------------------------------------

void JNICALL native_draw_screen(JNIEnv* env, jclass /*cls*/, jobject screen, jint mouse_x, jint mouse_y,
                                jfloat partial_ticks) noexcept {
    events::JniContext jni = jni_for(env);
    {
        LocalFrame frame(env);
        if (modules::adjust_main_menu::on_draw_screen(&jni, screen, mouse_x, mouse_y)) {
            return;
        }
    }
    static const jclass cls = jni::find_class(&jni, "axu", "net/minecraft/client/gui/GuiScreen");
    static const jmethodID mid = jni::method(&jni, cls, {"a", "drawScreen", "func_73863_a"}, "(IIF)V", "(IIF)V");
    if (mid != nullptr && screen != nullptr) {
        env->CallVoidMethod(screen, mid, mouse_x, mouse_y, partial_ticks); // exceptions stay pending
    }
}

// ---------------------------------------------------------------------------------------------------------------
// ForgeHooksClient.drawScreen(GuiScreen, int, int, float)V (static call, Forge's updateCameraAndRender)
// ---------------------------------------------------------------------------------------------------------------

void JNICALL native_forge_draw_screen(JNIEnv* env, jclass /*cls*/, jobject screen, jint mouse_x, jint mouse_y,
                                      jfloat partial_ticks) noexcept {
    events::JniContext jni = jni_for(env);
    {
        LocalFrame frame(env);
        if (modules::adjust_main_menu::on_draw_screen(&jni, screen, mouse_x, mouse_y)) {
            return;
        }
    }
    static const jclass cls = jni::find_class(&jni, nullptr, "net/minecraftforge/client/ForgeHooksClient");
    static const jmethodID mid = jni::static_method(&jni, cls, {"drawScreen", "drawScreen", "drawScreen"},
                                                    "(Laxu;IIF)V", "(Lnet/minecraft/client/gui/GuiScreen;IIF)V");
    if (mid != nullptr) {
        env->CallStaticVoidMethod(cls, mid, screen, mouse_x, mouse_y, partial_ticks);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// GuiScreen.handleInput()V (instance call in Minecraft.runTick)
// ---------------------------------------------------------------------------------------------------------------

void JNICALL native_handle_input(JNIEnv* env, jclass /*cls*/, jobject screen) noexcept {
    events::JniContext jni = jni_for(env);
    {
        LocalFrame frame(env);
        if (modules::adjust_main_menu::on_handle_input(&jni, screen)) {
            return; // a Java exception thrown by a vanilla call made there stays pending
        }
    }
    static const jclass cls = jni::find_class(&jni, "axu", "net/minecraft/client/gui/GuiScreen");
    static const jmethodID mid = jni::method(&jni, cls, {"p", "handleInput", "func_146269_k"}, "()V", "()V");
    if (mid != nullptr && screen != nullptr) {
        env->CallVoidMethod(screen, mid);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Minecraft.getLimitFramerate()I (instance call in Minecraft.runGameLoop)
// ---------------------------------------------------------------------------------------------------------------

jint JNICALL native_get_limit_framerate(JNIEnv* env, jclass /*cls*/, jobject minecraft) noexcept {
    events::JniContext jni = jni_for(env);
    {
        LocalFrame frame(env);
        int fps = 0;
        if (modules::adjust_main_menu::on_limit_framerate(&jni, minecraft, &fps)) {
            return fps;
        }
    }
    static const jmethodID mid = jni::method(&jni, jni.mappings != nullptr ? jni.mappings->cls_minecraft : nullptr,
                                             {"j", "getLimitFramerate", "func_90020_K"}, "()I", "()I");
    if (mid == nullptr || minecraft == nullptr) {
        return 120;
    }
    return env->CallIntMethod(minecraft, mid);
}

// ---------------------------------------------------------------------------------------------------------------
// Hook table
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* kMinecraft = "net/minecraft/client/Minecraft";
constexpr const char* kEntityRenderer = "net/minecraft/client/renderer/EntityRenderer";
constexpr const char* kGuiScreen = "net/minecraft/client/gui/GuiScreen";
constexpr const char* kForgeHooksClient = "net/minecraftforge/client/ForgeHooksClient";

constexpr bytecode::ExtraMemberNames kUpdateCameraAndRender{
    "a", "(FJ)V", "updateCameraAndRender", "(FJ)V", "func_181560_a", "(FJ)V"};

// Vanilla / OptiFine / MCP: this.mc.currentScreen.drawScreen(k1, l1, partialTicks)
const ExtraHookSpec kHookDrawScreen{
    "bfk", kEntityRenderer,
    kUpdateCameraAndRender,
    "axu", kGuiScreen,
    {"a", "(IIF)V", "drawScreen", "(IIF)V", "func_73863_a", "(IIF)V"},
    HookInsnKind::InvokeInstance,
    "adjustMenuDrawScreen", "(Ljava/lang/Object;IIF)V",
    reinterpret_cast<void*>(&native_draw_screen),
};

// Forge: net.minecraftforge.client.ForgeHooksClient.drawScreen(this.mc.currentScreen, k1, l1, partialTicks)
const ExtraHookSpec kHookForgeDrawScreen{
    "bfk", kEntityRenderer,
    kUpdateCameraAndRender,
    nullptr, kForgeHooksClient,
    {nullptr, nullptr,
     "drawScreen", "(Lnet/minecraft/client/gui/GuiScreen;IIF)V",
     "drawScreen", "(Lnet/minecraft/client/gui/GuiScreen;IIF)V"},
    HookInsnKind::InvokeStatic,
    "adjustMenuForgeDrawScreen", "(Ljava/lang/Object;IIF)V",
    reinterpret_cast<void*>(&native_forge_draw_screen),
};

// Minecraft.runTick: this.currentScreen.handleInput()
const ExtraHookSpec kHookHandleInput{
    "ave", kMinecraft,
    {"s", "()V", "runTick", "()V", "func_71407_l", "()V"},
    "axu", kGuiScreen,
    {"p", "()V", "handleInput", "()V", "func_146269_k", "()V"},
    HookInsnKind::InvokeInstance,
    "adjustMenuHandleInput", "(Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_handle_input),
};

// Minecraft.runGameLoop: Display.sync(this.getLimitFramerate())
const ExtraHookSpec kHookLimitFramerate{
    "ave", kMinecraft,
    {"av", "()V", "runGameLoop", "()V", "func_71411_J", "()V"},
    "ave", kMinecraft,
    {"j", "()I", "getLimitFramerate", "()I", "func_90020_K", "()I"},
    HookInsnKind::InvokeInstance,
    "adjustMenuGetLimitFramerate", "(Ljava/lang/Object;)I",
    reinterpret_cast<void*>(&native_get_limit_framerate),
};

} // namespace

const bytecode::ExtraHookSpec* const* mainmenu_hooks() noexcept {
    static const ExtraHookSpec* const s_hooks[] = {
        &kHookDrawScreen,
        &kHookForgeDrawScreen,
        &kHookHandleInput,
        &kHookLimitFramerate,
        nullptr,
    };
    return s_hooks;
}

} // namespace slinky::adjust::hooks
