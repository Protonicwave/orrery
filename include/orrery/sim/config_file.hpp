#pragma once

/// \file
/// The declarative configuration file: a small language, defined here rather
/// than adopted from elsewhere.
///
/// The format is sections in square brackets holding `key = value` lines, with
/// `#` starting a comment on any line whose first non-blank character it is:
///
///     # A cluster of four thousand, integrated for a thousand steps.
///     [run]
///     timestep = 0.001
///     steps    = 1000
///     seed     = 20260811
///
///     [initial_conditions]
///     kind  = plummer
///     count = 4096
///
///     [solver]
///     kind      = barnes-hut
///     softening = 0.05
///
///     [output]
///     diagnostics_path   = diagnostics.csv
///     diagnostics_stride = 100
///
/// `docs/formats/configuration.md` is the specification and lists every key.
/// ADR-0031 records why this project defines a format of its own rather than
/// taking a dependency on TOML, YAML or JSON, the short version being that the
/// subset of any of them this configuration needs is a flat table of scalars,
/// and that is a hundred lines to parse and a page to specify.
///
/// ## Strictness
///
/// An unknown section, an unknown key or a value that does not parse is an
/// error naming the line it is on. Nothing is skipped and nothing is guessed.
/// The alternative, ignoring what it does not recognise, is worse here than it
/// would be in most places: a run whose `softenning` was silently dropped is not
/// a run that failed, it is a run that produced a plausible answer to a question
/// nobody asked, and there is no later point at which anyone finds out.
///
/// A key given twice is also an error rather than a last-one-wins. A file with
/// two `timestep` lines has an author who believes one thing and a parser that
/// does another.
///
/// ## Numbers
///
/// Parsed in the classic locale whatever the environment says, so that a machine
/// configured for a language whose decimal separator is a comma reads
/// `timestep = 0.001` as a thousandth rather than as one. This is the kind of
/// defect that never appears on the machine that wrote the code.

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "orrery/sim/configuration.hpp"

namespace orrery::sim {

/// A configuration file that could not be read as one.
///
/// Carries where the problem was as well as what it was, since a message that
/// says only "expected a number" is of little use against a file with sixty
/// lines in it. Configuration is a setup boundary, which is where section 4 of
/// the implementation plan permits exceptions; nothing in this class is ever
/// thrown from inside a step.
class ConfigurationError : public std::runtime_error {
public:
    /// `origin` is the file name, or whatever else names the stream for a
    /// caller that parsed something other than a file. `line` counts from one,
    /// and is zero for a complaint about the document as a whole.
    ConfigurationError(const std::string& message, std::string origin, std::size_t line);

    /// The name of what was being parsed.
    [[nodiscard]] const std::string& origin() const noexcept { return origin_; }

    /// The line the problem was on, counting from one.
    [[nodiscard]] std::size_t line() const noexcept { return line_; }

private:
    std::string origin_;
    std::size_t line_;
};

/// Read a configuration from `in`, whose name for error messages is `origin`.
///
/// Every key absent from the file keeps the default its field carries, so a
/// file states what it changes rather than everything there is. The result is
/// not checked for sense: that is `problems_with`, and keeping the two apart is
/// what lets the command line override a file's settings before either is
/// judged.
///
/// Throws `ConfigurationError`.
[[nodiscard]] Configuration parse_configuration(std::istream& in, std::string_view origin);

/// Apply `key = value` assignments on top of an existing configuration.
///
/// Each assignment names its own section, as `solver.softening = 0.05`, which
/// is the qualified key form the file format also accepts. This is what the
/// command line's `--set` uses: an override arrives with nowhere to put a
/// section heading, and inventing a second syntax for it would mean the command
/// line and the file disagreed about what a setting is called.
///
/// Overriding a setting the file had already given is the point and is allowed.
/// Giving the same setting twice in one call is not, on the same grounds as in a
/// file: whoever wrote it believes one thing and gets another.
///
/// Throws `ConfigurationError`, whose line number is the index of the offending
/// assignment counting from one.
void apply_settings(Configuration& configuration, std::span<const std::string> assignments,
                    std::string_view origin);

/// Read a configuration from a file.
///
/// Throws `ConfigurationError` if the file cannot be opened, so that a
/// mistyped path is reported in the same way and in the same place as a
/// mistyped key.
[[nodiscard]] Configuration read_configuration_file(const std::filesystem::path& path);

/// Write `configuration` in the format above.
///
/// Every key is written, including the ones left at their defaults. A file
/// produced by this function is what a run records beside its output so that
/// the settings the run actually used can be read later, and one that omitted
/// its defaults would leave a reader to look up what this version's defaults
/// were.
///
/// Parsing the result gives back an equal configuration, which is asserted by
/// test over randomised settings rather than assumed.
void write_configuration(std::ostream& out, const Configuration& configuration);

} // namespace orrery::sim
