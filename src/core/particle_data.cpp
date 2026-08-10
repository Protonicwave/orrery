#include "orrery/core/particle_data.hpp"

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::core {

ParticleData::ParticleData(Index count) {
    resize(count);
}

void ParticleData::reserve(Index count) {
    // Reserving can throw, and if it does it will have grown some arrays and
    // not others. That is harmless: spare capacity is not observable through
    // this class, and no array has changed length, so the invariant that
    // matters still holds.
    for_each_array([count](ComponentArray& array) { array.reserve(count); });
}

void ParticleData::resize(Index count) {
    // Allocating for all ten arrays first is what makes this operation safe to
    // fail. Once every array has the capacity, no resize below can allocate,
    // and since Real has no throwing operations none of them can throw. Without
    // this, a failure while growing the sixth array would leave five arrays
    // longer than the other five and the class invariant broken.
    if (count > capacity()) {
        reserve(count);
    }

    for_each_array([count](ComponentArray& array) { array.resize(count); });
}

void ParticleData::clear() noexcept {
    for_each_array([](ComponentArray& array) { array.clear(); });
}

Index ParticleData::add(Vec3 position, Vec3 velocity, Real mass) {
    const Index index = size();

    if (index == capacity()) {
        // Doubling reproduces the geometric growth a vector applies to itself.
        // Growing by one instead would reallocate and copy all ten arrays on
        // every call, which turns a loop of appends into quadratic work.
        reserve(capacity() == 0 ? kInitialCapacity : capacity() * 2);
    }

    resize(index + 1);
    positions().set(index, position);
    velocities().set(index, velocity);
    mass_[index] = mass;

    // The acceleration of the new particle is left at the zero the resize wrote
    // there. It is an output of the force solver, not an input, and any value
    // set here would be overwritten before it was read.
    return index;
}

} // namespace orrery::core
