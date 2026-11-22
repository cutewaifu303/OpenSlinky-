// Port of com.adjustclient.module.impl.render.hud.target.{TargetHud, TargetHudEntry} and the target tracking of
// HUDModule (onRender2D position, onPacket, onTick).
//
// Boxes start at the screen centre + (18, 18), two per row (1 unit apart): skin face (hurt/death tinted red),
// name (Tahoma 12), the health difference to the local player ("+n" / "-n"), held item + armor at half size and
// an animated health bar (health, slow "trail" and absorption, themed with the HUD colour / secondary / rainbow).
//
// Tracking:
//   - own attacks set the target: outgoing C02PacketUseEntity(ATTACK) on a player that is not a bot, seen on the
//     main-thread send hook (kEventMainThreadPacket = Adjust's outgoing DirectionalPacketEvent). Fallback when the
//     packet fields cannot be resolved: kEventPreAttackOrUseEntity + Minecraft.objectMouseOver;
//   - while the chat is open the local player is shown;
//   - an entry fades out 20 ticks after the last hit, when the target dies or is further than 12 blocks;
//   - join game / respawn / teleport over 32 blocks clear the list. The HUD module does not receive incoming
//     packets, so these are detected in the tick: new world or new local player object (S01 / S07) and a
//     position jump of more than 32 blocks between two ticks (S08).
// Not ported: KillAura target (no such module in OpenSlinky++), Adjust client tags of other users (backend),
// Hypixel nick aliases (PlayerDetect) and Streamer-mode name replacement.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "config/settings.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/hud/target/target_textures.hpp"

