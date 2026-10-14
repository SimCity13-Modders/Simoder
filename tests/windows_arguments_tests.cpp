#include "platform/windows_arguments.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <array>
#include <iostream>
#include <stdexcept>

/** @summary Verify argument boundaries through the operating-system command-line decoder. */
int main() {
    try {
        const std::array<std::wstring, 8> values{L"", L"plain", L"space separated", L"quote\"inside",
            L"C:\\directory with spaces\\", L"\\\\\"\\", L"# & % ()", L"Unicode \u017c\u00f3\u0142\u0107"};
        std::wstring command = L"fixture.exe";
        for (const auto& value : values) command += L" " + sc13::platform::QuoteWindowsArgument(value);
        int count{};
        auto arguments = CommandLineToArgvW(command.c_str(), &count);
        if (!arguments) throw std::runtime_error("Windows could not decode the command line");
        bool valid = count == static_cast<int>(values.size() + 1);
        if (valid) for (std::size_t index = 0; index < values.size(); ++index)
            valid = valid && values[index] == arguments[index + 1];
        LocalFree(arguments);
        if (!valid) throw std::runtime_error("Argument content or boundaries changed");
        bool rejected{};
        try { static_cast<void>(sc13::platform::QuoteWindowsArgument(std::wstring_view(L"a\0b", 3))); }
        catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Embedded NUL silently truncated a process argument");
        std::cout << "Windows argument boundaries and NUL rejection passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
