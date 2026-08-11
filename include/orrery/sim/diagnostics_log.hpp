#pragma once

/// \file
/// The conserved quantities of a run, as they change, in a format anything can
/// read.
///
/// This is the output the project's headline claims are made from. Whether a
/// symplectic integrator holds its energy inside a bounded envelope while RK4
/// drifts is a statement about a column of numbers, and the test suite makes it
/// over a few hundred steps in memory. A run of a few million steps has to write
/// them somewhere, and what reads them afterwards is a plotting script rather
/// than a C++ program, so the format is CSV with a header row and nothing
/// clever in it.
///
/// Two columns are worth pointing at.
///
/// `relative_energy_error` is the quantity every energy plot in the project is
/// actually of: the total energy less the total energy at the first row,
/// divided by the magnitude of that first value. It is computed here rather than
/// left to the reader because the alternative is every script that reads one of
/// these files having its own idea of which row was the reference.
///
/// `virial_ratio` is `2T/|U|`, the convention `core/diagnostics.hpp` fixes for
/// the whole project. It is one for a self-gravitating system in equilibrium,
/// and watching a sampled Plummer sphere hold it is how a run says it started
/// where it claimed to.
///
/// Numbers are written to the full precision of the build, so a value read back
/// from the file is the value that was measured. A diagnostics stream rounded to
/// six digits would report an energy that is conserved to six digits whatever
/// the integrator did.

#include <filesystem>
#include <fstream>
#include <optional>

#include "orrery/core/diagnostics.hpp"
#include "orrery/core/types.hpp"

namespace orrery::sim {

/// Appends one row per measurement to a CSV file.
class DiagnosticsLog {
public:
    /// Create the file and write the header row.
    ///
    /// Throws `std::runtime_error` if the file cannot be opened. A setup
    /// boundary, and the right place to discover an unwritable path.
    explicit DiagnosticsLog(const std::filesystem::path& path);

    /// Write one row.
    ///
    /// The first row measured fixes the reference the energy error is relative
    /// to, which is the initial state when a run starts and the state resumed
    /// from when it is resumed. That difference is real and is why the step
    /// number is in the file: an energy error column from a resumed run is
    /// relative to where that segment began, and a reader joining two segments
    /// has the information needed to notice.
    void record(core::Index step, core::Real time, const core::Diagnostics& diagnostics);

    /// The total energy of the first row, if there has been one.
    [[nodiscard]] std::optional<core::Real> reference_energy() const noexcept {
        return reference_energy_;
    }

    [[nodiscard]] bool ok() const { return file_.good(); }

    void flush() { file_.flush(); }

private:
    std::ofstream file_;
    std::optional<core::Real> reference_energy_;
};

} // namespace orrery::sim
