// Port of com.adjustclient.module.impl.render.blockoverlay.{BlockOverlayModule, BlockOverlaySettings,
// RangedOverlaySettings, ChestOverlaySettings, SpawnerOverlaySettings, BedOverlaySettings, ObsidianOverlaySettings,
// BlockTarget, TileEntityTarget}: filled / outlined boxes on the looked-at block, containers, spawners, beds and
// obsidian.
//
// Differences forced by OpenSlinky++ (see the porting report):
//   * "Looking at" is drawn in our world pass on top of Minecraft's own selection outline: replacing vanilla's
//     outline needs a hook in RenderGlobal.drawSelectionBoundingBox that does not exist.
//   * Beds / obsidian come from a chunk scanner fed by kEventWorldBlockUpdate instead of Adjust's
//     BlockRendererDispatcher hook; tile entities are re-collected when World.loadedTileEntityList changes size
//     (and every second) instead of Adjust's World.updateEntities hook.
//   * Hypixel-only behaviour (lobby check, SkyWars "Hide opened", Bed Wars "Ignore local / Ignore whitelisted /
//     Team color" and "Show missing blocks") depends on Adjust's server / game-state trackers and is not ported.

#include "modules/adjust/render/block_overlay/block_overlay.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/block_overlay/render_common.hpp"

namespace slinky::modules {

namespace {

namespace ar = adjust_world;
namespace ajni = adjust::jni;
using adjust::Argb;

constexpr int32_t kBlockBed = 26;
constexpr int32_t kBlockObsidian = 49;

// ---- settings --------------------------------------------------------------------------------------------------

Argb darker_outline(Argb c) noexcept {
    // new Color((int)(r * 0.7), (int)(g * 0.7), (int)(b * 0.7), a)
    return adjust::argb(static_cast<int>(adjust::red_of(c) * 0.7), static_cast<int>(adjust::green_of(c) * 0.7),
                        static_cast<int>(adjust::blue_of(c) * 0.7), adjust::alpha_of(c));
}

// BlockOverlaySettings (+ RangedOverlaySettings when `ranged`).
struct OverlaySettings {
    const char* name;
    const char* key;
    bool ranged;
    config::BoolSetting full;
    adjust::ColorOption full_color;
    config::BoolSetting outline;
    adjust::ColorOption outline_color;
    config::FloatSetting thickness;
    config::FloatSetting max_range;

    OverlaySettings(const char* n, const char* k, bool is_ranged, Argb color, bool outline_default,
                    bool full_default) noexcept
        : name(n),
          key(k),
          ranged(is_ranged),
          full{{}, "Full", full_default, full_default, {}},
          full_color("Full color", "Full color mode", color),
          outline{{}, "Outline", outline_default, outline_default, {}},
          outline_color("Outline color", "Outline color mode", darker_outline(color)),
          thickness{{}, "Thickness", "%0.1f", 0.1f, 0.0f, 1.0f, 5.0f, 1.0f, 1.0f},
          max_range{{}, "Max. range", "%0.0f (65 = infinite)", 1.0f, 0.0f, 6.0f, 65.0f, 65.0f, 65.0f} {}

    // BlockOverlaySettings.isVisible (Adjust checks the full colour's alpha for both parts).
    bool visible() const noexcept {
        const bool has_alpha = adjust::alpha_of(full_argb()) != 0;
        return (full.value && has_alpha) || (outline.value && has_alpha);
    }

    Argb full_argb() const noexcept { return with_setting_alpha(full_color); }
    Argb outline_argb() const noexcept { return with_setting_alpha(outline_color); }

    static Argb with_setting_alpha(const adjust::ColorOption& option) noexcept {
        // ColorSetting.getColor(): Plain keeps the configured alpha; Rainbow keeps it too; Main = HUD colour.
        const Argb c = option.get();
        return option.current_mode() == adjust::ColorMode::Main ? c : adjust::with_alpha(c, option.alpha);
    }

    void render(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
        gui::widgets::render_section_header(gui, renderer, name);
        full.render(gui, renderer);
        full_color.render(gui, renderer);
        outline.render(gui, renderer);
        outline_color.render(gui, renderer);
        if (outline.value) {
            thickness.render(gui, renderer);
        }
        if (ranged) {
            max_range.render(gui, renderer);
        }
    }

