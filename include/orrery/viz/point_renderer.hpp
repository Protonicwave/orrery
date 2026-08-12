#pragma once

/// \file
/// Drawing a few hundred thousand point masses as a field of stars.
///
/// The whole renderer is two passes. The first draws every particle as a small
/// round sprite with additive blending into a floating-point target, so that
/// where many particles overlap the light adds up rather than the nearest one
/// winning. The second reads that target, compresses its range with the curve in
/// `tone_map.hpp` and writes the result to the screen.
///
/// ## Why additive blending, and why a floating-point target
///
/// A galaxy is not a set of opaque objects. Each particle in this simulation
/// stands for many millions of stars, and what a picture of it should show is
/// how much light comes from each direction: a sum along the line of sight, not
/// the nearest contributor to it. Additive blending is that sum, and it is why
/// there is no depth test here. Turning one on would make the picture depend on
/// the order the particles happen to be stored in.
///
/// A sum has no upper bound, so the target it accumulates into must not either.
/// Rendering to the usual 8-bit-per-channel buffer would saturate the middle of
/// a galaxy after a few dozen particles, and every particle after that would be
/// added to a value that was already at the maximum, so the brightest structure
/// in the image would carry no information at all. A half-float target holds the
/// sum with room to spare and lets the tone curve decide afterwards what counts
/// as white.
///
/// ## Why the component arrays go up as three attributes
///
/// The simulation stores positions as three separate arrays (ADR-0004) and a
/// vertex attribute is a strided read of one array, so the natural thing is
/// three attributes of one float each, assembled into a vector by the vertex
/// shader. No interleaving pass, no gather, and in a single-precision build the
/// upload is a copy of exactly the bytes the solver wrote.
///
/// ## What this class does not do
///
/// It does not own a window, create a context, or read the keyboard. It is given
/// a function that resolves OpenGL entry points and assumes a context is current
/// on the calling thread. That is what lets the same class serve both the
/// interactive viewer and the offline export, which differ only in whether
/// anything is ever presented to a screen.

#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/viz/gl_api.hpp"
#include "orrery/viz/matrix4.hpp"
#include "orrery/viz/tone_map.hpp"

namespace orrery::viz {

/// A colour in linear light, before tone mapping.
struct Colour {
    float red = 1;
    float green = 1;
    float blue = 1;
};

/// Everything about how the picture looks that is not the camera.
struct RenderSettings {
    ToneMapping tone_mapping;

    /// The diameter in pixels a particle would have one length unit from the
    /// camera, shrinking with distance as anything else does.
    ///
    /// Sprites rather than single pixels because a single pixel is invisible on
    /// a modern display and aliases badly when it moves. A sprite a few pixels
    /// across, with a soft edge, is what makes a moving point cloud read as a
    /// continuous structure rather than as a swarm of flickering dots.
    float point_size = 40;

    /// The smallest a particle is allowed to become.
    ///
    /// Below about one pixel a sprite starts to disappear between the samples of
    /// the rasteriser, and a distant part of the system fades out for a reason
    /// that has nothing to do with how much light is there. Holding a floor and
    /// letting the brightness carry the distance instead is the standard remedy.
    float minimum_point_size = 1.5F;

    /// The light one particle contributes at the centre of its sprite.
    ///
    /// Multiplied by the tone mapping's exposure in the end, so the two are
    /// redundant as a pair of numbers. They are separate because they are
    /// adjusted for different reasons: this follows the particle count, since
    /// twice as many particles for the same mass means each carries half the
    /// light, and the exposure follows what the viewer wants to see.
    float brightness = 1;

    /// The two ends of the colour ramp the per-particle tint selects between.
    ///
    /// The default is the one a star field wants: a cool blue-white for the
    /// young population of a disc and a warm yellow for an older one. In the
    /// collision the tint separates the two galaxies, which is what makes it
    /// possible to see which material in the merged remnant came from where.
    Colour cool{0.55F, 0.70F, 1.0F};
    Colour warm{1.0F, 0.78F, 0.50F};
};

/// The two-pass point renderer described above.
class PointRenderer {
public:
    /// Build every OpenGL object the renderer needs, for a target of the given
    /// size.
    ///
    /// A context must be current on the calling thread. Throws
    /// `std::runtime_error` if an entry point is missing, a shader fails to
    /// compile or link, or the framebuffer is incomplete, each with what the
    /// driver said about it.
    PointRenderer(GlProcedureLoader loader, std::size_t width, std::size_t height);

    ~PointRenderer();

    // The class owns names allocated by the driver, so it is neither copyable
    // nor movable. A moved-from renderer would have to hold zeroes that the
    // destructor then asks the driver to delete, and OpenGL defines zero as a
    // valid argument meaning "no object", so the mistake would be silent.
    PointRenderer(const PointRenderer&) = delete;
    PointRenderer& operator=(const PointRenderer&) = delete;
    PointRenderer(PointRenderer&&) = delete;
    PointRenderer& operator=(PointRenderer&&) = delete;

    /// Change the size of the accumulation target.
    ///
    /// Does nothing if the size has not changed, which is what makes it safe to
    /// call on every frame from a window that reports its size each time.
    void resize(std::size_t width, std::size_t height);

    [[nodiscard]] std::size_t width() const noexcept;
    [[nodiscard]] std::size_t height() const noexcept;

    /// Draw the particles into the accumulation target, replacing what was there.
    ///
    /// `tints` selects between the two colours of the settings for each particle
    /// and may be empty, which draws everything in the cool colour. If it is not
    /// empty it must hold one value per particle.
    void accumulate(core::Vec3Span<const core::Real> positions, std::span<const float> tints,
                    const Mat4& view_projection, const RenderSettings& settings);

    /// Tone map the accumulation target onto the default framebuffer, which is
    /// the window.
    ///
    /// Separate from `accumulate` because the offline export does not want it:
    /// what an exported frame is made from is the accumulated radiance, and it
    /// is tone mapped on the host so that the result does not depend on how a
    /// driver rounds a fragment shader. ADR-0036.
    void present(const RenderSettings& settings);

    /// How many floats `read_radiance` needs, which is three per pixel.
    [[nodiscard]] std::size_t radiance_size() const noexcept;

    /// Read the accumulated radiance back from the device.
    ///
    /// The first row is the bottom of the picture, which is OpenGL's convention
    /// and what `tone_map_into` calls bottom up.
    ///
    /// This is a synchronous read of the whole target and stalls the pipeline. It
    /// is what the export path does once per frame and what nothing else should
    /// do at all.
    void read_radiance(std::span<float> radiance) const;

    /// What the driver calls itself, for the line a run prints about where its
    /// frames came from.
    [[nodiscard]] std::string device_description() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace orrery::viz
