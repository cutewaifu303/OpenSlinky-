// Port of com.adjustclient.module.impl.render.indicators.{IndicatorsModule, IndicatorEntry, IndicatorSettings,
// SizedIndicatorSettings, LivingIndicatorSettings, PlayerIndicatorSettings}: off-screen entities are shown as
// markers on a circle around the crosshair (triangles pointing at players / mobs, item icons for projectiles),
// with name, distance and health.
//
// Rendering is split like Adjust's own mix of GL and NanoVG: markers, health bars and item icons are drawn with GL
// in the projected overlay pass (kEventRenderWorld2DProj, scaled to the Adjust HUD resolution), the Tahoma labels
// are drawn with the Adjust kit on the NanoVG HUD layer (kEventGuiRender2D) at the positions computed in that pass.
//
// Differences forced by OpenSlinky++ (see the porting report):
//   * Adjust's ServerTracker entity tracker is replaced by a scan of World.loadedEntityList every player tick, with
//     the same category filters. Its movement fade (TrackedEntity.movementAnimation) is ported.
//   * Tags: "Friendly" = OpenSlinky++ teammate or friend tag; IndicatorEntry.enemy (Adjust: WHITELIST tag) = friend
//     tag. The "Enemy" category needs Adjust's TARGET tag, which OpenSlinky++ has no equivalent for, so it never
//     matches (its settings are kept).
//   * Vanilla RenderItem has no alpha parameter, so projectile icons are drawn opaque.
//   * Murder Mystery roles and the Streamer name replacement are Adjust/Hypixel services and not ported.

#include "modules/adjust/render/indicators/indicators.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/render/block_overlay/render_common.hpp"
#include "modules/adjust/render/trajectories/projectile_sim.hpp"

namespace slinky::modules {

namespace {

namespace ar = adjust_world;
namespace as = adjust_sim;
namespace ajni = adjust::jni;
using adjust::Argb;

constexpr int32_t kItemArrow = 262;
constexpr int32_t kItemSnowball = 332;
constexpr int32_t kItemEgg = 344;
constexpr int32_t kItemEnderPearl = 368;
constexpr int32_t kItemFireCharge = 385;

// ---- settings --------------------------------------------------------------------------------------------------

enum class Kind : uint8_t { Basic, Living, Player }; // IndicatorSettings / LivingIndicatorSettings / Player...

struct GroupSettings {
    const char* name;
    Kind kind;
    std::string prefix;
    config::BoolSetting enabled;
    config::FloatSetting alpha{{}, "Alpha", "%0.0f", 1.0f, 0.0f, 70.0f, 255.0f, 255.0f, 255.0f};
    adjust::ColorOption color;
    config::FloatSetting size{{}, "Size", "%0.1f", 0.5f, 0.0f, 1.0f, 15.0f, 7.0f, 7.0f};
    config::BoolSetting name_enabled{{}, "Name", true, true, {}};
    config::BoolSetting name_team_color{{}, "Name team color", false, false, {}};
    config::BoolSetting distance{{}, "Distance", false, false, {}};
    config::BoolSetting health_enabled{{}, "Health", false, false, {}};
    config::BoolSetting custom_color_enabled{{}, "Custom health color", true, true, {}};
    adjust::ColorOption health_color;
    config::BoolSetting team_color{{}, "Team color", false, false, {}};
    std::string keys[15];

    GroupSettings(const char* n, const char* key_prefix, Kind k, bool on, uint32_t rgb) noexcept
        : name(n),
          kind(k),
          prefix(key_prefix),
          enabled{{}, "Enabled", on, on, {}},
          color("Color", "Color mode", 0xFF000000u | rgb),
          health_color("Health color", "Health color mode", adjust::argb(67, 90, 209)) {
        const char* suffixes[15] = {"enabled", "alpha", "color", "color_mode", "size", "name", "name_team_color",
                                    "distance", "health", "health_custom", "health_color", "health_color_mode",
                                    "team_color", "", ""};
        for (int i = 0; i < 15; ++i) {
            keys[i] = prefix + "_" + suffixes[i];
        }
    }

    bool sized() const noexcept { return kind != Kind::Basic; }

    void render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
        gui::widgets::render_section_header(gui, renderer, name);
        enabled.render(gui, renderer);
        if (!enabled.value) {
            return;
        }
        alpha.render(gui, renderer);
        if (sized()) {
            if (!(kind == Kind::Player && team_color.value)) {
                color.render(gui, renderer);
            }
            size.render(gui, renderer);
        }
        name_enabled.render(gui, renderer);
        if (kind == Kind::Player && name_enabled.value) {
            name_team_color.render(gui, renderer);
        }
        distance.render(gui, renderer);
        if (kind != Kind::Basic) {
            health_enabled.render(gui, renderer);
            if (health_enabled.value) {
                custom_color_enabled.render(gui, renderer);
                if (custom_color_enabled.value) {
                    health_color.render(gui, renderer);
                }
            }
        }
        if (kind == Kind::Player) {
            team_color.render(gui, renderer);
        }
    }

