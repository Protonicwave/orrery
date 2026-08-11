#include "orrery/solvers/sycl_tree_solver.hpp"

#ifdef ORRERY_ENABLE_SYCL

#    include <algorithm>
#    include <cstddef>
#    include <cstdint>
#    include <limits>
#    include <memory>
#    include <optional>
#    include <span>
#    include <stdexcept>
#    include <utility>
#    include <vector>

#    include <sycl/sycl.hpp>

#    include "orrery/backend/executor.hpp"
#    include "orrery/backend/sycl_device.hpp"
#    include "orrery/backend/sycl_usm.hpp"
#    include "orrery/backend/worker_statistics.hpp"
#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/units.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/core/vec3_span.hpp"
#    include "orrery/solvers/morton.hpp"
#    include "orrery/solvers/octree.hpp"
#    include "orrery/solvers/tree_walk.hpp"

namespace orrery::solvers {

using core::Index;
using core::Real;
using core::Vec3;
using core::Vec3Span;

namespace {

using backend::Clock;

/// One cell of the tree, in the shape the device wants it.
///
/// This is the GPU-suitable tree representation the phase asks for, and most of
/// the work of making the tree suitable was done in Phase 8 rather than here.
/// `solvers/octree.hpp` already stores the nodes in one array, in depth-first
/// order, with no pointers and with an escape index instead of a stack, because
/// that is what makes a traversal a loop over a single integer. Every one of
/// those properties is worth more on a device than on a host: a stack per
/// work-item would be private memory, which on this hardware spills to global
/// memory and is the usual reason a first GPU tree walk is slow, and a pointer
/// per child would be a dependent load whose address no lane could predict.
///
/// So the conversion below is narrowing rather than restructuring, and it does
/// two things.
///
/// The indices become 32-bit. `core::Index` is `std::size_t` for the reason
/// `core/types.hpp` gives, which is a statement about the host's containers and
/// buys nothing here: this configuration would need four billion particles
/// before a 32-bit node index could overflow, and four billion particles is
/// 144 GB of component arrays on a machine with 32. Halving the three index
/// fields takes a node in a single-precision build from 48 bytes to 32, which is
/// exactly half a cache line, so a sub-group reading one node reads one aligned
/// 32-byte block and two nodes share a line.
///
/// And the moments are separated from the geometry only where Phase 8 already
/// separated them. The quadrupoles stay in an array of their own, for the reason
/// `octree.hpp` gives and which applies with more force here: they are read only
/// when they are switched on, and carrying them inside the node would double
/// what every walk pulls through the cache in the configuration that does not
/// use them.
struct DeviceNode {
    core::Vec3 centre_of_mass;
    Real mass{};
    Real acceptance_radius_squared{};

    std::uint32_t next{};
    std::uint32_t first_particle{};
    std::uint32_t particle_count{};
};

/// Everything a traversal reads and writes, in one trivially copyable bundle.
///
/// Raw pointers, which is the one place section 4 of the implementation plan
/// permits them over spans, and this is that place: a captured `std::span`
/// carries a length no kernel here reads, and the capture has to be trivially
/// copyable to cross to the device at all. Bundled rather than passed one by one
/// because there are fourteen of them and two kernels take the same fourteen.
struct WalkArguments {
    const DeviceNode* nodes{};

    /// Null when the moments are switched off, which is the whole of the test
    /// the kernel makes. The branch is uniform across every work-item in the
    /// launch, so it predicts perfectly and costs one register.
    const Quadrupole* quadrupoles{};

    const Real* position_x{};
    const Real* position_y{};
    const Real* position_z{};
    const Real* mass{};

    Real* acceleration_x{};
    Real* acceleration_y{};
    Real* acceleration_z{};

