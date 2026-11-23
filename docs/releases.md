# CI and runtime releases

Simoder is a Rust 2024 Cargo workspace. The GitHub Actions workflows run on
Windows 2022, check out the matching Simoder-Base source, run Clippy with
warnings denied and execute workspace tests on the host and `i686-pc-windows-msvc`.
The Base checkout is pinned to an exact commit so integration tests use a
reproducible source tree.

The CI build produces one integrated 32-bit x86 `d3d9.dll`. Staging includes
the project MIT license, current third-party notices, and license files
collected from the locked Cargo dependency graph. A distribution check verifies
the x86 PE32 DLL, expected files, and SHA-256 checksum before upload.

The release workflow runs for `vMAJOR.MINOR.PATCH` tags, with optional prerelease
suffixes such as `-alpha.1`. It downloads the artifact from the same workflow
run, checks it again, packages legal notices, and publishes the DLL, checksum,
and license archive. Prerelease tags create GitHub prereleases. The workflow
does not install the DLL into a game directory.

See [installation](single-file-installation.md) for local setup. A successful
CI build is not evidence of live-game acceptance; record that separately in
[first-test](first-test.md).
