#pragma once

// Adjust render kit - a faithful C++ port of the drawing primitives the Adjust client (openadjust) builds its
// visuals on, mapped onto OpenSlinky++'s NanoVG context:
//
//   com.adjustclient.render.font.NvgContext       -> begin_hud / end_hud, current_scale, push/pop_even_scale
//   com.adjustclient.render.font.NvgFontRenderer  -> Font (Tahoma / icon fonts, § formatting codes, 0.5 shadow)
//   com.adjustclient.render.font.image.NvgDraw    -> fill_rect, stroke_rect, gradients, circles, scissors ...
//   com.adjustclient.render.ColorUtil             -> blend, with_alpha, hsb_to_rgb, rainbow, pulse ...
//   com.adjustclient.render.animation.*           -> Easing, Animation, TimedFade
//   net.minecraft ScaledResolution(mc, double)    -> resolve_hud_scale
//
// Colours are Java-style 0xAARRGGBB integers (Argb) so ported code can keep its constants unchanged.
// Coordinates are Adjust "HUD units" (Minecraft scaled-resolution units) inside a begin_hud()/end_hud() pair.
//
// Fonts: Adjust ships Tahoma; OpenSlinky++ does not redistribute that Microsoft font but loads it at runtime
// from %WINDIR%\Fonts (identical look). If it is missing, Inter is used. Adjust's own icon fonts are embedded.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "nanovg.h"
#include "util/math.hpp"

namespace slinky::gui {
struct GuiRenderer;
}

namespace slinky::adjust {

// =============================================================================================================
// Time
// =============================================================================================================

// Wall-clock milliseconds, the equivalent of System.currentTimeMillis().
int64_t now_ms() noexcept;

// =============================================================================================================
// Colour (ColorUtil + java.awt.Color helpers)
// =============================================================================================================

using Argb = uint32_t;

constexpr Argb argb(int r, int g, int b, int a = 255) noexcept {
    return (static_cast<Argb>(a & 0xFF) << 24) | (static_cast<Argb>(r & 0xFF) << 16) |
           (static_cast<Argb>(g & 0xFF) << 8) | static_cast<Argb>(b & 0xFF);
}
constexpr int alpha_of(Argb c) noexcept { return static_cast<int>(c >> 24); }
constexpr int red_of(Argb c) noexcept { return static_cast<int>((c >> 16) & 0xFF); }
constexpr int green_of(Argb c) noexcept { return static_cast<int>((c >> 8) & 0xFF); }
constexpr int blue_of(Argb c) noexcept { return static_cast<int>(c & 0xFF); }

// NvgFontRenderer shadow colour: same alpha, quarter brightness.
constexpr Argb shadow_of(Argb c) noexcept { return (c & 0xFF000000u) | ((c & 0xFCFCFCu) >> 2); }

NVGcolor to_nvg(Argb c) noexcept;
Argb from_color3f(const math::Color3f& c, int alpha = 255) noexcept;
math::Color3f to_color3f(Argb c) noexcept;

Argb with_alpha(Argb c, int alpha) noexcept;                       // ColorUtil.withAlpha
Argb blend(Argb first, Argb second, double ratio) noexcept;        // ColorUtil.blend (ratio weights `first`)
Argb scale(Argb c, float factor) noexcept;                         // ColorUtil.scale
Argb brighter(Argb c, float factor) noexcept;                      // ColorUtil.brighter(Color, float)
Argb lerp_argb(Argb a, Argb b, double ta, double tb) noexcept;     // ToggledEntry.lerpArgb
Argb hsb_to_rgb(float hue, float saturation, float brightness) noexcept; // java.awt.Color.HSBtoRGB (opaque)
void rgb_to_hsb(Argb c, float out_hsb[3]) noexcept;                // java.awt.Color.RGBtoHSB
void to_hsl(Argb c, float out_hsl[3]) noexcept;                    // ColorUtil.toHsl (hue in degrees)
Argb hsl_to_argb(const float hsl[3], int alpha) noexcept;          // ColorUtil.hslToColor
Argb rainbow(float offset, float saturation, float brightness) noexcept; // ColorUtil.rainbow
int pulse() noexcept;                                              // ColorUtil.pulse

// =============================================================================================================
// Animation (Easing / Animation / TimedFade)
// =============================================================================================================

enum class Easing : uint8_t {
    Linear,
    EaseInQuad, EaseOutQuad, EaseInOutQuad,
    EaseInCubic, EaseOutCubic, EaseInOutCubic,
    EaseInQuart, EaseOutQuart, EaseInOutQuart,
    EaseInQuint, EaseOutQuint, EaseInOutQuint,
    EaseInSine, EaseOutSine, EaseInOutSine,
    EaseInExpo, EaseOutExpo, EaseInOutExpo,
    EaseInCirc, EaseOutCirc, EaseInOutCirc,
    Sigmoid,
    EaseOutElastic,
    EaseInBack,
    Parabolic,
};

float ease(Easing easing, float x) noexcept;

// Port of com.adjustclient.render.animation.Animation (value chases the last requested target over `duration`).
struct Animation {
    Easing easing = Easing::Linear;
    int64_t duration = 0;
    int64_t last_call = 0;
    int64_t started_at = 0;
    float from = 0.0f;
    float target = 0.0f;
    float value = 0.0f;
    bool finished = false;

