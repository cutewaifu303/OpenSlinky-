// Port of com.adjustclient.module.impl.render.BlockPlacesModule (+ PlacedBlock): outlines blocks you recently
// placed and fades them out.
//
// Adjust gets a BlockPlacedEvent from patched ItemBlock / ItemDoor / ItemReed.onItemUse. OpenSlinky++ has no such
// hook, so the port watches World.setBlockState (kEventWorldBlockUpdate, kind 2) while Minecraft.rightClickMouse
// runs (between kEventPreAttackOrUseEntity and kEventPostAttackOrUseEntity) with a block / door / reed item in
// hand, and confirms the placement after rightClickMouse returned. Every OpenSlinky++ module places blocks through
// rightClickMouse, so they are covered too.

#include "modules/adjust/render/block_places/block_places.hpp"

#include <glad/glad.h>

#include <algorithm>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "adjust/kit.hpp"
#include "adjust/settings.hpp"
#include "config/settings.hpp"
#include "gui/widgets.hpp"
#include "minecraft/game_helpers.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "util/math.hpp"

namespace slinky::modules {

namespace {

using adjust::Argb;

constexpr double kExpand = 0.002000000094994903; // 0.002F

void on_pre_use(void* event) noexcept;
void on_post_use(void* event) noexcept;
void on_world_block_update(void* event) noexcept;
void on_post_living_update(void* event) noexcept;
void on_render_world_3d(void* event) noexcept;
void on_module_state_changed(void* event) noexcept;
void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept;
void visit_config(config::ConfigArchive* archive) noexcept;

// PlacedBlock
struct PlacedBlock {
    minecraft::Vec3i pos{};
    math::AABB bounds{};
    int64_t created_ms = 0; // timer.lastMs
    int32_t block_id = 0;
};

struct PendingPlacement {
    minecraft::Vec3i pos{};
    int32_t block_id = 0;
};

struct BlockPlacesState {
    events::Module base{};

    adjust::ColorOption color{"Color", "Color mode", 0xFF9090FFu, adjust::ColorMode::Main};
    config::FloatSetting timeout{{}, "Timeout", "%.1f", 0.5f, 0.0f, 0.5f, 5.0f, 1.5f, 1.5f};
    config::FloatSetting width{{}, "Width", "%.1f", 0.5f, 0.0f, 0.5f, 2.0f, 0.5f, 0.5f};

    std::vector<PlacedBlock> placed; // placedBlocks (keyed by position)
    std::vector<PendingPlacement> pending;
    bool in_right_click = false;
    bool holding_door = false;

