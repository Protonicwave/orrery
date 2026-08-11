#pragma once

/// \file
/// The order the tree solver reads particles in, and the reason it is not the
/// order they arrived in.
///
/// A Barnes-Hut evaluation asks the same question of every particle: which
/// cells of the tree are far enough away to be treated as one mass. Two
/// particles that sit beside each other in space answer it almost identically,
/// so if they are also beside each other in memory the second one's walk finds
/// the nodes the first one just read still in cache. Two particles that sit
/// beside each other in the *input* answer it in completely different ways,
/// because nothing in a Plummer sampler or a simulation's history makes the
/// order of the array anything to do with the geometry.
///
/// A Morton code is the standard way to buy that. Interleaving the bits of the
/// three integer coordinates produces a single number whose ordering follows a
/// space-filling curve, and the curve has the property this project wants: two
/// points close on the curve are always close in space. The converse fails at
/// the seams of the curve and that is fine, because the cost of a seam is a
/// cache miss rather than a wrong answer.
///
/// The same codes then build the tree. Sorted by code, the particles of any
/// octree cell occupy one contiguous range, and the eight children of a cell
/// are the eight sub-ranges split by the three bits at the next level. So the
/// sort that was done for locality also removes the need for any pointer
/// chasing, any per-node particle list and any insertion pass during
/// construction. `solvers/octree.hpp` is the file that spends that.
///
/// ## The grid
///
/// 21 bits per axis, which is 63 bits of code in a 64-bit integer and the most
/// the type can hold for three axes. That is a grid of two million cells along
/// each side of the bounding cube, and it is also the maximum depth of the tree,
/// since one level of subdivision consumes one bit per axis.
///
/// Both figures are far beyond what this machine will ever need. A million
/// particles in a Plummer sphere reach a depth of about twenty only in the
/// densest part of the core, and the tree stops subdividing long before the grid
/// runs out because a cell holding few enough particles becomes a leaf. The
/// depth limit therefore exists for the one configuration that would otherwise
/// not terminate: particles at identical positions, which share a code exactly
/// and cannot be separated by any number of levels.
///
/// ## What is deliberately not here
///
/// The codes are not a spatial index anything queries, and no part of the
/// project holds one between evaluations. They are computed, sorted, used to
/// build a tree and discarded, every force evaluation, because the positions
/// have moved by then and a stale ordering is a slower ordering rather than a
/// wrong one. ADR-0022 records why that cost is paid rather than amortised.

#include <cstdint>
#include <span>
#include <vector>

#include "orrery/backend/executor.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::solvers {

/// A position on the space-filling curve.
///
/// Unsigned 64-bit, of which 63 bits are used. The unused top bit is not a
/// waste worth recovering: a 32-bit code would give ten levels of depth, which
/// a million-particle configuration exhausts in its core, and the alternative
/// of a 96-bit code would cost more to sort than the locality is worth.
using MortonCode = std::uint64_t;

/// Bits of code per axis, and so the number of levels of subdivision available.
inline constexpr unsigned kMortonBitsPerAxis = 21;

/// The number of grid cells along one side of the bounding cube.
inline constexpr std::uint32_t kMortonGridSize = std::uint32_t{1} << kMortonBitsPerAxis;

/// The cube that encloses a configuration.
///
/// A cube rather than the tight bounding box, because the octree that follows
/// halves each axis at every level and cells that are cubes at the root stay
/// cubes all the way down. The opening criterion in `octree.hpp` compares one
/// cell size against one distance, and a cell with three different sizes would
/// leave the criterion to choose between them.
struct BoundingCube {
    /// The corner with the lowest coordinate on every axis.
    core::Vec3 origin;

    /// The length of one side. Zero only for a configuration whose particles
    /// are all at one point, or for an empty one.
    core::Real size{};

    [[nodiscard]] core::Vec3 centre() const noexcept {
        return origin + core::Vec3{size / 2, size / 2, size / 2};
    }
};

/// The smallest cube containing every position, centred on their bounding box.
///
/// Serial, and O(N) with a body of six comparisons. It is left serial because a
/// reduction across the work-stealing scheduler would depend on how the range
/// happened to be divided, and although minimum and maximum are exact in
/// floating-point arithmetic and so would give the same answer either way, the
/// cost being avoided is under one per cent of a tree build. The measurement is
/// in `docs/performance/barnes_hut.md` rather than assumed.
///
/// An empty configuration gives a cube of zero size at the origin, which the
/// tree builder turns into no nodes at all.
[[nodiscard]] BoundingCube bounding_cube(core::Vec3Span<const core::Real> positions) noexcept;

