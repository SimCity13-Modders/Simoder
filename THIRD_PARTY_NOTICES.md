# Third-party notices

Simoder's own source is distributed under the MIT License in [`LICENSE`](LICENSE).
The release license archive also contains the upstream license and notice files
for every package resolved by [`Cargo.lock`](Cargo.lock). `Cargo.lock` is the
authoritative version list; the package license expressions are recorded in
`licenses/dependencies/THIRD_PARTY_LICENSES.txt` inside the release archive.

## Runtime dependencies

| Component | Use | License expression |
| --- | --- | --- |
| `dbpf`, `sc-properties`, `sc-format`, `rw4` from [Simoder-OpenSCP](https://github.com/FluffyChi-Xing/fluffy-open-scp) | SimCity package, property and RW4 codecs; pinned at one Git revision in `Cargo.toml` | MIT |
| `mlua` and `luau0-src` | Embedded Luau VM and compiler | MIT |
| `rquickjs` and `rquickjs-sys` | Embedded JavaScript ES2023 VM | MIT |
| `goblin` | PE32 executable and DLL inspection | MIT |
| `gltf` | glTF data handling | MIT OR Apache-2.0 |
| `flate2`, `png`, `serde`, `serde_json`, `serde_toon`, `toml`, `thiserror`, `sha2`, `hex`, `parking_lot`, `relative-path`, `strum`, `walkdir` | Compression, formats, state, hashing, synchronization and runtime support | See each locked package's SPDX license expression in the release index |

The release includes static Rust dependencies in `d3d9.dll` and packages their
license/notice files under `licenses/dependencies/<package>-<version>/`. Build
and test dependencies resolved by Cargo are included too. Windows system APIs
are imported from the operating system and are not redistributed by Simoder.

The repository does not contain SimCity executables, game package files or game
assets. Their owners' terms remain applicable to those materials.