    Animation() noexcept = default;
    Animation(Easing e, int64_t duration_ms) noexcept;
    Animation(Easing e, int64_t duration_ms, int64_t started_at_ms) noexcept;

    void restart() noexcept;
    float update(float requested) noexcept;
};

// Port of TimedFade: fade in, hold, fade out. value() returns the current opacity 0..1.
struct TimedFade {
    int64_t fade_in_ms = 0;
    int64_t hold_ms = 0;
    int64_t fade_out_ms = 0;
    bool has_fade_in = false;
    bool has_timer = false;
    bool has_fade_out = false;
    int64_t hold_started = 0;
    Animation fade_in{};
    Animation fade_out{};

    TimedFade() noexcept = default;
    TimedFade(int64_t in_ms, int64_t hold, int64_t out_ms) noexcept;
    float value() noexcept;
};

// =============================================================================================================
// HUD frame (NvgContext)
// =============================================================================================================

// Scaled resolution used by the frame: width/height in HUD units, `scale` = pixels per unit.
struct HudFrame {
    float width = 0.0f;
    float height = 0.0f;
    float scale = 2.0f;
    int scaled_width = 0;   // MathHelper.ceiling_double_int(width)
    int scaled_height = 0;  // MathHelper.ceiling_double_int(height)
    bool valid = false;
};

// ScaledResolution(mc, double scale) of the Adjust fork. `requested` is the HUD scale (2.0, 2.25, ...).
float resolve_hud_scale(float requested, float fb_width, float fb_height) noexcept;

// Adjust's HUD "Scale" setting value (1..16) -> scale (HUDModule.getHudScale; 16 = auto/max).
float hud_scale_from_setting(int setting_value) noexcept;

// Opens a HUD frame on the OpenSlinky++ GUI layer: saves NanoVG state, makes `renderer->vg` the kit context,
// loads fonts on first use and scales so that 1 unit = `scale` framebuffer pixels. Always pair with end_hud().
HudFrame begin_hud(gui::GuiRenderer* renderer, float requested_scale) noexcept;
void end_hud() noexcept;

// Same as begin_hud, but for drawing on top of a world-projected overlay (kEventRenderWorld2DProj style code that
// already set its own transform): only binds the context and fonts, keeps the current transform.
void bind_context(NVGcontext* vg) noexcept;

NVGcontext* vg() noexcept;          // context of the open frame (nullptr outside)
float current_scale() noexcept;     // NvgContext.currentScale (0 when no frame is open)
void set_scale(float scale) noexcept;
void push_even_scale() noexcept;    // NvgContext.pushEvenScale
void pop_even_scale() noexcept;     // NvgContext.popEvenScale
float even_scale_factor() noexcept; // NvgContext.scaleFactor (0 when not pushed)
math::Vec4f to_pixels(float x1, float y1, float x2, float y2) noexcept; // NvgContext.toPixels (x1,y1,x2,y2)

// =============================================================================================================
// Fonts (NvgFontRenderer)
// =============================================================================================================

enum class FontFamily : uint8_t {
    Tahoma,  // regular + bold (italic faces fall back to the upright ones)
    Icons,   // AdjustIcons.ttf
    Icons2,  // AdjustIcons2.ttf
    Seraph,  // SeraphTags.ttf
};

// Text renderer for one family at an Adjust "nominal" size (Fonts.TAHOMA_12 = Font{Tahoma, 12}); the NanoVG font
// size is half the nominal size, exactly like Adjust. Supports § colour/format codes (§0-§f, §l bold, §o, §n, §m,
// §k, §r). Strings are UTF-8.
class Font {
public:
    constexpr Font(FontFamily family, float nominal) noexcept : family_(family), nominal_(nominal) {}