    /// Per target, reduced on the host. A counter shared between work-items
    /// would be an atomic written billions of times per evaluation and the most
    /// contended location on the device, which is the same argument
    /// `WalkCounts` makes for the CPU walk. Thirty-two bits because a single
    /// target cannot interact with more particles than exist, and the
    /// representation above has already established that there are fewer than
    /// four billion of those.
    std::uint32_t* pair_counts{};
    std::uint32_t* cell_counts{};
    std::uint32_t* visit_counts{};

    std::uint32_t node_count{};
    std::uint32_t count{};

    core::Softening softening;
};

/// The pairs of one leaf, summed into `acceleration`, with the target skipped.
///
/// The CPU walk skips the target by summing the two ranges either side of its
/// index, which costs nothing in a loop over a range the caller chose. Here the
/// term is masked instead, exactly as the Phase 9 direct kernel masks it and for
/// a related reason: on a device the two-range form is two loops with different
/// trip counts, which is a second source of divergence inside the one place the
/// lanes of a sub-group were doing identical work.
///
/// Both the mass and the squared separation are selected, never the mass alone.
/// The reciprocal distance of a zero separation is infinite in an unsoftened run
/// and `inf * 0` is NaN rather than the zero contribution intended, which is the
/// defect `tests/solvers/sycl_direct_solver_test.cpp` records as a regression
/// case for the direct kernel and which this loop would otherwise reproduce.
///
/// The summation order is the order of the leaf's particles, which is the order
/// the CPU walk sums them in, with an exact zero inserted where the target sits.
/// That is what lets the two walks be compared as the same function rather than
/// as two approximations of one.
void accumulate_leaf(const WalkArguments& arguments, Index target, Vec3 position, Index begin,
                     Index end, Vec3& acceleration) {
    for (Index j = begin; j < end; ++j) {
        const Real dx = arguments.position_x[j] - position.x;
        const Real dy = arguments.position_y[j] - position.y;
        const Real dz = arguments.position_z[j] - position.z;

        const bool self = j == target;
        const Real separation_squared = self ? Real{1} : ((dx * dx) + (dy * dy) + (dz * dz));
        const Real source_mass = self ? Real{0} : arguments.mass[j];

        const Real factor = source_mass * core::softened_inverse_distance_cubed(
                                              separation_squared, arguments.softening);

        acceleration.x += dx * factor;
        acceleration.y += dy * factor;
        acceleration.z += dz * factor;
    }
}

/// What one lane accumulates as it walks, kept together so that the two
/// traversals share the shape of their bookkeeping as well as their physics.
struct LaneState {
    Vec3 acceleration{};
    std::uint32_t pairs{};
    std::uint32_t cells{};
    std::uint32_t visits{};
};

/// What this lane does at `node`, and where its own walk would go next.
///
/// The whole of Barnes-Hut, for one target at one cell, factored out of both
/// loops below. The two traversals differ in how the node index advances and in
/// nothing else, which is the claim this function exists to make structural
/// rather than verbal: if the acceptance test or the leaf summation differed
/// between them, comparing their timings would be comparing two algorithms.
[[nodiscard]] std::uint32_t visit_node(const WalkArguments& arguments, Index target, Vec3 position,
                                       std::uint32_t node, LaneState& state) {
    const DeviceNode cell = arguments.nodes[node];
    const Vec3 offset = cell.centre_of_mass - position;

    if (core::squared_norm(offset) >= cell.acceptance_radius_squared) {
        // The same two functions the CPU walk calls, compiled for the device.
        // That is the property single-source buys and the reason ADR-0025 weighs
        // it above the alternatives: there is one multipole expansion in this
        // project rather than one per backend.
        state.acceleration += monopole_acceleration(offset, cell.mass, arguments.softening);

        if (arguments.quadrupoles != nullptr) {
            state.acceleration +=
                quadrupole_acceleration(offset, arguments.quadrupoles[node], arguments.softening);
        }

        ++state.cells;
        return cell.next;
    }

    if (cell.particle_count > 0) {
        const Index begin = cell.first_particle;
        const Index end = begin + cell.particle_count;

        accumulate_leaf(arguments, target, position, begin, end, state.acceleration);

        // The target's own leaf contributes one pair fewer, since a particle
        // does not attract itself. Counted the way the CPU walk counts it so
        // that the two totals can be required to be equal.
        const bool holds_target = target >= begin && target < end;
        state.pairs += cell.particle_count - (holds_target ? 1U : 0U);

        return cell.next;
    }

    // Not far enough away and not a leaf, so the answer is somewhere below and
    // the first child is the next node in the array.
    return node + 1;
}

void write_result(const WalkArguments& arguments, Index target, const LaneState& state) {
    // G is one in this project's units (ADR-0007), written for the reason
    // `direct_solver.cpp` gives rather than for its value.
    arguments.acceleration_x[target] = core::kGravitationalConstant * state.acceleration.x;
    arguments.acceleration_y[target] = core::kGravitationalConstant * state.acceleration.y;
    arguments.acceleration_z[target] = core::kGravitationalConstant * state.acceleration.z;

    arguments.pair_counts[target] = state.pairs;
    arguments.cell_counts[target] = state.cells;
    arguments.visit_counts[target] = state.visits;
}

/// One work-item per target, each following its own node index.
///
/// The port a first attempt writes, and the baseline the coherent walk is
/// measured against. Nothing here is wrong: it computes the right answer, it
/// keeps no stack, and every branch it takes is the branch the CPU walk takes.
/// What it does not do is acknowledge that its neighbours in the sub-group are
/// executing the same instruction stream, so the time it spends is the time
/// their walks take put together rather than the time its own does.
void independent_walk(sycl::nd_item<1> item, const WalkArguments& arguments) {
    const Index target = item.get_global_id(0);
    if (target >= arguments.count) {
        // The launch is padded to whole work-groups. There are no collectives in
        // this traversal, so a padded work-item has nothing to take part in and
        // leaves.
        return;
    }

    const Vec3 position{arguments.position_x[target], arguments.position_y[target],
                        arguments.position_z[target]};

    LaneState state;
    std::uint32_t node = 0;

    while (node < arguments.node_count) {
        ++state.visits;
        node = visit_node(arguments, target, position, node, state);
    }

    write_result(arguments, target, state);
}

/// One node index per sub-group, advanced to the nearest node any lane needs.
///
/// The mitigation. Every lane of the sub-group is at the same node at the same
/// time, so the node is read once for the group rather than once per lane, the
/// acceptance test is one instruction over 32 targets, and the loop has a single
/// instruction stream instead of 32 interleaved ones.
///
/// A lane that has accepted a cell must not then see the cell's contents, so it
/// records the index its own walk would have jumped to and contributes nothing
/// until the group reaches it. That is the entire mechanism by which this
/// traversal computes the same sum as the independent one rather than a more
/// accurate one, and ADR-0029 records why computing the more accurate sum, which
/// is what the published warp-coherent traversals do, was not acceptable here.
///
/// The group advances to the smallest index any lane still wants, which makes
/// the sequence of visited nodes exactly the union of the lanes' own walks.
/// A vote on whether to descend would be one cheap instruction rather than a
/// reduction, and would sometimes step through a subtree that every lane had
/// already accepted; the minimum is taken instead because the union is the
/// figure `node_visits` is supposed to report.
void coherent_walk(sycl::nd_item<1> item, const WalkArguments& arguments) {
    const sycl::sub_group lanes = item.get_sub_group();

    const Index target = item.get_global_id(0);
    const bool active = target < arguments.count;

    // Read unconditionally. The arrays are allocated to the padded launch and
    // the tail was zeroed at staging time, so a padded lane reads inside the
    // allocation and its position is never used for anything.
    const Vec3 position{arguments.position_x[target], arguments.position_y[target],
                        arguments.position_z[target]};

    LaneState state;

    std::uint32_t node = 0;

    /// The index this lane's own walk jumped to when it accepted a cell. While
    /// the group is below it, this lane is inside a subtree it has already
    /// summed as a single mass and takes no part.
    std::uint32_t skip_until = 0;

    while (node < arguments.node_count) {
        ++state.visits;

        // Where this lane's own walk would go next. A lane that is skipping
        // wants the end of the subtree it accepted; an inactive lane wants the
        // end of the tree and so never holds the group back.
        std::uint32_t lane_next = arguments.node_count;

        if (active && node >= skip_until) {
            lane_next = visit_node(arguments, target, position, node, state);

            // An accepted cell is the only case that jumps past a subtree, and
            // it is the only case where this lane has to stop looking. A leaf
            // and a descent both move to a node this lane still wants to see.
            if (lane_next > node + 1) {
                skip_until = lane_next;
            }
        } else if (active) {
            lane_next = skip_until;
        }

        node = sycl::reduce_over_group(lanes, lane_next, sycl::minimum<std::uint32_t>());
    }

    if (active) {
        write_result(arguments, target, state);
    }
}

/// The traversal, submitted at whatever sub-group width the compiler chooses.
void submit_independent(sycl::queue& queue, const WalkArguments& arguments, Index padded,
                        Index group) {
    queue.submit([&](sycl::handler& handler) {
        handler.parallel_for(sycl::nd_range<1>{sycl::range<1>{padded}, sycl::range<1>{group}},
                             [=](sycl::nd_item<1> item) { independent_walk(item, arguments); });
    });
}

void submit_coherent(sycl::queue& queue, const WalkArguments& arguments, Index padded,
                     Index group) {
    queue.submit([&](sycl::handler& handler) {
        handler.parallel_for(sycl::nd_range<1>{sycl::range<1>{padded}, sycl::range<1>{group}},
                             [=](sycl::nd_item<1> item) { coherent_walk(item, arguments); });
    });
}

/// The traversal at a sub-group width named at compile time.
///
/// The width is the granularity of the coherence, so it is the knob that decides
/// how much divergence the scheme removes and how much redundant walking it pays
/// for. It cannot be a run-time argument: `reqd_sub_group_size` is an attribute
/// on the kernel, so each width is a separate compilation, which is why the
/// dispatch below is a switch over the two or three the device offers rather
/// than a loop over an arbitrary number.
template<int Width>
void submit_coherent_at(sycl::queue& queue, const WalkArguments& arguments, Index padded,
                        Index group) {
    queue.submit([&](sycl::handler& handler) {
        handler.parallel_for(sycl::nd_range<1>{sycl::range<1>{padded}, sycl::range<1>{group}},
                             [=](sycl::nd_item<1> item) [[sycl::reqd_sub_group_size(Width)]] {
                                 coherent_walk(item, arguments);
                             });
    });
}

/// The work-group size the traversal launches with.
///
/// Not the tile size of the Phase 9 direct kernel, and the difference is worth
/// stating because the two look alike. There the work-group size was also the
/// number of sources staged through local memory, so it traded reuse against
/// occupancy. Here the traversal uses no local memory at all: the coherence is
/// between the lanes of a sub-group, which cooperate through registers, and the
/// work-group is merely how many sub-groups are dispatched together.
///
/// So this number is chosen to be a comfortable multiple of every sub-group
/// width the device offers, and capped where occupancy stops improving.
///
/// A power of two, so that it is a whole number of sub-groups at every width the
/// hardware offers and no group ends with a partly populated one.
[[nodiscard]] Index choose_work_group_size(const backend::DeviceDescription& device) noexcept {
    constexpr Index kCeiling = 256;

    const Index limit = std::min<Index>(device.max_work_group_size, kCeiling);

    Index power = 1;
    while (power * 2 <= limit) {
        power *= 2;
    }

    return power;
}

/// The sub-group widths this device will compile a kernel for.
///
/// Filtered to the ones `submit_coherent_at` has an instantiation for. A device
/// offering a width outside that set is served by the compiler's own choice,
/// which is what the default is for.
[[nodiscard]] std::vector<unsigned> discover_widths(const sycl::device& device) {
    std::vector<unsigned> widths;

    try {
        for (const std::size_t width : device.get_info<sycl::info::device::sub_group_sizes>()) {
            if (width == 8 || width == 16 || width == 32) {
                widths.push_back(static_cast<unsigned>(width));
            }
        }
    } catch (const sycl::exception&) {
        // A runtime that cannot answer leaves the list empty, which means the
        // width cannot be selected and the compiler's choice is the only one.
        // That is a degraded configuration rather than a broken one.
        widths.clear();
    }

    std::sort(widths.begin(), widths.end());
    return widths;
}

} // namespace

/// Everything that requires a SYCL header, kept out of the public interface.
struct SyclTreeSolver::Impl {
    Impl(const sycl::device& device, backend::DeviceDescription description,
         TreeParameters parameters, core::Softening softening, backend::Executor* executor)
        : queue(device, sycl::property::queue::in_order{}),
          description(std::move(description)),
          parameters(corrected_parameters(parameters)),
          softening(softening),
          executor(executor),
          widths(discover_widths(device)),
          group(choose_work_group_size(this->description)) {}

