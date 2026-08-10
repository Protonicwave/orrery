#include "orrery/backend/cpu_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// CPUID exists on one architecture and is spelled two ways on it. The macro
// below names that condition once so that the two regions guarded by it cannot
// drift apart, and it is undefined at the end of the file so that nothing else
// in the project inherits it. Section 4 of the implementation plan avoids
// macros in the C++; a preprocessor question can only be answered by the
// preprocessor.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#    define ORRERY_X86
#    ifdef _MSC_VER
#        include <intrin.h>
#    else
#        include <cpuid.h>
#    endif
#endif

namespace orrery::backend {

namespace {

/// The brand string, in storage of its own.
///
/// A fixed buffer rather than a `std::string` because `cpu_brand` is `noexcept`
/// and a string allocates. There is no useful way for a query about the
/// processor to report that it ran out of memory, and the answer is never
/// longer than the forty-eight bytes CPUID has to give, so the allocation buys
/// nothing and costs the guarantee.
struct BrandBuffer {
    /// Exactly what the three extended leaves return, and not a byte more.
    std::array<char, 48> text{};

    /// How much of `text` is the brand, after the padding has been removed.
    std::size_t length{};

    /// Discard the trailing nulls and the leading spaces the field is padded
    /// with, neither of which belongs in a report.
    void trim() noexcept {
        while (length > 0 && (text[length - 1] == '\0' || text[length - 1] == ' ')) {
            --length;
        }

        std::size_t first = 0;
        while (first < length && text[first] == ' ') {
            ++first;
        }

        if (first > 0) {
            for (std::size_t index = first; index < length; ++index) {
                text[index - first] = text[index];
            }
            length -= first;
        }
    }
};

#ifdef ORRERY_X86

/// One CPUID result, named rather than left as four loose unsigned values.
struct CpuidResult {
    std::uint32_t eax{};
    std::uint32_t ebx{};
    std::uint32_t ecx{};
    std::uint32_t edx{};
};

[[nodiscard]] CpuidResult cpuid(std::uint32_t leaf, std::uint32_t subleaf) noexcept {
    CpuidResult result;

#    ifdef _MSC_VER
    // MSVC's intrinsic writes through an array of four signed integers, which
    // is the shape the instruction has rather than a choice this code makes.
    std::array<int, 4> registers{};
    __cpuidex(registers.data(), static_cast<int>(leaf), static_cast<int>(subleaf));
    result.eax = static_cast<std::uint32_t>(registers[0]);
    result.ebx = static_cast<std::uint32_t>(registers[1]);
    result.ecx = static_cast<std::uint32_t>(registers[2]);
    result.edx = static_cast<std::uint32_t>(registers[3]);
#    else
    __cpuid_count(leaf, subleaf, result.eax, result.ebx, result.ecx, result.edx);
#    endif

    return result;
}

/// The extended control register that says which vector state the operating
/// system has agreed to preserve across a context switch.
///
/// Reading it is only legal once CPUID has reported OSXSAVE, which the one
/// caller below checks first. This is the check that separates "the silicon has
/// AVX" from "AVX may be executed": on a system whose kernel does not save the
/// upper halves of the vector registers, the first is true and the second is
/// not, and using the instructions anyway corrupts them quietly rather than
/// faulting.
[[nodiscard]] std::uint64_t extended_control_register() noexcept {
#    ifdef _MSC_VER
    return _xgetbv(0);
#    else
    // Written as inline assembly rather than through the `_xgetbv` intrinsic,
    // which is only declared when the translation unit is compiled with the
    // XSAVE feature enabled. This file is deliberately compiled for the
    // project's baseline instruction set, and the instruction itself needs no
    // such permission.
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<std::uint64_t>(high) << 32U) | low;
#    endif
}

// Leaf numbers and bit positions from the architecture manual. They are
// constants of the hardware rather than parameters of this project, which is
// why they are written down once here and passed in from nowhere.
constexpr std::uint32_t kLeafHighest = 0;
constexpr std::uint32_t kLeafFeatures = 1;
constexpr std::uint32_t kLeafExtendedFeatures = 7;
constexpr std::uint32_t kFmaBit = 1U << 12U;
constexpr std::uint32_t kOsxsaveBit = 1U << 27U;
constexpr std::uint32_t kAvxBit = 1U << 28U;
constexpr std::uint32_t kAvx2Bit = 1U << 5U;

/// The control register bits covering the lower and upper halves of a 256-bit
/// vector register. Both must be set for a YMM register to survive a context
/// switch.
constexpr std::uint64_t kVectorStateBits = 0x6;

[[nodiscard]] CpuFeatures detect() noexcept {
    CpuFeatures features;

    const CpuidResult highest = cpuid(kLeafHighest, 0);
    if (highest.eax < kLeafFeatures) {
        return features;
    }

    const CpuidResult basic = cpuid(kLeafFeatures, 0);

    const bool operating_system_saves_vectors =
        (basic.ecx & kOsxsaveBit) != 0 &&
        (extended_control_register() & kVectorStateBits) == kVectorStateBits;

    // Every feature below is a 256-bit one, so an operating system that has not
    // enabled the upper vector state makes all of them unusable at once.
    if (!operating_system_saves_vectors) {
        return features;
    }

    features.avx = (basic.ecx & kAvxBit) != 0;
    features.fma = (basic.ecx & kFmaBit) != 0;

    if (highest.eax >= kLeafExtendedFeatures) {
        features.avx2 = (cpuid(kLeafExtendedFeatures, 0).ebx & kAvx2Bit) != 0;
    }

    return features;
}

[[nodiscard]] BrandBuffer detect_brand() noexcept {
    // The brand string lives in three consecutive extended leaves, and a
    // processor that implements fewer of them has no brand string to give.
    constexpr std::uint32_t kExtendedBase = 0x80000000;
    constexpr std::uint32_t kBrandFirst = 0x80000002;
    constexpr std::uint32_t kBrandLast = 0x80000004;

    BrandBuffer brand;

    if (cpuid(kExtendedBase, 0).eax < kBrandLast) {
        return brand;
    }

    // Four bytes per register, least significant first. That order is the
    // instruction's, not a choice.
    for (std::uint32_t leaf = kBrandFirst; leaf <= kBrandLast; ++leaf) {
        const CpuidResult registers = cpuid(leaf, 0);
        const std::array<std::uint32_t, 4> values{registers.eax, registers.ebx, registers.ecx,
                                                  registers.edx};

        for (const std::uint32_t value : values) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                brand.text[brand.length] = static_cast<char>((value >> (byte * 8U)) & 0xFFU);
                ++brand.length;
            }
        }
    }

    brand.trim();
    return brand;
}

#else

[[nodiscard]] CpuFeatures detect() noexcept {
    return {};
}

[[nodiscard]] BrandBuffer detect_brand() noexcept {
    return {};
}

#endif

} // namespace

const CpuFeatures& cpu_features() noexcept {
    static const CpuFeatures kFeatures = detect();
    return kFeatures;
}

std::string_view cpu_brand() noexcept {
    // A function-local static, so that the view returned outlives the call. The
    // buffer is filled once, under the thread-safe initialisation C++
    // guarantees here, and never written again.
    static const BrandBuffer kBrand = detect_brand();
    return {kBrand.text.data(), kBrand.length};
}

} // namespace orrery::backend

#undef ORRERY_X86
