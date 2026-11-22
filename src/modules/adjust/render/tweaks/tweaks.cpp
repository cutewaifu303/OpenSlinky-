// Port of com.adjustclient.module.impl.render.tweaks.* (TweaksModule and its tweaks):
//   ScoreboardTweak, ChatTweak (+ ChatLineKey / ChatStackEntry / StackedChatComponent), BowTweak, TabOverlayTweak,
//   ZoomTweak, NoRenderTweak, SmoothRotationYawTweak, MinimalBobbingTweak, DisconnectConfirmTweak,
//   NoHotbarScrollTweak, FullscreenTweak.
//
// Adjust patched Minecraft's source for most tweaks; here those parts are bytecode hooks in
// src/adjust/hooks/tweaks_hooks.cpp that query this module. This file keeps the settings and the parts driven by
// Adjust events: BowTweak (IngameActionEvent -> kEventGameTick, which runs from Minecraft.runTick before the input
// handling), ChatTweak.filterLobbyMessage (DirectionalPacketEvent -> kEventIncomingPacket), the setting listeners
// (stack reset, FullscreenTweak REFRESH) and the GuiIngameMenu disconnect timer.
//
// Not ported: ChatTweak "Modern suggestions" (Adjust's own CommandSuggestions chat-completion UI, ~600 lines of
// patched GuiChat code) and the Adjust chat channels (global chat backend).

#include "modules/adjust/render/tweaks/tweaks.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "events/module_manager.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/streamer/streamer.hpp"

