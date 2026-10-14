#include "formats/toml/value_conversion.hpp"
#include <cmath>
#include <stdexcept>

namespace sc13::formats::toml_value {
namespace {
/** @summary Bound recursive configuration conversion before allocating nested containers. */
toon::Value ConvertNode(const toml::node& node, unsigned depth, std::size_t& count) {
    if (depth > 16 || ++count > 65536) throw std::runtime_error("TOML configuration exceeds depth/node limits");
    if (const auto table = node.as_table()) {
        toon::Value::Object result;
        for (const auto& [key, child] : *table) result.emplace(std::string(key.str()), ConvertNode(child, depth + 1, count));
        return toon::Value(std::move(result));
    }
    if (const auto array = node.as_array()) {
        toon::Value::Array result;
        result.reserve(array->size());
        for (const auto& child : *array) result.push_back(ConvertNode(child, depth + 1, count));
        return toon::Value(std::move(result));
    }
    if (node.is_string()) return toon::Value(*node.value<std::string>());
    if (node.is_boolean()) return toon::Value(*node.value<bool>());
    if (node.is_integer()) {
        const auto number = *node.value<std::int64_t>();
        if (number < -9007199254740991LL || number > 9007199254740991LL)
            throw std::runtime_error("Integer cannot be represented exactly; use a canonical hexadecimal identifier string");
        return toon::Value(static_cast<double>(number));
    }
    if (node.is_floating_point()) {
        const auto number = *node.value<double>();
        if (!std::isfinite(number)) throw std::runtime_error("Patch values must be finite");
        return toon::Value(number);
    }
    throw std::runtime_error("Date/time values are not supported in mod configuration");
}
}
toon::Value Convert(const toml::node& node) { std::size_t count{}; return ConvertNode(node, 0, count); }
}