    void visit(config::ConfigArchive* ar_) noexcept {
        enabled.visit(ar_, keys[0].c_str());
        alpha.visit(ar_, keys[1].c_str());
        name_enabled.visit(ar_, keys[5].c_str());
        distance.visit(ar_, keys[7].c_str());
        if (sized()) {
            color.visit(ar_, keys[2].c_str(), keys[3].c_str());
            size.visit(ar_, keys[4].c_str());
            health_enabled.visit(ar_, keys[8].c_str());
            custom_color_enabled.visit(ar_, keys[9].c_str());
            health_color.visit(ar_, keys[10].c_str(), keys[11].c_str());
        }
        if (kind == Kind::Player) {
            name_team_color.visit(ar_, keys[6].c_str());
            team_color.visit(ar_, keys[12].c_str());
        }
    }
};

GroupSettings g_players{"Players", "players", Kind::Player, true, 0xE84949u};
GroupSettings g_friendly{"Friendly", "friendly", Kind::Player, false, 0x5B79B2u};
GroupSettings g_enemy{"Enemy", "enemy", Kind::Player, true, 0xB78A28u};
GroupSettings g_hostile{"Hostile", "hostile", Kind::Living, false, 0x7A7A7Au};
GroupSettings g_passive{"Passive", "passive", Kind::Living, false, 0xC0BBBBu};
GroupSettings g_projectiles{"Projectiles", "projectiles", Kind::Basic, true, 0xFFFFFFu};
config::FloatSetting g_radius{{}, "Radius", "%0.0f", 1.0f, 0.0f, 20.0f, 130.0f, 50.0f, 50.0f};

// ---- entries (IndicatorEntry) ------------------------------------------------------------------------------

struct Entry {
    int32_t id = -1;
    GroupSettings* settings = nullptr;
    bool projectile = false;
    int32_t item_id = kItemSnowball; // projectile icon
    std::string label;
    Argb color = adjust::argb(233, 235, 234);
    std::string distance_label;
    Argb distance_color = adjust::argb(170, 170, 170);
    bool has_health = false;
    float health = 0.0f;
    Argb health_color = 0;
    bool has_team_color = false;
    Argb team_color = 0;
    bool enemy = false;
};

// TrackedEntity.movementAnimation
struct Movement {
    bool active = false;
    bool returning = false;
    adjust::Animation anim{};
    bool seen = false;
};

// Danger state of a thrown item (TrackedThrowable: computed once per projectile).
struct Danger {
    bool computed = false;
    bool dangerous = false;
    bool simulated = false;
    bool has_block = false;
    math::Vec3d block{};
    bool seen = false;
};

std::vector<Entry> g_entries;
std::unordered_map<int32_t, Movement> g_movement;
std::unordered_map<int32_t, Danger> g_danger;
as::ShooterTracker g_shooters;
as::SimWorld g_world;
std::unordered_map<int32_t, jobject> g_icon_stacks; // item id -> ItemStack global ref

// Labels produced by the projected pass, drawn on the NanoVG layer.
struct Label {
    float x = 0.0f;       // anchor (HUD units)
    float y = 0.0f;
    bool centered = true; // x offset = -width(measure) / 2
    std::string text;
    std::string measure;  // text used for the centring width (IndicatorEntry.nameX / distanceX)
    bool small = false;   // TAHOMA_8 instead of TAHOMA_10
    Argb color = 0;
};
std::vector<Label> g_labels;
bool g_labels_drawn = false;

std::string to_utf8(events::JniContext* jni, jstring s) {
    if (s == nullptr) {
        return {};
    }
    const char* chars = jni->env->GetStringUTFChars(s, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) {
        jni->env->ReleaseStringUTFChars(s, chars);
    }
    return out;
}

std::string entity_name(events::JniContext* jni, jobject entity) {
    jstring name = minecraft::entity_get_name(jni, entity);
    ajni::clear_exception(jni);
    std::string out = to_utf8(jni, name);
    if (name != nullptr) {
        jni->env->DeleteLocalRef(name);
    }
    return out;
}

std::string translate(events::JniContext* jni, const char* key) {
    jstring jkey = jni->env->NewStringUTF(key);
    if (jkey == nullptr) {
        ajni::clear_exception(jni);
        return key;
    }
    jstring value = minecraft::stat_collector_translate_to_local(jni, jkey);
    ajni::clear_exception(jni);
    std::string out = value != nullptr ? to_utf8(jni, value) : std::string(key);
    if (value != nullptr) {
        jni->env->DeleteLocalRef(value);
    }
    jni->env->DeleteLocalRef(jkey);
    return out;
}

// WorldUtil.getTeamColor: scoreboard team prefix "§X..." -> FontRenderer colour code of X.
bool team_color_of(events::JniContext* jni, jobject living, Argb* out) {
    static constexpr uint32_t kColors[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
                                             0xFFAA00, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
                                             0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
    jobject team = minecraft::living_get_team(jni, living);
    ajni::clear_exception(jni);
    if (team == nullptr) {
        return false;
    }
    bool ok = false;
    jobject score_team = minecraft::as_score_player_team(jni, team);
    if (score_team != nullptr) {
        jstring prefix = minecraft::score_player_team_get_color_prefix(jni, score_team);
        ajni::clear_exception(jni);
        if (prefix != nullptr) {
            JNIEnv* env = jni->env;
            if (env->GetStringLength(prefix) >= 2) {
                jchar code = 0;
                env->GetStringRegion(prefix, 1, 1, &code);
                const char* digits = "0123456789abcdef";
                for (int i = 0; i < 16; ++i) {
                    if (static_cast<jchar>(digits[i]) == code) {
                        *out = 0xFF000000u | kColors[i];
                        ok = true;
                        break;
                    }
                }
            }
            env->DeleteLocalRef(prefix);
        }
    }
    jni->env->DeleteLocalRef(team);
    return ok;
}

float distance_to(events::JniContext* jni, jobject from, jobject to) noexcept {
    const math::Vec3d a = minecraft::entity_get_position(jni, from);
    const math::Vec3d b = minecraft::entity_get_position(jni, to);
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    const float dz = static_cast<float>(a.z - b.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// TrackedThrowable.isDangerous / getImpactDistanceFactor; TrackedFireball.isDangerous / getImpactDistanceFactor.
// Returns true when dangerous; *factor = blend ratio (1 = keep colour, 0 = full red).
bool projectile_danger(events::GameContext* ctx, jobject entity, int32_t id, bool fireball, bool pearl,
                       float* factor) {
    events::JniContext* jni = ctx->jni;
    const as::ShooterTracker::Info* info = g_shooters.find(id);
    const int32_t local_id = minecraft::entity_get_id(jni, ctx->local_player);
    const bool shooter_local = info != nullptr && info->shooter_id >= 0 && info->shooter_id == local_id;
    const math::Vec3d player = minecraft::entity_get_position(jni, ctx->local_player);
    g_world.bind(jni, ctx->world);

    if (fireball) {
        as::FireballSim sim{};
        sim.init_from_fireball(jni, entity);
        for (int tick = 0; tick < 400; ++tick) {
            if (sim.on_update(g_world)) {
                break;
            }
        }
        bool has_impact = false;
        math::Vec3d impact{};
        if (sim.has_collision) {
            if (sim.collision.type == 2) {
                const as::SimEntity* hit = g_world.find(sim.collision.entity_id);
                if (hit != nullptr) {
                    impact = hit->pos;
                    has_impact = true;
                }
            } else {
                impact = sim.collision.vec;
                has_impact = true;
            }
        }
        const double dist = has_impact ? math::vec3_dist(player, impact) : 0.0;
        *factor = has_impact ? static_cast<float>(std::min(1.0, dist / 8.0)) : 0.0f;
        return !shooter_local && has_impact && dist < 8.0;
    }

    Danger& d = g_danger[id];
    d.seen = true;
    if (!d.computed && !shooter_local) {
        d.computed = true;
        d.dangerous = false;
        if (info != nullptr && info->has_launch) {
            // TrackedThrowable.simulate
            d.simulated = true;
            as::ProjectileSim sim{};
            sim.kind = as::SimKind::Throwable;
            const math::Vec2f rot = minecraft::entity_get_rotation(jni, entity);
            sim.set_throwable_heading(info->shooter_eye, info->launch.x, info->launch.y, info->launch.z, -rot.x, -rot.y);
            do {
                sim.on_update(g_world);
            } while (!sim.has_collision && sim.ticks_in_air < 201);
            if (sim.has_collision) {
                if (sim.collision.type == 2 && sim.collision.entity_id == local_id) {
                    d.dangerous = true;
                } else if (sim.collision.type == 1) {
                    d.has_block = true;
                    d.block = math::Vec3d{static_cast<double>(sim.collision.bx), static_cast<double>(sim.collision.by),
                                          static_cast<double>(sim.collision.bz)};
                    if (pearl) {
                        d.dangerous = math::vec3_dist(player, d.block) < 12.0;
                    }
                }
            }
        }
    }
    const bool dangerous = !shooter_local && d.computed && d.dangerous;
    if (d.simulated && pearl) {
        *factor = d.has_block ? static_cast<float>(std::min(1.0, math::vec3_dist(player, d.block) / 12.0)) : 0.0f;
    } else if (shooter_local) {
        *factor = 1.0f;
    } else {
        *factor = dangerous ? 0.0f : 1.0f;
    }
    return dangerous;
}

// TrackedEntity.update (movement fade)
void update_movement(events::JniContext* jni, int32_t id, jobject entity) {
    Movement& m = g_movement[id];
    m.seen = true;
    const math::Vec3d last = ar::entity_last_tick_pos(jni, entity);
    const math::Vec3d pos = minecraft::entity_get_position(jni, entity);
    if (math::vec3_dist(last, pos) > 0.01 && !m.active) {
        m.anim = adjust::Animation(adjust::Easing::EaseInOutCubic, 540);
        m.anim.value = 1.0f;
        m.active = true;
    }
    if (m.active) {
        m.anim.update(m.returning ? 1.0f : 0.43f);
        if (m.anim.finished) {
            if (m.returning) {
                m.active = false;
                m.returning = false;
            } else {
                m.returning = true;
            }
        }
    }
}

// IndicatorsModule.collectIndicators + IndicatorEntry.update
void on_post_living_update(void* raw) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(raw)->ctx;
    g_entries.clear();
    if (ctx == nullptr || ctx->world == nullptr || ctx->local_player == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    const ar::Classes* c = ar::classes(jni);
    if (c == nullptr) {
        return;
    }
    JNIEnv* env = jni->env;
    if (g_projectiles.enabled.value) {
        // Only projectiles need the physics snapshot / shooter attribution (danger colouring, shooter label).
        g_world.capture(ctx);
        g_shooters.update(ctx);
    }
    for (auto& [id, m] : g_movement) {
        m.seen = false;
    }
    for (auto& [id, d] : g_danger) {
        d.seen = false;
    }
    const int32_t local_id = minecraft::entity_get_id(jni, ctx->local_player);

    jobject list = minecraft::world_get_loaded_entity_list(jni, ctx->world);
    const int32_t count = list != nullptr ? minecraft::java_list_size(jni, list) : 0;
    for (int32_t i = 0; i < count; ++i) {
        if (env->PushLocalFrame(48) != 0) {
            ajni::clear_exception(jni);
            break;
        }
        jobject entity = minecraft::java_list_get(jni, list, i);
        if (entity == nullptr || ajni::clear_exception(jni)) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        const int32_t id = minecraft::entity_get_id(jni, entity);
        // EntityTracker filter
        const bool is_player = ar::is_instance(jni, entity, c->player);
        const bool is_throwable = ar::is_instance(jni, entity, c->snowball) || ar::is_instance(jni, entity, c->egg) ||
                                  ar::is_instance(jni, entity, c->ender_pearl);
        const bool is_arrow = ar::is_instance(jni, entity, c->arrow);
        const bool is_fireball = ar::is_instance(jni, entity, c->fireball);
        const bool is_living = !is_player && ar::is_instance(jni, entity, c->entity_living);
        if (id == local_id || ar::is_instance(jni, entity, c->armor_stand) ||
            !(is_player || is_throwable || is_arrow || is_fireball || is_living) ||
            (is_player && (minecraft::player_get_tag_flags(jni, entity) & minecraft::kPlayerTagBot) != 0)) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        update_movement(jni, id, entity);

        GroupSettings* selected = nullptr;
        if (is_throwable || is_arrow || is_fireball) {
            if (g_projectiles.enabled.value && !(is_arrow && minecraft::entity_arrow_is_in_ground(jni, entity))) {
                selected = &g_projectiles;
            }
        } else if (!ar::entity_is_invisible(jni, entity)) {
            if (is_player) {
                const bool teammate =
                    minecraft::player_has_tag_flags(jni, entity, minecraft::kPlayerTagTeammate | minecraft::kPlayerTagFriend);
                selected = teammate ? &g_friendly : &g_players;
            } else {
                const bool hostile = ar::is_instance(jni, entity, c->mob) || ar::is_instance(jni, entity, c->iron_golem);
                selected = hostile ? &g_hostile : &g_passive;
            }
        }
        if (selected == nullptr || !selected->enabled.value) {
            env->PopLocalFrame(nullptr);
            continue;
        }

        Entry e{};
        e.id = id;
        e.settings = selected;
        e.projectile = selected == &g_projectiles;
        e.enemy = is_player && minecraft::player_has_tag_flags(jni, entity, minecraft::kPlayerTagFriend);

        float factor = 1.0f;
        bool dangerous = false;
        if (e.projectile) {
            if (is_fireball) e.item_id = kItemFireCharge;
            else if (is_arrow) e.item_id = kItemArrow;
            else if (ar::is_instance(jni, entity, c->egg)) e.item_id = kItemEgg;
            else if (ar::is_instance(jni, entity, c->ender_pearl)) e.item_id = kItemEnderPearl;
            else e.item_id = kItemSnowball;
            if (is_throwable || is_fireball) {
                dangerous = projectile_danger(ctx, entity, id, is_fireball, e.item_id == kItemEnderPearl, &factor);
            }
        }

        // updateLabel
        if (selected->name_enabled.value) {
            if (is_fireball) {
                e.label = "Fireball";
            } else if (e.item_id == kItemEgg && is_throwable) {
                e.label = translate(jni, "item.egg.name");
            } else if (e.item_id == kItemEnderPearl && is_throwable) {
                e.label = translate(jni, "item.enderPearl.name");
            } else {
                e.label = entity_name(jni, entity);
            }
            if (is_throwable || is_fireball) {
                const as::ShooterTracker::Info* info = g_shooters.find(id);
                if (info != nullptr && info->shooter_id >= 0) {
                    std::string shooter;
                    if (info->shooter_id == local_id) {
                        shooter = "You";
                    } else {
                        jobject s = minecraft::world_get_entity_by_id(jni, ctx->world, info->shooter_id);
                        ajni::clear_exception(jni);
                        if (s != nullptr) {
                            shooter = entity_name(jni, s);
                            env->DeleteLocalRef(s);
                        }
                    }
                    if (!shooter.empty()) {
                        e.label += ADJUST_FMT "7 (" ADJUST_FMT "f" + shooter + ADJUST_FMT "7)";
                    }
                }
            }
            if (selected->kind == Kind::Player && selected->name_team_color.value) {
                e.has_team_color = team_color_of(jni, entity, &e.team_color);
                if (e.has_team_color) {
                    e.color = e.team_color;
                }
            }
            if (dangerous) {
                e.color = adjust::blend(e.color, adjust::argb(208, 69, 69), static_cast<double>(factor));
            }
            if (e.enemy) {
                e.color = adjust::argb(210, 53, 53);
            }
        }
        if (selected->distance.value) {
            e.distance_label = std::to_string(static_cast<long>(std::lround(distance_to(jni, ctx->local_player, entity)))) + "m";
            if (dangerous) {
                e.distance_color = adjust::blend(e.distance_color, adjust::argb(234, 93, 93), static_cast<double>(factor));
            }
        }
        // updateHealthColor
        if (selected->kind != Kind::Basic && selected->health_enabled.value) {
            const float maximum = minecraft::living_get_max_health(jni, entity);
            const float hp = minecraft::living_get_health(jni, entity);
            e.health = maximum > 0.0f ? std::min(hp, maximum) / maximum : 0.0f;
            e.has_health = true;
            e.health_color = selected->custom_color_enabled.value
                                 ? selected->health_color.get()
                                 : adjust::hsb_to_rgb(e.health * 0.33333334f, 0.6f, 0.6f);
        }
        if (selected->kind == Kind::Player && selected->team_color.value && !e.has_team_color) {
            e.has_team_color = team_color_of(jni, entity, &e.team_color);
        }
        ajni::clear_exception(jni);
        g_entries.push_back(std::move(e));
        env->PopLocalFrame(nullptr);
    }
    if (list != nullptr) {
        env->DeleteLocalRef(list);
    }
    for (auto it = g_movement.begin(); it != g_movement.end();) {
        it = it->second.seen ? std::next(it) : g_movement.erase(it);
    }
    for (auto it = g_danger.begin(); it != g_danger.end();) {
        it = it->second.seen ? std::next(it) : g_danger.erase(it);
    }
}

// ---- rendering -------------------------------------------------------------------------------------------------

// Frustum test of a camera-relative box with the matrices of the projected pass (RenderUtil.isInFrustum).
bool in_frustum(const events::RenderWorld2DProjEvent* ev, const math::AABB& rel) noexcept {
    const float* mv = ev->modelview_matrix;
    const float* pr = ev->projection_matrix;
    if (mv == nullptr || pr == nullptr) {
        return false;
    }
    int outside[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        const float x = static_cast<float>((i & 1) ? rel.max.x : rel.min.x);
        const float y = static_cast<float>((i & 2) ? rel.max.y : rel.min.y);
        const float z = static_cast<float>((i & 4) ? rel.max.z : rel.min.z);
        const float ex = mv[0] * x + mv[4] * y + mv[8] * z + mv[12];
        const float ey = mv[1] * x + mv[5] * y + mv[9] * z + mv[13];
        const float ez = mv[2] * x + mv[6] * y + mv[10] * z + mv[14];
        const float ew = mv[3] * x + mv[7] * y + mv[11] * z + mv[15];
        const float cx = pr[0] * ex + pr[4] * ey + pr[8] * ez + pr[12] * ew;
        const float cy = pr[1] * ex + pr[5] * ey + pr[9] * ez + pr[13] * ew;
        const float cz = pr[2] * ex + pr[6] * ey + pr[10] * ez + pr[14] * ew;
        const float cw = pr[3] * ex + pr[7] * ey + pr[11] * ez + pr[15] * ew;
        outside[0] += cx < -cw;
        outside[1] += cx > cw;
        outside[2] += cy < -cw;
        outside[3] += cy > cw;
        outside[4] += cz < -cw;
        outside[5] += cz > cw;
    }
    for (const int o : outside) {
        if (o == 8) {
            return false;
        }
    }
    return true;
}

jobject icon_stack(events::JniContext* jni, int32_t item_id) noexcept {
    auto it = g_icon_stacks.find(item_id);
    if (it != g_icon_stacks.end()) {
        return it->second;
    }
    jobject result = nullptr;
    const ar::Classes* c = ar::classes(jni);
    if (c != nullptr) {
        static const jmethodID init = ajni::method(jni, c->item_stack, {"<init>", "<init>", "<init>"}, "(Lzw;II)V",
                                                   "(Lnet/minecraft/item/Item;II)V");
        jobject item = minecraft::item_get_by_id(jni, item_id);
        if (item != nullptr && init != nullptr) {
            jobject stack = jni->env->NewObject(c->item_stack, init, item, 1, 0);
            if (!ajni::clear_exception(jni) && stack != nullptr) {
                result = jni->env->NewGlobalRef(stack);
                jni->env->DeleteLocalRef(stack);
            }
        }
        if (item != nullptr) {
            jni->env->DeleteLocalRef(item);
        }
    }
    g_icon_stacks.emplace(item_id, result);
    return result;
}

void release_icons(JNIEnv* env) noexcept {
    if (env != nullptr) {
        for (auto& [id, stack] : g_icon_stacks) {
            if (stack != nullptr) {
                env->DeleteGlobalRef(stack);
            }
        }
    }
    g_icon_stacks.clear();
}

void quad(float x, float y, float width, float height) noexcept {
    glVertex2f(x, y);
    glVertex2f(x, y + height);
    glVertex2f(x + width, y + height);
    glVertex2f(x + width, y);
}

void gl_argb(Argb c) noexcept {
    glColor4f(static_cast<float>(adjust::red_of(c)) / 255.0f, static_cast<float>(adjust::green_of(c)) / 255.0f,
              static_cast<float>(adjust::blue_of(c)) / 255.0f, static_cast<float>(adjust::alpha_of(c)) / 255.0f);
}

struct RenderCtx {
    events::GameContext* ctx = nullptr;
    jobject render_item = nullptr;
    jobject font = nullptr;
    jobject textures = nullptr;
};

// IndicatorsModule.drawIndicator (GL part) + label positions for the NanoVG pass.
void draw_indicator(const RenderCtx& rc, const events::RenderWorld2DProjEvent* ev, const Entry& entry, float radius,
                    float yaw, float aspect, float center_x, float center_y, int32_t riding_id) {
    events::JniContext* jni = rc.ctx->jni;
    jobject entity = minecraft::world_get_entity_by_id(jni, rc.ctx->world, entry.id);
    ajni::clear_exception(jni);
    if (entity == nullptr) {
        return;
    }
    const math::Vec3d cam = ev->render_pos;
    const math::Vec3d pos = minecraft::entity_get_position(jni, entity);
    const math::Vec3d prev = minecraft::entity_get_prev_position(jni, entity);
    const double pt = static_cast<double>(ev->partial_ticks);
    const math::AABB box = ar::entity_box(jni, entity);
    if (in_frustum(ev, ar::box_offset(box, -cam.x, -cam.y, -cam.z))) {
        return;
    }
    int alpha = static_cast<int>(entry.settings->alpha.value);
    auto movement = g_movement.find(entry.id);
    if (movement != g_movement.end() && movement->second.active) {
        alpha = static_cast<int>(static_cast<float>(alpha) * movement->second.anim.value);
    }
    if (alpha < 25) {
        return;
    }
    const double x = prev.x + (pos.x - prev.x) * pt;
    const double z = prev.z + (pos.z - prev.z) * pt;
    float angle = 180.0f;
    if (entry.id != riding_id) {
        // RotationUtil.getYawTo(yaw, camX, camZ, x, z) - yaw
        const float target = static_cast<float>(std::atan2(z - cam.z, x - cam.x) * 180.0 / math::kPi) - 90.0f;
        angle = ar::wrap_degrees(target - yaw);
    }
    const float radians = static_cast<float>(static_cast<double>(angle) * 0.0174532925199433);
    const float dx = ar::mh_sin(radians) * radius * aspect;
    const float dy = -ar::mh_cos(radians) * radius;
    const float ox = center_x + dx;
    const float oy = center_y + dy;

    // drawLabels -> NanoVG layer
    const bool living = entry.settings->sized();
    const float half = living ? entry.settings->size.value * 0.5f : 5.0f;
    if (entry.settings->name_enabled.value) {
        Label l{};
        l.x = ox;
        l.y = oy - half - 5.5f;
        l.text = (entry.enemy ? std::string(ADJUST_FMT "l") : std::string()) + entry.label;
        l.measure = entry.label;
        l.color = adjust::with_alpha(entry.color, alpha);
        g_labels.push_back(std::move(l));
    }
    if (entry.settings->distance.value) {
        float offset = living ? 3.0f : 4.0f;
        if (entry.has_health) {
            offset = 7.0f;
        }
        Label l{};
        l.x = ox;
        l.y = oy + half + offset;
        l.text = entry.distance_label;
        l.measure = entry.distance_label;
        l.small = true;
        l.color = adjust::with_alpha(entry.distance_color, alpha);
        g_labels.push_back(std::move(l));
    }

    glPushMatrix();
    glTranslatef(ox, oy, 0.0f);
    minecraft::gl_state_disable_texture_2d(jni);
    minecraft::gl_state_enable_blend(jni);
    minecraft::gl_state_blend_func(jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // drawHealth
    if (entry.has_health) {
        const float size = entry.settings->size.value;
        const float width = size + 3.0f;
        const float y = size * 0.5f + 2.0f;
        const float left = -width * 0.5f;
        glBegin(GL_QUADS);
        glColor4f(0.0f, 0.0f, 0.0f, static_cast<float>(alpha) / 255.0f);
        quad(left, y, width, 2.0f);
        gl_argb((static_cast<Argb>(alpha) << 24) | (entry.health_color & 0xFFFFFFu));
        quad(left + 0.5f, y + 0.5f, (width - 1.0f) * entry.health, 1.0f);
        glEnd();
    }

    // drawMarker
    if (living) {
        glRotatef(angle, 0.0f, 0.0f, 1.0f);
        glEnable(GL_POLYGON_SMOOTH);
        glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
        Argb color = (entry.settings->kind == Kind::Player && entry.settings->team_color.value && entry.has_team_color)
                         ? entry.team_color
                         : entry.settings->color.get();
        gl_argb(adjust::with_alpha(color, alpha));
        const float h = entry.settings->size.value * 0.5f;
        glBegin(GL_TRIANGLES);
        glVertex2f(0.0f, -h);
        glVertex2f(-h, h);
        glVertex2f(h, h);
        glEnd();
        glDisable(GL_POLYGON_SMOOTH);
        glHint(GL_POLYGON_SMOOTH_HINT, GL_FASTEST);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    } else {
        glDisable(GL_POLYGON_SMOOTH);
        glHint(GL_POLYGON_SMOOTH_HINT, GL_FASTEST);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        jobject stack = icon_stack(jni, entry.item_id);
        if (stack != nullptr && rc.render_item != nullptr) {
            minecraft::gl_state_enable_texture_2d(jni);
            minecraft::gl_state_disable_blend(jni);
            minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
            glTranslatef(-8.0f, -8.0f, 0.0f);
            minecraft::render_item_into_gui(jni, rc.render_item, rc.font, rc.textures, stack, 0, 0);
            ajni::clear_exception(jni);
            minecraft::gl_state_disable_texture_2d(jni);
            minecraft::gl_state_enable_blend(jni);
            minecraft::gl_state_blend_func(jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }
    glPopMatrix();
}

void on_render_world_2d(void* raw) noexcept {
    const auto* ev = static_cast<const events::RenderWorld2DProjEvent*>(raw);
    g_labels.clear();
    events::GameContext* ctx = ev->ctx;
    if (g_entries.empty() || ctx == nullptr || ctx->world == nullptr || ar::classes(ctx->jni) == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    const float scale = adjust::resolve_hud_scale(adjust_hud::requested_hud_scale(), ev->viewport_size.x,
                                                  ev->viewport_size.y);
    if (scale <= 0.0f) {
        return;
    }
    if (env->PushLocalFrame(32) != 0) {
        ajni::clear_exception(jni);
        return;
    }
    const float width = ev->viewport_size.x / scale;
    const float height = ev->viewport_size.y / scale;
    const float center_x = width * 0.5f;
    const float center_y = height * 0.5f;
    const float aspect = width / height;

    // ThirdPersonModule.getYaw(): yaw of the camera entity.
    jobject view = minecraft::mc_get_render_view_entity(jni, ctx->minecraft);
    float yaw = 0.0f;
    if (view != nullptr) {
        const math::Vec2f rot = minecraft::entity_get_rotation(jni, view);
        const math::Vec2f prev = minecraft::entity_get_prev_rotation(jni, view);
        yaw = prev.x + (rot.x - prev.x) * ev->partial_ticks;
    }
    int32_t riding_id = -2;
    jobject riding = minecraft::entity_get_riding_entity(jni, ctx->local_player);
    if (riding != nullptr) {
        riding_id = minecraft::entity_get_id(jni, riding);
    }

    RenderCtx rc{};
    rc.ctx = ctx;
    rc.render_item = minecraft::mc_get_render_item(jni, ctx->minecraft);
    jobject render_manager = minecraft::mc_get_render_manager(jni, ctx->minecraft);
    rc.font = render_manager != nullptr ? minecraft::render_manager_get_font_renderer(jni, render_manager) : nullptr;
    rc.textures = minecraft::mc_get_texture_manager(jni, ctx->minecraft);

    glPushMatrix();
    glScalef(scale, scale, 1.0f);
    minecraft::gl_state_disable_depth(jni);
    minecraft::gl_state_disable_lighting(jni);
    for (const Entry& entry : g_entries) {
        if (env->PushLocalFrame(16) != 0) {
            ajni::clear_exception(jni);
            break;
        }
        draw_indicator(rc, ev, entry, g_radius.value, yaw, aspect, center_x, center_y, riding_id);
        env->PopLocalFrame(nullptr);
    }
    glPopMatrix();
    // Back to the projected-pass state.
    minecraft::gl_state_disable_texture_2d(jni);
    minecraft::gl_state_disable_depth(jni);
    minecraft::gl_state_enable_blend(jni);
    minecraft::gl_state_blend_func(jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
    ajni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

void on_gui_frame_poll(void* raw) noexcept {
    auto* ev = static_cast<events::GuiFramePollEvent*>(raw);
    if (ev->ctx == nullptr || ev->ctx->world == nullptr) {
        g_labels.clear(); // no world pass this frame: drop the labels of the last one
    }
    if (!g_labels.empty() || g_labels_drawn) {
        ev->render_flags |= 3u;
    }
}

void on_gui_render_2d(void* raw) noexcept {
    const auto* ev = static_cast<const events::GuiRender2DEvent*>(raw);
    g_labels_drawn = !g_labels.empty();
    if (g_labels.empty() || ev->renderer == nullptr) {
        return;
    }
    const adjust::HudFrame frame = adjust::begin_hud(ev->renderer, adjust_hud::requested_hud_scale());
    if (frame.valid) {
        for (const Label& l : g_labels) {
            const adjust::Font& font = l.small ? adjust::kTahoma8 : adjust::kTahoma10;
            const float x = l.x - font.width(l.measure) * 0.5f;
            font.draw(l.text, x, l.y, l.color);
        }
    }
    adjust::end_hud();
}

void reset_all(JNIEnv* env) noexcept {
    g_entries.clear();
    g_movement.clear();
    g_danger.clear();
    g_shooters.clear();
    g_world.clear();
    g_labels.clear();
    (void)env;
}

void on_module_state_changed(void* raw) noexcept {
    const auto* ev = static_cast<const events::ModuleStateChangedEvent*>(raw);
    if (ev->module != get_adjust_indicators_module()) {
        return;
    }
    reset_all(nullptr);
    if (!ev->enabled) {
        g_labels_drawn = false;
    }
}

void on_client_shutdown(void* raw) noexcept {
    const auto* ev = static_cast<const events::ContextEvent*>(raw);
    JNIEnv* env = (ev != nullptr && ev->ctx != nullptr && ev->ctx->jni != nullptr) ? ev->ctx->jni->env : nullptr;
    reset_all(env);
    release_icons(env);
}

// ---- settings UI / config --------------------------------------------------------------------------------------

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_players.render(gui, renderer);
    g_friendly.render(gui, renderer);
    g_enemy.render(gui, renderer);
    g_hostile.render(gui, renderer);
    g_passive.render(gui, renderer);
    g_projectiles.render(gui, renderer);
    gui::widgets::render_section_header(gui, renderer, "General");
    g_radius.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar_) noexcept {
    g_players.visit(ar_);
    g_friendly.visit(ar_);
    g_enemy.visit(ar_);
    g_hostile.visit(ar_);
    g_passive.visit(ar_);
    g_projectiles.visit(ar_);
    g_radius.visit(ar_, "radius");
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Indicators";
    mod.description = "Points at off-screen players, mobs and projectiles around the crosshair.";
    mod.category = events::ModuleCategory::Visual;
    mod.events[events::kEventModuleStateChanged] = {&on_module_state_changed, 0};
    mod.events[events::kEventPostLivingUpdate] = {&on_post_living_update, -20};
    mod.events[events::kEventRenderWorld2DProj] = {&on_render_world_2d, -300};
    mod.events[events::kEventGuiFramePoll] = {&on_gui_frame_poll, 0};
    mod.events[events::kEventGuiRender2D] = {&on_gui_render_2d, 0};
    mod.events[events::kEventClientShutdown] = {&on_client_shutdown, 0};
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleAdjust;
    events::set_module_config_key(&mod, "Adjust Indicators");
    return mod;
}

} // namespace

events::Module* get_adjust_indicators_module() noexcept {
    static events::Module s_module = create_module();
    return &s_module;
}

} // namespace slinky::modules
