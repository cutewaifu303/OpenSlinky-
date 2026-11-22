// Adjust Streamer hooks (Minecraft 1.8.9) - the Minecraft side of modules/adjust/render/streamer:
//   * GuiNewChat.printChatMessageWithOptionalDeletion / refreshChat -> setChatLine: sender registration and a
//     deep-copied, name-masked chat component is displayed (Adjust: StreamerModule.onChatReceived on the
//     ChatLineEvent pre of GuiPersistentChat.setChatLine). The stored history line keeps the original component,
//     so toggling the module + refreshChat() re-renders the history masked / unmasked like Adjust.
//   * GuiIngame.renderScoreboard -> Scoreboard.getSortedScores: masked Score copies (ScoreboardEntryEvent).
//   * GuiPlayerTabOverlay.renderPlayerlist -> getPlayerName: replaceName(s, profile name).
//   * Render.renderName / Render.renderOffsetLivingLabel / RenderPlayer.renderOffsetLivingLabel ->
//     renderLivingLabel: replaceName(str, profile name) for players (Adjust patches renderLivingLabel itself).
//   * RendererLivingEntity.renderName: players -> replaceName(s, profile name), armor stands ->
//     replaceKnownNames(s). The string comes from an interface call (IChatComponent.getFormattedText), which the
//     rewriter cannot replace, so its consumers are hooked: getDistanceSqToEntity (records the entity), the
//     sneaking branch's FontRenderer.getStringWidth / drawString and the renderOffsetLivingLabel call.
//   * TileEntitySignRenderer.renderTileEntityAt -> GuiUtilRenderComponents.splitText: each line is rendered as
//     new ChatComponentText(replaceKnownNames(line.getFormattedText())).
//   * GuiChat.keyTyped -> GuiScreen.sendChatMessage(String): restoreNameInText for every masked tab-list name.
//
// Every native calls the original member (with the original or the masked arguments); exceptions thrown by the
// original call stay pending for Java. Exceptions thrown by our own extra JNI calls are cleared and the original
// value is used instead.

#include "adjust/hooks/hooks.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "adjust/hooks/tweaks_hooks.hpp"
#include "adjust/jni_lookup.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/streamer/streamer.hpp"

