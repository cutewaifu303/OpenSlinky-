// Adjust "Tweaks" hooks (Minecraft 1.8.9). Each native replaces one call site / field read that Adjust patched in
// Minecraft's source and calls the original member while the corresponding tweak is off:
//
//   NoRenderTweak         GuiIngame.renderGameOverlay: getfield titlesTimer, invoke renderBossHealth
//                         Minecraft.runGameLoop: invoke GuiAchievement.updateAchievementWindow
//   ScoreboardTweak       GuiIngame.renderGameOverlay: invoke renderScoreboard (No scoreboard)
//                         GuiIngame.renderScoreboard: Gui.drawRect (Transparent), FontRenderer.drawString and
//                         FontRenderer.getStringWidth (Remove scores)
//   ChatTweak             GuiNewChat.drawChat: Gui.drawRect (Transparent), ChatLine.getUpdatedCounter +
//                         FontRenderer.drawStringWithShadow (Player heads)
//                         GuiNewChat.setChatLine: GuiUtilRenderComponents.splitText (Player heads: 10 px narrower
//                         lines). Message stacking and the head registration run around setChatLine, whose call
//                         sites belong to the Streamer hooks: they call tweaks_set_chat_line() (tweaks_hooks.hpp).
//   NoHotbarScrollTweak + ZoomTweak (scroll zoom)
//                         Minecraft.runTick: invoke InventoryPlayer.changeCurrentItem
//   ZoomTweak             EntityRenderer (all methods): invoke getFOVModifier (OptiFine zoom factor / cinematic)
//   FullscreenTweak       Minecraft.toggleFullscreen: invokestatic Display.setFullscreen
//   DisconnectConfirmTweak GuiScreen.mouseClicked: invoke actionPerformed (GuiIngameMenu button 1)
//   TabOverlayTweak       GuiPlayerTabOverlay.renderPlayerlist: invoke NetHandlerPlayClient.getPlayerInfoMap
//   SmoothRotationYawTweak EntityOtherPlayerMP.onLivingUpdate: invoke updateArmSwingProgress,
//                         NetHandlerPlayClient.handleEntityHeadLook: invoke Entity.setRotationYawHead
//   MinimalBobbingTweak   EntityRenderer.setupCameraTransform: invoke setupViewBobbing
//
// Forge: GuiIngameForge renders the HUD itself; its title / boss bar / scoreboard call sites are hooked with
// wildcard specs (class_vanilla = nullptr).

#include "adjust/hooks/tweaks_hooks.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "core/client.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/tweaks/tweaks.hpp"

namespace slinky::adjust::hooks {

namespace {

using bytecode::ExtraHookSpec;
using bytecode::HookInsnKind;
namespace tw = modules::adjust_tweaks;

int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

// ===============================================================================================================
// NoRenderTweak
// ===============================================================================================================

// GuiIngame.renderGameOverlay: `if (this.titlesTimer > 0 && !noRender.hideTitle())`
jint JNICALL native_titles_timer(JNIEnv* env, jclass /*cls*/, jobject gui) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "avo", "net/minecraft/client/gui/GuiIngame");
    static const jfieldID fid = jni::field(&jni, cls, {"w", "titlesTimer", "field_175195_w"}, "I", "I");
    if (fid == nullptr || tw::hide_title()) {
        return 0;
    }
    return env->GetIntField(gui, fid);
}

// GuiIngame.renderBossHealth: `if (noRender.hideBossBar()) return;`
void JNICALL native_render_boss_health(JNIEnv* env, jclass /*cls*/, jobject gui) noexcept {
    if (tw::hide_boss_bar()) {
        return;
    }
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "avo", "net/minecraft/client/gui/GuiIngame");
    static const jmethodID mid = jni::method(&jni, cls, {"j", "renderBossHealth", "func_73828_d"}, "()V", "()V");
    if (mid != nullptr) {
        env->CallVoidMethod(gui, mid); // virtual on Forge (GuiIngameForge override), private on vanilla
    }
}

// GuiAchievement.updateAchievementWindow: `if (noRender.hideAchievements()) return;`
void JNICALL native_update_achievement_window(JNIEnv* env, jclass /*cls*/, jobject gui_achievement) noexcept {
    if (tw::hide_achievements()) {
        return;
    }
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "ayd", "net/minecraft/client/gui/achievement/GuiAchievement");
    static const jmethodID mid =
        jni::method(&jni, cls, {"a", "updateAchievementWindow", "func_146254_a"}, "()V", "()V");
    if (mid != nullptr) {
        env->CallVoidMethod(gui_achievement, mid);
    }
}

// ===============================================================================================================
// ScoreboardTweak (GuiIngame.renderScoreboard)
// ===============================================================================================================

int g_sb_row_left = 0x7FFFFFFF; // l1 of the row being drawn (drawRect(l1 - 2, ...) precedes every row)

// `if (scoreboardEnabled && noScoreboard) return;`
void JNICALL native_render_scoreboard(JNIEnv* env, jclass /*cls*/, jobject gui, jobject objective,
                                      jobject resolution) noexcept {
    if (tw::scoreboard_hidden()) {
        return;
    }
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "avo", "net/minecraft/client/gui/GuiIngame");
    static const jmethodID mid = jni::method(
        &jni, cls, {"a", "renderScoreboard", "func_180475_a"}, "(Lauk;Lavr;)V",
        "(Lnet/minecraft/scoreboard/ScoreObjective;Lnet/minecraft/client/gui/ScaledResolution;)V");
    if (mid != nullptr) {
        g_sb_row_left = 0x7FFFFFFF;
        env->CallVoidMethod(gui, mid, objective, resolution);
    }
}

struct GuiDrawRect {
    jclass gui = nullptr;
    jmethodID draw_rect = nullptr;
};

const GuiDrawRect& gui_draw_rect(events::JniContext* jni) noexcept {
    static const GuiDrawRect s_ids = [&] {
        GuiDrawRect ids{};
        ids.gui = jni::find_class(jni, "avp", "net/minecraft/client/gui/Gui");
        ids.draw_rect = jni::static_method(jni, ids.gui, {"a", "drawRect", "func_73734_a"}, "(IIIII)V", "(IIIII)V");
        return ids;
    }();
    return s_ids;
}

void call_draw_rect(JNIEnv* env, jint left, jint top, jint right, jint bottom, jint color) noexcept {
    events::JniContext jni = jni_for(env);
    const GuiDrawRect& ids = gui_draw_rect(&jni);
    if (ids.draw_rect != nullptr) {
        env->CallStaticVoidMethod(ids.gui, ids.draw_rect, left, top, right, bottom, color);
    }
}

// `if (!transparent) drawRect(...)` (row backgrounds and the title background)
void JNICALL native_scoreboard_draw_rect(JNIEnv* env, jclass /*cls*/, jint left, jint top, jint right, jint bottom,
                                         jint color) noexcept {
    g_sb_row_left = left + 2;
    if (tw::scoreboard_transparent()) {
        return;
    }
    call_draw_rect(env, left, top, right, bottom, color);
}

// Score text drawn by renderScoreboard: EnumChatFormatting.RED + "" + points ("\u00a7c-?[0-9]+").
bool is_score_text(JNIEnv* env, jstring text) noexcept {
    if (text == nullptr) {
        return false;
    }
    const jsize length = env->GetStringLength(text);
    if (length < 3 || length > 16) {
        return false;
    }
    jchar buf[16]{};
    env->GetStringRegion(text, 0, length, buf);
    if (buf[0] != 0x00A7 || buf[1] != u'c') {
        return false;
    }
    jsize i = 2;
    if (buf[i] == u'-') {
        ++i;
    }
    if (i >= length) {
        return false;
    }
    for (; i < length; ++i) {
        if (buf[i] < u'0' || buf[i] > u'9') {
            return false;
        }
    }
    return true;
}

struct FontIds {
    jmethodID draw_string = nullptr;             // drawString(String, int, int, int)
    jmethodID draw_string_with_shadow = nullptr; // drawStringWithShadow(String, float, float, int)
    jmethodID get_string_width = nullptr;
};

const FontIds& font_ids(events::JniContext* jni) noexcept {
    static const FontIds s_ids = [&] {
        FontIds ids{};
        const jclass font = jni::find_class(jni, "avn", "net/minecraft/client/gui/FontRenderer");
        ids.draw_string = jni::method(jni, font, {"a", "drawString", "func_78276_b"}, "(Ljava/lang/String;III)I",
                                      "(Ljava/lang/String;III)I");
        ids.draw_string_with_shadow =
            jni::method(jni, font, {"a", "drawStringWithShadow", "func_175063_a"}, "(Ljava/lang/String;FFI)I",
                        "(Ljava/lang/String;FFI)I");
        ids.get_string_width =
            jni::method(jni, font, {"a", "getStringWidth", "func_78256_a"}, "(Ljava/lang/String;)I",
                        "(Ljava/lang/String;)I");
        return ids;
    }();
    return s_ids;
}

// `if (!hideScores) fontRenderer.drawString(s2, l - width(s2), k, 553648127);` - the score is the red number that is
// not drawn at the row's left edge (the player name always is).
jint JNICALL native_scoreboard_draw_string(JNIEnv* env, jclass /*cls*/, jobject font, jobject text, jint x, jint y,
                                           jint color) noexcept {
    if (tw::scoreboard_remove_scores() && x != g_sb_row_left && is_score_text(env, static_cast<jstring>(text))) {
        return x;
    }
    events::JniContext jni = jni_for(env);
    const FontIds& ids = font_ids(&jni);
    if (ids.draw_string == nullptr) {
        return x;
    }
    return env->CallIntMethod(font, ids.draw_string, text, x, y, color);
}

// Width loop: `if (!hideScores) s = s + ": " + RED + score.getScorePoints();` - measure the text without the
// ": \u00a7c<points>" suffix.
jint JNICALL native_scoreboard_string_width(JNIEnv* env, jclass /*cls*/, jobject font, jobject text) noexcept {
    events::JniContext jni = jni_for(env);
    const FontIds& ids = font_ids(&jni);
    if (ids.get_string_width == nullptr) {
        return 0;
    }
    auto str = static_cast<jstring>(text);
    if (str != nullptr && tw::scoreboard_remove_scores()) {
        const jsize length = env->GetStringLength(str);
        if (length >= 5 && length <= 1024) {
            std::vector<jchar> buf(static_cast<size_t>(length));
            env->GetStringRegion(str, 0, length, buf.data());
            jsize i = length;
            while (i > 0 && buf[static_cast<size_t>(i - 1)] >= u'0' && buf[static_cast<size_t>(i - 1)] <= u'9') {
                --i;
            }
            const bool has_digits = i < length;
            if (has_digits && i > 0 && buf[static_cast<size_t>(i - 1)] == u'-') {
                --i;
            }
            if (has_digits && i >= 4 && buf[static_cast<size_t>(i - 1)] == u'c' &&
                buf[static_cast<size_t>(i - 2)] == 0x00A7 && buf[static_cast<size_t>(i - 3)] == u' ' &&
                buf[static_cast<size_t>(i - 4)] == u':') {
                jstring name = env->NewString(buf.data(), i - 4);
                if (name != nullptr) {
                    const jint width = env->CallIntMethod(font, ids.get_string_width, name);
                    env->DeleteLocalRef(name);
                    return width;
                }
                env->ExceptionClear();
            }
        }
    }
    return env->CallIntMethod(font, ids.get_string_width, text);
}

// ===============================================================================================================
// ChatTweak: transparent background, player heads, message stacking
// ===============================================================================================================

