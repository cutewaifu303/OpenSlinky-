#pragma once

// Central place for every user-visible client name.
//
// OpenSlinky++ only rebrands what you can see (HUD, GUI, dialogs, notifications). Everything that is
// stored or used for interop deliberately keeps the original Slinky names so existing setups keep
// working: the %USERPROFILE%\.slinky\ folder (profiles + settings), the config JSON format, the
// `slinky_init` export, the injected Java classes (SlinkyHooks13 / SlinkyChannelHandler) and the
// `slinky_handler` Netty pipeline name.

// Macro form so it can be concatenated with other string literals ("Unload " SLINKY_BRAND_NAME "?").
#define SLINKY_BRAND_NAME "OpenSlinky++"

namespace slinky::branding {

inline constexpr const char* kClientName      = SLINKY_BRAND_NAME; // "OpenSlinky++"
inline constexpr const char* kClientNameUpper = "OPENSLINKY++";    // notification badge
inline constexpr const char* kWatermarkBase   = "OpenSlinky";      // watermark, drawn in the text color
inline constexpr const char* kWatermarkSuffix = "++";              // watermark, drawn in the accent color

} // namespace slinky::branding
