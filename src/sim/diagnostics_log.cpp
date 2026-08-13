#include "orrery/sim/diagnostics_log.hpp"

#include <filesystem>
#include <limits>
#include <locale>
#include <stdexcept>

#include "orrery/core/diagnostics.hpp"
#include "orrery/core/types.hpp"

namespace orrery::sim {

DiagnosticsLog::DiagnosticsLog(const std::filesystem::path& path) : file_(path) {
    if (!file_) {
        throw std::runtime_error(path.string() + ": could not be opened for writing");
    }

    // The classic locale, so that a machine configured for a language whose
    // decimal separator is a comma does not write a CSV file in which every
    // number is two fields.
    file_.imbue(std::locale::classic());
    file_.precision(std::numeric_limits<core::Real>::max_digits10);

    file_ << "step,time,kinetic_energy,potential_energy,total_energy,relative_energy_error,"
             "virial_ratio,momentum_x,momentum_y,momentum_z,angular_momentum_x,angular_momentum_y,"
             "angular_momentum_z\n";
}

void DiagnosticsLog::record(core::Index step, core::Real time,
                            const core::Diagnostics& diagnostics) {
    if (!file_.good()) {
        return;
    }

    const core::Real energy = diagnostics.total_energy();
    if (!reference_energy_) {
        reference_energy_ = energy;
    }

    const core::Real error = core::relative_energy_error(*reference_energy_, energy);

    file_ << step << ',' << time << ',' << diagnostics.kinetic_energy << ','
          << diagnostics.potential_energy << ',' << energy << ',' << error << ','
          << diagnostics.virial_ratio() << ',' << diagnostics.linear_momentum.x << ','
          << diagnostics.linear_momentum.y << ',' << diagnostics.linear_momentum.z << ','
          << diagnostics.angular_momentum.x << ',' << diagnostics.angular_momentum.y << ','
          << diagnostics.angular_momentum.z << '\n';
}

} // namespace orrery::sim