    /// In-order for the reason `SyclDirectSolver` gives: there is one kernel per
    /// evaluation and nothing to overlap it with.
    sycl::queue queue;

    backend::DeviceDescription description;
    TreeParameters parameters;
    core::Softening softening;

    /// Not owned, and null by default, exactly as on `BarnesHutSolver`.
    backend::Executor* executor{nullptr};

    std::vector<unsigned> widths;
    Index group;

    TreeTraversal traversal{TreeTraversal::kCoherent};
    unsigned requested_width{0};

    MortonOrdering ordering;
    Octree tree;

    InteractionCount count;
    std::uint64_t visits{};
    SyclTreeTimings timings;

    Index capacity{0};
    Index node_capacity{0};
    Index quadrupole_capacity{0};

    backend::UsmArray<Real> position_x;
    backend::UsmArray<Real> position_y;
    backend::UsmArray<Real> position_z;
    backend::UsmArray<Real> mass;
    backend::UsmArray<Real> acceleration_x;
    backend::UsmArray<Real> acceleration_y;
    backend::UsmArray<Real> acceleration_z;

    backend::UsmArray<DeviceNode> nodes;
    backend::UsmArray<Quadrupole> quadrupoles;

    backend::UsmArray<std::uint32_t> pair_counts;
    backend::UsmArray<std::uint32_t> cell_counts;
    backend::UsmArray<std::uint32_t> visit_counts;

