#pragma once

#include <Windows.h>
#include "bootstrap/install_context.hpp"

namespace sc13::bootstrap {

/** Records the DLL module handle without performing work under the loader lock. */
void SetModuleHandle(HMODULE module) noexcept;

/** @summary Initialize the integrated runtime outside the Windows loader lock. */
DWORD Initialize(const InstallContext& context) noexcept;

/** @summary Stop hooks and services before releasing high-level runtime state. */
DWORD Shutdown() noexcept;

}
