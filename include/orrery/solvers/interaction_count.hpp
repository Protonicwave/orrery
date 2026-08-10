#pragma once

/// \file
/// The unit in which the cost of a force evaluation is reported.
///
/// The whole point of a tree solver is that it computes fewer interactions than
/// direct summation for an answer that is nearly as good, and the whole point of
/// keeping the direct solver afterwards is to say how much nearly is. Neither
/// statement can be made in seconds. A timing compares two implementations on one
/// machine on one afternoon, and it moves with the thread count, the vector
/// width, the memory layout and the temperature of the laptop. An interaction
/// count is a property of the algorithm alone: it is the same number on any
/// machine, it is what the O(N^2) and O(N log N) in the two names refer to, and
/// it is the denominator that turns a timing into a rate that can be put beside
/// the roofline of Phase 7.
///
/// So the counter is written now, in the phase that has only one algorithm to
/// count, rather than in Phase 8 where it would arrive alongside the result it is
/// meant to substantiate.

#include <cstdint>

namespace orrery::solvers {

/// What a solver has done since the count was last reset.
///
/// An aggregate of plain counters, because it is a record of measurements with
/// no invariant relating them.
///
/// The counters are 64-bit explicitly rather than `Index`, which is the natural
/// choice for anything counting particles and is the wrong one here. Direct
/// summation over a million particles is 10^12 interactions in a single
/// evaluation, and a run makes millions of evaluations. That overflows 32 bits
/// immediately, and `Index` is only as wide as the platform's `std::size_t`,
/// which is a promise about addressable memory rather than about how much
/// arithmetic a program may do.
struct InteractionCount {
    /// How many times the solver has been asked for accelerations.
    std::uint64_t evaluations{};

    /// How many particle-particle interactions those evaluations computed.
    ///
    /// One interaction is one ordered pair: the contribution of particle j to
    /// the acceleration of particle i. Direct summation computes N(N-1) of them
    /// per evaluation rather than N(N-1)/2, because it evaluates each pair from
    /// both ends rather than applying Newton's third law. ADR-0015 records why,
    /// and this is the number that reports the choice honestly instead of
    /// quoting the pair count and leaving the factor of two implicit.
    ///
    /// Named for the kind of interaction rather than simply `interactions`
    /// because the hierarchical solver of Phase 8 spends most of its time on a
    /// second kind, the interaction between a particle and a distant cell
    /// treated as one mass. Comparing the two algorithms means keeping those
    /// separate, so the name that will have to be distinguished later is
    /// distinguished now.
    std::uint64_t particle_particle{};

    /// How many of those evaluations were between a particle and a cell of a
    /// tree treated as a single mass.
    ///
    /// Zero for direct summation, which is the point of the separation: the sum
    /// of the two counters is what a tree solver actually computed, and the
    /// ratio between them is what it bought. A Barnes-Hut evaluation over a
    /// million particles performs a few hundred of these per particle where
    /// direct summation performs a million particle-particle interactions, and
    /// quoting one total would hide that behind an average.
    ///
    /// The two are not interchangeable as costs, either, which is the second
    /// reason to keep them apart. A particle-particle interaction is one
    /// iteration of a vectorised loop over contiguous memory. A particle-cell
    /// interaction is preceded by a walk down a tree, is not vectorised, and
    /// costs several times as much arithmetic where quadrupole moments are in
    /// use. `docs/performance/barnes_hut.md` measures the ratio rather than
    /// assuming it.
    std::uint64_t particle_cell{};
};

} // namespace orrery::solvers
