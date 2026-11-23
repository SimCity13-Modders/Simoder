# Single-DLL installation

The supported runtime is the integrated x86 `d3d9.dll` built by the
`simoder-d3d9` crate. Close SimCity before installing or updating it.

## Build and install

Build the production target from the repository root:

```powershell
cargo build --release --target i686-pc-windows-msvc -p simoder-d3d9
```

Copy `target/i686-pc-windows-msvc/release/simoder_d3d9.dll` beside the actual
`SimCity.exe`, renaming the copy to `d3d9.dll`. Launch the game normally through
EA App. Remove that copied DLL to uninstall Simoder; user data stays in place.

The proxy forwards Direct3D 9 calls and starts Simoder outside `DllMain`.
Unsupported game builds are rejected before hooks are installed, while graphics
forwarding remains available. Chaining another `d3d9.dll` proxy is not supported.

## Runtime data

The runtime creates configuration, state, logs and crash data under the game
installation's `simoder/` directory. User mods are discovered under `mods/`.
`simoder/proxy.disabled` disables runtime startup after restarting the game,
while keeping graphics forwarding active. Remove the flag and restart to enable
the runtime again.

## Evidence limits

Run the workspace Clippy and tests described in the [README](../README.md).
They cover Rust code and synthetic integration fixtures. They do not establish
compatibility with a live EA-launched game; follow the separate
[first-test checklist](first-test.md) for that acceptance.
