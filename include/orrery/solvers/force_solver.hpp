#pragma once

/// \file
/// What every gravitational force solver in this project provides.
///
/// An integrator asks one question of gravity, and `AccelerationField` in the
/// layer below is exactly that question: given these masses at these positions,
/// what is the acceleration of each. Nothing an integrator does depends on how
/// the answer was reached, which is why that interface is deliberately as narrow
/// as it is.
///
/// This interface is the same question asked by everything else. A benchmark
/// needs to know which solver it is timing and how much work that solver did. A
/// validation test comparing an approximation against direct summation needs both
/// to be softening the same way, or the difference it measures is the softening
/// rather than the approximation. A diagnostic needs the softening the kernel
/// used, for the reason ADR-0008 gives. None of that belongs on the interface the
/// integrators see, and all of it is common to every solver that will ever be
/// written here, so it lives one layer up. ADR-0014 records the alternatives.
///
/// A solver is an acceleration field, so it can be handed to an integrator
/// directly. The relationship is inheritance rather than a wrapper because it is
/// a true is-a: computing accelerations is not something a solver does as well as
/// its real job, it is the job.

#include <string_view>

#include "orrery/core/softening.hpp"
#include "orrery/integrators/acceleration_field.hpp"
#include "orrery/solvers/interaction_count.hpp"

namespace orrery::solvers {

/// A method for computing the gravitational acceleration of a set of particles.
///
/// Implementations differ in the algorithm, in the hardware they run on and in
/// the accuracy they promise, and are selected at run time from a command-line
/// flag. The virtual call is made once per force evaluation, ahead of at least
/// N log N and usually N^2 interactions, which is the boundary section 3 of the
/// implementation plan permits dispatch at.
class ForceSolver : public integrators::AccelerationField {
public:
    /// Public and virtual, the second by inheritance from `AccelerationField`.
    ///
    /// Stated here rather than left implicit because the copy and move
    /// operations below are declared, and a class that declares some of the
    /// special members and not the rest is the shape that hides a mistake.
    /// Public rather than protected like the others: a caller holding a
    /// `std::unique_ptr<ForceSolver>` has to be able to destroy what it points
    /// at, and it is the derived destructor that runs.
    ~ForceSolver() override = default;

    /// The solver's name, for benchmark tables and test messages.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// The softening this solver applies.
    ///
    /// Exposed so that a caller measuring energies asks the solver what it
    /// softened with rather than carrying a second copy of the value alongside
    /// it. Two copies of a number that must agree are how they come to disagree,
    /// and a potential energy computed with a different softening from the force
    /// kernel turns the project's headline conservation result into a
    /// measurement of the mismatch (ADR-0008).
    [[nodiscard]] virtual core::Softening softening() const noexcept = 0;

    /// The work done since construction or since the last reset.
    ///
    /// Cheap: solvers accumulate this as they go and this call only reads it.
    [[nodiscard]] virtual InteractionCount interaction_count() const noexcept = 0;

    /// Set every counter back to zero.
    ///
    /// A benchmark counts the work of the measured region rather than of the
    /// warm-up that preceded it, and the two are otherwise indistinguishable
    /// once they have been added together.
    virtual void reset_interaction_count() noexcept = 0;

protected:
    // As on `AccelerationField`, and for the same reasons: a class with a
    // virtual destructor suppresses the implicit copy and move operations, and
    // copying an implementation through a reference to this base would slice it.
    ForceSolver() = default;
    ForceSolver(const ForceSolver&) = default;
    ForceSolver(ForceSolver&&) = default;
    ForceSolver& operator=(const ForceSolver&) = default;
    ForceSolver& operator=(ForceSolver&&) = default;
};

} // namespace orrery::solvers
