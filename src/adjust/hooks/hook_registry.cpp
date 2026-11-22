#include "adjust/hooks/hooks.hpp"

#include <vector>

#include "core/client.hpp"

namespace slinky::adjust::hooks {

events::JniContext jni_for(JNIEnv* env) noexcept {
    return events::JniContext{core::runtime().mappings, env};
}

} // namespace slinky::adjust::hooks

namespace slinky::bytecode {

const ExtraHookSpec* const* all_extra_hooks() noexcept {
    static const std::vector<const ExtraHookSpec*> s_all = [] {
        using Fn = const ExtraHookSpec* const* (*)() noexcept;
        const Fn domains[] = {
            &adjust::hooks::render_hooks,
            &adjust::hooks::tweaks_hooks,
            &adjust::hooks::camera_hooks,
            &adjust::hooks::world_fx_hooks,
            &adjust::hooks::item_hooks,
            &adjust::hooks::mainmenu_hooks,
            &adjust::hooks::streamer_hooks,
            &adjust::hooks::hurtcam_hooks,
        };
        std::vector<const ExtraHookSpec*> all;
        for (Fn domain : domains) {
            for (const ExtraHookSpec* const* it = domain(); it != nullptr && *it != nullptr; ++it) {
                all.push_back(*it);
            }
        }
        all.push_back(nullptr);
        return all;
    }();
    return s_all.data();
}

} // namespace slinky::bytecode
