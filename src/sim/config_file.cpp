#include "orrery/sim/config_file.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <istream>
#include <limits>
#include <locale>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "orrery/core/types.hpp"
#include "orrery/sim/configuration.hpp"

namespace orrery::sim {
namespace {

/// The characters that separate a token from its neighbours.
///
/// Carriage return is in the list so that a file written on Windows and read on
/// Linux, or fetched through a checkout that changed the line endings, parses
/// the same. Without it the last value on every line would end in an invisible
/// character and every number in the file would fail to parse for a reason
/// nothing on the screen shows.
constexpr std::string_view kBlank = " \t\r\n";

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const std::size_t first = text.find_first_not_of(kBlank);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kBlank) - first + 1);
}

/// A floating-point value, parsed in the classic locale.
///
/// A `std::istringstream` with the classic locale imbued rather than
/// `std::from_chars`, which would be the natural choice and is not available for
/// floating-point types in every standard library this project builds against.
/// The locale is set explicitly because the alternative is a parser whose
/// meaning depends on the environment of the machine running it.
[[nodiscard]] std::optional<core::Real> to_real(std::string_view text) {
    std::istringstream stream{std::string{text}};
    stream.imbue(std::locale::classic());

    core::Real value{};
    stream >> value;
    if (stream.fail()) {
        return std::nullopt;
    }

    // Anything left over is a mistake rather than a suffix to ignore: `1.0e` and
    // `3 4` should both be rejected, and a parser that stopped at the first
    // character it did not understand would accept them.
    if (!(stream >> std::ws).eof()) {
        return std::nullopt;
    }
    return value;
}

template<typename Integer> [[nodiscard]] std::optional<Integer> to_integer(std::string_view text) {
    Integer value{};

    // A pair of pointers rather than a pointer and a length, because that is
    // what `std::from_chars` takes. The end is computed first and named so that
    // it can be compared against where the conversion stopped: a partial
    // conversion, as of the `5` in `5 apples`, has to be a mistake rather than a
    // number with something after it.
    const char* const first = text.data();
    const char* const last = first + text.size();

    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<bool> to_bool(std::string_view text) noexcept {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::nullopt;
}

/// Reads the format `config_file.hpp` describes.
///
/// A class rather than a function because every diagnostic needs the current
/// line number and the name of what is being parsed, and threading those
/// through a dozen assignment helpers as parameters would drown the code that
/// does the work.
class Parser {
public:
    /// `initial` is what the settings start from, which is the defaults when a
    /// file is being read and the configuration read from the file when the
    /// command line is being applied on top of it.
    Parser(std::istream& in, std::string_view origin, Configuration initial)
        : configuration_(std::move(initial)), in_(&in), origin_(origin) {}

    [[nodiscard]] Configuration parse();

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw ConfigurationError(message, origin_, line_number_);
    }

    void check_section(std::string_view name) const;
    void begin_section(std::string_view line);
    void assign(std::string_view line);

    void assign_run(std::string_view key, std::string_view value);
    void assign_initial_conditions(std::string_view key, std::string_view value);
    void assign_solver(std::string_view key, std::string_view value);
    void assign_integrator(std::string_view key, std::string_view value);
    void assign_output(std::string_view key, std::string_view value);

    /// The value as a `Real`, or a diagnostic naming the key.
    [[nodiscard]] core::Real real(std::string_view key, std::string_view value) const {
        const std::optional<core::Real> parsed = to_real(value);
        if (!parsed) {
            fail(std::string{key} + ": expected a number, found '" + std::string{value} + "'");
        }
        return *parsed;
    }

    [[nodiscard]] core::Index index(std::string_view key, std::string_view value) const {
        const std::optional<core::Index> parsed = to_integer<core::Index>(value);
        if (!parsed) {
            fail(std::string{key} + ": expected a whole number that is not negative, found '" +
                 std::string{value} + "'");
        }
        return *parsed;
    }

    [[nodiscard]] std::uint64_t unsigned64(std::string_view key, std::string_view value) const {
        const std::optional<std::uint64_t> parsed = to_integer<std::uint64_t>(value);
        if (!parsed) {
            fail(std::string{key} + ": expected a whole number that is not negative, found '" +
                 std::string{value} + "'");
        }
        return *parsed;
    }

    [[nodiscard]] unsigned unsigned_value(std::string_view key, std::string_view value) const {
        const std::optional<unsigned> parsed = to_integer<unsigned>(value);
        if (!parsed) {
            fail(std::string{key} + ": expected a whole number that is not negative, found '" +
                 std::string{value} + "'");
        }
        return *parsed;
    }

    [[nodiscard]] bool boolean(std::string_view key, std::string_view value) const {
        const std::optional<bool> parsed = to_bool(value);
        if (!parsed) {
            fail(std::string{key} + ": expected true or false, found '" + std::string{value} + "'");
        }
        return *parsed;
    }

    Configuration configuration_;
    std::istream* in_;
    std::string origin_;
    std::string section_;
    std::size_t line_number_ = 0;
    std::set<std::string> assigned_;
};