// drawChat: `if (!transparent) drawRect(i2, j2 - 9, i2 + l + 4, j2, l1 / 2 << 24);`. The scroll bar rectangles have
// a colour and are always drawn.
void JNICALL native_chat_draw_rect(JNIEnv* env, jclass /*cls*/, jint left, jint top, jint right, jint bottom,
                                   jint color) noexcept {
    if ((static_cast<uint32_t>(color) & 0x00FFFFFFu) == 0u && tw::chat_transparent()) {
        return;
    }
    call_draw_rect(env, left, top, right, bottom, color);
}

struct ChatIds {
    bool ok = false;
    jclass new_chat = nullptr;
    jmethodID set_chat_line = nullptr;
    jfieldID chat_lines = nullptr;
    jfieldID drawn_chat_lines = nullptr;
    jmethodID line_get_updated_counter = nullptr;
    jmethodID line_get_id = nullptr;
    jmethodID line_get_component = nullptr;
    jclass split = nullptr;
    jmethodID split_text = nullptr;
    // java.util
    jmethodID list_size = nullptr;
    jmethodID list_get = nullptr;
    jmethodID list_remove = nullptr;
    jmethodID collection_iterator = nullptr;
    jmethodID iterator_has_next = nullptr;
    jmethodID iterator_next = nullptr;
    // IChatComponent
    jmethodID component_formatted = nullptr;
    jmethodID component_style = nullptr;
    jmethodID component_append = nullptr;
    jclass serializer = nullptr;
    jmethodID serializer_to_json = nullptr;
    jmethodID style_set_color = nullptr;
    jobject gray = nullptr; // EnumChatFormatting.GRAY (global ref)
    // ChatSenderPatterns.findSender / drawHead
    jmethodID net_get_player_info = nullptr;
    jmethodID net_get_player_info_map = nullptr;
    jmethodID info_get_display_name = nullptr;
    jmethodID info_get_location_skin = nullptr;
    jmethodID info_get_game_profile = nullptr;
    jmethodID profile_get_id = nullptr;
    jmethodID profile_get_name = nullptr;
    jclass default_skin = nullptr;
    jmethodID default_skin_get = nullptr;
    jmethodID texture_bind = nullptr;
    jclass gui = nullptr;
    jmethodID draw_scaled_rect = nullptr;
    jclass gl_state = nullptr;
    jmethodID blend_func_separate = nullptr;
};

const ChatIds& chat_ids(events::JniContext* jni) noexcept {
    static const ChatIds s_ids = [&] {
        ChatIds ids{};
        JNIEnv* env = jni->env;
        const jclass chat = jni::find_class(jni, "avt", "net/minecraft/client/gui/GuiNewChat");
        ids.new_chat = chat;
        ids.set_chat_line = jni::method(jni, chat, {"a", "setChatLine", "func_146237_a"}, "(Leu;IIZ)V",
                                        "(Lnet/minecraft/util/IChatComponent;IIZ)V");
        ids.chat_lines = jni::field(jni, chat, {"h", "chatLines", "field_146252_h"}, "Ljava/util/List;",
                                    "Ljava/util/List;");
        ids.drawn_chat_lines = jni::field(jni, chat, {"i", "drawnChatLines", "field_146253_i"}, "Ljava/util/List;",
                                          "Ljava/util/List;");
        const jclass line = jni::find_class(jni, "ava", "net/minecraft/client/gui/ChatLine");
        ids.line_get_updated_counter =
            jni::method(jni, line, {"b", "getUpdatedCounter", "func_74540_b"}, "()I", "()I");
        ids.line_get_id = jni::method(jni, line, {"c", "getChatLineID", "func_74539_c"}, "()I", "()I");
        ids.line_get_component = jni::method(jni, line, {"a", "getChatComponent", "func_151461_a"}, "()Leu;",
                                             "()Lnet/minecraft/util/IChatComponent;");
        ids.split = jni::find_class(jni, "avu", "net/minecraft/client/gui/GuiUtilRenderComponents");
        ids.split_text = jni::static_method(
            jni, ids.split, {"a", "splitText", "func_178908_a"}, "(Leu;ILavn;ZZ)Ljava/util/List;",
            "(Lnet/minecraft/util/IChatComponent;ILnet/minecraft/client/gui/FontRenderer;ZZ)Ljava/util/List;");

        jclass list = env->FindClass("java/util/List");
        jclass collection = env->FindClass("java/util/Collection");
        jclass iterator = env->FindClass("java/util/Iterator");
        if (list != nullptr && collection != nullptr && iterator != nullptr) {
            ids.list_size = env->GetMethodID(list, "size", "()I");
            ids.list_get = env->GetMethodID(list, "get", "(I)Ljava/lang/Object;");
            ids.list_remove = env->GetMethodID(list, "remove", "(I)Ljava/lang/Object;");
            ids.collection_iterator = env->GetMethodID(collection, "iterator", "()Ljava/util/Iterator;");
            ids.iterator_has_next = env->GetMethodID(iterator, "hasNext", "()Z");
            ids.iterator_next = env->GetMethodID(iterator, "next", "()Ljava/lang/Object;");
        }
        jni::clear_exception(jni);
        for (jclass local : {list, collection, iterator}) {
            if (local != nullptr) {
                env->DeleteLocalRef(local);
            }
        }

        const jclass component = jni::find_class(jni, "eu", "net/minecraft/util/IChatComponent");
        ids.component_formatted = jni::method(jni, component, {"d", "getFormattedText", "func_150254_d"},
                                              "()Ljava/lang/String;", "()Ljava/lang/String;");
        ids.component_style = jni::method(jni, component, {"b", "getChatStyle", "func_150256_b"}, "()Lez;",
                                          "()Lnet/minecraft/util/ChatStyle;");
        ids.component_append = jni::method(jni, component, {"a", "appendSibling", "func_150257_a"}, "(Leu;)Leu;",
                                           "(Lnet/minecraft/util/IChatComponent;)Lnet/minecraft/util/IChatComponent;");
        ids.serializer = jni::find_class(jni, "eu$a", "net/minecraft/util/IChatComponent$Serializer");
        ids.serializer_to_json =
            jni::static_method(jni, ids.serializer, {"a", "componentToJson", "func_150696_a"},
                               "(Leu;)Ljava/lang/String;", "(Lnet/minecraft/util/IChatComponent;)Ljava/lang/String;");
        const jclass style = jni::find_class(jni, "ez", "net/minecraft/util/ChatStyle");
        ids.style_set_color = jni::method(jni, style, {"a", "setColor", "func_150238_a"}, "(La;)Lez;",
                                          "(Lnet/minecraft/util/EnumChatFormatting;)Lnet/minecraft/util/ChatStyle;");
        const jclass formatting = jni::find_class(jni, "a", "net/minecraft/util/EnumChatFormatting");
        const jfieldID gray = jni::static_field(jni, formatting, {"h", "GRAY", "GRAY"}, "La;",
                                                "Lnet/minecraft/util/EnumChatFormatting;");
        if (formatting != nullptr && gray != nullptr) {
            jobject value = env->GetStaticObjectField(formatting, gray);
            if (value != nullptr) {
                ids.gray = env->NewGlobalRef(value);
                env->DeleteLocalRef(value);
            }
        }

        const jclass net = jni::find_class(jni, "bcy", "net/minecraft/client/network/NetHandlerPlayClient");
        ids.net_get_player_info =
            jni::method(jni, net, {"a", "getPlayerInfo", "func_175104_a"}, "(Ljava/lang/String;)Lbdc;",
                        "(Ljava/lang/String;)Lnet/minecraft/client/network/NetworkPlayerInfo;");
        ids.net_get_player_info_map = jni::method(jni, net, {"d", "getPlayerInfoMap", "func_175106_d"},
                                                  "()Ljava/util/Collection;", "()Ljava/util/Collection;");
        const jclass info = jni::find_class(jni, "bdc", "net/minecraft/client/network/NetworkPlayerInfo");
        ids.info_get_display_name = jni::method(jni, info, {"k", "getDisplayName", "func_178854_k"}, "()Leu;",
                                                "()Lnet/minecraft/util/IChatComponent;");
        ids.info_get_location_skin = jni::method(jni, info, {"g", "getLocationSkin", "func_178837_g"}, "()Ljy;",
                                                 "()Lnet/minecraft/util/ResourceLocation;");
        ids.info_get_game_profile =
            jni::method(jni, info, {"a", "getGameProfile", "func_178845_a"}, "()Lcom/mojang/authlib/GameProfile;",
                        "()Lcom/mojang/authlib/GameProfile;");
        const jclass profile = jni::find_class(jni, "com/mojang/authlib/GameProfile", "com/mojang/authlib/GameProfile");
        if (profile != nullptr) {
            ids.profile_get_id = env->GetMethodID(profile, "getId", "()Ljava/util/UUID;");
            ids.profile_get_name = env->GetMethodID(profile, "getName", "()Ljava/lang/String;");
            jni::clear_exception(jni);
        }
        ids.default_skin = jni::find_class(jni, "bmz", "net/minecraft/client/resources/DefaultPlayerSkin");
        ids.default_skin_get =
            jni::static_method(jni, ids.default_skin, {"a", "getDefaultSkin", "func_177334_a"},
                               "(Ljava/util/UUID;)Ljy;", "(Ljava/util/UUID;)Lnet/minecraft/util/ResourceLocation;");
        const jclass textures = jni::find_class(jni, "bmj", "net/minecraft/client/renderer/texture/TextureManager");
        ids.texture_bind = jni::method(jni, textures, {"a", "bindTexture", "func_110577_a"}, "(Ljy;)V",
                                       "(Lnet/minecraft/util/ResourceLocation;)V");
        ids.gui = jni::find_class(jni, "avp", "net/minecraft/client/gui/Gui");
        ids.draw_scaled_rect = jni::static_method(jni, ids.gui, {"a", "drawScaledCustomSizeModalRect", "func_152125_a"},
                                                  "(IIFFIIIIFF)V", "(IIFFIIIIFF)V");
        ids.gl_state = jni::find_class(jni, "bfl", "net/minecraft/client/renderer/GlStateManager");
        ids.blend_func_separate = jni::static_method(jni, ids.gl_state, {"a", "tryBlendFuncSeparate", "func_179120_a"},
                                                     "(IIII)V", "(IIII)V");

        ids.ok = ids.set_chat_line != nullptr && ids.chat_lines != nullptr && ids.drawn_chat_lines != nullptr &&
                 ids.line_get_updated_counter != nullptr && ids.line_get_id != nullptr &&
                 ids.line_get_component != nullptr && ids.split_text != nullptr &&
                 ids.list_size != nullptr && ids.list_get != nullptr && ids.list_remove != nullptr &&
                 ids.collection_iterator != nullptr && ids.iterator_has_next != nullptr &&
                 ids.iterator_next != nullptr && ids.component_formatted != nullptr && ids.component_style != nullptr &&
                 ids.component_append != nullptr && ids.serializer_to_json != nullptr &&
                 ids.style_set_color != nullptr && ids.gray != nullptr && ids.net_get_player_info != nullptr &&
                 ids.net_get_player_info_map != nullptr && ids.info_get_display_name != nullptr &&
                 ids.info_get_location_skin != nullptr && ids.info_get_game_profile != nullptr &&
                 ids.profile_get_id != nullptr && ids.profile_get_name != nullptr && ids.default_skin_get != nullptr &&
                 ids.texture_bind != nullptr && ids.draw_scaled_rect != nullptr && ids.blend_func_separate != nullptr;
        return ids;
    }();
    return s_ids;
}

