#pragma once
#include "core/tgi.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace sc13::scripting {
/** @summary Describe one owner-scoped absolute float override or removal after a successful callback. */
struct ScriptPatchCommand final {
    core::Tgi target{};
    std::uint32_t property{};
    /** @summary A missing value clears only this script owner's override. */
    std::optional<float> value;
    std::string propertyName;
    std::string source;
};
}
