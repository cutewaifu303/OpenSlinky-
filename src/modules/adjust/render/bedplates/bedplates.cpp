// Port of com.adjustclient.module.impl.render.bedplates.{BedPlatesModule, BedPlatesRenderer, BedPlatesSettings,
// BedPlatesMode, BedTracker, BedPlate, BedLayer, BedScanResult}: a small plate above every bed listing the blocks
// of its defence, either as one row of block icons ("Blocks") or as one column per defence layer ("Layers").
//
// Rendering happens in the projected overlay pass (kEventRenderWorld2DProj, GL in framebuffer pixels), scaled to
// Adjust's fixed ScaledResolution(mc, 2.0); block icons are drawn with Minecraft's own RenderItem like Adjust.
//
// Differences forced by OpenSlinky++ (see the porting report):
//   * Beds are found by a chunk scanner (kEventWorldBlockUpdate) instead of Adjust's BlockRendererDispatcher hook;
//     a plate is rebuilt when a block changes near it (same 12-block / 500 ms rule as BedPlate.markDirty).
//   * Vanilla RenderItem has no alpha parameter (Adjust patched one in), so icons are drawn opaque; the
//     background and border use the configured alpha.
//   * Hypixel-only parts (Bed Wars lobby check, "Ignore local", "Ignore whitelisted", team colour, "incomplete"
//     defence detection) depend on Adjust's game-state / server trackers and are not ported: the border uses the
//     HUD colour and the background is always the plain dark one.

#include "modules/adjust/render/bedplates/bedplates.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"
#include "modules/adjust/render/block_overlay/render_common.hpp"
#include "util/time.hpp"

namespace slinky::modules {

namespace {

namespace ar = adjust_world;
namespace ajni = adjust::jni;
using adjust::Argb;

constexpr int32_t kBlockAir = 0;
constexpr int32_t kBlockBed = 26;
constexpr int32_t kBlockLadder = 65;
constexpr int32_t kBlockStainedGlass = 95;
constexpr int32_t kItemBed = 355;

constexpr const char* kModeOptions[2] = {"Blocks", "Layers"};
enum Mode : int32_t { kModeBlocks = 0, kModeLayers = 1 };

config::ModeSetting g_mode{{}, "Mode", 2, 0, kModeOptions, kModeBlocks, kModeBlocks, false, {}};
config::FloatSetting g_alpha{{}, "Alpha", "%0.0f%%", 1.0f, 0.0f, 70.0f, 100.0f, 85.0f, 85.0f};

// BedPlatesSettings.getAlpha
int plate_alpha() noexcept {
    return static_cast<int>(static_cast<float>(static_cast<int>(g_alpha.value)) / 100.0f * 255.0f);
}

// ---- model -----------------------------------------------------------------------------------------------------

struct LayerBlock {
    int32_t block_id = 0;
    int32_t metadata = 0; // BedLayer.metadata (damageDropped of the last state seen)
};

struct Layer {
    int32_t index = 0;
    std::vector<LayerBlock> blocks; // insertion order (Adjust: HashMap<Block, Set<BlockPos>>)
    int32_t count = 0;              // number of positions

    LayerBlock* find(int32_t id) noexcept {
        for (LayerBlock& b : blocks) {
            if (b.block_id == id) {
                return &b;
            }
        }
        return nullptr;
    }
};

struct Plate {
    math::Vec3d center{};
    std::vector<Layer> layers;
    std::vector<LayerBlock> blocks; // BedPlate.blocks (linked map Block -> metadata)
    double created_at = 0.0;        // BedPlate.timer
    bool dirty = false;
    bool incomplete = false;        // Hypixel-only (ServerTracker), always false

    void rebuild_blocks() {
        blocks.clear();
        for (const Layer& layer : layers) {
            for (const LayerBlock& b : layer.blocks) {
                auto it = std::find_if(blocks.begin(), blocks.end(),
                                       [&](const LayerBlock& e) { return e.block_id == b.block_id; });
                if (it == blocks.end()) {
                    blocks.push_back(b);
                } else {
                    it->metadata = b.metadata;
                }
            }
        }
    }

