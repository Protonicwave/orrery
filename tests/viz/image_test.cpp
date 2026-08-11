#include "orrery/viz/image.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "orrery/viz/tone_map.hpp"

namespace {

using orrery::viz::Image;
using orrery::viz::tone_map_into;
using orrery::viz::ToneMapping;

/// A file that removes itself, so a failing case leaves no rubbish behind.
///
/// The same idea as `tests/sim/temporary_file.hpp`, written again here rather
/// than shared because that header is on the sim tests' include path and reaching
/// across from another layer's tests to get it would be a worse dependency than
/// six lines.
class TemporaryFile {
public:
    explicit TemporaryFile(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / name) {}

    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("a new image is black and the size it was asked for", "[unit][viz]") {
    const Image image(4, 3);

    REQUIRE(image.width() == 4);
    REQUIRE(image.height() == 3);
    REQUIRE(image.pixels().size() == std::size_t{4} * 3 * Image::kChannels);

    for (const std::uint8_t byte : image.pixels()) {
        CHECK(byte == 0);
    }
}

TEST_CASE("a pixel is written where it was addressed", "[unit][viz]") {
    Image image(3, 2);
    image.set(2, 1, 10, 20, 30);

    // Row major from the top left, so the last pixel of the second row is the
    // last three bytes of the buffer.
    const std::span<const std::uint8_t> pixels = image.pixels();
    CHECK(pixels[15] == 10);
    CHECK(pixels[16] == 20);
    CHECK(pixels[17] == 30);

    // Out of range is ignored rather than fatal: an export that is midway
    // through four thousand frames should not be stopped by one.
    image.set(3, 0, 255, 255, 255);
    image.set(0, 2, 255, 255, 255);
    for (std::size_t index = 0; index < 15; ++index) {
        CHECK(pixels[index] == 0);
    }
}

TEST_CASE("tone mapping fills the image from a radiance buffer", "[unit][viz]") {
    constexpr ToneMapping kMapping{.exposure = 1, .white_point = 4};

    Image image(2, 2);
    const std::vector<float> radiance{
        0,    0,    0,  // bottom left, black
        40,   40,   40, // bottom right, far past the white point
        0.5F, 0,    0,  // top left, a dim red
        0,    0.5F, 0,  // top right, a dim green
    };

    tone_map_into(image, radiance, kMapping, true);

    // Read bottom up, so the first row of the buffer becomes the last row of the
    // image. Getting this wrong gives a picture that is upside down and
    // otherwise perfect, which is why the flag exists and why it is tested.
    const std::span<const std::uint8_t> pixels = image.pixels();

    // Top left of the image is the dim red, the third triple of the buffer.
    CHECK(pixels[0] > 0);
    CHECK(pixels[1] == 0);
    CHECK(pixels[2] == 0);

    // Bottom left is the black one.
    CHECK(pixels[6] == 0);
    CHECK(pixels[7] == 0);
    CHECK(pixels[8] == 0);

    // Bottom right saturates to white in all three channels.
    CHECK(pixels[9] == 255);
    CHECK(pixels[10] == 255);
    CHECK(pixels[11] == 255);
}

TEST_CASE("a written frame reads back as the PPM it claims to be", "[unit][viz]") {
    const TemporaryFile file("orrery_image_test.ppm");

    Image image(2, 1);
    image.set(0, 0, 1, 2, 3);
    image.set(1, 0, 253, 254, 255);
    image.write_ppm(file.path());

    std::ifstream stream(file.path(), std::ios::binary);
    REQUIRE(stream);

    std::string magic;
    std::size_t width = 0;
    std::size_t height = 0;
    int maximum = 0;
    stream >> magic >> width >> height >> maximum;
    stream.get();

    CHECK(magic == "P6");
    CHECK(width == 2);
    CHECK(height == 1);
    CHECK(maximum == 255);

    // Then the pixels, uncompressed and in order. This is the whole of the
    // format, and it is what makes an encoder able to read the frames without
    // anything else being told to it.
    std::vector<char> body(6);
    stream.read(body.data(), 6);
    CHECK(stream.gcount() == 6);

    CHECK(static_cast<std::uint8_t>(body[0]) == 1);
    CHECK(static_cast<std::uint8_t>(body[2]) == 3);
    CHECK(static_cast<std::uint8_t>(body[5]) == 255);
}
