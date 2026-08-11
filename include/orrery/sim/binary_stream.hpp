#pragma once

/// \file
/// Fixed-width little-endian reading and writing, which is what makes the two
/// binary formats in this layer specifications rather than memory dumps.
///
/// A file format written by pointing `ostream::write` at a struct is not a
/// format. It records the compiler's padding, the platform's endianness and the
/// width of whatever `unsigned long` happened to be, none of which appear in any
/// document, and it is read back correctly only by the binary that wrote it. The
/// checkpoint format exists so that a run interrupted on this machine can be
/// resumed, and the trajectory format so that something else can read the
/// positions later, so both have to say exactly which bytes go where.
///
/// So every scalar goes through this file. Integers are assembled byte by byte
/// with shifts rather than copied, which means the result does not depend on the
/// endianness of the machine doing the assembling and no `#ifdef` is needed to
/// say so. Floating-point values are converted to an unsigned integer of the
/// same width and written the same way, which preserves every bit including the
/// sign of a zero and the payload of a NaN. That last property is the one the
/// bitwise-resume requirement in Phase 11 rests on: a checkpoint that wrote
/// `1.0000000000000002` as text and read back `1.0` would resume a different
/// simulation, and the difference would take a few thousand steps to become
/// visible.
///
/// ## The checksum
///
/// Both writers finish with a checksum of everything they wrote and both readers
/// verify it. It is FNV-1a over the bytes, which is not a cryptographic hash and
/// is not trying to be: the failure it exists to catch is a checkpoint written by
/// a run that was killed halfway through writing it, which is the ordinary way a
/// long simulation ends and would otherwise be detected as a physically absurd
/// result several hours later. A truncated file also fails the length check, but
/// a file truncated to exactly the right length by a full disc does not, and
/// neither does one that lost a block in the middle.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "orrery/core/types.hpp"

namespace orrery::sim {

static_assert(std::numeric_limits<core::Real>::is_iec559,
              "The binary formats store the bit pattern of a Real, which assumes IEEE 754");

/// An unsigned integer the same width as `Real`, which is what a bit pattern is
/// carried in between this file and the stream.
///
/// An alias rather than a branch inside the reading and writing functions.
/// `if constexpr` discards nothing in a function that is not a template, so a
/// double-precision build would still have to compile a `bit_cast` between a
/// `double` and a `std::uint32_t` and would fail to.
using RealBits = std::conditional_t<core::kSinglePrecision, std::uint32_t, std::uint64_t>;

static_assert(sizeof(RealBits) == sizeof(core::Real));

/// How many bytes a `Real` occupies in both formats.
inline constexpr std::size_t kRealBytes = sizeof(core::Real);

/// The offset basis and prime of 64-bit FNV-1a.
///
/// Named rather than written in place because the reader and the writer both
/// need them and a hash that disagreed with itself would reject every file.
inline constexpr std::uint64_t kChecksumBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kChecksumPrime = 0x00000100000001b3ULL;

/// Fold one byte into a running FNV-1a checksum.
[[nodiscard]] constexpr std::uint64_t checksum_byte(std::uint64_t checksum,
                                                    std::byte value) noexcept {
    return (checksum ^ static_cast<std::uint64_t>(value)) * kChecksumPrime;
}

/// Writes the scalar types the formats in this layer are built from.
///
/// Holds the stream by reference and does not own it, so that a caller can
/// write a header through one of these and the bulk arrays through another, or
/// wrap a `std::ostringstream` in a test rather than a file. The checksum
/// accumulates across everything one instance writes, which is why the caller
/// keeps hold of it rather than making a fresh one per field.
class BinaryWriter {
public:
    explicit BinaryWriter(std::ostream& out) noexcept : out_(&out) {}

    void write_u8(std::uint8_t value) { write_integer(value, 1); }

    void write_u32(std::uint32_t value) { write_integer(value, 4); }

    void write_u64(std::uint64_t value) { write_integer(value, 8); }

    /// Write a `Real` as its IEEE 754 bit pattern, four bytes or eight
    /// according to the precision this build was configured with.
    ///
    /// The formats record which in their headers, so a reader can refuse a file
    /// rather than silently read a double-precision checkpoint as twice as many
    /// single-precision particles.
    void write_real(core::Real value) { write_integer(std::bit_cast<RealBits>(value), kRealBytes); }

