#pragma once
#include <memory>

namespace sc13::mods { class ManagerCommands; class ManagerCommandRoute; struct ManagerCommandsTestAccess; struct ManagerCommandTicket; }
namespace sc13::scripting {
/** @summary Opaque one-shot owned scheduler output; textual IDs cannot construct a native ticket. */
class ScriptCommandBatch final {
public:
    /** @summary Construct an empty non-authoritative holder. */
    ScriptCommandBatch() noexcept;
    /** @summary Release only this envelope's owned reservation outside publication guards. */
    ~ScriptCommandBatch();
    /** @summary Transfer exact native ownership. @param other Original envelope. */
    ScriptCommandBatch(ScriptCommandBatch&& other) noexcept;
    /** @summary Release former storage and transfer ownership off publication guards. @param other Original envelope. @return This holder. */
    ScriptCommandBatch& operator=(ScriptCommandBatch&& other) noexcept;
    ScriptCommandBatch(const ScriptCommandBatch&)=delete;
    ScriptCommandBatch& operator=(const ScriptCommandBatch&)=delete;
private:
    friend class mods::ManagerCommands;
    friend struct mods::ManagerCommandsTestAccess;
    std::unique_ptr<mods::ManagerCommandTicket> ticket_;
};
}