/// Spread the low 21 bits of `value` out to every third bit.
///
/// The standard branch-free dilation: each step splits the bits into two halves
/// and moves the upper half further apart, so five steps take 21 adjacent bits
/// to 21 bits spaced three apart. The alternative is a loop of 21 iterations
/// with a shift and a mask in each, which computes the same function about ten
/// times more slowly, once per particle per axis per force evaluation.
///
/// Bits above the low 21 are ignored rather than rejected. Every caller here
/// derives the argument from a coordinate already clamped to the grid.
[[nodiscard]] MortonCode spread_bits(std::uint32_t value) noexcept;

/// The code of a point at integer grid coordinates.
///
/// The x bit of each triple is the most significant, then y, then z. Which axis
/// takes which position is arbitrary and only has to be consistent, since it
/// amounts to a relabelling of the eight children of every cell. It is stated
/// because `octree.hpp` derives the geometric centre of a child from its octant
/// index and has to agree with this.
[[nodiscard]] MortonCode morton_code(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept;

/// The code of a position within `cube`.
///
/// A position outside the cube is clamped onto its boundary rather than
/// rejected. The cube comes from the same positions, so this cannot happen for
/// a caller that used `bounding_cube`; the clamp is there because the mapping
/// is a floating-point multiplication and a coordinate exactly on the far face
/// would otherwise round to one past the last grid cell.
[[nodiscard]] MortonCode morton_code(core::Vec3 position, const BoundingCube& cube) noexcept;

/// The octant, 0 to 7, that `code` occupies at `level`.
///
/// Level 1 is the root's eight children and level `kMortonBitsPerAxis` is the
/// finest the code can express. Level 0 would be the root itself, which is not
/// an octant of anything, and returns zero.
[[nodiscard]] unsigned morton_octant(MortonCode code, unsigned level) noexcept;

/// One particle's place on the curve.
///
/// The code and the index travel together because both are needed after the
/// sort: the code builds the tree and the index says which particle of the
/// caller's configuration each sorted slot came from. Sorting them as one
/// 16-byte record rather than as two arrays permuted afterwards is the choice
/// that keeps the sort to one pass over one array.
struct MortonKey {
    MortonCode code{};
    core::Index index{};

    /// Ordered by code, and by index where two codes are equal.
    ///
    /// Defaulted, which for a class compares the members in declaration order
    /// and so is exactly the ordering wanted here rather than a shorthand for
    /// something close to it.
    ///
    /// The tie-break on the index is what makes the ordering total rather than
    /// merely weak, and a total order is what makes the result independent of
    /// the sorting algorithm, of the number of threads it ran on and of how
    /// they happened to divide the array. Identical codes are common: every
    /// configuration with two particles closer together than the finest grid
    /// cell has some, and a configuration with repeated positions has many.
    /// Without the tie-break their order would be an implementation detail, and
    /// each particle's acceleration is summed in the order they end up in.
    [[nodiscard]] friend constexpr auto operator<=>(const MortonKey&,
                                                    const MortonKey&) noexcept = default;
};

/// The particles of a configuration, in the order the tree wants them.
///
/// Holds the sorted keys and the scratch space the sort needs, so that a solver
/// evaluating millions of times allocates on the first evaluation and on none of
/// the rest.
class MortonOrdering {
public:
    /// Compute and sort the codes of `positions`.
    ///
    /// Runs the sort through `executor` where one is given, and on the calling
    /// thread otherwise. The result does not depend on which: the ordering is
    /// total, so every path produces the same permutation.
    ///
    /// Safe to call with no particles.
    void build(core::Vec3Span<const core::Real> positions, backend::Executor* executor);

    /// The keys, ascending. Empty until `build` has been called.
    [[nodiscard]] std::span<const MortonKey> keys() const noexcept { return keys_; }

    /// The cube the codes were computed in.
    [[nodiscard]] const BoundingCube& cube() const noexcept { return cube_; }

private:
    /// Sort `keys_`, using `scratch_` as the second buffer of a merge sort.
    ///
    /// Split out from `build` because it is the only part that has a parallel
    /// form and a serial one, and because the argument for the parallel form is
    /// long enough that it wants a comment of its own.
    void sort(backend::Executor* executor);

    std::vector<MortonKey> keys_;
    std::vector<MortonKey> scratch_;
    BoundingCube cube_;
};

} // namespace orrery::solvers
