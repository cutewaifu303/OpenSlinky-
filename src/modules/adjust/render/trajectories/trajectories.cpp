// Port of com.adjustclient.module.impl.render.trajectories.{TrajectoriesModule, TrajectoryRenderer, TrajectoryPath,
// TrajectoryTarget, ProjectileType, ProjectileSnapshot, predictor.*, settings.*}: predicted flight paths of
// projectiles held by players (bow, snowball, egg, ender pearl, splash potion, fire charge) and of projectiles in
// flight (arrows, thrown items, fireballs), with impact markers, entity hit boxes and fireball explosion previews.
//
// The physics live in projectile_sim.cpp (ports of Adjust's ProjectileSimulation / ArrowSimulation /
// PotionSimulation / FireballSimulation / SimulationWorld).
//
// Differences forced by OpenSlinky++ (see the porting report):
//   * Adjust's ServerTracker entity tracker is replaced by a scan of World.loadedEntityList every player tick
//     (same filters: no armor stands / bots, invisible players skipped, thrown potions are not tracked).
//   * "Friendly" = OpenSlinky++ teammate or friend tag (Adjust: Teams detection or whitelist tag).
//   * Hypixel Bed Wars snowball suppression and the slot spoofer's render stack are not ported.
//   * RenderUtil.setupOverlayRendering (view-bobbing re-setup) is not needed: our world pass already has the
//     camera transform of the frame.

#include "modules/adjust/render/trajectories/trajectories.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <bit>
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
#include "modules/adjust/render/block_overlay/render_common.hpp"
#include "modules/adjust/render/trajectories/projectile_sim.hpp"

namespace slinky::modules {

namespace {

namespace ar = adjust_world;
namespace as = adjust_sim;
namespace ajni = adjust::jni;
using adjust::Argb;

// ---- ProjectileType --------------------------------------------------------------------------------------------

enum ProjectileType : int32_t {
    kFireball = 0,
    kArrow = 1,
    kEnderPearl = 2,
    kSnowball = 3,
    kEgg = 4,
    kSplashPotion = 5,
    kTypeCount = 6,
};
constexpr const char* kTypeNames[kTypeCount] = {"Fireball", "Arrow", "Ender Pearl", "Snowball", "Egg", "Splash Potion"};

enum class Predictor : uint8_t { Throwable, Potion, Arrow, Fireball };

constexpr int32_t kItemBow = 261;
constexpr int32_t kItemSnowball = 332;
constexpr int32_t kItemEgg = 344;
constexpr int32_t kItemEnderPearl = 368;
constexpr int32_t kItemPotion = 373;
constexpr int32_t kItemFireCharge = 385;

// ---- settings --------------------------------------------------------------------------------------------------

Argb setting_argb(const adjust::ColorOption& option) noexcept {
    const Argb c = option.get();
    return option.current_mode() == adjust::ColorMode::Main ? c : adjust::with_alpha(c, option.alpha);
}

// TrajectoryOwnerSettings (+ PlayerTrajectorySettings when `player`)
struct OwnerSettings {
    const char* name;
    bool player;
    std::string prefix;
    config::BoolSetting enabled{{}, "Enabled", true, true, {}};
    adjust::ColorOption color;
    adjust::ColorOption hit_color;
    config::BoolSetting fireballs{{}, "Fireballs", true, true, {}};
    config::BoolSetting explosion{{}, "Explosion", false, false, {}};
    adjust::ColorOption explosion_color;
    config::BoolSetting explosion_outline{{}, "Explosion Outline", true, true, {}};
    adjust::ColorOption explosion_outline_color;
    config::BoolSetting held[kTypeCount];
    config::BoolSetting fov_alpha{{}, "FOV Alpha", false, false, {}};
    std::string keys[16];

    OwnerSettings(const char* n, const char* key_prefix, bool is_player, uint32_t rgb) noexcept
        : name(n),
          player(is_player),
          prefix(key_prefix),
          color("Color", "Color mode", 0xAA000000u | rgb),
          hit_color("Hit Color", "Hit Color mode", 0x82000000u | rgb),
          explosion_color("Explosion Color", "Explosion Color mode", 0x46000000u | rgb),
          explosion_outline_color("Explosion Outline Color", "Explosion Outline Color mode", 0x64000000u | rgb) {
        for (int32_t i = 0; i < kTypeCount; ++i) {
            held[i] = config::BoolSetting{{}, kTypeNames[i], true, true, {}};
        }
        const char* suffixes[16] = {"enabled", "color", "color_mode", "hit_color", "hit_color_mode", "fireballs",
                                    "explosion", "explosion_color", "explosion_color_mode", "explosion_outline",
                                    "explosion_outline_color", "explosion_outline_color_mode", "fov_alpha",
                                    "held_", "", ""};
        for (int32_t i = 0; i < 16; ++i) {
            keys[i] = prefix + "_" + suffixes[i];
        }
    }

    void render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
        gui::widgets::render_section_header(gui, renderer, name);
        enabled.render(gui, renderer);
        if (!enabled.value) {
            return;
        }
        color.render(gui, renderer);
        hit_color.render(gui, renderer);
        fireballs.render(gui, renderer);
        if (fireballs.value) {
            explosion.render(gui, renderer);
            if (explosion.value) {
                explosion_color.render(gui, renderer);
                explosion_outline.render(gui, renderer);
                explosion_outline_color.render(gui, renderer);
            }
        }
        if (player) {
            std::string header = std::string(name) + " - Held Projectiles";
            gui::widgets::render_section_header(gui, renderer, header.c_str());
            for (config::BoolSetting& h : held) {
                h.render(gui, renderer);
            }
            fov_alpha.render(gui, renderer);
        }
    }