// ---- text helpers (UTF-8, Minecraft "\u00a7" codes) ---------------------------------------------------------

bool is_java_space(char c) noexcept { // regex \s
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0B' || c == '\f' || c == '\r';
}

// StringUtils.stripControlCodes: removes "\u00a7[0-9A-FK-OR]" (case-insensitive).
std::string strip_control_codes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xC2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            const char code = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i + 2])));
            if ((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f') || (code >= 'k' && code <= 'o') ||
                code == 'r') {
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string_view java_trim(std::string_view text) noexcept {
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') {
        text.remove_suffix(1);
    }
    return text;
}

// ChatTweak.isSeparator: at least 5 characters, all '-', '=' or U+25AC.
bool is_separator(std::string_view text) noexcept {
    size_t chars = 0;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '-' || text[i] == '=') {
            ++i;
        } else if (text.substr(i, 3) == "\xE2\x96\xAC") {
            i += 3;
        } else {
            return false;
        }
        ++chars;
    }
    return chars >= 5;
}

// ---- ChatSenderPatterns -------------------------------------------------------------------------------------

struct Cursor {
    std::string_view t;
    size_t i = 0;

    bool at_end() const noexcept { return i >= t.size(); }
    char peek() const noexcept { return at_end() ? '\0' : t[i]; }
    void skip_ws() noexcept {
        while (!at_end() && is_java_space(t[i])) {
            ++i;
        }
    }
    bool ws_plus() noexcept {
        const size_t start = i;
        skip_ws();
        return i > start;
    }
    // [^\s:\[\]<>]+
    bool name(std::string_view* out) noexcept {
        const size_t start = i;
        while (!at_end()) {
            const char c = t[i];
            if (is_java_space(c) || c == ':' || c == '[' || c == ']' || c == '<' || c == '>') {
                break;
            }
            ++i;
        }
        *out = t.substr(start, i - start);
        return i > start;
    }
    // \[[^]\r\n]+]
    bool bracket() noexcept {
        if (peek() != '[') {
            return false;
        }
        size_t j = i + 1;
        while (j < t.size() && t[j] != ']' && t[j] != '\r' && t[j] != '\n') {
            ++j;
        }
        if (j == i + 1 || j >= t.size() || t[j] != ']') {
            return false;
        }
        i = j + 1;
        return true;
    }
    // (?:\[[^]\r\n]+]\s*)*
    void brackets() noexcept {
        while (peek() == '[') {
            const size_t save = i;
            if (!bracket()) {
                i = save;
                return;
            }
            skip_ws();
        }
    }
    bool literal(std::string_view word, bool ignore_case) noexcept {
        if (t.size() - std::min(i, t.size()) < word.size()) {
            return false;
        }
        for (size_t k = 0; k < word.size(); ++k) {
            const char a = t[i + k];
            const char b = word[k];
            if (ignore_case ? std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))
                            : a != b) {
                return false;
            }
        }
        i += word.size();
        return true;
    }
};

// (?:\s+\[[^]\r\n]+])?\s*  followed by `tail` (":\s" or end of text). Tries with the suffix first.
bool name_tail(Cursor c, bool colon_space) noexcept {
    const auto finish = [colon_space](Cursor k) noexcept {
        k.skip_ws();
        if (!colon_space) {
            return k.at_end();
        }
        if (k.peek() != ':') {
            return false;
        }
        ++k.i;
        return !k.at_end() && is_java_space(k.t[k.i]);
    };
    Cursor with = c;
    if (with.ws_plus() && with.bracket() && finish(with)) {
        return true;
    }
    return finish(c);
}

// "^\s*<([^\s:\[\]<>]+)>\s"
bool match_angle(std::string_view text, std::string_view* out) noexcept {
    Cursor c{text};
    c.skip_ws();
    if (c.peek() != '<') {
        return false;
    }
    ++c.i;
    if (!c.name(out) || c.peek() != '>') {
        return false;
    }
    ++c.i;
    return !c.at_end() && is_java_space(c.t[c.i]);
}

// "^\s*(?:(?:Party|Guild|Officer|Co-op|Team|All)\s*>\s*)?(?:(?:From|To)\s+)?(?:\[[^]\r\n]+]\s*)*
//  ([^\s:\[\]<>]+)(?:\s+\[[^]\r\n]+])?\s*:\s" (case-insensitive)
bool match_channel(std::string_view text, std::string_view* out) noexcept {
    static constexpr std::string_view kChannels[] = {"Party", "Guild", "Officer", "Co-op", "Team", "All"};
    static constexpr std::string_view kDirections[] = {"From", "To"};
    Cursor start{text};
    start.skip_ws();
    for (int with_channel = 1; with_channel >= 0; --with_channel) {
        Cursor a = start;
        if (with_channel != 0) {
            bool matched = false;
            for (std::string_view word : kChannels) {
                Cursor k = a;
                if (k.literal(word, true)) {
                    k.skip_ws();
                    if (k.peek() == '>') {
                        ++k.i;
                        k.skip_ws();
                        a = k;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                continue;
            }
        }
        for (int with_direction = 1; with_direction >= 0; --with_direction) {
            Cursor b = a;
            if (with_direction != 0) {
                bool matched = false;
                for (std::string_view word : kDirections) {
                    Cursor k = b;
                    if (k.literal(word, true) && k.ws_plus()) {
                        b = k;
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    continue;
                }
            }
            b.brackets();
            std::string_view name;
            if (b.name(&name) && name_tail(b, true)) {
                *out = name;
                return true;
            }
        }
    }
    return false;
}

// "^\s*([^\s:\[\]<>]+)\s+whispers to you:\s"
bool match_whisper_from(std::string_view text, std::string_view* out) noexcept {
    Cursor c{text};
    c.skip_ws();
    if (!c.name(out) || !c.ws_plus() || !c.literal("whispers to you:", false)) {
        return false;
    }
    return !c.at_end() && is_java_space(c.t[c.i]);
}

// "^\s*You whisper to\s+([^\s:\[\]<>]+):\s"
bool match_whisper_to(std::string_view text, std::string_view* out) noexcept {
    Cursor c{text};
    c.skip_ws();
    if (!c.literal("You whisper to", false) || !c.ws_plus() || !c.name(out) || c.peek() != ':') {
        return false;
    }
    ++c.i;
    return !c.at_end() && is_java_space(c.t[c.i]);
}

// DISPLAY_NAME "^\s*(?:\[[^]\r\n]+]\s*)*([^\s:\[\]<>]+)(?:\s+\[[^]\r\n]+])?\s*$" (matches())
bool match_display_name(std::string_view text, std::string_view* out) noexcept {
    Cursor c{text};
    c.skip_ws();
    c.brackets();
    return c.name(out) && name_tail(c, false);
}

bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

jstring component_formatted(JNIEnv* env, const ChatIds& ids, jobject component) noexcept {
    auto text = static_cast<jstring>(env->CallObjectMethod(component, ids.component_formatted));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return text;
}

// ChatSenderPatterns.findSender: local ref to the sender's NetworkPlayerInfo or nullptr. Call inside a local frame.
jobject find_sender(events::JniContext* jni, const ChatIds& ids, jobject component) {
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    if (mc == nullptr) {
        return nullptr;
    }
    jobject player = minecraft::mc_get_player(jni, mc);
    if (player == nullptr) {
        return nullptr;
    }
    const std::string text = strip_control_codes(to_utf8(env, component_formatted(env, ids, component)));
    std::string_view sender;
    if (!match_angle(text, &sender) && !match_channel(text, &sender) && !match_whisper_from(text, &sender) &&
        !match_whisper_to(text, &sender)) {
        return nullptr;
    }
    jobject net = minecraft::mc_get_net_handler(jni, mc); // == thePlayer.sendQueue
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    if (net == nullptr) {
        return nullptr;
    }
    const std::string sender_name(sender);
    jstring jname = env->NewStringUTF(sender_name.c_str());
    if (jname == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject info = env->CallObjectMethod(net, ids.net_get_player_info, jname);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        info = nullptr;
    }
    if (info != nullptr) {
        return info;
    }
    jobject infos = env->CallObjectMethod(net, ids.net_get_player_info_map);
    if (env->ExceptionCheck() || infos == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject it = env->CallObjectMethod(infos, ids.collection_iterator);
    if (env->ExceptionCheck() || it == nullptr) {
        env->ExceptionClear();
        return nullptr;
    }
    while (env->CallBooleanMethod(it, ids.iterator_has_next) != JNI_FALSE && !env->ExceptionCheck()) {
        jobject candidate = env->CallObjectMethod(it, ids.iterator_next);
        if (env->ExceptionCheck() || candidate == nullptr) {
            break;
        }
        jobject display = env->CallObjectMethod(candidate, ids.info_get_display_name);
        if (!env->ExceptionCheck() && display != nullptr) {
            jstring formatted = component_formatted(env, ids, display);
            const std::string stripped = strip_control_codes(to_utf8(env, formatted));
            if (formatted != nullptr) {
                env->DeleteLocalRef(formatted);
            }
            env->DeleteLocalRef(display);
            std::string_view name;
            if (match_display_name(stripped, &name) && equals_ignore_case(name, sender_name)) {
                return candidate;
            }
        }
        env->ExceptionClear();
        env->DeleteLocalRef(candidate);
    }
    env->ExceptionClear();
    return nullptr;
}

// ChatSenderPatterns.drawHead
void draw_head(events::JniContext* jni, const ChatIds& ids, jobject info, int x, int y, int alpha) noexcept {
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        env->ExceptionClear();
        return;
    }
    minecraft::gl_state_enable_texture_2d(jni);
    minecraft::gl_state_enable_alpha(jni);
    minecraft::gl_state_enable_blend(jni);
    env->CallStaticVoidMethod(ids.gl_state, ids.blend_func_separate, 770, 771, 1, 0);
    minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, static_cast<float>(alpha) / 255.0f);

    jobject skin = env->CallObjectMethod(info, ids.info_get_location_skin);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        skin = nullptr;
    }
    if (skin == nullptr) { // Objects.firstNonNull(getLocationSkin(), DefaultPlayerSkin.getDefaultSkin(id))
        jobject profile = env->CallObjectMethod(info, ids.info_get_game_profile);
        jobject id = (!env->ExceptionCheck() && profile != nullptr) ? env->CallObjectMethod(profile, ids.profile_get_id)
                                                                     : nullptr;
        if (!env->ExceptionCheck()) {
            skin = env->CallStaticObjectMethod(ids.default_skin, ids.default_skin_get, id);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            skin = nullptr;
        }
    }
    jobject mc = minecraft::mc_get_instance(jni);
    jobject textures = mc != nullptr ? minecraft::mc_get_texture_manager(jni, mc) : nullptr;
    if (skin != nullptr && textures != nullptr) {
        env->CallVoidMethod(textures, ids.texture_bind, skin);
        env->CallStaticVoidMethod(ids.gui, ids.draw_scaled_rect, x, y, 8.0f, 8.0f, 8, 8, 8, 8, 64.0f, 64.0f);
        env->CallStaticVoidMethod(ids.gui, ids.draw_scaled_rect, x, y, 40.0f, 8.0f, 8, 8, 8, 8, 64.0f, 64.0f);
    }
    env->ExceptionClear();
    minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
    env->PopLocalFrame(nullptr);
}

// ---- chat state ---------------------------------------------------------------------------------------------

// GuiPersistentChat.lineSenders (WeakHashMap<ChatLine, NetworkPlayerInfo>), first wrapped lines only - the head is
// only drawn beside those.
struct HeadLine {
    jweak line = nullptr;
    jobject sender = nullptr; // global ref
};

// One message added under a stack hash: the ChatLine stored in chatLines, the printed component (identifies the
// stored line even after the Streamer hook replaced chatLines[0], and the re-wrapped lines after refreshChat) and
// its drawn (wrapped) lines. Together these replace Adjust's ChatLine identity and ChatLine.originalComponent checks.
struct StackRecord {
    jweak stored = nullptr;
    jweak component = nullptr;
    std::vector<jweak> drawn;
};

struct StackLines { // ChatLineKey.lines entry
    std::string key;
    std::vector<StackRecord> records;
};

struct StackCount { // ChatLineKey.counts entry (ChatStackEntry)
    std::string key;
    int64_t timer_ms = 0;
    int count = 1;
};

struct ChatState {
    std::mutex mutex;
    std::vector<HeadLine> heads;
    std::vector<StackLines> lines;
    std::vector<StackCount> counts;
    uint32_t generation = 0;
};

ChatState g_chat{};

thread_local bool t_split_for_head = false;      // the message being wrapped has a sender head
thread_local jobject t_pending_head = nullptr;   // sender of the chat line drawChat is processing (global ref)

void delete_record(JNIEnv* env, StackRecord& record) noexcept {
    if (record.stored != nullptr) {
        env->DeleteWeakGlobalRef(record.stored);
    }
    if (record.component != nullptr) {
        env->DeleteWeakGlobalRef(record.component);
    }
    for (jweak ref : record.drawn) {
        env->DeleteWeakGlobalRef(ref);
    }
    record = StackRecord{};
}

void clear_stacks(JNIEnv* env) noexcept {
    for (StackLines& entry : g_chat.lines) {
        for (StackRecord& record : entry.records) {
            delete_record(env, record);
        }
    }
    g_chat.lines.clear();
    g_chat.counts.clear();
}

// ChatTweak.rebuildChannels / clearSuggestions -> a fresh ChatLineKey.
void sync_generation(JNIEnv* env) noexcept {
    const uint32_t generation = tw::chat_reset_generation();
    if (generation != g_chat.generation) {
        g_chat.generation = generation;
        clear_stacks(env);
    }
}

bool is_dead(JNIEnv* env, jweak ref) noexcept {
    return ref == nullptr || env->IsSameObject(ref, nullptr) == JNI_TRUE;
}

void prune(JNIEnv* env) noexcept {
    if (g_chat.heads.size() > 256) {
        auto& heads = g_chat.heads;
        heads.erase(std::remove_if(heads.begin(), heads.end(),
                                   [env](HeadLine& head) {
                                       if (!is_dead(env, head.line)) {
                                           return false;
                                       }
                                       env->DeleteWeakGlobalRef(head.line);
                                       env->DeleteGlobalRef(head.sender);
                                       return true;
                                   }),
                    heads.end());
    }
    if (g_chat.lines.size() > 256) {
        auto& lines = g_chat.lines;
        for (StackLines& entry : lines) {
            entry.records.erase(std::remove_if(entry.records.begin(), entry.records.end(),
                                               [env](StackRecord& record) {
                                                   if (!is_dead(env, record.stored)) {
                                                       return false;
                                                   }
                                                   delete_record(env, record);
                                                   return true;
                                               }),
                                entry.records.end());
        }
        lines.erase(std::remove_if(lines.begin(), lines.end(),
                                   [](const StackLines& entry) { return entry.records.empty(); }),
                    lines.end());
    }
    if (g_chat.counts.size() > 256) {
        // An expired ChatStackEntry behaves exactly like a missing one, so dropping it changes nothing.
        const int64_t now = now_ms();
        auto& counts = g_chat.counts;
        counts.erase(std::remove_if(counts.begin(), counts.end(),
                                    [now](const StackCount& entry) { return entry.timer_ms + 15000 <= now; }),
                     counts.end());
    }
}

jint list_size(JNIEnv* env, const ChatIds& ids, jobject list) noexcept {
    if (list == nullptr) {
        return 0;
    }
    const jint size = env->CallIntMethod(list, ids.list_size);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
    return size;
}

// Removes (up to `max_examined` entries from the head of `list`) every element matched by `matches`.
template <typename Pred>
bool remove_matching(JNIEnv* env, const ChatIds& ids, jobject list, int max_examined, Pred matches) {
    bool removed = false;
    jint size = list_size(env, ids, list);
    jint index = 0;
    for (int examined = 0; index < size && examined < max_examined; ++examined) {
        jobject element = env->CallObjectMethod(list, ids.list_get, index);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            break;
        }
        const bool hit = element != nullptr && matches(element);
        if (element != nullptr) {
            env->DeleteLocalRef(element);
        }
        if (hit) {
            jobject old = env->CallObjectMethod(list, ids.list_remove, index);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                break;
            }
            if (old != nullptr) {
                env->DeleteLocalRef(old);
            }
            --size;
            removed = true;
        } else {
            ++index;
        }
    }
    return removed;
}

