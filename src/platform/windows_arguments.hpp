#pragma once

#include <string>
#include <string_view>

namespace sc13::platform {
/** @summary Encode one Windows CRT argument, preserving quotes and trailing backslashes.
 * @param value Argument data without embedded NUL characters.
 * @return Quoted argument suitable for CreateProcessW with an explicit application path.
 * @throws std::invalid_argument When the argument contains an embedded NUL.
 */
[[nodiscard]] std::wstring QuoteWindowsArgument(std::wstring_view value);
}