Configuration Parser::parse() {
    std::string line;
    while (std::getline(*in_, line)) {
        ++line_number_;

        const std::string_view content = trim(line);
        if (content.empty() || content.front() == '#') {
            // A comment is a whole line rather than a trailing field, so that a
            // path containing a hash is a path and not half a path. The
            // alternative would need a quoting rule, and a configuration format
            // with quoting in it is one that needs an escaping rule next.
            continue;
        }

        if (content.front() == '[') {
            begin_section(content);
        } else {
            assign(content);
        }
    }

    if (in_->bad()) {
        fail("could not be read");
    }
    return std::move(configuration_);
}

void Parser::check_section(std::string_view name) const {
    if (name != "run" && name != "initial_conditions" && name != "solver" && name != "integrator" &&
        name != "output") {
        fail("unknown section '" + std::string{name} +
             "'. The sections are run, initial_conditions, solver, integrator and output");
    }
}

void Parser::begin_section(std::string_view line) {
    if (line.back() != ']') {
        fail("a section heading must end with ']'");
    }

    const std::string_view name = trim(line.substr(1, line.size() - 2));
    check_section(name);
    section_ = name;
}

void Parser::assign(std::string_view line) {
    const std::size_t separator = line.find('=');
    if (separator == std::string_view::npos) {
        fail("expected 'key = value'");
    }

    std::string_view key = trim(line.substr(0, separator));
    const std::string_view value = trim(line.substr(separator + 1));
    if (key.empty()) {
        fail("expected a key before the '='");
    }
    if (value.empty()) {
        fail("'" + std::string{key} + "' has no value");
    }

    // A key may name its own section, as `solver.softening = 0.05`. That is what
    // the command line's --set uses, since an override has nowhere to put a
    // section heading, and it is accepted in a file too rather than being a
    // second syntax that exists only for the command line. The qualified form
    // does not change the current section, so it can appear inside one without
    // moving it.
    std::string section{section_};
    if (const std::size_t dot = key.rfind('.'); dot != std::string_view::npos) {
        section = key.substr(0, dot);
        key = key.substr(dot + 1);
        if (key.empty()) {
            fail("expected a setting name after the '.'");
        }
        check_section(section);
    }

    if (section.empty()) {
        fail("'" + std::string{key} +
             "' appears before any section heading, and does not name its section");
    }

    const std::string qualified = section + '.' + std::string{key};
    if (!assigned_.insert(qualified).second) {
        fail("'" + qualified + "' is set more than once");
    }

    if (section == "run") {
        assign_run(key, value);
    } else if (section == "initial_conditions") {
        assign_initial_conditions(key, value);
    } else if (section == "solver") {
        assign_solver(key, value);
    } else if (section == "integrator") {
        assign_integrator(key, value);
    } else {
        assign_output(key, value);
    }
}

void Parser::assign_run(std::string_view key, std::string_view value) {
    RunSettings& run = configuration_.run;
    if (key == "timestep") {
        run.timestep = real(key, value);
    } else if (key == "steps") {
        run.steps = index(key, value);
    } else if (key == "seed") {
        run.seed = unsigned64(key, value);
    } else {
        fail("unknown setting 'run." + std::string{key} + "'");
    }
}