// ChatTweak.mergeStack + removeOldChatLines.
bool merge_stack(JNIEnv* env, const ChatIds& ids, jobject chat, const std::string& key) {
    auto it = std::find_if(g_chat.lines.begin(), g_chat.lines.end(),
                           [&key](const StackLines& entry) { return entry.key == key; });
    if (it == g_chat.lines.end() || it->records.empty()) {
        return false;
    }
    std::vector<StackRecord> records = std::move(it->records);
    g_chat.lines.erase(it); // history.lines.remove(hash)

    jobject chat_lines = env->GetObjectField(chat, ids.chat_lines);
    const bool removed = remove_matching(env, ids, chat_lines, 101, [&](jobject line) {
        jobject line_component = nullptr;
        bool hit = false;
        for (const StackRecord& record : records) {
            if (record.stored != nullptr && env->IsSameObject(record.stored, line) == JNI_TRUE) {
                hit = true;
                break;
            }
            if (record.component == nullptr) {
                continue;
            }
            if (line_component == nullptr) {
                line_component = env->CallObjectMethod(line, ids.line_get_component);
                if (env->ExceptionCheck() || line_component == nullptr) {
                    env->ExceptionClear();
                    break;
                }
            }
            if (env->IsSameObject(record.component, line_component) == JNI_TRUE) {
                hit = true;
                break;
            }
        }
        if (line_component != nullptr) {
            env->DeleteLocalRef(line_component);
        }
        return hit;
    });
    if (chat_lines != nullptr) {
        env->DeleteLocalRef(chat_lines);
    }
    if (removed) {
        jobject drawn_lines = env->GetObjectField(chat, ids.drawn_chat_lines);
        remove_matching(env, ids, drawn_lines, 301, [&](jobject line) {
            for (const StackRecord& record : records) {
                for (jweak drawn : record.drawn) {
                    if (env->IsSameObject(drawn, line) == JNI_TRUE) {
                        return true;
                    }
                }
            }
            return false;
        });
        if (drawn_lines != nullptr) {
            env->DeleteLocalRef(drawn_lines);
        }
    }
    for (StackRecord& record : records) {
        delete_record(env, record);
    }
    return removed;
}

// String.format("%,d", count)
std::string format_thousands(int value) {
    const std::string digits = std::to_string(value < 0 ? -static_cast<long long>(value) : value);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return value < 0 ? "-" + out : out;
}

// " (n)" in GRAY appended to the message (StackedChatComponent).
void append_stack_suffix(events::JniContext* jni, const ChatIds& ids, jobject component, int count) {
    JNIEnv* env = jni->env;
    const std::string text = " (" + format_thousands(count) + ")";
    jstring jtext = env->NewStringUTF(text.c_str());
    if (jtext == nullptr) {
        env->ExceptionClear();
        return;
    }
    jobject suffix = minecraft::chat_component_text_new(jni, jtext);
    if (env->ExceptionCheck() || suffix == nullptr) {
        env->ExceptionClear();
        return;
    }
    jobject style = env->CallObjectMethod(suffix, ids.component_style);
    if (!env->ExceptionCheck() && style != nullptr) {
        env->CallObjectMethod(style, ids.style_set_color, ids.gray);
    }
    if (!env->ExceptionCheck()) {
        env->CallObjectMethod(component, ids.component_append, suffix);
    }
    env->ExceptionClear();
}

// ChatTweak.decorateMessage (pre event). Returns true with the stack key when the message takes part in stacking
// (history.hash != null). `component` = the message Minecraft printed, `shown` = what is displayed (a masked copy
// while the Streamer module is active, else the same object).
bool stack_pre(events::JniContext* jni, const ChatIds& ids, jobject chat, jobject component, jobject shown,
               std::string* key) {
    JNIEnv* env = jni->env;
    const std::string text = strip_control_codes(to_utf8(env, component_formatted(env, ids, component)));
    const std::string_view trimmed = java_trim(text);
    if (trimmed.empty() || is_separator(trimmed)) {
        return false;
    }
    // hashComponent: text, siblings and styles (incl. hover / click / insertion) - the JSON form carries all of it.
    auto json = static_cast<jstring>(env->CallStaticObjectMethod(ids.serializer, ids.serializer_to_json, component));
    if (env->ExceptionCheck() || json == nullptr) {
        env->ExceptionClear();
        return false;
    }
    *key = to_utf8(env, json);

    const int64_t now = now_ms();
    auto entry = std::find_if(g_chat.counts.begin(), g_chat.counts.end(),
                              [key](const StackCount& c) { return c.key == *key; });
    if (entry == g_chat.counts.end()) {
        g_chat.counts.push_back(StackCount{*key, now, 1});
        return true;
    }
    if (entry->timer_ms + tw::chat_stack_timeout_ms() <= now || !merge_stack(env, ids, chat, *key)) {
        // merge_stack never touches `counts`, so `entry` is still valid
        *entry = StackCount{*key, now, 1};
        return true;
    }
    entry->timer_ms = now;
    ++entry->count;
    // message.appendSibling(suffix): on the displayed component and (Streamer masking) on the stored original.
    append_stack_suffix(jni, ids, shown, entry->count);
    if (env->IsSameObject(shown, component) != JNI_TRUE) {
        append_stack_suffix(jni, ids, component, entry->count);
    }
    return true;
}

// `removedOldLine`: setChatLine removes lines with the same non-zero id first.
bool has_line_with_id(JNIEnv* env, const ChatIds& ids, jobject chat, jint id) {
    for (jfieldID fid : {ids.chat_lines, ids.drawn_chat_lines}) {
        jobject list = env->GetObjectField(chat, fid);
        const jint size = list_size(env, ids, list);
        for (jint i = 0; i < size; ++i) {
            jobject line = env->CallObjectMethod(list, ids.list_get, i);
            if (env->ExceptionCheck() || line == nullptr) {
                env->ExceptionClear();
                break;
            }
            const jint line_id = env->CallIntMethod(line, ids.line_get_id);
            env->DeleteLocalRef(line);
            if (!env->ExceptionCheck() && line_id == id) {
                env->DeleteLocalRef(list);
                return true;
            }
            env->ExceptionClear();
        }
        if (list != nullptr) {
            env->DeleteLocalRef(list);
        }
    }
    return false;
}

jobject list_head(JNIEnv* env, const ChatIds& ids, jobject list) noexcept {
    if (list_size(env, ids, list) <= 0) {
        return nullptr;
    }
    jobject head = env->CallObjectMethod(list, ids.list_get, 0);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return head;
}

