#pragma once
#include <Windows.h>

namespace sc13::bootstrap {
/** @summary Supply explicit game and mutable-data roots to the integrated D3D9 runtime. */
struct InstallContext final {
    DWORD size{sizeof(InstallContext)};
    DWORD version{1};
    const wchar_t* gameRoot{};
    const wchar_t* dataRoot{};
};
}