namespace slinky::adjust::hooks {

const bytecode::ExtraHookSpec* const* streamer_hooks() noexcept;

namespace {

using bytecode::ExtraHookSpec;
using bytecode::HookInsnKind;
namespace streamer = modules::adjust_streamer;

constexpr int kOrdinalGreen = 10;      // EnumChatFormatting.GREEN
constexpr int kOrdinalRunCommand = 2;  // ClickEvent.Action.RUN_COMMAND
constexpr int kMaxComponentDepth = 32; // guards against pathological component trees

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------

// Clears an exception raised by one of our own extra calls (never call it right after the original member).
bool failed(JNIEnv* env) noexcept {
    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

std::string to_utf8(JNIEnv* env, jstring s) {
    std::string out;
    if (s == nullptr) {
        return out;
    }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (chars != nullptr) {
        out = chars;
        env->ReleaseStringUTFChars(s, chars);
    }
    return out;
}

// New Java string, or nullptr (exception cleared).
jstring to_java(JNIEnv* env, const std::string& s) noexcept {
    jstring out = env->NewStringUTF(s.c_str());
    if (failed(env)) {
        return nullptr;
    }
    return out;
}

struct CommonIds {
    jclass entity_player = nullptr;   // EntityPlayer
    jclass armor_stand = nullptr;     // EntityArmorStand
    jmethodID player_profile = nullptr; // EntityPlayer.getGameProfile()
    jmethodID profile_name = nullptr;   // GameProfile.getName()
    jclass java_string = nullptr;
    jclass java_enum = nullptr;
    jmethodID enum_ordinal = nullptr;
    jclass list = nullptr;
    jmethodID list_size = nullptr;
    jmethodID list_get = nullptr;
    jmethodID list_set = nullptr;
    jmethodID list_add = nullptr;
    jclass collection = nullptr;
    jmethodID collection_to_array = nullptr;
    jclass array_list = nullptr;
    jmethodID array_list_ctor = nullptr;
    jclass collections = nullptr;
    jmethodID collections_sort = nullptr;
};

const CommonIds& common_ids(events::JniContext* jni) noexcept {
    static const CommonIds s_ids = [&] {
        CommonIds ids{};
        ids.entity_player = jni::find_class(jni, "wn", "net/minecraft/entity/player/EntityPlayer");
        ids.armor_stand = jni::find_class(jni, "um", "net/minecraft/entity/item/EntityArmorStand");
        ids.player_profile = jni::method(jni, ids.entity_player, {"cd", "getGameProfile", "func_146103_bH"},
                                         "()Lcom/mojang/authlib/GameProfile;", "()Lcom/mojang/authlib/GameProfile;");
        const jclass profile = jni::find_class(jni, "com/mojang/authlib/GameProfile", "com/mojang/authlib/GameProfile");
        ids.profile_name = jni::method(jni, profile, {"getName", "getName", "getName"}, "()Ljava/lang/String;",
                                       "()Ljava/lang/String;");
        ids.java_string = jni::find_class(jni, "java/lang/String", "java/lang/String");
        ids.java_enum = jni::find_class(jni, "java/lang/Enum", "java/lang/Enum");
        ids.enum_ordinal = jni::method(jni, ids.java_enum, {"ordinal", "ordinal", "ordinal"}, "()I", "()I");
        ids.list = jni::find_class(jni, "java/util/List", "java/util/List");
        ids.list_size = jni::method(jni, ids.list, {"size", "size", "size"}, "()I", "()I");
        ids.list_get = jni::method(jni, ids.list, {"get", "get", "get"}, "(I)Ljava/lang/Object;",
                                   "(I)Ljava/lang/Object;");
        ids.list_set = jni::method(jni, ids.list, {"set", "set", "set"}, "(ILjava/lang/Object;)Ljava/lang/Object;",
                                   "(ILjava/lang/Object;)Ljava/lang/Object;");
        ids.list_add = jni::method(jni, ids.list, {"add", "add", "add"}, "(Ljava/lang/Object;)Z",
                                   "(Ljava/lang/Object;)Z");
        ids.collection = jni::find_class(jni, "java/util/Collection", "java/util/Collection");
        ids.collection_to_array = jni::method(jni, ids.collection, {"toArray", "toArray", "toArray"},
                                              "()[Ljava/lang/Object;", "()[Ljava/lang/Object;");
        ids.array_list = jni::find_class(jni, "java/util/ArrayList", "java/util/ArrayList");
        ids.array_list_ctor = jni::method(jni, ids.array_list, {"<init>", "<init>", "<init>"}, "(I)V", "(I)V");
        ids.collections = jni::find_class(jni, "java/util/Collections", "java/util/Collections");
        ids.collections_sort = jni::static_method(jni, ids.collections, {"sort", "sort", "sort"},
                                                  "(Ljava/util/List;Ljava/util/Comparator;)V",
                                                  "(Ljava/util/List;Ljava/util/Comparator;)V");
        return ids;
    }();
    return s_ids;
}

bool is_instance(JNIEnv* env, jobject obj, jclass cls) noexcept {
    return obj != nullptr && cls != nullptr && env->IsInstanceOf(obj, cls) == JNI_TRUE;
}

int enum_ordinal(JNIEnv* env, const CommonIds& ids, jobject value) noexcept {
    if (value == nullptr || ids.enum_ordinal == nullptr) {
        return -1;
    }
    const jint ordinal = env->CallIntMethod(value, ids.enum_ordinal);
    return failed(env) ? -1 : static_cast<int>(ordinal);
}

// ((EntityPlayer) entity).getGameProfile().getName(), false if not a player / no profile.
bool player_profile_name(JNIEnv* env, const CommonIds& ids, jobject entity, std::string* out) {
    if (ids.player_profile == nullptr || ids.profile_name == nullptr || !is_instance(env, entity, ids.entity_player)) {
        return false;
    }
    jobject profile = env->CallObjectMethod(entity, ids.player_profile);
    if (failed(env) || profile == nullptr) {
        return false;
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(profile, ids.profile_name));
    env->DeleteLocalRef(profile);
    if (failed(env) || name == nullptr) {
        return false;
    }
    *out = to_utf8(env, name);
    env->DeleteLocalRef(name);
    return true;
}

// Name-tag masking of one entity (RendererLivingEntity.renderName rules).
enum class LabelKind : int { kNone = 0, kPlayer = 1, kArmorStand = 2 };

struct LabelContext {
    LabelKind kind = LabelKind::kNone;
    std::string profile;
};

LabelContext label_context(JNIEnv* env, const CommonIds& ids, jobject entity, bool armor_stands) {
    LabelContext ctx;
    if (player_profile_name(env, ids, entity, &ctx.profile)) {
        ctx.kind = LabelKind::kPlayer;
    } else if (armor_stands && is_instance(env, entity, ids.armor_stand)) {
        ctx.kind = LabelKind::kArmorStand;
    }
    return ctx;
}

// Masked copy of `str` per `ctx`, or nullptr when unchanged (caller keeps `str`).
jstring mask_label(JNIEnv* env, const LabelContext& ctx, jstring str) {
    if (str == nullptr || ctx.kind == LabelKind::kNone) {
        return nullptr;
    }
    const std::string text = to_utf8(env, str);
    std::string masked;
    if (ctx.kind == LabelKind::kPlayer) {
        if (ctx.profile.empty()) {
            return nullptr;
        }
        masked = streamer::replace_name(text, ctx.profile);
    } else {
        masked = streamer::replace_known_names(text);
    }
    return masked == text ? nullptr : to_java(env, masked);
}

// ---------------------------------------------------------------------------------------------------------------
// GuiNewChat.setChatLine(IChatComponent, int, int, boolean) - replaced in printChatMessageWithOptionalDeletion and
// refreshChat
// ---------------------------------------------------------------------------------------------------------------

struct ChatIds {
    jclass new_chat = nullptr;
    jmethodID set_chat_line = nullptr;
    jfieldID chat_lines = nullptr;
    jclass chat_line = nullptr;
    jmethodID chat_line_ctor = nullptr;
    jmethodID chat_line_component = nullptr;
    jclass component = nullptr;
    jmethodID create_copy = nullptr;
    jmethodID siblings = nullptr;
    jmethodID chat_style = nullptr;
    jmethodID formatted_text = nullptr;
    jmethodID unformatted_for_chat = nullptr;
    jclass text = nullptr;
    jfieldID text_value = nullptr;
    jclass translation = nullptr;
    jmethodID translation_key = nullptr;
    jmethodID format_args = nullptr;
    jmethodID style_color = nullptr;
    jmethodID style_click = nullptr;
    jmethodID click_action = nullptr;
    jmethodID click_value = nullptr;
    jmethodID mc_server_data = nullptr;
    jfieldID server_ip = nullptr;
};

const ChatIds& chat_ids(events::JniContext* jni) noexcept {
    static const ChatIds s_ids = [&] {
        ChatIds ids{};
        ids.new_chat = jni::find_class(jni, "avt", "net/minecraft/client/gui/GuiNewChat");
        ids.set_chat_line = jni::method(jni, ids.new_chat, {"a", "setChatLine", "func_146237_a"}, "(Leu;IIZ)V",
                                        "(Lnet/minecraft/util/IChatComponent;IIZ)V");
        ids.chat_lines = jni::field(jni, ids.new_chat, {"h", "chatLines", "field_146252_h"}, "Ljava/util/List;",
                                    "Ljava/util/List;");
        ids.chat_line = jni::find_class(jni, "ava", "net/minecraft/client/gui/ChatLine");
        ids.chat_line_ctor = jni::method(jni, ids.chat_line, {"<init>", "<init>", "<init>"}, "(ILeu;I)V",
                                         "(ILnet/minecraft/util/IChatComponent;I)V");
        ids.chat_line_component = jni::method(jni, ids.chat_line, {"a", "getChatComponent", "func_151461_a"},
                                              "()Leu;", "()Lnet/minecraft/util/IChatComponent;");
        ids.component = jni::find_class(jni, "eu", "net/minecraft/util/IChatComponent");
        ids.create_copy = jni::method(jni, ids.component, {"f", "createCopy", "func_150259_f"}, "()Leu;",
                                      "()Lnet/minecraft/util/IChatComponent;");
        ids.siblings = jni::method(jni, ids.component, {"a", "getSiblings", "func_150253_a"}, "()Ljava/util/List;",
                                   "()Ljava/util/List;");
        ids.chat_style = jni::method(jni, ids.component, {"b", "getChatStyle", "func_150256_b"}, "()Lez;",
                                     "()Lnet/minecraft/util/ChatStyle;");
        ids.formatted_text = jni::method(jni, ids.component, {"d", "getFormattedText", "func_150254_d"},
                                         "()Ljava/lang/String;", "()Ljava/lang/String;");
        ids.unformatted_for_chat = jni::method(jni, ids.component, {"e", "getUnformattedTextForChat", "func_150261_e"},
                                               "()Ljava/lang/String;", "()Ljava/lang/String;");
        ids.text = jni::find_class(jni, "fa", "net/minecraft/util/ChatComponentText");
        ids.text_value = jni::field(jni, ids.text, {"b", "text", "field_150267_b"}, "Ljava/lang/String;",
                                    "Ljava/lang/String;");
        ids.translation = jni::find_class(jni, "fb", "net/minecraft/util/ChatComponentTranslation");
        ids.translation_key = jni::method(jni, ids.translation, {"i", "getKey", "func_150268_i"},
                                          "()Ljava/lang/String;", "()Ljava/lang/String;");
        ids.format_args = jni::method(jni, ids.translation, {"j", "getFormatArgs", "func_150271_j"},
                                      "()[Ljava/lang/Object;", "()[Ljava/lang/Object;");
        const jclass style = jni::find_class(jni, "ez", "net/minecraft/util/ChatStyle");
        ids.style_color = jni::method(jni, style, {"a", "getColor", "func_150215_a"}, "()La;",
                                      "()Lnet/minecraft/util/EnumChatFormatting;");
        ids.style_click = jni::method(jni, style, {"h", "getChatClickEvent", "func_150235_h"}, "()Let;",
                                      "()Lnet/minecraft/event/ClickEvent;");
        const jclass click = jni::find_class(jni, "et", "net/minecraft/event/ClickEvent");
        ids.click_action = jni::method(jni, click, {"a", "getAction", "func_150669_a"}, "()Let$a;",
                                       "()Lnet/minecraft/event/ClickEvent$Action;");
        ids.click_value = jni::method(jni, click, {"b", "getValue", "func_150668_b"}, "()Ljava/lang/String;",
                                      "()Ljava/lang/String;");
        ids.mc_server_data = jni::method(jni, jni->mappings->cls_minecraft,
                                         {"D", "getCurrentServerData", "func_147104_D"}, "()Lbde;",
                                         "()Lnet/minecraft/client/multiplayer/ServerData;");
        const jclass server_data = jni::find_class(jni, "bde", "net/minecraft/client/multiplayer/ServerData");
        ids.server_ip = jni::field(jni, server_data, {"b", "serverIP", "field_78845_b"}, "Ljava/lang/String;",
                                   "Ljava/lang/String;");
        return ids;
    }();
    return s_ids;
}

bool masking_supported(const ChatIds& ids) noexcept {
    return ids.create_copy != nullptr && ids.siblings != nullptr && ids.chat_style != nullptr &&
           ids.text != nullptr && ids.text_value != nullptr && ids.translation != nullptr &&
           ids.format_args != nullptr && ids.style_color != nullptr;
}

// ServerTracker: Adjust only parses chat senders on Hypixel.
bool on_hypixel(events::JniContext* jni, const ChatIds& ids) {
    if (ids.mc_server_data == nullptr || ids.server_ip == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    jobject mc = minecraft::mc_get_instance(jni);
    if (failed(env) || mc == nullptr) {
        return false;
    }
    jobject data = env->CallObjectMethod(mc, ids.mc_server_data);
    if (failed(env) || data == nullptr) {
        return false;
    }
    auto ip = static_cast<jstring>(env->GetObjectField(data, ids.server_ip));
    env->DeleteLocalRef(data);
    if (failed(env) || ip == nullptr) {
        return false;
    }
    std::string host = to_utf8(env, ip);
    env->DeleteLocalRef(ip);
    for (char& c : host) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return host.find("hypixel.net") != std::string::npos;
}

// StreamerModule.extractProfileClickName(component).
bool profile_click_name(JNIEnv* env, const CommonIds& common, const ChatIds& ids, jobject component, std::string* out) {
    if (ids.style_click == nullptr || ids.click_action == nullptr || ids.click_value == nullptr) {
        return false;
    }
    jobject style = env->CallObjectMethod(component, ids.chat_style);
    if (failed(env) || style == nullptr) {
        return false;
    }
    jobject click = env->CallObjectMethod(style, ids.style_click);
    env->DeleteLocalRef(style);
    if (failed(env) || click == nullptr) {
        return false;
    }
    bool found = false;
    jobject action = env->CallObjectMethod(click, ids.click_action);
    if (!failed(env) && action != nullptr) {
        if (enum_ordinal(env, common, action) == kOrdinalRunCommand) {
            auto value = static_cast<jstring>(env->CallObjectMethod(click, ids.click_value));
            if (!failed(env) && value != nullptr) {
                found = streamer::profile_click_name(to_utf8(env, value), out);
                env->DeleteLocalRef(value);
            }
        }
        env->DeleteLocalRef(action);
    }
    env->DeleteLocalRef(click);
    return found;
}

// StreamerModule.getChatSenderName(component).
bool chat_sender_name(JNIEnv* env, const CommonIds& common, const ChatIds& ids, jobject component, std::string* out) {
    if (profile_click_name(env, common, ids, component, out)) {
        return true;
    }
    jobject siblings = env->CallObjectMethod(component, ids.siblings);
    if (!failed(env) && siblings != nullptr) {
        const jint count = env->CallIntMethod(siblings, common.list_size);
        for (jint i = 0; !failed(env) && i < count; ++i) {
            jobject sibling = env->CallObjectMethod(siblings, common.list_get, i);
            if (failed(env)) {
                break;
            }
            const bool found = sibling != nullptr && profile_click_name(env, common, ids, sibling, out);
            env->DeleteLocalRef(sibling);
            if (found) {
                env->DeleteLocalRef(siblings);
                return true;
            }
        }
        env->DeleteLocalRef(siblings);
    }
    auto formatted = static_cast<jstring>(env->CallObjectMethod(component, ids.formatted_text));
    if (failed(env) || formatted == nullptr) {
        return false;
    }
    const bool found = streamer::chat_sender_name(to_utf8(env, formatted), out);
    env->DeleteLocalRef(formatted);
    return found;
}

// onChatReceived: sender registration (before the copy is masked).
void register_chat_sender(events::JniContext* jni, const CommonIds& common, const ChatIds& ids, jobject component) {
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(32) != 0) {
        failed(env);
        return;
    }
    std::string sender;
    if (on_hypixel(jni, ids) && chat_sender_name(env, common, ids, component, &sender)) {
        streamer::replace_names(sender);
    }
    if (is_instance(env, component, ids.translation) && ids.translation_key != nullptr) {
        auto key = static_cast<jstring>(env->CallObjectMethod(component, ids.translation_key));
        if (!failed(env) && key != nullptr && to_utf8(env, key) == "chat.type.text") {
            auto args = static_cast<jobjectArray>(env->CallObjectMethod(component, ids.format_args));
            if (!failed(env) && args != nullptr && env->GetArrayLength(args) > 0) {
                jobject first = env->GetObjectArrayElement(args, 0);
                if (!failed(env) && first != nullptr) {
                    if (is_instance(env, first, ids.component) && ids.unformatted_for_chat != nullptr) {
                        auto raw = static_cast<jstring>(env->CallObjectMethod(first, ids.unformatted_for_chat));
                        if (!failed(env) && raw != nullptr) {
                            streamer::register_chat_sender_argument(to_utf8(env, raw));
                        }
                    } else if (is_instance(env, first, common.java_string)) {
                        streamer::register_chat_sender_argument(to_utf8(env, static_cast<jstring>(first)));
                    }
                }
            }
        }
    }
    env->PopLocalFrame(nullptr);
}

// StreamerModule.replaceComponent (+ maskServerIds) on a deep copy. False on a JNI failure.
bool mask_component(JNIEnv* env, const CommonIds& common, const ChatIds& ids, jobject component, int depth) {
    if (component == nullptr || depth > kMaxComponentDepth) {
        return true;
    }
    if (env->PushLocalFrame(16) != 0) {
        failed(env);
        return false;
    }
    bool ok = true;
    if (is_instance(env, component, ids.text)) {
        auto value = static_cast<jstring>(env->GetObjectField(component, ids.text_value));
        if (failed(env)) {
            ok = false;
        } else if (value != nullptr) {
            const std::string original = to_utf8(env, value);
            std::string text = streamer::replace_known_names(original);
            // maskServerIds: only for GREEN text.
            jobject style = env->CallObjectMethod(component, ids.chat_style);
            if (!failed(env) && style != nullptr) {
                jobject color = env->CallObjectMethod(style, ids.style_color);
                if (!failed(env) && enum_ordinal(env, common, color) == kOrdinalGreen) {
                    std::string server_masked;
                    if (streamer::mask_server_ids(text, &server_masked)) {
                        text = std::move(server_masked);
                    }
                }
            }
            if (text != original) {
                jstring replaced = to_java(env, text);
                if (replaced == nullptr) {
                    ok = false;
                } else {
                    env->SetObjectField(component, ids.text_value, replaced); // ChatComponentText.text is final:
                    ok = !failed(env);                                        // JNI ignores that, like Adjust's patch
                }
            }
        }
    } else if (is_instance(env, component, ids.translation)) {
        auto args = static_cast<jobjectArray>(env->CallObjectMethod(component, ids.format_args));
        if (failed(env)) {
            ok = false;
        } else if (args != nullptr) {
            const jsize count = env->GetArrayLength(args);
            for (jsize i = 0; ok && i < count; ++i) {
                jobject arg = env->GetObjectArrayElement(args, i);
                if (failed(env)) {
                    ok = false;
                    break;
                }
                if (is_instance(env, arg, ids.component)) {
                    ok = mask_component(env, common, ids, arg, depth + 1);
                } else if (is_instance(env, arg, common.java_string)) {
                    const std::string original = to_utf8(env, static_cast<jstring>(arg));
                    const std::string text = streamer::replace_known_names(original);
                    if (text != original) {
                        jstring replaced = to_java(env, text);
                        if (replaced == nullptr) {
                            ok = false;
                        } else {
                            env->SetObjectArrayElement(args, i, replaced);
                            ok = !failed(env);
                            env->DeleteLocalRef(replaced);
                        }
                    }
                }
                env->DeleteLocalRef(arg);
            }
        }
    }
    if (ok) {
        jobject siblings = env->CallObjectMethod(component, ids.siblings);
        if (failed(env)) {
            ok = false;
        } else if (siblings != nullptr) {
            const jint count = env->CallIntMethod(siblings, common.list_size);
            ok = !failed(env);
            for (jint i = 0; ok && i < count; ++i) {
                jobject sibling = env->CallObjectMethod(siblings, common.list_get, i);
                if (failed(env)) {
                    ok = false;
                    break;
                }
                ok = mask_component(env, common, ids, sibling, depth + 1);
                env->DeleteLocalRef(sibling);
            }
        }
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

// Registers the sender and returns a masked deep copy (local ref), or nullptr to display the original.
jobject masked_chat_copy(events::JniContext* jni, const CommonIds& common, const ChatIds& ids, jobject component) {
    JNIEnv* env = jni->env;
    register_chat_sender(jni, common, ids, component);
    jobject copy = env->CallObjectMethod(component, ids.create_copy);
    if (failed(env) || copy == nullptr) {
        return nullptr;
    }
    if (!mask_component(env, common, ids, copy, 0)) {
        env->DeleteLocalRef(copy);
        return nullptr;
    }
    return copy;
}

// After setChatLine(masked, ..., false): chatLines[0] keeps the original component instead of the masked copy.
void keep_original_in_history(JNIEnv* env, const CommonIds& common, const ChatIds& ids, jobject gui,
                              jobject original, jobject masked, jint id, jint counter) {
    if (ids.chat_lines == nullptr || ids.chat_line_ctor == nullptr || ids.chat_line_component == nullptr ||
        common.list_set == nullptr) {
        return;
    }
    jobject lines = env->GetObjectField(gui, ids.chat_lines);
    if (failed(env) || lines == nullptr) {
        return;
    }
    const jint size = env->CallIntMethod(lines, common.list_size);
    if (!failed(env) && size > 0) {
        jobject first = env->CallObjectMethod(lines, common.list_get, 0);
        if (!failed(env) && first != nullptr) {
            jobject stored = env->CallObjectMethod(first, ids.chat_line_component);
            if (!failed(env) && env->IsSameObject(stored, masked) == JNI_TRUE) {
                jobject line = env->NewObject(ids.chat_line, ids.chat_line_ctor, counter, original, id);
                if (!failed(env) && line != nullptr) {
                    jobject previous = env->CallObjectMethod(lines, common.list_set, 0, line);
                    failed(env);
                    env->DeleteLocalRef(previous);
                    env->DeleteLocalRef(line);
                }
            }
            env->DeleteLocalRef(stored);
            env->DeleteLocalRef(first);
        }
    }
    env->DeleteLocalRef(lines);
}

void JNICALL native_set_chat_line(JNIEnv* env, jclass /*cls*/, jobject gui, jobject component, jint id,
                                  jint counter, jboolean display_only) noexcept {
    events::JniContext jni = jni_for(env);
    const ChatIds& ids = chat_ids(&jni);
    if (ids.set_chat_line == nullptr) {
        return;
    }
    const auto set_chat_line = [&](jobject shown, jboolean only_display) noexcept {
        // Adjust Tweaks (chat stacking, player heads) wraps the original setChatLine call.
        tweaks_set_chat_line(env, gui, shown, component, id, counter, only_display);
    };
    const CommonIds& common = common_ids(&jni);
    jobject masked = nullptr;
    if (component != nullptr && masking_supported(ids) && common.list_size != nullptr &&
        common.list_get != nullptr && streamer::active()) {
        masked = masked_chat_copy(&jni, common, ids, component);
    }
    if (masked == nullptr) {
        set_chat_line(component, display_only);
        return;
    }
    set_chat_line(masked, display_only);
    if (display_only == JNI_FALSE && env->ExceptionCheck() == JNI_FALSE) {
        keep_original_in_history(env, common, ids, gui, component, masked, id, counter);
    }
    if (env->ExceptionCheck() == JNI_FALSE) {
        env->DeleteLocalRef(masked);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Scoreboard.getSortedScores(ScoreObjective) - replaced in GuiIngame.renderScoreboard
// ---------------------------------------------------------------------------------------------------------------

struct ScoreIds {
    jmethodID sorted_scores = nullptr;
    jmethodID players_team = nullptr;
    jclass player_team = nullptr;
    jmethodID format_player_name = nullptr;
    jclass score = nullptr;
    jmethodID score_ctor = nullptr;
    jmethodID player_name = nullptr;
    jmethodID objective = nullptr;
    jmethodID points = nullptr;
    jmethodID set_points = nullptr;
    jmethodID locked = nullptr;
    jmethodID set_locked = nullptr;
    jfieldID comparator = nullptr;
};

const ScoreIds& score_ids(events::JniContext* jni) noexcept {
    static const ScoreIds s_ids = [&] {
        ScoreIds ids{};
        const jclass scoreboard = jni::find_class(jni, "auo", "net/minecraft/scoreboard/Scoreboard");
        ids.sorted_scores = jni::method(jni, scoreboard, {"i", "getSortedScores", "func_96534_i"},
                                        "(Lauk;)Ljava/util/Collection;",
                                        "(Lnet/minecraft/scoreboard/ScoreObjective;)Ljava/util/Collection;");
        ids.players_team = jni::method(jni, scoreboard, {"h", "getPlayersTeam", "func_96509_i"},
                                       "(Ljava/lang/String;)Laul;",
                                       "(Ljava/lang/String;)Lnet/minecraft/scoreboard/ScorePlayerTeam;");
        ids.player_team = jni::find_class(jni, "aul", "net/minecraft/scoreboard/ScorePlayerTeam");
        ids.format_player_name = jni::static_method(
            jni, ids.player_team, {"a", "formatPlayerName", "func_96667_a"},
            "(Lauq;Ljava/lang/String;)Ljava/lang/String;",
            "(Lnet/minecraft/scoreboard/Team;Ljava/lang/String;)Ljava/lang/String;");
        ids.score = jni::find_class(jni, "aum", "net/minecraft/scoreboard/Score");
        ids.score_ctor = jni::method(
            jni, ids.score, {"<init>", "<init>", "<init>"}, "(Lauo;Lauk;Ljava/lang/String;)V",
            "(Lnet/minecraft/scoreboard/Scoreboard;Lnet/minecraft/scoreboard/ScoreObjective;Ljava/lang/String;)V");
        ids.player_name = jni::method(jni, ids.score, {"e", "getPlayerName", "func_96653_e"}, "()Ljava/lang/String;",
                                      "()Ljava/lang/String;");
        ids.objective = jni::method(jni, ids.score, {"d", "getObjective", "func_96645_d"}, "()Lauk;",
                                    "()Lnet/minecraft/scoreboard/ScoreObjective;");
        ids.points = jni::method(jni, ids.score, {"c", "getScorePoints", "func_96652_c"}, "()I", "()I");
        ids.set_points = jni::method(jni, ids.score, {"c", "setScorePoints", "func_96647_c"}, "(I)V", "(I)V");
        ids.locked = jni::method(jni, ids.score, {"g", "isLocked", "func_178816_g"}, "()Z", "()Z");
        ids.set_locked = jni::method(jni, ids.score, {"a", "setLocked", "func_178815_a"}, "(Z)V", "(Z)V");
        ids.comparator = jni::static_field(jni, ids.score, {"a", "scoreComparator", "field_96658_a"},
                                           "Ljava/util/Comparator;", "Ljava/util/Comparator;");
        return ids;
    }();
    return s_ids;
}

// One entry of StreamerModule.getScoreboardName: masked Score (local ref) or nullptr to keep `score`.
jobject masked_score(JNIEnv* env, const ScoreIds& ids, jobject scoreboard, jobject score) {
    auto name = static_cast<jstring>(env->CallObjectMethod(score, ids.player_name));
    if (failed(env) || name == nullptr) {
        return nullptr;
    }
    jobject team = env->CallObjectMethod(scoreboard, ids.players_team, name);
    if (failed(env)) {
        return nullptr;
    }
    auto formatted = static_cast<jstring>(
        env->CallStaticObjectMethod(ids.player_team, ids.format_player_name, team, name));
    if (failed(env) || formatted == nullptr) {
        return nullptr;
    }
    std::string replacement;
    if (!streamer::mask_scoreboard_entry(to_utf8(env, formatted), &replacement)) {
        return nullptr;
    }
    jobject objective = env->CallObjectMethod(score, ids.objective);
    const jint points = failed(env) ? 0 : env->CallIntMethod(score, ids.points);
    const jboolean locked = failed(env) ? JNI_FALSE : env->CallBooleanMethod(score, ids.locked);
    if (failed(env)) {
        return nullptr;
    }
    jstring text = to_java(env, replacement);
    if (text == nullptr) {
        return nullptr;
    }
    jobject masked = env->NewObject(ids.score, ids.score_ctor, scoreboard, objective, text);
    if (failed(env) || masked == nullptr) {
        return nullptr;
    }
    env->CallVoidMethod(masked, ids.set_points, points);
    if (!failed(env)) {
        env->CallVoidMethod(masked, ids.set_locked, locked);
    }
    return failed(env) ? nullptr : masked;
}

// Masked copy of the sorted collection (re-sorted with Score.scoreComparator like Adjust, which masks before
// sorting), or nullptr when nothing changed / on failure.
jobject masked_scores(JNIEnv* env, const CommonIds& common, const ScoreIds& ids, jobject scoreboard,
                      jobject scores) {
    auto entries = static_cast<jobjectArray>(env->CallObjectMethod(scores, common.collection_to_array));
    if (failed(env) || entries == nullptr) {
        return nullptr;
    }
    const jsize count = env->GetArrayLength(entries);
    jobject list = env->NewObject(common.array_list, common.array_list_ctor, count);
    if (failed(env) || list == nullptr) {
        env->DeleteLocalRef(entries);
        return nullptr;
    }
    bool changed = false;
    bool ok = true;
    for (jsize i = 0; ok && i < count; ++i) {
        if (env->PushLocalFrame(24) != 0) {
            failed(env);
            ok = false;
            break;
        }
        jobject score = env->GetObjectArrayElement(entries, i);
        ok = !failed(env);
        if (ok) {
            jobject masked = score != nullptr ? masked_score(env, ids, scoreboard, score) : nullptr;
            changed = changed || masked != nullptr;
            env->CallBooleanMethod(list, common.list_add, masked != nullptr ? masked : score);
            ok = !failed(env);
        }
        env->PopLocalFrame(nullptr);
    }
    env->DeleteLocalRef(entries);
    if (ok && changed && ids.comparator != nullptr && common.collections_sort != nullptr) {
        jobject comparator = env->GetStaticObjectField(ids.score, ids.comparator);
        if (!failed(env) && comparator != nullptr) {
            env->CallStaticVoidMethod(common.collections, common.collections_sort, list, comparator);
            ok = !failed(env);
            env->DeleteLocalRef(comparator);
        }
    }
    if (!ok || !changed) {
        env->DeleteLocalRef(list);
        return nullptr;
    }
    return list;
}

jobject JNICALL native_sorted_scores(JNIEnv* env, jclass /*cls*/, jobject scoreboard, jobject objective) noexcept {
    events::JniContext jni = jni_for(env);
    const ScoreIds& ids = score_ids(&jni);
    if (ids.sorted_scores == nullptr) {
        return nullptr;
    }
    jobject scores = env->CallObjectMethod(scoreboard, ids.sorted_scores, objective);
    if (env->ExceptionCheck() == JNI_TRUE || scores == nullptr || !streamer::active()) {
        return scores;
    }
    const CommonIds& common = common_ids(&jni);
    if (ids.players_team == nullptr || ids.format_player_name == nullptr || ids.score_ctor == nullptr ||
        ids.player_name == nullptr || ids.objective == nullptr || ids.points == nullptr ||
        ids.set_points == nullptr || ids.locked == nullptr || ids.set_locked == nullptr ||
        common.collection_to_array == nullptr || common.array_list_ctor == nullptr || common.list_add == nullptr) {
        return scores;
    }
    jobject masked = masked_scores(env, common, ids, scoreboard, scores);
    if (masked == nullptr) {
        return scores;
    }
    env->DeleteLocalRef(scores);
    return masked;
}

// ---------------------------------------------------------------------------------------------------------------
// GuiPlayerTabOverlay.getPlayerName(NetworkPlayerInfo) - replaced in GuiPlayerTabOverlay.renderPlayerlist
// ---------------------------------------------------------------------------------------------------------------

jstring JNICALL native_tab_player_name(JNIEnv* env, jclass /*cls*/, jobject overlay, jobject info) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls_overlay = jni::find_class(&jni, "awh", "net/minecraft/client/gui/GuiPlayerTabOverlay");
    static const jmethodID mid_player_name = jni::method(
        &jni, cls_overlay, {"a", "getPlayerName", "func_175243_a"}, "(Lbdc;)Ljava/lang/String;",
        "(Lnet/minecraft/client/network/NetworkPlayerInfo;)Ljava/lang/String;");
    static const jclass cls_info = jni::find_class(&jni, "bdc", "net/minecraft/client/network/NetworkPlayerInfo");
    static const jmethodID mid_info_profile = jni::method(
        &jni, cls_info, {"a", "getGameProfile", "func_178845_a"}, "()Lcom/mojang/authlib/GameProfile;",
        "()Lcom/mojang/authlib/GameProfile;");
    if (mid_player_name == nullptr) {
        return nullptr;
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(overlay, mid_player_name, info));
    if (env->ExceptionCheck() == JNI_TRUE || name == nullptr || info == nullptr || mid_info_profile == nullptr ||
        !streamer::active()) {
        return name;
    }
    const CommonIds& common = common_ids(&jni);
    if (common.profile_name == nullptr) {
        return name;
    }
    jobject profile = env->CallObjectMethod(info, mid_info_profile);
    if (failed(env) || profile == nullptr) {
        return name;
    }
    auto profile_name = static_cast<jstring>(env->CallObjectMethod(profile, common.profile_name));
    env->DeleteLocalRef(profile);
    if (failed(env) || profile_name == nullptr) {
        return name;
    }
    const std::string real = to_utf8(env, profile_name);
    env->DeleteLocalRef(profile_name);
    if (real.empty()) {
        return name;
    }
    const std::string text = to_utf8(env, name);
    const std::string masked = streamer::replace_name(text, real);
    if (masked == text) {
        return name;
    }
    jstring replaced = to_java(env, masked);
    if (replaced == nullptr) {
        return name;
    }
    env->DeleteLocalRef(name);
    return replaced;
}

// ---------------------------------------------------------------------------------------------------------------
// Render.renderLivingLabel(Entity, String, double, double, double, int) - replaced in Render.renderName,
// Render.renderOffsetLivingLabel and RenderPlayer.renderOffsetLivingLabel
// ---------------------------------------------------------------------------------------------------------------

jmethodID render_living_label_mid(events::JniContext* jni) noexcept {
    static const jclass cls = jni::find_class(jni, "biv", "net/minecraft/client/renderer/entity/Render");
    static const jmethodID mid = jni::method(jni, cls, {"a", "renderLivingLabel", "func_147906_a"},
                                             "(Lpk;Ljava/lang/String;DDDI)V",
                                             "(Lnet/minecraft/entity/Entity;Ljava/lang/String;DDDI)V");
    return mid;
}

void JNICALL native_render_living_label(JNIEnv* env, jclass /*cls*/, jobject render, jobject entity, jstring str,
                                        jdouble x, jdouble y, jdouble z, jint max_distance) noexcept {
    events::JniContext jni = jni_for(env);
    const jmethodID mid = render_living_label_mid(&jni);
    if (mid == nullptr) {
        return;
    }
    jstring shown = nullptr;
    if (entity != nullptr && str != nullptr && streamer::active()) {
        // Adjust: players with a GameProfile only.
        shown = mask_label(env, label_context(env, common_ids(&jni), entity, false), str);
    }
    env->CallVoidMethod(render, mid, entity, shown != nullptr ? shown : str, x, y, z, max_distance);
}

// ---------------------------------------------------------------------------------------------------------------
// RendererLivingEntity.renderName(EntityLivingBase, double, double, double)
// ---------------------------------------------------------------------------------------------------------------

// Entity of the renderName call in progress (recorded by its getDistanceSqToEntity call), used by the sneaking
// branch, whose FontRenderer calls do not see the entity. Entity rendering runs on the client thread only (no
// thread_local: TLS destructors would outlive an unloaded DLL).
LabelContext g_name_tag{};

jdouble JNICALL native_name_tag_distance(JNIEnv* env, jclass /*cls*/, jobject entity, jobject viewer) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls_entity = jni::find_class(&jni, "pk", "net/minecraft/entity/Entity");
    static const jmethodID mid = jni::method(&jni, cls_entity, {"h", "getDistanceSqToEntity", "func_70068_e"},
                                             "(Lpk;)D", "(Lnet/minecraft/entity/Entity;)D");
    g_name_tag = LabelContext{};
    if (entity != nullptr && streamer::active()) {
        g_name_tag = label_context(env, common_ids(&jni), entity, true);
    }
    if (mid == nullptr) {
        return 0.0;
    }
    return env->CallDoubleMethod(entity, mid, viewer);
}

jmethodID string_width_mid(events::JniContext* jni) noexcept {
    static const jclass cls = jni::find_class(jni, "avn", "net/minecraft/client/gui/FontRenderer");
    static const jmethodID mid = jni::method(jni, cls, {"a", "getStringWidth", "func_78256_a"},
                                             "(Ljava/lang/String;)I", "(Ljava/lang/String;)I");
    return mid;
}

jint JNICALL native_name_tag_width(JNIEnv* env, jclass /*cls*/, jobject font, jstring text) noexcept {
    events::JniContext jni = jni_for(env);
    const jmethodID mid = string_width_mid(&jni);
    if (mid == nullptr) {
        return 0;
    }
    jstring shown = streamer::active() ? mask_label(env, g_name_tag, text) : nullptr;
    const jint width = env->CallIntMethod(font, mid, shown != nullptr ? shown : text);
    if (shown != nullptr && env->ExceptionCheck() == JNI_FALSE) {
        env->DeleteLocalRef(shown);
    }
    return width;
}

jint JNICALL native_name_tag_draw(JNIEnv* env, jclass /*cls*/, jobject font, jstring text, jint x, jint y,
                                  jint color) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "avn", "net/minecraft/client/gui/FontRenderer");
    static const jmethodID mid = jni::method(&jni, cls, {"a", "drawString", "func_78276_b"},
                                             "(Ljava/lang/String;III)I", "(Ljava/lang/String;III)I");
    if (mid == nullptr) {
        return 0;
    }
    jstring shown = streamer::active() ? mask_label(env, g_name_tag, text) : nullptr;
    const jint result = env->CallIntMethod(font, mid, shown != nullptr ? shown : text, x, y, color);
    if (shown != nullptr && env->ExceptionCheck() == JNI_FALSE) {
        env->DeleteLocalRef(shown);
    }
    return result;
}

void JNICALL native_name_tag_offset_label(JNIEnv* env, jclass /*cls*/, jobject renderer, jobject entity,
                                          jdouble x, jdouble y, jdouble z, jstring text, jfloat scale,
                                          jdouble distance_sq) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls = jni::find_class(&jni, "biv", "net/minecraft/client/renderer/entity/Render");
    static const jmethodID mid = jni::method(&jni, cls, {"a", "renderOffsetLivingLabel", "func_177069_a"},
                                             "(Lpk;DDDLjava/lang/String;FD)V",
                                             "(Lnet/minecraft/entity/Entity;DDDLjava/lang/String;FD)V");
    if (mid == nullptr) {
        return;
    }
    jstring shown = nullptr;
    if (entity != nullptr && text != nullptr && streamer::active()) {
        shown = mask_label(env, label_context(env, common_ids(&jni), entity, true), text);
    }
    env->CallVoidMethod(renderer, mid, entity, x, y, z, shown != nullptr ? shown : text, scale, distance_sq);
}

// ---------------------------------------------------------------------------------------------------------------
// GuiUtilRenderComponents.splitText(IChatComponent, int, FontRenderer, boolean, boolean) (static) - replaced in
// TileEntitySignRenderer.renderTileEntityAt
// ---------------------------------------------------------------------------------------------------------------

// replaceKnownNames results of sign lines, dropped whenever the name map changes (signs redraw every frame).
// Tile entities render on the client thread only.
std::unordered_map<std::string, std::string> g_sign_cache;
uint64_t g_sign_generation = 0;

std::string cached_known_names(const std::string& text) {
    const uint64_t generation = streamer::names_generation();
    if (generation != g_sign_generation || g_sign_cache.size() > 1024) {
        g_sign_cache.clear();
        g_sign_generation = generation;
    }
    auto it = g_sign_cache.find(text);
    if (it != g_sign_cache.end()) {
        return it->second;
    }
    std::string masked = streamer::replace_known_names(text);
    g_sign_cache.emplace(text, masked);
    return masked;
}

jobject JNICALL native_sign_split_text(JNIEnv* env, jclass /*cls*/, jobject component, jint width, jobject font,
                                       jboolean keep_newlines, jboolean force_codes) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls_util = jni::find_class(&jni, "avu", "net/minecraft/client/gui/GuiUtilRenderComponents");
    static const jmethodID mid_split = jni::static_method(
        &jni, cls_util, {"a", "splitText", "func_178908_a"}, "(Leu;ILavn;ZZ)Ljava/util/List;",
        "(Lnet/minecraft/util/IChatComponent;ILnet/minecraft/client/gui/FontRenderer;ZZ)Ljava/util/List;");
    static const jclass cls_component = jni::find_class(&jni, "eu", "net/minecraft/util/IChatComponent");
    static const jmethodID mid_formatted = jni::method(&jni, cls_component, {"d", "getFormattedText", "func_150254_d"},
                                                       "()Ljava/lang/String;", "()Ljava/lang/String;");
    static const jclass cls_text = jni::find_class(&jni, "fa", "net/minecraft/util/ChatComponentText");
    static const jmethodID mid_text_ctor =
        jni::method(&jni, cls_text, {"<init>", "<init>", "<init>"}, "(Ljava/lang/String;)V", "(Ljava/lang/String;)V");
    if (mid_split == nullptr) {
        return nullptr;
    }
    jobject shown = nullptr;
    if (component != nullptr && mid_formatted != nullptr && mid_text_ctor != nullptr && streamer::active()) {
        // ichatcomponent = new ChatComponentText(StreamerModule.replaceKnownNames(ichatcomponent.getFormattedText()))
        auto formatted = static_cast<jstring>(env->CallObjectMethod(component, mid_formatted));
        if (!failed(env) && formatted != nullptr) {
            const std::string text = to_utf8(env, formatted);
            env->DeleteLocalRef(formatted);
            const std::string masked = cached_known_names(text);
            if (masked != text) { // unchanged lines render identically, keep the original component
                jstring value = to_java(env, masked);
                if (value != nullptr) {
                    shown = env->NewObject(cls_text, mid_text_ctor, value);
                    if (failed(env)) {
                        shown = nullptr;
                    }
                    env->DeleteLocalRef(value);
                }
            }
        }
    }
    jobject lines = env->CallStaticObjectMethod(cls_util, mid_split, shown != nullptr ? shown : component, width,
                                                font, keep_newlines, force_codes);
    if (shown != nullptr && env->ExceptionCheck() == JNI_FALSE) {
        env->DeleteLocalRef(shown);
    }
    return lines;
}

// ---------------------------------------------------------------------------------------------------------------
// GuiScreen.sendChatMessage(String) - replaced in GuiChat.keyTyped
// ---------------------------------------------------------------------------------------------------------------

void JNICALL native_send_chat_message(JNIEnv* env, jclass /*cls*/, jobject gui, jstring message) noexcept {
    events::JniContext jni = jni_for(env);
    static const jclass cls_screen = jni::find_class(&jni, "axu", "net/minecraft/client/gui/GuiScreen");
    static const jmethodID mid_send = jni::method(&jni, cls_screen, {"f", "sendChatMessage", "func_175275_f"},
                                                  "(Ljava/lang/String;)V", "(Ljava/lang/String;)V");
    if (mid_send == nullptr) {
        return;
    }
    jstring shown = nullptr;
    if (message != nullptr && streamer::active()) {
        const std::string text = to_utf8(env, message);
        const std::string restored = streamer::restore_typed_names(text);
        if (restored != text) {
            shown = to_java(env, restored);
        }
    }
    env->CallVoidMethod(gui, mid_send, shown != nullptr ? shown : message);
}

// ---------------------------------------------------------------------------------------------------------------
// Hook table
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* kGuiNewChat = "net/minecraft/client/gui/GuiNewChat";
constexpr const char* kGuiIngame = "net/minecraft/client/gui/GuiIngame";
constexpr const char* kScoreboard = "net/minecraft/scoreboard/Scoreboard";
constexpr const char* kTabOverlay = "net/minecraft/client/gui/GuiPlayerTabOverlay";
constexpr const char* kRender = "net/minecraft/client/renderer/entity/Render";
constexpr const char* kRenderPlayer = "net/minecraft/client/renderer/entity/RenderPlayer";
constexpr const char* kRendererLiving = "net/minecraft/client/renderer/entity/RendererLivingEntity";
constexpr const char* kEntityLivingBase = "net/minecraft/entity/EntityLivingBase";
constexpr const char* kFontRenderer = "net/minecraft/client/gui/FontRenderer";
constexpr const char* kSignRenderer = "net/minecraft/client/renderer/tileentity/TileEntitySignRenderer";
constexpr const char* kRenderComponents = "net/minecraft/client/gui/GuiUtilRenderComponents";
constexpr const char* kGuiChat = "net/minecraft/client/gui/GuiChat";
constexpr const char* kGuiScreen = "net/minecraft/client/gui/GuiScreen";

// --- chat ---

constexpr bytecode::ExtraMemberNames kSetChatLine{
    "a", "(Leu;IIZ)V",
    "setChatLine", "(Lnet/minecraft/util/IChatComponent;IIZ)V",
    "func_146237_a", "(Lnet/minecraft/util/IChatComponent;IIZ)V"};
constexpr const char* kSetChatLineNative = "adjustStreamerSetChatLine";
constexpr const char* kSetChatLineDesc = "(Ljava/lang/Object;Ljava/lang/Object;IIZ)V";

// GuiNewChat.printChatMessageWithOptionalDeletion(IChatComponent, int) calls this.setChatLine(c, id, counter, false).
const ExtraHookSpec kHookChatPrint{
    "avt", kGuiNewChat,
    {"a", "(Leu;I)V",
     "printChatMessageWithOptionalDeletion", "(Lnet/minecraft/util/IChatComponent;I)V",
     "func_146234_a", "(Lnet/minecraft/util/IChatComponent;I)V"},
    "avt", kGuiNewChat,
    kSetChatLine,
    HookInsnKind::InvokeInstance,
    kSetChatLineNative, kSetChatLineDesc,
    reinterpret_cast<void*>(&native_set_chat_line),
};

// GuiNewChat.refreshChat() re-adds every stored line with this.setChatLine(c, id, counter, true).
const ExtraHookSpec kHookChatRefresh{
    "avt", kGuiNewChat,
    {"b", "()V", "refreshChat", "()V", "func_146245_b", "()V"},
    "avt", kGuiNewChat,
    kSetChatLine,
    HookInsnKind::InvokeInstance,
    kSetChatLineNative, kSetChatLineDesc,
    reinterpret_cast<void*>(&native_set_chat_line),
};

// --- scoreboard ---

// GuiIngame.renderScoreboard(ScoreObjective, ScaledResolution) calls scoreboard.getSortedScores(objective).
const ExtraHookSpec kHookSortedScores{
    "avo", kGuiIngame,
    {"a", "(Lauk;Lavr;)V",
     "renderScoreboard", "(Lnet/minecraft/scoreboard/ScoreObjective;Lnet/minecraft/client/gui/ScaledResolution;)V",
     "func_180475_a", "(Lnet/minecraft/scoreboard/ScoreObjective;Lnet/minecraft/client/gui/ScaledResolution;)V"},
    "auo", kScoreboard,
    {"i", "(Lauk;)Ljava/util/Collection;",
     "getSortedScores", "(Lnet/minecraft/scoreboard/ScoreObjective;)Ljava/util/Collection;",
     "func_96534_i", "(Lnet/minecraft/scoreboard/ScoreObjective;)Ljava/util/Collection;"},
    HookInsnKind::InvokeInstance,
    "adjustStreamerSortedScores", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/util/Collection;",
    reinterpret_cast<void*>(&native_sorted_scores),
};

// --- tab list ---

// GuiPlayerTabOverlay.renderPlayerlist(int, Scoreboard, ScoreObjective) calls this.getPlayerName(info) (twice).
const ExtraHookSpec kHookTabPlayerName{
    "awh", kTabOverlay,
    {"a", "(ILauo;Lauk;)V",
     "renderPlayerlist", "(ILnet/minecraft/scoreboard/Scoreboard;Lnet/minecraft/scoreboard/ScoreObjective;)V",
     "func_175249_a", "(ILnet/minecraft/scoreboard/Scoreboard;Lnet/minecraft/scoreboard/ScoreObjective;)V"},
    "awh", kTabOverlay,
    {"a", "(Lbdc;)Ljava/lang/String;",
     "getPlayerName", "(Lnet/minecraft/client/network/NetworkPlayerInfo;)Ljava/lang/String;",
     "func_175243_a", "(Lnet/minecraft/client/network/NetworkPlayerInfo;)Ljava/lang/String;"},
    HookInsnKind::InvokeInstance,
    "adjustStreamerTabPlayerName", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/String;",
    reinterpret_cast<void*>(&native_tab_player_name),
};

// --- Render.renderLivingLabel ---

constexpr bytecode::ExtraMemberNames kRenderLivingLabel{
    "a", "(Lpk;Ljava/lang/String;DDDI)V",
    "renderLivingLabel", "(Lnet/minecraft/entity/Entity;Ljava/lang/String;DDDI)V",
    "func_147906_a", "(Lnet/minecraft/entity/Entity;Ljava/lang/String;DDDI)V"};
constexpr const char* kRenderLivingLabelNative = "adjustStreamerRenderLivingLabel";
constexpr const char* kRenderLivingLabelDesc = "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;DDDI)V";

// Render.renderName(Entity, double, double, double) calls this.renderLivingLabel(...) (non-living entities).
const ExtraHookSpec kHookLabelRenderName{
    "biv", kRender,
    {"a", "(Lpk;DDD)V",
     "renderName", "(Lnet/minecraft/entity/Entity;DDD)V",
     "func_177067_a", "(Lnet/minecraft/entity/Entity;DDD)V"},
    "biv", kRender,
    kRenderLivingLabel,
    HookInsnKind::InvokeInstance,
    kRenderLivingLabelNative, kRenderLivingLabelDesc,
    reinterpret_cast<void*>(&native_render_living_label),
};

// Render.renderOffsetLivingLabel(Entity, double, double, double, String, float, double) calls
// this.renderLivingLabel(...) (living-entity name tags that are not sneaking).
const ExtraHookSpec kHookLabelOffset{
    "biv", kRender,
    {"a", "(Lpk;DDDLjava/lang/String;FD)V",
     "renderOffsetLivingLabel", "(Lnet/minecraft/entity/Entity;DDDLjava/lang/String;FD)V",
     "func_177069_a", "(Lnet/minecraft/entity/Entity;DDDLjava/lang/String;FD)V"},
    "biv", kRender,
    kRenderLivingLabel,
    HookInsnKind::InvokeInstance,
    kRenderLivingLabelNative, kRenderLivingLabelDesc,
    reinterpret_cast<void*>(&native_render_living_label),
};

// RenderPlayer.renderOffsetLivingLabel(AbstractClientPlayer, ...) calls this.renderLivingLabel(...) for the
// below-name scoreboard line (the CP owner of that call is RenderPlayer).
const ExtraHookSpec kHookLabelPlayerScore{
    "bln", kRenderPlayer,
    {"a", "(Lbet;DDDLjava/lang/String;FD)V",
     "renderOffsetLivingLabel", "(Lnet/minecraft/client/entity/AbstractClientPlayer;DDDLjava/lang/String;FD)V",
     "func_177069_a", "(Lnet/minecraft/client/entity/AbstractClientPlayer;DDDLjava/lang/String;FD)V"},
    "bln", kRenderPlayer,
    kRenderLivingLabel,
    HookInsnKind::InvokeInstance,
    kRenderLivingLabelNative, kRenderLivingLabelDesc,
    reinterpret_cast<void*>(&native_render_living_label),
};

// --- RendererLivingEntity.renderName(EntityLivingBase, double, double, double) ---

constexpr bytecode::ExtraMemberNames kLivingRenderName{
    "b", "(Lpr;DDD)V",
    "renderName", "(Lnet/minecraft/entity/EntityLivingBase;DDD)V",
    "func_177067_a", "(Lnet/minecraft/entity/EntityLivingBase;DDD)V"};

// entity.getDistanceSqToEntity(renderManager.livingPlayer) - records the entity (CP owner EntityLivingBase).
const ExtraHookSpec kHookNameTagDistance{
    "bjl", kRendererLiving,
    kLivingRenderName,
    "pr", kEntityLivingBase,
    {"h", "(Lpk;)D",
     "getDistanceSqToEntity", "(Lnet/minecraft/entity/Entity;)D",
     "func_70068_e", "(Lnet/minecraft/entity/Entity;)D"},
    HookInsnKind::InvokeInstance,
    "adjustStreamerNameTagDistance", "(Ljava/lang/Object;Ljava/lang/Object;)D",
    reinterpret_cast<void*>(&native_name_tag_distance),
};

// Sneaking branch: fontrenderer.getStringWidth(s) (twice).
const ExtraHookSpec kHookNameTagWidth{
    "bjl", kRendererLiving,
    kLivingRenderName,
    "avn", kFontRenderer,
    {"a", "(Ljava/lang/String;)I",
     "getStringWidth", "(Ljava/lang/String;)I",
     "func_78256_a", "(Ljava/lang/String;)I"},
    HookInsnKind::InvokeInstance,
    "adjustStreamerNameTagWidth", "(Ljava/lang/Object;Ljava/lang/Object;)I",
    reinterpret_cast<void*>(&native_name_tag_width),
};

// Sneaking branch: fontrenderer.drawString(s, x, 0, 553648127).
const ExtraHookSpec kHookNameTagDraw{
    "bjl", kRendererLiving,
    kLivingRenderName,
    "avn", kFontRenderer,
    {"a", "(Ljava/lang/String;III)I",
     "drawString", "(Ljava/lang/String;III)I",
     "func_78276_b", "(Ljava/lang/String;III)I"},
    HookInsnKind::InvokeInstance,
    "adjustStreamerNameTagDraw", "(Ljava/lang/Object;Ljava/lang/Object;III)I",
    reinterpret_cast<void*>(&native_name_tag_draw),
};

// Non-sneaking branch: this.renderOffsetLivingLabel(entity, x, y, z, s, 0.02666667F, d0) (CP owner
// RendererLivingEntity).
const ExtraHookSpec kHookNameTagOffset{
    "bjl", kRendererLiving,
    kLivingRenderName,
    "bjl", kRendererLiving,
    {"a", "(Lpk;DDDLjava/lang/String;FD)V",
     "renderOffsetLivingLabel", "(Lnet/minecraft/entity/Entity;DDDLjava/lang/String;FD)V",
     "func_177069_a", "(Lnet/minecraft/entity/Entity;DDDLjava/lang/String;FD)V"},
    HookInsnKind::InvokeInstance,
    "adjustStreamerNameTagOffsetLabel", "(Ljava/lang/Object;Ljava/lang/Object;DDDLjava/lang/Object;FD)V",
    reinterpret_cast<void*>(&native_name_tag_offset_label),
};

// --- signs ---

// TileEntitySignRenderer.renderTileEntityAt(TileEntitySign, ...) calls GuiUtilRenderComponents.splitText(line, 90,
// fontrenderer, false, true) for each line.
const ExtraHookSpec kHookSignSplitText{
    "bhj", kSignRenderer,
    {"a", "(Laln;DDDFI)V",
     "renderTileEntityAt", "(Lnet/minecraft/tileentity/TileEntitySign;DDDFI)V",
     "func_180535_a", "(Lnet/minecraft/tileentity/TileEntitySign;DDDFI)V"},
    "avu", kRenderComponents,
    {"a", "(Leu;ILavn;ZZ)Ljava/util/List;",
     "splitText", "(Lnet/minecraft/util/IChatComponent;ILnet/minecraft/client/gui/FontRenderer;ZZ)Ljava/util/List;",
     "func_178908_a", "(Lnet/minecraft/util/IChatComponent;ILnet/minecraft/client/gui/FontRenderer;ZZ)Ljava/util/List;"},
    HookInsnKind::InvokeStatic,
    "adjustStreamerSignSplitText", "(Ljava/lang/Object;ILjava/lang/Object;ZZ)Ljava/util/List;",
    reinterpret_cast<void*>(&native_sign_split_text),
};

// --- chat sending ---

// GuiChat.keyTyped(char, int), Enter: this.sendChatMessage(s) (CP owner GuiChat, inherited from GuiScreen).
const ExtraHookSpec kHookSendChat{
    "awv", kGuiChat,
    {"a", "(CI)V", "keyTyped", "(CI)V", "func_73869_a", "(CI)V"},
    "awv", kGuiChat,
    {"f", "(Ljava/lang/String;)V",
     "sendChatMessage", "(Ljava/lang/String;)V",
     "func_175275_f", "(Ljava/lang/String;)V"},
    HookInsnKind::InvokeInstance,
    "adjustStreamerSendChatMessage", "(Ljava/lang/Object;Ljava/lang/Object;)V",
    reinterpret_cast<void*>(&native_send_chat_message),
};

} // namespace

const bytecode::ExtraHookSpec* const* streamer_hooks() noexcept {
    static const ExtraHookSpec* const s_hooks[] = {
        &kHookChatPrint,
        &kHookChatRefresh,
        &kHookSortedScores,
        &kHookTabPlayerName,
        &kHookLabelRenderName,
        &kHookLabelOffset,
        &kHookLabelPlayerScore,
        &kHookNameTagDistance,
        &kHookNameTagWidth,
        &kHookNameTagDraw,
        &kHookNameTagOffset,
        &kHookSignSplitText,
        &kHookSendChat,
        nullptr,
    };
    return s_hooks;
}

} // namespace slinky::adjust::hooks