    BlockPlacesState() noexcept {
        base.name = "Block Places";
        base.description = "Highlights blocks that you recently placed.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust; // BlockPlacesModule.setHidden(false)
        // Lowest priority: runs after every other subscriber, so a cancelled right click is seen as cancelled.
        base.events[events::kEventPreAttackOrUseEntity] = {on_pre_use, -100000, 0};
        base.events[events::kEventPostAttackOrUseEntity] = {on_post_use, 0, 0};
        base.events[events::kEventWorldBlockUpdate] = {on_world_block_update, 0, 0};
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, 0, 0};
        base.events[events::kEventRenderWorld3D] = {on_render_world_3d, 0, 0};
        base.events[events::kEventModuleStateChanged] = {on_module_state_changed, 0, 0};
        base.render_settings = render_settings;
        base.visit_config = visit_config;
        events::set_module_config_key(&base, "Adjust Block Places");
    }
};

BlockPlacesState g_state{};

bool same_pos(const minecraft::Vec3i& a, const minecraft::Vec3i& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same_box(const math::AABB& a, const math::AABB& b) noexcept {
    return a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z && a.max.x == b.max.x &&
           a.max.y == b.max.y && a.max.z == b.max.z;
}

// Block id at `pos` (0 = air / unknown). Caller owns a local frame.
int32_t block_id_at(events::JniContext* jni, jobject world, const minecraft::Vec3i& pos) noexcept {
    jobject block = minecraft::world_get_block(jni, world, pos.x, pos.y, pos.z);
    if (adjust::jni::clear_exception(jni) || block == nullptr) {
        return 0;
    }
    return minecraft::block_get_id(jni, block);
}

// block.getSelectedBoundingBox(world, pos).expand(0.002, 0.002, 0.002) for the block currently at `pos`.
bool selected_bounds(events::JniContext* jni, jobject world, const minecraft::Vec3i& pos, int32_t* out_id,
                     math::AABB* out) noexcept {
    static const jmethodID mid_selected_box = adjust::jni::method(
        jni, jni->mappings->cls_block, {"b", "getSelectedBoundingBox", "func_180646_a"}, "(Ladm;Lcj;)Laug;",
        "(Lnet/minecraft/world/World;Lnet/minecraft/util/BlockPos;)Lnet/minecraft/util/AxisAlignedBB;");
    if (mid_selected_box == nullptr) {
        return false;
    }
    JNIEnv* env = jni->env;
    if (env->PushLocalFrame(16) != 0) {
        adjust::jni::clear_exception(jni);
        return false;
    }
    bool ok = false;
    jobject block = minecraft::world_get_block(jni, world, pos.x, pos.y, pos.z);
    if (!adjust::jni::clear_exception(jni) && block != nullptr) {
        *out_id = minecraft::block_get_id(jni, block);
        jobject block_pos = minecraft::block_pos_new(jni, pos.x, pos.y, pos.z);
        jobject box = block_pos != nullptr ? env->CallObjectMethod(block, mid_selected_box, world, block_pos) : nullptr;
        if (!adjust::jni::clear_exception(jni) && box != nullptr) {
            *out = math::aabb_expand(minecraft::aabb_to_aabb(jni, box), kExpand);
            ok = true;
        }
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

// The held item can place a block (Adjust posts BlockPlacedEvent from ItemBlock, ItemDoor and ItemReed).
bool holding_placeable(events::GameContext* ctx, bool* out_door) noexcept {
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    static const jclass cls_item_reed = adjust::jni::find_class(jni, "yp", "net/minecraft/item/ItemReed");
    static const jclass cls_item_door = adjust::jni::find_class(jni, "zb", "net/minecraft/item/ItemDoor");
    *out_door = false;
    if (env->PushLocalFrame(16) != 0) {
        adjust::jni::clear_exception(jni);
        return false;
    }
    bool placeable = false;
    jobject item = minecraft::player_get_held_item(jni, ctx->local_player);
    if (item != nullptr) {
        if (cls_item_door != nullptr && env->IsInstanceOf(item, cls_item_door)) {
            *out_door = true;
            placeable = true;
        } else if (cls_item_reed != nullptr && env->IsInstanceOf(item, cls_item_reed)) {
            placeable = true;
        } else {
            placeable = minecraft::is_block_item(jni, item);
        }
    }
    adjust::jni::clear_exception(jni);
    env->PopLocalFrame(nullptr);
    return placeable;
}

void on_pre_use(void* event) noexcept {
    auto* ev = static_cast<events::AttackOrUseEntityEvent*>(event);
    g_state.in_right_click = false;
    g_state.pending.clear();
    events::GameContext* ctx = ev->ctx;
    if (ev->is_attack || ev->cancelled || ctx == nullptr || ctx->local_player == nullptr || ctx->world == nullptr) {
        return;
    }
    g_state.in_right_click = holding_placeable(ctx, &g_state.holding_door);
}

// World.setBlockState while the right click runs (the block is not set yet).
void on_world_block_update(void* event) noexcept {
    auto* ev = static_cast<events::WorldBlockUpdateEvent*>(event);
    if (!g_state.in_right_click || ev->update_kind != 2 || ev->block_id == 0 || ev->jni == nullptr ||
        ev->world == nullptr) {
        return;
    }
    if (g_state.holding_door && !g_state.pending.empty()) {
        return; // ItemDoor reports only the lower half
    }
    const minecraft::Vec3i pos{ev->x, ev->y_or_cz, ev->z};
    JNIEnv* env = ev->jni->env;
    if (env->PushLocalFrame(16) != 0) {
        adjust::jni::clear_exception(ev->jni);
        return;
    }
    const int32_t previous = block_id_at(ev->jni, ev->world, pos);
    env->PopLocalFrame(nullptr);
    if (previous == ev->block_id) {
        return; // state change of an existing block (door / lever / trapdoor toggles), not a placement
    }
    g_state.pending.push_back({pos, ev->block_id});
}

// After rightClickMouse returned: keep the placements that succeeded (BlockPlacesModule.onBlockPlaced).
void on_post_use(void* event) noexcept {
    auto* ev = static_cast<events::AttackOrUseEntityEvent*>(event);
    const bool active = g_state.in_right_click;
    g_state.in_right_click = false;
    events::GameContext* ctx = ev->ctx;
    if (ev->is_attack || !active || ctx == nullptr || ctx->world == nullptr || g_state.pending.empty()) {
        g_state.pending.clear();
        return;
    }
    const int64_t now = adjust::now_ms();
    for (const PendingPlacement& placement : g_state.pending) {
        int32_t id = 0;
        math::AABB box{};
        if (!selected_bounds(ctx->jni, ctx->world, placement.pos, &id, &box) || id != placement.block_id) {
            continue;
        }
        PlacedBlock block{placement.pos, box, now, placement.block_id};
        auto it = std::find_if(g_state.placed.begin(), g_state.placed.end(),
                               [&](const PlacedBlock& p) noexcept { return same_pos(p.pos, placement.pos); });
        if (it != g_state.placed.end()) {
            *it = block;
        } else {
            g_state.placed.push_back(block);
        }
    }
    g_state.pending.clear();
}

// BlockPlacesModule.isExpired
bool is_expired(events::GameContext* ctx, const PlacedBlock& placed, int64_t now) noexcept {
    const int64_t lifetime = static_cast<int64_t>(static_cast<int>(g_state.timeout.value * 1000.0f)) + 470;
    if (placed.created_ms + lifetime <= now) {
        return true;
    }
    int32_t id = 0;
    math::AABB box{};
    if (!selected_bounds(ctx->jni, ctx->world, placed.pos, &id, &box)) {
        return true;
    }
    return id != placed.block_id || !same_box(placed.bounds, box);
}

// BlockPlacesModule.onUpdate -> updateBlocks
void on_post_living_update(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    g_state.in_right_click = false;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->world == nullptr || g_state.placed.empty()) {
        return;
    }
    const int64_t now = adjust::now_ms();
    g_state.placed.erase(std::remove_if(g_state.placed.begin(), g_state.placed.end(),
                                        [&](const PlacedBlock& p) noexcept { return is_expired(ctx, p, now); }),
                         g_state.placed.end());
}

// PlacedBlock.getFadeAlpha
int fade_alpha(const PlacedBlock& placed, int64_t now) noexcept {
    const auto base = static_cast<int>(static_cast<double>(g_state.timeout.value) * 1000.0);
    const int64_t elapsed = now - placed.created_ms;
    if (base > elapsed) {
        return 255;
    }
    const int64_t remaining = static_cast<int64_t>(base) + 500 - elapsed;
    if (remaining < 30) {
        return 0;
    }
    return static_cast<int>(static_cast<float>(remaining) / 500.0f * 255.0f);
}

void gl_color(Argb argb) noexcept {
    glColor4f(static_cast<float>(adjust::red_of(argb)) / 255.0f, static_cast<float>(adjust::green_of(argb)) / 255.0f,
              static_cast<float>(adjust::blue_of(argb)) / 255.0f, static_cast<float>(adjust::alpha_of(argb)) / 255.0f);
}

// RenderUtil.drawOutlinedBox: top and bottom loops + four vertical edges.
void draw_outlined_box(const math::AABB& box) noexcept {
    const double ys[2] = {box.min.y, box.max.y};
    for (double y : ys) {
        glBegin(GL_LINE_STRIP);
        glVertex3d(box.min.x, y, box.min.z);
        glVertex3d(box.max.x, y, box.min.z);
        glVertex3d(box.max.x, y, box.max.z);
        glVertex3d(box.min.x, y, box.max.z);
        glVertex3d(box.min.x, y, box.min.z);
        glEnd();
    }
    glBegin(GL_LINES);
    glVertex3d(box.min.x, box.min.y, box.min.z);
    glVertex3d(box.min.x, box.max.y, box.min.z);
    glVertex3d(box.max.x, box.min.y, box.min.z);
    glVertex3d(box.max.x, box.max.y, box.min.z);
    glVertex3d(box.max.x, box.min.y, box.max.z);
    glVertex3d(box.max.x, box.max.y, box.max.z);
    glVertex3d(box.min.x, box.min.y, box.max.z);
    glVertex3d(box.min.x, box.max.y, box.max.z);
    glEnd();
}

// BlockPlacesModule.renderBlocks (RenderUtil.begin3d / end3d, depth test on, no depth writes)
void on_render_world_3d(void* event) noexcept {
    if (g_state.placed.empty()) {
        return;
    }
    auto* ev = static_cast<events::RenderWorld3DEvent*>(event);

    glPushMatrix();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glDepthMask(GL_FALSE);
    glLineWidth(g_state.width.value);
    glEnable(GL_DEPTH_TEST);

    const int64_t now = adjust::now_ms();
    const Argb rgb = g_state.color.get() & 0xFFFFFFu;
    for (const PlacedBlock& placed : g_state.placed) {
        const math::AABB box = math::make_aabb(math::vec3_sub(placed.bounds.min, ev->render_pos),
                                               math::vec3_sub(placed.bounds.max, ev->render_pos));
        gl_color((static_cast<Argb>(fade_alpha(placed, now)) << 24) | rgb);
        draw_outlined_box(box);
    }

    // Back to the world pass state (depth test off, blend on, textures off).
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glPopMatrix();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// onEnable: placedBlocks = new HashMap; onDisable: placedBlocks = null
void on_module_state_changed(void* event) noexcept {
    auto* ev = static_cast<events::ModuleStateChangedEvent*>(event);
    if (ev->module != &g_state.base) {
        return;
    }
    g_state.placed.clear();
    g_state.pending.clear();
    g_state.in_right_click = false;
}

void render_settings(gui::ClickGui* gui, gui::GuiRenderer* renderer) noexcept {
    gui::widgets::render_section_header(gui, renderer, "Main");
    g_state.color.render(gui, renderer);
    g_state.timeout.render(gui, renderer);
    g_state.width.render(gui, renderer);
}

void visit_config(config::ConfigArchive* archive) noexcept {
    g_state.color.visit(archive, "color", "color_mode");
    g_state.timeout.visit(archive, "timeout");
    g_state.width.visit(archive, "width");
}

} // namespace

events::Module* get_adjust_block_places_module() noexcept {
    return &g_state.base;
}

} // namespace slinky::modules
