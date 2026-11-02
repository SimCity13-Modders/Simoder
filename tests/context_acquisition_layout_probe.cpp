/** @file @summary Compile the exact same object layout under macro-free, BASE_TEST and AUTH header profiles; no objects or authority are constructed. */
#include "bootstrap/runtime_context.hpp"
#include <array>
#include <cstddef>
#ifndef SC13_ACQUISITION_LAYOUT_SYMBOL
#error Exact private layout diagnostic symbol required
#endif
/** @summary Return compiler-observed object size/alignment only; never a runtime owner, source or permission. */
extern "C" std::array<std::size_t,4> SC13_ACQUISITION_LAYOUT_SYMBOL()noexcept{
    return {sizeof(sc13::bootstrap::BaseDependency),alignof(sc13::bootstrap::BaseDependency),
        sizeof(sc13::bootstrap::RuntimeContext),alignof(sc13::bootstrap::RuntimeContext)};
}
