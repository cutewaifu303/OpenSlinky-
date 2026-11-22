#pragma once

#include <cstddef>
#include "events/module.hpp"

namespace slinky::modules {

inline constexpr size_t kModuleRegistryCount = 98;

// Returns the module registry (97 reverse-engineered modules + OpenSlinky++ Alt Manager).
// The first 97 are in exact alphabetical order (matches off_18016BC90); the extras are appended last.
events::Module* const* get_module_registry() noexcept;

} // namespace slinky::modules
