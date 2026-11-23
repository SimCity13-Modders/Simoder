# Runtime builds and releases

The CI workflow builds the integrated Windows x86 runtime and synthetic tests on
Windows 2022. It runs the full runtime CTest suite, installs into a fresh staging
directory, verifies exact distribution contents and records the DLL SHA-256.
It does not install Node.js or ship authoring workers, examples or developer services.

The release workflow runs on pushed `vMAJOR.MINOR.PATCH` tags, with an optional
prerelease suffix. It invokes the same CI workflow, downloads the tested artifact
from that exact workflow run, rechecks its checksum/layout and publishes:

- `d3d9.dll`: the only game installation binary.
- `SHA256SUMS.txt`: SHA-256 for that DLL.
- `licenses.zip`: dependency licenses and notices, retained with the distribution.

Only the publishing job receives repository write permission. The workflow
requires an existing remote tag rather than creating one. Prerelease tags produce
prereleases. Existing releases are not overwritten; after a failed upload inspect
any draft before retrying. No workflow step installs the DLL into a user's game.

Publishing requires explicitly committing and pushing the intended changes/tag.
Preparing these workflow files locally does not publish anything. A successful
GitHub run must be verified separately from local builds and live-game acceptance.

GitHub CLI behavior is documented in [release create](https://cli.github.com/manual/gh_release_create).
