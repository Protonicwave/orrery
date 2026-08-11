#include "orrery/backend/sycl_usm.hpp"

#ifdef ORRERY_ENABLE_SYCL

#    include <sycl/sycl.hpp>

namespace orrery::backend {

UsmKind allocation_kind(const void* pointer, const sycl::queue& queue) noexcept {
    if (pointer == nullptr) {
        return UsmKind::kUnknown;
    }

    // The runtime's own answer about a pointer it may or may not have made.
    // `get_pointer_type` is the query rather than an inference from which
    // allocator was called, which is what makes it usable as evidence: a test
    // asserting kShared here would fail if the allocation had quietly been made
    // some other way, and a paragraph claiming zero copy would not.
    switch (sycl::get_pointer_type(pointer, queue.get_context())) {
    case sycl::usm::alloc::host:
        return UsmKind::kHost;
    case sycl::usm::alloc::device:
        return UsmKind::kDevice;
    case sycl::usm::alloc::shared:
        return UsmKind::kShared;
    case sycl::usm::alloc::unknown:
        break;
    }
    return UsmKind::kUnknown;
}

} // namespace orrery::backend

#endif // ORRERY_ENABLE_SYCL
