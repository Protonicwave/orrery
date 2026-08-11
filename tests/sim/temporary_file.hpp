#pragma once

/// \file
/// A file that removes itself, for the tests of the two binary formats.
///
/// The formats in `sim/` are about bytes on a disc, so testing them without one
/// would be testing something else: a writer and a reader that agreed with each
/// other through a string stream could still disagree about what reaches a file.
/// What this provides is that a failing test does not leave a file behind for
/// the next run to find, which would turn one failure into a series of
/// unrelated ones.
///
/// The name carries the process identifier so that two test executables run at
/// once, which is what `ctest -j` does, do not write each other's files.

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#    include <process.h>
#else
#    include <unistd.h>
#endif

namespace orrery::sim::testing {

/// The identifier of the running process, for a name no other test shares.
[[nodiscard]] inline std::uint64_t process_identifier() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

/// A path in the system temporary directory, with nothing at it.
class TemporaryFile {
public:
    explicit TemporaryFile(const std::string& name)
        : path_(std::filesystem::temp_directory_path() /
                ("orrery-" + std::to_string(process_identifier()) + '-' + name)),
          partial_(path_.string() + ".partial") {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(partial_, ignored);
    }

    /// Both paths are formed in the constructor and only removed here, because
    /// a destructor may not throw and forming a path allocates.
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);

        // The checkpoint writer writes here first and renames, so a test that
        // fails partway through can leave one of these behind.
        std::filesystem::remove(partial_, ignored);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// The path as a string, for the configuration fields that hold one.
    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
    std::filesystem::path partial_;
};

} // namespace orrery::sim::testing
