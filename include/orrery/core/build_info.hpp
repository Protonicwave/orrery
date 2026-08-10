#pragma once

/// \file
/// What the running binary was built from and built for.
///
/// Two facts about a build have to be recoverable from the binary itself
/// rather than from the command that produced it. The first is the version,
/// because a result filed against the wrong revision is worse than an
/// unlabelled one. The second is the scalar precision, because from Phase 2
/// onwards a single-precision build and a double-precision build of the same
/// source disagree about accuracy and about how much memory bandwidth every
/// kernel consumes, and Phase 7 reports measurements against the build that
/// produced them.
///
/// These are functions rather than compile-time constants deliberately. A
/// header-only answer would be baked into each translation unit that asks,
/// which is exactly the arrangement that lets a library and its consumer
/// disagree without anyone noticing.

#include <string_view>

namespace orrery::core {

/// The project version, as declared in the top-level CMakeLists.txt.
[[nodiscard]] std::string_view version() noexcept;

/// True when the build was configured with ORRERY_SINGLE_PRECISION, and the
/// scalar type is therefore float rather than double.
[[nodiscard]] bool uses_single_precision() noexcept;

} // namespace orrery::core
