#include "orrery/sim/binary_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <string>
#include <string_view>

namespace orrery::sim {

void BinaryWriter::write_bytes(std::string_view text) {
    for (const char character : text) {
        checksum_ = checksum_byte(checksum_, static_cast<std::byte>(character));
    }
    out_->write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::uint64_t BinaryReader::read_integer(std::size_t width) {
    std::array<char, 8> bytes{};
    in_->read(bytes.data(), static_cast<std::streamsize>(width));
    if (!in_->good()) {
        // Deliberately not folded into the checksum. A short read leaves part of
        // the buffer holding whatever the previous read left there, and hashing
        // it would make the checksum depend on how the file failed rather than
        // on what it contained.
        return 0;
    }

    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        const auto octet = static_cast<unsigned char>(bytes[index]);
        value |= static_cast<std::uint64_t>(octet) << (8 * index);
        checksum_ = checksum_byte(checksum_, static_cast<std::byte>(octet));
    }
    return value;
}

std::string BinaryReader::read_bytes(std::size_t length) {
    std::string text(length, '\0');
    in_->read(text.data(), static_cast<std::streamsize>(length));
    if (!in_->good()) {
        return {};
    }

    for (const char character : text) {
        checksum_ = checksum_byte(checksum_, static_cast<std::byte>(character));
    }
    return text;
}

std::string BinaryReader::read_string(std::size_t limit) {
    const std::uint32_t length = read_u32();
    if (!in_->good() || length > limit) {
        // Marking the stream failed rather than returning an empty string is
        // what makes an over-long length an error the caller sees. A reader that
        // returned nothing and stayed good would let a corrupt file present
        // itself as one with an empty field in it.
        in_->setstate(std::ios::failbit);
        return {};
    }
    return read_bytes(length);
}

} // namespace orrery::sim
