#include "modules/adjust/render/esp/adjust_render_jni.hpp"

#include "adjust/jni_lookup.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"

namespace slinky::modules::adjust_render {

namespace J = adjust::jni;

namespace {

// Field/method readers that tolerate unresolved IDs and pending exceptions.
float get_float(events::JniContext* jni, jobject obj, jfieldID id) noexcept {
    return (obj != nullptr && id != nullptr) ? jni->env->GetFloatField(obj, id) : 0.0f;
}

double get_double(events::JniContext* jni, jobject obj, jfieldID id) noexcept {
    return (obj != nullptr && id != nullptr) ? jni->env->GetDoubleField(obj, id) : 0.0;
}

int get_int(events::JniContext* jni, jobject obj, jfieldID id) noexcept {
    return (obj != nullptr && id != nullptr) ? jni->env->GetIntField(obj, id) : 0;
}

bool get_bool(events::JniContext* jni, jobject obj, jfieldID id) noexcept {
    return obj != nullptr && id != nullptr && jni->env->GetBooleanField(obj, id) != JNI_FALSE;
}

bool call_bool(events::JniContext* jni, jobject obj, jmethodID id) noexcept {
    if (obj == nullptr || id == nullptr) {
        return false;
    }
    const bool result = jni->env->CallBooleanMethod(obj, id) != JNI_FALSE;
    return !J::clear_exception(jni) && result;
}

int call_int(events::JniContext* jni, jobject obj, jmethodID id) noexcept {
    if (obj == nullptr || id == nullptr) {
        return 0;
    }
    const int result = jni->env->CallIntMethod(obj, id);
    return J::clear_exception(jni) ? 0 : result;
}

float call_float(events::JniContext* jni, jobject obj, jmethodID id) noexcept {
    if (obj == nullptr || id == nullptr) {
        return 0.0f;
    }
    const float result = jni->env->CallFloatMethod(obj, id);
    return J::clear_exception(jni) ? 0.0f : result;
}

jobject call_object(events::JniContext* jni, jobject obj, jmethodID id) noexcept {
    if (obj == nullptr || id == nullptr) {
        return nullptr;
    }
    jobject result = jni->env->CallObjectMethod(obj, id);
    if (J::clear_exception(jni)) {
        drop(jni, result);
        return nullptr;
    }
    return result;
}

std::string call_string(events::JniContext* jni, jobject obj, jmethodID id) {
    auto text = static_cast<jstring>(call_object(jni, obj, id));
    std::string out = to_utf8(jni, text);
    drop(jni, text);
    return out;
}

jclass cls_entity(events::JniContext* jni) noexcept { return jni->mappings->cls_entity; }
jclass cls_living(events::JniContext* jni) noexcept { return jni->mappings->cls_entity_living_base; }
jclass cls_stack(events::JniContext* jni) noexcept { return jni->mappings->cls_item_stack; }

jclass lookup_class(events::JniContext* jni, Cls cls) noexcept {
    switch (cls) {
        case Cls::EntityLivingBase: return jni->mappings->cls_entity_living_base;
        case Cls::EntityPlayer: return jni->mappings->cls_entity_player;
        case Cls::EntityPlayerSP: return jni->mappings->cls_entity_player_sp;
        case Cls::EntityItem: return jni->mappings->cls_entity_item;
        case Cls::EntityLiving: {
            static const jclass c = J::find_class(jni, "ps", "net/minecraft/entity/EntityLiving");
            return c;
        }
        case Cls::EntityTNTPrimed: {
            static const jclass c = J::find_class(jni, "vj", "net/minecraft/entity/item/EntityTNTPrimed");
            return c;
        }
        case Cls::EntityMob: {
            static const jclass c = J::find_class(jni, "vv", "net/minecraft/entity/monster/EntityMob");
            return c;
        }
        case Cls::EntityIronGolem: {
            static const jclass c = J::find_class(jni, "ty", "net/minecraft/entity/monster/EntityIronGolem");
            return c;
        }
        case Cls::EntityArmorStand: {
            static const jclass c = J::find_class(jni, "um", "net/minecraft/entity/item/EntityArmorStand");
            return c;
        }
        case Cls::EntityZombie: {
            static const jclass c = J::find_class(jni, "we", "net/minecraft/entity/monster/EntityZombie");
            return c;
        }
        case Cls::ItemSword: {
            static const jclass c = J::find_class(jni, "aay", "net/minecraft/item/ItemSword");
            return c;
        }
        case Cls::ItemTool: {
            static const jclass c = J::find_class(jni, "za", "net/minecraft/item/ItemTool");
            return c;
        }
        default: return nullptr;
    }
}

} // namespace

// ---- generic --------------------------------------------------------------------------------------------------

bool available(events::JniContext* jni) noexcept {
    return J::is_1_8(jni);
}

std::string to_utf8(events::JniContext* jni, jstring text) {
    if (text == nullptr) {
        return {};
    }
    const char* chars = jni->env->GetStringUTFChars(text, nullptr);
    if (chars == nullptr) {
        J::clear_exception(jni);
        return {};
    }
    std::string out(chars);
    jni->env->ReleaseStringUTFChars(text, chars);
    return out;
}

std::string object_to_string(events::JniContext* jni, jobject obj) {
    if (obj == nullptr) {
        return {};
    }
    static const jmethodID mid = [&]() -> jmethodID {
        jclass object_cls = jni->env->FindClass("java/lang/Object");
        if (object_cls == nullptr) {
            J::clear_exception(jni);
            return nullptr;
        }
        jmethodID id = jni->env->GetMethodID(object_cls, "toString", "()Ljava/lang/String;");
        jni->env->DeleteLocalRef(object_cls);
        if (id == nullptr) {
            J::clear_exception(jni);
        }
        return id;
    }();
    return call_string(jni, obj, mid);
}

bool is_instance(events::JniContext* jni, jobject obj, Cls cls) noexcept {
    if (obj == nullptr || !available(jni)) {
        return false;
    }
    jclass c = lookup_class(jni, cls);
    return c != nullptr && jni->env->IsInstanceOf(obj, c) != JNI_FALSE;
}

// ---- Entity ---------------------------------------------------------------------------------------------------

math::Vec3d entity_last_tick_pos(events::JniContext* jni, jobject entity) noexcept {
    if (!available(jni)) {
        return minecraft::entity_get_prev_position(jni, entity);
    }
    static const jfieldID fx = J::field(jni, cls_entity(jni), {"P", "lastTickPosX", "field_70142_S"}, "D", "D");
    static const jfieldID fy = J::field(jni, cls_entity(jni), {"Q", "lastTickPosY", "field_70137_T"}, "D", "D");
    static const jfieldID fz = J::field(jni, cls_entity(jni), {"R", "lastTickPosZ", "field_70136_U"}, "D", "D");
    if (fx == nullptr || fy == nullptr || fz == nullptr) {
        return minecraft::entity_get_prev_position(jni, entity);
    }
    return math::make_vec3(get_double(jni, entity, fx), get_double(jni, entity, fy), get_double(jni, entity, fz));
}

float entity_width(events::JniContext* jni, jobject entity) noexcept {
    if (!available(jni)) {
        return 0.6f;
    }
    static const jfieldID f = J::field(jni, cls_entity(jni), {"J", "width", "field_70130_N"}, "F", "F");
    return f != nullptr ? get_float(jni, entity, f) : 0.6f;
}

float entity_height(events::JniContext* jni, jobject entity) noexcept {
    if (!available(jni)) {
        return 1.8f;
    }
    static const jfieldID f = J::field(jni, cls_entity(jni), {"K", "height", "field_70131_O"}, "F", "F");
    return f != nullptr ? get_float(jni, entity, f) : 1.8f;
}

bool entity_is_invisible(events::JniContext* jni, jobject entity) noexcept {
    if (!available(jni)) {
        return false;
    }
    static const jmethodID m = J::method(jni, cls_entity(jni), {"ax", "isInvisible", "func_82150_aj"}, "()Z", "()Z");
    return call_bool(jni, entity, m);
}

bool entity_is_riding(events::JniContext* jni, jobject entity) noexcept {
    if (!available(jni)) {
        return false;
    }
    static const jmethodID m = J::method(jni, cls_entity(jni), {"au", "isRiding", "func_70115_ae"}, "()Z", "()Z");
    return call_bool(jni, entity, m);
}

bool entity_ignore_frustum_check(events::JniContext* jni, jobject entity) noexcept {
    if (!available(jni)) {
        return false;
    }
    static const jfieldID f = J::field(jni, cls_entity(jni), {"ah", "ignoreFrustumCheck", "field_70158_ak"}, "Z", "Z");
    return get_bool(jni, entity, f);
}

float entity_pitch(events::JniContext* jni, jobject entity) noexcept {
    return minecraft::entity_get_rotation(jni, entity).y;
}

float entity_prev_pitch(events::JniContext* jni, jobject entity) noexcept {
    return minecraft::entity_get_prev_rotation(jni, entity).y;
}

std::string entity_name(events::JniContext* jni, jobject entity) {
    jstring name = minecraft::entity_get_name(jni, entity);
    J::clear_exception(jni);
    std::string out = to_utf8(jni, name);
    drop(jni, name);
    return out;
}

std::string entity_formatted_display_name(events::JniContext* jni, jobject entity) {
    jobject component = minecraft::entity_get_display_name(jni, entity);
    if (J::clear_exception(jni) || component == nullptr) {
        drop(jni, component);
        return {};
    }
    jstring text = minecraft::chat_component_get_formatted_text(jni, component);
    J::clear_exception(jni);
    std::string out = to_utf8(jni, text);
    drop(jni, text);
    drop(jni, component);
    return out;
}

// ---- EntityLivingBase -----------------------------------------------------------------------------------------

#define ADJUST_LIVING_FLOAT_FIELD(fn, notch, mcp, srg)                                                   \
    float fn(events::JniContext* jni, jobject living) noexcept {                                        \
        if (!available(jni)) {                                                                          \
            return 0.0f;                                                                                \
        }                                                                                               \
        static const jfieldID f = J::field(jni, cls_living(jni), {notch, mcp, srg}, "F", "F");          \
        return get_float(jni, living, f);                                                               \
    }

ADJUST_LIVING_FLOAT_FIELD(living_render_yaw_offset, "aI", "renderYawOffset", "field_70761_aq")
ADJUST_LIVING_FLOAT_FIELD(living_prev_render_yaw_offset, "aJ", "prevRenderYawOffset", "field_70760_ar")
ADJUST_LIVING_FLOAT_FIELD(living_head_yaw, "aK", "rotationYawHead", "field_70759_as")
ADJUST_LIVING_FLOAT_FIELD(living_prev_head_yaw, "aL", "prevRotationYawHead", "field_70758_at")
ADJUST_LIVING_FLOAT_FIELD(living_limb_swing, "aC", "limbSwing", "field_70754_ba")
ADJUST_LIVING_FLOAT_FIELD(living_limb_swing_amount, "aB", "limbSwingAmount", "field_70721_aZ")
ADJUST_LIVING_FLOAT_FIELD(living_prev_limb_swing_amount, "aA", "prevLimbSwingAmount", "field_70722_aY")

#undef ADJUST_LIVING_FLOAT_FIELD

int living_death_time(events::JniContext* jni, jobject living) noexcept {
    if (!available(jni)) {
        return 0;
    }
    static const jfieldID f = J::field(jni, cls_living(jni), {"ax", "deathTime", "field_70725_aQ"}, "I", "I");
    return get_int(jni, living, f);
}

bool living_is_child(events::JniContext* jni, jobject living) noexcept {
    if (!available(jni)) {
        return false;
    }
    static const jmethodID m = J::method(jni, cls_living(jni), {"j_", "isChild", "func_70631_g_"}, "()Z", "()Z");
    return call_bool(jni, living, m);
}

float living_swing_progress(events::JniContext* jni, jobject living, float partial_ticks) noexcept {
    if (!available(jni) || living == nullptr) {
        return 0.0f;
    }
    static const jmethodID m = J::method(jni, cls_living(jni), {"l", "getSwingProgress", "func_70678_g"}, "(F)F", "(F)F");
    if (m == nullptr) {
        return 0.0f;
    }
    const float result = jni->env->CallFloatMethod(living, m, partial_ticks);
    return J::clear_exception(jni) ? 0.0f : result;
}

int living_total_armor(events::JniContext* jni, jobject living) noexcept {
    if (!available(jni)) {
        return 0;
    }
    static const jmethodID m = J::method(jni, cls_living(jni), {"br", "getTotalArmorValue", "func_70658_aO"}, "()I", "()I");
    return call_int(jni, living, m);
}

float living_absorption(events::JniContext* jni, jobject living) noexcept {
    if (!available(jni)) {
        return 0.0f;
    }
    static const jmethodID m = J::method(jni, cls_living(jni), {"bN", "getAbsorptionAmount", "func_110139_bj"}, "()F", "()F");
    return call_float(jni, living, m);
}

jobject living_held_item(events::JniContext* jni, jobject living) noexcept {
    if (!available(jni)) {
        return nullptr;
    }
    static const jmethodID m = J::method(jni, cls_living(jni), {"bA", "getHeldItem", "func_70694_bm"},
                                         "()Lzx;", "()Lnet/minecraft/item/ItemStack;");
    return call_object(jni, living, m);
}

jobject living_current_armor(events::JniContext* jni, jobject living, int slot) noexcept {
    if (!available(jni) || living == nullptr) {
        return nullptr;
    }
    static const jmethodID m = J::method(jni, cls_living(jni), {"q", "getCurrentArmor", "func_82169_q"},
                                         "(I)Lzx;", "(I)Lnet/minecraft/item/ItemStack;");
    if (m == nullptr) {
        return nullptr;
    }
    jobject result = jni->env->CallObjectMethod(living, m, slot);
    if (J::clear_exception(jni)) {
        drop(jni, result);
        return nullptr;
    }
    return result;
}

int living_potion_icons(events::JniContext* jni, jobject living, PotionIcon* out, int max_count) noexcept {
    if (!available(jni) || living == nullptr || out == nullptr || max_count <= 0) {
        return 0;
    }
    static const jmethodID m_effects = J::method(jni, cls_living(jni), {"bl", "getActivePotionEffects", "func_70651_bq"},
                                                 "()Ljava/util/Collection;", "()Ljava/util/Collection;");
    static const jmethodID m_to_array = [&]() -> jmethodID {
        jclass collection = jni->env->FindClass("java/util/Collection");
        if (collection == nullptr) {
            J::clear_exception(jni);
            return nullptr;
        }
        jmethodID id = jni->env->GetMethodID(collection, "toArray", "()[Ljava/lang/Object;");
        jni->env->DeleteLocalRef(collection);
        if (id == nullptr) {
            J::clear_exception(jni);
        }
        return id;
    }();
    static const jmethodID m_potion_id = J::method(jni, jni->mappings->cls_potion_effect,
                                                   {"a", "getPotionID", "func_76456_a"}, "()I", "()I");
    static const jmethodID m_amplifier = J::method(jni, jni->mappings->cls_potion_effect,
                                                   {"c", "getAmplifier", "func_76458_c"}, "()I", "()I");
    static const jfieldID f_potion_types = J::static_field(jni, jni->mappings->cls_potion,
                                                           {"a", "potionTypes", "field_76425_a"},
                                                           "[Lpe;", "[Lnet/minecraft/potion/Potion;");
    static const jmethodID m_icon = J::method(jni, jni->mappings->cls_potion,
                                              {"f", "getStatusIconIndex", "func_76392_e"}, "()I", "()I");
    if (m_effects == nullptr || m_to_array == nullptr || m_potion_id == nullptr || m_amplifier == nullptr ||
        f_potion_types == nullptr || m_icon == nullptr) {
        return 0;
    }

    JNIEnv* env = jni->env;
    jobject effects = call_object(jni, living, m_effects);
    if (effects == nullptr) {
        return 0;
    }
    auto array = static_cast<jobjectArray>(call_object(jni, effects, m_to_array));
    drop(jni, effects);
    if (array == nullptr) {
        return 0;
    }
    auto types = static_cast<jobjectArray>(env->GetStaticObjectField(jni->mappings->cls_potion, f_potion_types));
    if (J::clear_exception(jni) || types == nullptr) {
        drop(jni, array);
        drop(jni, types);
        return 0;
    }
    const jsize type_count = env->GetArrayLength(types);
    const jsize count = env->GetArrayLength(array);
    int written = 0;
    for (jsize i = 0; i < count && written < max_count; ++i) {
        jobject effect = env->GetObjectArrayElement(array, i);
        if (effect == nullptr) {
            continue;
        }
        const int id = call_int(jni, effect, m_potion_id);
        const int amplifier = call_int(jni, effect, m_amplifier);
        drop(jni, effect);
        if (id < 0 || id >= type_count) {
            continue;
        }
        jobject potion = env->GetObjectArrayElement(types, id);
        if (potion == nullptr) {
            continue;
        }
        const int icon = call_int(jni, potion, m_icon);
        drop(jni, potion);
        if (icon < 0) {
            continue;
        }
        out[written].icon_index = icon;
        out[written].amplifier = amplifier;
        ++written;
    }
    drop(jni, types);
    drop(jni, array);
    return written;
}

// ---- EntityPlayer ---------------------------------------------------------------------------------------------

bool player_is_blocking(events::JniContext* jni, jobject player) noexcept {
    if (!available(jni)) {
        return false;
    }
    static const jmethodID m = J::method(jni, jni->mappings->cls_entity_player, {"bW", "isBlocking", "func_70632_aY"},
                                         "()Z", "()Z");
    return call_bool(jni, player, m);
}

bool player_is_creative(events::JniContext* jni, jobject player) noexcept {
    if (!available(jni) || player == nullptr) {
        return false;
    }
    static const jfieldID f_caps = J::field(jni, jni->mappings->cls_entity_player, {"bA", "capabilities", "field_71075_bZ"},
                                            "Lwl;", "Lnet/minecraft/entity/player/PlayerCapabilities;");
    static const jclass cls_caps = J::find_class(jni, "wl", "net/minecraft/entity/player/PlayerCapabilities");
    static const jfieldID f_creative = J::field(jni, cls_caps, {"d", "isCreativeMode", "field_75098_d"}, "Z", "Z");
    if (f_caps == nullptr || f_creative == nullptr) {
        return false;
    }
    jobject caps = jni->env->GetObjectField(player, f_caps);
    const bool creative = get_bool(jni, caps, f_creative);
    drop(jni, caps);
    return creative;
}

bool player_is_spectator(events::JniContext* jni, jobject player) noexcept {
    if (!available(jni)) {
        return false;
    }
    static const jmethodID m = J::method(jni, jni->mappings->cls_entity_player, {"v", "isSpectator", "func_175149_v"},
                                         "()Z", "()Z");
    return call_bool(jni, player, m);
}

// ---- EntityItem / EntityTNTPrimed -----------------------------------------------------------------------------

float item_entity_hover_start(events::JniContext* jni, jobject item_entity) noexcept {
    if (!available(jni)) {
        return 0.0f;
    }
    static const jfieldID f = J::field(jni, jni->mappings->cls_entity_item, {"a", "hoverStart", "field_70290_d"}, "F", "F");
    return get_float(jni, item_entity, f);
}

int item_entity_age(events::JniContext* jni, jobject item_entity) noexcept {
    if (!available(jni)) {
        return 0;
    }
    static const jfieldID f = J::field(jni, jni->mappings->cls_entity_item, {"c", "age", "field_70292_b"}, "I", "I");
    return get_int(jni, item_entity, f);
}

int tnt_fuse(events::JniContext* jni, jobject tnt) noexcept {
    if (!available(jni)) {
        return 0;
    }
    static const jfieldID f = J::field(jni, lookup_class(jni, Cls::EntityTNTPrimed), {"a", "fuse", "field_70516_a"}, "I", "I");
    return get_int(jni, tnt, f);
}

// ---- ItemStack / Item -----------------------------------------------------------------------------------------

int stack_metadata(events::JniContext* jni, jobject stack) noexcept {
    if (!available(jni)) {
        return 0;
    }
    static const jmethodID m = J::method(jni, cls_stack(jni), {"i", "getMetadata", "func_77960_j"}, "()I", "()I");
    return call_int(jni, stack, m);
}

int stack_item_damage(events::JniContext* jni, jobject stack) noexcept {
    if (!available(jni)) {
        return 0;
    }
    static const jmethodID m = J::method(jni, cls_stack(jni), {"h", "getItemDamage", "func_77952_i"}, "()I", "()I");
    return call_int(jni, stack, m);
}

int stack_max_damage(events::JniContext* jni, jobject stack) noexcept {
    if (!available(jni)) {
        return 0;
    }
    static const jmethodID m = J::method(jni, cls_stack(jni), {"j", "getMaxDamage", "func_77958_k"}, "()I", "()I");
    return call_int(jni, stack, m);
}

std::string stack_display_name(events::JniContext* jni, jobject stack) {
    if (!available(jni)) {
        return {};
    }
    static const jmethodID m = J::method(jni, cls_stack(jni), {"q", "getDisplayName", "func_82833_r"},
                                         "()Ljava/lang/String;", "()Ljava/lang/String;");
    return call_string(jni, stack, m);
}

std::string stack_rarity_color(events::JniContext* jni, jobject stack) {
    if (!available(jni)) {
        return {};
    }
    static const jmethodID m = J::method(jni, cls_stack(jni), {"u", "getRarity", "func_77953_t"},
                                         "()Laaj;", "()Lnet/minecraft/item/EnumRarity;");
    static const jclass cls_rarity = J::find_class(jni, "aaj", "net/minecraft/item/EnumRarity");
    static const jfieldID f = J::field(jni, cls_rarity, {"e", "rarityColor", "field_77937_e"},
                                       "La;", "Lnet/minecraft/util/EnumChatFormatting;");
    if (f == nullptr) {
        return {};
    }
    jobject rarity = call_object(jni, stack, m);
    if (rarity == nullptr) {
        return {};
    }
    jobject color = jni->env->GetObjectField(rarity, f);
    std::string out = object_to_string(jni, color);
    drop(jni, color);
    drop(jni, rarity);
    return out;
}

namespace {

jmethodID m_get_tag(events::JniContext* jni) noexcept {
    static const jmethodID m = J::method(jni, cls_stack(jni), {"o", "getTagCompound", "func_77978_p"},
                                         "()Ldn;", "()Lnet/minecraft/nbt/NBTTagCompound;");
    return m;
}

jmethodID m_has_key_typed(events::JniContext* jni) noexcept {
    static const jmethodID m = J::method(jni, jni->mappings->cls_nbt_tag_compound, {"b", "hasKey", "func_150297_b"},
                                         "(Ljava/lang/String;I)Z", "(Ljava/lang/String;I)Z");
    return m;
}

bool compound_has_key(events::JniContext* jni, jobject compound, const char* key, int type) noexcept {
    jmethodID m = m_has_key_typed(jni);
    if (compound == nullptr || m == nullptr) {
        return false;
    }
    jstring jkey = jni->env->NewStringUTF(key);
    const bool result = jni->env->CallBooleanMethod(compound, m, jkey, type) != JNI_FALSE;
    drop(jni, jkey);
    return !J::clear_exception(jni) && result;
}

} // namespace

bool stack_has_display_tag(events::JniContext* jni, jobject stack) noexcept {
    if (!available(jni)) {
        return false;
    }
    jobject tag = call_object(jni, stack, m_get_tag(jni));
    const bool result = compound_has_key(jni, tag, "display", 10);
    drop(jni, tag);
    return result;
}

std::string stack_custom_name(events::JniContext* jni, jobject stack, bool* found) {
    *found = false;
    if (!available(jni)) {
        return {};
    }
    static const jmethodID m_compound = J::method(jni, jni->mappings->cls_nbt_tag_compound,
                                                  {"m", "getCompoundTag", "func_74775_l"},
                                                  "(Ljava/lang/String;)Ldn;",
                                                  "(Ljava/lang/String;)Lnet/minecraft/nbt/NBTTagCompound;");
    static const jmethodID m_string = J::method(jni, jni->mappings->cls_nbt_tag_compound, {"j", "getString", "func_74779_i"},
                                                "(Ljava/lang/String;)Ljava/lang/String;",
                                                "(Ljava/lang/String;)Ljava/lang/String;");
    if (m_compound == nullptr || m_string == nullptr) {
        return {};
    }
    std::string out;
    jobject tag = call_object(jni, stack, m_get_tag(jni));
    if (compound_has_key(jni, tag, "display", 10)) {
        jstring jdisplay = jni->env->NewStringUTF("display");
        jobject display = jni->env->CallObjectMethod(tag, m_compound, jdisplay);
        drop(jni, jdisplay);
        if (J::clear_exception(jni)) {
            drop(jni, display);
            display = nullptr;
        }
        if (compound_has_key(jni, display, "Name", 8)) {
            jstring jname = jni->env->NewStringUTF("Name");
            auto value = static_cast<jstring>(jni->env->CallObjectMethod(display, m_string, jname));
            drop(jni, jname);
            if (!J::clear_exception(jni)) {
                out = to_utf8(jni, value);
                *found = true;
            }
            drop(jni, value);
        }
        drop(jni, display);
    }
    drop(jni, tag);
    return out;
}

bool item_has_subtypes(events::JniContext* jni, jobject item) noexcept {
    if (!available(jni)) {
        return false;
    }
    static const jmethodID m = J::method(jni, jni->mappings->cls_item, {"k", "getHasSubtypes", "func_77614_k"}, "()Z", "()Z");
    return call_bool(jni, item, m);
}

std::string item_stack_display_name(events::JniContext* jni, jobject item, jobject stack) {
    if (!available(jni) || item == nullptr) {
        return {};
    }
    static const jmethodID m = J::method(jni, jni->mappings->cls_item, {"a", "getItemStackDisplayName", "func_77653_i"},
                                         "(Lzx;)Ljava/lang/String;", "(Lnet/minecraft/item/ItemStack;)Ljava/lang/String;");
    if (m == nullptr) {
        return {};
    }
    auto text = static_cast<jstring>(jni->env->CallObjectMethod(item, m, stack));
    if (J::clear_exception(jni)) {
        drop(jni, text);
        return {};
    }
    std::string out = to_utf8(jni, text);
    drop(jni, text);
    return out;
}

int item_tool_material_ordinal(events::JniContext* jni, jobject tool_item) noexcept {
    if (!available(jni)) {
        return 0;
    }
    static const jmethodID m = J::method(jni, lookup_class(jni, Cls::ItemTool), {"g", "getToolMaterial", "func_150913_i"},
                                         "()Lzw$a;", "()Lnet/minecraft/item/Item$ToolMaterial;");
    jobject material = call_object(jni, tool_item, m);
    if (material == nullptr) {
        return 0;
    }
    const int ordinal = minecraft::enum_get_ordinal(jni, material);
    J::clear_exception(jni);
    drop(jni, material);
    return ordinal;
}

// ---- Rendering ------------------------------------------------------------------------------------------------

math::Vec3d render_manager_viewer_pos(events::JniContext* jni, jobject render_manager) noexcept {
    if (!available(jni) || render_manager == nullptr) {
        return {};
    }
    jclass cls = jni->mappings->cls_render_manager;
    static const jfieldID fx = J::field(jni, cls, {"h", "viewerPosX", "field_78730_l"}, "D", "D");
    static const jfieldID fy = J::field(jni, cls, {"i", "viewerPosY", "field_78731_m"}, "D", "D");
    static const jfieldID fz = J::field(jni, cls, {"j", "viewerPosZ", "field_78728_n"}, "D", "D");
    return math::make_vec3(get_double(jni, render_manager, fx), get_double(jni, render_manager, fy),
                           get_double(jni, render_manager, fz));
}

jobject renderer_main_model(events::JniContext* jni, jobject living_renderer) noexcept {
    if (!available(jni)) {
        return nullptr;
    }
    static const jmethodID m = J::method(jni, jni->mappings->cls_renderer_living_entity,
                                         {"b", "getMainModel", "func_177087_b"},
                                         "()Lbbo;", "()Lnet/minecraft/client/model/ModelBase;");
    return call_object(jni, living_renderer, m);
}

bool bind_inventory_background(events::JniContext* jni, jobject texture_manager) noexcept {
    if (!available(jni) || texture_manager == nullptr) {
        return false;
    }
    static const jclass cls_container = J::find_class(jni, "ayl", "net/minecraft/client/gui/inventory/GuiContainer");
    static const jfieldID f_background = J::static_field(jni, cls_container, {"a", "inventoryBackground", "field_147001_a"},
                                                         "Ljy;", "Lnet/minecraft/util/ResourceLocation;");
    static const jclass cls_textures = J::find_class(jni, "bmj", "net/minecraft/client/renderer/texture/TextureManager");
    static const jmethodID m_bind = J::method(jni, cls_textures, {"a", "bindTexture", "func_110577_a"},
                                              "(Ljy;)V", "(Lnet/minecraft/util/ResourceLocation;)V");
    if (f_background == nullptr || m_bind == nullptr) {
        return false;
    }
    jobject location = jni->env->GetStaticObjectField(cls_container, f_background);
    if (J::clear_exception(jni) || location == nullptr) {
        drop(jni, location);
        return false;
    }
    jni->env->CallVoidMethod(texture_manager, m_bind, location);
    drop(jni, location);
    return !J::clear_exception(jni);
}

} // namespace slinky::modules::adjust_render
