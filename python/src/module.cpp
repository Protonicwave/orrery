/// \file
/// The extension module, and nothing else.
///
/// What this file decides is the shape of the module rather than the content of
/// it: the two facts about the build that a caller has to be able to ask for,
/// and the order the groups of bindings are registered in. That order matters
/// in one direction only. pybind11 resolves a type by looking it up when a
/// function using it is called rather than when it is declared, so a signature
/// may name a type registered later, but a default argument is converted at
/// registration time and cannot. Registering the layers bottom up, as the
/// library itself is built, means the question never arises.

#include <string>

#include <pybind11/pybind11.h>

#include "bindings.hpp"
#include "orrery/core/build_info.hpp"

PYBIND11_MODULE(_orrery, module) {
    module.doc() = R"(The compiled half of the Orrery bindings.

Import `orrery` rather than this. What is here is the extension module, and the
package around it decides what the public surface is.)";

    // Asked of the library rather than baked into this translation unit, which
    // is the whole point of build_info.hpp: an extension compiled against
    // headers configured one way and loaded beside a library configured the
    // other would otherwise disagree about the size of every scalar it passes
    // and say nothing about it.
    module.attr("__version__") = std::string(orrery::core::version());
    module.attr("single_precision") = orrery::core::uses_single_precision();

    orrery::python::bind_core(module);
    orrery::python::bind_initial_conditions(module);
    orrery::python::bind_sim(module);
}
