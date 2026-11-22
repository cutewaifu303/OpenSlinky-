// Port of com.adjustclient.module.impl.render.BarrierRevealModule: "Makes barriers always visible."
//
// Vanilla only shows barriers (as BARRIER particles) in creative mode while holding a barrier item:
// WorldClient.doVoidFogParticles samples 1000 random positions within +-15 blocks of the player every tick and
// spawns EnumParticleTypes.BARRIER at each barrier it hits. Adjust forces that flag on (the hook itself is not in
// the restored source). This port runs the same sampling once per tick on the main thread without a hook: the
// block ids come from copies of the chunk sections around the player (no per-block JNI calls) and the particles
// are spawned through World.spawnParticle, so they look and behave exactly like vanilla's.
//
// Adjust's UtilityModule has no settings. Minecraft 1.8.9 only (inert on 1.7.10).

#include "modules/adjust/render/barrier_reveal/barrier_reveal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "util/random.hpp"

namespace slinky::modules {

namespace {

constexpr int kBarrierId = 166;
constexpr int kRange = 16;      // doVoidFogParticles: i = 16
constexpr int kSamples = 1000;  // doVoidFogParticles: 1000 iterations

void on_post_living_update(void* event) noexcept;

struct Section {
    int cx = 0;
    int sy = 0;
    int cz = 0;
    std::array<uint16_t, 4096> blocks{}; // (id << 4) | meta, index (y << 8) | (z << 4) | x
};

struct BarrierRevealState {
    events::Module base{};
    std::vector<Section> sections; // sections around the player that contain a barrier
    jintArray empty_args = nullptr; // new int[0] (global ref)

