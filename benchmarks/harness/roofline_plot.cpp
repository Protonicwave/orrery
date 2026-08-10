#include "harness/roofline_plot.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <ios>
#include <ostream>
#include <span>
#include <sstream>
#include <string>

namespace orrery::benchmark {

namespace {

/// The drawing, in user units. An SVG scales, so these decide proportions
/// rather than size.
constexpr double kWidth = 900;
constexpr double kHeight = 560;
constexpr double kLeft = 90;
constexpr double kRight = kWidth - 30;
constexpr double kTop = 50;
constexpr double kBottom = kHeight - 60;

/// Mid tones, so that the same file is legible on a white page and a dark one.
/// A plot that assumed one background would be unreadable on the other, and
/// this one is committed to a repository read on both.
constexpr const char* kAxisColour = "#8a8a8a";
constexpr const char* kTextColour = "#8a8a8a";
constexpr const char* kRoofColour = "#4c78a8";
constexpr const char* kPointColour = "#e4572e";

/// A logarithmic axis, in whole decades.
///
/// The bounds are integers because every tick is a power of ten and because a
/// loop counter that advances by one has no business being a floating-point
/// number.
struct LogAxis {
    int low{};
    int high{};

    [[nodiscard]] double position(double value, double from, double to) const {
        if (value <= 0 || high <= low) {
            return from;
        }
        const double span = static_cast<double>(high) - static_cast<double>(low);
        const double fraction = (std::log10(value) - static_cast<double>(low)) / span;
        return from + (fraction * (to - from));
    }

    [[nodiscard]] static double value_at(int decade) { return std::pow(10.0, decade); }
};

/// Round a range out to whole decades, with at least two of them.
///
/// Whole decades so that every tick is a power of ten and the reader is not
/// asked to interpolate on a log scale, and at least two so that a plot whose
/// points happen to be close together still shows the shape of the roof rather
/// than a magnified fragment of it.
[[nodiscard]] LogAxis decades_covering(double smallest, double largest) {
    if (smallest <= 0 || largest <= 0) {
        return LogAxis{.low = 0, .high = 1};
    }

    LogAxis axis{.low = static_cast<int>(std::floor(std::log10(smallest))),
                 .high = static_cast<int>(std::ceil(std::log10(largest)))};

    if (axis.high - axis.low < 2) {
        axis.high = axis.low + 2;
    }

    return axis;
}

/// A number in the form a table would use: 12.3, 1.23k, 45.6M.
[[nodiscard]] std::string engineering(double value) {
    if (value <= 0) {
        return "0";
    }

    static constexpr std::array<const char*, 6> kSuffixes{"", "k", "M", "G", "T", "P"};

    std::size_t suffix = 0;
    double scaled = value;
    while (scaled >= 1000.0 && suffix + 1 < kSuffixes.size()) {
        scaled /= 1000.0;
        ++suffix;
    }

    std::ostringstream text;
    text << std::fixed << std::setprecision(scaled < 10 ? 2 : 1) << scaled << kSuffixes[suffix];
    return text.str();
}

/// A text element, since the plot draws a dozen of them and the attributes are
/// the same every time.
void write_text(std::ostream& out, double x, double y, const char* anchor, int size,
                const char* colour, const std::string& content) {
    out << R"(  <text x=")" << x << R"(" y=")" << y << R"(" text-anchor=")" << anchor
        << R"(" font-size=")" << size << R"(" fill=")" << colour << R"(">)" << content
        << "</text>\n";
}

void write_grid_line(std::ostream& out, double x1, double y1, double x2, double y2) {
    out << R"(  <line x1=")" << x1 << R"(" y1=")" << y1 << R"(" x2=")" << x2 << R"(" y2=")" << y2
        << R"(" stroke=")" << kAxisColour << R"(" stroke-width="0.4" stroke-dasharray="3 4"/>)"
        << '\n';
}