namespace slinky::modules::adjust_hud {

namespace {

namespace jl = adjust::jni;
using adjust::Animation;
using adjust::Argb;
using adjust::Easing;

struct Settings {
    config::BoolSetting enabled{{}, "Enabled", true, true, {}};
};

Settings g_settings{};

// ---- TargetHudEntry ------------------------------------------------------------------------------------------

struct ItemSlot {
    bool present = false;   // stack != null (advances the row even if the sprite is unavailable)
    target::Sprite sprite{};
};

struct Entry {
    Animation fade{Easing::EaseOutExpo, 300};
    jobject target = nullptr; // global ref to the EntityPlayer
    uint32_t skin = 0;
    ItemSlot items[5]{};
    bool has_health_anim = false;
    Animation health_anim{};
    Animation trail_anim{};
    bool has_absorption_anim = false;
    Animation absorption_anim{};
    std::string name;
    float min_width = 80.0f;
    float width = 80.0f;
    float height = 24.0f;
    float health_ratio = 0.0f;
    float absorption_ratio = 0.0f;
    float health_change = 0.0f;
    int ticks = 0;
    bool fading_out = false;
    // hurt / death state for the head tint (previousHurtTime / hurtTime sampled per tick)
    int prev_hurt_time = 0;
    int hurt_time = 0;
    int max_hurt_time = 10;
    int death_time = 0;
    bool alive = true; // health > 0
};

using EntryPtr = std::unique_ptr<Entry>;

std::mutex g_mutex;
std::vector<EntryPtr> g_targets; // front = most recent target (addFirst)
JavaVM* g_vm = nullptr;

// Reset detection (join / respawn / teleport)
jweak g_last_world = nullptr;
jweak g_last_player = nullptr;
math::Vec3d g_last_pos{};
bool g_has_last_pos = false;

void remember_vm(JNIEnv* env) noexcept {
    if (g_vm == nullptr && env != nullptr) {
        env->GetJavaVM(&g_vm);
    }
}

void release_entry(JNIEnv* env, Entry& entry) noexcept {
    if (entry.target != nullptr && env != nullptr) {
        env->DeleteGlobalRef(entry.target);
    }
    entry.target = nullptr;
}

// TargetHud.clear (caller holds the lock)
void clear_locked(JNIEnv* env) noexcept {
    for (EntryPtr& e : g_targets) {
        release_entry(env, *e);
    }
    g_targets.clear();
}

std::string java_string(JNIEnv* env, jstring str) {
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

bool chat_open(events::JniContext* jni, jobject minecraft) noexcept {
    if (jni == nullptr || minecraft == nullptr) {
        return false;
    }
    static const jclass s_gui_chat = jl::find_class(jni, "awv", "net/minecraft/client/gui/GuiChat");
    const jfieldID fid = jni->mappings->fid_mc_current_screen;
    if (s_gui_chat == nullptr || fid == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    jobject screen = env->GetObjectField(minecraft, fid);
    const bool open = screen != nullptr && env->IsInstanceOf(screen, s_gui_chat);
    if (screen != nullptr) {
        env->DeleteLocalRef(screen);
    }
    return open;
}

// Samples previousHurtTime / hurtTime / maxHurtTime / deathTime and the alive state for the head tint.
void sample_hurt(events::JniContext* jni, Entry& entry) noexcept {
    static const jclass s_living = jl::find_class(jni, "pr", "net/minecraft/entity/EntityLivingBase");
    static const jfieldID s_max_hurt = jl::field(jni, s_living, {"av", "maxHurtTime", "field_70738_aO"}, "I", "I");
    static const jfieldID s_death = jl::field(jni, s_living, {"ax", "deathTime", "field_70725_aQ"}, "I", "I");
    JNIEnv* env = jni->env;
    entry.prev_hurt_time = entry.hurt_time;
    entry.hurt_time = minecraft::living_get_hurt_time(jni, entry.target);
    entry.max_hurt_time = s_max_hurt != nullptr ? env->GetIntField(entry.target, s_max_hurt) : 10;
    entry.death_time = s_death != nullptr ? env->GetIntField(entry.target, s_death) : 0;
    entry.alive = minecraft::living_get_health(jni, entry.target) > 0.0f;
    jl::clear_exception(jni);
}

// TargetHudEntry.update
void update_entry(events::JniContext* jni, jobject minecraft, jobject self, Entry& entry) noexcept {
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(32) != 0) {
        jl::clear_exception(jni);
        return;
    }
    jobject target = entry.target;
    entry.height = 24.0f;

    // held item, then armor helmet .. boots (inventory.armorInventory[3..0] = equipment slots 4..1)
    static constexpr int kSlots[5] = {0, 4, 3, 2, 1};
    for (int i = 0; i < 5; ++i) {
        jobject stack = minecraft::player_get_equipment_in_slot(jni, target, kSlots[i]);
        jl::clear_exception(jni);
        entry.items[i].present = stack != nullptr;
        entry.items[i].sprite = target::resolve_item_sprite(jni, minecraft, stack);
        if (stack != nullptr) {
            env->DeleteLocalRef(stack);
        }
    }
    entry.skin = target::resolve_skin_texture(jni, minecraft, target);

    const float max_health = minecraft::living_get_max_health(jni, target);
    const float health = minecraft::living_get_health(jni, target);
    const float shown_health = std::min(health, max_health);
    entry.health_ratio = max_health > 0.0f ? shown_health / max_health : 0.0f;
    if (!entry.has_health_anim) {
        entry.health_anim = Animation(Easing::EaseOutExpo, 220);
        entry.health_anim.value = entry.health_ratio; // starts at the current value
        entry.trail_anim = Animation(Easing::EaseOutExpo, 1660);
        entry.trail_anim.value = entry.health_ratio;
        entry.has_health_anim = true;
    }
    // absorption (DataWatcher 17)
    const float absorption = minecraft::player_get_absorption_amount(jni, target);
    const float shown_absorption = std::min(absorption, max_health);
    if (shown_absorption > 0.0f && max_health > 0.0f) {
        entry.absorption_ratio = shown_absorption / max_health;
        if (!entry.has_absorption_anim) {
            entry.absorption_anim = Animation(Easing::EaseOutExpo, 220);
            entry.absorption_anim.value = entry.absorption_ratio;
            entry.has_absorption_anim = true;
        }
    } else {
        entry.has_absorption_anim = false;
        entry.absorption_ratio = 0.0f;
    }

    // display name: the profile name (EntityPlayer.getName)
    try {
        entry.name = java_string(env, minecraft::entity_get_name(jni, target));
    } catch (...) {
    }
    jl::clear_exception(jni);

    // "+n" / "-n": (own absorption + own health) - (target absorption + target health)
    if (self != nullptr) {
        const float own_total = minecraft::player_get_absorption_amount(jni, self) + minecraft::living_get_health(jni, self);
        entry.health_change = own_total - (absorption + health);
    }

    // box width: name + 21 (the width is replaced whenever it reaches the 80 unit minimum)
    const float name_width = adjust::kTahoma12.width(entry.name) + 21.0f;
    if (name_width >= entry.min_width) {
        entry.width = name_width;
    }
    jl::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

// TargetHud.setTarget (caller holds the lock)
void set_target_locked(events::JniContext* jni, jobject minecraft, jobject entity, int ticks) noexcept {
    JNIEnv* env = jni->env;
    for (EntryPtr& e : g_targets) {
        if (env->IsSameObject(e->target, entity)) {
            e->fading_out = false;
            e->ticks = ticks;
            return;
        }
    }
    try {
        auto entry = std::make_unique<Entry>();
        entry->target = env->NewGlobalRef(entity);
        if (entry->target == nullptr) {
            return;
        }
        entry->ticks = ticks;
        entry->fading_out = false;
        jobject self = minecraft::mc_get_player(jni, minecraft);
        sample_hurt(jni, *entry);
        update_entry(jni, minecraft, self, *entry);
        if (self != nullptr) {
            env->DeleteLocalRef(self);
        }
        g_targets.insert(g_targets.begin(), std::move(entry));
    } catch (...) {
    }
}

// HUDModule.onPacket (outgoing part): C02PacketUseEntity with action ATTACK on a non-bot player.
void handle_attack_entity(events::JniContext* jni, jobject entity) noexcept {
    if (entity == nullptr) {
        return;
    }
    jobject player = minecraft::as_entity_player(jni, entity);
    if (player == nullptr || minecraft::player_has_tag_flags(jni, player, minecraft::kPlayerTagBot)) {
        return;
    }
    jobject mc = minecraft::mc_get_instance(jni);
    if (mc == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    set_target_locked(jni, mc, player, 0);
}

struct C02Fields {
    jfieldID entity_id = nullptr;
    jfieldID action = nullptr;
};

const C02Fields& c02_fields(events::JniContext* jni) noexcept {
    static const C02Fields s_fields = [&]() noexcept {
        C02Fields f{};
        const jclass cls = jni->mappings->cls_c02_packet_use_entity;
        f.entity_id = jl::field(jni, cls, {"a", "entityId", "field_149567_a"}, "I", "I");
        f.action = jl::field(jni, cls, {"b", "action", "field_149566_b"}, "Lin$a;",
                             "Lnet/minecraft/network/play/client/C02PacketUseEntity$Action;");
        return f;
    }();
    return s_fields;
}

void on_main_thread_packet(events::MainThreadPacketEvent* ev) noexcept {
    if (!g_settings.enabled.value || ev == nullptr || ev->cancelled || ev->jni == nullptr || ev->packet == nullptr) {
        return;
    }
    events::JniContext* jni = ev->jni;
    if (minecraft::as_packet_c02_use_entity(jni, ev->packet) == nullptr) {
        return;
    }
    const C02Fields& f = c02_fields(jni);
    if (f.entity_id == nullptr || f.action == nullptr) {
        return; // fallback: on_attack
    }
    JNIEnv* env = jni->env;
    remember_vm(env);
    if (env->PushLocalFrame(16) != 0) {
        jl::clear_exception(jni);
        return;
    }
    jobject action = env->GetObjectField(ev->packet, f.action);
    if (action != nullptr && minecraft::enum_get_ordinal(jni, action) == 1) { // Action.ATTACK
        const jint id = env->GetIntField(ev->packet, f.entity_id);
        jobject mc = minecraft::mc_get_instance(jni);
        jobject world = mc != nullptr ? minecraft::mc_get_world(jni, mc) : nullptr;
        jobject entity = world != nullptr ? minecraft::world_get_entity_by_id(jni, world, id) : nullptr;
        jl::clear_exception(jni);
        handle_attack_entity(jni, entity);
    }
    jl::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

// Fallback when the C02 fields are unavailable (e.g. 1.7.10): Minecraft.clickMouse attacks objectMouseOver.
void on_attack(events::AttackOrUseEntityEvent* ev) noexcept {
    if (!g_settings.enabled.value || ev == nullptr || !ev->is_attack || ev->cancelled || ev->ctx == nullptr ||
        ev->ctx->jni == nullptr || ev->ctx->minecraft == nullptr) {
        return;
    }
    events::JniContext* jni = ev->ctx->jni;
    const C02Fields& f = c02_fields(jni);
    if (f.entity_id != nullptr && f.action != nullptr) {
        return; // the packet path handles it
    }
    remember_vm(jni->env);
    if (minecraft::mc_get_left_click_counter(jni, ev->ctx->minecraft) > 0) {
        return;
    }
    jobject mop = minecraft::mc_get_object_mouse_over(jni, ev->ctx->minecraft);
    if (mop == nullptr || minecraft::mop_get_type_ordinal(jni, mop) != 2) {
        return;
    }
    handle_attack_entity(jni, minecraft::mop_get_entity_hit(jni, mop));
}

// Join game / respawn (new world or new local player object) and teleports over 32 blocks clear the targets.
bool detect_reset(events::GameContext* ctx) noexcept {
    JNIEnv* env = ctx->jni->env;
    bool reset = false;
    if (g_last_world == nullptr || !env->IsSameObject(g_last_world, ctx->world)) {
        reset = g_last_world != nullptr;
        if (g_last_world != nullptr) env->DeleteWeakGlobalRef(g_last_world);
        g_last_world = env->NewWeakGlobalRef(ctx->world);
    }
    if (g_last_player == nullptr || !env->IsSameObject(g_last_player, ctx->local_player)) {
        reset = reset || g_last_player != nullptr;
        if (g_last_player != nullptr) env->DeleteWeakGlobalRef(g_last_player);
        g_last_player = env->NewWeakGlobalRef(ctx->local_player);
        g_has_last_pos = false;
    }
    const math::Vec3d pos = minecraft::entity_get_position(ctx->jni, ctx->local_player);
    if (g_has_last_pos) {
        const double dx = pos.x - g_last_pos.x;
        const double dy = pos.y - g_last_pos.y;
        const double dz = pos.z - g_last_pos.z;
        if (dx * dx + dy * dy + dz * dz > 1024.0) {
            reset = true;
        }
    }
    g_last_pos = pos;
    g_has_last_pos = true;
    return reset;
}

// TargetHud.tick
void tick(events::GameContext* ctx) noexcept {
    if (ctx == nullptr || ctx->jni == nullptr || ctx->minecraft == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    remember_vm(env);
    target::trim_image_cache();

    std::lock_guard<std::mutex> lock(g_mutex);
    if (detect_reset(ctx)) {
        clear_locked(env);
    }
    if (!g_settings.enabled.value) {
        clear_locked(env);
        return;
    }
    const bool chat = chat_open(jni, ctx->minecraft);
    if (chat) {
        set_target_locked(jni, ctx->minecraft, ctx->local_player, 69);
    }

    const math::Vec3d self_pos = minecraft::entity_get_position(jni, ctx->local_player);
    for (auto it = g_targets.begin(); it != g_targets.end();) {
        Entry& entry = **it;
        sample_hurt(jni, entry);
        if (!entry.fading_out) {
            if (!minecraft::entity_is_dead(jni, entry.target) && entry.alive) {
                const math::Vec3d pos = minecraft::entity_get_position(jni, entry.target);
                const double dx = self_pos.x - pos.x;
                const double dy = self_pos.y - pos.y;
                const double dz = self_pos.z - pos.z;
                if (dx * dx + dy * dy + dz * dz <= 144.0) {
                    ++entry.ticks;
                    // after 20 ticks only the local player stays (while the chat is open)
                    const bool is_self = env->IsSameObject(entry.target, ctx->local_player);
                    if (entry.ticks < 21 || (is_self && chat)) {
                        update_entry(jni, ctx->minecraft, ctx->local_player, entry);
                        ++it;
                        continue;
                    }
                }
            }
        }
        // invalid / too far / expired: fade out, then remove
        if (entry.fading_out) {
            if (entry.fade.finished) {
                release_entry(env, entry);
                it = g_targets.erase(it);
                continue;
            }
        } else {
            entry.fading_out = true;
        }
        ++it;
    }
    jl::clear_exception(jni);
}

// ---- rendering (TargetHud.render) -------------------------------------------------------------------------------

int clamp255(int value) noexcept {
    return value < 0 ? 0 : std::min(value, 255);
}

// RandomUtil.format: integers without decimals, otherwise rounded half-up to one decimal.
std::string format_number(double value) {
    char buf[32];
    if (static_cast<double>(static_cast<int>(value)) == value) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(value));
    } else {
        const double rounded = std::floor(value * 10.0 + 0.5) / 10.0;
        std::snprintf(buf, sizeof(buf), "%.1f", rounded);
    }
    return buf;
}

// RenderUtil.drawThemedRect
void draw_themed_rect(Argb color, float x1, float y1, float x2, float y2, float blend) noexcept {
    if (secondary_active()) {
        const Argb other = adjust::blend(secondary_for(color), color, blend * 0.85f);
        adjust::fill_gradient_rect(color, other, x1, y1, x2, y2, x1, y1, x2, y1);
    } else {
        adjust::fill_rect(color, x1, y1, x2, y2);
    }
}

// TargetHud.fillGradientBar (rainbow HUD colour): four gradients, hue = (time % 5000 + offset) / 5000.
void fill_gradient_bar(int alpha, float brightness, float left, float top, float bottom, float span) noexcept {
    static constexpr int kOffsets[5] = {5000, 4700, 4400, 4100, 3800};
    const float time = static_cast<float>(adjust::now_ms() % 5000);
    for (int i = 0; i < 4; ++i) {
        const float start = time + static_cast<float>(kOffsets[i]);
        const float end = time + static_cast<float>(kOffsets[i + 1]);
        const Argb start_color = (adjust::hsb_to_rgb(start / 5000.0f, 0.45f, brightness) & 0xFFFFFFu) |
                                 (static_cast<Argb>(alpha) << 24);
        const Argb end_color = (adjust::hsb_to_rgb(end / 5000.0f, 0.45f, brightness) & 0xFFFFFFu) |
                               (static_cast<Argb>(alpha) << 24);
        adjust::fill_gradient_rect(start_color, end_color, left + span * (static_cast<float>(i) * 0.25f), top,
                                   left + span * (static_cast<float>(i + 1) * 0.25f), bottom);
    }
}

// TargetHud.drawHead: skin face tinted towards (237, 60, 60) while hurt / dying.
void draw_head(const Entry& entry, int alpha, float partial_ticks, float x, float y) noexcept {
    const float interpolated = static_cast<float>(entry.prev_hurt_time) +
                               static_cast<float>(entry.hurt_time - entry.prev_hurt_time) * partial_ticks;
    float numerator = 0.0f;
    float denominator = 1.0f;
    if (entry.alive) {
        numerator = std::min(interpolated, static_cast<float>(entry.max_hurt_time));
        denominator = static_cast<float>(entry.max_hurt_time) * 2.15f;
    } else {
        numerator = std::min(static_cast<float>(std::max(entry.death_time, 0)), 20.0f);
        denominator = 43.0f;
    }
    const float ratio = denominator != 0.0f ? numerator / denominator : 0.0f;
    const float t = ratio < 0.0f ? 0.0f : std::min(ratio, 1.0f);
    const int r = static_cast<int>(t * 237.0f + 255.0f * (1.0f - t));
    const int g = static_cast<int>(255.0f * (1.0f - t) + t * 60.0f);
    const int b = static_cast<int>(255.0f * (1.0f - t) + t * 60.0f);
    target::draw_head(entry.skin, x + 2.0f, y + 2.0f, adjust::argb(r, g, b, alpha));
}

// TargetHud.drawItems: present stacks in a row, 8 units apart, at half size.
void draw_items(const Entry& entry, int alpha, float x, float y) noexcept {
    float offset = 0.0f;
    for (const ItemSlot& slot : entry.items) {
        if (!slot.present) {
            continue;
        }
        target::draw_item(slot.sprite, offset + x, y, 8.0f, alpha);
        offset += 8.0f;
    }
}

void render(const HudContext& ctx) noexcept {
    if (!g_settings.enabled.value || ctx.frame == nullptr) {
        return;
    }
    float partial_ticks = 1.0f;
    if (ctx.game != nullptr && ctx.game->jni != nullptr && ctx.game->minecraft != nullptr) {
        jobject timer = minecraft::mc_get_timer(ctx.game->jni, ctx.game->minecraft);
        if (timer != nullptr) {
            partial_ticks = minecraft::timer_get_render_partial_ticks(ctx.game->jni, timer);
            ctx.game->jni->env->DeleteLocalRef(timer);
        }
    }

    // HUDModule.onRender2D: centre + (18, 18), HUD colour
    const float start_x = static_cast<float>(ctx.frame->scaled_width) * 0.5f + 18.0f;
    const float start_y = static_cast<float>(ctx.frame->scaled_height) * 0.5f + 18.0f;
    const Argb hud = hud_color();

    std::lock_guard<std::mutex> lock(g_mutex);
    float x = start_x;
    float y = start_y;
    int column = 0;
    for (EntryPtr& ptr : g_targets) {
        Entry& entry = *ptr;
        Animation& fade = entry.fade;
        if (entry.fading_out) {
            fade.duration = 250;
            fade.update(0.0f);
        } else {
            fade.update(1.0f);
        }
        const float fade_value = fade.value;
        const int alpha = clamp255(static_cast<int>(fade_value * 255.0f));
        const Argb color = adjust::alpha_of(hud) != alpha ? adjust::with_alpha(hud, alpha) : hud;
        const int bg_alpha = clamp255(static_cast<int>(fade_value * 170.0f));
        const float width = entry.width;
        const float height = entry.height;
        const float right = x + width;
        const float bottom = y + height;
        const Argb bg_color = (static_cast<Argb>(bg_alpha) << 24) | 0x131313u;
        adjust::fill_rect(bg_color, x, y, right, bottom);

        entry.health_anim.update(entry.health_ratio);
        entry.trail_anim.update(entry.health_ratio);

        // health bar: 3 units high at the bottom of the box, 1.5 inset left and right
        adjust::push_even_scale();
        const math::Vec4f bar = adjust::to_pixels(x + 1.5f, bottom - 4.5f, right - 1.5f, bottom - 1.5f);
        adjust::fill_rect((static_cast<Argb>(alpha >> 1) << 24) | 0x27272Au, bar.x, bar.y, bar.w, bar.h); // border
        const float factor = adjust::even_scale_factor();
        const float thickness = factor == 0.0f ? 0.5f : static_cast<float>(static_cast<int>(factor)) * 0.5f;
        const float inner_left = bar.x + thickness;
        const float inner_top = bar.y + thickness;
        const float inner_right = bar.w - thickness;
        const float inner_bottom = bar.h - thickness;
        adjust::fill_rect(bg_color, inner_left, inner_top, inner_right, inner_bottom);
        const float trail_value = entry.trail_anim.value;
        const float health_value = entry.health_anim.value;
        const float health_right = inner_left + ((health_value * (inner_right - bar.x)) - thickness);
        const float trail_right = ((trail_value * (inner_right - bar.x)) - thickness) + inner_left;
        if (hud_color_rainbow()) {
            // dark trail, then the bright health part
            fill_gradient_bar(alpha, 0.504f, inner_left, inner_top, inner_bottom, trail_right - inner_left);
            fill_gradient_bar(alpha, 0.8f, inner_left, inner_top, inner_bottom, health_right - inner_left);
        } else {
            // the HUD colour darkened to 63 %
            const Argb dark = adjust::argb(static_cast<int>(adjust::red_of(color) * 0.63f),
                                           static_cast<int>(adjust::green_of(color) * 0.63f),
                                           static_cast<int>(adjust::blue_of(color) * 0.63f), adjust::alpha_of(color));
            draw_themed_rect(dark, inner_left, inner_top, trail_right, inner_bottom, entry.health_ratio);
            draw_themed_rect(color, inner_left, inner_top, health_right, inner_bottom, entry.health_ratio);
        }
        if (entry.has_absorption_anim) {
            entry.absorption_anim.update(entry.absorption_ratio);
            const float absorption_right =
                (((inner_right - bar.x) * entry.absorption_anim.value) - thickness) + inner_left;
            const Argb a24 = color & 0xFF000000u;
            if (adjust::red_of(color) != 0 || (color & 0xFFFFu) != 0) {
                // brighter variant of the HUD colour: every channel / 0.45, clamped to 255
                const int r = std::min(static_cast<int>(static_cast<float>(adjust::red_of(color)) / 0.45f), 255);
                const int g = std::min(static_cast<int>(static_cast<float>(adjust::green_of(color)) / 0.45f), 255);
                const int b = std::min(static_cast<int>(static_cast<float>(adjust::blue_of(color)) / 0.45f), 255);
                draw_themed_rect(a24 | (static_cast<Argb>(r) << 16) | (static_cast<Argb>(g) << 8) | static_cast<Argb>(b),
                                 inner_left, inner_top, absorption_right, inner_bottom, entry.absorption_ratio);
            } else {
                draw_themed_rect(a24 | 0x010101u, inner_left, inner_top, absorption_right, inner_bottom,
                                 entry.absorption_ratio); // pure black HUD colour
            }
        }
        adjust::pop_even_scale();

        // text
        const int text_alpha = clamp255(static_cast<int>(fade_value * 220.0f));
        const Argb text_color = (static_cast<Argb>(text_alpha) << 24) | 0xE6E6ECu;
        adjust::kTahoma12.draw(entry.name, x + 19.5f, y + 3.5f, text_color);
        const float change = entry.health_change;
        if (change != 0.0f) {
            try {
                const char* sign = change > 0.0f ? "+" : "-";
                const std::string number = format_number(static_cast<double>(std::fabs(change)));
                const float number_x = (right - 2.0f) - adjust::kTahoma10.width(number);
                adjust::kTahoma8.draw(sign, number_x - adjust::kTahoma8.width(sign), y + 16.5f, text_color);
                adjust::kTahoma10.draw(number, number_x, y + 16.0f, text_color);
            } catch (...) {
            }
        }

        // head and items were post tasks (drawn on top) in Adjust
        draw_head(entry, alpha, partial_ticks, x, y);
        draw_items(entry, alpha, x + 20.0f, y + 10.0f);

        // two boxes per row
        if (column + 1 >= 2) {
            column = 0;
            y = (height + 1.0f) + y;
            x = start_x;
        } else {
            x = (width + 1.0f) + x;
            ++column;
        }
    }
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_settings.enabled.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    g_settings.enabled.visit(ar, "target_enabled");
}

void on_client_shutdown() noexcept {
    JNIEnv* env = nullptr;
    if (g_vm != nullptr && g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        env = nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_locked(env); // without an env the global refs are dropped (process is unloading anyway)
    if (env != nullptr) {
        if (g_last_world != nullptr) env->DeleteWeakGlobalRef(g_last_world);
        if (g_last_player != nullptr) env->DeleteWeakGlobalRef(g_last_player);
    }
    g_last_world = nullptr;
    g_last_player = nullptr;
    g_has_last_pos = false;
    target::reset_image_cache();
}

} // namespace

const HudElement& element_target() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Target HUD";
        e.render_settings = render_settings;
        e.visit_config = visit_config;
        e.tick = tick;
        e.render = render;
        e.on_attack = on_attack;
        e.on_main_thread_packet = on_main_thread_packet;
        e.on_client_shutdown = on_client_shutdown;
        return e;
    }();
    return s_element;
}

} // namespace slinky::modules::adjust_hud
