"""@summary Collect dependency license and notice files from the Cargo lock graph."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


LICENSE_PREFIXES = ("LICENSE", "LICENCE", "COPYING", "NOTICE", "COPYRIGHT")


def license_files(package: dict) -> list[Path]:
    """@summary Find package license files, including a git-workspace license at its root.

    @param package Package record returned by `cargo metadata`.
    @return Sorted existing license files or an empty list when none are present.
    """
    manifest = Path(package["manifest_path"]).resolve()
    package_root = manifest.parent
    explicit = package.get("license_file")
    if explicit:
        path = (package_root / explicit).resolve()
        if path.is_file():
            return [path]

    for root in (package_root, *package_root.parents[:6]):
        try:
            candidates = [
                path for path in root.iterdir()
                if path.is_file() and path.name.upper().startswith(LICENSE_PREFIXES)
            ]
        except OSError:
            continue
        if candidates:
            return sorted(candidates, key=lambda path: path.name.casefold())

    if package.get("license"):
        readme = package_root / "README.md"
        if readme.is_file():
            return [readme]
    return []


def safe_segment(value: str) -> str:
    """@summary Convert package identifiers to safe deterministic directory names."""
    return re.sub(r"[^A-Za-z0-9._+-]", "_", value)


def collect(destination: Path) -> int:
    """@summary Copy resolved Cargo package license files and write a provenance index.

    @param destination Release staging `licenses/dependencies` directory.
    @return Number of dependency packages whose notices were collected.
    """
    cargo = os.environ.get("CARGO", "cargo")
    result = subprocess.run(
        [cargo, "metadata", "--format-version", "1", "--locked"],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    packages = json.loads(result.stdout)["packages"]
    external = sorted(
        (package for package in packages if package.get("source")),
        key=lambda package: (package["name"].casefold(), package["version"]),
    )
    destination.mkdir(parents=True, exist_ok=True)
    index_lines = [
        "Cargo.lock package license files",
        "Package versions and license expressions are resolved from Cargo.lock.",
        "",
    ]
    copied = 0
    for package in external:
        files = license_files(package)
        if not files:
            raise ValueError(f"No license file found for {package['name']} {package['version']}")
        folder = destination / f"{safe_segment(package['name'])}-{safe_segment(package['version'])}"
        folder.mkdir(parents=True, exist_ok=True)
        for source in files:
            shutil.copyfile(source, folder / source.name)
        index_lines.append(
            f"{package['name']} {package['version']} — {package.get('license') or 'license metadata not declared'}"
        )
        copied += 1

    (destination / "THIRD_PARTY_LICENSES.txt").write_text("\n".join(index_lines) + "\n", encoding="utf-8")
    return copied


def main() -> int:
    """@summary Collect all resolved dependency licenses into the release staging folder."""
    try:
        count = collect(Path(sys.argv[1]))
        print(f"Collected license files for {count} Cargo packages")
        return 0
    except (OSError, ValueError, TypeError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"Dependency license collection failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