    void mark_dirty() noexcept {
        if (time::now_seconds() - created_at >= 0.5) {
            dirty = true;
        }
    }
};

int64_t pos_key(int32_t x, int32_t y, int32_t z) noexcept {
    return (static_cast<int64_t>(x & 0x3FFFFFF) << 38) | (static_cast<int64_t>(z & 0x3FFFFFF) << 12) |
           static_cast<int64_t>(y & 0xFFF);
}

struct BedPos {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct PlateEntry {
    BedPos pos{};
    Plate plate{};
};

ar::BlockIdTracker g_tracker{kBlockBed};
std::vector<PlateEntry> g_plates; // BedTracker.beds (insertion order)
uint64_t g_seen_revision = 0;
jweak g_world = nullptr;
std::vector<ar::TrackedBlock> g_scratch;
std::vector<ar::TrackedBlock> g_changes;

// Item stack cache for icons (global refs), key = block id << 4 | meta, bed = -1.
std::unordered_map<int32_t, jobject> g_stacks;

PlateEntry* find_plate(int32_t x, int32_t y, int32_t z) noexcept {
    for (PlateEntry& e : g_plates) {
        if (e.pos.x == x && e.pos.y == y && e.pos.z == z) {
            return &e;
        }
    }
    return nullptr;
}

void release_stacks(JNIEnv* env) noexcept {
    if (env != nullptr) {
        for (auto& [key, stack] : g_stacks) {
            if (stack != nullptr) {
                env->DeleteGlobalRef(stack);
            }
        }
    }
    g_stacks.clear();
}

void reset_all(JNIEnv* env) noexcept {
    g_plates.clear();
    g_tracker.clear();
    g_seen_revision = 0;
    if (g_world != nullptr && env != nullptr) {
        env->DeleteWeakGlobalRef(g_world);
    }
    g_world = nullptr;
}

// ---- scanning (BedTracker) ---------------------------------------------------------------------------------

// Horizontal EnumFacing index from bed metadata (getHorizontal: S, W, N, E) -> EnumFacing ordinal.
constexpr int32_t kHorizontalToFacing[4] = {ar::kSouth, ar::kWest, ar::kNorth, ar::kEast};

struct ScanContext {
    events::JniContext* jni = nullptr;
    jobject world = nullptr;
    std::unordered_map<int64_t, ar::BlockInfo> cache;

