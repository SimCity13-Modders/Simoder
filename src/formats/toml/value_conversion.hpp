#pragma once
#include "formats/toon/toon_document.hpp"
#include "../../../third_party/tomlplusplus/toml.hpp"

namespace sc13::formats::toml_value {
/** @summary Convert bounded TOML configuration into the shared JSON-shaped value model.
 * @param node Decoded configuration node; dates and inexact integer identities are rejected.
 * @return A value retaining strings, finite numbers, arrays and objects.
 * @throws std::runtime_error Unsupported values or exceeded depth/node limits.
 */
[[nodiscard]] toon::Value Convert(const toml::node& node);
}
