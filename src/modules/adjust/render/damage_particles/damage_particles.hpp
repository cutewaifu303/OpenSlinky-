#pragma once

// Adjust "Damage Particles" (com.adjustclient.module.impl.render.DamageParticlesModule + DamageParticle,
// DamageParticlesDisplay, DamageParticlesColorCondition, UnusedDamageParticleData).

#include "events/module.hpp"

namespace slinky::modules {

events::Module* get_adjust_damage_particles_module() noexcept;

} // namespace slinky::modules
