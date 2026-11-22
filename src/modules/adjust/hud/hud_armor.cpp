// Port of the Adjust HUD armor display: HUDModule (setting "Armor", updateArmorItems, setArmorItem, renderArmor,
// armorOffset) and com.adjustclient.module.impl.render.hud.EnchantInfo.
//
// Held item + the four armor pieces (helmet, chestplate, leggings, boots) are laid out 17 units apart, starting 9
// units right of the screen centre, 56 units above the bottom (66 under water). Each one gets its abbreviated
// enchantments (bold Tahoma 8, widest first, stacked upwards 4 units apart) and, when damaged, its remaining
// durability (bold Tahoma 10, centred). The held item is only shown when it is enchanted or damaged. Shown in
// survival and adventure only. armorOffset (7, or 17 under water) is published through adjust_hud::set_armor_offset.
//
// Text runs on the NanoVG HUD layer; the item icons are drawn by armor_render_items() (armor/armor_items.hpp) in
// the kEventRenderWorld2DProj pass. Minecraft 1.8.9 only (Adjust's target); does nothing on 1.7.10.
//
// Not ported: Adjust's SlotSpoofer "render spoofed slot" special case (OpenSlinky++ has no slot spoofer) and the
// armorMovable snap guide of Adjust's HUD editor.

#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/hud/armor/armor_items.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <glad/glad.h>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules::adjust_hud {

namespace {

constexpr int kSlotCount = 5; // [0] held item, [1] helmet, [2] chestplate, [3] leggings, [4] boots
constexpr int kProjectileProtectionId = 4; // Enchantment.projectileProtection

config::BoolSetting g_armor{{}, "Armor", true, true, {}};

// EnchantInfo
struct EnchantInfo {
    bool present = false;
    jobject stack = nullptr; // global ref (EnchantInfo.stack)
    std::vector<std::string> lines;
    std::string durability_text;
    bool damaged = false; // EnchantInfo.isDamaged()
    bool sorted = false;  // lines sorted by width (needs the NanoVG fonts, so done on the first render)
    int x = 0;
    int y = 0;
};

struct ArmorState {
    EnchantInfo items[kSlotCount]{};
    JNIEnv* env = nullptr;    // client thread env (ticks + rendering), used to free the global refs
    bool mode_visible = false; // current game type is SURVIVAL or ADVENTURE
    bool in_water = false;     // thePlayer.isInsideOfMaterial(Material.water)