    constexpr float size() const noexcept { return nominal_ * 0.5f; } // NvgFontRenderer.size
    constexpr float nominal() const noexcept { return nominal_; }
    constexpr FontFamily family() const noexcept { return family_; }

    float width(std::string_view text) const noexcept;                          // getStringWidth
    float draw(std::string_view text, float x, float y, Argb color) const noexcept;  // drawString (with shadow)
    float draw_no_shadow(std::string_view text, float x, float y, Argb color) const noexcept;
    float draw_centered(std::string_view text, float center_x, float y, Argb color) const noexcept;
    std::string trim_to_width(std::string_view text, float max_width) const;    // trimStringToWidth

    // One pass of renderString; returns the x after the text.
    float render(std::string_view text, float x, float y, Argb color, bool shadow) const noexcept;

private:
    FontFamily family_;
    float nominal_;
};

constexpr Font tahoma(float nominal) noexcept { return Font(FontFamily::Tahoma, nominal); }
constexpr Font icons(float nominal) noexcept { return Font(FontFamily::Icons, nominal); }
constexpr Font icons2(float nominal) noexcept { return Font(FontFamily::Icons2, nominal); }
constexpr Font seraph(float nominal) noexcept { return Font(FontFamily::Seraph, nominal); }

// Fonts.TAHOMA_8 ... Fonts.SERAPH_10
inline constexpr Font kTahoma8 = tahoma(8.0f);
inline constexpr Font kTahoma9 = tahoma(9.0f);
inline constexpr Font kTahoma10 = tahoma(10.0f);
inline constexpr Font kTahoma11 = tahoma(11.0f);
inline constexpr Font kTahoma12 = tahoma(12.0f);
inline constexpr Font kTahoma13 = tahoma(13.0f);
inline constexpr Font kTahoma14 = tahoma(14.0f);
inline constexpr Font kTahoma20 = tahoma(20.0f);
inline constexpr Font kTahoma22 = tahoma(22.0f);
inline constexpr Font kIcons9 = icons(9.0f);
inline constexpr Font kIcons11 = icons(11.0f);
inline constexpr Font kIcons12 = icons(12.0f);
inline constexpr Font kIcons13 = icons(13.0f);
inline constexpr Font kIcons14 = icons(14.0f);
inline constexpr Font kSeraph10 = seraph(10.0f);

// Registers the fonts in `vg` once (called by begin_hud / bind_context). Returns false if NanoVG failed.
bool ensure_fonts(NVGcontext* vg) noexcept;
bool tahoma_available() noexcept; // false -> Inter fallback in use

// Minecraft § codes as UTF-8 literals: "§7" -> kFmt "7"
#define ADJUST_FMT "\xC2\xA7"

// =============================================================================================================
// Drawing (NvgDraw) - coordinates are (x1, y1)-(x2, y2) like Adjust unless named otherwise
// =============================================================================================================

void enable_anti_alias() noexcept;
void disable_anti_alias() noexcept;
void set_global_alpha(float alpha) noexcept;
float global_alpha() noexcept;

void fill_rect(Argb color, float x1, float y1, float x2, float y2) noexcept;
void fill_rounded_rect(Argb color, float x1, float y1, float x2, float y2, float radius) noexcept;
void stroke_rect(bool outside, Argb color, float x1, float y1, float x2, float y2, float line_width) noexcept;
void fill_circle(Argb color, float x, float y, float radius) noexcept;
void fill_triangle(Argb color, float x1, float y1, float x2, float y2, float x3, float y3) noexcept;
void fill_gradient_rect(Argb start, Argb end, float x1, float y1, float x2, float y2) noexcept; // top -> bottom
void fill_gradient_rect(Argb start, Argb end, float x1, float y1, float x2, float y2,
                        float sx, float sy, float ex, float ey) noexcept;
void stroke_line(Argb color, float x1, float y1, float x2, float y2, float width) noexcept;

void push_scissor(float x, float y, float width, float height) noexcept;
void pop_scissor() noexcept;

} // namespace slinky::adjust
