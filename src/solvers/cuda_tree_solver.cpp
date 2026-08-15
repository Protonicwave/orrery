#include "orrery/solvers/cuda_tree_solver.hpp"

#ifdef ORRERY_ENABLE_CUDA

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

#    include <cuda_runtime.h>

#    include "orrery/backend/cuda_device.hpp"
#    include "orrery/backend/cuda_memory.hpp"
#    include "orrery/backend/executor.hpp"
#    include "orrery/backend/worker_statistics.hpp"
#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/core/vec3_span.hpp"
#    include "orrery/solvers/cuda_kernels.hpp"
#    include "orrery/solvers/morton.hpp"
#    include "orrery/solvers/octree.hpp"

namespace orrery::solvers {

using backend::Clock;
using core::Index;
using core::Real;
using core::Vec3;
using core::Vec3Span;

namespace {

/// The block size the traversal launches with.
///
/// Not the tile size of the direct kernel, and the difference is worth stating
/// because the two look alike. There the block size was also the number of
/// sources staged through shared memory, so it traded reuse against occupancy.
/// Here the traversal uses no shared memory at all: the coherence is between the
/// lanes of a warp, which cooperate through registers, and the block is merely
/// how many warps are dispatched together.
///
/// A power of two, so that it is a whole number of warps and no block ends with
/// a partly populated one, which the shuffle ladder assumes.
[[nodiscard]] Index choose_block_size(const backend::CudaDeviceDescription& device) noexcept {
    constexpr Index kCeiling = 256;

    const Index limit = std::min<Index>(device.max_threads_per_block, kCeiling);

    Index power = 1;
    while (power * 2 <= limit) {
        power *= 2;
    }

    return power;
}

/// The coherence widths this solver has a traversal for.
///
/// Fixed rather than discovered, which is the opposite of what the SYCL solver
/// does and follows from the hardware rather than from a preference. There the
/// device is asked which sub-group sizes it will compile a kernel for, because
/// the answer varies. Here the warp is 32 lanes on every part shipped so far,
/// and the narrower widths are not hardware at all: they are segments of the
/// reduction, so the set is decided by which instantiations exist.
[[nodiscard]] std::vector<unsigned> supported_widths(unsigned warp) {
    std::vector<unsigned> widths{8, 16};

    // The warp width itself, which is 32 on everything this project has met but
    // is read rather than assumed, for the reason `CudaDeviceDescription`
    // gives. A device reporting something else is served by the default
    // instantiation, so the list would be claiming an option that does not
    // exist.
    if (warp == 32) {
        widths.push_back(32);
    }

    return widths;
}

} // namespace

/// Everything that requires the CUDA runtime's header, kept out of the public
/// interface.
struct CudaTreeSolver::Impl {
    Impl(backend::CudaDeviceDescription description, TreeParameters parameters,
         core::Softening softening, backend::Executor* executor)
        : description(std::move(description)),
          parameters(corrected_parameters(parameters)),
          softening(softening),
          executor(executor),
          widths(supported_widths(this->description.warp_size)),
          block(choose_block_size(this->description)),
          width(this->description.warp_size) {}

    backend::CudaDeviceDescription description;
    TreeParameters parameters;
    core::Softening softening;

    /// Not owned, and null by default, exactly as on `BarnesHutSolver`.
    backend::Executor* executor{nullptr};

    std::vector<unsigned> widths;
    Index block;
    unsigned width;

    CudaTraversal traversal{CudaTraversal::kCoherent};

    MortonOrdering ordering;
    Octree tree;

    InteractionCount count;
    std::uint64_t visits{};
    CudaTreeTimings timings;

    Index capacity{0};
    Index node_capacity{0};
    Index quadrupole_capacity{0};

    /// The particles and the results, each in one allocation on each side of the
    /// bus, for the reason `CudaDirectSolver::Impl` gives: a transfer costs a
    /// fixed few microseconds before it has moved a byte, and a tree evaluation
    /// at a size where the GPU is worth using makes several as it is.
    backend::CudaHostArray<Real> staged_sources;
    backend::CudaHostArray<Real> staged_results;
    backend::CudaArray<Real> device_sources;
    backend::CudaArray<Real> device_results;

    /// The three per-target counters, transferred together.
    backend::CudaHostArray<std::uint32_t> staged_counts;
    backend::CudaArray<std::uint32_t> device_counts;

    backend::CudaHostArray<CudaTreeNode> staged_nodes;
    backend::CudaArray<CudaTreeNode> device_nodes;

    backend::CudaHostArray<Quadrupole> staged_quadrupoles;
    backend::CudaArray<Quadrupole> device_quadrupoles;

    [[nodiscard]] Index padded_count(Index needed) const noexcept {
        return ((needed + block - 1) / block) * block;
    }