    // Layout of the last HUD frame, for the item icon pass.
    bool layout_valid = false;
    int64_t layout_time = 0;
    float layout_scale = 2.0f;
};

ArmorState g_state{};

// ---- JNI helpers -------------------------------------------------------------------------------------------

std::string to_utf8(JNIEnv* env, jstring str) {
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

// StatCollector.translateToLocal(key)
std::string translate(events::JniContext* jni, const char* key) {
    JNIEnv* env = jni->env;
    jstring jkey = env->NewStringUTF(key);
    if (jkey == nullptr) {
        env->ExceptionClear();
        return key;
    }
    jstring result = minecraft::stat_collector_translate_to_local(jni, jkey);
    std::string out = adjust::jni::clear_exception(jni) ? std::string(key) : to_utf8(env, result);
    if (result != nullptr) {
        env->DeleteLocalRef(result);
    }
    env->DeleteLocalRef(jkey);
    return out;
}

int item_max_damage(events::JniContext* jni, jobject item) noexcept {
    static const jmethodID mid =
        adjust::jni::method(jni, jni->mappings->cls_item, {"l", "getMaxDamage", "func_77612_l"}, "()I", "()I");
    if (mid == nullptr || item == nullptr) {
        return 0;
    }
    const jint value = jni->env->CallIntMethod(item, mid);
    return adjust::jni::clear_exception(jni) ? 0 : value;
}

jobject stack_tag_compound(events::JniContext* jni, jobject stack) noexcept {
    static const jmethodID mid = adjust::jni::method(jni, jni->mappings->cls_item_stack,
                                                     {"o", "getTagCompound", "func_77978_p"}, "()Ldn;",
                                                     "()Lnet/minecraft/nbt/NBTTagCompound;");
    if (mid == nullptr) {
        return nullptr;
    }
    jobject tag = jni->env->CallObjectMethod(stack, mid);
    return adjust::jni::clear_exception(jni) ? nullptr : tag;
}

int compound_get_byte(events::JniContext* jni, jobject compound, const char* key) noexcept {
    static const jmethodID mid = adjust::jni::method(jni, jni->mappings->cls_nbt_tag_compound,
                                                     {"d", "getByte", "func_74771_c"}, "(Ljava/lang/String;)B",
                                                     "(Ljava/lang/String;)B");
    if (mid == nullptr) {
        return 0;
    }
    JNIEnv* env = jni->env;
    jstring jkey = env->NewStringUTF(key);
    if (jkey == nullptr) {
        env->ExceptionClear();
        return 0;
    }
    const jbyte value = env->CallByteMethod(compound, mid, jkey);
    env->DeleteLocalRef(jkey);
    return adjust::jni::clear_exception(jni) ? 0 : static_cast<int>(value);
}

// ItemStack.areItemStacksEqual(a, b)
bool stacks_equal(events::JniContext* jni, jobject a, jobject b) noexcept {
    static const jmethodID mid = adjust::jni::static_method(
        jni, jni->mappings->cls_item_stack, {"b", "areItemStacksEqual", "func_77989_b"}, "(Lzx;Lzx;)Z",
        "(Lnet/minecraft/item/ItemStack;Lnet/minecraft/item/ItemStack;)Z");
    if (mid == nullptr) {
        return false;
    }
    const bool equal = jni->env->CallStaticBooleanMethod(jni->mappings->cls_item_stack, mid, a, b) != JNI_FALSE;
    return adjust::jni::clear_exception(jni) ? false : equal;
}

// Enchantment.getEnchantmentById(id) (nullptr outside 0..255 / unknown id)
jobject enchantment_by_id(events::JniContext* jni, int id) noexcept {
    static const jclass cls = adjust::jni::find_class(jni, "aci", "net/minecraft/enchantment/Enchantment");
    static const jmethodID mid = adjust::jni::static_method(
        jni, cls, {"c", "getEnchantmentById", "func_180306_c"}, "(I)Laci;", "(I)Lnet/minecraft/enchantment/Enchantment;");
    if (cls == nullptr || mid == nullptr) {
        return nullptr;
    }
    jobject ench = jni->env->CallStaticObjectMethod(cls, mid, static_cast<jint>(id));
    return adjust::jni::clear_exception(jni) ? nullptr : ench;
}

std::string enchantment_name(events::JniContext* jni, jobject ench) {
    static const jclass cls = adjust::jni::find_class(jni, "aci", "net/minecraft/enchantment/Enchantment");
    static const jmethodID mid = adjust::jni::method(jni, cls, {"a", "getName", "func_77320_a"},
                                                     "()Ljava/lang/String;", "()Ljava/lang/String;");
    if (mid == nullptr) {
        return {};
    }
    auto name = static_cast<jstring>(jni->env->CallObjectMethod(ench, mid));
    if (adjust::jni::clear_exception(jni) || name == nullptr) {
        return {};
    }
    std::string out = to_utf8(jni->env, name);
    jni->env->DeleteLocalRef(name);
    return out;
}

int enchantment_max_level(events::JniContext* jni, jobject ench) noexcept {
    static const jclass cls = adjust::jni::find_class(jni, "aci", "net/minecraft/enchantment/Enchantment");
    static const jmethodID mid = adjust::jni::method(jni, cls, {"b", "getMaxLevel", "func_77325_b"}, "()I", "()I");
    if (mid == nullptr) {
        return 0;
    }
    const jint value = jni->env->CallIntMethod(ench, mid);
    return adjust::jni::clear_exception(jni) ? 0 : value;
}

// ---- EnchantInfo -------------------------------------------------------------------------------------------

// Number of code points of a UTF-8 string.
size_t utf8_length(const std::string& s) noexcept {
    size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++n;
        }
    }
    return n;
}

