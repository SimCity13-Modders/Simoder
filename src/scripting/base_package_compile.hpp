/** @file @summary Reusable strict Base captured-byte compiler; input and output are data, never runtime authority. */
#pragma once
#include "scripting/script_runtime.hpp"
#include <map>
#include <string>
namespace sc13::scripting {
struct BasePackagePin;
namespace detail {
/** @summary Refuse noncanonical Base executable names without filesystem effects or ownership claims. */
void ValidateBaseSourceName(std::string_view);
}
/** @summary Verify every captured source against independent native pins and compile the same immutable bytes.
 * @remarks No file opens, VM, readiness, permission, compiled release selection or witness issuance occurs.
 * Caller holds input immutable through synchronous return. Exactly pinned executable files only; notice
 * must be independently verified by the complete package layer and excluded before this executable API.
 * Limits preserve existing BaseHost source4MiB/bytecode8MiB/64files, manifest memory8MiB and callback2ms.
 * @throws std::runtime_error Any source/pin/manifest/bytecode disagreement; no partial result is returned. */
[[nodiscard]] ScriptDefinition CompileCapturedBasePackage(const std::map<std::string,std::string>& captured,const BasePackagePin& pin);
}