// GuiPersistentChat.setChatLine around vanilla's GuiNewChat.setChatLine (see tweaks_hooks.hpp).
void set_chat_line_impl(JNIEnv* env, jobject chat, jobject shown, jobject component, jint id, jint update_counter,
                        jboolean display_only) noexcept {
    events::JniContext jni = jni_for(env);
    const ChatIds& ids = chat_ids(&jni);
    if (ids.set_chat_line == nullptr || ids.new_chat == nullptr) {
        return;
    }
    if (component == nullptr) {
        component = shown;
    }
    const bool heads = ids.ok && tw::chat_player_heads();
    const bool stacking = ids.ok && tw::chat_stacking();
    if ((!heads && !stacking) || shown == nullptr || env->PushLocalFrame(64) != 0) {
        env->ExceptionClear();
        env->CallNonvirtualVoidMethod(chat, ids.new_chat, ids.set_chat_line, shown, id, update_counter, display_only);
        return;
    }

    std::unique_lock<std::mutex> lock(g_chat.mutex);
    sync_generation(env);

    // GuiPersistentChat.setChatLine: sender = ChatSenderPatterns.findSender(origChatComponent)
    jobject sender = heads ? find_sender(&jni, ids, component) : nullptr;

    // ChatLineEvent (pre) -> ChatTweak.decorateMessage
    std::string key;
    bool has_key = false;
    if (stacking && display_only == JNI_FALSE && (id == 0 || !has_line_with_id(env, ids, chat, id))) {
        has_key = stack_pre(&jni, ids, chat, component, shown, &key);
    }

    jobject drawn_list = env->GetObjectField(chat, ids.drawn_chat_lines);
    jobject stored_list = env->GetObjectField(chat, ids.chat_lines);
    jobject old_drawn_head = list_head(env, ids, drawn_list);
    jobject old_stored_head = list_head(env, ids, stored_list);
    lock.unlock();

    t_split_for_head = sender != nullptr;
    env->CallNonvirtualVoidMethod(chat, ids.new_chat, ids.set_chat_line, shown, id, update_counter, display_only);
    t_split_for_head = false;
    if (env->ExceptionCheck()) {
        env->PopLocalFrame(nullptr); // keep the exception pending for Java
        return;
    }

    lock.lock();
    // New drawn lines: drawnChatLines[0 .. n) (added at index 0, so [n - 1] is the first wrapped line).
    std::vector<jobject> new_drawn;
    const jint drawn_size = list_size(env, ids, drawn_list);
    for (jint i = 0; i < drawn_size && i < 100; ++i) {
        jobject line = env->CallObjectMethod(drawn_list, ids.list_get, i);
        if (env->ExceptionCheck() || line == nullptr) {
            env->ExceptionClear();
            break;
        }
        if (old_drawn_head != nullptr && env->IsSameObject(line, old_drawn_head) == JNI_TRUE) {
            break;
        }
        new_drawn.push_back(line);
    }

    if (sender != nullptr && !new_drawn.empty()) {
        jweak line = env->NewWeakGlobalRef(new_drawn.back());
        jobject global_sender = env->NewGlobalRef(sender);
        if (line != nullptr && global_sender != nullptr) {
            g_chat.heads.push_back(HeadLine{line, global_sender});
        }
    }

    if (stacking) {
        if (has_key) {
            // ChatLineAddedEvent -> ChatTweak.updateStack: history.lines[hash].add(chatLine)
            jobject stored = list_head(env, ids, stored_list);
            if (stored != nullptr &&
                (old_stored_head == nullptr || env->IsSameObject(stored, old_stored_head) != JNI_TRUE)) {
                StackRecord record{};
                record.stored = env->NewWeakGlobalRef(stored);
                record.component = env->NewWeakGlobalRef(component);
                for (jobject line : new_drawn) {
                    record.drawn.push_back(env->NewWeakGlobalRef(line));
                }
                auto it = std::find_if(g_chat.lines.begin(), g_chat.lines.end(),
                                       [&key](const StackLines& entry) { return entry.key == key; });
                if (it == g_chat.lines.end()) {
                    g_chat.lines.push_back(StackLines{key, {}});
                    it = g_chat.lines.end() - 1;
                }
                it->records.push_back(std::move(record));
            }
        } else if (display_only != JNI_FALSE) {
            // refreshChat re-wraps stored lines: the old drawn lines are gone, track the new ones.
            for (StackLines& entry : g_chat.lines) {
                for (StackRecord& record : entry.records) {
                    if (record.component != nullptr && env->IsSameObject(record.component, component) == JNI_TRUE) {
                        for (jweak ref : record.drawn) {
                            env->DeleteWeakGlobalRef(ref);
                        }
                        record.drawn.clear();
                        for (jobject line : new_drawn) {
                            record.drawn.push_back(env->NewWeakGlobalRef(line));
                        }
                    }
                }
            }
        }
    }
    prune(env);
    lock.unlock();
    env->PopLocalFrame(nullptr);
}

// setChatLine: `if (sender != null) i = (i - 10 < 2) ? 1 : (i - 10);` before splitText.
jobject JNICALL native_chat_split_text(JNIEnv* env, jclass /*cls*/, jobject component, jint width, jobject font,
                                       jboolean styled, jboolean trim) noexcept {
    events::JniContext jni = jni_for(env);
    const ChatIds& ids = chat_ids(&jni);
    if (ids.split_text == nullptr) {
        return nullptr;
    }
    if (t_split_for_head) {
        width = (width - 10 < 2) ? 1 : (width - 10);
    }
    return env->CallStaticObjectMethod(ids.split, ids.split_text, component, width, font, styled, trim);
}

// drawChat: `int j1 = updateCounter - chatline.getUpdatedCounter();` - remembers whether this line gets a head.
jint JNICALL native_chat_line_counter(JNIEnv* env, jclass /*cls*/, jobject line) noexcept {
    events::JniContext jni = jni_for(env);
    const ChatIds& ids = chat_ids(&jni);
    if (ids.line_get_updated_counter == nullptr) {
        return 0;
    }
    t_pending_head = nullptr;
    const jint counter = env->CallIntMethod(line, ids.line_get_updated_counter);
    if (env->ExceptionCheck() || !ids.ok || !tw::chat_player_heads()) {
        return counter;
    }
    std::lock_guard<std::mutex> lock(g_chat.mutex);
    for (const HeadLine& head : g_chat.heads) {
        if (env->IsSameObject(head.line, line) == JNI_TRUE) {
            t_pending_head = head.sender;
            break;
        }
    }
    return counter;
}

// drawChat: `if (playerHeads && chatline.firstWrappedLine && sender != null) { drawHead(sender, i2, j2 - 8, l1);
// i2 += 10; }` then drawStringWithShadow(s, i2, j2 - 8, color).
jint JNICALL native_chat_draw_string(JNIEnv* env, jclass /*cls*/, jobject font, jobject text, jfloat x, jfloat y,
                                     jint color) noexcept {
    events::JniContext jni = jni_for(env);
    const FontIds& fonts = font_ids(&jni);
    if (fonts.draw_string_with_shadow == nullptr) {
        return 0;
    }
    jobject head = t_pending_head;
    t_pending_head = nullptr;
    if (head != nullptr && tw::chat_player_heads()) {
        const ChatIds& ids = chat_ids(&jni);
        const int alpha = static_cast<int>((static_cast<uint32_t>(color) >> 24) & 0xFFu);
        draw_head(&jni, ids, head, static_cast<int>(x), static_cast<int>(y), alpha);
        x += 10.0f;
    }
    return env->CallIntMethod(font, fonts.draw_string_with_shadow, text, x, y, color);
}

// ===============================================================================================================
// NoHotbarScrollTweak + ZoomTweak (Minecraft.runTick mouse wheel)
// ===============================================================================================================

struct ZoomIds {
    jfieldID mc_current_screen = nullptr;
    jfieldID mc_game_settings = nullptr;
    jclass settings = nullptr;
    jfieldID of_key_bind_zoom = nullptr; // OptiFine only
    jmethodID is_key_down = nullptr;
    jfieldID smooth_camera = nullptr;
    jfieldID display_list_entities_dirty = nullptr;
};

const ZoomIds& zoom_ids(events::JniContext* jni) noexcept {
    static const ZoomIds s_ids = [&] {
        ZoomIds ids{};
        const jclass mc = jni::find_class(jni, "ave", "net/minecraft/client/Minecraft");
        ids.mc_current_screen = jni::field(jni, mc, {"m", "currentScreen", "field_71462_r"}, "Laxu;",
                                           "Lnet/minecraft/client/gui/GuiScreen;");
        ids.mc_game_settings = jni::field(jni, mc, {"t", "gameSettings", "field_71474_y"}, "Lavh;",
                                          "Lnet/minecraft/client/settings/GameSettings;");
        ids.settings = jni::find_class(jni, "avh", "net/minecraft/client/settings/GameSettings");
        ids.of_key_bind_zoom = jni::field(jni, ids.settings, {"ofKeyBindZoom", "ofKeyBindZoom", "ofKeyBindZoom"},
                                          "Lavb;", "Lnet/minecraft/client/settings/KeyBinding;");
        ids.is_key_down = jni::static_method(jni, ids.settings, {"a", "isKeyDown", "func_100015_a"}, "(Lavb;)Z",
                                             "(Lnet/minecraft/client/settings/KeyBinding;)Z");
        ids.smooth_camera = jni::field(jni, ids.settings, {"aG", "smoothCamera", "field_74326_T"}, "Z", "Z");
        const jclass render_global = jni::find_class(jni, "bfr", "net/minecraft/client/renderer/RenderGlobal");
        ids.display_list_entities_dirty =
            jni::field(jni, render_global, {"ac", "displayListEntitiesDirty", "field_147595_R"}, "Z", "Z");
        return ids;
    }();
    return s_ids;
}

// GameSettings.isKeyDown(mc.gameSettings.ofKeyBindZoom) && mc.currentScreen == null. Call inside a local frame.
bool zoom_key_down(events::JniContext* jni, jobject mc, jobject* settings_out) noexcept {
    const ZoomIds& ids = zoom_ids(jni);
    JNIEnv* env = jni->env;
    if (mc == nullptr || ids.of_key_bind_zoom == nullptr || ids.is_key_down == nullptr ||
        ids.mc_current_screen == nullptr || ids.mc_game_settings == nullptr) {
        return false;
    }
    jobject screen = env->GetObjectField(mc, ids.mc_current_screen);
    if (screen != nullptr) {
        return false;
    }
    jobject settings = env->GetObjectField(mc, ids.mc_game_settings);
    if (settings == nullptr) {
        return false;
    }
    jobject key = env->GetObjectField(settings, ids.of_key_bind_zoom);
    if (key == nullptr) {
        return false;
    }
    const bool down = env->CallStaticBooleanMethod(ids.settings, ids.is_key_down, key) != JNI_FALSE;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (settings_out != nullptr) {
        *settings_out = settings;
    }
    return down;
}

