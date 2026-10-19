#pragma once
#include <string>
#include <string_view>

namespace sc13::dev::session {
/** @summary Append the supported unavailable diagnostic using the existing world-unit result predicate.
 * @param packetJson Original serialized operation, inspected without native memory access.
 * @param replyJson Router result to augment in place; all existing data remains unchanged.
 */
inline void AppendNativeBindingsStatus(std::string_view packetJson, std::string& replyJson) {
    if (packetJson.find("\"op\":\"world-units\"") != std::string_view::npos &&
        replyJson.size() >= 2 && replyJson.substr(replyJson.size() - 2) == "}}" &&
        replyJson.find("\"ok\":true") != std::string::npos)
        replyJson.insert(replyJson.size() - 2, ",\"nativeBindings\":{\"available\":false}");
}
}