    BarrierRevealState() noexcept {
        base.name = "Barrier Reveal";
        base.description = "Makes barriers always visible.";
        base.category = events::ModuleCategory::Visual;
        base.flags = events::kModuleFlagStyleAdjust | events::kModuleFlagHiddenFromHud; // Adjust default: hidden
        base.events[events::kEventPostLivingUpdate] = {on_post_living_update, 0, 0};
        events::set_module_config_key(&base, "Adjust Barrier Reveal");
    }
};

BarrierRevealState g_state{};

int floor_div16(int v) noexcept {
    return v >> 4; // arithmetic shift == floor division by 16
}

// Copies the sections within the sampling cube that contain at least one barrier.
void collect_sections(events::JniContext* jni, jobject world, int px, int py, int pz) noexcept {
    JNIEnv* env = jni->env;
    g_state.sections.clear();
    const int min_sy = std::max(0, floor_div16(py - (kRange - 1)));
    const int max_sy = std::min(15, floor_div16(py + (kRange - 1)));
    if (min_sy > max_sy) {
        return;
    }
    for (int cx = floor_div16(px - (kRange - 1)); cx <= floor_div16(px + (kRange - 1)); ++cx) {
        for (int cz = floor_div16(pz - (kRange - 1)); cz <= floor_div16(pz + (kRange - 1)); ++cz) {
            if (env->PushLocalFrame(32) != 0) {
                adjust::jni::clear_exception(jni);
                return;
            }
            jobject chunk = minecraft::world_get_chunk_from_chunk_coords(jni, world, cx, cz);
            jobjectArray storages =
                (!adjust::jni::clear_exception(jni) && chunk != nullptr) ? minecraft::chunk_get_storage_arrays(jni, chunk)
                                                                          : nullptr;
            if (storages != nullptr) {
                for (int sy = min_sy; sy <= max_sy; ++sy) {
                    jobject ebs = env->GetObjectArrayElement(storages, sy);
                    if (ebs == nullptr) {
                        continue;
                    }
                    auto data = static_cast<jcharArray>(minecraft::extended_block_storage_get_data(jni, ebs));
                    if (data == nullptr || env->GetArrayLength(data) < 4096) {
                        env->DeleteLocalRef(ebs);
                        continue;
                    }
                    auto* raw = static_cast<const uint16_t*>(env->GetPrimitiveArrayCritical(data, nullptr));
                    if (raw != nullptr) {
                        bool has_barrier = false;
                        for (int i = 0; i < 4096; ++i) {
                            if ((raw[i] >> 4) == kBarrierId) {
                                has_barrier = true;
                                break;
                            }
                        }
                        if (has_barrier) {
                            Section& section = g_state.sections.emplace_back();
                            section.cx = cx;
                            section.sy = sy;
                            section.cz = cz;
                            for (int i = 0; i < 4096; ++i) {
                                section.blocks[static_cast<size_t>(i)] = raw[i];
                            }
                        }
                        env->ReleasePrimitiveArrayCritical(data, const_cast<uint16_t*>(raw), JNI_ABORT);
                    }
                    env->DeleteLocalRef(data);
                    env->DeleteLocalRef(ebs);
                }
            }
            env->PopLocalFrame(nullptr);
        }
    }
}

bool is_barrier(int x, int y, int z) noexcept {
    if (y < 0 || y > 255) {
        return false;
    }
    const int cx = floor_div16(x);
    const int sy = y >> 4;
    const int cz = floor_div16(z);
    for (const Section& section : g_state.sections) {
        if (section.cx == cx && section.sy == sy && section.cz == cz) {
            const int index = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15);
            return (section.blocks[static_cast<size_t>(index)] >> 4) == kBarrierId;
        }
    }
    return false;
}

int rand_range() noexcept {
    return static_cast<int>(random::next_u32() % static_cast<uint32_t>(kRange));
}

// WorldClient.doVoidFogParticles with the "holding a barrier in creative" flag forced on (barrier part only).
void on_post_living_update(void* event) noexcept {
    events::GameContext* ctx = static_cast<events::ContextEvent*>(event)->ctx;
    if (ctx == nullptr || ctx->jni == nullptr || ctx->local_player == nullptr || ctx->world == nullptr ||
        !adjust::jni::is_1_8(ctx->jni)) {
        return;
    }
    events::JniContext* jni = ctx->jni;
    JNIEnv* env = jni->env;
    static const jmethodID mid_spawn_particle = adjust::jni::method(
        jni, jni->mappings->cls_world, {"a", "spawnParticle", "func_175688_a"}, "(Lcy;DDDDDD[I)V",
        "(Lnet/minecraft/util/EnumParticleTypes;DDDDDD[I)V");
    static const jclass cls_particle_types = adjust::jni::find_class(jni, "cy", "net/minecraft/util/EnumParticleTypes");
    static const jfieldID sfid_barrier = adjust::jni::static_field(
        jni, cls_particle_types, {"J", "BARRIER", "BARRIER"}, "Lcy;", "Lnet/minecraft/util/EnumParticleTypes;");
    if (mid_spawn_particle == nullptr || sfid_barrier == nullptr) {
        return;
    }
    if (g_state.empty_args == nullptr) {
        jintArray local = env->NewIntArray(0);
        if (local == nullptr) {
            adjust::jni::clear_exception(jni);
            return;
        }
        g_state.empty_args = static_cast<jintArray>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        if (g_state.empty_args == nullptr) {
            return;
        }
    }

    const math::Vec3d pos = minecraft::entity_get_position(jni, ctx->local_player);
    const int px = static_cast<int>(std::floor(pos.x));
    const int py = static_cast<int>(std::floor(pos.y));
    const int pz = static_cast<int>(std::floor(pos.z));
    collect_sections(jni, ctx->world, px, py, pz);
    if (g_state.sections.empty()) {
        return;
    }

    jobject barrier = env->GetStaticObjectField(cls_particle_types, sfid_barrier);
    if (barrier == nullptr) {
        adjust::jni::clear_exception(jni);
        return;
    }
    for (int j = 0; j < kSamples; ++j) {
        const int k = px + rand_range() - rand_range();
        const int l = py + rand_range() - rand_range();
        const int i1 = pz + rand_range() - rand_range();
        if (!is_barrier(k, l, i1)) {
            continue;
        }
        env->CallVoidMethod(ctx->world, mid_spawn_particle, barrier, static_cast<double>(static_cast<float>(k) + 0.5f),
                            static_cast<double>(static_cast<float>(l) + 0.5f),
                            static_cast<double>(static_cast<float>(i1) + 0.5f), 0.0, 0.0, 0.0, g_state.empty_args);
        if (adjust::jni::clear_exception(jni)) {
            break;
        }
    }
    env->DeleteLocalRef(barrier);
}

} // namespace

events::Module* get_adjust_barrier_reveal_module() noexcept {
    return &g_state.base;
}

} // namespace slinky::modules