void write_axes(std::ostream& out, const LogAxis& horizontal, const LogAxis& vertical) {
    out << R"(  <rect x=")" << kLeft << R"(" y=")" << kTop << R"(" width=")" << (kRight - kLeft)
        << R"(" height=")" << (kBottom - kTop) << R"(" fill="none" stroke=")" << kAxisColour
        << R"(" stroke-width="1"/>)" << '\n';

    for (int decade = horizontal.low; decade <= horizontal.high; ++decade) {
        const double x = horizontal.position(LogAxis::value_at(decade), kLeft, kRight);
        write_grid_line(out, x, kTop, x, kBottom);
        write_text(out, x, kBottom + 20, "middle", 12, kTextColour,
                   engineering(LogAxis::value_at(decade)));
    }

    for (int decade = vertical.low; decade <= vertical.high; ++decade) {
        const double y = vertical.position(LogAxis::value_at(decade), kBottom, kTop);
        write_grid_line(out, kLeft, y, kRight, y);
        write_text(out, kLeft - 8, y + 4, "end", 12, kTextColour,
                   engineering(LogAxis::value_at(decade)));
    }

    write_text(out, (kLeft + kRight) / 2, kBottom + 45, "middle", 13, kTextColour,
               "arithmetic intensity, flop per byte read from memory");

    // The vertical label, rotated about its own anchor. Written directly rather
    // than through the helper because the transform is the only attribute in
    // the drawing that appears once.
    const double middle = (kTop + kBottom) / 2;
    out << R"(  <text x="20" y=")" << middle << R"(" text-anchor="middle" font-size="13" fill=")"
        << kTextColour << R"(" transform="rotate(-90 20 )" << middle
        << R"svg()">attained flop per second</text>)svg" << '\n';
}

void write_roof(std::ostream& out, const LogAxis& horizontal, const LogAxis& vertical,
                const RooflineCeilings& ceilings) {
    // Three points: the left edge on the sloped section, the ridge, and the
    // right edge on the flat one. A polyline because that is exactly what the
    // roof is, rather than a sampled curve, which would make two straight lines
    // look approximate.
    const double left_intensity = LogAxis::value_at(horizontal.low);
    const double right_intensity = LogAxis::value_at(horizontal.high);

    out << R"(  <polyline fill="none" stroke=")" << kRoofColour << R"(" stroke-width="2" points=")"
        << horizontal.position(left_intensity, kLeft, kRight) << ','
        << vertical.position(ceilings.bandwidth * left_intensity, kBottom, kTop) << ' '
        << horizontal.position(ceilings.ridge(), kLeft, kRight) << ','
        << vertical.position(ceilings.peak, kBottom, kTop) << ' '
        << horizontal.position(right_intensity, kLeft, kRight) << ','
        << vertical.position(ceilings.peak, kBottom, kTop) << R"("/>)" << '\n';

    write_text(out, horizontal.position(ceilings.ridge(), kLeft, kRight) + 8,
               vertical.position(ceilings.peak, kBottom, kTop) - 10, "start", 12, kRoofColour,
               "peak " + engineering(ceilings.peak) + " flop/s, ridge at " +
                   engineering(ceilings.ridge()) + " flop/byte");

    if (ceilings.restricted <= 0) {
        return;
    }

    // The second roof, dashed to say that it is a property of what the kernel
    // issues rather than of what the machine can do. It leaves the sloped
    // section at its own ridge and runs flat from there.
    const double corner = std::max(ceilings.restricted_ridge(), left_intensity);
    const double y = vertical.position(ceilings.restricted, kBottom, kTop);

    out << R"(  <polyline fill="none" stroke=")" << kRoofColour
        << R"(" stroke-width="1.6" stroke-dasharray="6 4" points=")"
        << horizontal.position(corner, kLeft, kRight) << ',' << y << ' '
        << horizontal.position(right_intensity, kLeft, kRight) << ',' << y << R"("/>)" << '\n';

    write_text(out, horizontal.position(corner, kLeft, kRight) + 8, y - 10, "start", 12,
               kRoofColour,
               ceilings.restricted_label + ", " + engineering(ceilings.restricted) + " flop/s");
}

