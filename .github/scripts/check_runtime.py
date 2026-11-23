"""@summary Validate the staged Windows x86 runtime distribution."""

import hashlib
from pathlib import Path
import struct
import sys


def verify(root: Path) -> str:
    """@summary Require the single-DLL package layout and verify PE32 and its checksum.

    @param root Fresh release staging directory containing `d3d9.dll` and legal files.
    @return SHA-256 digest of the production DLL.
    """
    root = Path(root)
    required = {
        "d3d9.dll",
        "licenses/LICENSE.txt",
        "licenses/THIRD_PARTY_NOTICES.md",
        "licenses/dependencies/THIRD_PARTY_LICENSES.txt",
    }
    files = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}
    files.discard("SHA256SUMS.txt")
    unexpected = {name for name in files if name not in required and not name.startswith("licenses/dependencies/")}
    missing = required - files
    if missing or unexpected:
        raise ValueError(f"Unexpected staging: missing={sorted(missing)}, extra={sorted(unexpected)}")
    if not any(name.startswith("licenses/dependencies/") for name in files - required):
        raise ValueError("Dependency license bundle is empty")
    for name in required:
        if not (root / name).stat().st_size:
            raise ValueError(f"Empty distribution file: {name}")

    data = (root / "d3d9.dll").read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Runtime is not a PE image")
    offset = struct.unpack_from("<I", data, 60)[0]
    if offset + 26 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError("Invalid PE header")
    machine = struct.unpack_from("<H", data, offset + 4)[0]
    flags = struct.unpack_from("<H", data, offset + 22)[0]
    magic = struct.unpack_from("<H", data, offset + 24)[0]
    if machine != 0x14C or magic != 0x10B or not flags & 0x2000:
        raise ValueError("Runtime must be an x86 PE32 DLL")

    digest = hashlib.sha256(data).hexdigest()
    checksum = root / "SHA256SUMS.txt"
    if checksum.exists() and checksum.read_text(encoding="ascii") != f"{digest}  d3d9.dll\n":
        raise ValueError("Recorded checksum does not match the runtime DLL")
    return digest


def main() -> int:
    """@summary Validate staging and record the DLL checksum after validation succeeds."""
    root = Path(sys.argv[1])
    try:
        digest = verify(root)
        (root / "SHA256SUMS.txt").write_text(f"{digest}  d3d9.dll\n", encoding="ascii")
        print(f"Validated one x86 DLL and dependency licenses: {digest}")
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"Distribution rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