    void ensure_particle_capacity(Index needed) {
        if (needed <= capacity) {
            return;
        }

        // Sized to the padded launch rather than to the particle count, so that
        // the trailing threads of the final block read inside the allocation
        // instead of being masked at every access.
        const Index padded = padded_count(needed);

        staged_sources = backend::CudaHostArray<Real>{4 * padded};
        staged_results = backend::CudaHostArray<Real>{3 * padded};
        device_sources = backend::CudaArray<Real>{4 * padded};
        device_results = backend::CudaArray<Real>{3 * padded};

        staged_counts = backend::CudaHostArray<std::uint32_t>{3 * padded};
        device_counts = backend::CudaArray<std::uint32_t>{3 * padded};

        capacity = needed;
    }

    void ensure_node_capacity(Index needed, bool with_quadrupoles) {
        // Grown with headroom rather than to the exact figure, because the node
        // count changes by a handful between one timestep and the next as
        // particles cross cell boundaries, and an allocation that tracked it
        // exactly would make a driver call on most steps of a simulation to
        // recover a few kilobytes. On this backend that call is heavier than on
        // the other one: `cudaMalloc` synchronises the device.
        if (needed > node_capacity) {
            const Index generous = needed + (needed / 4) + 1;
            staged_nodes = backend::CudaHostArray<CudaTreeNode>{generous};
            device_nodes = backend::CudaArray<CudaTreeNode>{generous};
            node_capacity = generous;
        }

        if (with_quadrupoles && needed > quadrupole_capacity) {
            const Index generous = needed + (needed / 4) + 1;
            staged_quadrupoles = backend::CudaHostArray<Quadrupole>{generous};
            device_quadrupoles = backend::CudaArray<Quadrupole>{generous};
            quadrupole_capacity = generous;
        }
    }
};

std::unique_ptr<CudaTreeSolver> CudaTreeSolver::try_create(TreeParameters parameters,
                                                           core::Softening softening,
                                                           backend::Executor* executor) {
    const std::optional<backend::CudaDeviceDescription> description =
        backend::discover_cuda_device();
    if (!description.has_value()) {
        return nullptr;
    }

    if (cudaSetDevice(0) != cudaSuccess) {
        static_cast<void>(cudaGetLastError());
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(*description, parameters, softening, executor);
    return std::unique_ptr<CudaTreeSolver>{new CudaTreeSolver{std::move(impl)}};
}

CudaTreeSolver::CudaTreeSolver(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

CudaTreeSolver::~CudaTreeSolver() = default;
CudaTreeSolver::CudaTreeSolver(CudaTreeSolver&&) noexcept = default;
CudaTreeSolver& CudaTreeSolver::operator=(CudaTreeSolver&&) noexcept = default;

core::Softening CudaTreeSolver::softening() const noexcept {
    return impl_->softening;
}

InteractionCount CudaTreeSolver::interaction_count() const noexcept {
    return impl_->count;
}

void CudaTreeSolver::reset_interaction_count() noexcept {
    impl_->count = {};
    impl_->visits = 0;
}

const TreeParameters& CudaTreeSolver::parameters() const noexcept {
    return impl_->parameters;
}

const Octree& CudaTreeSolver::tree() const noexcept {
    return impl_->tree;
}

CudaTraversal CudaTreeSolver::traversal() const noexcept {
    return impl_->traversal;
}

void CudaTreeSolver::select_traversal(CudaTraversal traversal) noexcept {
    impl_->traversal = traversal;
}

void CudaTreeSolver::select_coherence_width(unsigned width) noexcept {
    // A request the solver has no traversal for becomes the warp width rather
    // than a launch that fails, and a request wider than a block becomes it too:
    // a segment cannot span two blocks, so a width above the block size would
    // silently reduce to whatever a warp holds.
    const bool implemented =
        std::find(impl_->widths.begin(), impl_->widths.end(), width) != impl_->widths.end();

    impl_->width = implemented && width <= impl_->block ? width : impl_->description.warp_size;
}

unsigned CudaTreeSolver::coherence_width() const noexcept {
    return impl_->width;
}

std::span<const unsigned> CudaTreeSolver::supported_coherence_widths() const noexcept {
    return impl_->widths;
}

Index CudaTreeSolver::block_size() const noexcept {
    return impl_->block;
}

std::uint64_t CudaTreeSolver::node_visits() const noexcept {
    return impl_->visits;
}

const backend::CudaDeviceDescription& CudaTreeSolver::device() const noexcept {
    return impl_->description;
}

const CudaTreeTimings& CudaTreeSolver::timings() const noexcept {
    return impl_->timings;
}

bool CudaTreeSolver::uses_device_memory() const noexcept {
    if (impl_->device_sources.empty() || impl_->device_nodes.empty() ||
        impl_->staged_sources.empty()) {
        return false;
    }

    return backend::pointer_kind(impl_->device_sources.data()) ==
               backend::CudaPointerKind::kDevice &&
           backend::pointer_kind(impl_->device_nodes.data()) == backend::CudaPointerKind::kDevice &&
           backend::pointer_kind(impl_->staged_sources.data()) ==
               backend::CudaPointerKind::kPinnedHost;
}

void CudaTreeSolver::evaluate(Vec3Span<const Real> positions, std::span<const Real> masses,
                              Vec3Span<Real> accelerations) {
    const Index count = positions.size();

    if (count == 0) {
        ++impl_->count.evaluations;
        impl_->timings = {};
        return;
    }

    // The device representation indexes nodes and particles with 32 bits, for
    // the reason `CudaTreeNode` sets out. The bound is unreachable on any card
    // this backend runs on by three orders of magnitude, and is checked rather
    // than assumed because the failure it would otherwise produce is silently
    // wrong accelerations rather than a diagnostic. Thrown here, which is the
    // evaluation boundary and not a kernel or a per-particle loop.
    constexpr Index kLargestIndex = std::numeric_limits<std::uint32_t>::max();
    if (count > kLargestIndex) {
        throw std::length_error{"the CUDA tree solver indexes particles with 32 bits"};
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
    const Index padded = impl_->padded_count(count);

    Real* const staged = impl_->staged_sources.data();
    Real* const sorted_x = staged;
    Real* const sorted_y = staged + padded;
    Real* const sorted_z = staged + (2 * padded);
    Real* const sorted_mass = staged + (3 * padded);

    // The gather the tree needs, written straight into the staging buffer. This
    // is the step where Phase 8's reordering and this backend's staging turn out
    // to be the same step, which is half of the dividend the integrated part
    // gave in full: the copy the transfer reads from is the copy the algorithm
    // needed anyway, and only the transfer itself is new.
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
        throw std::length_error{"the CUDA tree solver indexes tree nodes with 32 bits"};
    }

    impl_->ensure_node_capacity(host_nodes.size(), !host_quadrupoles.empty());

    CudaTreeNode* const narrowed = impl_->staged_nodes.data();
    for (Index i = 0; i < host_nodes.size(); ++i) {
        const TreeNode& node = host_nodes[i];
        narrowed[i] =
            CudaTreeNode{.centre_of_mass = node.centre_of_mass,
                         .mass = node.mass,
                         .acceptance_radius_squared = node.acceptance_radius_squared,
                         .next = static_cast<std::uint32_t>(node.next),
                         .first_particle = static_cast<std::uint32_t>(node.first_particle),
                         .particle_count = static_cast<std::uint32_t>(node.particle_count)};
    }

    if (!host_quadrupoles.empty()) {
        std::copy(host_quadrupoles.begin(), host_quadrupoles.end(),
                  impl_->staged_quadrupoles.data());
    }

    impl_->timings.node_staging = since();

    impl_->device_sources.copy_from_host(staged, 4 * padded);
    impl_->device_nodes.copy_from_host(narrowed, host_nodes.size());

    if (!host_quadrupoles.empty()) {
        impl_->device_quadrupoles.copy_from_host(impl_->staged_quadrupoles.data(),
                                                 host_quadrupoles.size());
    }

    impl_->timings.transfer = since();

    Real* const results = impl_->device_results.data();
    std::uint32_t* const counts = impl_->device_counts.data();

    const CudaTreeArguments arguments{
        .nodes = impl_->device_nodes.data(),
        .quadrupoles = host_quadrupoles.empty() ? nullptr : impl_->device_quadrupoles.data(),
        .position_x = impl_->device_sources.data(),
        .position_y = impl_->device_sources.data() + padded,
        .position_z = impl_->device_sources.data() + (2 * padded),
        .mass = impl_->device_sources.data() + (3 * padded),
        .acceleration_x = results,
        .acceleration_y = results + padded,
        .acceleration_z = results + (2 * padded),
        .pair_counts = counts,
        .cell_counts = counts + padded,
        .visit_counts = counts + (2 * padded),
        .node_count = static_cast<std::uint32_t>(host_nodes.size()),
        .count = static_cast<std::uint32_t>(count),
        .softening = impl_->softening};

    backend::check_cuda(launch_cuda_tree(arguments, impl_->traversal,
                                         static_cast<unsigned>(impl_->block), impl_->width),
                        "the tree traversal kernel");
    impl_->timings.kernel = since();

    impl_->device_results.copy_to_host(impl_->staged_results.data(), 3 * padded);
    impl_->device_counts.copy_to_host(impl_->staged_counts.data(), 3 * padded);

    const Real* const result_x = impl_->staged_results.data();
    const Real* const result_y = result_x + padded;
    const Real* const result_z = result_x + (2 * padded);

    const std::uint32_t* const pairs = impl_->staged_counts.data();
    const std::uint32_t* const cells = pairs + padded;
    const std::uint32_t* const visits = pairs + (2 * padded);

    std::uint64_t total_pairs = 0;
    std::uint64_t total_cells = 0;
    std::uint64_t total_visits = 0;

    // The scatter back to the caller's order, which is the step the CPU tree
    // solver also performs, with the counter reduction riding along. Charged to
    // this figure together with the return transfer rather than split, because
    // the two are one round trip from the device to the caller's arrays and
    // nothing useful happens between them.
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

#endif // ORRERY_ENABLE_CUDA