/// One kernel's marker, its label, and a dotted line up to the roof.
///
/// The line is there so that the gap the label quantifies is visible as a
/// distance rather than only as a percentage.
void write_point(std::ostream& out, const LogAxis& horizontal, const LogAxis& vertical,
                 const RooflineCeilings& ceilings, const RooflinePoint& point) {
    const double x = horizontal.position(point.arithmetic_intensity, kLeft, kRight);
    const double y = vertical.position(point.achieved, kBottom, kTop);
    const double roof = std::min(ceilings.peak, ceilings.bandwidth * point.arithmetic_intensity);

    out << R"(  <line x1=")" << x << R"(" y1=")" << y << R"(" x2=")" << x << R"(" y2=")"
        << vertical.position(roof, kBottom, kTop) << R"(" stroke=")" << kPointColour
        << R"(" stroke-width="0.8" stroke-dasharray="2 3"/>)" << '\n'
        << R"(  <circle cx=")" << x << R"(" cy=")" << y << R"(" r="5" fill=")" << kPointColour
        << R"("/>)" << '\n';

    std::ostringstream label;
    label << point.label << ", " << engineering(point.achieved) << " flop/s (" << std::fixed
          << std::setprecision(0) << (point.fraction_of_roof(ceilings) * 100.0) << "% of roof)";

    // Labels go to the left of a marker in the right half of the plot and to
    // the right of one in the left half, so that they stay inside the frame.
    // The direct solver's arithmetic intensity is enormous, so every point this
    // project plots is at the right-hand edge and would otherwise be labelled
    // off the page.
    const bool on_the_right = x > (kLeft + kRight) / 2;

    write_text(out, on_the_right ? x - 10 : x + 10, y + 4, on_the_right ? "end" : "start", 12,
               kPointColour, label.str());
}

} // namespace

double RooflinePoint::fraction_of_roof(const RooflineCeilings& ceilings) const noexcept {
    const double roof = std::min(ceilings.peak, ceilings.bandwidth * arithmetic_intensity);
    return roof > 0 ? achieved / roof : 0.0;
}

void write_roofline_svg(std::ostream& out, const std::string& title,
                        const RooflineCeilings& ceilings, std::span<const RooflinePoint> points) {
    // The plot has to show the ridge, every point, and enough either side of
    // them for the two sections of the roof to be distinguishable. Taking the
    // extremes of everything that will be drawn and rounding out to decades is
    // what does that without any per-plot tuning.
    double smallest_intensity = ceilings.ridge();
    double largest_intensity = ceilings.ridge();
    double smallest_rate = ceilings.peak;

    if (ceilings.restricted > 0) {
        smallest_intensity = std::min(smallest_intensity, ceilings.restricted_ridge());
        smallest_rate = std::min(smallest_rate, ceilings.restricted);
    }

    for (const RooflinePoint& point : points) {
        smallest_intensity = std::min(smallest_intensity, point.arithmetic_intensity);
        largest_intensity = std::max(largest_intensity, point.arithmetic_intensity);
        smallest_rate = std::min(smallest_rate, point.achieved);
    }

    const LogAxis horizontal = decades_covering(smallest_intensity / 4.0, largest_intensity * 4.0);
    const LogAxis vertical = decades_covering(smallest_rate / 2.0, ceilings.peak * 2.0);

    out << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 )" << kWidth << ' ' << kHeight
        << R"(" width=")" << kWidth << R"(" height=")" << kHeight
        << R"(" font-family="system-ui, sans-serif">)" << '\n'
        << "  <title>" << title << "</title>\n";

    write_text(out, kLeft, 28, "start", 16, kTextColour, title);
    write_axes(out, horizontal, vertical);
    write_roof(out, horizontal, vertical, ceilings);

    for (const RooflinePoint& point : points) {
        write_point(out, horizontal, vertical, ceilings, point);
    }

    out << "</svg>\n";
}

void write_roofline_csv(std::ostream& out, const RooflineCeilings& ceilings,
                        std::span<const RooflinePoint> points) {
    out << "quantity,arithmetic_intensity_flop_per_byte,rate_flop_per_second,fraction_of_roof\n"
        << "measured read bandwidth,," << ceilings.bandwidth << ",\n"
        << "measured peak throughput,," << ceilings.peak << ",\n"
        << "ridge," << ceilings.ridge() << ",,\n";

    if (ceilings.restricted > 0) {
        out << ceilings.restricted_label << " ceiling,," << ceilings.restricted << ",\n";
    }

    for (const RooflinePoint& point : points) {
        out << point.label << ',' << point.arithmetic_intensity << ',' << point.achieved << ','
            << point.fraction_of_roof(ceilings) << '\n';
    }
}

} // namespace orrery::benchmark
