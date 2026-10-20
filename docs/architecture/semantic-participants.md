# Private semantic participants

The semantic transport distinguishes an actual script VM from an original retained native module. Both use the existing Base bridge, copied-value pool and task path. Participant identity describes a lifetime; it does not grant permission to execute an operation.

`SemanticParticipantKey` is a copied tagged identity. Its script projection comes only from the real scheduler-issued VM generation. Its native projection comes from the loader's successfully prepared, retained State, including the original image, owner, scope and incarnation. Neither type has a public scalar identity constructor. A native module does not receive a fabricated VM generation.

`SemanticParticipant` retains the original native participant when necessary. The outer `ProviderParticipant` retains either the actual `ScriptRuntime` or that native State. An endpoint does not acquire strong ownership of its script VM; the native State does not own its provider or endpoint. This avoids cycles while keeping original resources alive through their established cleanup.

## Binding and lock boundaries

Binding capture stages original strong ownership outside host guards. Under the original bridge lock it captures one complete tuple: participant key, scope and authority, Base epoch and generation, storage owner and endpoint. Requests compare that complete original binding under the same bridge lock as queue admission. A historical copied identity cannot replace a current grant, context or owner check.

Native scope and lifecycle observations happen outside the bridge guard. The local atomic native retirement word is deliberately narrower: external scope or lifecycle stop can invalidate the participant while that local word remains live. No caller may treat it as a complete permission or quiescence proof. Existing task and commit fences remain mandatory.

Queue transfers declare an initially empty retired holder before taking the bridge lock. Under that lock they move the previously populated output into the holder, then transfer the queued item. The retired holder is destroyed after the lock is released. Expired weak endpoint registrations use the same final-disposal ordering. Strong-owner acquisition, allocation and final resource destruction must not be introduced beneath bridge or ordered publication guards.

The existing endpoint, queue and copied-value limits remain in force. Charges use the actual enlarged representations through `sizeof`; adding a participant kind does not create a second pool or increase admission limits. Exhaustion and failed admission must leave prior counters, original tasks and retained values unchanged.

## Current activation boundary

The native adapter remains closed. It can capture the actual original identity and lifetime, but it has no qualified operation origin, verified agent session, factual context producer or installed production Host facet. Native Request/Bind/Invoke/dispatch cannot reserve a task, operation serial or queue entry through this seam. A closed Base bridge or stale binding remains a stale rejection; an otherwise unqualified native operation reports NOT_READY.

Real native operation authority, authenticated actor/context sources and canonical manager membership are subsequent migration work. None may be simulated with an ordinary-script origin, caller-supplied session, boolean approval or replacement authority store. This internal foundation is not a declaration that native MCP tools or native mod permissions are operational.

## Verification

The ordinary `simoder-semantic-participant-tests` target uses actual Base and script VMs. It checks identity substitution, aliased binding outputs, original task delivery, off-lock final scope disposal, endpoint exhaustion and owner-pool rollback. Configure the usual explicit Base and SDK profile to include it with the existing Core regressions.

The same private test source contains separately compiled native scenarios using a successfully prepared synthetic DLL and the original TESTHOST loader. Their isolated qualification covers original and successor identity, wrong owner/session/lifecycle substitution, unchanged closed-path counters and original stop sources. That fixture graph is distinct from production. Its private observation helpers must be absent from the production objects, archives and DLL.

Tests and synthetic loading do not establish actual game context, native-code isolation, authenticated MCP interoperability or installed-game acceptance. Exact builds, run manifests, remaining coverage limits and independent audits belong in ignored `.analysis/` evidence.
