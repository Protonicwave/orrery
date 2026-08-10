#pragma once

/// \file
/// The roofline, drawn.
///
/// A roofline plot answers one question that a table of timings cannot: of the
/// two things that could be limiting a kernel, arithmetic or data movement,
/// which one is, and how much of that limit is the kernel reaching. Both axes
/// are logarithmic. The horizontal is arithmetic intensity, floating-point
/// operations performed per byte moved from memory; the vertical is the rate
/// achieved.
///
/// The roof itself is
///
///     attainable(I) = min(peak, bandwidth * I)
///
/// a sloped section on the left where a kernel cannot be fed fast enough
/// however much arithmetic the part could do, and a flat section on the right
/// where the arithmetic is the limit. The corner between them, at
/// `peak / bandwidth`, is the intensity at which the machine changes character.
/// A kernel plotted below the sloped part is wasting bandwidth; one below the
/// flat part is wasting arithmetic; and which of those a reader is looking at
/// decides what to do next, which is the whole value of the picture.
///
/// ## Why the output is an SVG written here
///
/// Section 5 of the implementation plan requires every claim to be reproducible
/// by a documented command, and section 3 keeps the dependency surface small. A
/// plot produced by a plotting library would add a dependency, and one produced
/// by a script the reader has to run separately would put a step between the
/// measurement and the figure where the two can drift apart. An SVG is text,
/// this file writes about a hundred lines of it, and the same program that took
/// the measurements emits the picture of them in the same run.
///
/// A CSV of the same data is written beside it, because a reader who wants to
/// plot the numbers differently should not have to read them out of a drawing.

#include <iosfwd>
#include <span>
#include <string>

namespace orrery::benchmark {

/// The two ceilings the roof is built from, as measured.
struct RooflineCeilings {
    /// Bytes per second, from `measure_read_bandwidth`.
    double bandwidth{};

    /// Floating-point operations per second, from `measure_peak_throughput`.
    double peak{};

    /// A second, lower flat ceiling for the instruction mix a kernel actually
    /// issues, in the same units as `peak`. Zero when there is none to draw.
    ///
    /// The plain roofline has one flat section, and for a kernel issuing only
    /// multiplies and adds that is the right ceiling. The direct solver issues
    /// one square root and one division in every twenty operations, and those
    /// retire on a unit with a small fraction of the throughput of the
    /// multiply-add pipelines, so its ceiling is far below the peak and no
    /// amount of good implementation would reach the upper one.
    ///
    /// Drawing only the peak would therefore mislead in the direction that
    /// flatters nobody: it would show a well-written kernel at a third of the
    /// roof and invite the conclusion that two thirds had been left on the
    /// table. Drawing only this one would hide what the machine can do. Both
    /// are drawn, and the gap between them is a property of the algorithm
    /// rather than of the code.
    double restricted{};

    /// What the restricted ceiling is a ceiling of, for its label.
    std::string restricted_label;

    /// The arithmetic intensity where the bandwidth and the peak cross.
    ///
    /// Below it a kernel is bound by memory and above it by arithmetic. It is a
    /// property of the machine rather than of any kernel, and it is the number
    /// that decides whether the layout decisions of ADR-0004 or the vector
    /// kernel of this phase is the thing that matters for a given loop.
    [[nodiscard]] double ridge() const noexcept { return bandwidth > 0 ? peak / bandwidth : 0; }

    /// Where the restricted ceiling leaves the sloped section, which is the
    /// leftmost point at which there is anything to draw.
    [[nodiscard]] double restricted_ridge() const noexcept {
        return bandwidth > 0 ? restricted / bandwidth : 0;
    }
};

/// One kernel's place on the plot.
struct RooflinePoint {
    /// What it is, printed beside the marker.
    std::string label;

    /// Floating-point operations per byte read from memory.
    double arithmetic_intensity{};

    /// Floating-point operations per second achieved.
    double achieved{};

    /// The achieved rate as a fraction of what the roof allows at this
    /// intensity. This is the figure section 7 of the implementation plan asks
    /// each kernel to state.
    [[nodiscard]] double fraction_of_roof(const RooflineCeilings& ceilings) const noexcept;
};

/// Write a self-contained SVG of the ceilings and the points.
///
/// No external stylesheet, no font file and no script, so the file renders the
/// same wherever it is opened and can be committed beside the document that
/// includes it. The colours are chosen to be legible against both a light and a
/// dark page background, since the same file is read on both.
void write_roofline_svg(std::ostream& out, const std::string& title,
                        const RooflineCeilings& ceilings, std::span<const RooflinePoint> points);

/// Write the same data as comma-separated values.
void write_roofline_csv(std::ostream& out, const RooflineCeilings& ceilings,
                        std::span<const RooflinePoint> points);

} // namespace orrery::benchmark
