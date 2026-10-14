# Stable mod input validation

`mods/mod_candidate` owns validated input acquisition. It reads a candidate only
when input fingerprints before and after parsing agree, with at most two attempts
when files change. Failure leaves lifecycle publication to the caller; the module
does not mutate activation order, persistent state or the patch registry.

The fingerprint contains one digest of current verified inputs. Authoring input
identity covers the manifest and declared sources through `InspectSourceProject`.
Raw-resource identity covers the manifest after validating the declared payload
sizes and hashes. Changing a payload without updating its declared hash fails
validation. Retired sidecar presence and separate override digests are not part
of the current revision contract.

ModManager owns lifecycle state and calls this module during discovery. UI pending
revisions and reload preconditions use the same digest. These revisions are opaque
tokens; clients must fetch the latest snapshot rather than construct tokens or
reuse tokens saved by an earlier runtime build.

This separation keeps filesystem validation and compilation independent of state
publication. It does not make filesystem inputs immutable: concurrent edits can
still invalidate a candidate and must be reported rather than partly activated.