    void visit(config::ConfigArchive* ar_, const char* const keys[7]) noexcept {
        full.visit(ar_, keys[0]);
        full_color.visit(ar_, keys[1], keys[2]);
        outline.visit(ar_, keys[3]);
        outline_color.visit(ar_, keys[4], keys[5]);
        thickness.visit(ar_, keys[6]);
    }
};

OverlaySettings g_looking_at{"Looking at", "looking_at", false, adjust::argb(67, 90, 209, 55), true, true};
OverlaySettings g_chest{"Chest", "chest", true, adjust::argb(231, 153, 76, 55), false, true};
OverlaySettings g_trapped_chest{"Trapped Chest", "trapped_chest", true, adjust::argb(232, 78, 78, 55), false, false};
OverlaySettings g_ender_chest{"Ender Chest", "ender_chest", true, adjust::argb(162, 62, 229, 55), false, false};
OverlaySettings g_hopper{"Hopper", "hopper", true, adjust::argb(103, 103, 102, 55), false, false};
OverlaySettings g_dispenser{"Dispenser", "dispenser", true, adjust::argb(57, 232, 255, 55), false, false};
OverlaySettings g_dropper{"Dropper", "dropper", true, adjust::argb(81, 201, 87, 55), false, false};
OverlaySettings g_mob_spawner{"Mob Spawner", "mob_spawner", true, adjust::argb(237, 99, 36, 55), false, false};
OverlaySettings g_bed{"Bed", "bed", true, adjust::argb(98, 158, 201, 85), false, false};
OverlaySettings g_obsidian{"Obsidian", "obsidian", true, adjust::argb(109, 19, 168, 55), false, false};

config::BoolSetting g_spawner_activated_only{{}, "Activated Only", false, false, {}};
config::BoolSetting g_obsidian_only_beds{{}, "Only surrounding beds", true, true, {}};

OverlaySettings* const kAllSettings[] = {&g_looking_at, &g_chest,       &g_trapped_chest, &g_ender_chest,
                                         &g_hopper,     &g_dispenser,   &g_dropper,       &g_mob_spawner,
                                         &g_bed,        &g_obsidian};

// ---- state -----------------------------------------------------------------------------------------------------

struct TileTarget {
    OverlaySettings* settings = nullptr;
    math::AABB bounds{};
    jobject tile = nullptr; // global ref
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct BlockTarget {
    OverlaySettings* settings = nullptr;
    math::AABB bounds{};
};

ar::BlockIdTracker g_tracker{kBlockBed, kBlockObsidian};
std::vector<TileTarget> g_tiles;
std::vector<BlockTarget> g_blocks;
uint64_t g_blocks_revision = 0;
bool g_blocks_only_beds = true;
bool g_needs_refresh = true;
int32_t g_last_tile_count = -1;
int32_t g_refresh_timer = 0;
jweak g_world = nullptr;
std::vector<ar::TrackedBlock> g_scratch;

int64_t pos_key(int32_t x, int32_t y, int32_t z) noexcept {
    return (static_cast<int64_t>(x & 0x3FFFFFF) << 38) | (static_cast<int64_t>(z & 0x3FFFFFF) << 12) |
           static_cast<int64_t>(y & 0xFFF);
}

void release_tiles(JNIEnv* env) noexcept {
    for (TileTarget& t : g_tiles) {
        if (t.tile != nullptr && env != nullptr) {
            env->DeleteGlobalRef(t.tile);
        }
        t.tile = nullptr;
    }
    g_tiles.clear();
}

void reset_all(JNIEnv* env) noexcept {
    release_tiles(env);
    g_blocks.clear();
    g_blocks_revision = 0;
    g_tracker.clear();
    g_needs_refresh = true;
    g_last_tile_count = -1;
    if (g_world != nullptr && env != nullptr) {
        env->DeleteWeakGlobalRef(g_world);
    }
    g_world = nullptr;
}

// ---- tile entities ---------------------------------------------------------------------------------------------

struct TileClasses {
    jclass tile = nullptr;
    jclass chest = nullptr;
    jclass ender_chest = nullptr;
    jclass dispenser = nullptr;
    jclass dropper = nullptr;
    jclass hopper = nullptr;
    jclass spawner = nullptr;
    jclass spawner_logic = nullptr;
    jmethodID get_block_type = nullptr;
    jmethodID chest_type = nullptr;
    jmethodID chest_check_adjacent = nullptr;
    jfieldID chest_adjacent[4]{}; // ZNeg, XNeg, XPos, ZPos (lookup order of BlockOverlayModule)
    jmethodID spawner_logic_get = nullptr;
    jfieldID spawn_delay = nullptr;
    bool ok = false;
};

const TileClasses& tile_classes(events::JniContext* jni) noexcept {
    static TileClasses s{};
    static bool s_resolved = false;
    if (s_resolved) {
        return s;
    }
    s_resolved = true;
    if (!ajni::is_1_8(jni)) {
        return s;
    }
    s.tile = ajni::find_class(jni, "akw", "net/minecraft/tileentity/TileEntity");
    s.chest = ajni::find_class(jni, "aky", "net/minecraft/tileentity/TileEntityChest");
    s.ender_chest = ajni::find_class(jni, "alf", "net/minecraft/tileentity/TileEntityEnderChest");
    s.dispenser = ajni::find_class(jni, "alc", "net/minecraft/tileentity/TileEntityDispenser");
    s.dropper = ajni::find_class(jni, "ald", "net/minecraft/tileentity/TileEntityDropper");
    s.hopper = ajni::find_class(jni, "alj", "net/minecraft/tileentity/TileEntityHopper");
    s.spawner = ajni::find_class(jni, "all", "net/minecraft/tileentity/TileEntityMobSpawner");
    s.spawner_logic = ajni::find_class(jni, "add", "net/minecraft/tileentity/MobSpawnerBaseLogic");
    if (s.tile == nullptr || s.chest == nullptr || s.ender_chest == nullptr || s.dispenser == nullptr ||
        s.dropper == nullptr || s.hopper == nullptr || s.spawner == nullptr || s.spawner_logic == nullptr) {
        return s;
    }
    s.get_block_type = ajni::method(jni, s.tile, {"w", "getBlockType", "func_145838_q"}, "()Lafh;",
                                    "()Lnet/minecraft/block/Block;");
    s.chest_type = ajni::method(jni, s.chest, {"n", "getChestType", "func_145980_j"}, "()I", "()I");
    s.chest_check_adjacent = ajni::method(jni, s.chest, {"m", "checkForAdjacentChests", "func_145979_i"}, "()V", "()V");
    s.chest_adjacent[0] = ajni::field(jni, s.chest, {"f", "adjacentChestZNeg", "field_145992_i"}, "Laky;",
                                      "Lnet/minecraft/tileentity/TileEntityChest;");
    s.chest_adjacent[1] = ajni::field(jni, s.chest, {"h", "adjacentChestXNeg", "field_145991_k"}, "Laky;",
                                      "Lnet/minecraft/tileentity/TileEntityChest;");
    s.chest_adjacent[2] = ajni::field(jni, s.chest, {"g", "adjacentChestXPos", "field_145990_j"}, "Laky;",
                                      "Lnet/minecraft/tileentity/TileEntityChest;");
    s.chest_adjacent[3] = ajni::field(jni, s.chest, {"i", "adjacentChestZPos", "field_145988_l"}, "Laky;",
                                      "Lnet/minecraft/tileentity/TileEntityChest;");
    s.spawner_logic_get = ajni::method(jni, s.spawner, {"b", "getSpawnerBaseLogic", "func_145881_a"}, "()Ladd;",
                                       "()Lnet/minecraft/tileentity/MobSpawnerBaseLogic;");
    s.spawn_delay = ajni::field(jni, s.spawner_logic, {"a", "spawnDelay", "field_98286_b"}, "I", "I");
    s.ok = s.get_block_type != nullptr && s.chest_type != nullptr;
    return s;
}

// BlockOverlayModule.settingsFor(TileEntity)
OverlaySettings* settings_for(events::JniContext* jni, const TileClasses& tc, jobject tile) noexcept {
    JNIEnv* env = jni->env;
    if (env->IsInstanceOf(tile, tc.chest)) {
        const int32_t type = env->CallIntMethod(tile, tc.chest_type);
        ajni::clear_exception(jni);
        return type == 1 ? &g_trapped_chest : &g_chest;
    }
    if (env->IsInstanceOf(tile, tc.ender_chest)) return &g_ender_chest;
    if (env->IsInstanceOf(tile, tc.dropper)) return &g_dropper;
    if (env->IsInstanceOf(tile, tc.dispenser)) return &g_dispenser;
    if (env->IsInstanceOf(tile, tc.hopper)) return &g_hopper;
    if (env->IsInstanceOf(tile, tc.spawner)) return &g_mob_spawner;
    return nullptr;
}

// BlockOverlayModule.acceptsTileEntity (true = hidden). SkyWars "Hide opened" is Hypixel-only and not ported.
bool accepts_tile(events::JniContext* jni, const TileClasses& tc, jobject tile, const OverlaySettings* settings) noexcept {
    if (settings == &g_mob_spawner && g_spawner_activated_only.value && tc.spawner_logic_get != nullptr &&
        tc.spawn_delay != nullptr && jni->env->IsInstanceOf(tile, tc.spawner)) {
        jobject logic = jni->env->CallObjectMethod(tile, tc.spawner_logic_get);
        bool hidden = false;
        if (!ajni::clear_exception(jni) && logic != nullptr) {
            hidden = jni->env->GetIntField(logic, tc.spawn_delay) == 20;
            jni->env->DeleteLocalRef(logic);
        }
        return hidden;
    }
    return false;
}

bool tile_pos(events::JniContext* jni, jobject tile, int32_t* x, int32_t* y, int32_t* z) noexcept {
    const math::Vec3d p = minecraft::tile_entity_get_position(jni, tile);
    *x = static_cast<int32_t>(std::floor(p.x));
    *y = static_cast<int32_t>(std::floor(p.y));
    *z = static_cast<int32_t>(std::floor(p.z));
    return true;
}

void refresh_tiles(events::GameContext* ctx, jobject tile_list) noexcept {
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    const TileClasses& tc = tile_classes(jni);
    release_tiles(env);
    if (!tc.ok || tile_list == nullptr) {
        return;
    }
    std::unordered_map<int64_t, size_t> visible;
    const int32_t count = minecraft::java_list_size(jni, tile_list);
    for (int32_t i = 0; i < count; ++i) {
        if (env->PushLocalFrame(24) != 0) {
            ajni::clear_exception(jni);
            break;
        }
        jobject tile = minecraft::java_list_get(jni, tile_list, i);
        if (ajni::clear_exception(jni) || tile == nullptr) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        OverlaySettings* settings = settings_for(jni, tc, tile);
        if (settings == nullptr || !settings->visible()) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        int32_t x = 0, y = 0, z = 0;
        tile_pos(jni, tile, &x, &y, &z);
        // `world.getBlockState(pos).getBlock() != tile.getBlockType()` -> skip
        jobject tile_block = env->CallObjectMethod(tile, tc.get_block_type);
        ajni::clear_exception(jni);
        jobject state = ar::block_state_at(jni, ctx->world, x, y, z);
        jobject world_block = state != nullptr ? minecraft::iblock_state_get_block(jni, state) : nullptr;
        if (tile_block == nullptr || world_block == nullptr || !env->IsSameObject(tile_block, world_block) ||
            accepts_tile(jni, tc, tile, settings)) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        math::AABB bounds{};
        if (!ar::block_selected_box(jni, ctx->world, x, y, z, &bounds)) {
            env->PopLocalFrame(nullptr);
            continue;
        }
        bool skip = false;
        if (env->IsInstanceOf(tile, tc.chest)) {
            if (tc.chest_check_adjacent != nullptr) {
                env->CallVoidMethod(tile, tc.chest_check_adjacent);
                ajni::clear_exception(jni);
            }
            jobject adjacent = nullptr;
            for (const jfieldID fid : tc.chest_adjacent) {
                if (fid != nullptr && adjacent == nullptr) {
                    adjacent = env->GetObjectField(tile, fid);
                }
            }
            if (adjacent != nullptr) {
                int32_t ax = 0, ay = 0, az = 0;
                tile_pos(jni, adjacent, &ax, &ay, &az);
                if (visible.count(pos_key(ax, ay, az)) != 0) {
                    skip = true;
                } else {
                    math::AABB other{};
                    if (ar::block_selected_box(jni, ctx->world, ax, ay, az, &other)) {
                        bounds = ar::box_union(bounds, other);
                    }
                }
            }
        }
        if (!skip) {
            TileTarget target{};
            target.settings = settings;
            target.bounds = bounds;
            target.tile = env->NewGlobalRef(tile);
            target.x = x;
            target.y = y;
            target.z = z;
            visible[pos_key(x, y, z)] = g_tiles.size();
            g_tiles.push_back(target);
        }
        env->PopLocalFrame(nullptr);
    }
}

// ---- beds / obsidian ---------------------------------------------------------------------------------------

// Horizontal EnumFacing from bed metadata (EnumFacing.getHorizontal: S, W, N, E).
constexpr int32_t kBedFacingDx[4] = {0, -1, 0, 1};
constexpr int32_t kBedFacingDz[4] = {1, 0, -1, 0};

void rebuild_block_targets(events::GameContext* ctx) noexcept {
    g_blocks_revision = g_tracker.revision();
    g_blocks_only_beds = g_obsidian_only_beds.value;
    g_blocks.clear();
    g_tracker.collect(&g_scratch);

    std::unordered_set<int64_t> beds;
    for (const ar::TrackedBlock& b : g_scratch) {
        if (b.id == kBlockBed) {
            beds.insert(pos_key(b.x, b.y, b.z));
        }
    }
    std::unordered_set<int64_t> cached_beds;
    for (const ar::TrackedBlock& b : g_scratch) {
        if (b.id == kBlockObsidian) {
            // isBedPartVisible: true when no bed touches the block (UP excluded).
            if (g_obsidian_only_beds.value) {
                bool touches_bed = false;
                for (int32_t f = 0; f < 6 && !touches_bed; ++f) {
                    if (f == ar::kUp) {
                        continue;
                    }
                    touches_bed = beds.count(pos_key(b.x + ar::kFacingOffsets[f][0], b.y + ar::kFacingOffsets[f][1],
                                                     b.z + ar::kFacingOffsets[f][2])) != 0;
                }
                if (!touches_bed) {
                    continue;
                }
            }
            g_blocks.push_back(BlockTarget{&g_obsidian,
                                           math::AABB{{static_cast<double>(b.x), static_cast<double>(b.y),
                                                       static_cast<double>(b.z)},
                                                      {b.x + 1.0, b.y + 1.0, b.z + 1.0}}});
            continue;
        }
        // Bed: one target per bed (both halves united).
        const int64_t key = pos_key(b.x, b.y, b.z);
        if (cached_beds.count(key) != 0) {
            continue;
        }
        ar::BlockInfo info{};
        if (!ar::block_info_at(ctx->jni, ctx->world, b.x, b.y, b.z, &info) || info.id != kBlockBed) {
            continue;
        }
        const int32_t facing = info.meta & 3;
        const bool head = (info.meta & 8) != 0;
        const int32_t sign = head ? -1 : 1; // HEAD -> facing.getOpposite(), FOOT -> facing
        const int32_t ox = b.x + kBedFacingDx[facing] * sign;
        const int32_t oz = b.z + kBedFacingDz[facing] * sign;
        const int64_t other_key = pos_key(ox, b.y, oz);
        if (beds.count(other_key) == 0 || cached_beds.count(other_key) != 0) {
            continue;
        }
        // BlockBed bounds: 0..1 x 0..0.5625 x 0..1
        const math::AABB a{{static_cast<double>(b.x), static_cast<double>(b.y), static_cast<double>(b.z)},
                           {b.x + 1.0, b.y + 0.5625, b.z + 1.0}};
        const math::AABB o{{static_cast<double>(ox), static_cast<double>(b.y), static_cast<double>(oz)},
                           {ox + 1.0, b.y + 0.5625, oz + 1.0}};
        g_blocks.push_back(BlockTarget{&g_bed, ar::box_union(a, o)});
        cached_beds.insert(key);
        cached_beds.insert(other_key);
    }
}

// ---- events ----------------------------------------------------------------------------------------------------

bool world_changed(events::GameContext* ctx) noexcept {
    JNIEnv* env = ctx->jni->env;
    if (g_world != nullptr && env->IsSameObject(g_world, ctx->world)) {
        return false;
    }
    if (g_world != nullptr) {
        env->DeleteWeakGlobalRef(g_world);
    }
    g_world = ctx->world != nullptr ? env->NewWeakGlobalRef(ctx->world) : nullptr;
    return true;
}

void on_post_living_update(void* raw) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(raw)->ctx;
    if (ctx == nullptr || ctx->world == nullptr || ar::classes(ctx->jni) == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    if (world_changed(ctx)) {
        release_tiles(jni->env);
        g_tracker.scan_all(ctx);
        g_needs_refresh = true;
    }

    jobject tile_list = minecraft::world_get_loaded_tile_entity_list(jni, ctx->world);
    const int32_t tile_count = tile_list != nullptr ? minecraft::java_list_size(jni, tile_list) : 0;
    if (tile_count != g_last_tile_count || ++g_refresh_timer >= 20) {
        g_needs_refresh = true;
    }
    if (g_needs_refresh) {
        g_refresh_timer = 0;
        g_last_tile_count = tile_count;
        refresh_tiles(ctx, tile_list);
        g_needs_refresh = false;
    } else {
        const TileClasses& tc = tile_classes(jni);
        for (size_t i = 0; i < g_tiles.size();) {
            if (accepts_tile(jni, tc, g_tiles[i].tile, g_tiles[i].settings)) {
                jni->env->DeleteGlobalRef(g_tiles[i].tile);
                g_tiles.erase(g_tiles.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
    if (tile_list != nullptr) {
        jni->env->DeleteLocalRef(tile_list);
    }

    if (g_tracker.revision() != g_blocks_revision || g_obsidian_only_beds.value != g_blocks_only_beds) {
        rebuild_block_targets(ctx);
    }
}

// BlockOverlayModule.isInRange: true when the box is FARTHER than the configured range.
bool out_of_range(const math::AABB& b, const OverlaySettings* s, const math::Vec3d& player) noexcept {
    if (!s->ranged) {
        return false;
    }
    const int max = static_cast<int>(s->max_range.value);
    if (max >= static_cast<int>(s->max_range.max_val)) {
        return false;
    }
    const double x = std::max(b.min.x, std::min(player.x, b.max.x));
    const double y = std::max(b.min.y, std::min(player.y, b.max.y));
    const double z = std::max(b.min.z, std::min(player.z, b.max.z));
    const double dx = player.x - x;
    const double dy = player.y - y;
    const double dz = player.z - z;
    return dx * dx + dy * dy + dz * dz > static_cast<double>(max * max);
}

// BlockOverlayModule.drawTarget (current line width = Adjust's "prevWidth").
void draw_target(const OverlaySettings* s, const math::AABB& bounds, float base_line_width) noexcept {
    const bool has_alpha = adjust::alpha_of(s->full_argb()) != 0;
    if (s->full.value && has_alpha) {
        ar::gl_color(s->full_argb());
        ar::draw_filled_box(bounds);
    }
    if (s->outline.value && has_alpha) {
        ar::gl_color(s->outline_argb());
        glLineWidth(base_line_width * s->thickness.value);
        ar::draw_outlined_box(bounds);
        glLineWidth(base_line_width);
    }
}

void render_looking_at(events::GameContext* ctx, const math::Vec3d& cam) noexcept {
    if (!g_looking_at.visible()) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        ajni::clear_exception(jni);
        return;
    }
    jobject mop = minecraft::mc_get_object_mouse_over(jni, ctx->minecraft);
    if (mop != nullptr && minecraft::mop_get_type_ordinal(jni, mop) == 1) {
        const minecraft::Vec3i pos = minecraft::mop_get_block_pos_vec3i(jni, mop);
        ar::BlockInfo info{};
        math::AABB box{};
        if (ar::block_info_at(jni, ctx->world, pos.x, pos.y, pos.z, &info) && info.id != 0 &&
            ar::block_selected_box(jni, ctx->world, pos.x, pos.y, pos.z, &box)) {
            // RenderGlobal.drawSelectionBox: expand(0.002).offset(-camera), drawn with depth test, line width 2.
            box = ar::box_offset(ar::box_expand(box, 0.0020000000949949026, 0.0020000000949949026,
                                                0.0020000000949949026),
                                 -cam.x, -cam.y, -cam.z);
            glPushMatrix();
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_TEXTURE_2D);
            glDepthMask(GL_FALSE);
            glEnable(GL_DEPTH_TEST);
            glLineWidth(2.0f);
            draw_target(&g_looking_at, box, 2.0f);
            glDepthMask(GL_TRUE);
            glPopMatrix();
            glDisable(GL_DEPTH_TEST);
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }
    ajni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

void on_render_world_3d(void* raw) noexcept {
    const auto* ev = static_cast<const events::RenderWorld3DEvent*>(raw);
    events::GameContext* ctx = ev->ctx;
    if (ctx == nullptr || ctx->world == nullptr || ar::classes(ctx->jni) == nullptr) {
        return;
    }
    const math::Vec3d cam = ev->render_pos;

    render_looking_at(ctx, cam);

    if (g_tiles.empty() && g_blocks.empty()) {
        return;
    }
    const math::Vec3d player = minecraft::entity_get_position(ctx->jni, ctx->local_player);
    ar::begin3d();
    for (const TileTarget& t : g_tiles) {
        if (!t.settings->visible() || out_of_range(t.bounds, t.settings, player)) {
            continue;
        }
        draw_target(t.settings, ar::box_offset(t.bounds, -cam.x, -cam.y, -cam.z), 0.4f);
    }
    for (const BlockTarget& b : g_blocks) {
        if (!b.settings->visible() || out_of_range(b.bounds, b.settings, player)) {
            continue;
        }
        draw_target(b.settings, ar::box_offset(b.bounds, -cam.x, -cam.y, -cam.z), 0.4f);
    }
    ar::end3d();
}

void on_world_block_update(void* raw) noexcept {
    g_tracker.on_world_block_update(static_cast<const events::WorldBlockUpdateEvent*>(raw));
}

void on_module_state_changed(void* raw) noexcept {
    const auto* ev = static_cast<const events::ModuleStateChangedEvent*>(raw);
    if (ev->module != get_adjust_block_overlay_module()) {
        return;
    }
    JNIEnv* env = (ev->ctx != nullptr && ev->ctx->jni != nullptr) ? ev->ctx->jni->env : nullptr;
    reset_all(env);
    if (ev->enabled && ev->ctx != nullptr && ev->ctx->world != nullptr && ar::classes(ev->ctx->jni) != nullptr) {
        world_changed(ev->ctx);
        g_tracker.scan_all(ev->ctx);
    }
}

void on_client_shutdown(void* raw) noexcept {
    const auto* ev = static_cast<const events::ContextEvent*>(raw);
    JNIEnv* env = (ev != nullptr && ev->ctx != nullptr && ev->ctx->jni != nullptr) ? ev->ctx->jni->env : nullptr;
    reset_all(env);
}

// ---- settings UI / config --------------------------------------------------------------------------------------

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    for (OverlaySettings* s : kAllSettings) {
        s->render(gui, renderer);
        if (s == &g_mob_spawner) {
            g_spawner_activated_only.render(gui, renderer);
        } else if (s == &g_obsidian) {
            g_obsidian_only_beds.render(gui, renderer);
        }
    }
}

void visit_group(config::ConfigArchive* ar_, OverlaySettings& s, const char* full, const char* full_color,
                 const char* full_mode, const char* outline, const char* outline_color, const char* outline_mode,
                 const char* thickness, const char* range) noexcept {
    const char* const keys[7] = {full, full_color, full_mode, outline, outline_color, outline_mode, thickness};
    s.visit(ar_, keys);
    if (s.ranged && range != nullptr) {
        s.max_range.visit(ar_, range);
    }
}

void visit_config(config::ConfigArchive* ar_) noexcept {
    visit_group(ar_, g_looking_at, "looking_at_full", "looking_at_full_color", "looking_at_full_color_mode",
                "looking_at_outline", "looking_at_outline_color", "looking_at_outline_color_mode",
                "looking_at_thickness", nullptr);
    visit_group(ar_, g_chest, "chest_full", "chest_full_color", "chest_full_color_mode", "chest_outline",
                "chest_outline_color", "chest_outline_color_mode", "chest_thickness", "chest_max_range");
    visit_group(ar_, g_trapped_chest, "trapped_chest_full", "trapped_chest_full_color",
                "trapped_chest_full_color_mode", "trapped_chest_outline", "trapped_chest_outline_color",
                "trapped_chest_outline_color_mode", "trapped_chest_thickness", "trapped_chest_max_range");
    visit_group(ar_, g_ender_chest, "ender_chest_full", "ender_chest_full_color", "ender_chest_full_color_mode",
                "ender_chest_outline", "ender_chest_outline_color", "ender_chest_outline_color_mode",
                "ender_chest_thickness", "ender_chest_max_range");
    visit_group(ar_, g_hopper, "hopper_full", "hopper_full_color", "hopper_full_color_mode", "hopper_outline",
                "hopper_outline_color", "hopper_outline_color_mode", "hopper_thickness", "hopper_max_range");
    visit_group(ar_, g_dispenser, "dispenser_full", "dispenser_full_color", "dispenser_full_color_mode",
                "dispenser_outline", "dispenser_outline_color", "dispenser_outline_color_mode",
                "dispenser_thickness", "dispenser_max_range");
    visit_group(ar_, g_dropper, "dropper_full", "dropper_full_color", "dropper_full_color_mode", "dropper_outline",
                "dropper_outline_color", "dropper_outline_color_mode", "dropper_thickness", "dropper_max_range");
    visit_group(ar_, g_mob_spawner, "mob_spawner_full", "mob_spawner_full_color", "mob_spawner_full_color_mode",
                "mob_spawner_outline", "mob_spawner_outline_color", "mob_spawner_outline_color_mode",
                "mob_spawner_thickness", "mob_spawner_max_range");
    g_spawner_activated_only.visit(ar_, "mob_spawner_activated_only");
    visit_group(ar_, g_bed, "bed_full", "bed_full_color", "bed_full_color_mode", "bed_outline", "bed_outline_color",
                "bed_outline_color_mode", "bed_thickness", "bed_max_range");
    visit_group(ar_, g_obsidian, "obsidian_full", "obsidian_full_color", "obsidian_full_color_mode",
                "obsidian_outline", "obsidian_outline_color", "obsidian_outline_color_mode", "obsidian_thickness",
                "obsidian_max_range");
    g_obsidian_only_beds.visit(ar_, "obsidian_only_surrounding_beds");
    // Visibility toggles may have changed: re-collect tile entities on the next tick (Adjust: markRefresh).
    g_needs_refresh = true;
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Block Overlay";
    mod.description = "Highlights the block you look at, containers, spawners, beds and obsidian.";
    mod.category = events::ModuleCategory::Visual;
    mod.events[events::kEventModuleStateChanged] = {&on_module_state_changed, 0};
    mod.events[events::kEventPostLivingUpdate] = {&on_post_living_update, 0};
    mod.events[events::kEventRenderWorld3D] = {&on_render_world_3d, -150};
    mod.events[events::kEventWorldBlockUpdate] = {&on_world_block_update, 0};
    mod.events[events::kEventClientShutdown] = {&on_client_shutdown, 0};
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleAdjust;
    events::set_module_config_key(&mod, "Adjust Block Overlay");
    return mod;
}

} // namespace

events::Module* get_adjust_block_overlay_module() noexcept {
    static events::Module s_module = create_module();
    return &s_module;
}

bool adjust_block_overlay_replaces_vanilla_outline() noexcept {
    return events::module_is_running(get_adjust_block_overlay_module()) && g_looking_at.visible();
}

} // namespace slinky::modules
