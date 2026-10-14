"""@summary Enforce the runtime repository's tracked-file ownership boundaries."""

from pathlib import Path, PurePosixPath
import subprocess
import sys


def violations(paths):
    """@summary Reject runtime-owned examples, analyses and retired deployment entry points.
    @param paths Existing tracked repository-relative paths.
    @return Human-readable policy violations.
    """
    retired = {"Start-Simoder.sh", "src/bootstrap/launcher_main.cpp", "src/bootstrap/dll_main.cpp",
               "src/bootstrap/exports.def", "src/bootstrap/proxy/bundled_payload.cpp",
               "src/bootstrap/proxy/bundled_payload.hpp", "tools/scripts/bundle_distribution.py",
               "tests/script_worker_smoke.py"}
    errors = []
    for name in paths:
        path = PurePosixPath(name)
        parts = tuple(part.lower() for part in path.parts)
        if any(part in {".analysis", "analysis", "analyses"} for part in parts):
            errors.append(f"Analysis must remain untracked in .analysis/: {name}")
        if "examples" in parts or (parts and parts[0] == "mods"):
            errors.append(f"Example/user mods belong outside the runtime repository: {name}")
        if parts and parts[0] == "packaging" and name != "packaging/config.toon":
            errors.append(f"Runtime packaging must not contain mod or developer payloads: {name}")
        if path.suffix.lower() == ".bat" or name in retired:
            errors.append(f"Retired runtime deployment entry point: {name}")
    return errors


def main():
    """@summary Inspect tracked live files and verify the ignored analysis directory rule."""
    root = Path(__file__).resolve().parents[2]
    result = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True)
    tracked = result.stdout.decode("utf-8").split("\0")
    # @summary Pending tracked deletions are absent locally and will be absent in the final commit.
    errors = violations(name for name in tracked if name and (root / name).exists())
    ignored = subprocess.run(["git", "check-ignore", "--no-index", ".analysis/policy-probe.md"],
                             cwd=root, capture_output=True)
    if ignored.returncode:
        errors.append("Repository must ignore .analysis/")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Runtime repository ownership policy passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