void Parser::assign_initial_conditions(std::string_view key, std::string_view value) {
    InitialConditionSettings& initial = configuration_.initial_conditions;
    if (key == "kind") {
        const std::optional<InitialConditionKind> kind = parse_initial_condition_kind(value);
        if (!kind) {
            fail("unknown initial_conditions.kind '" + std::string{value} + "'. The choices are " +
                 initial_condition_kind_names());
        }
        initial.kind = *kind;
    } else if (key == "count") {
        initial.count = index(key, value);
    } else if (key == "total_mass") {
        initial.total_mass = real(key, value);
    } else if (key == "scale_radius") {
        initial.scale_radius = real(key, value);
    } else if (key == "radius") {
        initial.radius = real(key, value);
    } else if (key == "mass_fraction_cutoff") {
        initial.mass_fraction_cutoff = real(key, value);
    } else if (key == "primary_mass") {
        initial.primary_mass = real(key, value);
    } else if (key == "secondary_mass") {
        initial.secondary_mass = real(key, value);
    } else if (key == "semi_major_axis") {
        initial.semi_major_axis = real(key, value);
    } else if (key == "eccentricity") {
        initial.eccentricity = real(key, value);
    } else if (key == "bulge_fraction") {
        initial.bulge_fraction = real(key, value);
    } else if (key == "scale_length") {
        initial.scale_length = real(key, value);
    } else if (key == "scale_height") {
        initial.scale_height = real(key, value);
    } else if (key == "bulge_radius") {
        initial.bulge_radius = real(key, value);
    } else if (key == "inclination") {
        initial.inclination = real(key, value);
    } else if (key == "position_angle") {
        initial.position_angle = real(key, value);
    } else if (key == "mass_ratio") {
        initial.mass_ratio = real(key, value);
    } else if (key == "secondary_inclination") {
        initial.secondary_inclination = real(key, value);
    } else if (key == "secondary_position_angle") {
        initial.secondary_position_angle = real(key, value);
    } else if (key == "separation") {
        initial.separation = real(key, value);
    } else if (key == "impact_parameter") {
        initial.impact_parameter = real(key, value);
    } else if (key == "approach_speed") {
        initial.approach_speed = real(key, value);
    } else {
        fail("unknown setting 'initial_conditions." + std::string{key} + "'");
    }
}

void Parser::assign_solver(std::string_view key, std::string_view value) {
    SolverSettings& solver = configuration_.solver;
    if (key == "kind") {
        const std::optional<SolverKind> kind = parse_solver_kind(value);
        if (!kind) {
            fail("unknown solver.kind '" + std::string{value} + "'. The choices are " +
                 solver_kind_names());
        }
        solver.kind = *kind;
    } else if (key == "softening") {
        solver.softening = real(key, value);
    } else if (key == "opening_angle") {
        solver.opening_angle = real(key, value);
    } else if (key == "leaf_capacity") {
        solver.leaf_capacity = index(key, value);
    } else if (key == "quadrupole") {
        solver.quadrupole = boolean(key, value);
    } else if (key == "executor") {
        const std::optional<ExecutorKind> executor = parse_executor_kind(value);
        if (!executor) {
            fail("unknown solver.executor '" + std::string{value} + "'. The choices are " +
                 executor_kind_names());
        }
        solver.executor = *executor;
    } else if (key == "threads") {
        solver.threads = unsigned_value(key, value);
    } else if (key == "allow_cpu_fallback") {
        solver.allow_cpu_fallback = boolean(key, value);
    } else {
        fail("unknown setting 'solver." + std::string{key} + "'");
    }
}

void Parser::assign_integrator(std::string_view key, std::string_view value) {
    if (key != "kind") {
        fail("unknown setting 'integrator." + std::string{key} + "'");
    }

    const std::optional<IntegratorKind> kind = parse_integrator_kind(value);
    if (!kind) {
        fail("unknown integrator.kind '" + std::string{value} + "'. The choices are " +
             integrator_kind_names());
    }
    configuration_.integrator.kind = *kind;
}

void Parser::assign_output(std::string_view key, std::string_view value) {
    OutputSettings& output = configuration_.output;
    if (key == "trajectory_path") {
        output.trajectory_path = value;
    } else if (key == "trajectory_stride") {
        output.trajectory_stride = index(key, value);
    } else if (key == "trajectory_velocities") {
        output.trajectory_velocities = boolean(key, value);
    } else if (key == "diagnostics_path") {
        output.diagnostics_path = value;
    } else if (key == "diagnostics_stride") {
        output.diagnostics_stride = index(key, value);
    } else if (key == "checkpoint_path") {
        output.checkpoint_path = value;
    } else if (key == "checkpoint_stride") {
        output.checkpoint_stride = index(key, value);
    } else {
        fail("unknown setting 'output." + std::string{key} + "'");
    }
}

} // namespace

ConfigurationError::ConfigurationError(const std::string& message, std::string origin,
                                       std::size_t line)
    : std::runtime_error(line == 0 ? origin + ": " + message
                                   : origin + ':' + std::to_string(line) + ": " + message),
      origin_(std::move(origin)),
      line_(line) {}

Configuration parse_configuration(std::istream& in, std::string_view origin) {
    Parser parser(in, origin, Configuration{});
    return parser.parse();
}

