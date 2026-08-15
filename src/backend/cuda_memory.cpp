#include "orrery/backend/cuda_memory.hpp"

#ifdef ORRERY_ENABLE_CUDA

#    include <cuda_runtime.h>

namespace orrery::backend {

CudaPointerKind pointer_kind(const void* pointer) noexcept {
    if (pointer == nullptr) {
        return CudaPointerKind::kUnknown;
    }

    cudaPointerAttributes attributes{};
    const cudaError_t status = cudaPointerGetAttributes(&attributes, pointer);

    if (status != cudaSuccess) {
        // An unrecognised pointer is not a failure of this query, it is the
        // answer to it, and older runtimes report it as `cudaErrorInvalidValue`
        // rather than by filling in `cudaMemoryTypeUnregistered`. The error is
        // consumed here so that the next unrelated runtime call does not report
        // it as its own: CUDA keeps the last error until somebody asks, and a
        // capability query that left one behind would make an entirely healthy
        // launch look like it had failed.
        static_cast<void>(cudaGetLastError());
        return CudaPointerKind::kUnknown;
    }

    switch (attributes.type) {
    case cudaMemoryTypeDevice:
        return CudaPointerKind::kDevice;
    case cudaMemoryTypeManaged:
        return CudaPointerKind::kManaged;
    case cudaMemoryTypeHost:
        // The runtime knows about this host pointer, which it only does for
        // memory it pinned or registered itself. Ordinary host memory takes the
        // unregistered branch below.
        return CudaPointerKind::kPinnedHost;
    case cudaMemoryTypeUnregistered:
        break;
    }

    return CudaPointerKind::kUnknown;
}

} // namespace orrery::backend

#endif // ORRERY_ENABLE_CUDA