    [[nodiscard]] Index padded_count(Index needed) const noexcept {
        return ((needed + group - 1) / group) * group;
    }

    void ensure_particle_capacity(Index needed) {
        if (needed <= capacity) {
            return;
        }

        // Sized to the padded launch rather than to the particle count, so that
        // the tail work-items of the final group read inside the allocation
        // instead of being masked at every access, which is the arrangement
        // ADR-0027 and the Phase 9 solver already established.
        const Index padded = padded_count(needed);

        position_x = backend::UsmArray<Real>{queue, padded};
        position_y = backend::UsmArray<Real>{queue, padded};
        position_z = backend::UsmArray<Real>{queue, padded};
        mass = backend::UsmArray<Real>{queue, padded};
        acceleration_x = backend::UsmArray<Real>{queue, padded};
        acceleration_y = backend::UsmArray<Real>{queue, padded};
        acceleration_z = backend::UsmArray<Real>{queue, padded};

        pair_counts = backend::UsmArray<std::uint32_t>{queue, padded};
        cell_counts = backend::UsmArray<std::uint32_t>{queue, padded};
        visit_counts = backend::UsmArray<std::uint32_t>{queue, padded};

        capacity = needed;
    }

    void ensure_node_capacity(Index needed, bool with_quadrupoles) {
        // Grown with headroom rather than to the exact figure, because the node
        // count changes by a handful between one timestep and the next as
        // particles cross cell boundaries, and an allocation that tracked it
        // exactly would make a driver call on most steps of a simulation to
        // recover a few kilobytes.
        if (needed > node_capacity) {
            const Index generous = needed + (needed / 4) + 1;
            nodes = backend::UsmArray<DeviceNode>{queue, generous};
            node_capacity = generous;
        }

        if (with_quadrupoles && needed > quadrupole_capacity) {
            const Index generous = needed + (needed / 4) + 1;
            quadrupoles = backend::UsmArray<Quadrupole>{queue, generous};
            quadrupole_capacity = generous;
        }
    }
};

std::unique_ptr<SyclTreeSolver> SyclTreeSolver::try_create(TreeParameters parameters,
                                                           core::Softening softening,
                                                           backend::Executor* executor) {
    const std::optional<backend::DeviceDescription> description = backend::discover_gpu_device();
    if (!description.has_value()) {
        return nullptr;
    }

    if (!description->supports_build_precision() || !description->supports_shared_usm) {
        return nullptr;
    }

    try {
        const std::vector<sycl::device> devices =
            sycl::device::get_devices(sycl::info::device_type::gpu);
        if (devices.empty()) {
            return nullptr;
        }

        auto impl =
            std::make_unique<Impl>(devices.front(), *description, parameters, softening, executor);
        return std::unique_ptr<SyclTreeSolver>{new SyclTreeSolver{std::move(impl)}};
    } catch (const sycl::exception&) {
        // Queue creation is where a driver that enumerated a device but cannot
        // use it fails. Reported as no device, for the reason
        // `backend/sycl_device.hpp` gives.
        return nullptr;
    }
}

SyclTreeSolver::SyclTreeSolver(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

SyclTreeSolver::~SyclTreeSolver() = default;
SyclTreeSolver::SyclTreeSolver(SyclTreeSolver&&) noexcept = default;
SyclTreeSolver& SyclTreeSolver::operator=(SyclTreeSolver&&) noexcept = default;

core::Softening SyclTreeSolver::softening() const noexcept {
    return impl_->softening;
}

InteractionCount SyclTreeSolver::interaction_count() const noexcept {
    return impl_->count;
}

void SyclTreeSolver::reset_interaction_count() noexcept {
    impl_->count = {};
    impl_->visits = 0;
}

const TreeParameters& SyclTreeSolver::parameters() const noexcept {
    return impl_->parameters;
}

const Octree& SyclTreeSolver::tree() const noexcept {
    return impl_->tree;
}

TreeTraversal SyclTreeSolver::traversal() const noexcept {
    return impl_->traversal;
}

void SyclTreeSolver::select_traversal(TreeTraversal traversal) noexcept {
    impl_->traversal = traversal;
}

void SyclTreeSolver::select_sub_group_width(unsigned width) noexcept {
    // A request the device cannot meet becomes the compiler's own choice rather
    // than a submission that throws, on the same terms as
    // `DirectSolver::select_kernel`: the caller asked for a configuration, and
    // the solver reports what it settled on.
    //
    // Two ways it can fail. The hardware may not implement the width at all, and
    // the work-group may be too small to contain one, which cannot happen on any
    // device this project has met but would be a submission error rather than a
    // slower kernel if it did.
    const bool implemented =
        std::find(impl_->widths.begin(), impl_->widths.end(), width) != impl_->widths.end();

    impl_->requested_width = implemented && width <= impl_->group ? width : 0;
}

unsigned SyclTreeSolver::sub_group_width() const noexcept {
    return impl_->requested_width;
}

std::span<const unsigned> SyclTreeSolver::supported_sub_group_widths() const noexcept {
    return impl_->widths;
}

Index SyclTreeSolver::work_group_size() const noexcept {
    return impl_->group;
}

std::uint64_t SyclTreeSolver::node_visits() const noexcept {
    return impl_->visits;
}

const backend::DeviceDescription& SyclTreeSolver::device() const noexcept {
    return impl_->description;
}

const SyclTreeTimings& SyclTreeSolver::timings() const noexcept {
    return impl_->timings;
}

bool SyclTreeSolver::uses_shared_memory() const noexcept {
    if (impl_->position_x.empty() || impl_->nodes.empty()) {
        return false;
    }

    return backend::allocation_kind(impl_->position_x.data(), impl_->queue) ==
               backend::UsmKind::kShared &&
           backend::allocation_kind(impl_->nodes.data(), impl_->queue) == backend::UsmKind::kShared;
}

void SyclTreeSolver::evaluate(Vec3Span<const Real> positions, std::span<const Real> masses,
                              Vec3Span<Real> accelerations) {
    const Index count = positions.size();

    if (count == 0) {
        ++impl_->count.evaluations;
        impl_->timings = {};
        return;
    }

    // The device representation indexes nodes and particles with 32 bits, for
    // the reason `DeviceNode` sets out. The bound is unreachable on this machine
    // by three orders of magnitude, and is checked rather than assumed because
    // the failure it would otherwise produce is silently wrong accelerations
    // rather than a diagnostic. Thrown here, which is the evaluation boundary
    // and not a kernel or a per-particle loop.
    constexpr Index kLargestIndex = std::numeric_limits<std::uint32_t>::max();
    if (count > kLargestIndex) {
        throw std::length_error{"the SYCL tree solver indexes particles with 32 bits"};
    }

    Clock::time_point mark = Clock::now();

    const auto since = [&mark]() noexcept {
        const Clock::time_point now = Clock::now();
        const backend::Duration elapsed = now - mark;
        mark = now;
        return elapsed;
    };

    impl_->ordering.build(positions, impl_->executor);
    impl_->timings.ordering = since();

    impl_->ensure_particle_capacity(count);

    const std::span<const MortonKey> keys = impl_->ordering.keys();

    Real* const sorted_x = impl_->position_x.data();
    Real* const sorted_y = impl_->position_y.data();
    Real* const sorted_z = impl_->position_z.data();
    Real* const sorted_mass = impl_->mass.data();

    // The gather the tree needs, written straight into shared memory. This is
    // the step where Phase 8's reordering and Phase 9's staging turn out to be
    // the same step, so the GPU tree solver pays for the second of them nothing
    // at all beyond what the CPU tree solver already pays for the first.
    for (Index i = 0; i < count; ++i) {
        const Index source = keys[i].index;
        sorted_x[i] = positions.x[source];
        sorted_y[i] = positions.y[source];
        sorted_z[i] = positions.z[source];
        sorted_mass[i] = masses[source];
    }

    // The padded tail is zeroed rather than left as it was. The traversal masks
    // it out of the physics by index, so these values never reach a sum, but the
    // coherent walk reads a padded lane's position unconditionally and reading
    // uninitialised memory is undefined however little the result is used.
    const Index padded = impl_->padded_count(count);
    std::fill(sorted_x + count, sorted_x + padded, Real{0});
    std::fill(sorted_y + count, sorted_y + padded, Real{0});
    std::fill(sorted_z + count, sorted_z + padded, Real{0});
    std::fill(sorted_mass + count, sorted_mass + padded, Real{0});

    impl_->timings.gathering = since();

    const Vec3Span<const Real> sorted{std::span<const Real>{sorted_x, count},
                                      std::span<const Real>{sorted_y, count},
                                      std::span<const Real>{sorted_z, count}};

    impl_->tree.build(sorted, std::span<const Real>{sorted_mass, count}, keys,
                      impl_->ordering.cube(), impl_->parameters, impl_->executor);
    impl_->timings.construction = since();

    const std::span<const TreeNode> host_nodes = impl_->tree.nodes();
    const std::span<const Quadrupole> host_quadrupoles = impl_->tree.quadrupoles();

    if (host_nodes.size() > kLargestIndex) {
        throw std::length_error{"the SYCL tree solver indexes tree nodes with 32 bits"};
    }

    impl_->ensure_node_capacity(host_nodes.size(), !host_quadrupoles.empty());

    DeviceNode* const device_nodes = impl_->nodes.data();
    for (Index i = 0; i < host_nodes.size(); ++i) {
        const TreeNode& node = host_nodes[i];
        device_nodes[i] =
            DeviceNode{.centre_of_mass = node.centre_of_mass,
                       .mass = node.mass,
                       .acceptance_radius_squared = node.acceptance_radius_squared,
                       .next = static_cast<std::uint32_t>(node.next),
                       .first_particle = static_cast<std::uint32_t>(node.first_particle),
                       .particle_count = static_cast<std::uint32_t>(node.particle_count)};
    }

    if (!host_quadrupoles.empty()) {
        std::copy(host_quadrupoles.begin(), host_quadrupoles.end(), impl_->quadrupoles.data());
    }

    impl_->timings.node_staging = since();

    const WalkArguments arguments{
        .nodes = device_nodes,
        .quadrupoles = host_quadrupoles.empty() ? nullptr : impl_->quadrupoles.data(),
        .position_x = sorted_x,
        .position_y = sorted_y,
        .position_z = sorted_z,
        .mass = sorted_mass,
        .acceleration_x = impl_->acceleration_x.data(),
        .acceleration_y = impl_->acceleration_y.data(),
        .acceleration_z = impl_->acceleration_z.data(),
        .pair_counts = impl_->pair_counts.data(),
        .cell_counts = impl_->cell_counts.data(),
        .visit_counts = impl_->visit_counts.data(),
        .node_count = static_cast<std::uint32_t>(host_nodes.size()),
        .count = static_cast<std::uint32_t>(count),
        .softening = impl_->softening};

    if (impl_->traversal == TreeTraversal::kIndependent) {
        submit_independent(impl_->queue, arguments, padded, impl_->group);
    } else {
        switch (impl_->requested_width) {
        case 8:
            submit_coherent_at<8>(impl_->queue, arguments, padded, impl_->group);
            break;
        case 16:
            submit_coherent_at<16>(impl_->queue, arguments, padded, impl_->group);
            break;
        case 32:
            submit_coherent_at<32>(impl_->queue, arguments, padded, impl_->group);
            break;
        default:
            submit_coherent(impl_->queue, arguments, padded, impl_->group);
            break;
        }
    }

    // The kernel writes shared memory the host is about to read, so the wait is
    // not merely for timing. Reading a USM allocation while a kernel that writes
    // it is in flight is a data race whatever the memory is shared with.
    impl_->queue.wait_and_throw();
    impl_->timings.kernel = since();

    const Real* const result_x = impl_->acceleration_x.data();
    const Real* const result_y = impl_->acceleration_y.data();
    const Real* const result_z = impl_->acceleration_z.data();

    const std::uint32_t* const pairs = impl_->pair_counts.data();
    const std::uint32_t* const cells = impl_->cell_counts.data();
    const std::uint32_t* const visits = impl_->visit_counts.data();

    std::uint64_t total_pairs = 0;
    std::uint64_t total_cells = 0;
    std::uint64_t total_visits = 0;

    // The scatter back to the caller's order, which is the step the CPU tree
    // solver also performs, with the counter reduction riding along. Three extra
    // sequential reads per particle against three scattered writes, so it is
    // charged to this figure rather than hidden in one of its own.
    for (Index i = 0; i < count; ++i) {
        accelerations.set(keys[i].index, Vec3{result_x[i], result_y[i], result_z[i]});

        total_pairs += pairs[i];
        total_cells += cells[i];
        total_visits += visits[i];
    }

    impl_->timings.scatter = since();

    ++impl_->count.evaluations;
    impl_->count.particle_particle += total_pairs;
    impl_->count.particle_cell += total_cells;
    impl_->visits += total_visits;
}

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_SYCL
