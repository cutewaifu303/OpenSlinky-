// Port of com.adjustclient.ui.hud.potion.{PotionEffects, PotionEntry}: Adjust's bottom-right potion effect list.
//
// One row per active effect, 6.5 apart, sorted by text width (narrowest on top): "<name> [<roman level>]" in
// Tahoma 12 coloured with the potion's liquid colour (alpha 150 for beneficial, 220 for harmful effects), followed by
// the remaining duration "m:ss" in Tahoma 10 that fades from white to red (204, 85, 85) once fewer than 260 ticks are
// left. The list is built like Adjust's NvgRenderer draw list: its bounds are anchored to the bottom-right corner
// (Anchor.BOTTOM_RIGHT) and it moves up while the chat is open (EASE_OUT_CUBIC, 500 ms).
//
// Adjust's PotionEffects draws text only (no potion icon textures), so there is nothing else to port. The element
// has no settings in Adjust (always enabled while the HUD runs). Minecraft 1.8.9 only (members resolved with
// adjust/jni_lookup); on 1.7.10 the list stays empty.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"

namespace slinky::modules::adjust_hud {

namespace {

using adjust::Animation;
using adjust::Argb;
using adjust::Easing;

// ---- JNI members (Minecraft 1.8.9) ------------------------------------------------------------------------------

struct PotionJni {
    bool resolved = false;
    bool usable = false;
    jmethodID mid_active_effects = nullptr;   // EntityLivingBase.getActivePotionEffects()Collection
    jclass cls_collection = nullptr;          // java.util.Collection
    jmethodID mid_to_array = nullptr;         // Collection.toArray()Object[]
    jmethodID mid_effect_id = nullptr;        // PotionEffect.getPotionID()I
    jmethodID mid_effect_duration = nullptr;  // PotionEffect.getDuration()I
    jmethodID mid_effect_amplifier = nullptr; // PotionEffect.getAmplifier()I
    jmethodID mid_effect_maxed = nullptr;     // PotionEffect.getIsPotionDurationMax()Z
    jmethodID mid_potion_name = nullptr;      // Potion.getName()String
    jmethodID mid_potion_bad = nullptr;       // Potion.isBadEffect()Z
    jmethodID mid_potion_liquid = nullptr;    // Potion.getLiquidColor()I
    jclass cls_i18n = nullptr;                // I18n
    jmethodID smid_i18n_format = nullptr;     // I18n.format(String, Object...)String
    jclass cls_gui_chat = nullptr;            // GuiChat
};

PotionJni g_jni{};

void resolve_jni(events::JniContext* jni) noexcept {
    if (g_jni.resolved || jni == nullptr || jni->env == nullptr || jni->mappings == nullptr) {
        return;
    }
    g_jni.resolved = true;
    if (!adjust::jni::is_1_8(jni)) {
        return;
    }
    namespace aj = adjust::jni;
    const mc::Mappings* m = jni->mappings;
    JNIEnv* env = jni->env;

    g_jni.mid_active_effects = aj::method(jni, m->cls_entity_living_base,
                                          {"bl", "getActivePotionEffects", "func_70651_bq"},
                                          "()Ljava/util/Collection;", "()Ljava/util/Collection;");
    if (jclass local = env->FindClass("java/util/Collection"); local != nullptr) {
        g_jni.cls_collection = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        g_jni.mid_to_array = env->GetMethodID(g_jni.cls_collection, "toArray", "()[Ljava/lang/Object;");
    }
    aj::clear_exception(jni);

    g_jni.mid_effect_id = aj::method(jni, m->cls_potion_effect, {"a", "getPotionID", "func_76456_a"}, "()I", "()I");
    g_jni.mid_effect_duration = aj::method(jni, m->cls_potion_effect, {"b", "getDuration", "func_76459_b"}, "()I", "()I");
    g_jni.mid_effect_amplifier = aj::method(jni, m->cls_potion_effect, {"c", "getAmplifier", "func_76458_c"}, "()I", "()I");
    g_jni.mid_effect_maxed = aj::method(jni, m->cls_potion_effect, {"h", "getIsPotionDurationMax", "func_100011_g"},
                                        "()Z", "()Z");
    g_jni.mid_potion_name = aj::method(jni, m->cls_potion, {"a", "getName", "func_76393_a"}, "()Ljava/lang/String;",
                                       "()Ljava/lang/String;");
    g_jni.mid_potion_bad = aj::method(jni, m->cls_potion, {"g", "isBadEffect", "func_76398_f"}, "()Z", "()Z");
    g_jni.mid_potion_liquid = aj::method(jni, m->cls_potion, {"k", "getLiquidColor", "func_76401_j"}, "()I", "()I");

    g_jni.cls_i18n = aj::find_class(jni, "bnq", "net/minecraft/client/resources/I18n");
    g_jni.smid_i18n_format = aj::static_method(jni, g_jni.cls_i18n, {"a", "format", "func_135052_a"},
                                               "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",
                                               "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;");
    g_jni.cls_gui_chat = aj::find_class(jni, "awv", "net/minecraft/client/gui/GuiChat");

    g_jni.usable = g_jni.mid_active_effects != nullptr && g_jni.mid_to_array != nullptr &&
                   g_jni.mid_effect_id != nullptr && g_jni.mid_effect_duration != nullptr &&
                   g_jni.mid_effect_amplifier != nullptr && g_jni.mid_effect_maxed != nullptr &&
                   g_jni.mid_potion_name != nullptr && g_jni.mid_potion_bad != nullptr &&
                   g_jni.mid_potion_liquid != nullptr && m->cls_potion != nullptr && m->sfid_potion_types != nullptr;
}

std::string jstring_to_utf8(JNIEnv* env, jstring str) {
    std::string out;
    if (str == nullptr) {
        return out;
    }
    const char* chars = env->GetStringUTFChars(str, nullptr);
    if (chars != nullptr) {
        out = chars;
        env->ReleaseStringUTFChars(str, chars);
    }
    return out;
}

// I18n.format(key) (falls back to StatCollector.translateToLocal, then to the raw key). `key` stays owned by caller.
std::string translate(events::JniContext* jni, jstring key) {
    JNIEnv* env = jni->env;
    jstring result = nullptr;
    if (g_jni.cls_i18n != nullptr && g_jni.smid_i18n_format != nullptr && jni->mappings->cls_java_lang_object != nullptr) {
        jobjectArray no_args = env->NewObjectArray(0, jni->mappings->cls_java_lang_object, nullptr);
        if (no_args != nullptr) {
            result = static_cast<jstring>(env->CallStaticObjectMethod(g_jni.cls_i18n, g_jni.smid_i18n_format, key, no_args));
            env->DeleteLocalRef(no_args);
        }
        if (adjust::jni::clear_exception(jni)) {
            result = nullptr;
        }
    }
    if (result == nullptr && jni->mappings->cls_stat_collector != nullptr &&
        jni->mappings->smid_stat_collector_translate_to_local != nullptr) {
        result = mc::stat_collector_translate_to_local(jni, key);
        if (adjust::jni::clear_exception(jni)) {
            result = nullptr;
        }
    }
    std::string text = jstring_to_utf8(env, result != nullptr ? result : key);
    if (result != nullptr) {
        env->DeleteLocalRef(result);
    }
    return text;
}

// StringUtil.toRoman
std::string to_roman(int number) {
    static constexpr int kValues[13] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static constexpr const char* kSymbols[13] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
    std::string roman;
    for (int i = 0; i < 13; ++i) {
        while (number >= kValues[i]) {
            roman += kSymbols[i];
            number -= kValues[i];
        }
    }
    return roman;
}

// StringUtils.ticksToElapsedTime
std::string ticks_to_elapsed_time(int ticks) {
    int seconds = ticks / 20;
    const int minutes = seconds / 60;
    seconds %= 60;
    return std::to_string(minutes) + (seconds < 10 ? ":0" : ":") + std::to_string(seconds);
}

// ---- state (PotionEffects / PotionEntry) ---------------------------------------------------------------------

struct Entry {
    std::string name;
    std::string duration_text; // empty = null (infinite / maxed duration)
    std::string level_text;    // empty = null (level I)
    bool has_duration = false;
    bool has_level = false;
    Animation fade{};
    Argb color = 0;            // duration text colour
    Argb liquid_color = 0;     // Potion.getLiquidColor()
    int32_t potion_id = 0;
    bool beneficial = false;
};

struct State {
    std::vector<Entry> entries;
    Animation shift{};
    bool initialized = false;
};

State g_state{};

void ensure_initialized() noexcept {
    if (!g_state.initialized) {
        g_state.initialized = true;
        g_state.shift = Animation(Easing::EaseOutCubic, 500);
        g_state.shift.value = -2.0f;
    }
}

// PotionEntry.update
void update_entry(Entry& entry, int amplifier, int duration, bool maxed) {
    if (duration >= 0x7FFF || maxed) {
        entry.has_duration = false;
        entry.duration_text.clear();
    } else {
        entry.has_duration = true;
        entry.duration_text = ticks_to_elapsed_time(duration);
    }
    if (amplifier >= 1) {
        entry.has_level = true;
        entry.level_text = to_roman(amplifier + 1);
    } else {
        entry.has_level = false;
        entry.level_text.clear();
    }

    // Blend between red (204, 85, 85) and white, alpha 150 (beneficial) / 220 (harmful).
    const float target = duration >= 260 ? 1.0f : 0.0f;
    entry.fade.update(target);
    const int alpha = entry.beneficial ? 150 : 220;
    float t = entry.fade.value;
    if (t > 1.0f) {
        t = 1.0f;
    }
    if (t < 0.0f) {
        entry.color = adjust::argb(204, 85, 85, alpha);
    } else {
        const float u = 1.0f - t;
        const auto r = static_cast<int>(u * 204.0f + 255.0f * t);
        const auto g = static_cast<int>(u * 85.0f + 255.0f * t);
        const auto b = static_cast<int>(u * 85.0f + 255.0f * t);
        const auto a = static_cast<int>(static_cast<float>(alpha) * u + static_cast<float>(alpha) * t);
        entry.color = adjust::argb(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255),
                                   std::clamp(a, 0, 255));
    }
}

// Creates a new entry for `potion_id` (name, beneficial flag, liquid colour); false if the potion is unknown.
bool create_entry(events::JniContext* jni, int32_t potion_id, Entry* out) {
    JNIEnv* env = jni->env;
    if (potion_id < 0) {
        return false;
    }
    jobject potion = mc::potion_get_by_id(jni, static_cast<uint32_t>(potion_id));
    if (adjust::jni::clear_exception(jni) || potion == nullptr) {
        return false;
    }
    auto key = static_cast<jstring>(env->CallObjectMethod(potion, g_jni.mid_potion_name));
    const bool bad = env->CallBooleanMethod(potion, g_jni.mid_potion_bad) != JNI_FALSE;
    const jint liquid = env->CallIntMethod(potion, g_jni.mid_potion_liquid);
    if (adjust::jni::clear_exception(jni)) {
        if (key != nullptr) {
            env->DeleteLocalRef(key);
        }
        env->DeleteLocalRef(potion);
        return false;
    }
    out->name = key != nullptr ? translate(jni, key) : std::string();
    out->potion_id = potion_id;
    out->beneficial = !bad;
    out->liquid_color = static_cast<Argb>(liquid);
    out->fade = Animation(Easing::EaseOutSine, 350); // started now
    out->fade.value = 1.0f;
    if (key != nullptr) {
        env->DeleteLocalRef(key);
    }
    env->DeleteLocalRef(potion);
    return true;
}

struct EffectInfo {
    int32_t id = 0;
    int32_t amplifier = 0;
    int32_t duration = 0;
    bool maxed = false;
};

// thePlayer.getActivePotionEffects(); false on failure.
bool read_active_effects(events::GameContext* ctx, std::vector<EffectInfo>* out) {
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(64) != 0) {
        adjust::jni::clear_exception(jni);
        return false;
    }
    bool ok = false;
    jobject collection = env->CallObjectMethod(ctx->local_player, g_jni.mid_active_effects);
    if (!adjust::jni::clear_exception(jni) && collection != nullptr) {
        auto array = static_cast<jobjectArray>(env->CallObjectMethod(collection, g_jni.mid_to_array));
        if (!adjust::jni::clear_exception(jni) && array != nullptr) {
            ok = true;
            const jsize count = env->GetArrayLength(array);
            for (jsize i = 0; i < count; ++i) {
                jobject effect = env->GetObjectArrayElement(array, i);
                if (effect == nullptr) {
                    continue;
                }
                EffectInfo info{};
                info.id = env->CallIntMethod(effect, g_jni.mid_effect_id);
                info.amplifier = env->CallIntMethod(effect, g_jni.mid_effect_amplifier);
                info.duration = env->CallIntMethod(effect, g_jni.mid_effect_duration);
                info.maxed = env->CallBooleanMethod(effect, g_jni.mid_effect_maxed) != JNI_FALSE;
                env->DeleteLocalRef(effect);
                if (adjust::jni::clear_exception(jni)) {
                    ok = false;
                    break;
                }
                out->push_back(info);
            }
        }
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

// PotionEffects.update (the width sort happens at render time, where fonts can be measured)
void tick(events::GameContext* ctx) noexcept {
    if (ctx == nullptr || ctx->jni == nullptr || ctx->jni->env == nullptr || ctx->local_player == nullptr) {
        return;
    }
    resolve_jni(ctx->jni);
    if (!g_jni.usable) {
        return;
    }
    std::vector<EffectInfo> effects;
    if (!read_active_effects(ctx, &effects)) {
        return;
    }

    std::erase_if(g_state.entries, [&](const Entry& entry) {
        return std::none_of(effects.begin(), effects.end(),
                            [&](const EffectInfo& e) { return e.id == entry.potion_id; });
    });

    for (const EffectInfo& effect : effects) {
        auto it = std::find_if(g_state.entries.begin(), g_state.entries.end(),
                               [&](const Entry& entry) { return entry.potion_id == effect.id; });
        if (it == g_state.entries.end()) {
            Entry created{};
            JNIEnv* env = ctx->jni->env;
            if (env->PushLocalFrame(16) != 0) {
                adjust::jni::clear_exception(ctx->jni);
                continue;
            }
            const bool ok = create_entry(ctx->jni, effect.id, &created); // potion_get_by_id leaves a local ref
            env->PopLocalFrame(nullptr);
            if (!ok) {
                continue;
            }
            g_state.entries.push_back(std::move(created));
            it = g_state.entries.end() - 1;
        }
        update_entry(*it, effect.amplifier, effect.duration, effect.maxed);
    }
}

bool chat_open(events::GameContext* game) noexcept {
    if (game == nullptr || game->jni == nullptr || game->jni->env == nullptr || game->minecraft == nullptr ||
        g_jni.cls_gui_chat == nullptr || game->jni->mappings->fid_mc_current_screen == nullptr) {
        return false;
    }
    JNIEnv* env = game->jni->env;
    jobject screen = env->GetObjectField(game->minecraft, game->jni->mappings->fid_mc_current_screen);
    if (screen == nullptr) {
        return false;
    }
    const bool result = env->IsInstanceOf(screen, g_jni.cls_gui_chat) != JNI_FALSE;
    env->DeleteLocalRef(screen);
    return result;
}

std::string entry_text(const Entry& entry) {
    return entry.has_level ? entry.name + " " + entry.level_text : entry.name;
}

// PotionEffects.getWidth
float entry_width(const Entry& entry) {
    return adjust::kTahoma12.width(entry_text(entry)) +
           (entry.has_duration ? adjust::kTahoma10.width(entry.duration_text) : 0.0f);
}

// One NvgRenderer TextCommand (shadowed drawString).
struct TextCommand {
    const adjust::Font* font = nullptr;
    std::string text;
    float x = 0.0f;
    float y = 0.0f;
    Argb color = 0;
};

// PotionEffects.build + HudElement.render (bounds, Anchor.BOTTOM_RIGHT, flush)
void render(const HudContext& ctx) noexcept {
    if (ctx.frame == nullptr) {
        return;
    }
    ensure_initialized();

    std::stable_sort(g_state.entries.begin(), g_state.entries.end(),
                     [](const Entry& a, const Entry& b) { return entry_width(a) < entry_width(b); });

    const float scale = ctx.frame->scale;
    const float target = chat_open(ctx.game) ? 12.5f / std::max(1.0f, scale - 1.0f) : -1.0f;
    g_state.shift.update(target);
    const float shift = g_state.shift.value;

    const adjust::Font& font12 = adjust::kTahoma12;
    const adjust::Font& font10 = adjust::kTahoma10;

    std::vector<TextCommand> commands;
    commands.reserve(g_state.entries.size() * 3);
    int index = 0;
    for (const Entry& entry : g_state.entries) {
        const std::string text = entry_text(entry);
        const auto alpha = static_cast<Argb>(entry.beneficial ? 150 : 220);
        const Argb name_color = (alpha << 24) | entry.liquid_color;

        float duration_block = 0.0f;
        if (entry.has_duration) {
            duration_block = font10.width(entry.duration_text) + 1.0f;
        }
        const float name_width = font12.width(text);
        // Empty spacer command (keeps the list bounds stable while it slides)
        commands.push_back({&font12, std::string(), (-name_width - duration_block) - 2.0f, -shift - 2.0f, name_color});
        const float y = shift + static_cast<float>(index) * 6.5f;

        // Name, right aligned to the left of the duration (2.0 from the right edge of the list)
        commands.push_back({&font12, text, (-name_width - duration_block) - 2.0f, y, name_color});

        if (entry.has_duration) {
            commands.push_back({&font10, entry.duration_text, (1.0f - duration_block) - 2.0f, y + 1.0f, entry.color});
        }
        ++index;
    }
    if (commands.empty()) {
        return;
    }

    // NvgRenderer.getBounds
    float min_x = static_cast<float>(INT32_MAX);
    float min_y = static_cast<float>(INT32_MAX);
    float max_x = static_cast<float>(INT32_MIN);
    float max_y = static_cast<float>(INT32_MIN);
    for (const TextCommand& cmd : commands) {
        const float w = cmd.font->width(cmd.text);
        const float h = cmd.font->size();
        min_x = std::min(min_x, cmd.x);
        min_y = std::min(min_y, cmd.y);
        max_x = std::max(max_x, cmd.x + w);
        max_y = std::max(max_y, cmd.y + h);
    }
    const float bounds_height = max_y - min_y;

    // Anchor.BOTTOM_RIGHT: x = screen width, y = screen height - bounds height; every command is offset by it.
    const float offset_x = ctx.frame->width;
    const float offset_y = ctx.frame->height - bounds_height;
    for (const TextCommand& cmd : commands) {
        if (!cmd.text.empty()) {
            cmd.font->draw(cmd.text, cmd.x + offset_x, cmd.y + offset_y, cmd.color);
        }
    }
}

void on_client_shutdown() noexcept {
    g_state.entries.clear();
}

} // namespace

const HudElement& element_potions() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Potion Effects";
        e.tick = tick;
        e.render = render;
        e.on_client_shutdown = on_client_shutdown;
        return e;
    }();
    return s_element;
}

} // namespace slinky::modules::adjust_hud
