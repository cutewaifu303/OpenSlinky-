#include "modules/adjust/render/esp/adjust_esp.hpp"
#include "modules/adjust/render/item_physics/item_physics.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glad/glad.h>

#include "adjust/kit.hpp"
#include "core/client.hpp"
#include "events/module_manager.hpp"
#include "gui/gui_renderer.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/esp/adjust_render_jni.hpp"
#include "modules/adjust/render/esp/esp_item_colors.hpp"
#include "modules/adjust/render/esp/esp_settings.hpp"
#include "nanovg.h"

namespace slinky::modules {

namespace {

using namespace adjust_esp;
namespace R = adjust_render;
using adjust::Argb;

#define ESP_FMT ADJUST_FMT       // "§"
constexpr const char* kHeart = "\xE2\x9D\xA4"; // "❤"

// =================================================================================================================
// Small Java-equivalent helpers
// =================================================================================================================

// StringUtils.stripControlCodes: removes "§x" for x in [0-9a-fk-or] (case-insensitive).
std::string strip_codes(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xC2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            const char c = static_cast<char>(text[i + 2] | 0x20);
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'k' && c <= 'o') || c == 'r') {
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

// String.trim()
std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ') {
        ++begin;
    }
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') {
        --end;
    }
    return text.substr(begin, end - begin);
}

// RandomUtil.format: integral values without decimals, otherwise rounded (half up) to one decimal.
std::string format_number(double value) {
    char buf[32];
    if (static_cast<double>(static_cast<int>(value)) == value) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(value));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", std::floor(value * 10.0 + 0.5) / 10.0);
    }
    return buf;
}

// TimeUtil.formatDuration(millis, true)
std::string format_duration(long long millis) {
    char buf[64];
    if (millis <= 0) {
        return "0.0s";
    }
    if (millis < 60000) {
        std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(millis) / 1000.0);
        return buf;
    }
    const long long seconds = millis % 60000 / 1000;
    if (millis < 3600000) {
        std::snprintf(buf, sizeof(buf), "%lldm %llds", millis / 60000, seconds);
        return buf;
    }
    const long long minutes = millis % 3600000 / 60000;
    if (millis < 86400000) {
        std::snprintf(buf, sizeof(buf), "%lldh %lldm %llds", millis / 3600000, minutes, seconds);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%lldd %lldh %lldm %llds", millis / 86400000, millis % 86400000 / 3600000, minutes,
                  seconds);
    return buf;
}

// StringUtil.toRoman
std::string to_roman(int number) {
    static constexpr int kValues[13] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static constexpr const char* kSymbols[13] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
    std::string out;
    for (int i = 0; i < 13; ++i) {
        while (number >= kValues[i]) {
            out += kSymbols[i];
            number -= kValues[i];
        }
    }
    return out;
}