    ar::BlockInfo get(int32_t x, int32_t y, int32_t z) {
        const int64_t key = pos_key(x, y, z);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
        ar::BlockInfo info{};
        if (!ar::block_info_at(jni, world, x, y, z, &info)) {
            info = ar::BlockInfo{};
        }
        cache.emplace(key, info);
        return info;
    }
};

// BedScanResult pair: optional min / max per axis.
struct Range {
    bool has_min = false;
    bool has_max = false;
    int32_t min = 0;
    int32_t max = 0;
    bool active = false;
};

bool in_range(int32_t x, int32_t z, const Range& rx, const Range& rz) noexcept {
    if (rx.active && ((rx.has_min && x < rx.min) || (rx.has_max && x > rx.max))) {
        return false;
    }
    return !rz.active || ((!rz.has_min || z >= rz.min) && (!rz.has_max || z <= rz.max));
}

bool defence_block(events::JniContext* jni, int32_t id) noexcept {
    return id != kBlockAir && (ar::block_is_full_block(jni, id) || id == kBlockLadder || id == kBlockStainedGlass);
}

void add_to_layer(ScanContext& sc, Plate& plate, int32_t layer_index, const ar::BlockInfo& info) {
    Layer& layer = plate.layers[static_cast<size_t>(layer_index)];
    LayerBlock* entry = layer.find(info.id);
    const int32_t dropped = ar::block_damage_dropped(sc.jni, info.id, info.meta);
    if (entry == nullptr) {
        layer.blocks.push_back(LayerBlock{info.id, dropped});
    } else {
        entry->metadata = dropped;
    }
    ++layer.count;
}

// BedTracker.scanNeighbors - `excluded` is shared through the whole recursion exactly like Adjust's HashSet.
void scan_neighbors(ScanContext& sc, int32_t x, int32_t y, int32_t z, int32_t depth, uint32_t& excluded,
                    Plate& plate, const Range& rx, const Range& rz) {
    for (int32_t f = 0; f < 6; ++f) {
        if (f == ar::kDown || (excluded & (1u << f)) != 0) {
            continue;
        }
        const int32_t nx = x + ar::kFacingOffsets[f][0];
        const int32_t ny = y + ar::kFacingOffsets[f][1];
        const int32_t nz = z + ar::kFacingOffsets[f][2];
        if (!in_range(nx, nz, rx, rz)) {
            continue;
        }
        const ar::BlockInfo info = sc.get(nx, ny, nz);
        if (!defence_block(sc.jni, info.id)) {
            continue;
        }
        if (depth > static_cast<int32_t>(plate.layers.size())) {
            Layer layer{};
            layer.index = depth;
            plate.layers.push_back(layer);
        }
        add_to_layer(sc, plate, depth - 1, info);
        excluded |= 1u << ar::facing_opposite(f);
        if (depth < 7) {
            scan_neighbors(sc, nx, ny, nz, depth + 1, excluded, plate, rx, rz);
        }
    }
}

// BedTracker.collect
void collect(ScanContext& sc, int32_t bx, int32_t by, int32_t bz, Plate& plate, int32_t facing) {
    Range rx{};
    Range rz{};
    switch (facing) {
        case ar::kNorth: rz.active = true; rz.has_max = true; rz.max = bz; break;
        case ar::kSouth: rz.active = true; rz.has_min = true; rz.min = bz; break;
        case ar::kWest: rx.active = true; rx.has_min = true; rx.min = bx; break;
        case ar::kEast: rx.active = true; rx.has_max = true; rx.max = bx; break;
        default: return;
    }
    for (int32_t d = 0; d < 6; ++d) {
        if (d == ar::kDown) {
            continue;
        }
        const int32_t px = bx + ar::kFacingOffsets[d][0];
        const int32_t py = by + ar::kFacingOffsets[d][1];
        const int32_t pz = bz + ar::kFacingOffsets[d][2];
        if (!in_range(px, pz, rx, rz)) {
            continue;
        }
        const ar::BlockInfo info = sc.get(px, py, pz);
        if (!defence_block(sc.jni, info.id)) {
            continue;
        }
        if (plate.layers.empty()) {
            Layer layer{};
            layer.index = 1;
            plate.layers.push_back(layer);
        }
        add_to_layer(sc, plate, 0, info);
        uint32_t excluded = 1u << ar::facing_opposite(d);
        scan_neighbors(sc, px, py, pz, 2, excluded, plate, rx, rz);
    }
}

// BedTracker.getCenter
math::Vec3d plate_center(ScanContext& sc, const Plate& plate, int32_t bx, int32_t by, int32_t bz, int32_t facing,
                         bool head) {
    if (head) {
        facing = ar::facing_opposite(facing);
    }
    const int32_t* off = ar::kFacingOffsets[facing];
    math::Vec3d center{bx + 0.5 + off[0] * 0.5, by + 0.5 + off[1] * 0.5, bz + 0.5 + off[2] * 0.5};
    for (int32_t i = 0; i <= static_cast<int32_t>(plate.layers.size()); ++i) {
        if (sc.get(bx, by + i, bz).id != kBlockAir || sc.get(bx + off[0], by + i + off[1], bz + off[2]).id != kBlockAir) {
            center.y += 1.0;
        }
    }
    return center;
}

// BedTracker.update(bed, state)
void update_bed(events::JniContext* jni, jobject world, int32_t x, int32_t y, int32_t z) {
    ScanContext sc{};
    sc.jni = jni;
    sc.world = world;
    const ar::BlockInfo bed = sc.get(x, y, z);
    if (bed.id != kBlockBed) {
        return;
    }
    const int32_t facing = kHorizontalToFacing[bed.meta & 3];
    const bool head = (bed.meta & 8) != 0;
    if (!head) {
        return; // FOOT
    }
    const int32_t back = ar::facing_opposite(facing);
    const int32_t ox = x + ar::kFacingOffsets[back][0];
    const int32_t oz = z + ar::kFacingOffsets[back][2];
    if (sc.get(ox, y, oz).id != kBlockBed) {
        return;
    }
    Plate plate{};
    plate.created_at = time::now_seconds();
    collect(sc, x, y, z, plate, facing);
    collect(sc, ox, y, oz, plate, back);
    plate.layers.erase(std::remove_if(plate.layers.begin(), plate.layers.end(),
                                      [](const Layer& l) { return l.count == 0; }),
                       plate.layers.end());
    if (plate.layers.empty()) {
        Layer layer{};
        layer.index = 1;
        layer.blocks.push_back(LayerBlock{kBlockBed, 0});
        layer.count = 1;
        plate.layers.push_back(layer);
    }
    plate.center = plate_center(sc, plate, x, y, z, facing, head);
    plate.rebuild_blocks();
    if (PlateEntry* existing = find_plate(x, y, z)) {
        existing->plate = plate;
    } else {
        g_plates.push_back(PlateEntry{BedPos{x, y, z}, plate});
    }
}

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

// BedPlatesModule.onTick + BedTracker.onBlockChange / tick
void on_post_living_update(void* raw) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(raw)->ctx;
    if (ctx == nullptr || ctx->world == nullptr || ar::classes(ctx->jni) == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    if (world_changed(ctx)) {
        g_plates.clear();
        g_tracker.scan_all(ctx);
        g_seen_revision = 0;
    }

    // New beds (Adjust: bed blocks reported by the chunk renderer).
    if (g_tracker.revision() != g_seen_revision) {
        g_seen_revision = g_tracker.revision();
        g_tracker.collect(&g_scratch);
        for (const ar::TrackedBlock& b : g_scratch) {
            if (find_plate(b.x, b.y, b.z) == nullptr) {
                update_bed(jni, ctx->world, b.x, b.y, b.z);
            }
        }
    }

    // Other block changes mark the first plate within 12 blocks (at or below the change) dirty.
    g_tracker.take_changes(&g_changes);
    for (const ar::TrackedBlock& c : g_changes) {
        if (c.id == kBlockBed) {
            continue;
        }
        for (PlateEntry& e : g_plates) {
            const double dx = c.x - e.pos.x;
            const double dy = c.y - e.pos.y;
            const double dz = c.z - e.pos.z;
            if (e.pos.y <= c.y && dx * dx + dy * dy + dz * dz <= 144.0) {
                e.plate.mark_dirty();
                break;
            }
        }
    }

    // BedTracker.tick
    std::vector<BedPos> pending;
    for (size_t i = 0; i < g_plates.size();) {
        PlateEntry& e = g_plates[i];
        ar::BlockInfo info{};
        if (!ar::block_info_at(jni, ctx->world, e.pos.x, e.pos.y, e.pos.z, &info) || info.id != kBlockBed) {
            g_plates.erase(g_plates.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (e.plate.dirty) {
            pending.push_back(e.pos);
        }
        ++i;
    }
    for (const BedPos& p : pending) {
        update_bed(jni, ctx->world, p.x, p.y, p.z);
    }
}

void on_world_block_update(void* raw) noexcept {
    g_tracker.on_world_block_update(static_cast<const events::WorldBlockUpdateEvent*>(raw));
}

// ---- rendering (BedPlatesRenderer) -------------------------------------------------------------------------

jobject stack_for(events::JniContext* jni, int32_t block_id, int32_t metadata) noexcept {
    const int32_t key = block_id == kBlockBed ? -1 : ((block_id << 4) | (std::max(0, metadata) & 15));
    auto it = g_stacks.find(key);
    if (it != g_stacks.end()) {
        return it->second;
    }
    const ar::Classes* c = ar::classes(jni);
    jobject result = nullptr;
    if (c != nullptr) {
        static const jmethodID item_from_block =
            ajni::static_method(jni, c->item_cls, {"a", "getItemFromBlock", "func_150898_a"}, "(Lafh;)Lzw;",
                                "(Lnet/minecraft/block/Block;)Lnet/minecraft/item/Item;");
        static const jmethodID block_by_id = ajni::static_method(
            jni, c->block, {"c", "getBlockById", "func_149729_e"}, "(I)Lafh;", "(I)Lnet/minecraft/block/Block;");
        static const jmethodID stack_init = ajni::method(jni, c->item_stack, {"<init>", "<init>", "<init>"},
                                                         "(Lzw;II)V", "(Lnet/minecraft/item/Item;II)V");
        JNIEnv* env = jni->env;
        jobject item = nullptr;
        if (block_id == kBlockBed) {
            item = minecraft::item_get_by_id(jni, kItemBed);
        } else if (item_from_block != nullptr && block_by_id != nullptr) {
            jobject block = env->CallStaticObjectMethod(c->block, block_by_id, block_id);
            if (!ajni::clear_exception(jni) && block != nullptr) {
                item = env->CallStaticObjectMethod(c->item_cls, item_from_block, block);
                ajni::clear_exception(jni);
                env->DeleteLocalRef(block);
            }
        }
        if (item != nullptr && stack_init != nullptr) {
            const int32_t meta = block_id == kBlockBed ? 0 : std::max(0, metadata);
            jobject stack = env->NewObject(c->item_stack, stack_init, item, 1, meta);
            if (!ajni::clear_exception(jni) && stack != nullptr) {
                result = env->NewGlobalRef(stack);
                env->DeleteLocalRef(stack);
            }
        }
        if (item != nullptr) {
            env->DeleteLocalRef(item);
        }
    }
    g_stacks.emplace(key, result); // nullptr = "stack.getItem() == null" -> skipped
    return result;
}

math::Vec2f plate_size(const Plate& plate) noexcept {
    if (g_mode.value == kModeBlocks) {
        return math::Vec2f{static_cast<float>(plate.blocks.size()) * 8.0f, 8.0f};
    }
    float width = 0.0f;
    float height = 8.0f;
    for (const Layer& layer : plate.layers) {
        height = std::max(height, static_cast<float>(layer.blocks.size()) * 8.0f);
        width += 8.0f;
    }
    return math::Vec2f{width, height};
}

void gui_item_lighting(events::JniContext* jni) noexcept {
    // RenderHelper.enableGUIStandardItemLighting
    static const jmethodID m =
        ajni::static_method(jni, jni->mappings->cls_render_helper, {"c", "enableGUIStandardItemLighting", "func_74520_c"},
                            "()V", "()V");
    if (m != nullptr) {
        jni->env->CallStaticVoidMethod(jni->mappings->cls_render_helper, m);
        ajni::clear_exception(jni);
    } else {
        minecraft::render_helper_enable_item_lighting(jni);
    }
}

// BedPlatesRenderer.renderItem: 0.5 scale, 16 px icon -> 8 HUD units.
void draw_icon(events::JniContext* jni, jobject render_item, jobject font, jobject textures, jobject stack, float x,
                 float y) noexcept {
    gui_item_lighting(jni);
    glPushMatrix();
    glScalef(0.5f, 0.5f, 0.5f);
    glTranslatef(x * 2.0f, y * 2.0f, 0.0f);
    minecraft::render_item_into_gui(jni, render_item, font, textures, stack, 0, 0);
    ajni::clear_exception(jni);
    glPopMatrix();
    minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
    minecraft::render_helper_disable_item_lighting(jni);
}

void render_plate(events::JniContext* jni, jobject render_item, jobject font, jobject textures, const Plate& plate,
                  float sx, float sy) noexcept {
    const int alpha = plate_alpha();
    const math::Vec2f size = plate_size(plate);
    const float x = sx - size.x * 0.5f;
    const float y = sy - size.y * 0.5f;
    const float left = x - 1.5f;
    const float top = y - 1.5f;
    const float right = size.x + x + 1.5f;
    const float bottom = size.y + y + 1.5f;

    glClear(GL_DEPTH_BUFFER_BIT);
    minecraft::gl_state_disable_depth(jni);
    minecraft::gl_state_disable_texture_2d(jni);
    minecraft::gl_state_enable_blend(jni);
    minecraft::gl_state_blend_func(jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Gui.drawRect
    const Argb background = (static_cast<Argb>(std::clamp(alpha - 127, 0, 255)) << 24) |
                            (plate.incomplete ? 0xAA5555u : 0u);
    const math::Color3f bg = adjust::to_color3f(background);
    minecraft::gl_state_color(jni, bg.r, bg.g, bg.b, static_cast<float>(adjust::alpha_of(background)) / 255.0f);
    ar::draw_quad_2d(left, top, right, bottom);

    // RenderUtil.drawBorderedRect(((alpha - 87) << 24) | team/HUD colour, ..., 0.5)
    const Argb color = adjust_hud::hud_color();
    const Argb border = (static_cast<Argb>(alpha - 87) << 24) | (color & 0xFFFFFFu);
    const math::Color3f bc = adjust::to_color3f(border);
    minecraft::gl_state_color(jni, bc.r, bc.g, bc.b, static_cast<float>(adjust::alpha_of(border)) / 255.0f);
    ar::draw_rect_outline_2d(left, top, right, bottom, 0.5f);

    // Icons
    minecraft::gl_state_enable_texture_2d(jni);
    minecraft::gl_state_enable_depth(jni);
    minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
    if (g_mode.value == kModeBlocks) {
        float ix = x;
        for (const LayerBlock& b : plate.blocks) {
            jobject stack = stack_for(jni, b.block_id, b.metadata);
            if (stack == nullptr) {
                continue;
            }
            draw_icon(jni, render_item, font, textures, stack, ix, y);
            ix += 8.0f;
        }
    } else {
        float ix = x;
        for (const Layer& layer : plate.layers) {
            float row_y = y;
            for (const LayerBlock& b : layer.blocks) {
                jobject stack = stack_for(jni, b.block_id, b.metadata);
                if (stack == nullptr) {
                    continue;
                }
                draw_icon(jni, render_item, font, textures, stack, ix, row_y);
                row_y += 8.0f;
            }
            ix += 8.0f;
        }
    }
}

void on_render_world_2d(void* raw) noexcept {
    const auto* ev = static_cast<const events::RenderWorld2DProjEvent*>(raw);
    events::GameContext* ctx = ev->ctx;
    if (g_plates.empty() || ctx == nullptr || ar::classes(ctx->jni) == nullptr) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;

    // ScaledResolution(mc, 2.0): every coordinate below is in those HUD units.
    const float scale = adjust::resolve_hud_scale(2.0f, ev->viewport_size.x, ev->viewport_size.y);
    if (scale <= 0.0f) {
        return;
    }

    struct Projected {
        const Plate* plate;
        math::Vec2f screen;
    };
    std::vector<Projected> projected;
    projected.reserve(g_plates.size());
    for (const PlateEntry& e : g_plates) {
        math::Vec2f screen{};
        if (minecraft::project_world_to_screen(ev, math::vec3_sub(e.plate.center, ev->render_pos), &screen)) {
            projected.push_back(Projected{&e.plate, math::vec2_scale(screen, 1.0f / scale)});
        }
    }
    if (projected.empty()) {
        return;
    }
    if (env->PushLocalFrame(16) != 0) {
        ajni::clear_exception(jni);
        return;
    }
    jobject render_item = minecraft::mc_get_render_item(jni, ctx->minecraft);
    jobject render_manager = minecraft::mc_get_render_manager(jni, ctx->minecraft);
    jobject font = render_manager != nullptr ? minecraft::render_manager_get_font_renderer(jni, render_manager) : nullptr;
    jobject textures = minecraft::mc_get_texture_manager(jni, ctx->minecraft);
    if (render_item != nullptr) {
        glPushMatrix();
        glScalef(scale, scale, 1.0f);
        minecraft::gl_state_disable_lighting(jni);
        for (const Projected& p : projected) {
            render_plate(jni, render_item, font, textures, *p.plate, p.screen.x, p.screen.y);
        }
        glPopMatrix();
        // Back to the projected-pass state.
        minecraft::gl_state_disable_lighting(jni);
        minecraft::gl_state_disable_depth(jni);
        minecraft::gl_state_disable_texture_2d(jni);
        minecraft::gl_state_enable_blend(jni);
        minecraft::gl_state_blend_func(jni, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        minecraft::gl_state_color(jni, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    ajni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
}

// ---- module ----------------------------------------------------------------------------------------------------

void on_module_state_changed(void* raw) noexcept {
    const auto* ev = static_cast<const events::ModuleStateChangedEvent*>(raw);
    if (ev->module != get_adjust_bedplates_module()) {
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
    release_stacks(env);
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    g_mode.render(gui, renderer);
    g_alpha.render(gui, renderer);
}

void visit_config(config::ConfigArchive* ar_) noexcept {
    g_mode.visit(ar_, "mode");
    g_alpha.visit(ar_, "alpha");
}

events::Module create_module() noexcept {
    events::Module mod{};
    mod.name = "Bed Plates";
    mod.description = "Shows the defence blocks of every bed on a plate above it.";
    mod.category = events::ModuleCategory::Visual;
    mod.events[events::kEventModuleStateChanged] = {&on_module_state_changed, 0};
    mod.events[events::kEventPostLivingUpdate] = {&on_post_living_update, 0};
    mod.events[events::kEventRenderWorld2DProj] = {&on_render_world_2d, 50};
    mod.events[events::kEventWorldBlockUpdate] = {&on_world_block_update, 0};
    mod.events[events::kEventClientShutdown] = {&on_client_shutdown, 0};
    mod.render_settings = &render_settings;
    mod.visit_config = &visit_config;
    mod.flags |= events::kModuleFlagStyleAdjust;
    events::set_module_config_key(&mod, "Adjust Bed Plates");
    return mod;
}

} // namespace

events::Module* get_adjust_bedplates_module() noexcept {
    static events::Module s_module = create_module();
    return &s_module;
}

} // namespace slinky::modules
