#include "dev/session/native_bindings_status.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
/** @summary Compare actual production serialization with one complete expected result.
 * @param packet Serialized request supplied to the production helper.
 * @param source Existing router reply, including arbitrary world data.
 * @param expected Exact required resulting bytes.
 */
void Check(std::string_view packet, std::string source, std::string_view expected) {
    sc13::dev::session::AppendNativeBindingsStatus(packet, source);
    if (source != expected) throw std::runtime_error("Native bindings result serialization differs from its contract.");
}
}

/** @summary Exercise the actual production unavailable-status helper without Windows or game memory.
 * @return Zero only when successful/error/non-world/short and existing-data behavior is preserved.
 */
int main() {
    try {
        constexpr auto world = "{\"op\":\"world-units\"}";
        Check(world, "{\"ok\":true,\"result\":{\"units\":[{\"id\":7,\"position\":[1,2],\"road\":true}],\"count\":1}}",
            "{\"ok\":true,\"result\":{\"units\":[{\"id\":7,\"position\":[1,2],\"road\":true}],\"count\":1,\"nativeBindings\":{\"available\":false}}}");
        Check(world, "{\"ok\":true,\"result\":{\"count\":0}}",
            "{\"ok\":true,\"result\":{\"count\":0,\"nativeBindings\":{\"available\":false}}}");
        Check(world, "{\"ok\":false,\"error\":{\"code\":\"unknown\"}}", "{\"ok\":false,\"error\":{\"code\":\"unknown\"}}");
        Check("{\"op\":\"world-unit\"}", "{\"ok\":true,\"result\":{}}", "{\"ok\":true,\"result\":{}}");
        Check("{\"op\":\"observe\"}", "{\"ok\":true,\"result\":{}}", "{\"ok\":true,\"result\":{}}");
        Check(world, "", "");
        Check(world, "}", "}");
        Check(world, "{\"ok\":true,\"result\":[]}", "{\"ok\":true,\"result\":[]}");
        Check(world, "{\"ok\":true,\"result\":{}}\n", "{\"ok\":true,\"result\":{}}\n");
        Check("{\"op\": \"world-units\"}", "{\"ok\":true,\"result\":{}}", "{\"ok\":true,\"result\":{}}");
        std::cout << "PASS: 10 actual production native bindings status cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