namespace slinky::modules {

namespace {

void on_game_tick(void* event) noexcept;
void on_incoming_packet(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct TweaksState {
    events::Module base{};

    // ScoreboardTweak ("Scoreboard")
    config::BoolSetting sb_enabled{{}, "Enabled", true, true, {}};
    config::BoolSetting sb_none{{}, "No scoreboard", false, false, {}};
    config::BoolSetting sb_remove_scores{{}, "Remove scores", true, true, {}};
    config::BoolSetting sb_transparent{{}, "Transparent", false, false, {}};

    // ChatTweak ("Chat")
    config::BoolSetting chat_enabled{{}, "Enabled", true, true, {}};
    config::BoolSetting stack_enabled{{}, "Enabled", true, true, {}};                    // "Message stacking"
    config::FloatSetting stack_timeout{{}, "Timeout", "%.1fs", 0.5f, 0.0f, 3.0f, 15.0f, 5.0f, 5.0f};
    config::BoolSetting chat_transparent{{}, "Transparent", false, false, {}};
    config::BoolSetting chat_no_lobby{{}, "No lobby messages", false, false, {}};
    config::BoolSetting chat_heads{{}, "Player heads", true, true, {}};

    // BowTweak ("Bow")
    config::BoolSetting bow_left_click{{}, "Left click shoot", false, false, {}};
    config::BoolSetting bow_full_charge{{}, "On full charge only", false, false, {}};

    // TabOverlayTweak ("Tab Overlay")
    config::BoolSetting tab_remove_npcs{{}, "Remove NPCs", true, true, {}};

    // ZoomTweak ("Zoom")
    config::BoolSetting zoom_no_cinematic{{}, "No cinematic", true, true, {}};
    config::BoolSetting zoom_scroll{{}, "Scroll zoom", true, true, {}};

    // NoRenderTweak ("No render")
    config::BoolSetting nr_title{{}, "Title", false, false, {}};
    config::BoolSetting nr_boss_bar{{}, "Boss bar", false, false, {}};
    config::BoolSetting nr_achievements{{}, "Achievement notifications", true, true, {}};

    config::BoolSetting smooth_yaw{{}, "Smooth rotation yaw", false, false, {}};
    config::BoolSetting minimal_bobbing{{}, "Minimal bobbing", false, false, {}};
    config::BoolSetting disconnect_confirm{{}, "Disconnect confirm", false, false, {}};
    config::BoolSetting no_hotbar_scroll{{}, "No hotbar scroll", false, false, {}};
    config::BoolSetting borderless{{}, "Borderless fullscreen", false, false, {}};

    // ZoomTweak.zoom
    float zoom = 4.0f;

    // ChatTweak stack map resets (consumed by the chat hooks)
    std::atomic<uint32_t> chat_generation{1};
    bool last_stacking = true;
    jweak last_world = nullptr;
    bool world_seen = false;

    // ChatTweak.filterLobbyMessage: ServerTracker serverType == HYPIXEL
    std::atomic<bool> on_hypixel{false};
    int server_check_ticks = 0;

    // FullscreenTweak REFRESH
    bool fullscreen_check = true;  // re-evaluate on the next tick
    bool last_borderless = false;
    std::atomic<bool> borderless_applied{false};

    // DisconnectConfirmTweak: GuiIngameMenu.disconnectConfirmTimer (one Timer per menu instance, started when the
    // menu is created)
    jweak menu = nullptr;
    int64_t menu_timer_ms = 0;

    TweaksState() noexcept {
        base.name = "Tweaks";
        base.description = "Small quality of life tweaks for the game.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust; // TweaksModule.setHidden(false)
        base.events[events::kEventGameTick] = {on_game_tick, 75, 0}; // BowTweak @EventHandler(priority = 75)
        base.events[events::kEventIncomingPacket] = {on_incoming_packet, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Tweaks");
    }
};

TweaksState g_state{};

bool running() noexcept {
    return events::module_is_running(&g_state.base);
}

void bump_chat_generation() noexcept {
    g_state.chat_generation.fetch_add(1, std::memory_order_acq_rel);
}

// ---- Minecraft IDs --------------------------------------------------------------------------------------------

struct TickIds {
    bool ok = false;
    jfieldID mc_current_screen = nullptr;
    jfieldID mc_fullscreen = nullptr;
    jmethodID mc_toggle_fullscreen = nullptr;
    jmethodID mc_get_current_server_data = nullptr;
    jfieldID server_ip = nullptr;
    jclass gui_ingame_menu = nullptr;
    // BowTweak
    jmethodID player_get_item_in_use = nullptr;
    jmethodID player_get_item_in_use_count = nullptr;
    jmethodID stack_get_item = nullptr;
    jmethodID stack_get_max_use_duration = nullptr;
    jclass item_bow = nullptr;
    jmethodID controller_stop_using = nullptr;
    jfieldID living_swing_progress_int = nullptr;
    jfieldID living_is_swing_in_progress = nullptr;
    jmethodID living_get_arm_swing_end = nullptr;
};

const TickIds& tick_ids(events::JniContext* jni) noexcept {
    static const TickIds s_ids = [&] {
        TickIds ids{};
        namespace aj = adjust::jni;
        const jclass mc = aj::find_class(jni, "ave", "net/minecraft/client/Minecraft");
        ids.mc_current_screen = aj::field(jni, mc, {"m", "currentScreen", "field_71462_r"}, "Laxu;",
                                          "Lnet/minecraft/client/gui/GuiScreen;");
        ids.mc_fullscreen = aj::field(jni, mc, {"T", "fullscreen", "field_71431_Q"}, "Z", "Z");
        ids.mc_toggle_fullscreen = aj::method(jni, mc, {"q", "toggleFullscreen", "func_71352_k"}, "()V", "()V");
        ids.mc_get_current_server_data =
            aj::method(jni, mc, {"D", "getCurrentServerData", "func_147104_D"}, "()Lbde;",
                       "()Lnet/minecraft/client/multiplayer/ServerData;");
        const jclass server_data = aj::find_class(jni, "bde", "net/minecraft/client/multiplayer/ServerData");
        ids.server_ip = aj::field(jni, server_data, {"b", "serverIP", "field_78845_b"}, "Ljava/lang/String;",
                                  "Ljava/lang/String;");
        ids.gui_ingame_menu = aj::find_class(jni, "axp", "net/minecraft/client/gui/GuiIngameMenu");

        const jclass player = aj::find_class(jni, "wn", "net/minecraft/entity/player/EntityPlayer");
        ids.player_get_item_in_use = aj::method(jni, player, {"bQ", "getItemInUse", "func_71011_bu"}, "()Lzx;",
                                                "()Lnet/minecraft/item/ItemStack;");
        ids.player_get_item_in_use_count =
            aj::method(jni, player, {"bR", "getItemInUseCount", "func_71052_bv"}, "()I", "()I");
        const jclass stack = aj::find_class(jni, "zx", "net/minecraft/item/ItemStack");
        ids.stack_get_item =
            aj::method(jni, stack, {"b", "getItem", "func_77973_b"}, "()Lzw;", "()Lnet/minecraft/item/Item;");
        ids.stack_get_max_use_duration =
            aj::method(jni, stack, {"l", "getMaxItemUseDuration", "func_77988_m"}, "()I", "()I");
        ids.item_bow = aj::find_class(jni, "yt", "net/minecraft/item/ItemBow");
        const jclass controller = aj::find_class(jni, "bda", "net/minecraft/client/multiplayer/PlayerControllerMP");
        ids.controller_stop_using = aj::method(jni, controller, {"c", "onStoppedUsingItem", "func_78766_c"},
                                               "(Lwn;)V", "(Lnet/minecraft/entity/player/EntityPlayer;)V");
        const jclass living = aj::find_class(jni, "pr", "net/minecraft/entity/EntityLivingBase");
        ids.living_swing_progress_int = aj::field(jni, living, {"as", "swingProgressInt", "field_110158_av"}, "I", "I");
        ids.living_is_swing_in_progress =
            aj::field(jni, living, {"ar", "isSwingInProgress", "field_82175_bq"}, "Z", "Z");
        ids.living_get_arm_swing_end =
            aj::method(jni, living, {"n", "getArmSwingAnimationEnd", "func_82166_i"}, "()I", "()I");
        ids.ok = ids.mc_current_screen != nullptr && ids.mc_fullscreen != nullptr;
        return ids;
    }();
    return s_ids;
}

// IDs used on the Netty thread by filterLobbyMessage. Resolved on the main thread (tick) only.
struct LobbyIds {
    jclass packet_chat = nullptr;
    jfieldID packet_component = nullptr;
    jfieldID packet_type = nullptr;
    jmethodID component_get_siblings = nullptr;
    jmethodID component_get_style = nullptr;
    jmethodID component_get_formatted = nullptr;
    jmethodID component_get_unformatted = nullptr;
    jmethodID style_get_click = nullptr;
    jmethodID click_get_action = nullptr;
    jmethodID click_get_value = nullptr;
    jobject action_run_command = nullptr; // global ref
    jmethodID list_size = nullptr;
    jmethodID list_get = nullptr;
};

LobbyIds g_lobby{};
std::atomic<bool> g_lobby_ready{false};

void resolve_lobby_ids(events::JniContext* jni) noexcept {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    namespace aj = adjust::jni;
    JNIEnv* env = jni->env;
    LobbyIds ids{};
    ids.packet_chat = aj::find_class(jni, "fy", "net/minecraft/network/play/server/S02PacketChat");
    ids.packet_component = aj::field(jni, ids.packet_chat, {"a", "chatComponent", "field_148919_a"}, "Leu;",
                                     "Lnet/minecraft/util/IChatComponent;");
    ids.packet_type = aj::field(jni, ids.packet_chat, {"b", "type", "field_179842_b"}, "B", "B");
    const jclass component = aj::find_class(jni, "eu", "net/minecraft/util/IChatComponent");
    ids.component_get_siblings =
        aj::method(jni, component, {"a", "getSiblings", "func_150253_a"}, "()Ljava/util/List;", "()Ljava/util/List;");
    ids.component_get_style = aj::method(jni, component, {"b", "getChatStyle", "func_150256_b"}, "()Lez;",
                                         "()Lnet/minecraft/util/ChatStyle;");
    ids.component_get_formatted = aj::method(jni, component, {"d", "getFormattedText", "func_150254_d"},
                                             "()Ljava/lang/String;", "()Ljava/lang/String;");
    ids.component_get_unformatted = aj::method(jni, component, {"c", "getUnformattedText", "func_150260_c"},
                                               "()Ljava/lang/String;", "()Ljava/lang/String;");
    const jclass style = aj::find_class(jni, "ez", "net/minecraft/util/ChatStyle");
    ids.style_get_click = aj::method(jni, style, {"h", "getChatClickEvent", "func_150235_h"}, "()Let;",
                                     "()Lnet/minecraft/event/ClickEvent;");
    const jclass click = aj::find_class(jni, "et", "net/minecraft/event/ClickEvent");
    ids.click_get_action = aj::method(jni, click, {"a", "getAction", "func_150669_a"}, "()Let$a;",
                                      "()Lnet/minecraft/event/ClickEvent$Action;");
    ids.click_get_value =
        aj::method(jni, click, {"b", "getValue", "func_150668_b"}, "()Ljava/lang/String;", "()Ljava/lang/String;");
    const jclass action = aj::find_class(jni, "et$a", "net/minecraft/event/ClickEvent$Action");
    const jfieldID run_command = aj::static_field(jni, action, {"c", "RUN_COMMAND", "RUN_COMMAND"}, "Let$a;",
                                                  "Lnet/minecraft/event/ClickEvent$Action;");
    if (action != nullptr && run_command != nullptr) {
        jobject value = env->GetStaticObjectField(action, run_command);
        if (value != nullptr) {
            ids.action_run_command = env->NewGlobalRef(value);
            env->DeleteLocalRef(value);
        }
    }
    jclass list = env->FindClass("java/util/List");
    if (list != nullptr) {
        ids.list_size = env->GetMethodID(list, "size", "()I");
        ids.list_get = env->GetMethodID(list, "get", "(I)Ljava/lang/Object;");
        env->DeleteLocalRef(list);
    }
    aj::clear_exception(jni);
    if (ids.packet_chat == nullptr || ids.packet_component == nullptr || ids.packet_type == nullptr ||
        ids.component_get_siblings == nullptr || ids.component_get_style == nullptr ||
        ids.component_get_formatted == nullptr || ids.component_get_unformatted == nullptr ||
        ids.style_get_click == nullptr || ids.click_get_action == nullptr || ids.click_get_value == nullptr ||
        ids.action_run_command == nullptr || ids.list_size == nullptr || ids.list_get == nullptr) {
        return;
    }
    g_lobby = ids;
    g_lobby_ready.store(true, std::memory_order_release);
}

std::string to_utf8(JNIEnv* env, jstring str) {
    std::string out;
    if (str == nullptr) {
        return out;
    }
    const char* chars = env->GetStringUTFChars(str, nullptr);
    if (chars != nullptr) {
        out.assign(chars);
        env->ReleaseStringUTFChars(str, chars);
    }
    return out;
}

bool contains_ignore_case(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// ---- tick ---------------------------------------------------------------------------------------------------

// ChatTweak.shouldClearStack (WorldResetEvent) + the stackingEnabled listener.
void update_chat_resets(JNIEnv* env, jobject world) noexcept {
    const bool stacking = g_state.stack_enabled.value;
    if (stacking != g_state.last_stacking) {
        g_state.last_stacking = stacking;
        bump_chat_generation();
    }
    const bool same_world = g_state.world_seen && ((g_state.last_world == nullptr && world == nullptr) ||
                                                   (g_state.last_world != nullptr && world != nullptr &&
                                                    env->IsSameObject(g_state.last_world, world) == JNI_TRUE));
    if (same_world) {
        return;
    }
    if (g_state.last_world != nullptr) {
        env->DeleteWeakGlobalRef(g_state.last_world);
        g_state.last_world = nullptr;
    }
    if (world != nullptr) {
        g_state.last_world = env->NewWeakGlobalRef(world);
    }
    if (g_state.world_seen && stacking) {
        bump_chat_generation();
    }
    g_state.world_seen = true;
}

void update_server_type(events::JniContext* jni, jobject mc, const TickIds& ids) noexcept {
    if (--g_state.server_check_ticks > 0) {
        return;
    }
    g_state.server_check_ticks = 20;
    bool hypixel = false;
    if (ids.mc_get_current_server_data != nullptr && ids.server_ip != nullptr) {
        JNIEnv* env = jni->env;
        jobject data = env->CallObjectMethod(mc, ids.mc_get_current_server_data);
        if (adjust::jni::clear_exception(jni)) {
            data = nullptr;
        }
        if (data != nullptr) {
            auto ip = static_cast<jstring>(env->GetObjectField(data, ids.server_ip));
            const std::string address = to_utf8(env, ip);
            hypixel = contains_ignore_case(address, "hypixel.net");
            if (ip != nullptr) {
                env->DeleteLocalRef(ip);
            }
            env->DeleteLocalRef(data);
        }
    }
    g_state.on_hypixel.store(hypixel, std::memory_order_release);
}

// FullscreenTweak REFRESH: `if (mc.fullscreen) mc.setFullscreen(true)`. 1.8.9 has no setFullscreen(boolean), so the
// window leaves and re-enters fullscreen (Minecraft.toggleFullscreen twice; F11 does the same from runTick). The
// tweaks hook on Display.setFullscreen applies or removes the borderless window.
void update_fullscreen(events::JniContext* jni, jobject mc, const TickIds& ids) noexcept {
    const bool desired = adjust_tweaks::borderless_fullscreen();
    if (!g_state.fullscreen_check && desired == g_state.last_borderless) {
        return;
    }
    g_state.fullscreen_check = false;
    g_state.last_borderless = desired;
    if (ids.mc_toggle_fullscreen == nullptr || jni->env->GetBooleanField(mc, ids.mc_fullscreen) == JNI_FALSE ||
        desired == g_state.borderless_applied.load(std::memory_order_acquire)) {
        return; // windowed, or the fullscreen window already has the wanted kind
    }
    jni->env->CallVoidMethod(mc, ids.mc_toggle_fullscreen);
    if (adjust::jni::clear_exception(jni)) {
        return;
    }
    jni->env->CallVoidMethod(mc, ids.mc_toggle_fullscreen);
    adjust::jni::clear_exception(jni);
}

// GuiIngameMenu.disconnectConfirmTimer is created with the menu: start a timer for every new menu instance.
void update_menu_timer(JNIEnv* env, jobject mc, const TickIds& ids) noexcept {
    if (ids.gui_ingame_menu == nullptr) {
        return;
    }
    jobject screen = env->GetObjectField(mc, ids.mc_current_screen);
    if (screen == nullptr) {
        return;
    }
    if (env->IsInstanceOf(screen, ids.gui_ingame_menu) &&
        (g_state.menu == nullptr || env->IsSameObject(g_state.menu, screen) != JNI_TRUE)) {
        if (g_state.menu != nullptr) {
            env->DeleteWeakGlobalRef(g_state.menu);
        }
        g_state.menu = env->NewWeakGlobalRef(screen);
        g_state.menu_timer_ms = now_ms();
    }
    env->DeleteLocalRef(screen);
}

// BowTweak.onTick (IngameActionEvent, posted right before Minecraft handles the use-item key while an item is in
// use). Adjust disables the right mouse button for that tick, so Minecraft releases the bow; here the release is
// done directly through PlayerControllerMP.onStoppedUsingItem.
void update_bow(events::JniContext* jni, jobject mc, jobject player, const TickIds& ids) noexcept {
    if (!g_state.bow_left_click.value || ids.player_get_item_in_use == nullptr || ids.stack_get_item == nullptr ||
        ids.item_bow == nullptr || ids.controller_stop_using == nullptr) {
        return;
    }
    JNIEnv* env = jni->env;
    jobject in_use = env->CallObjectMethod(player, ids.player_get_item_in_use);
    if (adjust::jni::clear_exception(jni) || in_use == nullptr) {
        return;
    }
    jobject item = env->CallObjectMethod(in_use, ids.stack_get_item);
    if (adjust::jni::clear_exception(jni) || item == nullptr) {
        return;
    }
    jclass item_class = env->GetObjectClass(item);
    const bool is_bow = env->IsSameObject(item_class, ids.item_bow) == JNI_TRUE; // getClass() == ItemBow.class
    env->DeleteLocalRef(item_class);
    if (!is_bow) {
        return;
    }

    jobject settings = minecraft::mc_get_game_settings(jni, mc);
    if (settings == nullptr) {
        return;
    }
    jobject attack = minecraft::settings_get_key_attack(jni, settings);
    if (attack == nullptr) {
        return;
    }
    const int press_time = minecraft::key_binding_get_press_time(jni, attack);
    if (g_state.bow_full_charge.value) {
        const int max_duration = ids.stack_get_max_use_duration != nullptr
                                     ? env->CallIntMethod(in_use, ids.stack_get_max_use_duration)
                                     : 0;
        const int remaining =
            ids.player_get_item_in_use_count != nullptr ? env->CallIntMethod(player, ids.player_get_item_in_use_count) : 0;
        if (adjust::jni::clear_exception(jni) || max_duration - remaining < 20) {
            return;
        }
        // left.isPressed(false) || left.isClicked(false)
        if (press_time < 1 && !minecraft::key_binding_is_pressed(jni, attack)) {
            return;
        }
    } else if (press_time < 1) { // left.isPressed(false)
        return;
    }

    // mouseClickHelper.right.disabled = true -> Minecraft stops using the item this tick.
    jobject controller = minecraft::mc_get_player_controller(jni, mc);
    if (controller == nullptr) {
        return;
    }
    env->CallVoidMethod(controller, ids.controller_stop_using, player);
    if (adjust::jni::clear_exception(jni)) {
        return;
    }
    // Minecraft consumes the left clicks while an item is in use (`while (left.isPressed(true));`).
    minecraft::key_binding_set_press_time(jni, attack, 0);

    if (ids.living_swing_progress_int == nullptr || ids.living_is_swing_in_progress == nullptr ||
        ids.living_get_arm_swing_end == nullptr) {
        return;
    }
    const bool swinging = env->GetBooleanField(player, ids.living_is_swing_in_progress) != JNI_FALSE;
    const int progress = env->GetIntField(player, ids.living_swing_progress_int);
    const int swing_end = env->CallIntMethod(player, ids.living_get_arm_swing_end);
    if (adjust::jni::clear_exception(jni)) {
        return;
    }
    if (!swinging || progress >= swing_end / 2 || progress < 0) {
        env->SetIntField(player, ids.living_swing_progress_int, -1);
        env->SetBooleanField(player, ids.living_is_swing_in_progress, JNI_TRUE);
    }
}

void on_game_tick(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->jni->env == nullptr || ctx->minecraft == nullptr ||
        !adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    const TickIds& ids = tick_ids(jni);
    if (!ids.ok) {
        return;
    }
    resolve_lobby_ids(jni);
    if (env->PushLocalFrame(32) != 0) {
        env->ExceptionClear();
        return;
    }
    update_chat_resets(env, ctx->world);
    update_server_type(jni, ctx->minecraft, ids);
    update_menu_timer(env, ctx->minecraft, ids);
    if (ctx->local_player != nullptr) {
        update_bow(jni, ctx->minecraft, ctx->local_player, ids);
    }
    update_fullscreen(jni, ctx->minecraft, ids);
    adjust::jni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

// ---- ChatTweak.filterLobbyMessage (Netty thread) ------------------------------------------------------------

// StreamerModule.extractProfileClickName on one component.
bool profile_click_sender(JNIEnv* env, jobject component) noexcept {
    const LobbyIds& ids = g_lobby;
    jobject style = env->CallObjectMethod(component, ids.component_get_style);
    if (env->ExceptionCheck() || style == nullptr) {
        env->ExceptionClear();
        return false;
    }
    bool found = false;
    jobject click = env->CallObjectMethod(style, ids.style_get_click);
    if (!env->ExceptionCheck() && click != nullptr) {
        jobject action = env->CallObjectMethod(click, ids.click_get_action);
        if (!env->ExceptionCheck() && action != nullptr && env->IsSameObject(action, ids.action_run_command)) {
            auto value = static_cast<jstring>(env->CallObjectMethod(click, ids.click_get_value));
            if (!env->ExceptionCheck() && value != nullptr) {
                std::string name;
                found = adjust_streamer::profile_click_name(to_utf8(env, value), &name);
                env->DeleteLocalRef(value);
            }
        }
        if (action != nullptr) {
            env->DeleteLocalRef(action);
        }
        env->DeleteLocalRef(click);
    }
    env->ExceptionClear();
    env->DeleteLocalRef(style);
    return found;
}

// StreamerModule.getChatSenderName(component) != null
bool has_chat_sender(JNIEnv* env, jobject component) noexcept {
    const LobbyIds& ids = g_lobby;
    if (profile_click_sender(env, component)) {
        return true;
    }
    jobject siblings = env->CallObjectMethod(component, ids.component_get_siblings);
    if (!env->ExceptionCheck() && siblings != nullptr) {
        const jint count = env->CallIntMethod(siblings, ids.list_size);
        for (jint i = 0; !env->ExceptionCheck() && i < count; ++i) {
            jobject sibling = env->CallObjectMethod(siblings, ids.list_get, i);
            if (env->ExceptionCheck() || sibling == nullptr) {
                break;
            }
            const bool found = profile_click_sender(env, sibling);
            env->DeleteLocalRef(sibling);
            if (found) {
                env->DeleteLocalRef(siblings);
                return true;
            }
        }
        env->DeleteLocalRef(siblings);
    }
    env->ExceptionClear();
    auto formatted = static_cast<jstring>(env->CallObjectMethod(component, ids.component_get_formatted));
    if (env->ExceptionCheck() || formatted == nullptr) {
        env->ExceptionClear();
        return false;
    }
    std::string name;
    const bool found = adjust_streamer::chat_sender_name(to_utf8(env, formatted), &name);
    env->DeleteLocalRef(formatted);
    return found;
}

void on_incoming_packet(void* event) noexcept {
    auto* ev = static_cast<events::NettyPacketEvent*>(event);
    if (ev->action != 0 || ev->jni == nullptr || ev->jni->env == nullptr || ev->packet == nullptr ||
        !g_state.chat_no_lobby.value || !g_lobby_ready.load(std::memory_order_acquire)) {
        return;
    }
    JNIEnv* env = ev->jni->env;
    const LobbyIds& ids = g_lobby;
    if (!env->IsInstanceOf(ev->packet, ids.packet_chat)) {
        return;
    }
    if (env->GetByteField(ev->packet, ids.packet_type) == 2) { // action bar
        return;
    }
    if (!g_state.on_hypixel.load(std::memory_order_acquire)) {
        return;
    }
    jobject component = env->GetObjectField(ev->packet, ids.packet_component);
    if (component == nullptr) {
        return;
    }
    bool drop = false;
    if (has_chat_sender(env, component)) {
        auto plain = static_cast<jstring>(env->CallObjectMethod(component, ids.component_get_unformatted));
        if (!env->ExceptionCheck() && plain != nullptr) {
            const jsize length = env->GetStringLength(plain);
            if (length >= 6) {
                jchar head[3]{};
                env->GetStringRegion(plain, 0, 3, head);
                if (head[0] == 0x00A7 && head[2] == u'[') {
                    const std::string text = to_utf8(env, plain);
                    drop = text.find("\xC2\xA7" "f" "\xC2\xA7" "6") != std::string::npos ||
                           text.find("\xC2\xA7" "f " "\xC2\xA7" "6") != std::string::npos;
                }
            }
        }
        if (plain != nullptr) {
            env->DeleteLocalRef(plain);
        }
        env->ExceptionClear();
    }
    env->DeleteLocalRef(component);
    if (drop) {
        ev->action = 2; // event.cancelled = true
    }
}

// TweaksModule.onEnable / onDisable -> every Tweak.onEnable / onDisable.
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_state.base) {
        return;
    }
    bump_chat_generation(); // ChatTweak.rebuildChannels / clearSuggestions
    if (!ev->enabled) {
        g_state.zoom = 4.0f; // ZoomTweak.onDisable -> resetZoom
    }
    g_state.fullscreen_check = true; // FullscreenTweak REFRESH (applied by the next tick while running)
}

// ---- settings -----------------------------------------------------------------------------------------------

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Scoreboard");
    g_state.sb_enabled.render(gui, renderer);
    g_state.sb_none.render(gui, renderer);
    if (!g_state.sb_none.value) {
        g_state.sb_remove_scores.render(gui, renderer);
        g_state.sb_transparent.render(gui, renderer);
    }

    gui::widgets::render_section_header(gui, renderer, "Chat");
    g_state.chat_enabled.render(gui, renderer);
    gui::widgets::render_section_header(gui, renderer, "Message stacking");
    g_state.stack_enabled.render(gui, renderer);
    g_state.stack_timeout.render(gui, renderer);
    g_state.chat_transparent.render(gui, renderer);
    g_state.chat_no_lobby.render(gui, renderer);
    g_state.chat_heads.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Bow");
    g_state.bow_left_click.render(gui, renderer);
    if (g_state.bow_left_click.value) {
        g_state.bow_full_charge.render(gui, renderer);
    }

    gui::widgets::render_section_header(gui, renderer, "Tab Overlay");
    g_state.tab_remove_npcs.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Zoom");
    g_state.zoom_no_cinematic.render(gui, renderer);
    g_state.zoom_scroll.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "No render");
    g_state.nr_title.render(gui, renderer);
    g_state.nr_boss_bar.render(gui, renderer);
    g_state.nr_achievements.render(gui, renderer);

    gui::widgets::render_section_header(gui, renderer, "Other");
    g_state.smooth_yaw.render(gui, renderer);
    g_state.minimal_bobbing.render(gui, renderer);
    g_state.disconnect_confirm.render(gui, renderer);
    g_state.no_hotbar_scroll.render(gui, renderer);
    g_state.borderless.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.sb_enabled.visit(archive, "scoreboard_enabled");
    g_state.sb_none.visit(archive, "scoreboard_no_scoreboard");
    g_state.sb_remove_scores.visit(archive, "scoreboard_remove_scores");
    g_state.sb_transparent.visit(archive, "scoreboard_transparent");
    g_state.chat_enabled.visit(archive, "chat_enabled");
    g_state.stack_enabled.visit(archive, "chat_stacking_enabled");
    g_state.stack_timeout.visit(archive, "chat_stacking_timeout");
    g_state.chat_transparent.visit(archive, "chat_transparent");
    g_state.chat_no_lobby.visit(archive, "chat_no_lobby_messages");
    g_state.chat_heads.visit(archive, "chat_player_heads");
    g_state.bow_left_click.visit(archive, "bow_left_click_shoot");
    g_state.bow_full_charge.visit(archive, "bow_on_full_charge_only");
    g_state.tab_remove_npcs.visit(archive, "tab_overlay_remove_npcs");
    g_state.zoom_no_cinematic.visit(archive, "zoom_no_cinematic");
    g_state.zoom_scroll.visit(archive, "zoom_scroll_zoom");
    g_state.nr_title.visit(archive, "no_render_title");
    g_state.nr_boss_bar.visit(archive, "no_render_boss_bar");
    g_state.nr_achievements.visit(archive, "no_render_achievement_notifications");
    g_state.smooth_yaw.visit(archive, "smooth_rotation_yaw");
    g_state.minimal_bobbing.visit(archive, "minimal_bobbing");
    g_state.disconnect_confirm.visit(archive, "disconnect_confirm");
    g_state.no_hotbar_scroll.visit(archive, "no_hotbar_scroll");
    g_state.borderless.visit(archive, "borderless_fullscreen");
}

} // namespace

events::Module* get_adjust_tweaks_module() noexcept {
    return &g_state.base;
}

namespace adjust_tweaks {

// ScoreboardTweak.enabledSetting(): module.isEnabled() && enabled
bool scoreboard_hidden() noexcept {
    return running() && g_state.sb_enabled.value && g_state.sb_none.value;
}
bool scoreboard_transparent() noexcept {
    return running() && g_state.sb_enabled.value && g_state.sb_transparent.value;
}
bool scoreboard_remove_scores() noexcept {
    return running() && g_state.sb_enabled.value && g_state.sb_remove_scores.value;
}

// TweaksModule.isEnabled() && chat.isModernSuggestionsEnabled() (= chat "Enabled") && chat.transparent
bool chat_transparent() noexcept {
    return running() && g_state.chat_enabled.value && g_state.chat_transparent.value;
}
// ChatTweak.isStackingEnabled(): module.isEnabled() && enabled && playerHeads
bool chat_player_heads() noexcept {
    return running() && g_state.chat_enabled.value && g_state.chat_heads.value;
}
// The stack map only exists while the module is enabled (onEnable -> rebuildChannels, onDisable -> clear).
bool chat_stacking() noexcept {
    return running() && g_state.stack_enabled.value;
}
int64_t chat_stack_timeout_ms() noexcept {
    return static_cast<int64_t>(std::clamp(g_state.stack_timeout.value, 3.0f, 15.0f) * 1000.0f);
}
uint32_t chat_reset_generation() noexcept {
    return g_state.chat_generation.load(std::memory_order_acquire);
}

// NoRenderTweak.enabledSetting() is always true -> isEnabled() == module.isEnabled()
bool hide_title() noexcept {
    return running() && g_state.nr_title.value;
}
bool hide_boss_bar() noexcept {
    return running() && g_state.nr_boss_bar.value;
}
bool hide_achievements() noexcept {
    return running() && g_state.nr_achievements.value;
}

bool tab_remove_npcs() noexcept {
    return running() && g_state.tab_remove_npcs.value;
}
bool smooth_rotation_yaw() noexcept {
    return running() && g_state.smooth_yaw.value;
}
bool minimal_bobbing() noexcept {
    return running() && g_state.minimal_bobbing.value;
}
bool no_hotbar_scroll() noexcept {
    return running() && g_state.no_hotbar_scroll.value;
}
bool borderless_fullscreen() noexcept {
    return running() && g_state.borderless.value;
}
void set_borderless_applied(bool applied) noexcept {
    g_state.borderless_applied.store(applied, std::memory_order_release);
}

// GuiIngameMenu.actionPerformed case 1: `if (disconnectConfirm.isEnabled() && timer.hasElapsed(1500L))
// { timer.reset(); return; }`
bool disconnect_confirm_swallow(JNIEnv* env, jobject menu) noexcept {
    if (!running() || !g_state.disconnect_confirm.value || env == nullptr || menu == nullptr) {
        return false;
    }
    if (g_state.menu == nullptr || env->IsSameObject(g_state.menu, menu) != JNI_TRUE) {
        // Not seen by a tick yet (opened this tick): its timer was started now.
        if (g_state.menu != nullptr) {
            env->DeleteWeakGlobalRef(g_state.menu);
        }
        g_state.menu = env->NewWeakGlobalRef(menu);
        g_state.menu_timer_ms = now_ms();
    }
    const int64_t now = now_ms();
    if (g_state.menu_timer_ms + 1500 <= now) {
        g_state.menu_timer_ms = now;
        return true;
    }
    return false;
}

// ZoomTweak.enabledSetting(): module.isEnabled() && isZoomActive()
bool zoom_enabled() noexcept {
    return running() && (g_state.zoom_no_cinematic.value || g_state.zoom_scroll.value);
}
bool zoom_no_cinematic() noexcept {
    return zoom_enabled() && g_state.zoom_no_cinematic.value;
}
bool zoom_scroll() noexcept {
    return zoom_enabled() && g_state.zoom_scroll.value;
}
// ZoomTweak.getZoom()
float zoom_factor() noexcept {
    return zoom_scroll() ? g_state.zoom : 4.0f;
}
// ZoomTweak.updateZoom (MouseWheelEvent)
void zoom_on_wheel(int delta) noexcept {
    if (!zoom_scroll()) {
        return;
    }
    if (delta > 0) {
        g_state.zoom *= 1.25f;
    } else if (delta < 0) {
        g_state.zoom *= 0.75f;
    }
    g_state.zoom = std::max(1.0f, std::min(600.0f, g_state.zoom));
}
void zoom_reset() noexcept {
    g_state.zoom = 4.0f;
}

} // namespace adjust_tweaks
} // namespace slinky::modules