    /// Write a contiguous run of `Real`, which is how both formats carry
    /// particle components.
    void write_reals(std::span<const core::Real> values) {
        for (const core::Real value : values) {
            write_real(value);
        }
    }

    /// Write the exact bytes of `text`, with no length prefix and no terminator.
    ///
    /// For the fixed-length magic strings at the head of each format. Anything
    /// of variable length is written as a length followed by the bytes, which is
    /// what `write_string` does.
    void write_bytes(std::string_view text);

    /// Write a 32-bit length followed by that many bytes.
    void write_string(std::string_view text) {
        write_u32(static_cast<std::uint32_t>(text.size()));
        write_bytes(text);
    }

    /// The checksum of everything written so far.
    [[nodiscard]] std::uint64_t checksum() const noexcept { return checksum_; }

    /// Whether every write so far reached the stream.
    ///
    /// Checked once at the end of a format rather than after each field. A
    /// stream that has failed stays failed, so a single test after the last
    /// write catches a failure at any point in it, and a disc that filled up
    /// halfway through a million particles should not cost a branch per
    /// component.
    [[nodiscard]] bool ok() const noexcept { return out_->good(); }

private:
    /// Write the low `width` bytes of `value`, least significant first.
    ///
    /// Assembled with shifts into a buffer of `char` rather than copied from
    /// the object's own storage, which is what makes the result independent of
    /// this machine's byte order. `char` rather than `std::byte` because that is
    /// what `std::ostream::write` takes, and converting between the two would
    /// need the cast this project would rather not write.
    void write_integer(std::uint64_t value, std::size_t width) {
        std::array<char, 8> bytes{};
        for (std::size_t index = 0; index < width; ++index) {
            const auto octet = static_cast<unsigned char>((value >> (8 * index)) & 0xffU);
            bytes[index] = static_cast<char>(octet);
            checksum_ = checksum_byte(checksum_, static_cast<std::byte>(octet));
        }
        out_->write(bytes.data(), static_cast<std::streamsize>(width));
    }

    std::ostream* out_;
    std::uint64_t checksum_{kChecksumBasis};
};

/// Reads what `BinaryWriter` writes.
///
/// Every read of a stream that has already failed returns zero rather than
/// blocking or throwing, and `ok()` stays false once it has been false. That
/// lets a format reader parse a whole header without checking after each field
/// and then decide what to do once, which is both easier to read and the only
/// way the error message can name the file rather than the field.
class BinaryReader {
public:
    explicit BinaryReader(std::istream& in) noexcept : in_(&in) {}

    [[nodiscard]] std::uint8_t read_u8() { return static_cast<std::uint8_t>(read_integer(1)); }

    [[nodiscard]] std::uint32_t read_u32() { return static_cast<std::uint32_t>(read_integer(4)); }

    [[nodiscard]] std::uint64_t read_u64() { return read_integer(8); }

    [[nodiscard]] core::Real read_real() {
        return std::bit_cast<core::Real>(static_cast<RealBits>(read_integer(kRealBytes)));
    }

    /// Fill `values` with that many consecutive `Real`.
    void read_reals(std::span<core::Real> values) {
        for (core::Real& value : values) {
            value = read_real();
        }
    }

    /// Read exactly `length` bytes.
    [[nodiscard]] std::string read_bytes(std::size_t length);

    /// Read a 32-bit length followed by that many bytes.
    ///
    /// The length is checked against `limit` before the allocation is made.
    /// Without that, a corrupt file claiming a four-billion-byte string would
    /// have this function ask for four gigabytes before discovering the file is
    /// eighty bytes long, and a reader that can be made to exhaust memory by a
    /// malformed input is a reader that cannot be pointed at an untrusted file.
    [[nodiscard]] std::string read_string(std::size_t limit);

    /// The checksum of everything read so far, comparable against the one the
    /// writer recorded.
    [[nodiscard]] std::uint64_t checksum() const noexcept { return checksum_; }

    /// Whether every read so far found the bytes it wanted.
    [[nodiscard]] bool ok() const noexcept { return in_->good(); }

private:
    [[nodiscard]] std::uint64_t read_integer(std::size_t width);

    std::istream* in_;
    std::uint64_t checksum_{kChecksumBasis};
};

} // namespace orrery::sim