    void visit(config::ConfigArchive* ar_) noexcept {
        enabled.visit(ar_, keys[0].c_str());
        color.visit(ar_, keys[1].c_str(), keys[2].c_str());
        hit_color.visit(ar_, keys[3].c_str(), keys[4].c_str());
        fireballs.visit(ar_, keys[5].c_str());
        explosion.visit(ar_, keys[6].c_str());
        explosion_color.visit(ar_, keys[7].c_str(), keys[8].c_str());
        explosion_outline.visit(ar_, keys[9].c_str());
        explosion_outline_color.visit(ar_, keys[10].c_str(), keys[11].c_str());
        if (player) {
            fov_alpha.visit(ar_, keys[12].c_str());
            static const char* const kHeldKeys[kTypeCount] = {"fireball", "arrow", "ender_pearl", "snowball", "egg",
                                                              "splash_potion"};
            for (int32_t i = 0; i < kTypeCount; ++i) {
                held_keys[i] = keys[13] + kHeldKeys[i];
                held[i].visit(ar_, held_keys[i].c_str());
            }
        }
    }

    std::string held_keys[kTypeCount];
};

OwnerSettings g_local{"Local", "local", false, 0x435AD1u};
OwnerSettings g_players{"Players", "players", true, 0xE84949u};
OwnerSettings g_friendly{"Friendly", "friendly", true, 0x5B79B2u};

// ProjectileColorSettings
config::BoolSetting g_colors_enabled{{}, "Enabled", true, true, {}};
adjust::ColorOption g_type_colors[kTypeCount] = {
    adjust::ColorOption("Fireball", "Fireball mode", 0x96FF8014u),
    adjust::ColorOption("Arrow", "Arrow mode", 0x96EBEBEBu),
    adjust::ColorOption("Ender Pearl", "Ender Pearl mode", 0xA07828BEu),
    adjust::ColorOption("Snowball", "Snowball mode", 0x96CDE6FFu),
    adjust::ColorOption("Egg", "Egg mode", 0x96E6D8AAu),
    adjust::ColorOption("Splash Potion", "Splash Potion mode", 0x96CD5AC8u),
};

// ExplosionSettings ("Entity Hit Box")
config::BoolSetting g_hitbox_enabled{{}, "Enabled", false, false, {}};
config::FloatSetting g_hitbox_expand{{}, "Expand", "%0.2f", 0.05f, 0.0f, 0.0f, 0.2f, 0.1f, 0.1f};
config::BoolSetting g_hitbox_outline{{}, "Outline", true, true, {}};

// ---- renderers -------------------------------------------------------------------------------------------------

struct Path {
    std::vector<math::Vec3d> points;
    bool has_hit = false;
    as::SimHit hit{};
    bool has_explosion = false;   // FireballPath.affectedBlocks / affectedEntities != null
    as::ExplosionResult explosion{};
};

struct Renderer {
    int32_t entity_id = -1;
    Predictor predictor = Predictor::Throwable;
    int32_t type = kSnowball;
    OwnerSettings* owner = nullptr;
    bool projectile = false;
    bool has_rotation = false;
    math::Vec2f rotation{};
    Path path{};
    bool has_anim = false;
    adjust::Animation anim{};
    // TrajectoryRenderer.alpha() change detection
    math::Vec3d last_pos{};
    math::Vec3d last_motion{};
    math::Vec3d last_accel{};
    int32_t ticks = 0;
    bool initialized = false;
};

as::SimWorld g_world;
as::ShooterTracker g_shooters;
std::unordered_map<int32_t, Renderer> g_renderers;
int32_t g_local_id = -1;

// ---- prediction ------------------------------------------------------------------------------------------------

// ThrowableProjectilePredictor.simulate
void simulate(as::ProjectileSim& sim, Path* path) noexcept {
    path->points.clear();
    path->has_hit = false;
    path->has_explosion = false;
    path->points.push_back(sim.pos);
    for (int32_t tick = 0; tick < 200; ++tick) {
        if (sim.on_update(g_world)) {
            path->points.push_back(sim.collision.vec);
            path->has_hit = true;
            path->hit = sim.collision;
            return;
        }
        path->points.push_back(sim.pos);
    }
}

// FireballPredictor.predict / predictHeld
void simulate_fireball(as::FireballSim& sim, int32_t exploder_id, bool explosion, Path* path) noexcept {
    path->points.clear();
    path->has_hit = false;
    path->has_explosion = false;
    path->points.push_back(sim.pos);
    for (int32_t i = 0; i < 200; ++i) {
        if (sim.on_update(g_world)) {
            path->points.push_back(sim.collision.vec);
            path->has_hit = true;
            path->hit = sim.collision;
            // The explosion is only drawn with "Explosion" enabled; skip the (JNI heavy) simulation otherwise.
            if (explosion) {
                as::explosion_affected(g_world, exploder_id, sim.pos, 1.0f, &path->explosion);
                path->has_explosion = true;
            }
            return;
        }
        path->points.push_back(sim.pos);
    }
}

math::Vec3d interpolate(events::JniContext* jni, jobject entity, float pt) noexcept {
    const math::Vec3d last = ar::entity_last_tick_pos(jni, entity);
    const math::Vec3d pos = minecraft::entity_get_position(jni, entity);
    const double t = static_cast<double>(pt);
    return math::Vec3d{last.x + (pos.x - last.x) * t, last.y + (pos.y - last.y) * t, last.z + (pos.z - last.z) * t};
}

// TrajectoryRenderer.update(rotation, partialTicks)
void predict_player(events::JniContext* jni, Renderer& r, jobject player, math::Vec2f rotation, float pt) noexcept {
    r.rotation = rotation;
    r.has_rotation = true;
    const math::Vec3d origin = interpolate(jni, player, pt);
    const float eye = ar::entity_eye_height(jni, player);
    const int32_t player_id = minecraft::entity_get_id(jni, player);
    switch (r.predictor) {
        case Predictor::Throwable:
        case Predictor::Potion: {
            as::ProjectileSim sim{};
            sim.kind = r.predictor == Predictor::Potion ? as::SimKind::Potion : as::SimKind::Throwable;
            sim.set_throwable_heading(eye, origin.x, origin.y, origin.z, rotation.x, rotation.y);
            sim.source_id = player_id;
            sim.partial_ticks = pt;
            simulate(sim, &r.path);
            break;
        }
        case Predictor::Arrow: {
            // HeldProjectilePredictor.initialize
            float velocity = 2.0f;
            if (minecraft::player_is_using_item(jni, player)) {
                const float duration =
                    (72000.0f - static_cast<float>(minecraft::player_get_item_in_use_count(jni, player)) - 1.0f) + pt;
                if (duration <= 0.0f) {
                    velocity = 0.0f;
                } else {
                    const float charge = duration / 20.0f;
                    const float strength = (charge * charge + charge * 2.0f) / 3.0f;
                    velocity = strength > 1.0f ? 2.0f : strength * 2.0f;
                }
            }
            as::ProjectileSim sim{};
            sim.kind = as::SimKind::Arrow;
            sim.init_arrow(eye, origin.x, origin.y, origin.z, rotation.x, rotation.y, velocity);
            sim.source_id = player_id;
            sim.partial_ticks = pt;
            simulate(sim, &r.path);
            break;
        }
        case Predictor::Fireball: {
            as::FireballSim sim{};
            sim.init_held(player_id, eye, origin.x, origin.y, origin.z, rotation.x, rotation.y);
            sim.partial_ticks = pt;
            simulate_fireball(sim, player_id, r.owner->explosion.value, &r.path);
            break;
        }
    }
    ajni::clear_exception(jni);
}

// TrajectoryRenderer.refresh
void refresh_projectile(events::JniContext* jni, Renderer& r, jobject entity) noexcept {
    if (r.predictor == Predictor::Fireball) {
        as::FireballSim sim{};
        sim.init_from_fireball(jni, entity);
        sim.partial_ticks = 1.0f;
        simulate_fireball(sim, sim.entity_id, r.owner->explosion.value, &r.path);
        r.last_accel = ar::fireball_acceleration(jni, entity);
    } else {
        as::ProjectileSim sim{};
        sim.kind = r.predictor == Predictor::Arrow ? as::SimKind::Arrow
                   : (r.predictor == Predictor::Potion ? as::SimKind::Potion : as::SimKind::Throwable);
        sim.init_from_entity(jni, entity, r.predictor == Predictor::Arrow);
        sim.partial_ticks = 1.0f;
        simulate(sim, &r.path);
    }
    r.ticks = minecraft::entity_get_ticks_existed(jni, entity);
    r.last_pos = minecraft::entity_get_position(jni, entity);
    r.last_motion = minecraft::entity_get_motion(jni, entity);
    r.initialized = true;
    ajni::clear_exception(jni);
}

// TrajectoryRenderer.alpha (projectiles: re-simulate when the entity changed)
void check_projectile(events::JniContext* jni, const ar::Classes* c, Renderer& r, jobject entity) noexcept {
    if (ar::is_instance(jni, entity, c->fireball)) {
        const math::Vec3d accel = ar::fireball_acceleration(jni, entity);
        if (accel.x != r.last_accel.x || accel.y != r.last_accel.y || accel.z != r.last_accel.z) {
            refresh_projectile(jni, r, entity);
            return;
        }
    }
    const math::Vec3d pos = minecraft::entity_get_position(jni, entity);
    const math::Vec3d motion = minecraft::entity_get_motion(jni, entity);
    if (!r.initialized || r.ticks != minecraft::entity_get_ticks_existed(jni, entity) || r.last_pos.x != pos.x ||
        r.last_pos.y != pos.y || r.last_pos.z != pos.z || r.last_motion.x != motion.x ||
        r.last_motion.y != motion.y || r.last_motion.z != motion.z) {
        refresh_projectile(jni, r, entity);
    }
}

// TrajectoriesModule.settingsFor(Entity)
OwnerSettings* settings_for(events::GameContext* ctx, int32_t entity_id) noexcept {
    if (entity_id < 0) {
        return &g_players;
    }
    if (entity_id == g_local_id) {
        return &g_local;
    }
    events::JniContext* jni = ctx->jni;
    const ar::Classes* c = ar::classes(jni);
    jobject entity = minecraft::world_get_entity_by_id(jni, ctx->world, entity_id);
    ajni::clear_exception(jni);
    OwnerSettings* result = &g_players;
    if (entity != nullptr) {
        if (c != nullptr && ar::is_instance(jni, entity, c->player) &&
            minecraft::player_has_tag_flags(jni, entity, minecraft::kPlayerTagTeammate | minecraft::kPlayerTagFriend)) {
            result = &g_friendly;
        }
        jni->env->DeleteLocalRef(entity);
    }
    return result;
}

bool held_enabled(const OwnerSettings* owner, int32_t type) noexcept {
    return !owner->player || owner->held[type].value;
}

// TrajectoriesModule.typeOf
bool type_of(events::JniContext* jni, jobject player, const OwnerSettings* owner, Predictor* predictor,
             int32_t* type) noexcept {
    jobject stack = ar::player_held_stack(jni, player);
    if (stack == nullptr) {
        return false;
    }
    jobject item = minecraft::item_stack_get_item(jni, stack);
    const int32_t item_id = item != nullptr ? minecraft::item_get_id(jni, item) : 0;
    const int32_t meta = minecraft::item_stack_get_damage(jni, stack);
    if (item != nullptr) {
        jni->env->DeleteLocalRef(item);
    }
    jni->env->DeleteLocalRef(stack);
    switch (item_id) {
        case kItemSnowball: *predictor = Predictor::Throwable; *type = kSnowball; break;
        case kItemEgg: *predictor = Predictor::Throwable; *type = kEgg; break;
        case kItemEnderPearl: *predictor = Predictor::Throwable; *type = kEnderPearl; break;
        case kItemPotion: *predictor = Predictor::Potion; *type = kSplashPotion; break;
        case kItemBow: *predictor = Predictor::Arrow; *type = kArrow; break;
        case kItemFireCharge: *predictor = Predictor::Fireball; *type = kFireball; break;
        default: return false;
    }
    if (item_id == kItemBow && !minecraft::player_is_using_item(jni, player)) {
        return false;
    }
    if (item_id == kItemPotion && (meta & 16384) == 0) { // ItemPotion.isSplash
        return false;
    }
    if (item_id == kItemFireCharge && !owner->fireballs.value) {
        return false;
    }
    return true;
}

Renderer make_renderer(int32_t id, Predictor predictor, int32_t type, OwnerSettings* owner, bool projectile) noexcept {
    Renderer r{};
    r.entity_id = id;
    r.predictor = predictor;
    r.type = type;
    r.owner = owner;
    r.projectile = projectile;
    return r;
}

// TrajectoriesModule.rendererFor
bool renderer_for(events::JniContext* jni, jobject entity, int32_t id, Predictor predictor, int32_t type,
                  OwnerSettings* owner, std::unordered_map<int32_t, Renderer>& next) noexcept {
    if (!owner->enabled.value) {
        return false;
    }
    auto it = g_renderers.find(id);
    if (it != g_renderers.end() && it->second.predictor == predictor && it->second.type == type) {
        next.emplace(id, std::move(it->second));
        return true;
    }
    Renderer r = make_renderer(id, predictor, type, owner, true);
    refresh_projectile(jni, r, entity);
    next.emplace(id, std::move(r));
    return true;
}

// TrajectoriesModule.updateAll (Adjust: EntityPlayerSP.onUpdate after the motion update)
void on_post_living_update(void* raw) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(raw)->ctx;
    if (ctx == nullptr || ctx->world == nullptr || ctx->local_player == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    const ar::Classes* c = ar::classes(jni);
    if (c == nullptr) {
        return;
    }
    JNIEnv* env = jni->env;
    g_local_id = minecraft::entity_get_id(jni, ctx->local_player);
    g_world.capture(ctx);
    g_shooters.update(ctx);

    std::unordered_map<int32_t, Renderer> next;
    jobject list = minecraft::world_get_loaded_entity_list(jni, ctx->world);
    const int32_t count = list != nullptr ? minecraft::java_list_size(jni, list) : 0;
    for (int32_t i = 0; i < count; ++i) {
        if (env->PushLocalFrame(32) != 0) {
            ajni::clear_exception(jni);
            break;
        }
        jobject entity = minecraft::java_list_get(jni, list, i);
        if (entity == nullptr || ajni::clear_exception(jni) || ar::is_instance(jni, entity, c->armor_stand)) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        const int32_t id = minecraft::entity_get_id(jni, entity);
        if (ar::is_instance(jni, entity, c->player)) {
            const bool local = id == g_local_id;
            if ((minecraft::player_get_tag_flags(jni, entity) & minecraft::kPlayerTagBot) != 0 ||
                (!local && ar::entity_is_invisible(jni, entity))) {
                env->PopLocalFrame(nullptr);
                continue;
            }
            OwnerSettings* owner = local ? &g_local
                : (minecraft::player_has_tag_flags(jni, entity, minecraft::kPlayerTagTeammate | minecraft::kPlayerTagFriend)
                       ? &g_friendly
                       : &g_players);
            Predictor predictor{};
            int32_t type = 0;
            if (owner->enabled.value && type_of(jni, entity, owner, &predictor, &type) && held_enabled(owner, type)) {
                auto it = g_renderers.find(id);
                Renderer r = (it != g_renderers.end() && it->second.predictor == predictor && it->second.type == type &&
                              !it->second.projectile)
                                 ? std::move(it->second)
                                 : make_renderer(id, predictor, type, owner, false);
                r.owner = owner;
                predict_player(jni, r, entity, minecraft::entity_get_rotation(jni, entity), 1.0f);
                next.emplace(id, std::move(r));
            }
        } else if (ar::is_instance(jni, entity, c->fireball)) {
            // TrajectoriesModule.updateEntity(TrackedFireball)
            const as::ShooterTracker::Info* info = g_shooters.find(id);
            OwnerSettings* owner = settings_for(ctx, info != nullptr ? info->shooter_id : -1);
            if (owner->enabled.value && owner->fireballs.value && !minecraft::entity_is_dead(jni, entity)) {
                renderer_for(jni, entity, id, Predictor::Fireball, kFireball, owner, next);
            }
        } else if (ar::is_instance(jni, entity, c->arrow)) {
            // TrajectoriesModule.shouldRender(TrackedOtherEntity)
            if (!minecraft::entity_is_dead(jni, entity) && !minecraft::entity_arrow_is_in_ground(jni, entity)) {
                int32_t shooter_id = -1;
                jobject shooter = ar::arrow_shooting_entity(jni, entity);
                if (shooter != nullptr && ar::is_instance(jni, shooter, c->living)) {
                    shooter_id = minecraft::entity_get_id(jni, shooter);
                }
                renderer_for(jni, entity, id, Predictor::Arrow, kArrow, settings_for(ctx, shooter_id), next);
            }
        } else if (ar::is_instance(jni, entity, c->snowball) || ar::is_instance(jni, entity, c->egg) ||
                   ar::is_instance(jni, entity, c->ender_pearl)) {
            // TrackedThrowable (thrown potions are not part of Adjust's entity tracker)
            if (!minecraft::entity_is_dead(jni, entity)) {
                const int32_t type = ar::is_instance(jni, entity, c->ender_pearl) ? kEnderPearl
                                     : ar::is_instance(jni, entity, c->snowball) ? kSnowball
                                                                                 : kEgg;
                const as::ShooterTracker::Info* info = g_shooters.find(id);
                renderer_for(jni, entity, id, Predictor::Throwable, type,
                             settings_for(ctx, info != nullptr ? info->shooter_id : -1), next);
            }
        }
        ajni::clear_exception(jni);
        env->PopLocalFrame(nullptr);
    }
    if (list != nullptr) {
        env->DeleteLocalRef(list);
    }
    g_renderers.swap(next);
}

// ---- rendering -------------------------------------------------------------------------------------------------

struct Frame {
    events::GameContext* ctx = nullptr;
    math::Vec3d cam{};
    float pt = 1.0f;
    int32_t third_person = 0;
};

void vertex(const Frame& f, const math::Vec3d& p) noexcept {
    glVertex3d(p.x - f.cam.x, p.y - f.cam.y, p.z - f.cam.z);
}

math::Vec3d rotate_pitch(const math::Vec3d& v, float pitch) noexcept {
    const float c = ar::mh_cos(pitch);
    const float s = ar::mh_sin(pitch);
    return math::Vec3d{v.x, v.y * c + v.z * s, v.z * c - v.y * s};
}

math::Vec3d rotate_yaw(const math::Vec3d& v, float yaw) noexcept {
    const float c = ar::mh_cos(yaw);
    const float s = ar::mh_sin(yaw);
    return math::Vec3d{v.x * c + v.z * s, v.y, v.z * c - v.x * s};
}

// ThrowableProjectilePredictor.getImpactVertices
void impact_vertices(const Frame& f, const as::SimHit& hit) noexcept {
    const math::Vec3d corners[4] = {{-0.25, 0.0, -0.25}, {0.25, 0.0, 0.25}, {-0.25, 0.0, 0.25}, {0.25, 0.0, -0.25}};
    for (math::Vec3d p : corners) {
        switch (ar::facing_axis(hit.side)) {
            case 0: p = rotate_yaw(rotate_pitch(p, 1.5707999f), 1.5707999f); break;
            case 2: p = rotate_pitch(p, 1.5707999f); break;
            default: break;
        }
        vertex(f, math::vec3_add(hit.vec, p));
    }
}

// TrajectoriesModule.getHitBoxVertices (side == null)
void hit_box_vertices(const Frame& f, const math::AABB& box, const math::Vec3d& hit) noexcept {
    const double x = std::min(std::abs(hit.x - box.min.x), std::abs(hit.x - box.max.x));
    const double y = std::min(std::abs(hit.y - box.min.y), std::abs(hit.y - box.max.y));
    const double z = std::min(std::abs(hit.z - box.min.z), std::abs(hit.z - box.max.z));
    const int axis = y < x ? (z < y ? 2 : 1) : (z < x ? (z < y ? 2 : 1) : 0);
    const double hx = hit.x;
    const double hy = hit.y;
    const double hz = hit.z;
    if (axis == 0) {
        vertex(f, {hx, hy - .25, hz - .25}); vertex(f, {hx, hy + .25, hz + .25});
        vertex(f, {hx, hy - .25, hz + .25}); vertex(f, {hx, hy + .25, hz - .25});
    } else if (axis == 1) {
        vertex(f, {hx - .25, hy, hz - .25}); vertex(f, {hx + .25, hy, hz + .25});
        vertex(f, {hx - .25, hy, hz + .25}); vertex(f, {hx + .25, hy, hz - .25});
    } else {
        vertex(f, {hx - .25, hy - .25, hz}); vertex(f, {hx + .25, hy + .25, hz});
        vertex(f, {hx - .25, hy + .25, hz}); vertex(f, {hx + .25, hy - .25, hz});
    }
}

Argb with_alpha_bits(int alpha, Argb rgb) noexcept {
    return (static_cast<Argb>(alpha & 0xFF) << 24) | (rgb & 0xFFFFFFu);
}

// TrajectoriesModule.drawEntityHit (alpha < 0 = null)
void draw_entity_hit(const Frame& f, int32_t entity_id, const math::Vec3d* hit, const OwnerSettings* owner,
                     const Argb* override_color, int alpha) noexcept {
    const as::SimEntity* e = g_world.find(entity_id);
    if (e == nullptr || (e->local && f.third_person == 0)) {
        return;
    }
    Argb argb = override_color != nullptr ? *override_color : setting_argb(owner->hit_color);
    if (g_hitbox_enabled.value) {
        const double pt = static_cast<double>(f.pt);
        const double x = e->last.x + (e->pos.x - e->last.x) * pt;
        const double y = e->last.y + (e->pos.y - e->last.y) * pt;
        const double z = e->last.z + (e->pos.z - e->last.z) * pt;
        const double expand = static_cast<double>(g_hitbox_expand.value);
        const double half_x = (e->box.max.x - e->box.min.x) * .5;
        const double half_z = (e->box.max.z - e->box.min.z) * .5;
        const double height = e->box.max.y - e->box.min.y;
        const math::AABB box{{x - half_x - expand, y, z - half_z - expand},
                             {x + half_x + expand, y + height + (e->sneaking ? -.25 : 0.0) + expand,
                              z + half_z + expand}};
        const math::AABB rel = ar::box_offset(box, -f.cam.x, -f.cam.y, -f.cam.z);
        ar::gl_color(argb);
        ar::draw_filled_box(rel);
        if (g_hitbox_outline.value) {
            glLineWidth(1.0f);
            ar::gl_color(with_alpha_bits(std::min(255, adjust::alpha_of(argb) + 30), argb));
            ar::draw_outlined_box(rel);
        }
    } else if (hit != nullptr) {
        if (alpha >= 0) {
            if (alpha < 25) {
                return;
            }
            argb = with_alpha_bits(alpha, argb);
        }
        glBegin(GL_LINES);
        ar::gl_color(argb);
        hit_box_vertices(f, e->box, *hit);
        glEnd();
    }
}

// ThrowableProjectilePredictor.render
void render_throwable_hit(const Frame& f, const Path& path, bool translated, const OwnerSettings* owner,
                          const Argb* override_color, int alpha) noexcept {
    const as::SimHit& hit = path.hit;
    if (hit.type == 1) {
        if (alpha < 25) {
            return;
        }
        if (!translated) {
            ar::begin_lines();
        }
        glBegin(GL_LINES);
        ar::gl_color(with_alpha_bits(alpha, override_color != nullptr ? *override_color : setting_argb(owner->hit_color)));
        impact_vertices(f, hit);
        glEnd();
        if (!translated) {
            ar::end_lines();
        }
    } else if (hit.type == 2) {
        if (!g_hitbox_enabled.value) {
            ar::begin_lines();
            draw_entity_hit(f, hit.entity_id, &hit.vec, owner, override_color, alpha);
            ar::end_lines();
        } else {
            ar::begin3d();
            draw_entity_hit(f, hit.entity_id, &hit.vec, owner, override_color, alpha);
            ar::end3d();
        }
    }
}

// FireballPredictor.render
void render_fireball_hit(const Frame& f, const Path& path, const OwnerSettings* owner, const Argb* override_color,
                         int alpha) noexcept {
    const as::SimHit& hit = path.hit;
    const bool explosion = owner->explosion.value;
    if (hit.type == 1) {
        if (alpha >= 25) {
            glBegin(GL_LINES);
            ar::gl_color(with_alpha_bits(alpha, override_color != nullptr ? *override_color
                                                                          : setting_argb(owner->hit_color)));
            impact_vertices(f, hit);
            glEnd();
        }
        if (!explosion) {
            return;
        }
    }
    std::vector<int32_t> entities;
    if (explosion && path.has_explosion) {
        entities = path.explosion.entity_ids;
    }
    if (hit.type == 2 && std::find(entities.begin(), entities.end(), hit.entity_id) == entities.end()) {
        entities.push_back(hit.entity_id);
    }
    if (!explosion) {
        entities.clear();
        if (hit.type == 2) {
            entities.push_back(hit.entity_id);
        }
    }
    if (!entities.empty()) {
        const bool boxes = g_hitbox_enabled.value;
        if (boxes) ar::begin3d(); else ar::begin_lines();
        for (const int32_t id : entities) {
            const bool is_hit = hit.type == 2 && hit.entity_id == id;
            draw_entity_hit(f, id, is_hit ? &hit.vec : nullptr, owner, override_color, explosion ? -1 : alpha);
        }
        if (boxes) ar::end3d(); else ar::end_lines();
    }
    if (!explosion || !path.has_explosion || path.explosion.block_boxes.empty()) {
        return;
    }
    const auto scaled_alpha = [](Argb c) {
        return (static_cast<Argb>(static_cast<int>(static_cast<float>(c >> 24) * 0.45f)) << 24) | (c & 0xFFFFFFu);
    };
    const Argb fill = override_color != nullptr ? scaled_alpha(*override_color) : setting_argb(owner->explosion_color);
    const Argb outline = override_color != nullptr ? scaled_alpha(*override_color)
                                                   : setting_argb(owner->explosion_outline_color);
    ar::begin3d();
    for (const math::AABB& b : path.explosion.block_boxes) {
        const math::AABB rel = ar::box_offset(b, -f.cam.x, -f.cam.y, -f.cam.z);
        ar::gl_color(fill);
        ar::draw_filled_box(rel);
        if (owner->explosion_outline.value) {
            glLineWidth(1.0f);
            ar::gl_color(outline);
            ar::draw_outlined_box(rel);
        }
    }
    ar::end3d();
}

// RotationUtil.getYawBetween(target, origin)
math::Vec2f yaw_between(const math::Vec3d& target, const math::Vec3d& origin) noexcept {
    const double x = target.x - origin.x;
    const double y = target.y - origin.y;
    const double z = target.z - origin.z;
    const double horizontal = std::sqrt(x * x + z * z);
    return math::Vec2f{static_cast<float>(std::atan2(z, x) * 180.0 / math::kPi - 90.0),
                       static_cast<float>(-(std::atan2(y, horizontal) * 180.0 / math::kPi))};
}

// TrajectoriesModule.renderEntity
void render_entity(const Frame& f, const ar::Classes* c, Renderer& r) noexcept {
    events::JniContext* jni = f.ctx->jni;
    jobject entity = minecraft::world_get_entity_by_id(jni, f.ctx->world, r.entity_id);
    ajni::clear_exception(jni);
    if (entity == nullptr || minecraft::entity_is_dead(jni, entity)) {
        return;
    }
    const bool is_player = ar::is_instance(jni, entity, c->player);
    if (!is_player && ar::projectile_in_ground(jni, entity)) {
        return;
    }
    if (r.projectile) {
        check_projectile(jni, c, r, entity);
    }
    if (is_player) {
        const math::Vec2f rot = minecraft::entity_get_rotation(jni, entity);
        const math::Vec2f prev = minecraft::entity_get_prev_rotation(jni, entity);
        const float yaw = prev.x + ar::wrap_degrees(rot.x - prev.x) * f.pt;
        const float pitch = prev.y + (rot.y - prev.y) * f.pt;
        const math::Vec3d pos = minecraft::entity_get_position(jni, entity);
        const math::Vec3d last = ar::entity_last_tick_pos(jni, entity);
        bool moved_target = false;
        if (r.path.has_hit && r.path.hit.type == 2) {
            const as::SimEntity* target = g_world.find(r.path.hit.entity_id);
            moved_target = target != nullptr && (target->last.x != target->pos.x || target->last.y != target->pos.y ||
                                                 target->last.z != target->pos.z);
        }
        const bool drawing_bow = r.predictor == Predictor::Arrow && minecraft::player_is_using_item(jni, entity) &&
                                 72000 - minecraft::player_get_item_in_use_count(jni, entity) < 21;
        if (drawing_bow || pos.x != last.x || pos.y != last.y || pos.z != last.z || moved_target || !r.has_rotation ||
            std::bit_cast<uint32_t>(r.rotation.x) != std::bit_cast<uint32_t>(yaw) ||
            std::bit_cast<uint32_t>(r.rotation.y) != std::bit_cast<uint32_t>(pitch)) {
            predict_player(jni, r, entity, math::Vec2f{yaw, pitch}, f.pt);
        }
    }
    const Path& path = r.path;
    const bool translated = path.points.size() >= 2;
    const OwnerSettings* owner = r.owner;
    Argb override_value = 0;
    const Argb* override_color = nullptr;
    if (g_colors_enabled.value) {
        override_value = setting_argb(g_type_colors[r.type]);
        override_color = &override_value;
    }
    int alpha = 255;
    if (is_player && owner->player && owner->fov_alpha.value) {
        const math::Vec3d local_eyes = math::vec3_add(minecraft::entity_get_position(jni, f.ctx->local_player),
                                                      math::Vec3d{0.0, static_cast<double>(ar::entity_eye_height(jni, f.ctx->local_player)), 0.0});
        const math::Vec3d eyes = math::vec3_add(minecraft::entity_get_position(jni, entity),
                                                math::Vec3d{0.0, static_cast<double>(ar::entity_eye_height(jni, entity)), 0.0});
        const math::Vec2f towards = yaw_between(local_eyes, eyes);
        const float difference = std::abs(ar::wrap_degrees(towards.x - r.rotation.x)) + std::abs(towards.y - r.rotation.y);
        alpha = static_cast<int>(std::max(0.0, std::min(1.0, 1.0 - static_cast<double>(difference / 75.0f))) * 255.0);
    }
    const bool fresh = !r.has_anim;
    if (fresh) {
        r.anim = adjust::Animation(adjust::Easing::EaseOutSine, 180);
        r.has_anim = true;
    }
    const bool entity_hit = path.has_hit && path.hit.type == 2;
    if (fresh && entity_hit) {
        r.anim.value = 1.0f;
    } else {
        r.anim.update(entity_hit ? 1.0f : 0.0f);
    }
    if (translated) {
        ar::begin_lines();
        const Argb blended = adjust::blend(setting_argb(owner->hit_color), setting_argb(owner->color),
                                           static_cast<double>(r.anim.value));
        const Argb color = override_color != nullptr ? *override_color : blended;
        if (alpha >= 25) {
            ar::gl_color(with_alpha_bits(alpha, color));
            glLineWidth(2.0f);
            glBegin(GL_LINE_STRIP);
            if (!r.projectile && r.type == kFireball && r.entity_id == g_local_id) {
                const float yaw = r.rotation.x / 180.0f * static_cast<float>(math::kPi);
                const math::Vec3d first{path.points.front().x - static_cast<double>(ar::mh_cos(yaw) * .16f),
                                        path.points.front().y - .1000000014901161,
                                        path.points.front().z - static_cast<double>(ar::mh_sin(yaw) * .16f)};
                vertex(f, first);
                vertex(f, path.points.back());
            } else {
                for (size_t i = 0; i < path.points.size(); ++i) {
                    vertex(f, r.projectile && i == 0 ? interpolate(jni, entity, f.pt) : path.points[i]);
                }
            }
            glEnd();
        }
    }
    if (path.has_hit) {
        if (r.predictor == Predictor::Fireball) {
            render_fireball_hit(f, path, owner, override_color, alpha);
        } else {
            render_throwable_hit(f, path, translated, owner, override_color, alpha);
        }
    }
    if (translated) {
        ar::end_lines();
    }
}

void on_render_world_3d(void* raw) noexcept {
    const auto* ev = static_cast<const events::RenderWorld3DEvent*>(raw);
    events::GameContext* ctx = ev->ctx;
    if (g_renderers.empty() || ctx == nullptr || ctx->world == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    const ar::Classes* c = ar::classes(jni);
    if (c == nullptr) {
        return;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        ajni::clear_exception(jni);
        return;
    }
    g_world.bind(jni, ctx->world);
    Frame f{};
    f.ctx = ctx;
    f.cam = ev->render_pos;
    f.pt = ev->partial_ticks;
    jobject settings = minecraft::mc_get_game_settings(jni, ctx->minecraft);
    f.third_person = settings != nullptr ? minecraft::settings_get_third_person_view(jni, settings) : 0;
    for (auto& [id, r] : g_renderers) {
        if (env->PushLocalFrame(32) != 0) {
            ajni::clear_exception(jni);
            break;
        }
        render_entity(f, c, r);
        env->PopLocalFrame(nullptr);
    }
    glLineWidth(1.0f);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    ajni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

void on_module_state_changed(void* raw) noexcept {
    const auto* ev = static_cast<const events::ModuleStateChangedEvent*>(raw);
    if (ev->module != get_adjust_trajectories_module()) {
        return;
    }
    g_renderers.clear();
    g_world.clear();
    g_shooters.clear();
}

void on_client_shutdown(void* /*raw*/) noexcept {
    g_renderers.clear();
    g_world.clear();
    g_shooters.clear();
}

// ---- settings UI / config --------------------------------------------------------------------------------------

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_local.render(gui, renderer);
    g_players.render(gui, renderer);
    g_friendly.render(gui, renderer);
    gui::widgets::render_section_header(gui, renderer, "Projectile Colors");
    g_colors_enabled.render(gui, renderer);
    if (g_colors_enabled.value) {
        for (adjust::ColorOption& option : g_type_colors) {
            option.render(gui, renderer);
        }
    }
    gui::widgets::render_section_header(gui, renderer, "Entity Hit Box");
    g_hitbox_enabled.render(gui, renderer);
    if (g_hitbox_enabled.value) {
        g_hitbox_expand.render(gui, renderer);
        g_hitbox_outline.render(gui, renderer);
    }
}

void visit_config(config::ConfigArchive* ar_) noexcept {
    g_local.visit(ar_);
    g_players.visit(ar_);
    g_friendly.visit(ar_);
    g_colors_enabled.visit(ar_, "projectile_colors_enabled");
    static const char* const kColorKeys[kTypeCount][2] = {
        {"projectile_color_fireball", "projectile_color_fireball_mode"},
        {"projectile_color_arrow", "projectile_color_arrow_mode"},
        {"projectile_color_ender_pearl", "projectile_color_ender_pearl_mode"},
        {"projectile_color_snowball", "projectile_color_snowball_mode"},
        {"projectile_color_egg", "projectile_color_egg_mode"},
        {"projectile_color_splash_potion", "projectile_color_splash_potion_mode"},
    };
    for (int32_t i = 0; i < kTypeCount; ++i) {
        g_type_colors[i].visit(ar_, kColorKeys[i][0], kColorKeys[i][1]);
    }
    g_hitbox_enabled.visit(ar_, "entity_hit_box_enabled");
    g_hitbox_expand.visit(ar_, "entity_hit_box_expand");
    g_hitbox_outline.visit(ar_, "entity_hit_box_outline");
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Trajectories";
    mod.description = "Predicts where held and flying projectiles will land.";
    mod.category = events::ModuleCategory::Visual;
    mod.events[events::kEventModuleStateChanged] = {&on_module_state_changed, 0};
    mod.events[events::kEventPostLivingUpdate] = {&on_post_living_update, 0};
    mod.events[events::kEventRenderWorld3D] = {&on_render_world_3d, -1000};
    mod.events[events::kEventClientShutdown] = {&on_client_shutdown, 0};
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleAdjust;
    events::set_module_config_key(&mod, "Adjust Trajectories");
    return mod;
}

} // namespace

events::Module* get_adjust_trajectories_module() noexcept {
    static events::Module s_module = create_module();
    return &s_module;
}

} // namespace slinky::modules
