#include "orrery/core/build_info.hpp"

#include <string_view>

namespace orrery::core {

std::string_view version() noexcept {
    // Injected by the build system so that the version has one home, the
    // project() call in the top-level CMakeLists.txt, rather than two that can
    // drift apart.
    return ORRERY_VERSION_STRING;
}

bool uses_single_precision() noexcept {
#ifdef ORRERY_SINGLE_PRECISION
    return true;
#else
    return false;
#endif
}

} // namespace orrery::core