// runTick: `else if (zoom.isZoomKeyDown()) { } else if (!noHotbarScroll.enabledSetting())
// this.thePlayer.inventory.changeCurrentItem(j);` (+ MouseWheelEvent -> ZoomTweak.updateZoom)
void JNICALL native_change_current_item(JNIEnv* env, jclass /*cls*/, jobject inventory, jint delta) noexcept {
    events::JniContext jni = jni_for(env);
    if (tw::zoom_scroll() && env->PushLocalFrame(16) == 0) {
        jobject mc = minecraft::mc_get_instance(&jni);
        const bool zooming = zoom_key_down(&jni, mc, nullptr);
        if (zooming) {
            tw::zoom_on_wheel(delta);
            const ZoomIds& ids = zoom_ids(&jni);
            if (delta < 0 && ids.display_list_entities_dirty != nullptr) {
                jobject render_global = minecraft::mc_get_render_global(&jni, mc);
                if (render_global != nullptr) {
                    env->SetBooleanField(render_global, ids.display_list_entities_dirty, JNI_TRUE);
                }
            }
        }
        env->PopLocalFrame(nullptr);
        if (zooming) {
            return; // ZoomTweak consumes the mouse wheel while zooming
        }
    }
    env->ExceptionClear();
    if (tw::no_hotbar_scroll()) {
        return;
    }
    static const jclass cls = jni::find_class(&jni, "wm", "net/minecraft/entity/player/InventoryPlayer");
    static const jmethodID mid = jni::method(&jni, cls, {"d", "changeCurrentItem", "func_70453_c"}, "(I)V", "(I)V");
    if (mid != nullptr) {
        env->CallVoidMethod(inventory, mid, delta);
    }
}

// ===============================================================================================================
// ZoomTweak (EntityRenderer.getFOVModifier, OptiFine zoom)
// ===============================================================================================================

bool g_was_zooming = false;

// OptiFine divides the FOV by 4 while its zoom key is held; Adjust divides by ZoomTweak.getZoom() instead and sets
// smoothCamera = !noCinematic. The rest of getFOVModifier only multiplies, so rescaling the result is exact.
jfloat JNICALL native_get_fov_modifier(JNIEnv* env, jclass /*cls*/, jobject renderer, jfloat partial_ticks,
                                       jboolean use_fov_setting) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "bfk", "net/minecraft/client/renderer/EntityRenderer");
    static const jmethodID mid =
        jni::method(&jni, cls, {"a", "getFOVModifier", "func_78481_a"}, "(FZ)F", "(FZ)F");
    if (mid == nullptr) {
        return 70.0f;
    }
    jfloat fov = env->CallFloatMethod(renderer, mid, partial_ticks, use_fov_setting);
    if (env->ExceptionCheck()) {
        return fov;
    }
    if (!tw::zoom_enabled()) {
        g_was_zooming = false;
        return fov;
    }
    if (env->PushLocalFrame(16) != 0) {
        env->ExceptionClear();
        return fov;
    }
    jobject settings = nullptr;
    const bool zooming = zoom_key_down(&jni, minecraft::mc_get_instance(&jni), &settings);
    if (!zooming) {
        if (g_was_zooming) {
            g_was_zooming = false;
            tw::zoom_reset(); // zoom released -> tweaks.zoom.resetZoom()
        }
    } else {
        g_was_zooming = true;
        const ZoomIds& ids = zoom_ids(&jni);
        if (tw::zoom_no_cinematic() && settings != nullptr && ids.smooth_camera != nullptr) {
            env->SetBooleanField(settings, ids.smooth_camera, JNI_FALSE);
        }
        if (tw::zoom_scroll()) {
            fov = fov * 4.0f / tw::zoom_factor();
        }
    }
    env->PopLocalFrame(nullptr);
    return fov;
}

// ===============================================================================================================
// FullscreenTweak (Minecraft.toggleFullscreen -> Display.setFullscreen)
// ===============================================================================================================

HWND g_borderless_window = nullptr;
LONG_PTR g_saved_style = 0;

bool apply_borderless() noexcept {
    HWND window = core::find_process_main_window();
    if (window == nullptr) {
        return false;
    }
    // GLFW.glfwGetPrimaryMonitor / glfwGetVideoMode / glfwGetMonitorPos
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
        return false;
    }
    if (g_borderless_window != window) {
        g_saved_style = GetWindowLongPtrW(window, GWL_STYLE);
    }
    g_borderless_window = window;
    // glfwSetWindowAttrib(GLFW_DECORATED, false) + glfwSetWindowMonitor(window, 0, x, y, w, h)
    const LONG_PTR style = (g_saved_style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(window, GWL_STYLE, style);
    const RECT& rc = info.rcMonitor;
    SetWindowPos(window, HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    return true;
}

void restore_borderless() noexcept {
    if (g_borderless_window == nullptr) {
        return;
    }
    // LWJGL may have recreated the window meanwhile (Display.setDisplayMode); only restore the one we changed.
    if (IsWindow(g_borderless_window) != FALSE && g_borderless_window == core::find_process_main_window()) {
        SetWindowLongPtrW(g_borderless_window, GWL_STYLE, g_saved_style);
        SetWindowPos(g_borderless_window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_borderless_window = nullptr;
    tw::set_borderless_applied(false);
}

// Minecraft.setFullscreen (Adjust): `if (fullscreen && borderless) { Display.setFullscreen(false); undecorated window
// covering the primary monitor }`.
void JNICALL native_set_fullscreen(JNIEnv* env, jclass /*cls*/, jboolean fullscreen) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass display = jni::find_class(&jni, "org/lwjgl/opengl/Display", "org/lwjgl/opengl/Display");
    static const jmethodID mid = jni::static_method(&jni, display, {"setFullscreen", "setFullscreen", "setFullscreen"},
                                                    "(Z)V", "(Z)V");
    if (mid == nullptr) {
        return;
    }
    if (fullscreen != JNI_FALSE && tw::borderless_fullscreen()) {
        env->CallStaticVoidMethod(display, mid, JNI_FALSE);
        if (env->ExceptionCheck()) {
            return;
        }
        if (apply_borderless()) {
            tw::set_borderless_applied(true);
            return;
        }
        // no window found: fall back to vanilla's exclusive fullscreen
    } else {
        restore_borderless();
    }
    env->CallStaticVoidMethod(display, mid, fullscreen);
}

// ===============================================================================================================
// DisconnectConfirmTweak (GuiScreen.mouseClicked -> actionPerformed)
// ===============================================================================================================

void JNICALL native_action_performed(JNIEnv* env, jclass /*cls*/, jobject screen, jobject button) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass screen_cls = jni::find_class(&jni, "axu", "net/minecraft/client/gui/GuiScreen");
    static const jmethodID mid = jni::method(&jni, screen_cls, {"a", "actionPerformed", "func_146284_a"}, "(Lavs;)V",
                                             "(Lnet/minecraft/client/gui/GuiButton;)V");
    static const jclass menu_cls = jni::find_class(&jni, "axp", "net/minecraft/client/gui/GuiIngameMenu");
    static const jclass button_cls = jni::find_class(&jni, "avs", "net/minecraft/client/gui/GuiButton");
    static const jfieldID fid_id = jni::field(&jni, button_cls, {"k", "id", "field_146127_k"}, "I", "I");
    if (mid == nullptr) {
        return;
    }
    // GuiIngameMenu.actionPerformed case 1: confirm timer
    if (button != nullptr && menu_cls != nullptr && fid_id != nullptr && env->IsInstanceOf(screen, menu_cls) &&
        env->GetIntField(button, fid_id) == 1 && tw::disconnect_confirm_swallow(env, screen)) {
        return;
    }
    env->CallVoidMethod(screen, mid, button); // virtual: the screen's own actionPerformed
}

// ===============================================================================================================
// TabOverlayTweak (GuiPlayerTabOverlay.renderPlayerlist -> NetHandlerPlayClient.getPlayerInfoMap)
// ===============================================================================================================

struct TabIds {
    bool ok = false;
    jmethodID get_player_info_map = nullptr;
    jmethodID world_get_scoreboard = nullptr;
    jmethodID scoreboard_get_players_team = nullptr;
    jmethodID team_get_visibility = nullptr;
    jobject visible_never = nullptr; // Team.EnumVisible.NEVER (global ref)
    jmethodID info_get_game_profile = nullptr;
    jmethodID profile_get_name = nullptr;
    jclass array_list = nullptr;
    jmethodID array_list_init = nullptr;
    jmethodID array_list_add = nullptr;
    jmethodID collection_size = nullptr;
    jmethodID collection_iterator = nullptr;
    jmethodID iterator_has_next = nullptr;
    jmethodID iterator_next = nullptr;
};

const TabIds& tab_ids(events::JniContext* jni) noexcept {
    static const TabIds s_ids = [&] {
        TabIds ids{};
        JNIEnv* env = jni->env;
        const jclass net = jni::find_class(jni, "bcy", "net/minecraft/client/network/NetHandlerPlayClient");
        ids.get_player_info_map = jni::method(jni, net, {"d", "getPlayerInfoMap", "func_175106_d"},
                                              "()Ljava/util/Collection;", "()Ljava/util/Collection;");
        const jclass world = jni::find_class(jni, "adm", "net/minecraft/world/World");
        ids.world_get_scoreboard = jni::method(jni, world, {"Z", "getScoreboard", "func_96441_U"}, "()Lauo;",
                                               "()Lnet/minecraft/scoreboard/Scoreboard;");
        const jclass scoreboard = jni::find_class(jni, "auo", "net/minecraft/scoreboard/Scoreboard");
        ids.scoreboard_get_players_team =
            jni::method(jni, scoreboard, {"h", "getPlayersTeam", "func_96509_i"}, "(Ljava/lang/String;)Laul;",
                        "(Ljava/lang/String;)Lnet/minecraft/scoreboard/ScorePlayerTeam;");
        const jclass team = jni::find_class(jni, "aul", "net/minecraft/scoreboard/ScorePlayerTeam");
        ids.team_get_visibility = jni::method(jni, team, {"i", "getNameTagVisibility", "func_178770_i"},
                                              "()Lauq$a;", "()Lnet/minecraft/scoreboard/Team$EnumVisible;");
        const jclass visible = jni::find_class(jni, "auq$a", "net/minecraft/scoreboard/Team$EnumVisible");
        const jfieldID never = jni::static_field(jni, visible, {"b", "NEVER", "NEVER"}, "Lauq$a;",
                                                 "Lnet/minecraft/scoreboard/Team$EnumVisible;");
        if (visible != nullptr && never != nullptr) {
            jobject value = env->GetStaticObjectField(visible, never);
            if (value != nullptr) {
                ids.visible_never = env->NewGlobalRef(value);
                env->DeleteLocalRef(value);
            }
        }
        const jclass info = jni::find_class(jni, "bdc", "net/minecraft/client/network/NetworkPlayerInfo");
        ids.info_get_game_profile =
            jni::method(jni, info, {"a", "getGameProfile", "func_178845_a"}, "()Lcom/mojang/authlib/GameProfile;",
                        "()Lcom/mojang/authlib/GameProfile;");
        const jclass profile = jni::find_class(jni, "com/mojang/authlib/GameProfile", "com/mojang/authlib/GameProfile");
        if (profile != nullptr) {
            ids.profile_get_name = env->GetMethodID(profile, "getName", "()Ljava/lang/String;");
        }
        jclass array_list = env->FindClass("java/util/ArrayList");
        jclass collection = env->FindClass("java/util/Collection");
        jclass iterator = env->FindClass("java/util/Iterator");
        if (array_list != nullptr && collection != nullptr && iterator != nullptr) {
            ids.array_list = static_cast<jclass>(env->NewGlobalRef(array_list));
            ids.array_list_init = env->GetMethodID(array_list, "<init>", "(I)V");
            ids.array_list_add = env->GetMethodID(array_list, "add", "(Ljava/lang/Object;)Z");
            ids.collection_size = env->GetMethodID(collection, "size", "()I");
            ids.collection_iterator = env->GetMethodID(collection, "iterator", "()Ljava/util/Iterator;");
            ids.iterator_has_next = env->GetMethodID(iterator, "hasNext", "()Z");
            ids.iterator_next = env->GetMethodID(iterator, "next", "()Ljava/lang/Object;");
        }
        jni::clear_exception(jni);
        for (jclass local : {array_list, collection, iterator}) {
            if (local != nullptr) {
                env->DeleteLocalRef(local);
            }
        }
        ids.ok = ids.get_player_info_map != nullptr && ids.world_get_scoreboard != nullptr &&
                 ids.scoreboard_get_players_team != nullptr && ids.team_get_visibility != nullptr &&
                 ids.visible_never != nullptr && ids.info_get_game_profile != nullptr &&
                 ids.profile_get_name != nullptr && ids.array_list != nullptr && ids.array_list_init != nullptr &&
                 ids.array_list_add != nullptr && ids.collection_size != nullptr &&
                 ids.collection_iterator != nullptr && ids.iterator_has_next != nullptr &&
                 ids.iterator_next != nullptr;
        return ids;
    }();
    return s_ids;
}

// FriendServerTracker.isNpcName: team.getNameTagVisibility() == NEVER && team.getColorPrefix() matches
// "^\u00a7[8a]\[NPC] $".
bool is_npc_team(events::JniContext* jni, const TabIds& ids, jobject team) noexcept {
    JNIEnv* env = jni->env;
    if (team == nullptr) {
        return false;
    }
    jobject visibility = env->CallObjectMethod(team, ids.team_get_visibility);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    const bool never = visibility != nullptr && env->IsSameObject(visibility, ids.visible_never) == JNI_TRUE;
    if (visibility != nullptr) {
        env->DeleteLocalRef(visibility);
    }
    if (!never) {
        return false;
    }
    jstring prefix = minecraft::score_player_team_get_color_prefix(jni, team);
    if (prefix == nullptr) {
        return false;
    }
    bool npc = false;
    if (env->GetStringLength(prefix) == 8) {
        jchar buf[8]{};
        env->GetStringRegion(prefix, 0, 8, buf);
        static constexpr char16_t kTail[] = u"[NPC] ";
        npc = buf[0] == 0x00A7 && (buf[1] == u'8' || buf[1] == u'a');
        for (int i = 0; npc && i < 6; ++i) {
            npc = buf[2 + i] == static_cast<jchar>(kTail[i]);
        }
    }
    env->DeleteLocalRef(prefix);
    return npc;
}

// renderPlayerlist: `if (tabOverlay.enabledSetting()) list.removeIf(info -> isNpcName(scoreboard.getPlayersTeam(
// info.getGameProfile().getName())));` - filtering before sortedCopy gives the same list.
jobject JNICALL native_tab_player_info_map(JNIEnv* env, jclass /*cls*/, jobject net_handler) noexcept {
    events::JniContext jni = jni_for(env);
    const TabIds& ids = tab_ids(&jni);
    if (ids.get_player_info_map == nullptr) {
        return nullptr;
    }
    jobject infos = env->CallObjectMethod(net_handler, ids.get_player_info_map);
    if (env->ExceptionCheck() || infos == nullptr || !ids.ok || !tw::tab_remove_npcs()) {
        return infos;
    }
    if (env->PushLocalFrame(32) != 0) {
        env->ExceptionClear();
        return infos;
    }
    jobject result = nullptr;
    jobject mc = minecraft::mc_get_instance(&jni);
    jobject world = mc != nullptr ? minecraft::mc_get_world(&jni, mc) : nullptr;
    jobject scoreboard = world != nullptr ? env->CallObjectMethod(world, ids.world_get_scoreboard) : nullptr;
    if (!env->ExceptionCheck() && scoreboard != nullptr) {
        const jint size = env->CallIntMethod(infos, ids.collection_size);
        jobject filtered = env->NewObject(ids.array_list, ids.array_list_init, std::max<jint>(size, 0));
        jobject it = filtered != nullptr ? env->CallObjectMethod(infos, ids.collection_iterator) : nullptr;
        bool failed = env->ExceptionCheck() || it == nullptr;
        while (!failed && env->CallBooleanMethod(it, ids.iterator_has_next) != JNI_FALSE) {
            jobject info = env->CallObjectMethod(it, ids.iterator_next);
            if (env->ExceptionCheck()) {
                failed = true;
                break;
            }
            bool npc = false;
            if (info != nullptr) {
                jobject profile = env->CallObjectMethod(info, ids.info_get_game_profile);
                jobject name = (!env->ExceptionCheck() && profile != nullptr)
                                   ? env->CallObjectMethod(profile, ids.profile_get_name)
                                   : nullptr;
                jobject team = (!env->ExceptionCheck() && name != nullptr)
                                   ? env->CallObjectMethod(scoreboard, ids.scoreboard_get_players_team, name)
                                   : nullptr;
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    team = nullptr;
                }
                npc = is_npc_team(&jni, ids, team);
                for (jobject local : {team, name, profile}) {
                    if (local != nullptr) {
                        env->DeleteLocalRef(local);
                    }
                }
            }
            if (!npc) {
                env->CallBooleanMethod(filtered, ids.array_list_add, info);
            }
            if (info != nullptr) {
                env->DeleteLocalRef(info);
            }
            failed = env->ExceptionCheck();
        }
        if (!failed) {
            result = filtered;
        }
    }
    env->ExceptionClear();
    result = env->PopLocalFrame(result);
    if (result == nullptr) {
        return infos;
    }
    env->DeleteLocalRef(infos);
    return result;
}

