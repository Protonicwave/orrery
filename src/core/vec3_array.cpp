#include "orrery/core/vec3_array.hpp"

#include "orrery/core/types.hpp"

namespace orrery::core {

Vec3Array::Vec3Array(Index count) {
    resize(count);
}

void Vec3Array::resize(Index count) {
    // Reserving all three before resizing any is what makes a failed allocation
    // harmless: once the capacity is there no resize below can allocate, and
    // since `Real` has no throwing operations none of them can throw. The same
    // argument as in `ParticleData::resize`, and the same reason for it.
    if (count > x_.capacity()) {
        x_.reserve(count);
        y_.reserve(count);
        z_.reserve(count);
    }

    x_.resize(count);
    y_.resize(count);
    z_.resize(count);
}

} // namespace orrery::core
