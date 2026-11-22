#include "events/module_manager.hpp"

#include <cstdlib>
#include <cstring>
#include "util/containers.hpp"

namespace slinky::events {

namespace {

thread_local uint32_t g_sort_event_index = 0;

VisualStyle g_active_style = VisualStyle::SlinkyPlusPlus;
VisualStyle g_requested_style = VisualStyle::SlinkyPlusPlus;

constexpr uint64_t kAllEventsMask = (1ULL << kEventCount) - 1ULL;

int compare_module_priority(const void* lhs, const void* rhs) noexcept {
    const auto* mod_a = *static_cast<const Module* const*>(lhs);
    const auto* mod_b = *static_cast<const Module* const*>(rhs);
    const int32_t prio_a = mod_a->events[g_sort_event_index].priority;
    const int32_t prio_b = mod_b->events[g_sort_event_index].priority;
    if (prio_b > prio_a) return 1;
    if (prio_b < prio_a) return -1;
    return 0;
}

} // namespace

VisualStyle get_active_visual_style() noexcept {
    return g_active_style;
}

VisualStyle get_requested_visual_style() noexcept {
    return g_requested_style;
}

void request_visual_style(VisualStyle style) noexcept {
    if (style != VisualStyle::SlinkyPlusPlus && style != VisualStyle::Adjust) {
        style = VisualStyle::SlinkyPlusPlus;
    }
    g_requested_style = style;
}

bool visual_style_change_pending() noexcept {
    return g_requested_style != g_active_style;
}

int ModuleManager::initialize(Module* const* registry, size_t count, SetModuleKeybindFn keybind_cb) noexcept {
    std::memset(&categories, 0, sizeof(ModuleManager) - offsetof(ModuleManager, categories));
    module_count = count;
    all_modules = static_cast<Module**>(util::zero_alloc(count, sizeof(Module*)));
    if (all_modules == nullptr) {
        return 1005;
    }
    std::memcpy(all_modules, registry, count * sizeof(Module*));

    for (size_t i = 0; i < count; ++i) {
        Module* mod = registry[i];
        const auto cat_idx = static_cast<size_t>(mod->category);
        if (cat_idx < static_cast<size_t>(ModuleCategory::Count)) {
            ++categories[cat_idx].count;
        }
        for (size_t ev = 0; ev < kEventCount; ++ev) {
            if (mod->events[ev].callback != nullptr) {
                ++subscribers[ev].count;
            }
        }
    }

    for (size_t c = 0; c < static_cast<size_t>(ModuleCategory::Count); ++c) {
        const size_t cat_cnt = categories[c].count;
        categories[c].data = static_cast<Module**>(util::zero_alloc(cat_cnt ? cat_cnt : 1, sizeof(Module*)));
        if (categories[c].data == nullptr) {
            return 1005;
        }
        categories[c].count = 0;
    }

    for (size_t ev = 0; ev < kEventCount; ++ev) {
        const size_t sub_cnt = subscribers[ev].count;
        subscribers[ev].data = static_cast<Module**>(util::zero_alloc(sub_cnt ? sub_cnt : 1, sizeof(Module*)));
        if (subscribers[ev].data == nullptr) {
            return 1005;
        }
        active_callbacks[ev] = static_cast<EventCallbackFn*>(util::zero_alloc(sub_cnt + 1, sizeof(EventCallbackFn)));
        if (active_callbacks[ev] == nullptr) {
            return 1005;
        }
        subscribers[ev].count = 0;
    }

    pending_enable = static_cast<Module**>(util::zero_alloc(count ? count : 1, sizeof(Module*)));
    if (pending_enable == nullptr) {
        return 1005;
    }
    pending_disable = static_cast<Module**>(util::zero_alloc(count ? count : 1, sizeof(Module*)));
    if (pending_disable == nullptr) {
        return 1005;
    }

    for (size_t i = 0; i < count; ++i) {
        Module* mod = registry[i];
        const auto cat_idx = static_cast<size_t>(mod->category);
        if (cat_idx < static_cast<size_t>(ModuleCategory::Count)) {
            categories[cat_idx].data[categories[cat_idx].count++] = mod;
        }
        for (size_t ev = 0; ev < kEventCount; ++ev) {
            if (mod->events[ev].callback != nullptr) {
                subscribers[ev].data[subscribers[ev].count++] = mod;
            }
        }
    }

    for (uint32_t ev = 0; ev < kEventCount; ++ev) {
        if (subscribers[ev].count > 1) {
            g_sort_event_index = ev;
            std::qsort(subscribers[ev].data, subscribers[ev].count, sizeof(Module*), compare_module_priority);
        }
    }

    set_keybind_fn = keybind_cb;

    // Populate initial active_callbacks for any module that starts enabled
    uint64_t initial_mask = 0;
    for (size_t i = 0; i < count; ++i) {
        if (all_modules[i]->enabled) {
            for (uint32_t ev = 0; ev < kEventCount; ++ev) {
                if (all_modules[i]->events[ev].callback != nullptr) {
                    initial_mask |= (1ULL << ev);
                }
            }
        }
    }
    if (initial_mask != 0) {
        rebuild_event_callbacks(initial_mask);
    }

    return 0;
}

void ModuleManager::shutdown() noexcept {
    std::free(all_modules);
    all_modules = nullptr;
    module_count = 0;

    for (auto& cat : categories) {
        std::free(cat.data);
        cat.data = nullptr;
        cat.count = 0;
    }
    for (size_t ev = 0; ev < kEventCount; ++ev) {
        std::free(subscribers[ev].data);
        subscribers[ev].data = nullptr;
        subscribers[ev].count = 0;

        std::free(active_callbacks[ev]);
        active_callbacks[ev] = nullptr;
    }
    std::free(pending_enable);
    pending_enable = nullptr;
    pending_enable_count = 0;

    std::free(pending_disable);
    pending_disable = nullptr;
    pending_disable_count = 0;
}

bool ModuleManager::request_set_enabled(Module* mod, bool enable) noexcept {
    if (mod == nullptr || mod->enabled == enable) {
        return true;
    }
    if (!enable) {
        PreModuleDisableEvent pre_ev{};
        pre_ev.id = kEventPreModuleDisable;
        pre_ev.module = mod;
        pre_ev.cancel_disable = false;
        dispatch(&pre_ev);
        if (pre_ev.cancel_disable) {
            return false;
        }
        // Avoid duplicate queue entries if toggled repeatedly within one tick
        for (size_t i = 0; i < pending_disable_count; ++i) {
            if (pending_disable[i] == mod) {
                return true;
            }
        }
        pending_disable[pending_disable_count++] = mod;
    } else {
        for (size_t i = 0; i < pending_enable_count; ++i) {
            if (pending_enable[i] == mod) {
                return true;
            }
        }
        pending_enable[pending_enable_count++] = mod;
    }
    return true;
}

void ModuleManager::rebuild_event_callbacks(uint64_t dirty_event_mask) noexcept {
    for (uint32_t ev = 0; ev < kEventCount; ++ev) {
        if ((dirty_event_mask & (1ULL << ev)) == 0) {
            continue;
        }
        EventCallbackFn* out = active_callbacks[ev];
        const size_t total = subscribers[ev].count;
        Module** mods = subscribers[ev].data;
        for (size_t i = 0; i < total; ++i) {
            Module* m = mods[i];
            if (m->enabled && m->events[ev].callback != nullptr && module_matches_style(m, g_active_style)) {
                *out++ = m->events[ev].callback;
            }
        }
        *out = nullptr;
    }
}

// Switches the active visual style. Modules that leave the style are told they were disabled while they are
// still subscribed; modules that join it are told they were enabled once they are subscribed again. The
// persisted `enabled` flag itself is never touched, so switching back restores everything exactly.
void ModuleManager::apply_visual_style(GameContext* ctx, OnModuleDisabledCleanupFn on_disabled_cleanup) noexcept {
    const VisualStyle old_style = g_active_style;
    const VisualStyle new_style = g_requested_style;
    if (old_style == new_style) {
        return;
    }

    for (size_t i = 0; i < module_count; ++i) {
        Module* mod = all_modules[i];
        if (mod->enabled && module_matches_style(mod, old_style) && !module_matches_style(mod, new_style)) {
            ModuleStateChangedEvent state_ev{};
            state_ev.id = kEventModuleStateChanged;
            state_ev.ctx = ctx;
            state_ev.module = mod;
            state_ev.enabled = false;
            dispatch(&state_ev);
            if (on_disabled_cleanup != nullptr) {
                on_disabled_cleanup(mod);
            }
        }
    }

    g_active_style = new_style;
    rebuild_event_callbacks(kAllEventsMask);

    for (size_t i = 0; i < module_count; ++i) {
        Module* mod = all_modules[i];
        if (mod->enabled && !module_matches_style(mod, old_style) && module_matches_style(mod, new_style)) {
            ModuleStateChangedEvent state_ev{};
            state_ev.id = kEventModuleStateChanged;
            state_ev.ctx = ctx;
            state_ev.module = mod;
            state_ev.enabled = true;
            dispatch(&state_ev);
        }
    }
}

void ModuleManager::flush_pending_toggles(GameContext* ctx, OnModuleDisabledCleanupFn on_disabled_cleanup) noexcept {
    if (visual_style_change_pending()) {
        apply_visual_style(ctx, on_disabled_cleanup);
    }

    if (pending_enable_count > 0) {
        uint64_t dirty_mask = 0;
        for (size_t i = 0; i < pending_enable_count; ++i) {
            Module* mod = pending_enable[i];
            mod->enabled = true;
            for (uint32_t ev = 0; ev < kEventCount; ++ev) {
                if (mod->events[ev].callback != nullptr) {
                    dirty_mask |= (1ULL << ev);
                }
            }
        }
        rebuild_event_callbacks(dirty_mask);

        for (size_t i = 0; i < pending_enable_count; ++i) {
            // Modules outside the active style only change their persisted flag, they do not start running.
            if (!module_matches_style(pending_enable[i], g_active_style)) {
                continue;
            }
            ModuleStateChangedEvent state_ev{};
            state_ev.id = kEventModuleStateChanged;
            state_ev.ctx = ctx;
            state_ev.module = pending_enable[i];
            state_ev.enabled = true;
            dispatch(&state_ev);
        }
        pending_enable_count = 0;
    }

    if (pending_disable_count > 0) {
        uint64_t dirty_mask = 0;
        for (size_t i = 0; i < pending_disable_count; ++i) {
            Module* mod = pending_disable[i];
            mod->enabled = false;
            for (uint32_t ev = 0; ev < kEventCount; ++ev) {
                if (mod->events[ev].callback != nullptr) {
                    dirty_mask |= (1ULL << ev);
                }
            }
        }

        for (size_t i = 0; i < pending_disable_count; ++i) {
            Module* mod = pending_disable[i];
            if (!module_matches_style(mod, g_active_style)) {
                continue; // was not running, nothing to tear down
            }
            ModuleStateChangedEvent state_ev{};
            state_ev.id = kEventModuleStateChanged;
            state_ev.ctx = ctx;
            state_ev.module = mod;
            state_ev.enabled = false;
            dispatch(&state_ev);
            if (on_disabled_cleanup != nullptr) {
                on_disabled_cleanup(mod);
            }
        }

        rebuild_event_callbacks(dirty_mask);
        pending_disable_count = 0;
    }
}

void ModuleManager::dispatch_shutdown_to_all(GameContext* ctx) noexcept {
    ContextEvent ev{};
    ev.id = kEventClientShutdown;
    ev.ctx = ctx;
    for (size_t i = 0; i < module_count; ++i) {
        EventCallbackFn shutdown_cb = all_modules[i]->events[kEventClientShutdown].callback;
        if (shutdown_cb != nullptr) {
            shutdown_cb(&ev);
        }
    }
    for (size_t i = 0; i < module_count; ++i) {
        request_set_enabled(all_modules[i], false);
    }
}

} // namespace slinky::events