// First `count` code points (String.substring(0, count)); the whole string if shorter.
std::string utf8_prefix(const std::string& s, size_t count) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0u) != 0x80u) {
            if (n == count) {
                return s.substr(0, i);
            }
            ++n;
        }
    }
    return s;
}

// StringUtils.stripControlCodes: removes "§" + one of [0-9a-fk-or] (case-insensitive).
std::string strip_control_codes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) == 0xC2u && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA7u) {
            const char c = static_cast<char>(s[i + 2] | 0x20); // lower case for letters
            if ((s[i + 2] >= '0' && s[i + 2] <= '9') || (c >= 'a' && c <= 'f') || (c >= 'k' && c <= 'o') ||
                c == 'r') {
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool equals_ignore_case_ascii(const std::string& a, const char* b) noexcept {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

// EnchantInfo.format: translated "name level" without colour codes; one-level enchantments give the first four
// letters, others the first three letters (Projectile Protection "Pro" -> "Prj"), grey, a space and the level.
std::string format_enchantment(events::JniContext* jni, jobject ench, int id, int level) {
    const std::string name = translate(jni, enchantment_name(jni, ench).c_str());
    char level_key[48]{};
    std::snprintf(level_key, sizeof(level_key), "enchantment.level.%d", level);
    const std::string level_text = translate(jni, level_key);
    const std::string stripped = strip_control_codes(name + " " + level_text);
    if (enchantment_max_level(jni, ench) == 1) {
        return utf8_prefix(stripped, 4); // Adjust: substring(0, 4)
    }
    if (utf8_length(stripped) >= 4) {
        std::string prefix = utf8_prefix(stripped, 3);
        if (id == kProjectileProtectionId) {
            prefix = equals_ignore_case_ascii(prefix, "Pro") ? "Prj" : "";
        }
        char suffix[32]{};
        std::snprintf(suffix, sizeof(suffix), ADJUST_FMT "7 %d", level);
        return prefix + suffix;
    }
    return stripped;
}

void release(EnchantInfo& info) noexcept {
    if (info.stack != nullptr && g_state.env != nullptr) {
        g_state.env->DeleteGlobalRef(info.stack);
    }
    info = EnchantInfo{};
}

// new EnchantInfo(stack)
void build(events::JniContext* jni, EnchantInfo& info, jobject stack) {
    release(info);
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(32) != 0) {
        env->ExceptionClear();
        return;
    }
    info.present = true;
    info.stack = env->NewGlobalRef(stack);

    jobject item = minecraft::item_stack_get_item(jni, stack);
    adjust::jni::clear_exception(jni);
    const int max_damage = item_max_damage(jni, item);
    const int damage = minecraft::item_stack_get_damage(jni, stack);
    const std::string remaining = std::to_string(max_damage - damage);

    jobject tag = stack_tag_compound(jni, stack);
    bool unbreakable = false;
    if (tag == nullptr) {
        info.durability_text = remaining;
    } else {
        jobject enchants = minecraft::item_stack_get_enchantment_tag_list(jni, stack); // tag.getTagList("ench", 10)
        adjust::jni::clear_exception(jni);
        jstring id_key = env->NewStringUTF("id");
        jstring lvl_key = env->NewStringUTF("lvl");
        const int count = (enchants != nullptr && id_key != nullptr && lvl_key != nullptr)
                              ? minecraft::nbt_tag_list_count(jni, enchants)
                              : 0;
        for (int i = 0; i < count; ++i) {
            jobject compound = minecraft::nbt_tag_list_get_compound_at(jni, enchants, i);
            if (adjust::jni::clear_exception(jni) || compound == nullptr) {
                continue;
            }
            const int id = minecraft::nbt_tag_compound_get_short(jni, compound, id_key);
            const int level = minecraft::nbt_tag_compound_get_short(jni, compound, lvl_key);
            adjust::jni::clear_exception(jni);
            if (id >= 0 && id < 256) {
                jobject ench = enchantment_by_id(jni, id);
                if (ench != nullptr) {
                    info.lines.push_back(format_enchantment(jni, ench, id, level));
                    env->DeleteLocalRef(ench);
                }
            }
            env->DeleteLocalRef(compound);
        }
        unbreakable = compound_get_byte(jni, tag, "Unbreakable") != 0;
        info.durability_text = unbreakable ? "Inf" : remaining;
    }
    info.damaged = max_damage >= 1 && !unbreakable && damage >= 1;
    info.sorted = false;
    env->PopLocalFrame(nullptr);
}

// HUDModule.setArmorItem / the held-item branch of updateArmorItems.
void set_item(events::JniContext* jni, int slot, jobject stack) {
    EnchantInfo& cur = g_state.items[slot];
    if (stack == nullptr) {
        if (cur.present) {
            release(cur);
        }
        return;
    }
    if (cur.present && (jni->env->IsSameObject(cur.stack, stack) != JNI_FALSE || stacks_equal(jni, cur.stack, stack))) {
        return;
    }
    build(jni, cur, stack);
    if (slot == 0 && cur.lines.empty() && !cur.damaged) {
        release(cur); // the held item is only shown when enchanted or damaged
    }
}

// Current game type is SURVIVAL or ADVENTURE (PlayerControllerMP.getCurrentGameType()).
bool survival_or_adventure(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    static const jmethodID mid = adjust::jni::method(
        jni, jni->mappings->cls_player_controller_mp, {"l", "getCurrentGameType", "func_178889_l"}, "()Ladp$a;",
        "()Lnet/minecraft/world/WorldSettings$GameType;");
    if (mid == nullptr || jni->mappings->mid_enum_ordinal == nullptr) {
        return false;
    }
    jobject controller = minecraft::mc_get_player_controller(jni, ctx->minecraft);
    if (controller == nullptr) {
        return false;
    }
    bool visible = false;
    jobject type = jni->env->CallObjectMethod(controller, mid);
    if (!adjust::jni::clear_exception(jni) && type != nullptr) {
        // NOT_SET, SURVIVAL, CREATIVE, ADVENTURE, SPECTATOR
        const jint ordinal = jni->env->CallIntMethod(type, jni->mappings->mid_enum_ordinal);
        visible = !adjust::jni::clear_exception(jni) && (ordinal == 1 || ordinal == 3);
        jni->env->DeleteLocalRef(type);
    }
    jni->env->DeleteLocalRef(controller);
    return visible;
}

// thePlayer.isInsideOfMaterial(Material.water)
bool inside_water(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    static const jclass material_cls = adjust::jni::find_class(jni, "arm", "net/minecraft/block/material/Material");
    static const jfieldID water_fid = adjust::jni::static_field(jni, material_cls, {"h", "water", "field_151586_h"},
                                                                "Larm;", "Lnet/minecraft/block/material/Material;");
    static const jmethodID inside_mid = adjust::jni::method(
        jni, jni->mappings->cls_entity, {"a", "isInsideOfMaterial", "func_70055_a"}, "(Larm;)Z",
        "(Lnet/minecraft/block/material/Material;)Z");
    if (material_cls == nullptr || water_fid == nullptr || inside_mid == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    jobject water = env->GetStaticObjectField(material_cls, water_fid);
    if (water == nullptr) {
        adjust::jni::clear_exception(jni);
        return false;
    }
    const bool inside = env->CallBooleanMethod(ctx->local_player, inside_mid, water) != JNI_FALSE;
    env->DeleteLocalRef(water);
    return adjust::jni::clear_exception(jni) ? false : inside;
}

// ---- element callbacks -------------------------------------------------------------------------------------

void tick(events::GameContext* ctx) noexcept {
    if (ctx == nullptr || ctx->jni == nullptr || ctx->jni->env == nullptr || ctx->local_player == nullptr) {
        return;
    }
    g_state.env = ctx->jni->env;
    if (!adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    g_state.mode_visible = survival_or_adventure(ctx);
    g_state.in_water = inside_water(ctx);

    // updateArmorItems
    if (!g_armor.value) {
        return;
    }
    if (env->PushLocalFrame(16) != 0) {
        env->ExceptionClear();
        return;
    }
    // getEquipmentInSlot: 0 = held item, 1 = boots .. 4 = helmet; Adjust order: held, helmet, chest, legs, boots.
    jobject held = minecraft::player_get_equipment_in_slot(jni, ctx->local_player, 0);
    adjust::jni::clear_exception(jni);
    set_item(jni, 0, held);
    for (int slot = 1; slot < kSlotCount; ++slot) {
        jobject stack = minecraft::player_get_equipment_in_slot(jni, ctx->local_player, kSlotCount - slot);
        adjust::jni::clear_exception(jni);
        set_item(jni, slot, stack);
    }
    env->PopLocalFrame(nullptr);
}

// HUDModule.renderArmor (text part) + layout for the icon pass.
void render(const HudContext& ctx) noexcept {
    g_state.layout_valid = false;
    if (!g_armor.value || ctx.game == nullptr || ctx.frame == nullptr || ctx.game->jni == nullptr ||
        !adjust::jni::is_1_8(ctx.game->jni) || !g_state.mode_visible) {
        set_armor_offset(0);
        return;
    }
    const int x0 = ctx.frame->scaled_width / 2 + 9;
    const bool in_water = g_state.in_water;
    const int y = ctx.frame->scaled_height - (in_water ? 66 : 56);
    const int offset = in_water ? 17 : 7;

    int x = x0;
    for (EnchantInfo& info : g_state.items) {
        if (!info.present) {
            continue;
        }
        info.x = x;
        info.y = y;
        x += 17;
    }

    for (EnchantInfo& info : g_state.items) {
        if (!info.present) {
            continue;
        }
        if (!info.sorted) {
            // Sort key: negated Tahoma 8 width, so the widest line comes first (stable, like List.sort).
            std::stable_sort(info.lines.begin(), info.lines.end(), [](const std::string& a, const std::string& b) {
                return -adjust::kTahoma8.width(a) < -adjust::kTahoma8.width(b);
            });
            info.sorted = true;
        }
        for (size_t i = 0; i < info.lines.size(); ++i) {
            const std::string line = std::string(ADJUST_FMT "l") + info.lines[i];
            adjust::kTahoma8.draw(line, static_cast<float>(info.x),
                                  (static_cast<float>(info.y) + 2.5f - adjust::kTahoma8.size()) -
                                      static_cast<float>(4 * i),
                                  0xFFFAFAFBu);
        }
        if (info.damaged) {
            const std::string text = std::string(ADJUST_FMT "l") + info.durability_text;
            adjust::kTahoma10.draw_centered(text, static_cast<float>(info.x) + 8.5f, static_cast<float>(info.y) + 9.0f,
                                            0xFFEBEBF5u);
        }
    }

    g_state.layout_valid = true;
    g_state.layout_time = adjust::now_ms();
    g_state.layout_scale = ctx.frame->scale;
    set_armor_offset(offset);
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_armor.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    g_armor.visit(ar, "armor_enabled");
}

void on_client_shutdown() noexcept {
    for (EnchantInfo& info : g_state.items) {
        release(info);
    }
    g_state.layout_valid = false;
}

} // namespace

void armor_render_items(events::RenderWorld2DProjEvent* ev) noexcept {
    if (ev == nullptr || ev->ctx == nullptr || ev->ctx->jni == nullptr || ev->ctx->minecraft == nullptr) {
        return;
    }
    // Only while the HUD element drew this layout recently (HUD module running, Armor on, survival/adventure).
    if (!g_armor.value || !g_state.layout_valid || adjust::now_ms() - g_state.layout_time > 250) {
        return;
    }
    events::GameContext* ctx = ev->ctx;
    events::JniContext* jni = ctx->jni;
    if (!adjust::jni::is_1_8(jni) || !ev->is_hand_pass) {
        return;
    }
    int first = -1;
    int last = -1;
    for (int i = 0; i < kSlotCount; ++i) {
        if (g_state.items[i].present) {
            if (first < 0) {
                first = i;
            }
            last = i;
        }
    }
    if (first < 0) {
        return;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        env->ExceptionClear();
        return;
    }
    jobject render_item = minecraft::mc_get_render_item(jni, ctx->minecraft);
    jobject render_manager = minecraft::mc_get_render_manager(jni, ctx->minecraft);
    jobject font_renderer =
        render_manager != nullptr ? minecraft::render_manager_get_font_renderer(jni, render_manager) : nullptr;
    jobject texture_manager = minecraft::mc_get_texture_manager(jni, ctx->minecraft);
    static const jmethodID gui_lighting = adjust::jni::static_method(
        jni, jni->mappings->cls_render_helper, {"c", "enableGUIStandardItemLighting", "func_74520_c"}, "()V", "()V");
    if (render_item == nullptr || font_renderer == nullptr || texture_manager == nullptr) {
        adjust::jni::clear_exception(jni);
        env->PopLocalFrame(nullptr);
        return;
    }

    const float scale = g_state.layout_scale;
    const float fb_height = ev->viewport_size.y;

    // The world depth is still in the depth buffer (the hook clears it right after this pass): clear it under the
    // icons only, so 3D block items depth-sort correctly without being hidden by the world.
    {
        const float px1 = static_cast<float>(g_state.items[first].x) * scale;
        const float px2 = static_cast<float>(g_state.items[last].x + 16) * scale;
        const float py1 = static_cast<float>(g_state.items[first].y) * scale;
        const float py2 = static_cast<float>(g_state.items[first].y + 16) * scale;
        const GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
        GLint saved_box[4]{};
        glGetIntegerv(GL_SCISSOR_BOX, saved_box);
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(px1) - 1, static_cast<GLint>(fb_height - py2) - 1,
                  static_cast<GLsizei>(px2 - px1) + 2, static_cast<GLsizei>(py2 - py1) + 2);
        glClear(GL_DEPTH_BUFFER_BIT);
        glScissor(saved_box[0], saved_box[1], saved_box[2], saved_box[3]);
        if (scissor_was_on == GL_FALSE) {
            glDisable(GL_SCISSOR_TEST);
        }
    }

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // RenderHelper.enableGUIStandardItemLighting(), with an unscaled modelview like Minecraft's GUI pass (the light
    // directions are captured in eye space).
    if (gui_lighting != nullptr) {
        env->CallStaticVoidMethod(jni->mappings->cls_render_helper, gui_lighting);
        adjust::jni::clear_exception(jni);
    } else {
        minecraft::render_helper_enable_item_lighting(jni);
    }
    // HUD units -> framebuffer pixels. Uniform so RenderItem's GL_RESCALE_NORMAL keeps the normals right; the
    // item depth (z = 100 +- 10 units) stays far inside the pass's -3000..3000 orthographic range.
    glScalef(scale, scale, scale);
    minecraft::gl_state_enable_depth(jni);
    glEnable(GL_DEPTH_TEST);

    for (const EnchantInfo& info : g_state.items) {
        if (!info.present || info.stack == nullptr) {
            continue;
        }
        minecraft::render_item_into_gui(jni, render_item, font_renderer, texture_manager, info.stack, info.x, info.y);
        adjust::jni::clear_exception(jni);
        minecraft::render_item_overlay_into_gui(jni, render_item, font_renderer, texture_manager, info.stack, info.x,
                                                info.y, nullptr);
        adjust::jni::clear_exception(jni);
    }

    // RenderHelper.disableStandardItemLighting(), then back to the state this pass runs in (no depth, no texture,
    // no lighting, alpha blending), keeping GlStateManager's cache in sync.
    minecraft::render_helper_disable_item_lighting(jni);
    minecraft::gl_state_disable_depth(jni);
    minecraft::gl_state_disable_lighting(jni);
    minecraft::gl_state_enable_blend(jni);
    minecraft::gl_state_blend_func(jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    minecraft::gl_state_disable_texture_2d(jni);
    minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
    adjust::jni::clear_exception(jni);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glPopMatrix();
    env->PopLocalFrame(nullptr);
}

const HudElement& element_armor() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Armor";
        e.render_settings = render_settings;
        e.visit_config = visit_config;
        e.tick = tick;
        e.render = render;
        e.on_client_shutdown = on_client_shutdown;
        e.on_render_world_2d = armor_render_items; // item icons via Minecraft's RenderItem
        return e;
    }();
    return s_element;
}

} // namespace slinky::modules::adjust_hud