// ===============================================================================================================
// SmoothRotationYawTweak
// ===============================================================================================================

// EntityOtherPlayerMP.onLivingUpdate: `if (smoothRotationYaw.isEnabled()) this.rotationYawHead = this.rotationYaw;`
// right before `this.updateArmSwingProgress()`.
void JNICALL native_other_player_swing(JNIEnv* env, jclass /*cls*/, jobject entity) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass living = jni::find_class(&jni, "pr", "net/minecraft/entity/EntityLivingBase");
    static const jclass base = jni::find_class(&jni, "pk", "net/minecraft/entity/Entity");
    static const jmethodID mid =
        jni::method(&jni, living, {"bx", "updateArmSwingProgress", "func_82168_bl"}, "()V", "()V");
    static const jfieldID fid_yaw = jni::field(&jni, base, {"y", "rotationYaw", "field_70177_z"}, "F", "F");
    static const jfieldID fid_head = jni::field(&jni, living, {"aK", "rotationYawHead", "field_70759_as"}, "F", "F");
    if (fid_yaw != nullptr && fid_head != nullptr && tw::smooth_rotation_yaw()) {
        env->SetFloatField(entity, fid_head, env->GetFloatField(entity, fid_yaw));
    }
    if (mid != nullptr) {
        env->CallVoidMethod(entity, mid);
    }
}

// NetHandlerPlayClient.handleEntityHeadLook: `if (entity instanceof EntityOtherPlayerMP && smoothRotationYaw
// .isEnabled()) return;` before `entity.setRotationYawHead(f)`.
void JNICALL native_set_rotation_yaw_head(JNIEnv* env, jclass /*cls*/, jobject entity, jfloat yaw) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass base = jni::find_class(&jni, "pk", "net/minecraft/entity/Entity");
    static const jclass other = jni::find_class(&jni, "bex", "net/minecraft/client/entity/EntityOtherPlayerMP");
    static const jmethodID mid = jni::method(&jni, base, {"f", "setRotationYawHead", "func_70034_d"}, "(F)V", "(F)V");
    if (other != nullptr && tw::smooth_rotation_yaw() && env->IsInstanceOf(entity, other)) {
        return;
    }
    if (mid != nullptr) {
        env->CallVoidMethod(entity, mid, yaw);
    }
}

// ===============================================================================================================
// MinimalBobbingTweak (ViewBobbingEvent WORLD)
// ===============================================================================================================

// EntityRenderer.setupCameraTransform: `if (viewBobbing && !ViewBobbingEvent(WORLD).cancelled)
// this.setupViewBobbing(partialTicks);` (the hand keeps its bobbing).
void JNICALL native_setup_view_bobbing(JNIEnv* env, jclass /*cls*/, jobject renderer, jfloat partial_ticks) noexcept {
    if (tw::minimal_bobbing()) {
        return;
    }
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "bfk", "net/minecraft/client/renderer/EntityRenderer");
    static const jmethodID mid = jni::method(&jni, cls, {"e", "setupViewBobbing", "func_78475_f"}, "(F)V", "(F)V");
    if (mid != nullptr) {
        env->CallVoidMethod(renderer, mid, partial_ticks);
    }
}

// ===============================================================================================================
// Hook table
// ===============================================================================================================

constexpr const char* kMinecraft = "net/minecraft/client/Minecraft";
constexpr const char* kGuiIngame = "net/minecraft/client/gui/GuiIngame";
constexpr const char* kGuiIngameForge = "net/minecraftforge/client/GuiIngameForge";
constexpr const char* kGuiNewChat = "net/minecraft/client/gui/GuiNewChat";
constexpr const char* kFontRenderer = "net/minecraft/client/gui/FontRenderer";
constexpr const char* kEntityRenderer = "net/minecraft/client/renderer/EntityRenderer";
constexpr const char* kNetHandler = "net/minecraft/client/network/NetHandlerPlayClient";
constexpr const char* kDisplay = "org/lwjgl/opengl/Display";

constexpr bytecode::ExtraMemberNames kRenderGameOverlay{
    "a", "(F)V", "renderGameOverlay", "(F)V", "func_175180_a", "(F)V"};
constexpr bytecode::ExtraMemberNames kRenderScoreboard{
    "a", "(Lauk;Lavr;)V",
    "renderScoreboard", "(Lnet/minecraft/scoreboard/ScoreObjective;Lnet/minecraft/client/gui/ScaledResolution;)V",
    "func_180475_a", "(Lnet/minecraft/scoreboard/ScoreObjective;Lnet/minecraft/client/gui/ScaledResolution;)V"};
constexpr bytecode::ExtraMemberNames kTitlesTimer{"w", "I", "titlesTimer", "I", "field_175195_w", "I"};
constexpr bytecode::ExtraMemberNames kRenderBossHealth{
    "j", "()V", "renderBossHealth", "()V", "func_73828_d", "()V"};
constexpr bytecode::ExtraMemberNames kDrawRect{"a", "(IIIII)V", "drawRect", "(IIIII)V", "func_73734_a", "(IIIII)V"};
constexpr bytecode::ExtraMemberNames kSetChatLine{
    "a", "(Leu;IIZ)V",
    "setChatLine", "(Lnet/minecraft/util/IChatComponent;IIZ)V",
    "func_146237_a", "(Lnet/minecraft/util/IChatComponent;IIZ)V"};

// ---- NoRenderTweak ----

const ExtraHookSpec kHookTitlesTimer{
    "avo", kGuiIngame, kRenderGameOverlay,
    "avo", kGuiIngame, kTitlesTimer,
    HookInsnKind::GetField,
    "adjustTweaksTitlesTimer", "(Ljava/lang/Object;)I",
    reinterpret_cast<void*>(&native_titles_timer),
};

const ExtraHookSpec kHookTitlesTimerForge{ // GuiIngameForge.renderTitle
    nullptr, kGuiIngameForge, {},
    nullptr, kGuiIngameForge, kTitlesTimer,
    HookInsnKind::GetField,
    "adjustTweaksTitlesTimer", "(Ljava/lang/Object;)I",
    reinterpret_cast<void*>(&native_titles_timer),
};

const ExtraHookSpec kHookBossHealth{
    "avo", kGuiIngame, kRenderGameOverlay,
    "avo", kGuiIngame, kRenderBossHealth,
    HookInsnKind::InvokeInstance,
    "adjustTweaksRenderBossHealth", "(Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_render_boss_health),
};

