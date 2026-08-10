#include "harness/roofline_plot.hpp"

#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

/// \file
/// That the roofline is drawn from the numbers it was given.
///
/// The plot is the figure `docs/performance/roofline.md` leads with, and it is
/// the one output of this phase that a reader will believe without checking.
/// The arithmetic behind it is therefore checked here against cases whose
/// answers are worked out by hand, and the drawing is checked for the failures
/// that would make it wrong rather than ugly: an empty file, a point that did
/// not appear, a ceiling that did not.

namespace {

using orrery::benchmark::RooflineCeilings;
using orrery::benchmark::RooflinePoint;
using orrery::benchmark::write_roofline_csv;
using orrery::benchmark::write_roofline_svg;

/// Round numbers, so that every expected value below is arithmetic. A hundred
/// gigabytes per second and two hundred gigaflops put the ridge at exactly two
/// operations per byte.
constexpr double kBandwidth = 100e9;
constexpr double kPeak = 200e9;

[[nodiscard]] RooflineCeilings ceilings() {
    return RooflineCeilings{.bandwidth = kBandwidth, .peak = kPeak};
}

} // namespace

TEST_CASE("the ridge is where the two ceilings cross", "[unit][benchmark]") {
    REQUIRE(ceilings().ridge() == 2.0);

    // A machine whose bandwidth was never measured has no ridge, and reporting
    // one would be a division by zero dressed up as a hardware property.
    const RooflineCeilings unmeasured{.bandwidth = 0, .peak = kPeak};
    REQUIRE(unmeasured.ridge() == 0.0);
}

TEST_CASE("a point below the ridge is judged against the sloped roof", "[unit][benchmark]") {
    // At half an operation per byte the roof is bandwidth times intensity,
    // which is 50 Gflop/s. A kernel achieving 25 is at half of it.
    const RooflinePoint point{
        .label = "memory bound", .arithmetic_intensity = 0.5, .achieved = 25e9};

    CAPTURE(point.fraction_of_roof(ceilings()));
    REQUIRE(point.fraction_of_roof(ceilings()) == 0.5);
}

TEST_CASE("a point above the ridge is judged against the flat roof", "[unit][benchmark]") {
    // At a thousand operations per byte the sloped roof is far above the peak,
    // so the peak is the limit and nothing about the bandwidth enters. This is
    // where the direct solver sits, which is the point of measuring it.
    const RooflinePoint point{
        .label = "compute bound", .arithmetic_intensity = 1000.0, .achieved = 50e9};

    REQUIRE(point.fraction_of_roof(ceilings()) == 0.25);
}

TEST_CASE("the plot contains the ceilings and every point it was given", "[unit][benchmark]") {
    const std::vector<RooflinePoint> points{
        RooflinePoint{.label = "scalar", .arithmetic_intensity = 500.0, .achieved = 20e9},
        RooflinePoint{.label = "avx2", .arithmetic_intensity = 500.0, .achieved = 60e9}};

    std::ostringstream drawing;
    write_roofline_svg(drawing, "a title", ceilings(), points);

    const std::string svg = drawing.str();
    CAPTURE(svg.size());

    // Well formed enough to render: an element that opens and closes, and a
    // viewBox so that it scales rather than being pinned to one size.
    REQUIRE(svg.starts_with("<svg"));
    REQUIRE(svg.ends_with("</svg>\n"));
    REQUIRE(svg.find("viewBox") != std::string::npos);

    // No external anything. The file is committed beside the document that
    // includes it and has to render with no network and no fonts installed.
    REQUIRE(svg.find("http://www.w3.org/2000/svg") != std::string::npos);
    REQUIRE(svg.find("<script") == std::string::npos);
    REQUIRE(svg.find("xlink:href") == std::string::npos);

    REQUIRE(svg.find("a title") != std::string::npos);
    REQUIRE(svg.find("scalar") != std::string::npos);
    REQUIRE(svg.find("avx2") != std::string::npos);

    // The roof itself, and two markers.
    REQUIRE(svg.find("<polyline") != std::string::npos);
    REQUIRE(svg.find("<circle") != std::string::npos);
}

TEST_CASE("a plot with no points is a plot of the machine", "[unit][benchmark]") {
    // The ceilings alone are a result: they are what section 2 of the
    // implementation plan asks Phase 7 to replace the manufacturer's figures
    // with. A drawing of them with nothing under the roof should not fail.
    std::ostringstream drawing;
    write_roofline_svg(drawing, "empty", ceilings(), {});

    const std::string svg = drawing.str();
    REQUIRE(svg.starts_with("<svg"));
    REQUIRE(svg.find("<polyline") != std::string::npos);
    REQUIRE(svg.find("<circle") == std::string::npos);
}

TEST_CASE("the values are written out beside the drawing of them", "[unit][benchmark]") {
    // A reader who wants to plot the numbers differently should not have to
    // read them out of a picture.
    const std::vector<RooflinePoint> points{
        RooflinePoint{.label = "avx2", .arithmetic_intensity = 1000.0, .achieved = 50e9}};

    std::ostringstream values;
    write_roofline_csv(values, ceilings(), points);

    const std::string csv = values.str();
    CAPTURE(csv);

    REQUIRE(csv.starts_with("quantity,"));
    REQUIRE(csv.find("ridge,2,") != std::string::npos);
    REQUIRE(csv.find("avx2,1000,5e+10,0.25") != std::string::npos);
}
