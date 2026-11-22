// Port of com.adjustclient.ui.hud.watermark.Watermark (rebranded: "OpenSlinky++" instead of "Adjust").
//
// First letter bold in the HUD colour, the rest in (225, 228, 228), Tahoma 14 at (2.5, 2.5), top-left anchor.
// The per-user "Edition" labels of the original are not ported (they depended on Adjust account names).

#include "core/branding.hpp"
#include "modules/adjust/hud/adjust_hud.hpp"

namespace slinky::modules::adjust_hud {

namespace {

void render(const HudContext& /*ctx*/) noexcept {
    if (!watermark_enabled()) {
        return;
    }
    constexpr const char* kName = SLINKY_BRAND_NAME; // "OpenSlinky++"
    constexpr float kX = 2.5f;
    constexpr float kY = 2.5f;

    const char first[] = {ADJUST_FMT[0], ADJUST_FMT[1], 'l', kName[0], '\0'}; // §l + first letter
    float x = adjust::kTahoma14.draw(first, kX, kY, hud_color());
    adjust::kTahoma14.draw(kName + 1, x, kY, adjust::argb(225, 228, 228));
}

} // namespace

const HudElement& element_watermark() noexcept {
    static const HudElement s_element = [] {
        HudElement e{};
        e.name = "Watermark";
        e.render = render;
        return e;
    }();
    return s_element;
}

} // namespace slinky::modules::adjust_hud