void apply_settings(Configuration& configuration, std::span<const std::string> assignments,
                    std::string_view origin) {
    // One assignment per line, so that the line number in any complaint is the
    // index of the assignment that caused it. Every assignment names its own
    // section, which is what the qualified key form exists for.
    std::string text;
    for (const std::string& assignment : assignments) {
        text += assignment;
        text += '\n';
    }

    std::istringstream stream(text);
    Parser parser(stream, origin, std::move(configuration));
    configuration = parser.parse();
}

Configuration read_configuration_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw ConfigurationError("could not be opened", path.string(), 0);
    }
    return parse_configuration(file, path.string());
}

void write_configuration(std::ostream& out, const Configuration& configuration) {
    const auto& initial = configuration.initial_conditions;
    const auto& solver = configuration.solver;
    const auto& output = configuration.output;

    // The classic locale for the same reason the parser reads in it. A file
    // written with a comma for a decimal point would not be readable by the
    // parser in this same file, which is the one property a written
    // configuration has to have.
    out.imbue(std::locale::classic());

    // Enough digits to recover the value exactly. A timestep written to six
    // figures and read back is a different timestep, and a run resumed from a
    // configuration that had been through this function would then diverge from
    // one that had not.
    out.precision(std::numeric_limits<core::Real>::max_digits10);

    out << "[run]\n"
        << "timestep = " << configuration.run.timestep << '\n'
        << "steps = " << configuration.run.steps << '\n'
        << "seed = " << configuration.run.seed << "\n\n";

    out << "[initial_conditions]\n"
        << "kind = " << to_string(initial.kind) << '\n'
        << "count = " << initial.count << '\n'
        << "total_mass = " << initial.total_mass << '\n'
        << "scale_radius = " << initial.scale_radius << '\n'
        << "radius = " << initial.radius << '\n'
        << "mass_fraction_cutoff = " << initial.mass_fraction_cutoff << '\n'
        << "primary_mass = " << initial.primary_mass << '\n'
        << "secondary_mass = " << initial.secondary_mass << '\n'
        << "semi_major_axis = " << initial.semi_major_axis << '\n'
        << "eccentricity = " << initial.eccentricity << '\n'
        << "bulge_fraction = " << initial.bulge_fraction << '\n'
        << "scale_length = " << initial.scale_length << '\n'
        << "scale_height = " << initial.scale_height << '\n'
        << "bulge_radius = " << initial.bulge_radius << '\n'
        << "inclination = " << initial.inclination << '\n'
        << "position_angle = " << initial.position_angle << '\n'
        << "mass_ratio = " << initial.mass_ratio << '\n'
        << "secondary_inclination = " << initial.secondary_inclination << '\n'
        << "secondary_position_angle = " << initial.secondary_position_angle << '\n'
        << "separation = " << initial.separation << '\n'
        << "impact_parameter = " << initial.impact_parameter << '\n'
        << "approach_speed = " << initial.approach_speed << "\n\n";

    out << "[solver]\n"
        << "kind = " << to_string(solver.kind) << '\n'
        << "softening = " << solver.softening << '\n'
        << "opening_angle = " << solver.opening_angle << '\n'
        << "leaf_capacity = " << solver.leaf_capacity << '\n'
        << "quadrupole = " << (solver.quadrupole ? "true" : "false") << '\n'
        << "executor = " << to_string(solver.executor) << '\n'
        << "threads = " << solver.threads << '\n'
        << "allow_cpu_fallback = " << (solver.allow_cpu_fallback ? "true" : "false") << "\n\n";

    out << "[integrator]\n"
        << "kind = " << to_string(configuration.integrator.kind) << "\n\n";

    // The paths are written only when they are set. An empty value is not a
    // legal line in this format, since a key with nothing after the '=' is
    // rejected by the parser as a mistake, and it would be one.
    out << "[output]\n";
    if (!output.trajectory_path.empty()) {
        out << "trajectory_path = " << output.trajectory_path << '\n';
    }
    out << "trajectory_stride = " << output.trajectory_stride << '\n'
        << "trajectory_velocities = " << (output.trajectory_velocities ? "true" : "false") << '\n';
    if (!output.diagnostics_path.empty()) {
        out << "diagnostics_path = " << output.diagnostics_path << '\n';
    }
    out << "diagnostics_stride = " << output.diagnostics_stride << '\n';
    if (!output.checkpoint_path.empty()) {
        out << "checkpoint_path = " << output.checkpoint_path << '\n';
    }
    out << "checkpoint_stride = " << output.checkpoint_stride << '\n';
}

} // namespace orrery::sim
