# Game-owned DevExperience

Optional DevBridge and MCP services belong to a separately installed developer
service mod. Install the DevTools package under mods/simoder.devtools and enable it
explicitly in ModLoader. Its current mod.toml declares kind=developer-services,
serviceApiVersion, bounded relative executable paths, arguments and dependencies.
See [service mod contract](developer-service-mods.md).

The old aiRoot configuration is no longer an activation path. Merely installing
the package does not launch services. The production DLL does not embed or extract
developer executable payloads. DeveloperConsole settings remain independent.

The DLL creates both child processes suspended, places them in a non-inherited Windows job
with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, and then resumes them. Its job handle remains open
throughout the game session. Normal process exit, a crash, or an explicit full runtime shutdown
closes ownership and ends both services. The services also watch the supplied parent PID.
Service startup rollback closes its job and ends partially started children.
Dependency launch order is not a readiness handshake; readiness remains a separate
service protocol concern.

DevBridge inherits the game's integrity level; it does not request its own UAC prompt.
The user-only IPC endpoint accepts only the sibling MCP executable in the same Windows session.
An explicit medium integrity label and user SID owner allow a normal Codex client to connect
when EA has started the game elevated. The broker retains its allowlisted window and capture-bound
input controls. Disconnecting a client does not terminate the broker.

`Simoder.McpServer.exe --game-host --parent-pid <PID> --game-root <root>` is the persistent MCP
service launched by the DLL. Codex launches the same executable in its normal stdio mode;
this process relays JSON-RPC to the game-owned service and may reconnect without replacing it.
`simoder_get_service_info` reports the actual server PID, game PID and transport.
`simoder_set_mod_enabled(modId, enabled)` changes an exact discovered mod through the DLL's
transactional controller. It requires a fresh snapshot advertising `controlAvailable`, one
verified game process, and a matching request nonce in the DLL acknowledgement. The fixed
installation-local request is bounded, expires after thirty seconds, and cannot target another
game PID. Only enable/disable is accepted; arbitrary paths, code, addresses and commands are not.
Runtime control is offered only while an explicitly enabled developer service mod owns live services. A returned generation confirms
controller commitment, not gameplay effects. Read runtime state afterward to inspect conflicts.
`simcity_close_game` is available only for explicit user-requested normal exit. It acknowledges
a queued close before sending `WM_CLOSE` to the verified main game window, because game exit
also ends its broker and MCP processes. Save and exit confirmation remain controlled by the
game. A queued response does not prove the process exited. No forced termination or automatic
restart is performed; never use this command in response to a suspected loading hang.
Before the game runs, the bootstrap stdio endpoint can request launch through EA and read
limited disk diagnostics; it does not spawn a broker or injector.

These are developer capabilities. Keep the optional AI installation and `devExperience`
configuration out of an end-user deployment unless explicitly needed.

Tests cover actual job cleanup on explicit shutdown and parent exit, two client reconnects
to the same MCP server, parent-exit cancellation, and the existing DevBridge protocol and
window allowlists. Live game acceptance must additionally confirm the owner PID, automatic
bootstrap, a loaded city, mod effects and shutdown of both service processes.