const ExtraHookSpec kHookBossHealthForge{ // GuiIngameForge.renderGameOverlay -> this.renderBossHealth()
    nullptr, kGuiIngameForge, {},
    nullptr, kGuiIngameForge, kRenderBossHealth,
    HookInsnKind::InvokeInstance,
    "adjustTweaksRenderBossHealth", "(Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_render_boss_health),
};

const ExtraHookSpec kHookAchievement{
    "ave", kMinecraft,
    {"av", "()V", "runGameLoop", "()V", "func_71411_J", "()V"},
    "ayd", "net/minecraft/client/gui/achievement/GuiAchievement",
    {"a", "()V", "updateAchievementWindow", "()V", "func_146254_a", "()V"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksUpdateAchievementWindow", "(Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_update_achievement_window),
};

// ---- ScoreboardTweak ----

const ExtraHookSpec kHookScoreboard{
    "avo", kGuiIngame, kRenderGameOverlay,
    "avo", kGuiIngame, kRenderScoreboard,
    HookInsnKind::InvokeInstance,
    "adjustTweaksRenderScoreboard", "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_render_scoreboard),
};

const ExtraHookSpec kHookScoreboardForge{
    nullptr, kGuiIngameForge, {},
    nullptr, kGuiIngameForge, kRenderScoreboard,
    HookInsnKind::InvokeInstance,
    "adjustTweaksRenderScoreboard", "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_render_scoreboard),
};

// Unqualified drawRect(...) inside GuiIngame: the constant pool owner is GuiIngame (verified with javap).
const ExtraHookSpec kHookScoreboardDrawRect{
    "avo", kGuiIngame, kRenderScoreboard,
    "avo", kGuiIngame, kDrawRect,
    HookInsnKind::InvokeStatic,
    "adjustTweaksScoreboardDrawRect", "(IIIII)V",
    reinterpret_cast<void*>(&native_scoreboard_draw_rect),
};

const ExtraHookSpec kHookScoreboardDrawString{
    "avo", kGuiIngame, kRenderScoreboard,
    "avn", kFontRenderer,
    {"a", "(Ljava/lang/String;III)I", "drawString", "(Ljava/lang/String;III)I", "func_78276_b",
     "(Ljava/lang/String;III)I"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksScoreboardDrawString", "(Ljava/lang/Object;Ljava/lang/Object;III)I",
    reinterpret_cast<void*>(&native_scoreboard_draw_string),
};

const ExtraHookSpec kHookScoreboardStringWidth{
    "avo", kGuiIngame, kRenderScoreboard,
    "avn", kFontRenderer,
    {"a", "(Ljava/lang/String;)I", "getStringWidth", "(Ljava/lang/String;)I", "func_78256_a",
     "(Ljava/lang/String;)I"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksScoreboardStringWidth", "(Ljava/lang/Object;Ljava/lang/Object;)I",
    reinterpret_cast<void*>(&native_scoreboard_string_width),
};

// ---- ChatTweak ----

constexpr bytecode::ExtraMemberNames kDrawChat{"a", "(I)V", "drawChat", "(I)V", "func_146230_a", "(I)V"};

const ExtraHookSpec kHookChatDrawRect{
    "avt", kGuiNewChat, kDrawChat,
    "avt", kGuiNewChat, kDrawRect,
    HookInsnKind::InvokeStatic,
    "adjustTweaksChatDrawRect", "(IIIII)V",
    reinterpret_cast<void*>(&native_chat_draw_rect),
};

const ExtraHookSpec kHookChatLineCounter{
    "avt", kGuiNewChat, kDrawChat,
    "ava", "net/minecraft/client/gui/ChatLine",
    {"b", "()I", "getUpdatedCounter", "()I", "func_74540_b", "()I"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksChatLineCounter", "(Ljava/lang/Object;)I",
    reinterpret_cast<void*>(&native_chat_line_counter),
};

const ExtraHookSpec kHookChatDrawString{
    "avt", kGuiNewChat, kDrawChat,
    "avn", kFontRenderer,
    {"a", "(Ljava/lang/String;FFI)I", "drawStringWithShadow", "(Ljava/lang/String;FFI)I", "func_175063_a",
     "(Ljava/lang/String;FFI)I"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksChatDrawString", "(Ljava/lang/Object;Ljava/lang/Object;FFI)I",
    reinterpret_cast<void*>(&native_chat_draw_string),
};

const ExtraHookSpec kHookChatSplitText{
    "avt", kGuiNewChat, kSetChatLine,
    "avu", "net/minecraft/client/gui/GuiUtilRenderComponents",
    {"a", "(Leu;ILavn;ZZ)Ljava/util/List;",
     "splitText", "(Lnet/minecraft/util/IChatComponent;ILnet/minecraft/client/gui/FontRenderer;ZZ)Ljava/util/List;",
     "func_178908_a", "(Lnet/minecraft/util/IChatComponent;ILnet/minecraft/client/gui/FontRenderer;ZZ)Ljava/util/List;"},
    HookInsnKind::InvokeStatic,
    "adjustTweaksChatSplitText", "(Ljava/lang/Object;ILjava/lang/Object;ZZ)Ljava/util/List;",
    reinterpret_cast<void*>(&native_chat_split_text),
};

// ---- NoHotbarScrollTweak / ZoomTweak ----

const ExtraHookSpec kHookChangeCurrentItem{
    "ave", kMinecraft,
    {"s", "()V", "runTick", "()V", "func_71407_l", "()V"},
    "wm", "net/minecraft/entity/player/InventoryPlayer",
    {"d", "(I)V", "changeCurrentItem", "(I)V", "func_70453_c", "(I)V"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksChangeCurrentItem", "(Ljava/lang/Object;I)V",
    reinterpret_cast<void*>(&native_change_current_item),
};

// getFOVModifier is called from setupCameraTransform, renderHand, renderWorldPass and renderCloudsCheck.
const ExtraHookSpec kHookFovModifier{
    "bfk", kEntityRenderer, {},
    "bfk", kEntityRenderer,
    {"a", "(FZ)F", "getFOVModifier", "(FZ)F", "func_78481_a", "(FZ)F"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksGetFovModifier", "(Ljava/lang/Object;FZ)F",
    reinterpret_cast<void*>(&native_get_fov_modifier),
};

// ---- FullscreenTweak ----

const ExtraHookSpec kHookSetFullscreen{
    "ave", kMinecraft,
    {"q", "()V", "toggleFullscreen", "()V", "func_71352_k", "()V"},
    kDisplay, kDisplay,
    {"setFullscreen", "(Z)V", "setFullscreen", "(Z)V", "setFullscreen", "(Z)V"},
    HookInsnKind::InvokeStatic,
    "adjustTweaksSetFullscreen", "(Z)V",
    reinterpret_cast<void*>(&native_set_fullscreen),
};

// ---- DisconnectConfirmTweak ----

const ExtraHookSpec kHookActionPerformed{
    "axu", "net/minecraft/client/gui/GuiScreen",
    {"a", "(III)V", "mouseClicked", "(III)V", "func_73864_a", "(III)V"},
    "axu", "net/minecraft/client/gui/GuiScreen",
    {"a", "(Lavs;)V", "actionPerformed", "(Lnet/minecraft/client/gui/GuiButton;)V", "func_146284_a",
     "(Lnet/minecraft/client/gui/GuiButton;)V"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksActionPerformed", "(Ljava/lang/Object;Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_action_performed),
};

// ---- TabOverlayTweak ----

const ExtraHookSpec kHookTabPlayerInfoMap{
    "awh", "net/minecraft/client/gui/GuiPlayerTabOverlay",
    {"a", "(ILauo;Lauk;)V",
     "renderPlayerlist", "(ILnet/minecraft/scoreboard/Scoreboard;Lnet/minecraft/scoreboard/ScoreObjective;)V",
     "func_175249_a", "(ILnet/minecraft/scoreboard/Scoreboard;Lnet/minecraft/scoreboard/ScoreObjective;)V"},
    "bcy", kNetHandler,
    {"d", "()Ljava/util/Collection;", "getPlayerInfoMap", "()Ljava/util/Collection;", "func_175106_d",
     "()Ljava/util/Collection;"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksTabPlayerInfoMap", "(Ljava/lang/Object;)Ljava/util/Collection;",
    reinterpret_cast<void*>(&native_tab_player_info_map),
};

// ---- SmoothRotationYawTweak ----

// this.updateArmSwingProgress() inside EntityOtherPlayerMP: the constant pool owner is EntityOtherPlayerMP.
const ExtraHookSpec kHookOtherPlayerSwing{
    "bex", "net/minecraft/client/entity/EntityOtherPlayerMP",
    {"m", "()V", "onLivingUpdate", "()V", "func_70636_d", "()V"},
    "bex", "net/minecraft/client/entity/EntityOtherPlayerMP",
    {"bx", "()V", "updateArmSwingProgress", "()V", "func_82168_bl", "()V"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksOtherPlayerSwing", "(Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_other_player_swing),
};

const ExtraHookSpec kHookHeadLook{
    "bcy", kNetHandler,
    {"a", "(Lhf;)V",
     "handleEntityHeadLook", "(Lnet/minecraft/network/play/server/S19PacketEntityHeadLook;)V",
     "func_147267_a", "(Lnet/minecraft/network/play/server/S19PacketEntityHeadLook;)V"},
    "pk", "net/minecraft/entity/Entity",
    {"f", "(F)V", "setRotationYawHead", "(F)V", "func_70034_d", "(F)V"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksSetRotationYawHead", "(Ljava/lang/Object;F)V",
    reinterpret_cast<void*>(&native_set_rotation_yaw_head),
};

// ---- MinimalBobbingTweak ----

const ExtraHookSpec kHookViewBobbing{
    "bfk", kEntityRenderer,
    {"a", "(FI)V", "setupCameraTransform", "(FI)V", "func_78479_a", "(FI)V"},
    "bfk", kEntityRenderer,
    {"e", "(F)V", "setupViewBobbing", "(F)V", "func_78475_f", "(F)V"},
    HookInsnKind::InvokeInstance,
    "adjustTweaksSetupViewBobbing", "(Ljava/lang/Object;F)V",
    reinterpret_cast<void*>(&native_setup_view_bobbing),
};

} // namespace

void tweaks_set_chat_line(JNIEnv* env, jobject gui_new_chat, jobject shown, jobject component, jint chat_line_id,
                          jint update_counter, jboolean display_only) noexcept {
    set_chat_line_impl(env, gui_new_chat, shown, component, chat_line_id, update_counter, display_only);
}

const bytecode::ExtraHookSpec* const* tweaks_hooks() noexcept {
    static const ExtraHookSpec* const s_hooks[] = {
        &kHookTitlesTimer,
        &kHookTitlesTimerForge,
        &kHookBossHealth,
        &kHookBossHealthForge,
        &kHookAchievement,
        &kHookScoreboard,
        &kHookScoreboardForge,
        &kHookScoreboardDrawRect,
        &kHookScoreboardDrawString,
        &kHookScoreboardStringWidth,
        &kHookChatDrawRect,
        &kHookChatLineCounter,
        &kHookChatDrawString,
        &kHookChatSplitText,
        &kHookChangeCurrentItem,
        &kHookFovModifier,
        &kHookSetFullscreen,
        &kHookActionPerformed,
        &kHookTabPlayerInfoMap,
        &kHookOtherPlayerSwing,
        &kHookHeadLook,
        &kHookViewBobbing,
        nullptr,
    };
    return s_hooks;
}

} // namespace slinky::adjust::hooks
