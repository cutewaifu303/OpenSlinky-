#include "adjust/kit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "embedded_assets.hpp"
#include "gui/gui_renderer.hpp"

namespace slinky::adjust {

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Frame / context state (NvgContext statics)
// ---------------------------------------------------------------------------------------------------------------

NVGcontext* g_vg = nullptr;
float g_current_scale = 0.0f;   // currentScale (0 = no frame)
float g_saved_scale = 0.0f;     // savedScale   (0 = not pushed)
float g_scale_factor = 0.0f;    // scaleFactor  (0 = not pushed)
float g_global_alpha = 1.0f;
bool g_frame_saved = false;

struct ScissorRect {
    float x, y, w, h;
};
std::vector<ScissorRect> g_scissors;

// ---------------------------------------------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* kFaceTahoma = "adjust_tahoma";
constexpr const char* kFaceTahomaBold = "adjust_tahoma_bold";
constexpr const char* kFaceIcons = "adjust_icons";
constexpr const char* kFaceIcons2 = "adjust_icons2";
constexpr const char* kFaceSeraph = "adjust_seraph";
constexpr const char* kFaceFallbackRegular = "regular"; // Inter, registered by GuiRenderer
constexpr const char* kFaceFallbackBold = "bold";

NVGcontext* g_fonts_vg = nullptr;
bool g_tahoma_ok = false;

struct FaceSet {
    const char* regular;
    const char* bold;        // nullptr = no bold face (stays on the current face)
    const char* italic;      // nullptr on Windows (Tahoma has no italic file there)
    const char* bold_italic;
};

FaceSet faces_for(FontFamily family) noexcept {
    switch (family) {
        case FontFamily::Icons:
            return {kFaceIcons, nullptr, nullptr, nullptr};
        case FontFamily::Icons2:
            return {kFaceIcons2, nullptr, nullptr, nullptr};
        case FontFamily::Seraph:
            return {kFaceSeraph, nullptr, nullptr, nullptr};
        case FontFamily::Tahoma:
        default:
            if (g_tahoma_ok) {
                return {kFaceTahoma, kFaceTahomaBold, nullptr, nullptr};
            }
            return {kFaceFallbackRegular, kFaceFallbackBold, nullptr, nullptr};
    }
}

bool load_system_font(NVGcontext* vg, const char* face, const char* file_name) noexcept {
    const char* windir = std::getenv("WINDIR");
    std::string path = (windir != nullptr && *windir != '\0') ? windir : "C:\\Windows";
    path += "\\Fonts\\";
    path += file_name;
    return nvgCreateFont(vg, face, path.c_str()) != -1;
}

// Formatting code table: index = code (0-15 colours, 16 k, 17 l, 18 m, 19 n, 20 o, 21 r).
constexpr const char kFormatCodes[] = "0123456789abcdefklmnor";
constexpr uint32_t kColorCodes[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xFFAA00, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

int format_code_index(uint32_t cp) noexcept {
    if (cp >= 'A' && cp <= 'Z') {
        cp = cp - 'A' + 'a';
    }
    for (int i = 0; kFormatCodes[i] != '\0'; ++i) {
        if (static_cast<uint32_t>(static_cast<unsigned char>(kFormatCodes[i])) == cp) {
            return i;
        }
    }
    return -1;
}

constexpr uint32_t kSection = 0x00A7; // '§'

// Decodes one UTF-8 code point at `i`; advances `i`. Invalid bytes decode as themselves.
uint32_t decode_utf8(std::string_view s, size_t& i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80 || i + 1 >= s.size()) {
        ++i;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0) {
        const uint32_t cp = ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
        const uint32_t cp = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                            (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        i += 3;
        return cp;
    }
    if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
        const uint32_t cp = ((b0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                            ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                            (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        i += 4;
        return cp;
    }
    ++i;
    return b0;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

float text_advance(NVGcontext* vg, const std::string& segment) noexcept {
    return nvgTextBounds(vg, 0.0f, 0.0f, segment.c_str(), segment.c_str() + segment.size(), nullptr);
}

uint32_t random_char() noexcept {
    return static_cast<uint32_t>('!' + (std::rand() % ('~' - '!' + 1)));
}

void begin_path() noexcept {
    nvgBeginPath(g_vg);
}

} // namespace

// =============================================================================================================
// Time
// =============================================================================================================

int64_t now_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// =============================================================================================================
// Colour
// =============================================================================================================

NVGcolor to_nvg(Argb c) noexcept {
    return nvgRGBA(static_cast<unsigned char>(red_of(c)), static_cast<unsigned char>(green_of(c)),
                   static_cast<unsigned char>(blue_of(c)), static_cast<unsigned char>(alpha_of(c)));
}

Argb from_color3f(const math::Color3f& c, int alpha) noexcept {
    const auto ch = [](float v) noexcept { return std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255); };
    return argb(ch(c.r), ch(c.g), ch(c.b), alpha);
}

math::Color3f to_color3f(Argb c) noexcept {
    return {static_cast<float>(red_of(c)) / 255.0f, static_cast<float>(green_of(c)) / 255.0f,
            static_cast<float>(blue_of(c)) / 255.0f};
}

Argb with_alpha(Argb c, int alpha) noexcept {
    alpha = std::clamp(alpha, 0, 255);
    return (c & 0x00FFFFFFu) | (static_cast<Argb>(alpha) << 24);
}

Argb blend(Argb first, Argb second, double ratio) noexcept {
    ratio = std::clamp(ratio, 0.0, 1.0);
    const double inv = 1.0 - ratio;
    const auto mix = [&](int a, int b) noexcept {
        return std::clamp(static_cast<int>(a * ratio + b * inv), 0, 255);
    };
    return argb(mix(red_of(first), red_of(second)), mix(green_of(first), green_of(second)),
                mix(blue_of(first), blue_of(second)), mix(alpha_of(first), alpha_of(second)));
}

Argb scale(Argb c, float factor) noexcept {
    const auto s = [&](int v) noexcept { return std::clamp(static_cast<int>(v * factor), 0, 255); };
    return argb(s(red_of(c)), s(green_of(c)), s(blue_of(c)), alpha_of(c));
}

Argb brighter(Argb c, float factor) noexcept {
    int r = red_of(c), g = green_of(c), b = blue_of(c);
    const int floor_v = static_cast<int>(1.0f / (1.0f - factor));
    if (r == 0 && g == 0 && b == 0) {
        return argb(floor_v, floor_v, floor_v, alpha_of(c));
    }
    if (r > 0 && r < floor_v) r = floor_v;
    if (g > 0 && g < floor_v) g = floor_v;
    if (b > 0 && b < floor_v) b = floor_v;
    return argb(std::clamp(static_cast<int>(r / factor), 0, 255), std::clamp(static_cast<int>(g / factor), 0, 255),
                std::clamp(static_cast<int>(b / factor), 0, 255), alpha_of(c));
}

Argb lerp_argb(Argb a, Argb b, double ta, double tb) noexcept {
    const auto mix = [&](int x, int y) noexcept { return std::clamp(static_cast<int>(x * ta + y * tb), 0, 255); };
    return argb(mix(red_of(a), red_of(b)), mix(green_of(a), green_of(b)), mix(blue_of(a), blue_of(b)),
                mix(alpha_of(a), alpha_of(b)));
}

Argb hsb_to_rgb(float hue, float saturation, float brightness) noexcept {
    int r = 0, g = 0, b = 0;
    if (saturation == 0.0f) {
        r = g = b = static_cast<int>(brightness * 255.0f + 0.5f);
    } else {
        const float h = (hue - std::floor(hue)) * 6.0f;
        const float f = h - std::floor(h);
        const float p = brightness * (1.0f - saturation);
        const float q = brightness * (1.0f - saturation * f);
        const float t = brightness * (1.0f - (saturation * (1.0f - f)));
        const auto c = [](float v) noexcept { return static_cast<int>(v * 255.0f + 0.5f); };
        switch (static_cast<int>(h)) {
            case 0: r = c(brightness); g = c(t); b = c(p); break;
            case 1: r = c(q); g = c(brightness); b = c(p); break;
            case 2: r = c(p); g = c(brightness); b = c(t); break;
            case 3: r = c(p); g = c(q); b = c(brightness); break;
            case 4: r = c(t); g = c(p); b = c(brightness); break;
            case 5: r = c(brightness); g = c(p); b = c(q); break;
            default: break;
        }
    }
    return 0xFF000000u | (static_cast<Argb>(r & 0xFF) << 16) | (static_cast<Argb>(g & 0xFF) << 8) |
           static_cast<Argb>(b & 0xFF);
}

void rgb_to_hsb(Argb c, float out_hsb[3]) noexcept {
    const int r = red_of(c), g = green_of(c), b = blue_of(c);
    const int cmax = std::max({r, g, b});
    const int cmin = std::min({r, g, b});
    const float brightness = static_cast<float>(cmax) / 255.0f;
    const float saturation = (cmax != 0) ? static_cast<float>(cmax - cmin) / static_cast<float>(cmax) : 0.0f;
    float hue = 0.0f;
    if (saturation != 0.0f) {
        const float span = static_cast<float>(cmax - cmin);
        const float redc = static_cast<float>(cmax - r) / span;
        const float greenc = static_cast<float>(cmax - g) / span;
        const float bluec = static_cast<float>(cmax - b) / span;
        if (r == cmax) {
            hue = bluec - greenc;
        } else if (g == cmax) {
            hue = 2.0f + redc - bluec;
        } else {
            hue = 4.0f + greenc - redc;
        }
        hue /= 6.0f;
        if (hue < 0.0f) {
            hue += 1.0f;
        }
    }
    out_hsb[0] = hue;
    out_hsb[1] = saturation;
    out_hsb[2] = brightness;
}

void to_hsl(Argb c, float out_hsl[3]) noexcept {
    const float r = static_cast<float>(red_of(c)) / 255.0f;
    const float g = static_cast<float>(green_of(c)) / 255.0f;
    const float b = static_cast<float>(blue_of(c)) / 255.0f;
    const float maxv = std::max(r, std::max(g, b));
    const float minv = std::min(r, std::min(g, b));
    const float delta = maxv - minv;
    float hue = 0.0f;
    if (delta != 0.0f) {
        if (r == maxv) {
            hue = std::fmod((g - b) / delta, 6.0f);
        } else if (g == maxv) {
            hue = (b - r) / delta + 2.0f;
        } else {
            hue = (r - g) / delta + 4.0f;
        }
        hue *= 60.0f;
        if (hue < 0.0f) {
            hue += 360.0f;
        }
    }
    const float lightness = (maxv + minv) / 2.0f;
    out_hsl[0] = hue;
    out_hsl[1] = (delta == 0.0f) ? 0.0f : delta / (1.0f - std::fabs(lightness * 2.0f - 1.0f));
    out_hsl[2] = lightness;
}

Argb hsl_to_argb(const float hsl[3], int alpha) noexcept {
    const float h = hsl[0], s = hsl[1], l = hsl[2];
    const float chroma = (1.0f - std::fabs(l * 2.0f - 1.0f)) * s;
    const float x = chroma * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (h >= 0.0f && h < 60.0f) { r = chroma; g = x; }
    else if (h >= 60.0f && h < 120.0f) { r = x; g = chroma; }
    else if (h >= 120.0f && h < 180.0f) { g = chroma; b = x; }
    else if (h >= 180.0f && h < 240.0f) { g = x; b = chroma; }
    else if (h >= 240.0f && h < 300.0f) { r = x; b = chroma; }
    else if (h >= 300.0f && h < 360.0f) { r = chroma; b = x; }
    const float m = l - chroma / 2.0f;
    const auto ch = [&](float v) noexcept { return std::clamp(static_cast<int>((v + m) * 255.0f), 0, 255); };
    return argb(ch(r), ch(g), ch(b), alpha);
}

Argb rainbow(float offset, float saturation, float brightness) noexcept {
    const float hue = (static_cast<float>(now_ms() % 5000) + (5000.0f - offset * 30.0f)) / 5000.0f;
    return hsb_to_rgb(hue, saturation, brightness);
}

int pulse() noexcept {
    int64_t t = now_ms() % 1300;
    if (t >= 650) {
        t = 1300 - t;
    }
    return std::max(0, static_cast<int>(static_cast<float>((std::sin(static_cast<double>(t) / 1300.0) * math::kPi + 1.0) * 40.0)));
}

// =============================================================================================================
// Animation
// =============================================================================================================

float ease(Easing e, float x) noexcept {
    switch (e) {
        case Easing::Linear: return x;
        case Easing::EaseInQuad: return x * x;
        case Easing::EaseOutQuad: return (2.0f - x) * x;
        case Easing::EaseInOutQuad: return x < 0.5f ? (x * 2.0f) * x : (4.0f - x * 2.0f) * x + -1.0f;
        case Easing::EaseInCubic: return (x * x) * x;
        case Easing::EaseOutCubic: return ((x - 1.0f) * (x - 1.0f)) * (x - 1.0f) + 1.0f;
        case Easing::EaseInOutCubic:
            return x >= 0.5f ? ((x - 1.0f) * (x * 2.0f - 2.0f)) * (x * 2.0f - 2.0f) + 1.0f : ((x * 4.0f) * x) * x;
        case Easing::EaseInQuart: return ((x * x) * x) * x;
        case Easing::EaseOutQuart: return 1.0f - (((x - 1.0f) * (x - 1.0f)) * (x - 1.0f)) * (x - 1.0f);
        case Easing::EaseInOutQuart:
            return x >= 0.5f ? 1.0f - (((8.0f * (x - 1.0f)) * (x - 1.0f)) * (x - 1.0f)) * (x - 1.0f)
                             : (((8.0f * x) * x) * x) * x;
        case Easing::EaseInQuint: return (((x * x) * x) * x) * x;
        case Easing::EaseOutQuint:
            return ((((x - 1.0f) * (x - 1.0f)) * (x - 1.0f)) * (x - 1.0f)) * (x - 1.0f) + 1.0f;
        case Easing::EaseInOutQuint:
            return x >= 0.5f ? ((((16.0f * (x - 1.0f)) * (x - 1.0f)) * (x - 1.0f)) * (x - 1.0f)) * (x - 1.0f) + 1.0f
                             : ((((16.0f * x) * x) * x) * x) * x;
        case Easing::EaseInSine:
        case Easing::EaseInOutSine:
            return 1.0f - std::cos(static_cast<float>(static_cast<double>(x) * math::kPi * 0.5));
        case Easing::EaseOutSine: return std::sin(static_cast<float>(static_cast<double>(x) * math::kPi * 0.5));
        case Easing::EaseInExpo: return x == 0.0f ? 0.0f : static_cast<float>(std::pow(2.0, static_cast<double>(x * 10.0f - 10.0f)));
        case Easing::EaseOutExpo: return x == 1.0f ? 1.0f : 1.0f - static_cast<float>(std::pow(2.0, static_cast<double>(x * -10.0f)));
        case Easing::EaseInOutExpo:
            if (x == 0.0f) return 0.0f;
            if (x == 1.0f) return 1.0f;
            return x < 0.5f ? static_cast<float>(std::pow(2.0, static_cast<double>(x * 20.0f - 10.0f))) * 0.5f
                            : (2.0f - static_cast<float>(std::pow(2.0, static_cast<double>(x * -20.0f + 10.0f)))) * 0.5f;
        case Easing::EaseInCirc: return 1.0f - std::sqrt(1.0f - x * x);
        case Easing::EaseOutCirc: return std::sqrt(1.0f - (x - 1.0f) * (x - 1.0f));
        case Easing::EaseInOutCirc:
            return x < 0.5f ? (1.0f - std::sqrt(1.0f - (4.0f * x) * x)) * 0.5f
                            : (std::sqrt(1.0f - ((x - 1.0f) * 4.0f) * x) + 1.0f) * 0.5f;
        case Easing::Sigmoid: return 1.0f / (std::exp(-x) + 1.0f);
        case Easing::EaseOutElastic:
            if (x == 0.0f) return 0.0f;
            if (x == 1.0f) return 1.0f;
            return static_cast<float>(std::sin((static_cast<double>(x * 10.0f) - 0.75) * 2.0943951023931953) *
                                      std::pow(2.0, static_cast<double>(x * -10.0f)) * 0.5 + 1.0);
        case Easing::EaseInBack: return ((x * 2.70158f) * x) * x - (x * 1.70158f) * x;
        case Easing::Parabolic: return 1.0f - (x * 2.0f - 1.0f) * (x * 2.0f - 1.0f);
    }
    return x;
}

Animation::Animation(Easing e, int64_t duration_ms) noexcept : Animation(e, duration_ms, now_ms()) {}

Animation::Animation(Easing e, int64_t duration_ms, int64_t started_at_ms) noexcept
    : easing(e), duration(duration_ms), started_at(started_at_ms) {}

void Animation::restart() noexcept {
    started_at = now_ms();
    from = value;
    finished = false;
}

float Animation::update(float requested) noexcept {
    last_call = now_ms();
    if (requested != target) {
        target = requested;
        started_at = now_ms();
        from = value;
        finished = false;
    } else if (last_call - duration > started_at) {
        finished = true;
        return value = requested;
    } else {
        finished = value == requested;
        if (finished) {
            return value = requested;
        }
    }
    const float progress = ease(easing, static_cast<float>(now_ms() - started_at) / static_cast<float>(duration));
    if (duration == 0) {
        value = requested;
    } else if (value > requested) {
        value = from - (from - requested) * progress;
    } else {
        value = (requested - from) * progress + from;
    }
    if (!std::isfinite(value)) {
        value = requested;
    }
    return value;
}

TimedFade::TimedFade(int64_t in_ms, int64_t hold, int64_t out_ms) noexcept
    : fade_in_ms(in_ms), hold_ms(hold), fade_out_ms(out_ms) {}

float TimedFade::value() noexcept {
    if (!has_fade_in) {
        fade_in = Animation(Easing::Linear, fade_in_ms);
        has_fade_in = true;
    }
    if (!fade_in.finished) {
        fade_in.update(1.0f);
        return fade_in.value;
    }
    if (!has_timer) {
        hold_started = now_ms();
        has_timer = true;
    }
    if (now_ms() - hold_started < hold_ms) {
        return 1.0f;
    }
    if (!has_fade_out) {
        fade_out = Animation(Easing::Linear, fade_out_ms);
        fade_out.value = 1.0f;
        fade_out.started_at = now_ms();
        fade_out.from = fade_out.value;
        fade_out.finished = false;
        has_fade_out = true;
    }
    if (fade_out.finished) {
        return 0.0f;
    }
    fade_out.update(0.0f);
    return fade_out.value;
}

// =============================================================================================================
// HUD frame
// =============================================================================================================

float resolve_hud_scale(float requested, float fb_width, float fb_height) noexcept {
    double d0 = 1.0;
    while (static_cast<double>(requested) > d0 &&
           static_cast<double>(fb_width) / (d0 + 1.0) >= 320.0 &&
           static_cast<double>(fb_height) / (d0 + 1.0) >= 240.0) {
        d0 += 0.25;
    }
    return static_cast<float>(std::max(2.0, d0));
}

float hud_scale_from_setting(int setting_value) noexcept {
    if (setting_value >= 16) {
        return 1000.0f;
    }
    return static_cast<float>(static_cast<double>(setting_value - 1) * 0.25 + 2.0);
}

HudFrame begin_hud(gui::GuiRenderer* renderer, float requested_scale) noexcept {
    HudFrame frame{};
    if (renderer == nullptr || renderer->vg == nullptr) {
        return frame;
    }
    NVGcontext* v = renderer->vg;
    const float fb_w = renderer->logical_width * renderer->scale;
    const float fb_h = renderer->logical_height * renderer->scale;
    const float s = resolve_hud_scale(requested_scale, fb_w, fb_h);

    nvgSave(v);
    nvgResetTransform(v);
    nvgResetScissor(v);
    nvgScale(v, s, s);
    nvgShapeAntiAlias(v, 0);
    nvgGlobalAlpha(v, 1.0f);
    g_frame_saved = true;

    g_vg = v;
    g_current_scale = s;
    g_saved_scale = 0.0f;
    g_scale_factor = 0.0f;
    g_global_alpha = 1.0f;
    g_scissors.clear();
    ensure_fonts(v);

    frame.scale = s;
    frame.width = fb_w / s;
    frame.height = fb_h / s;
    frame.scaled_width = static_cast<int>(std::ceil(frame.width));
    frame.scaled_height = static_cast<int>(std::ceil(frame.height));
    frame.valid = true;
    return frame;
}

void end_hud() noexcept {
    if (g_vg != nullptr && g_frame_saved) {
        nvgRestore(g_vg);
    }
    g_frame_saved = false;
    g_vg = nullptr;
    g_current_scale = 0.0f;
    g_saved_scale = 0.0f;
    g_scale_factor = 0.0f;
    g_scissors.clear();
}

void bind_context(NVGcontext* vg_ctx) noexcept {
    g_vg = vg_ctx;
    if (vg_ctx != nullptr) {
        ensure_fonts(vg_ctx);
    }
}

NVGcontext* vg() noexcept {
    return g_vg;
}

float current_scale() noexcept {
    return g_current_scale;
}

void set_scale(float s) noexcept {
    if (g_vg == nullptr || g_current_scale == 0.0f || s == 0.0f) {
        return;
    }
    const float relative = s / g_current_scale;
    nvgScale(g_vg, relative, relative);
    g_current_scale = s;
}

void push_even_scale() noexcept {
    if (g_vg == nullptr || g_current_scale == 0.0f) {
        return;
    }
    if (std::fmod(g_current_scale, 2.0f) == 0.0f) {
        return;
    }
    g_saved_scale = g_current_scale;
    nvgScale(g_vg, 2.0f / g_current_scale, 2.0f / g_current_scale);
    g_current_scale = 2.0f;
    g_scale_factor = g_saved_scale / 2.0f;
}

void pop_even_scale() noexcept {
    if (g_saved_scale == 0.0f) {
        return;
    }
    set_scale(g_saved_scale);
    g_scale_factor = 0.0f;
    g_saved_scale = 0.0f;
}

float even_scale_factor() noexcept {
    return g_scale_factor;
}

math::Vec4f to_pixels(float x1, float y1, float x2, float y2) noexcept {
    if (g_saved_scale == 0.0f) {
        return math::Vec4f{x1, y1, x2, y2};
    }
    const float factor = g_scale_factor;
    const float px1 = x1 * factor;
    const float py1 = y1 * factor;
    const float height = y2 - y1;
    const float width = x2 - x1;
    const auto whole = static_cast<float>(static_cast<int>(factor));
    const float px2 = width > 1.0f ? x2 * factor : px1 + width * whole;
    const float py2 = height > 1.0f ? y2 * factor : py1 + height * whole;
    return math::Vec4f{px1, py1, px2, py2};
}

// =============================================================================================================
// Fonts
// =============================================================================================================

bool ensure_fonts(NVGcontext* vg_ctx) noexcept {
    if (vg_ctx == nullptr) {
        return false;
    }
    if (g_fonts_vg == vg_ctx) {
        return true;
    }
    g_fonts_vg = vg_ctx;

    const bool regular_ok = load_system_font(vg_ctx, kFaceTahoma, "tahoma.ttf");
    const bool bold_ok = load_system_font(vg_ctx, kFaceTahomaBold, "tahomabd.ttf");
    g_tahoma_ok = regular_ok && bold_ok;

    bool ok = true;
    ok &= nvgCreateFontMem(vg_ctx, kFaceIcons, const_cast<unsigned char*>(assets::kFontAdjustIcons),
                           static_cast<int>(assets::kFontAdjustIcons_size), 0) != -1;
    ok &= nvgCreateFontMem(vg_ctx, kFaceIcons2, const_cast<unsigned char*>(assets::kFontAdjustIcons2),
                           static_cast<int>(assets::kFontAdjustIcons2_size), 0) != -1;
    ok &= nvgCreateFontMem(vg_ctx, kFaceSeraph, const_cast<unsigned char*>(assets::kFontSeraphTags),
                           static_cast<int>(assets::kFontSeraphTags_size), 0) != -1;
    if (g_tahoma_ok) {
        // Glyphs Tahoma lacks fall back to Inter.
        nvgAddFallbackFont(vg_ctx, kFaceTahoma, kFaceFallbackRegular);
        nvgAddFallbackFont(vg_ctx, kFaceTahomaBold, kFaceFallbackBold);
    }
    return ok;
}

bool tahoma_available() noexcept {
    return g_tahoma_ok;
}

float Font::width(std::string_view text) const noexcept {
    if (g_vg == nullptr || text.empty()) {
        return 0.0f;
    }
    const FaceSet faces = faces_for(family_);
    nvgFontFace(g_vg, faces.regular);
    nvgFontSize(g_vg, size());
    nvgTextAlign(g_vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

    float total = 0.0f;
    std::string segment;
    size_t i = 0;
    while (i < text.size()) {
        const uint32_t cp = decode_utf8(text, i);
        if (cp >= 0x700) {
            continue;
        }
        if (cp == kSection && i < text.size()) {
            if (!segment.empty()) {
                total += text_advance(g_vg, segment);
                segment.clear();
            }
            const uint32_t code_cp = decode_utf8(text, i);
            const int code = format_code_index(code_cp);
            if (code < 16 || code == 21) {
                nvgFontFace(g_vg, faces.regular);
            } else if (code == 17 && faces.bold != nullptr) {
                nvgFontFace(g_vg, faces.bold);
            }
            continue;
        }
        append_utf8(segment, cp);
    }
    if (!segment.empty()) {
        total += text_advance(g_vg, segment);
    }
    return total;
}

float Font::render(std::string_view text, float x, float y, Argb color, bool shadow) const noexcept {
    if (g_vg == nullptr || text.empty()) {
        return x;
    }
    if (shadow) {
        color = shadow_of(color);
    }
    const FaceSet faces = faces_for(family_);
    const float sz = size();
    nvgFontFace(g_vg, faces.regular);
    nvgFontSize(g_vg, sz);
    nvgTextAlign(g_vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    const int alpha = alpha_of(color);
    const NVGcolor base = to_nvg(color);
    nvgFillColor(g_vg, base);

    bool obfuscated = false;
    bool strikethrough = false;
    bool underline = false;
    bool italic = false;
    bool bold = false;
    std::string segment;

    const auto draw_segment = [&]() noexcept {
        if (segment.empty()) {
            return;
        }
        const char* begin = segment.c_str();
        const char* end = begin + segment.size();
        nvgText(g_vg, x, y + sz - 2.5f, begin, end);
        const float w = nvgTextBounds(g_vg, 0.0f, 0.0f, begin, end, nullptr);
        if (strikethrough) {
            const float line_y = sz * 0.15f + y;
            nvgBeginPath(g_vg);
            nvgMoveTo(g_vg, x, line_y);
            nvgLineTo(g_vg, x + (w + 0.25f), line_y + 0.5f);
            nvgFill(g_vg);
        }
        if (underline) {
            const float line_y = sz * 0.5f + y + 1.0f;
            nvgBeginPath(g_vg);
            nvgMoveTo(g_vg, x, line_y);
            nvgLineTo(g_vg, x + (w + 0.25f), line_y + 0.5f);
            nvgFill(g_vg);
        }
        x += w;
        segment.clear();
    };

    size_t i = 0;
    while (i < text.size()) {
        const uint32_t cp = decode_utf8(text, i);
        if (cp == kSection && i < text.size()) {
            draw_segment();
            const int code = format_code_index(decode_utf8(text, i));
            if (code < 0) {
                continue;
            }
            if (code < 16) {
                nvgFontFace(g_vg, faces.regular);
                Argb rgb = kColorCodes[code];
                if (shadow) {
                    rgb = shadow_of(rgb);
                }
                nvgFillColor(g_vg, to_nvg((static_cast<Argb>(alpha) << 24) | (rgb & 0xFFFFFFu)));
                obfuscated = false;
                italic = false;
                bold = false;
            } else if (code == 16) {
                obfuscated = true;
            } else if (code == 17) {
                if (faces.bold != nullptr) {
                    nvgFontFace(g_vg, (italic && faces.bold_italic != nullptr) ? faces.bold_italic : faces.bold);
                }
                bold = true;
            } else if (code == 18) {
                strikethrough = true;
            } else if (code == 19) {
                underline = true;
            } else if (code == 20) {
                if (faces.italic != nullptr) {
                    nvgFontFace(g_vg, (bold && faces.bold_italic != nullptr) ? faces.bold_italic : faces.italic);
                }
                italic = true;
            } else {
                nvgFontFace(g_vg, faces.regular);
                nvgFillColor(g_vg, base);
                obfuscated = false;
                italic = false;
                bold = false;
            }
            continue;
        }
        append_utf8(segment, obfuscated ? random_char() : cp);
    }
    draw_segment();
    return x;
}

float Font::draw(std::string_view text, float x, float y, Argb color) const noexcept {
    render(text, x + 0.5f, y + 0.5f, color, true);
    return render(text, x, y, color, false);
}

float Font::draw_no_shadow(std::string_view text, float x, float y, Argb color) const noexcept {
    return render(text, x, y, color, false);
}

float Font::draw_centered(std::string_view text, float center_x, float y, Argb color) const noexcept {
    return draw(text, center_x - width(text) * 0.5f, y, color);
}

std::string Font::trim_to_width(std::string_view text, float max_width) const {
    std::string result;
    float current = 0.0f;
    bool after_section = false;
    bool bold = false;
    size_t i = 0;
    while (i < text.size() && current < max_width) {
        const size_t start = i;
        const uint32_t cp = decode_utf8(text, i);
        if (cp == kSection) {
            after_section = true;
        } else if (after_section) {
            after_section = false;
            if (cp == 'l' || cp == 'L') {
                bold = true;
            } else if (cp == 'r' || cp == 'R') {
                bold = false;
            }
        } else {
            std::string one;
            append_utf8(one, cp);
            current += width(one);
            if (bold) {
                current += 1.0f;
            }
            if (current > max_width) {
                break;
            }
        }
        result.append(text.substr(start, i - start));
    }
    return result;
}

// =============================================================================================================
// Drawing
// =============================================================================================================

void enable_anti_alias() noexcept {
    if (g_vg != nullptr) {
        nvgShapeAntiAlias(g_vg, 1);
    }
}

void disable_anti_alias() noexcept {
    if (g_vg != nullptr) {
        nvgShapeAntiAlias(g_vg, 0);
    }
}

void set_global_alpha(float alpha) noexcept {
    if (g_vg != nullptr) {
        nvgGlobalAlpha(g_vg, alpha);
    }
    g_global_alpha = alpha;
}

float global_alpha() noexcept {
    return g_global_alpha;
}

void fill_rect(Argb color, float x1, float y1, float x2, float y2) noexcept {
    if (g_vg == nullptr) {
        return;
    }
    nvgFillColor(g_vg, to_nvg(color));
    begin_path();
    nvgRect(g_vg, x1, y1, x2 - x1, y2 - y1);
    nvgFill(g_vg);
}

void fill_rounded_rect(Argb color, float x1, float y1, float x2, float y2, float radius) noexcept {
    if (g_vg == nullptr) {
        return;
    }
    nvgFillColor(g_vg, to_nvg(color));
    enable_anti_alias();
    begin_path();
    nvgRoundedRect(g_vg, x1, y1, x2 - x1, y2 - y1, radius);
    nvgFill(g_vg);
    disable_anti_alias();
}

void stroke_rect(bool outside, Argb color, float x1, float y1, float x2, float y2, float line_width) noexcept {
    if (g_vg == nullptr) {
        return;
    }
    nvgStrokeColor(g_vg, to_nvg(color));
    const float half = line_width * 0.5f;
    const float w = x2 - x1;
    const float h = y2 - y1;
    begin_path();
    if (outside) {
        nvgRect(g_vg, x1 - half, y1 - half, w + line_width, h + line_width);
    } else {
        nvgRect(g_vg, x1 + half, y1 + half, w - line_width, h - line_width);
    }
    nvgStrokeWidth(g_vg, line_width);
    nvgStroke(g_vg);
}

void fill_circle(Argb color, float x, float y, float radius) noexcept {
    if (g_vg == nullptr) {
        return;
    }
    nvgFillColor(g_vg, to_nvg(color));
    enable_anti_alias();
    begin_path();
    nvgCircle(g_vg, x, y, radius);
    nvgFill(g_vg);
    disable_anti_alias();
}

void fill_triangle(Argb color, float x1, float y1, float x2, float y2, float x3, float y3) noexcept {
    if (g_vg == nullptr) {
        return;
    }
    nvgFillColor(g_vg, to_nvg(color));
    enable_anti_alias();
    begin_path();
    nvgMoveTo(g_vg, x1, y1);
    nvgLineTo(g_vg, x2, y2);
    nvgLineTo(g_vg, x3, y3);
    nvgFill(g_vg);
    disable_anti_alias();
}

void fill_gradient_rect(Argb start, Argb end, float x1, float y1, float x2, float y2) noexcept {
    fill_gradient_rect(start, end, x1, y1, x2, y2, x1, y1, x1, y2);
}

void fill_gradient_rect(Argb start, Argb end, float x1, float y1, float x2, float y2,
                        float sx, float sy, float ex, float ey) noexcept {
    if (g_vg == nullptr) {
        return;
    }
    begin_path();
    nvgRect(g_vg, x1, y1, x2 - x1, y2 - y1);
    nvgFillPaint(g_vg, nvgLinearGradient(g_vg, sx, sy, ex, ey, to_nvg(start), to_nvg(end)));
    nvgFill(g_vg);
}

void stroke_line(Argb color, float x1, float y1, float x2, float y2, float width) noexcept {
    if (g_vg == nullptr) {
        return;
    }
    nvgStrokeColor(g_vg, to_nvg(color));
    nvgStrokeWidth(g_vg, width);
    begin_path();
    nvgMoveTo(g_vg, x1, y1);
    nvgLineTo(g_vg, x2, y2);
    nvgStroke(g_vg);
}

void push_scissor(float x, float y, float width, float height) noexcept {
    g_scissors.push_back(ScissorRect{x, y, width, height});
    if (g_vg != nullptr) {
        nvgIntersectScissor(g_vg, x, y, width, height);
    }
}

void pop_scissor() noexcept {
    if (g_vg == nullptr) {
        return;
    }
    nvgResetScissor(g_vg);
    if (!g_scissors.empty()) {
        g_scissors.pop_back();
    }
    for (const ScissorRect& r : g_scissors) {
        nvgIntersectScissor(g_vg, r.x, r.y, r.w, r.h);
    }
}

} // namespace slinky::adjust