// FontRenderer.colorCode for "0123456789abcdef".
bool color_code_rgb(char code, Argb* out) noexcept {
    static constexpr Argb kCodes[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xFFAA00, 0xAAAAAA,
                                        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
    const char c = static_cast<char>(code | 0x20);
    int index = -1;
    if (c >= '0' && c <= '9') {
        index = c - '0';
    } else if (c >= 'a' && c <= 'f') {
        index = 10 + (c - 'a');
    }
    if (index < 0) {
        return false;
    }
    *out = 0xFF000000u | kCodes[index];
    return true;
}

// WorldUtil.getTeamColor(name): colour of the second character of the scoreboard team prefix (null -> false).
bool team_color(events::JniContext* jni, jobject entity, Argb* out) noexcept {
    jobject team = minecraft::living_get_team(jni, entity);
    if (team == nullptr) {
        R::drop(jni, team);
        return false;
    }
    bool found = false;
    jobject score_team = minecraft::as_score_player_team(jni, team);
    if (score_team != nullptr) {
        jstring prefix = minecraft::score_player_team_get_color_prefix(jni, score_team);
        if (prefix != nullptr && jni->env->GetStringLength(prefix) >= 2) {
            jchar code = 0;
            jni->env->GetStringRegion(prefix, 1, 1, &code);
            found = code < 0x80 && color_code_rgb(static_cast<char>(code), out);
        }
        R::drop(jni, prefix);
        if (score_team != team) {
            R::drop(jni, score_team);
        }
    }
    R::drop(jni, team);
    jni->env->ExceptionClear();
    return found;
}

int64_t now_ms() noexcept {
    return adjust::now_ms();
}

// =================================================================================================================
// Entity tracker (com.adjustclient.util.anticheat.entity.EntityTracker, ESP-relevant part)
// =================================================================================================================

enum class Kind : uint8_t { Player, Hostile, Passive, Item, Tnt };

struct Tracked {
    jobject entity = nullptr; // global reference
    int id = 0;
    Kind kind = Kind::Passive;
    bool removed = false;
    bool seen_in_frustum = false;
    int64_t removed_at = 0;
    int retention = 15000;
    uint64_t seen_tick = 0;
    // TrackedTnt
    int fuse = 0;
    bool has_fuse_text = false;
    std::string fuse_text;
};

std::unordered_map<int, std::unique_ptr<Tracked>> g_tracked;
jobject g_world = nullptr; // global reference of the world the tracker belongs to
uint64_t g_tick = 0;

// =================================================================================================================
// Renderers (EntityEspRenderer / ItemPoolEspRenderer)
// =================================================================================================================

struct IconRow {               // EspIconEntry
    std::vector<jobject> stacks; // global references (nullptr = empty slot)
    float advance = 0.0f;
};

struct PotionEntry {           // EspPotionEntry
    int icon_index = 0;
    std::string text;
    float width = 0.0f;
};

struct PoolLine {              // ItemPoolLine
    std::string text;
    Argb color = 0;
    float width = 0.0f;
};

struct Renderer {
    bool pooled = false;
    Tracked* target = nullptr;            // entity renderer
    std::vector<Tracked*> pool;           // pooled items
    Group* settings = nullptr;
    bool initialized = false;
    double sort_key = 0.0;

    // EntityEspRenderer state (filled by update())
    float text_scale = 0.0f;
    float half_width = 0.0f;              // item name scale * 0.5
    bool has_name = false;
    std::string display_name;
    float display_name_width = 0.0f;
    bool has_health = false;
    std::string health_text;
    float health_text_width = 0.0f;       // field96
    bool has_absorption = false;
    std::string absorption_text;
    float absorption_width = 0.0f;        // distanceTextWidth (absorption width + 2)
    bool has_distance = false;
    std::string distance_text;
    float distance_width = 0.0f;          // statusTextWidth
    std::vector<int> flags;
    bool has_flags = false;
    std::vector<IconRow> equipment;
    bool has_potions = false;
    std::vector<PotionEntry> potions;
    float potion_width = 0.0f;
    bool has_held_item = false;
    std::string held_item_name;
    float held_item_width = 0.0f;         // nameWidth
    float health_hue = 0.0f;
    float health_ratio = 0.0f;            // absorption / max health
    float bar_ratio = 0.0f;
    Argb name_color = 0;
    Argb bar_color = 0;
    bool health_color_cached = false;

    // ItemPoolEspRenderer state
    float pool_scale = 0.0f;
    std::vector<PoolLine> lines;
};

std::vector<Renderer> g_renderers;

void release_renderers(JNIEnv* env) noexcept {
    for (Renderer& r : g_renderers) {
        for (IconRow& row : r.equipment) {
            for (jobject stack : row.stacks) {
                if (stack != nullptr && env != nullptr) {
                    env->DeleteGlobalRef(stack);
                }
            }
        }
    }
    g_renderers.clear();
}

void release_tracker(JNIEnv* env) noexcept {
    release_renderers(env);
    if (env != nullptr) {
        for (auto& [id, tracked] : g_tracked) {
            if (tracked->entity != nullptr) {
                env->DeleteGlobalRef(tracked->entity);
            }
        }
        if (g_world != nullptr) {
            env->DeleteGlobalRef(g_world);
        }
    }
    g_tracked.clear();
    g_world = nullptr;
}

// EntityTracker.update / removeStale for the entity classes ESP renders.
void update_tracker(events::GameContext* ctx) noexcept {
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    ++g_tick;

    jobject list = minecraft::world_get_loaded_entity_list(jni, ctx->world);
    const int32_t count = list != nullptr ? minecraft::java_list_size(jni, list) : 0;
    for (int32_t i = 0; i < count; ++i) {
        if (env->PushLocalFrame(16) != 0) {
            env->ExceptionClear();
            break;
        }
        jobject entity = minecraft::java_list_get(jni, list, i);
        do {
            if (entity == nullptr || R::is_instance(jni, entity, R::Cls::EntityArmorStand)) {
                break;
            }
            Kind kind;
            if (R::is_instance(jni, entity, R::Cls::EntityPlayer)) {
                if ((minecraft::player_get_tag_flags(jni, entity) & minecraft::kPlayerTagBot) != 0) {
                    break; // EntityTracker skips AntiBot bots
                }
                kind = Kind::Player;
            } else if (R::is_instance(jni, entity, R::Cls::EntityItem)) {
                kind = Kind::Item;
            } else if (R::is_instance(jni, entity, R::Cls::EntityTNTPrimed)) {
                kind = Kind::Tnt;
            } else if (R::is_instance(jni, entity, R::Cls::EntityLiving)) {
                kind = (R::is_instance(jni, entity, R::Cls::EntityMob) ||
                        R::is_instance(jni, entity, R::Cls::EntityIronGolem))
                           ? Kind::Hostile
                           : Kind::Passive;
            } else {
                break;
            }
            const int id = minecraft::entity_get_id(jni, entity);
            auto it = g_tracked.find(id);
            if (it == g_tracked.end() || !env->IsSameObject(it->second->entity, entity)) {
                auto tracked = std::make_unique<Tracked>();
                tracked->entity = env->NewGlobalRef(entity);
                tracked->id = id;
                tracked->kind = kind;
                if (kind == Kind::Tnt) {
                    tracked->fuse = R::tnt_fuse(jni, entity);
                }
                if (it != g_tracked.end()) {
                    env->DeleteGlobalRef(it->second->entity);
                    it->second = std::move(tracked);
                } else {
                    it = g_tracked.emplace(id, std::move(tracked)).first;
                }
            }
            it->second->seen_tick = g_tick;
            it->second->removed = false;
        } while (false);
        env->PopLocalFrame(nullptr);
    }
    R::drop(jni, list);

    const int64_t now = now_ms();
    for (auto it = g_tracked.begin(); it != g_tracked.end();) {
        Tracked& t = *it->second;
        bool discard = false;
        if (t.seen_tick != g_tick) {
            const bool transient = minecraft::entity_is_dead(jni, t.entity) ||
                                   minecraft::entity_get_ticks_existed(jni, t.entity) < 6;
            if (t.removed || transient) {
                const int timeout = t.seen_in_frustum ? static_cast<int>(static_cast<float>(t.retention) / 2.5f)
                                                      : t.retention;
                discard = transient || now - t.removed_at >= timeout;
            } else {
                t.removed = true;
                t.removed_at = now;
            }
        }
        if (discard) {
            env->DeleteGlobalRef(t.entity);
            it = g_tracked.erase(it);
            continue;
        }
        if (t.kind == Kind::Tnt) { // TrackedTnt.update
            if (t.fuse < 1) {
                t.has_fuse_text = false;
            } else {
                t.has_fuse_text = true;
                t.fuse_text = format_duration(50LL * t.fuse);
                --t.fuse;
            }
        }
        ++it;
    }
}

// ESPModule.createRenderer: group for a tracked entity (nullptr = not rendered).
Group* select_group(events::GameContext* ctx, Tracked& t, bool third_person) noexcept {
    Settings& s = settings();
    events::JniContext* jni = ctx->jni;
    switch (t.kind) {
        case Kind::Player: {
            const bool local = ctx->local_player != nullptr && jni->env->IsSameObject(t.entity, ctx->local_player);
            if (local) {
                // Only tracked in third person (0x80); matchesType(224) -> "Friendly" (Adjust does not check that
                // group's Enabled toggle for the local player).
                return third_person ? &s.friendly : nullptr;
            }
            const uint16_t tags = minecraft::player_get_tag_flags(jni, t.entity);
            const bool teammate = (tags & (minecraft::kPlayerTagTeammate | minecraft::kPlayerTagFriend)) != 0;
            if (teammate) {
                return s.friendly.enabled.value ? &s.friendly : nullptr;
            }
            return s.players.enabled.value ? &s.players : nullptr;
        }
        case Kind::Hostile: return s.hostile.enabled.value ? &s.hostile : nullptr;
        case Kind::Passive: return s.passive.enabled.value ? &s.passive : nullptr;
        case Kind::Item: return s.items.enabled.value ? &s.items : nullptr;
        case Kind::Tnt: return s.tnt.enabled.value ? &s.tnt : nullptr;
    }
    return nullptr;
}

int item_id_of(events::JniContext* jni, jobject item_entity) noexcept {
    jobject stack = minecraft::entity_item_get_stack(jni, item_entity);
    if (stack == nullptr) {
        return -1;
    }
    jobject item = minecraft::item_stack_get_item(jni, stack);
    const int id = item != nullptr ? minecraft::item_get_id(jni, item) : -1;
    R::drop(jni, item);
    R::drop(jni, stack);
    return id;
}

// ESPModule.acceptsItem (without the "Validate" inventory check).
bool accepts_item(events::JniContext* jni, Tracked& t) noexcept {
    const int id = item_id_of(jni, t.entity);
    return id >= 0 && settings().items.accepts_item(id);
}

void create_renderer(events::GameContext* ctx, Tracked& t, bool third_person) {
    Group* group = select_group(ctx, t, third_person);
    if (group == nullptr) {
        return;
    }
    if (!group->dormant.value && t.removed) {
        return;
    }
    if (!group->invisible.value && R::entity_is_invisible(ctx->jni, t.entity)) {
        return;
    }
    if (t.kind == Kind::Item && !accepts_item(ctx->jni, t)) {
        return;
    }
    Renderer r{};
    r.target = &t;
    r.settings = group;
    g_renderers.push_back(std::move(r));
}

math::Vec3d interpolated(events::JniContext* jni, jobject entity, float pt) noexcept {
    const math::Vec3d last = R::entity_last_tick_pos(jni, entity);
    const math::Vec3d pos = minecraft::entity_get_position(jni, entity);
    const double p = static_cast<double>(pt);
    return math::make_vec3(last.x + (pos.x - last.x) * p, last.y + (pos.y - last.y) * p, last.z + (pos.z - last.z) * p);
}

// ItemPool.updateCenter: lowest interpolated y, average interpolated x/z.
math::Vec3d pool_center(events::JniContext* jni, const std::vector<Tracked*>& pool, float pt) noexcept {
    math::Vec3d first = interpolated(jni, pool[0]->entity, pt);
    double y = first.y;
    double x = 0.0;
    double z = 0.0;
    for (Tracked* t : pool) {
        const math::Vec3d p = interpolated(jni, t->entity, pt);
        y = std::min(y, p.y);
        x += p.x;
        z += p.z;
    }
    x /= static_cast<double>(pool.size());
    z /= static_cast<double>(pool.size());
    return math::make_vec3(x == -0.0 ? 0.0 : x, y == -0.0 ? 0.0 : y, z == -0.0 ? 0.0 : z);
}

// ItemPool.pool(): groups accepted items closer than 1.85 blocks.
void pool_items(events::GameContext* ctx, std::vector<Tracked*>& source, bool third_person) {
    events::JniContext* jni = ctx->jni;
    std::sort(source.begin(), source.end(), [](const Tracked* a, const Tracked* b) { return a->id < b->id; });
    std::vector<math::Vec3d> positions(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        positions[i] = minecraft::entity_get_position(jni, source[i]->entity);
    }
    std::vector<bool> used(source.size(), false);
    for (size_t index = 0; index < source.size(); ++index) {
        if (used[index]) {
            continue;
        }
        used[index] = true;
        std::vector<size_t> group{index};
        for (size_t member = 0; member < group.size(); ++member) {
            const math::Vec3d& p = positions[group[member]];
            for (size_t candidate = index + 1; candidate < source.size(); ++candidate) {
                if (!used[candidate] && math::vec3_dist_sq(p, positions[candidate]) <= 3.4225) {
                    group.push_back(candidate);
                    used[candidate] = true;
                }
            }
        }
        if (group.size() == 1) {
            create_renderer(ctx, *source[index], third_person);
            continue;
        }
        Renderer r{};
        r.pooled = true;
        r.settings = &settings().items;
        for (size_t g : group) {
            r.pool.push_back(source[g]);
        }
        g_renderers.push_back(std::move(r));
    }
}

// ESPModule.onRender (per tick).
void rebuild_renderers(events::GameContext* ctx) {
    events::JniContext* jni = ctx->jni;
    release_renderers(jni->env);
    Settings& s = settings();

    jobject game_settings = minecraft::mc_get_game_settings(jni, ctx->minecraft);
    const bool third_person =
        game_settings != nullptr && minecraft::settings_get_third_person_view(jni, game_settings) != 0;
    R::drop(jni, game_settings);

    std::vector<Tracked*> pooled;
    for (auto& [id, tracked] : g_tracked) {
        Tracked& t = *tracked;
        if (t.kind == Kind::Item && s.items.enabled.value) {
            if (!t.removed && accepts_item(jni, t)) {
                pooled.push_back(&t);
            }
            continue;
        }
        create_renderer(ctx, t, third_person);
    }
    if (s.items.enabled.value) {
        pool_items(ctx, pooled, third_person);
    }

    // sortEntities: farthest first (camera position, current tick positions).
    jobject camera = minecraft::mc_get_render_view_entity(jni, ctx->minecraft);
    const math::Vec3d origin = camera != nullptr ? minecraft::entity_get_position(jni, camera)
                                                 : minecraft::entity_get_position(jni, ctx->local_player);
    R::drop(jni, camera);
    for (Renderer& r : g_renderers) {
        math::Vec3d p{};
        if (r.pooled) {
            p = pool_center(jni, r.pool, 1.0f);
        } else {
            p = minecraft::entity_get_position(jni, r.target->entity);
        }
        r.sort_key = -math::vec3_dist_sq(origin, p);
    }
    std::stable_sort(g_renderers.begin(), g_renderers.end(),
                     [](const Renderer& a, const Renderer& b) { return a.sort_key < b.sort_key; });
}

// =================================================================================================================
// Frame (one kEventRenderWorld2DProj pass)
// =================================================================================================================

struct Frame {
    const events::RenderWorld2DProjEvent* ev = nullptr;
    events::JniContext* jni = nullptr;
    Theme theme = Theme::Minecraft;
    float sf = 2.0f;        // ScaledResolution(mc, 2).getScaleFactor()
    float sw = 0.0f;        // scaled width (double)
    float sh = 0.0f;
    int sw_int = 0;         // getScaledWidth()
    jobject font_renderer = nullptr;
    math::Vec3d local_pos{};
    std::vector<std::function<void()>> post;   // Render2DEvent.postTasks (after the NanoVG flush)
    struct ItemDraw {
        jobject stack;
        int x;
        int y;
    };
    std::vector<ItemDraw> items;                // RenderItem calls (before the NanoVG flush)
};

struct FontRef {         // TextRenderer chosen by EntityEspRenderer.fontForScale
    bool nvg = false;    // Adjust theme: Fonts.tahoma(scale * 20)
    float scale = 0.5f;  // Minecraft / Pixel themes: Minecraft font at `scale`
    adjust::Font font = adjust::tahoma(10.0f);
};

FontRef font_for_scale(Theme theme, float scale) noexcept {
    FontRef f{};
    f.scale = scale;
    if (theme == Theme::Adjust) {
        f.nvg = true;
        f.font = adjust::tahoma(scale * 20.0f);
    }
    return f;
}

float mc_width(Frame& f, const std::string& text) noexcept {
    if (text.empty() || f.font_renderer == nullptr) {
        return 0.0f;
    }
    jstring js = f.jni->env->NewStringUTF(text.c_str());
    if (js == nullptr) {
        f.jni->env->ExceptionClear();
        return 0.0f;
    }
    const int width = minecraft::font_renderer_get_string_width(f.jni, f.font_renderer, js);
    f.jni->env->ExceptionClear();
    f.jni->env->DeleteLocalRef(js);
    return static_cast<float>(width);
}

float text_width(Frame& f, const FontRef& font, const std::string& text) noexcept {
    return font.nvg ? font.font.width(text) : mc_width(f, text) * font.scale;
}

float font_height(const FontRef& font) noexcept {
    return font.nvg ? font.font.size() : 9.0f; // NvgFontRenderer.size / FontRenderer.FONT_HEIGHT
}

// Minecraft font draw at `scale` (renderString shadow pass at +scale, then the text). Must run in a post task.
void mc_draw_now(Frame& f, const std::string& text, float x, float y, Argb color, float scale, bool shadow) noexcept {
    if (text.empty() || f.font_renderer == nullptr) {
        return;
    }
    jstring js = f.jni->env->NewStringUTF(text.c_str());
    if (js == nullptr) {
        f.jni->env->ExceptionClear();
        return;
    }
    glPushMatrix();
    glTranslatef(x, y, 0.0f);
    glScalef(scale, scale, 1.0f);
    minecraft::font_renderer_draw_string(f.jni, f.font_renderer, js, 0.0f, 0.0f, static_cast<int32_t>(color), shadow);
    f.jni->env->ExceptionClear();
    glPopMatrix();
    f.jni->env->DeleteLocalRef(js);
}

// TextRenderer.drawString(text, x, y, color, scale) (with shadow); returns the x after the text.
float draw_text_now(Frame& f, const FontRef& font, const std::string& text, float x, float y, Argb color) noexcept {
    if (font.nvg) {
        return font.font.draw(text, x, y, color);
    }
    mc_draw_now(f, text, x, y, color, font.scale, true);
    return x + mc_width(f, text) * font.scale;
}

void post(Frame& f, std::function<void()> task) {
    f.post.push_back(std::move(task));
}

// =================================================================================================================
// EntityEspRenderer.update()
// =================================================================================================================

bool is_living(const Renderer& r) noexcept {
    return r.target != nullptr &&
           (r.target->kind == Kind::Player || r.target->kind == Kind::Hostile || r.target->kind == Kind::Passive);
}

// drawBox(TrackedLivingEntity): health hue + bar colour, computed once.
void compute_health_color(Frame& f, Renderer& r) noexcept {
    if (r.health_color_cached) {
        return;
    }
    jobject e = r.target->entity;
    const float maximum = minecraft::living_get_max_health(f.jni, e);
    const float health = minecraft::living_get_health(f.jni, e);
    r.health_hue = maximum > 0.0f ? std::min(health, maximum) / maximum : 0.0f;
    r.bar_color = r.target->removed ? adjust::argb(233, 235, 234)
                                    : (adjust::hsb_to_rgb(r.health_hue * 0.33333334f, 0.6f, 0.6f) | 0xFF000000u);
    r.health_color_cached = true;
}

int armor_ratio_value(Frame& f, jobject living) noexcept {
    int armor = R::living_total_armor(f.jni, living);
    if (R::is_instance(f.jni, living, R::Cls::EntityZombie)) {
        armor = std::min(20, std::max(0, armor - 2));
    }
    return armor;
}

// EspTargetSettings.getName (+ LivingEspSettings / TrackedItem overrides)
std::string target_name(Frame& f, Renderer& r) {
    events::JniContext* jni = f.jni;
    jobject e = r.target->entity;
    if (r.target->kind == Kind::Item) {
        jobject stack = minecraft::entity_item_get_stack(jni, e);
        if (stack == nullptr) {
            return "Invalid Item Name";
        }
        std::string name = R::stack_display_name(jni, stack);
        const bool custom = minecraft::item_stack_has_display_name(jni, stack);
        jni->env->ExceptionClear();
        R::drop(jni, stack);
        return custom ? std::string(ESP_FMT "o") + name : name;
    }
    if (is_living(r) && r.settings->custom_name.value) {
        const std::string name = trim(R::entity_formatted_display_name(jni, e));
        return r.target->removed ? strip_codes(name) : name;
    }
    return R::entity_name(jni, e);
}

// EntityEspRenderer.buildItemLines
std::string item_lines(Frame& f, Renderer& r) {
    const std::string prefix = ESP_FMT "7" ESP_FMT "r";
    if (r.target->kind == Kind::Item) {
        jobject stack = minecraft::entity_item_get_stack(f.jni, r.target->entity);
        const int size = stack != nullptr ? minecraft::item_stack_get_size(f.jni, stack) : 1;
        R::drop(f.jni, stack);
        return size == 1 ? std::string() : prefix + " x" + std::to_string(size);
    }
    if (r.target->kind == Kind::Tnt) {
        return r.target->has_fuse_text ? prefix + " (" + r.target->fuse_text + ")" : prefix;
    }
    return {};
}

// EntityEspRenderer.render(): name tag strings and widths.
void update_name(Frame& f, Renderer& r) {
    Group& s = *r.settings;
    events::JniContext* jni = f.jni;
    jobject e = r.target->entity;
    const bool pixel = f.theme != Theme::Adjust;
    r.text_scale = s.nametag_scale_value() * 0.5f;
    const FontRef font = font_for_scale(f.theme, r.text_scale);

    r.display_name = target_name(f, r) + item_lines(f, r);
    if (s.bold_text.value) {
        r.display_name = std::string(ESP_FMT "l") + r.display_name;
    }
    r.has_name = true;
    r.display_name_width = text_width(f, font, r.display_name);
    r.name_color = s.nametag_color.get();
    if (s.has(kKindPlayer) && s.team_color.value && !r.target->removed) {
        Argb color = 0;
        if (team_color(jni, e, &color)) {
            r.name_color = color;
        }
    }
    if (r.target->removed) {
        r.name_color = 0xFFFFFFFFu;
    }
    if (r.target->kind == Kind::Item) {
        jobject stack = minecraft::entity_item_get_stack(jni, e);
        Argb color = 0;
        if (stack != nullptr && modern_item_color(jni, stack, &color)) {
            r.name_color = color;
        }
        R::drop(jni, stack);
    }
    if (s.has(kKindLiving) && s.nametag_health_enabled.value && is_living(r)) {
        const float health = minecraft::living_get_health(jni, e);
        if (health > 0.0f) {
            const std::string value = format_number(health);
            r.health_text = pixel ? (r.target->removed ? std::string() : std::string(ESP_FMT "c")) + value + " " + kHeart
                                  : std::string(ESP_FMT "l") + value;
            r.has_health = true;
            if (s.nametag_health_colored.value || f.theme == Theme::Adjust) {
                compute_health_color(f, r);
            }
            r.health_text_width = text_width(f, font, r.health_text);
            const float absorption = R::living_absorption(jni, e);
            if (absorption > 0.0f) {
                const std::string abs_value = format_number(absorption);
                r.absorption_text = pixel ? abs_value + " " + kHeart : std::string(ESP_FMT "l") + abs_value;
                r.has_absorption = true;
                r.absorption_width = text_width(f, font, r.absorption_text) + 2.0f;
            }
        }
    }
    if (s.distance.value) {
        const math::Vec3d pos = minecraft::entity_get_position(jni, e);
        const auto distance = static_cast<int>(std::lround(std::sqrt(math::vec3_dist_sq(f.local_pos, pos))));
        r.distance_text = std::to_string(distance) + "m";
        if (pixel) {
            r.distance_text = "[" + r.distance_text + "]";
        }
        r.has_distance = true;
        r.distance_width = text_width(f, font, r.distance_text);
    }
}

// EntityEspRenderer.healthFraction(TrackedItem): remaining durability.
float durability_fraction(Frame& f, jobject item_entity) noexcept {
    jobject stack = minecraft::entity_item_get_stack(f.jni, item_entity);
    if (stack == nullptr) {
        return 0.0f;
    }
    const int damage = R::stack_item_damage(f.jni, stack);
    const int maximum = R::stack_max_damage(f.jni, stack);
    R::drop(f.jni, stack);
    if (maximum < 1) {
        return 0.0f;
    }
    if (static_cast<float>(damage) <= -1.0f) {
        return 1.0f;
    }
    const auto remaining = static_cast<float>(maximum - damage);
    if (remaining <= 0.0f) {
        return 1.0f;
    }
    return remaining / static_cast<float>(maximum);
}

jobject global_or_null(JNIEnv* env, jobject local) noexcept {
    if (local == nullptr) {
        return nullptr;
    }
    jobject global = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    return global;
}

// EntityEspRenderer.updateSettings()
void update_entity(Frame& f, Renderer& r) {
    Group& s = *r.settings;
    events::JniContext* jni = f.jni;
    JNIEnv* env = jni->env;
    jobject e = r.target->entity;

    if (s.nametags_enabled.value) {
        update_name(f, r);
    }
    if (s.has(kKindLiving) && s.health_enabled.value && is_living(r)) { // maxHealth()
        const float maximum = minecraft::living_get_max_health(jni, e);
        r.health_ratio = maximum > 0.0f ? std::min(maximum, R::living_absorption(jni, e)) / maximum : 0.0f;
        r.bar_ratio = static_cast<float>(armor_ratio_value(f, e)) / 20.0f;
        compute_health_color(f, r);
    }
    if (s.has(kKindMob)) {
        if (s.item_name_enabled.value) { // updateDisplayName
            r.half_width = s.item_name_scale.value * 0.5f;
            const FontRef font = font_for_scale(f.theme, r.half_width);
            jobject stack = R::living_held_item(jni, e);
            jobject item = stack != nullptr ? minecraft::item_stack_get_item(jni, stack) : nullptr;
            if (item != nullptr && (!s.item_name_custom_only.value || R::stack_has_display_tag(jni, stack))) {
                r.held_item_name = R::stack_rarity_color(jni, stack) + R::stack_display_name(jni, stack);
                if (r.target->removed) {
                    r.held_item_name = strip_codes(r.held_item_name);
                }
                r.has_held_item = true;
                r.held_item_width = text_width(f, font, r.held_item_name);
            }
            R::drop(jni, item);
            R::drop(jni, stack);
        }
        if (s.equipment.value) { // renderArmor / armorLines
            IconRow row{};
            row.stacks.push_back(global_or_null(env, R::living_held_item(jni, e)));
            for (int slot = 3; slot >= 0; --slot) {
                row.stacks.push_back(global_or_null(env, R::living_current_armor(jni, e, slot)));
            }
            for (jobject stack : row.stacks) {
                if (stack != nullptr) {
                    row.advance += 4.0f;
                }
            }
            if (row.advance > 0.0f) {
                r.equipment.push_back(std::move(row));
            } else {
                for (jobject stack : row.stacks) {
                    if (stack != nullptr) {
                        env->DeleteGlobalRef(stack);
                    }
                }
            }
        }
        if (s.potions.value) { // collectEquipment
            R::PotionIcon icons[32];
            const int count = R::living_potion_icons(jni, e, icons, 32);
            if (count > 0) {
                r.has_potions = true;
                for (int i = 0; i < count; ++i) {
                    PotionEntry entry{};
                    entry.icon_index = icons[i].icon_index;
                    if (icons[i].amplifier != 0) {
                        entry.text = to_roman(icons[i].amplifier + 1);
                        entry.width = mc_width(f, entry.text);
                    }
                    r.potions.push_back(std::move(entry));
                }
                r.potion_width = static_cast<float>(r.potions.size()) * 4.75f;
            }
        }
        if (s.has(kKindPlayer) && s.flags_enabled.value && r.target->kind == Kind::Player) { // collectPlayerFlags
            r.has_flags = true;
            for (int i = 0; i < static_cast<int>(PlayerFlag::Count); ++i) {
                if (!s.flags[i].enabled.value) {
                    continue;
                }
                bool active = false;
                switch (static_cast<PlayerFlag>(i)) {
                    case PlayerFlag::Whitelisted:
                        active = (minecraft::player_get_tag_flags(jni, e) & minecraft::kPlayerTagFriend) != 0;
                        break;
                    case PlayerFlag::CreativeMode: active = R::player_is_creative(jni, e); break;
                    case PlayerFlag::Blocking: active = R::player_is_blocking(jni, e); break;
                    case PlayerFlag::Sneaking: active = minecraft::entity_is_sneaking(jni, e); break;
                    default: break;
                }
                if (active) {
                    r.flags.push_back(i);
                }
            }
        }
        if (s.armor_enabled.value) { // health()
            r.bar_ratio = static_cast<float>(armor_ratio_value(f, e)) / 20.0f;
        }
    } else if (s.has(kKindItem)) {
        if (s.durability_enabled.value) {
            r.bar_ratio = durability_fraction(f, e);
        }
    } else if (s.has(kKindTnt)) {
        if (s.countdown_enabled.value) {
            r.bar_ratio = static_cast<float>(R::tnt_fuse(jni, e)) / 80.0f;
        }
    }
    env->ExceptionClear();
}

// ItemPoolEspRenderer.updateFont() / rebuild()
void update_pool(Frame& f, Renderer& r, const math::Vec3d& center) {
    Group& s = *r.settings;
    events::JniContext* jni = f.jni;
    r.pool_scale = s.nametag_scale_value() * 0.5f;
    const FontRef font = font_for_scale(f.theme, r.pool_scale);

    struct Entry {
        int item_id = -1;
        int metadata = 0;
        bool has_name = false;
        std::string key_name;
        std::string name;
        int count = 0;
        int sort_key = 0;
        bool has_color = false;
        Argb color = 0;
    };
    std::vector<Entry> grouped;
    for (Tracked* t : r.pool) {
        jobject stack = minecraft::entity_item_get_stack(jni, t->entity);
        if (stack == nullptr) {
            continue;
        }
        jobject item = minecraft::item_stack_get_item(jni, stack);
        Entry key{};
        key.item_id = item != nullptr ? minecraft::item_get_id(jni, item) : -1;
        key.metadata = (item != nullptr && R::item_has_subtypes(jni, item)) ? R::stack_metadata(jni, stack) : 0;
        key.has_name = minecraft::item_stack_has_display_name(jni, stack);
        if (key.has_name) {
            key.key_name = R::stack_display_name(jni, stack);
        }
        auto it = std::find_if(grouped.begin(), grouped.end(), [&](const Entry& e) {
            return e.item_id == key.item_id && e.metadata == key.metadata && e.has_name == key.has_name &&
                   e.key_name == key.key_name;
        });
        if (it == grouped.end()) {
            key.has_color = modern_item_color(jni, stack, &key.color);
            if (item != nullptr && R::is_instance(jni, item, R::Cls::ItemSword)) {
                key.sort_key = INT_MIN;
            } else if (item != nullptr && R::is_instance(jni, item, R::Cls::ItemTool)) {
                key.sort_key = INT_MIN + 10 + R::item_tool_material_ordinal(jni, item);
            } else {
                key.sort_key = INT_MAX; // minus count below
            }
            grouped.push_back(key);
            it = grouped.end() - 1;
        }
        it->count += std::max(1, minecraft::item_stack_get_size(jni, stack));
        it->name = item == nullptr ? std::string("Item") : R::item_stack_display_name(jni, item, stack);
        bool custom = false;
        std::string custom_name = R::stack_custom_name(jni, stack, &custom);
        if (custom) {
            it->name = custom_name;
        }
        R::drop(jni, item);
        R::drop(jni, stack);
    }
    for (Entry& e : grouped) {
        if (e.sort_key == INT_MAX) {
            e.sort_key = INT_MAX - e.count;
        }
    }
    std::stable_sort(grouped.begin(), grouped.end(),
                     [](const Entry& a, const Entry& b) { return a.sort_key < b.sort_key; });

    const Argb nametag = s.nametag_color.get();
    r.lines.clear();
    for (const Entry& e : grouped) {
        if (r.lines.size() >= 3) {
            break;
        }
        PoolLine line{};
        line.text = e.count == 1 ? e.name : e.name + ESP_FMT "7 x" + std::to_string(e.count);
        line.color = e.has_color ? e.color : nametag;
        line.width = text_width(f, font, line.text);
        r.lines.push_back(std::move(line));
    }
    if (grouped.size() > r.lines.size()) {
        const size_t remaining = grouped.size() - r.lines.size();
        PoolLine more{};
        more.text = std::string(ESP_FMT "7") + std::to_string(remaining) + " more item" + (remaining == 1 ? "" : "s") +
                    "...";
        more.color = nametag;
        more.width = text_width(f, font, more.text);
        r.lines.push_back(std::move(more));
    }
    if (s.distance.value) {
        const auto distance = static_cast<long long>(std::llround(std::sqrt(math::vec3_dist_sq(f.local_pos, center))));
        PoolLine line{};
        line.text = std::string(ESP_FMT "7") + std::to_string(distance) + "m";
        line.color = nametag;
        line.width = text_width(f, font, line.text);
        r.lines.insert(r.lines.begin(), std::move(line));
    }
    jni->env->ExceptionClear();
}

// =================================================================================================================
// Projection (RenderUtil.isInFrustum / worldToScreen / projectEntityBounds)
// =================================================================================================================

struct Bounds {
    float x = 0.0f; // left
    float y = 0.0f; // top
    float z = 0.0f; // right
    float w = 0.0f; // bottom
};

struct Clip {
    float x, y, z, w;
};

Clip to_clip(const events::RenderWorld2DProjEvent* ev, const math::Vec3d& world) noexcept {
    const auto rx = static_cast<float>(world.x - ev->render_pos.x);
    const auto ry = static_cast<float>(world.y - ev->render_pos.y);
    const auto rz = static_cast<float>(world.z - ev->render_pos.z);
    const float* mv = ev->modelview_matrix;
    const float* pr = ev->projection_matrix;
    const float ex = mv[0] * rx + mv[4] * ry + mv[8] * rz + mv[12];
    const float ey = mv[1] * rx + mv[5] * ry + mv[9] * rz + mv[13];
    const float ez = mv[2] * rx + mv[6] * ry + mv[10] * rz + mv[14];
    const float ew = mv[3] * rx + mv[7] * ry + mv[11] * rz + mv[15];
    return Clip{pr[0] * ex + pr[4] * ey + pr[8] * ez + pr[12] * ew, pr[1] * ex + pr[5] * ey + pr[9] * ez + pr[13] * ew,
                pr[2] * ex + pr[6] * ey + pr[10] * ez + pr[14] * ew, pr[3] * ex + pr[7] * ey + pr[11] * ez + pr[15] * ew};
}

// Frustum.isBoundingBoxInFrustum: false when all 8 corners are outside one clip plane.
bool in_frustum(const events::RenderWorld2DProjEvent* ev, const math::Vec3d& min, const math::Vec3d& max) noexcept {
    int outside[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        const math::Vec3d corner = math::make_vec3((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y,
                                                   (i & 4) ? max.z : min.z);
        const Clip c = to_clip(ev, corner);
        outside[0] += c.x < -c.w;
        outside[1] += c.x > c.w;
        outside[2] += c.y < -c.w;
        outside[3] += c.y > c.w;
        outside[4] += c.z < -c.w;
        outside[5] += c.z > c.w;
    }
    for (int count : outside) {
        if (count == 8) {
            return false;
        }
    }
    return true;
}

// RenderUtil.worldToScreen(pos, Vec2f(size_x, size_y), resolution); false = null.
bool world_to_screen(const Frame& f, const math::Vec3d& pos, double size_x, double size_y, Bounds* out) noexcept {
    const double min_x = std::min(pos.x - size_x, pos.x + size_x);
    const double min_y = std::min(pos.y, pos.y + size_y);
    const double min_z = std::min(pos.z - size_x, pos.z + size_x);
    const double max_x = std::max(pos.x - size_x, pos.x + size_x);
    const double max_y = std::max(pos.y, pos.y + size_y);
    const double max_z = std::max(pos.z - size_x, pos.z + size_x);
    const math::Vec3d corners[8] = {
        math::make_vec3(min_x, min_y, min_z), math::make_vec3(min_x, max_y, min_z),
        math::make_vec3(max_x, min_y, min_z), math::make_vec3(max_x, max_y, min_z),
        math::make_vec3(min_x, min_y, max_z), math::make_vec3(min_x, max_y, max_z),
        math::make_vec3(max_x, min_y, max_z), math::make_vec3(max_x, max_y, max_z),
    };
    bool have = false;
    for (int i = 0; i < 8; ++i) {
        const Clip c = to_clip(f.ev, corners[i]);
        if (c.w <= 0.0f) {
            continue;
        }
        const float depth = c.z / c.w * 0.5f + 0.5f;
        if (depth < 0.0f || depth >= 1.0f) {
            continue;
        }
        const float sx = (c.x / c.w * 0.5f + 0.5f) * f.ev->viewport_size.x;
        const float sy = f.ev->viewport_size.y - (c.y / c.w * 0.5f + 0.5f) * f.ev->viewport_size.y;
        const float x = sx / f.sf;
        const float y = sy / f.sf;
        if (!have) {
            // The first corner seeds right/bottom with Math.max(x, depth) / Math.max(y, 0) (or depth / 0).
            out->x = x;
            out->y = y;
            out->z = i == 0 ? std::max(x, depth) : depth;
            out->w = i == 0 ? std::max(y, 0.0f) : 0.0f;
            have = true;
            if (i == 0) {
                continue;
            }
        }
        out->x = std::min(out->x, x);
        out->y = std::min(out->y, y);
        out->z = std::max(out->z, x);
        out->w = std::max(out->w, y);
    }
    return have;
}

// CulledEspRenderer.project / GroupedEspRenderer.projectGroup
bool project(Frame& f, Renderer& r, math::Vec3d* out_center, Bounds* out) noexcept {
    events::JniContext* jni = f.jni;
    const float pt = f.ev->partial_ticks;
    if (r.pooled) {
        const math::Vec3d c = pool_center(jni, r.pool, pt);
        *out_center = c;
        if (!in_frustum(f.ev, math::make_vec3(c.x - 0.300000011920929, c.y, c.z - 0.300000011920929),
                        math::make_vec3(c.x + 0.300000011920929, c.y + 1.799999952316284, c.z + 0.300000011920929))) {
            return false;
        }
        return world_to_screen(f, c, 0.4000000059604645, 0.4000000059604645, out);
    }
    jobject e = r.target->entity;
    const float width = R::entity_width(jni, e);
    float height = R::entity_height(jni, e);
    if (!R::entity_ignore_frustum_check(jni, e)) {
        const math::Vec3d p = minecraft::entity_get_position(jni, e);
        const double half = static_cast<double>(width) * 0.5;
        if (!in_frustum(f.ev, math::make_vec3(p.x - half, p.y, p.z - half),
                        math::make_vec3(p.x + half, p.y + static_cast<double>(height), p.z + half))) {
            return false;
        }
    }
    if (r.target->removed) {
        r.target->seen_in_frustum = true;
    }
    // projectEntityBounds(entity, resolution, !isRemoved(), partialTicks)
    math::Vec3d pos = r.target->removed ? minecraft::entity_get_position(jni, e) : interpolated(jni, e, pt);
    if (r.target->kind == Kind::Item && !item_physics::running()) { // vanilla bobbing unless Item Physics renders
        const float age = static_cast<float>(R::item_entity_age(jni, e));
        pos.y += static_cast<double>(std::sin(age / 10.0f + R::item_entity_hover_start(jni, e)) * 0.1f + 0.1f);
    }
    if (r.target->kind == Kind::Player) {
        height += minecraft::entity_is_sneaking(jni, e) ? 0.0f : 0.2f;
    } else if (R::is_instance(jni, e, R::Cls::EntityZombie) && R::living_is_child(jni, e)) {
        height /= 1.75f;
    }
    return world_to_screen(f, pos, static_cast<double>(width / 1.65f), static_cast<double>(height), out);
}

// =================================================================================================================
// Drawing (EntityEspRenderer.textY and helpers)
// =================================================================================================================

Argb with_alpha_prefix(int alpha, Argb rgb) noexcept {
    return (static_cast<Argb>(std::clamp(alpha, 0, 255)) << 24) | (rgb & 0xFFFFFFu);
}

// buildRenderLines
void draw_nametag_lines(Frame& f, const Renderer& r, const FontRef& font, float top, float name_x, float health_x,
                        float distance_x, int alpha, bool health_colored) {
    const bool pixel = f.theme != Theme::Adjust;
    const float vertical = pixel ? 1.5f : std::max(0.0f, 2.5f - (r.text_scale - 0.5f));
    const float y = top + vertical;
    draw_text_now(f, font, r.display_name, name_x, y, with_alpha_prefix(alpha, r.name_color));
    if (r.has_health) {
        const Argb color = health_colored ? with_alpha_prefix(alpha, r.bar_color) : with_alpha_prefix(alpha, 0xE9EBEA);
        const float next_x = draw_text_now(f, font, r.health_text, health_x + 1.5f, y, color);
        if (r.has_absorption) {
            draw_text_now(f, font, r.absorption_text, next_x + 1.5f, y,
                          with_alpha_prefix(alpha, r.target->removed ? 0xAAAAAA : 0xFFAA00));
        }
    }
    if (r.has_distance) {
        draw_text_now(f, font, r.distance_text, distance_x + 1.5f, y, with_alpha_prefix(alpha, 0xAAAAAA));
    }
}

// healthColor(...): the name tag above the box; returns its height (8).
float draw_nametag(Frame& f, Renderer& r, int alpha, float bottom, float center) {
    Group& s = *r.settings;
    const bool pixel = f.theme != Theme::Adjust;
    const FontRef font = font_for_scale(f.theme, r.text_scale);
    const bool background = s.background.value;
    const RectLayout layout = background ? static_cast<RectLayout>(s.rectangles.value) : RectLayout::Together;
    const bool separated = layout == RectLayout::Separated;
    const bool health_colored = s.has(kKindLiving) && s.nametag_health_colored.value;
    float width = r.display_name_width;
    if (f.theme == Theme::Adjust) {
        alpha = std::min(255, alpha + 30);
    }
    const float extra_health = r.health_text_width + r.absorption_width;
    const float distance_advance = r.has_distance ? r.distance_width + 2.0f : 0.0f;
    float left;
    float distance_x = 0.0f;
    if (!separated) {
        width += distance_advance;
        if (r.has_health) {
            width += extra_health + 2.0f;
        }
        left = center - width * 0.5f - 1.5f;
    } else {
        left = center - width * 0.5f - 1.5f;
        if (r.has_health) {
            left -= extra_health * 0.5f + 2.0f;
        }
        if (r.has_distance) {
            left += r.distance_width * 0.5f + 2.0f;
        }
    }
    const float name_x = left + 1.5f + (separated ? 0.0f : distance_advance);
    const float fh = font_height(font);
    const float top = pixel ? bottom - fh * r.text_scale - 2.0f : bottom - (fh - 0.5f) - 2.0f;
    const Argb background_color = adjust::argb(0, 0, 0, std::min(alpha, 145));
    if (background) {
        adjust::fill_rect(background_color, left, top, left + width + 3.0f, bottom);
    }
    const float name_right = name_x + r.display_name_width;
    float health_x = name_right;
    if (separated) {
        float label_right = left + width + 3.0f;
        health_x = 0.0f;
        if (r.has_health) {
            health_x = label_right + 1.0f;
            label_right = health_x + extra_health + 3.0f;
            adjust::fill_rect(background_color, health_x, top, label_right, bottom);
        }
        if (r.has_distance) {
            distance_x = left - 1.0f - r.distance_width - 3.0f;
            adjust::fill_rect(background_color, distance_x, top, distance_x + r.distance_width + 3.0f, bottom);
        }
    } else {
        distance_x = left;
    }
    if (pixel) {
        const Renderer* rp = &r;
        post(f, [&f, rp, font, top, name_x, health_x, distance_x, alpha, health_colored]() {
            minecraft::gl_state_enable_blend(f.jni);
            minecraft::gl_state_blend_func(f.jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            draw_nametag_lines(f, *rp, font, top, name_x, health_x, distance_x, alpha, health_colored);
        });
    } else {
        draw_nametag_lines(f, r, font, top, name_x, health_x, distance_x, alpha, health_colored);
    }
    return 8.0f;
}

// renderNametag: held item name below the box.
void draw_item_name(Frame& f, Renderer& r, int alpha, float y, float center) {
    Group& s = *r.settings;
    const bool pixel = f.theme != Theme::Adjust;
    const FontRef font = font_for_scale(f.theme, r.half_width);
    const float half = r.held_item_width * 0.5f;
    if (s.background.value) {
        const float fh = font_height(font);
        const float bottom = y + (pixel ? fh * r.half_width : fh - 0.5f) + 2.0f;
        adjust::fill_rect(adjust::argb(0, 0, 0, std::min(145, alpha)), center - half - 1.5f, y, center + half + 1.5f,
                          bottom);
    }
    const int draw_alpha = f.theme == Theme::Adjust ? std::min(255, alpha + 20) : alpha;
    float top = y;
    if (s.background.value) {
        top += 1.5f;
    }
    if (f.theme == Theme::Adjust) {
        top += 1.0f;
    }
    const Argb color = with_alpha_prefix(draw_alpha, 0xE9EBEA);
    const std::string text = r.held_item_name;
    const float x = center - half;
    if (pixel) {
        post(f, [&f, font, text, x, top, color]() {
            minecraft::gl_state_enable_blend(f.jni);
            minecraft::gl_state_blend_func(f.jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            draw_text_now(f, font, text, x, top, color);
        });
    } else {
        draw_text_now(f, font, text, x, top, color);
    }
}

// renderFlags: player flag labels to the right of the box.
void draw_flags(Frame& f, Renderer& r, int alpha, float x, float y) {
    if (r.flags.empty()) {
        return;
    }
    Group& s = *r.settings;
    FontRef font{};
    font.scale = 0.5f;
    if (f.theme == Theme::Adjust) {
        font.nvg = true;
        font.font = adjust::kTahoma8;
        y += 1.0f;
        alpha = std::min(alpha + 30, 255);
    }
    std::vector<std::pair<std::string, Argb>> labels;
    for (int flag : r.flags) {
        labels.emplace_back(s.flags[flag].label, with_alpha_prefix(alpha, s.flags[flag].color.get()));
    }
    const auto draw = [&f, font, labels, x, y]() {
        for (size_t i = 0; i < labels.size(); ++i) {
            draw_text_now(f, font, labels[i].first, x + 2.0f, y + 4.0f * static_cast<float>(i), labels[i].second);
        }
    };
    if (f.theme != Theme::Adjust) {
        post(f, [&f, draw]() {
            minecraft::gl_state_enable_blend(f.jni);
            minecraft::gl_state_blend_func(f.jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            draw();
        });
    } else {
        draw();
    }
}

// drawEquipment: queues the RenderItem calls; returns the height used.
float draw_equipment(Frame& f, Renderer& r, float center, float y) {
    const float height = static_cast<float>(r.equipment.size() - 1) * 10.0f + 8.0f;
    float row_y = 0.0f;
    for (const IconRow& row : r.equipment) {
        float advance = 0.0f;
        for (jobject stack : row.stacks) {
            if (stack == nullptr) {
                continue;
            }
            f.items.push_back({stack, static_cast<int>((center - row.advance + advance) * 2.0f),
                               static_cast<int>((y - height + row_y) * 2.0f)});
            advance += 8.0f;
        }
        row_y += 10.0f;
    }
    return height + 2.0f;
}

// barX / textX: potion icons (deferred).
float draw_potions(Frame& f, Renderer& r, int alpha, float center, float y) {
    if (r.potion_width == 0.0f) {
        return 0.0f;
    }
    const float width = r.potion_width;
    std::vector<PotionEntry> potions = r.potions;
    post(f, [&f, potions, alpha, center, width, y]() {
        events::JniContext* jni = f.jni;
        jobject mc = minecraft::mc_get_instance(jni);
        jobject textures = minecraft::mc_get_texture_manager(jni, mc);
        const float a = static_cast<float>(alpha) / 255.0f;
        glPushMatrix();
        glScalef(0.5f, 0.5f, 0.5f);
        minecraft::gl_state_enable_blend(jni);
        minecraft::gl_state_blend_func(jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        float advance = 0.0f;
        for (const PotionEntry& potion : potions) {
            const float x = (center - width + advance) * 2.0f;
            const float top = (y - 8.0f) * 2.0f;
            if (R::bind_inventory_background(jni, textures)) {
                minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, a);
                const auto u = static_cast<float>(18 * (potion.icon_index % 8));
                const auto v = static_cast<float>(18 * (potion.icon_index / 8) + 198);
                glBegin(GL_QUADS); // RenderUtil.drawPotionIcon
                glTexCoord2f(u / 256.0f, (v + 18.0f) / 256.0f);
                glVertex3f(x, top + 18.0f, 0.0f);
                glTexCoord2f((u + 18.0f) / 256.0f, (v + 18.0f) / 256.0f);
                glVertex3f(x + 18.0f, top + 18.0f, 0.0f);
                glTexCoord2f((u + 18.0f) / 256.0f, v / 256.0f);
                glVertex3f(x + 18.0f, top, 0.0f);
                glTexCoord2f(u / 256.0f, v / 256.0f);
                glVertex3f(x, top, 0.0f);
                glEnd();
            }
            if (!potion.text.empty()) {
                mc_draw_now(f, potion.text, x + 17.0f - potion.width, top + 9.0f, with_alpha_prefix(alpha, 0xE9EBEA),
                            1.0f, true);
            }
            advance += 9.5f;
        }
        glPopMatrix();
        minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
        R::drop(jni, textures);
        R::drop(jni, mc);
    });
    return 9.5f;
}

// drawText(...): Cornered / Full border around the box.
void draw_border(Frame& f, Renderer& r, int alpha, float left, float top, float right, float bottom) {
    Group& s = *r.settings;
    Argb rgb;
    if (r.target->removed) {
        rgb = 0xB4B4B4;
    } else if (s.has(kKindPlayer) && s.border_team_color.value) {
        Argb team = 0;
        rgb = team_color(f.jni, r.target->entity, &team) ? team : 0xFFFFFF;
    } else {
        rgb = s.border_color.get();
    }
    const Argb color = with_alpha_prefix(alpha, rgb);
    const Argb black = static_cast<Argb>(std::clamp(alpha, 0, 255)) << 24;
    switch (static_cast<BorderMode>(s.border_mode.value)) {
        case BorderMode::Disabled:
            break;
        case BorderMode::Full:
            adjust::stroke_rect(false, black, left - 1.0f, top - 1.0f, right + 1.0f, bottom + 1.0f, 1.5f);
            adjust::stroke_rect(false, color, left - 0.5f, top - 0.5f, right + 0.5f, bottom + 0.5f, 0.5f);
            break;
        case BorderMode::Cornered: {
            // NvgDraw.fillRect without a colour reuses the last fill colour: `cur` tracks it.
            Argb cur = color;
            const auto fill = [&cur](float x1, float y1, float x2, float y2) { adjust::fill_rect(cur, x1, y1, x2, y2); };
            const auto fill_c = [&cur](Argb c, float x1, float y1, float x2, float y2) {
                cur = c;
                adjust::fill_rect(cur, x1, y1, x2, y2);
            };
            const float length = -(left - right) / 3.5f;
            const float x0 = left - 0.5f;
            const float y0 = top - 0.5f;
            const float x1 = x0 + length;
            fill_c(color, x0, y0, x1, top);
            const float t0 = top + 0.5f;
            const float l0 = x0 + 0.5f;
            const float t1 = length + t0;
            const float t2 = t0 - 0.5f;
            fill(x0, t2, l0, t1);
            const float t3 = y0 - 0.5f;
            const float l1 = left + length;
            fill_c(black, x0, t3, l1, y0);
            const float l2 = left + 0.5f;
            const float l3 = l2 - 0.5f;
            fill(l3, t2, l2, t1);
            fill(left, t2, l1, t0);
            fill(x1, y0, l1 + 0.5f - 0.5f, top);
            const float l4 = x0 - 0.5f;
            fill(l4, t3, x0, t1);
            fill(l4, t2 + length + 0.5f, x0 + 1.0f, t1 + 0.5f);
            const float r0 = right - length;
            fill_c(color, r0, y0, right, top);
            const float r1 = top + length;
            const float r2 = right + 0.5f;
            fill(right, y0, r2, r1);
            fill_c(black, r0, t3, r2, y0);
            const float r3 = right - 0.5f;
            fill(r3, t2, right, r1);
            fill(r0, t2, r3, t0);
            fill(r0 - 0.5f, t3, r0 - 0.5f + 0.5f, y0 + 1.0f);
            const float r4 = right + 1.0f;
            fill(r4 - 0.5f, t3, r4, r1);
            fill(r3, t1 - 0.5f, r4, t1 - 0.5f + 0.5f);
            const float b0 = bottom + 0.5f - 0.5f;
            const float b1 = bottom + 0.5f;
            fill_c(color, x0, b0, x1, b1);
            const float b2 = b0 - length;
            fill(x0, b2, l0, b1);
            const float b3 = bottom + 1.0f;
            fill_c(black, x0, b3 - 0.5f, x1, b3);
            fill(l3, b2, l2, bottom);
            const float b4 = bottom - 0.5f;
            fill(l2, b4, x1, bottom);
            const float b5 = x1 + 0.5f;
            const float b6 = b1 + 0.5f;
            fill(x1, b4, b5, b6);
            fill(l4, b2, x0, b6);
            const float b7 = bottom - length - 0.5f;
            const float b8 = b7 + 0.5f;
            const float b9 = bottom - length;
            fill(l4, b7, left + 1.0f - 0.5f, b8);
            fill_c(color, r0, b0, right, b1);
            fill(right, b2, r2, b1);
            fill_c(black, r0, b6, r2, b1);
            fill(r3, b9, right, b4);
            fill(r0, b4, right, bottom);
            fill(r3 - length, b4, r3 - length + 0.5f, b6);
            fill(r3 + 1.0f, b4 - length + 0.5f, r4, b3);
            fill(r3, b7, r3 + 1.5f, b8);
            break;
        }
    }
}

// drawDistance: armor / durability / countdown bar; returns the space used below the box.
float draw_bar(Frame& f, Renderer& r, int alpha, float left, float top, float right, float bottom) {
    if (r.bar_ratio <= 0.0f) {
        return 0.0f;
    }
    Group& s = *r.settings;
    Argb selected;
    bool thinner;
    BarPosition position;
    if (s.has(kKindMob)) {
        selected = s.armor_color.get();
        thinner = s.armor_thinner.value;
        position = static_cast<BarPosition>(s.armor_position.value);
    } else if (s.has(kKindItem)) {
        selected = s.durability_color.get();
        thinner = s.durability_thinner.value;
        position = BarPosition::Bottom;
    } else {
        selected = s.countdown_color.get();
        thinner = s.countdown_thinner.value;
        position = static_cast<BarPosition>(s.countdown_position.value);
    }
    const Argb color = with_alpha_prefix(alpha, r.target->removed ? 0xFFFFFF : selected);
    const Argb background = adjust::argb(10, 10, 10, std::max(0, alpha - 60));
    const float thickness = thinner ? 2.5f : 3.0f;
    if (position == BarPosition::Right) {
        const float filled_top = bottom - (bottom - top) * r.bar_ratio;
        adjust::fill_rect(background, right + 1.5f, top - 1.0f, right + thickness + 0.5f, bottom + 1.0f);
        adjust::fill_rect(color, right + 2.0f, bottom + 0.5f, right + thickness, filled_top - 0.5f);
        return 0.0f;
    }
    const float filled_right = left + (right - left) * r.bar_ratio;
    adjust::fill_rect(background, left - 1.0f, bottom + 1.5f, right + 1.0f, bottom + thickness + 0.5f);
    adjust::fill_rect(color, left - 0.5f, bottom + 2.0f, filled_right + 0.5f, bottom + thickness);
    return thinner ? 2.0f : 2.5f;
}

// renderItemIcons: health bar left of the box.
void draw_health_bar(Renderer& r, int alpha, float x, float top, float bottom) {
    if (r.health_hue <= 0.0f) {
        return;
    }
    Group& s = *r.settings;
    const float thickness = s.health_thinner.value ? 2.5f : 3.0f;
    const float outer = x - thickness;
    const float inner = x - 2.0f;
    const float height = bottom - top;
    const float health_top = bottom - r.health_hue * height;
    adjust::fill_rect(adjust::argb(10, 10, 10, std::max(0, alpha - 60)), inner + 0.5f, top - 1.0f, outer - 0.5f,
                      bottom + 1.0f);
    const bool custom = s.health_custom_color.value;
    const Argb color = with_alpha_prefix(alpha, r.target->removed ? 0xFFFFFF
                                                : custom          ? s.health_color.get()
                                                                  : r.bar_color);
    if (r.health_ratio > 0.0f) {
        const float split = bottom - r.health_ratio * height;
        const Argb absorption = with_alpha_prefix(alpha, r.target->removed ? 0xAAAAAA
                                                         : custom          ? adjust::brighter(color, 0.80000001f)
                                                                           : 0xD7D74B);
        adjust::fill_rect(color, inner, split - 0.5f, outer, health_top - 0.5f);
        adjust::fill_rect(absorption, inner, bottom + 0.5f, outer, split - 0.5f);
    } else {
        adjust::fill_rect(color, inner, bottom + 0.5f, outer, health_top - 0.5f);
    }
}

// EntityEspRenderer.textY
void draw_entity(Frame& f, Renderer& r, const Bounds& b) {
    Group& s = *r.settings;
    const float left = b.x;
    const float top = b.y;
    const float right = b.z;
    const float bottom = b.w;
    const float center = left + (right - left) * 0.5f;
    int alpha = 255;
    if (r.target->removed) {
        const int duration = r.target->seen_in_frustum
                                 ? static_cast<int>(static_cast<float>(r.target->retention) / 2.5f)
                                 : r.target->retention;
        const int64_t elapsed = now_ms() - r.target->removed_at;
        if (elapsed >= duration - 1000LL) {
            const int64_t remaining = duration - elapsed;
            alpha = remaining >= 30 ? static_cast<int>(static_cast<float>(remaining) / 1000.0f * 255.0f) : 0;
        }
    }
    alpha = std::min(alpha, static_cast<int>(s.max_alpha.value));
    if (alpha < 10) {
        return;
    }
    if (s.use_fov.value) {
        const float dx = f.sw * 0.5f - center;
        const float dy = f.sh * 0.5f - (top + (bottom - top) * 0.5f);
        const double distance = std::min(std::sqrt(static_cast<double>(dx * dx + dy * dy)) * f.sf,
                                         static_cast<double>(f.sw_int));
        alpha -= static_cast<int>(distance / f.sw_int * alpha);
        if (alpha < 25) {
            return;
        }
    }
    float above = 2.0f;
    if (r.has_name) { // nametagBind.isHeld(): no bind support -> always shown
        above += draw_nametag(f, r, alpha, top - above, center);
    }
    if (s.has(kKindMob)) {
        int icon_alpha = alpha;
        if (s.fov_alpha.value) {
            const float dx = f.sw * 0.5f - center;
            const float width = f.sw * 0.30000001f;
            const auto distance = static_cast<float>(std::min(std::fabs(dx) * f.sf, width));
            icon_alpha -= static_cast<int>(distance / width * static_cast<float>(icon_alpha));
        }
        if (icon_alpha >= 25) {
            if (!r.equipment.empty()) {
                above += draw_equipment(f, r, center, top - above);
            }
            if (r.has_potions) {
                above += draw_potions(f, r, icon_alpha, center, top - above);
            }
        }
        if (r.has_flags) {
            draw_flags(f, r, alpha, right, top);
        }
    }
    if (s.has(kKindBordered)) {
        draw_border(f, r, alpha, left, top, right, bottom);
    }
    float below = 2.0f;
    if ((s.has(kKindMob) && s.armor_enabled.value) || (s.has(kKindItem) && s.durability_enabled.value) ||
        (s.has(kKindTnt) && s.countdown_enabled.value)) {
        below += draw_bar(f, r, alpha, left, top, right, bottom);
    }
    if (s.has(kKindLiving)) {
        if (s.health_enabled.value) {
            draw_health_bar(r, alpha, left, top, bottom);
        }
        if (r.has_held_item) {
            draw_item_name(f, r, alpha, bottom + below, center);
        }
    }
}

// ItemPoolEspRenderer.updateDistance / drawLines
void draw_pool(Frame& f, Renderer& r, const Bounds& b) {
    Group& s = *r.settings;
    if (!s.nametags_enabled.value) {
        return;
    }
    int alpha = static_cast<int>(s.max_alpha.value);
    if (alpha < 25) {
        return;
    }
    const float x = (b.z - b.x) * 0.5f + b.x;
    if (s.use_fov.value) {
        const float dx = f.sw * 0.5f - x;
        const float dy = f.sh * 0.5f - ((b.w - b.y) * 0.5f + b.y);
        const float distance = std::min(std::sqrt(dx * dx + dy * dy) * f.sf, static_cast<float>(f.sw_int));
        alpha -= static_cast<int>(distance / static_cast<float>(f.sw_int) * static_cast<float>(alpha));
        if (alpha < 25) {
            return;
        }
    }
    if (r.lines.empty()) {
        return;
    }
    const bool pixel = f.theme != Theme::Adjust;
    const FontRef font = font_for_scale(f.theme, r.pool_scale);
    if (pixel) {
        post(f, [&f]() {
            minecraft::gl_state_enable_blend(f.jni);
            minecraft::gl_state_blend_func(f.jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        });
    }
    const Argb background = adjust::argb(0, 0, 0, std::min(145, alpha));
    for (size_t index = 0; index < r.lines.size(); ++index) {
        const PoolLine& line = r.lines[index];
        const float left = x - line.width * 0.5f;
        const Argb color = with_alpha_prefix(alpha, line.color);
        if (pixel) {
            const float top = b.y + r.pool_scale * 14.0f * static_cast<float>(index);
            if (s.background.value) {
                const float padding = r.pool_scale * 4.0f;
                adjust::fill_rect(background, left - padding, top - padding, left + line.width + padding,
                                  top + r.pool_scale * 6.0f + padding);
            }
            const std::string text = line.text;
            post(f, [&f, font, text, left, top, color]() { draw_text_now(f, font, text, left, top, color); });
        } else {
            const float top = b.y + static_cast<float>(index) * (r.pool_scale * 11.0f);
            draw_text_now(f, font, line.text, left, top, color);
        }
    }
}

// =================================================================================================================
// GL helpers around the NanoVG flush (keeps GlStateManager's cache valid)
// =================================================================================================================

struct GlBindings {
    GLint program = 0;
    GLint vertex_array = 0;
    GLint array_buffer = 0;
    GLint element_buffer = 0;
    GLint uniform_buffer = 0;
    GLint active_texture = GL_TEXTURE0;
};

GlBindings push_gl_state() noexcept {
    GlBindings b{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &b.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &b.vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &b.array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &b.element_buffer);
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &b.uniform_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &b.active_texture);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    return b;
}

void pop_gl_state(const GlBindings& b) noexcept {
    glPopClientAttrib();
    glPopAttrib();
    glUseProgram(static_cast<GLuint>(b.program));
    glBindVertexArray(static_cast<GLuint>(b.vertex_array));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(b.array_buffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(b.element_buffer));
    glBindBuffer(GL_UNIFORM_BUFFER, static_cast<GLuint>(b.uniform_buffer));
    glActiveTexture(static_cast<GLenum>(b.active_texture));
}

// Equipment icons (RenderItem.renderItemIntoGUI at 0.5 scale, zLevel - 300), before the NanoVG flush.
void flush_items(Frame& f) {
    if (f.items.empty()) {
        return;
    }
    events::JniContext* jni = f.jni;
    jobject mc = minecraft::mc_get_instance(jni);
    jobject render_item = minecraft::mc_get_render_item(jni, mc);
    jobject textures = minecraft::mc_get_texture_manager(jni, mc);
    if (render_item != nullptr) {
        const float z_level = minecraft::render_item_get_z_level(jni, render_item);
        minecraft::render_item_set_z_level(jni, render_item, z_level - 300.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
        minecraft::gl_state_enable_depth(jni);
        minecraft::gl_state_enable_texture_2d(jni);
        minecraft::render_helper_enable_item_lighting(jni);
        glPushMatrix();
        glScalef(0.5f, 0.5f, 0.5f);
        for (const Frame::ItemDraw& item : f.items) {
            minecraft::render_item_into_gui(jni, render_item, f.font_renderer, textures, item.stack, item.x, item.y);
            jni->env->ExceptionClear();
        }
        glPopMatrix();
        minecraft::render_helper_disable_item_lighting(jni);
        minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
        minecraft::gl_state_disable_texture_2d(jni);
        minecraft::gl_state_disable_depth(jni);
        minecraft::render_item_set_z_level(jni, render_item, z_level);
    }
    R::drop(jni, textures);
    R::drop(jni, render_item);
    R::drop(jni, mc);
    jni->env->ExceptionClear();
}

// Render2DEvent.runPostTasks: Minecraft-font text and potion icons.
void flush_post(Frame& f) {
    if (f.post.empty()) {
        return;
    }
    events::JniContext* jni = f.jni;
    minecraft::gl_state_enable_texture_2d(jni);
    minecraft::gl_state_enable_alpha(jni);
    minecraft::gl_state_enable_blend(jni);
    minecraft::gl_state_blend_func(jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (auto& task : f.post) {
        task();
    }
    minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
    minecraft::gl_state_disable_texture_2d(jni);
    jni->env->ExceptionClear();
}

// =================================================================================================================
// Events
// =================================================================================================================

void on_post_living_update(void* raw_event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(raw_event)->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || !R::available(ctx->jni)) {
        return;
    }
    JNIEnv* env = ctx->jni->env;
    if (ctx->world == nullptr || ctx->local_player == nullptr) {
        release_tracker(env);
        return;
    }
    if (g_world == nullptr || !env->IsSameObject(g_world, ctx->world)) {
        release_tracker(env);
        g_world = env->NewGlobalRef(ctx->world);
    }
    update_tracker(ctx);
    rebuild_renderers(ctx);
    env->ExceptionClear();
}

void on_render_world_2d(void* raw_event) noexcept {
    const auto* ev = static_cast<const events::RenderWorld2DProjEvent*>(raw_event);
    if (g_renderers.empty() || ev->ctx == nullptr || ev->ctx->jni == nullptr || !R::available(ev->ctx->jni) ||
        ev->modelview_matrix == nullptr || ev->projection_matrix == nullptr || ev->ctx->local_player == nullptr) {
        return;
    }
    events::JniContext* jni = ev->ctx->jni;
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(64) != 0) {
        env->ExceptionClear();
        return;
    }

    Frame f{};
    f.ev = ev;
    f.jni = jni;
    f.theme = settings().theme_value();
    const float fb_w = ev->viewport_size.x;
    const float fb_h = ev->viewport_size.y;
    f.sf = adjust::resolve_hud_scale(2.0f, fb_w, fb_h);
    f.sw = fb_w / f.sf;
    f.sh = fb_h / f.sf;
    f.sw_int = static_cast<int>(std::ceil(f.sw));
    jobject render_manager = minecraft::mc_get_render_manager(jni, ev->ctx->minecraft);
    f.font_renderer = render_manager != nullptr ? minecraft::render_manager_get_font_renderer(jni, render_manager)
                                                : nullptr;
    f.local_pos = minecraft::entity_get_position(jni, ev->ctx->local_player);

    // collectRenderPositions
    std::vector<Bounds> bounds(g_renderers.size());
    std::vector<math::Vec3d> centers(g_renderers.size());
    std::vector<bool> visible(g_renderers.size(), false);
    bool any_visible = false;
    for (size_t i = 0; i < g_renderers.size(); ++i) {
        visible[i] = project(f, g_renderers[i], &centers[i], &bounds[i]);
        any_visible = any_visible || visible[i];
    }

    core::ClientRuntime& rt = core::runtime();
    NVGcontext* vg = rt.gui_renderer != nullptr ? rt.gui_renderer->vg : nullptr;
    if (any_visible && vg != nullptr) {
        // GUI-style ortho in ESP units (glOrtho(0, w, h, 0, 1000, 3000) + translate(0, 0, -2000)).
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(f.sw), static_cast<double>(f.sh), 0.0, 1000.0, 3000.0);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -2000.0f);

        // NanoVG recording (rectangles, Tahoma text), measuring and per-tick update of the renderers.
        GlBindings bindings = push_gl_state();
        nvgBeginFrame(vg, fb_w, fb_h, 1.0f);
        nvgScale(vg, f.sf, f.sf);
        nvgShapeAntiAlias(vg, 0);
        NVGcontext* previous_kit_vg = adjust::vg();
        adjust::bind_context(vg);
        for (size_t i = 0; i < g_renderers.size(); ++i) {
            if (!visible[i]) {
                continue;
            }
            Renderer& r = g_renderers[i];
            if (!r.initialized) {
                if (r.pooled) {
                    update_pool(f, r, centers[i]);
                } else {
                    update_entity(f, r);
                }
                r.initialized = true;
            }
            if (r.pooled) {
                draw_pool(f, r, bounds[i]);
            } else {
                draw_entity(f, r, bounds[i]);
            }
        }
        adjust::bind_context(previous_kit_vg);
        pop_gl_state(bindings);

        // Items below the NanoVG content, then the NanoVG flush, then the deferred Minecraft-font text.
        flush_items(f);
        bindings = push_gl_state();
        nvgEndFrame(vg);
        pop_gl_state(bindings);
        flush_post(f);

        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
    }
    env->ExceptionClear();
    env->PopLocalFrame(nullptr);
}

void on_module_state_changed(void* raw_event) noexcept {
    const auto* ev = static_cast<const events::ModuleStateChangedEvent*>(raw_event);
    if (ev->module != get_adjust_esp_module()) {
        return;
    }
    JNIEnv* env = (ev->ctx != nullptr && ev->ctx->jni != nullptr) ? ev->ctx->jni->env : nullptr;
    release_tracker(env); // onEnable / onDisable: renderers = new ArrayList / null
}

void on_client_shutdown(void* raw_event) noexcept {
    const auto* ev = static_cast<const events::ContextEvent*>(raw_event);
    JNIEnv* env = (ev->ctx != nullptr && ev->ctx->jni != nullptr) ? ev->ctx->jni->env : nullptr;
    release_tracker(env);
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    settings().render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar) noexcept {
    settings().visit(ar);
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "ESP";
    mod.description = "Draws boxes, bars and name tags around entities.";
    mod.category = events::ModuleCategory::Visual;
    mod.events[events::kEventPostLivingUpdate] = {&on_post_living_update, 0};
    mod.events[events::kEventRenderWorld2DProj] = {&on_render_world_2d, 0};
    mod.events[events::kEventModuleStateChanged] = {&on_module_state_changed, 0};
    mod.events[events::kEventClientShutdown] = {&on_client_shutdown, 0};
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleAdjust;
    return mod;
}

} // namespace

bool adjust_esp_hides_vanilla_name(events::JniContext* jni, jobject entity) noexcept {
    if (!events::module_is_running(get_adjust_esp_module()) || jni == nullptr || entity == nullptr ||
        !R::available(jni) || !R::is_instance(jni, entity, R::Cls::EntityPlayer)) {
        return false;
    }
    if ((minecraft::player_get_tag_flags(jni, entity) & minecraft::kPlayerTagBot) != 0) {
        return false;
    }
    const Settings& s = settings();
    return (s.players.enabled.value && s.players.nametags_enabled.value) ||
           (s.friendly.enabled.value && s.friendly.nametags_enabled.value);
}

events::Module* get_adjust_esp_module() noexcept {
    static events::Module s_module = [] {
        events::Module mod = create_module();
        events::set_module_config_key(&mod, "Adjust ESP");
        return mod;
    }();
    return &s_module;
}

} // namespace slinky::modules
