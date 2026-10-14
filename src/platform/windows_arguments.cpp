#include "platform/windows_arguments.hpp"
#include <stdexcept>

namespace sc13::platform {
std::wstring QuoteWindowsArgument(std::wstring_view value) {
    if (value.find(L'\0') != std::wstring_view::npos)
        throw std::invalid_argument("Windows process argument contains an embedded NUL");
    std::wstring result = L"\"";
    std::size_t slashes{};
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (character == L'"') result += L'\\';
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
}
