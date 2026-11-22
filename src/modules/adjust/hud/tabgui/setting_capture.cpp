#include "modules/adjust/hud/tabgui/setting_capture.hpp"

#include "events/module.hpp"

namespace slinky::modules::adjust_hud::tabgui {

namespace {

constinit bool g_hook_installed = false;
constinit bool g_active = false;
constinit std::vector<CapturedItem>* g_target = nullptr;

} // namespace

bool capture_active() noexcept {
    return g_active;
}

void capture_item(CaptureKind kind, const void* item) noexcept {
    if (!g_active || g_target == nullptr || item == nullptr) {
        return;
    }
    try {
        g_target->push_back(CapturedItem{kind, item});
    } catch (...) {
        // out of memory: drop the item
    }
}

void mark_capture_hook_installed() noexcept {
    g_hook_installed = true;
}

bool capture_hook_installed() noexcept {
    return g_hook_installed;
}

bool capture_module_settings(events::Module* mod, std::vector<CapturedItem>& out) noexcept {
    out.clear();
    if (!g_hook_installed || g_active || mod == nullptr || mod->render_settings == nullptr) {
        return false;
    }
    g_target = &out;
    g_active = true;
    mod->render_settings(nullptr, nullptr);
    g_active = false;
    g_target = nullptr;
    return true;
}

} // namespace slinky::modules::adjust_hud::tabgui
