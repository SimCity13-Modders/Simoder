"""@summary Validate the production staging boundary and record the tested DLL checksum."""

import hashlib
from pathlib import Path
import struct
import sys


def verify(root):
    """@summary Require one x86 DLL and complete legal notices without developer payloads.
    @param root Fresh CMake installation staging directory.
    @return SHA-256 digest of the production DLL.
    """
    root = Path(root)
    expected = {"d3d9.dll", "licenses/LICENSE", "licenses/THIRD_PARTY_NOTICES.md",
                "licenses/luau-LICENSE.txt", "licenses/minhook-LICENSE.txt",
                "licenses/imgui-LICENSE.txt", "licenses/tomlplusplus-LICENSE.txt"}
    files = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}
    files.discard("SHA256SUMS.txt")
    if files != expected:
        raise ValueError(f"Unexpected staging: missing={sorted(expected-files)}, extra={sorted(files-expected)}")
    for name in expected:
        if not (root / name).stat().st_size:
            raise ValueError(f"Empty distribution file: {name}")
    data = (root / "d3d9.dll").read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Runtime is not a PE image")
    offset = struct.unpack_from("<I", data, 60)[0]
    if offset + 26 > len(data) or data[offset:offset+4] != b"PE\0\0":
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


def main():
    """@summary Write a checksum only after the complete staging layout passes validation."""
    root = Path(sys.argv[1])
    try:
        digest = verify(root)
        (root / "SHA256SUMS.txt").write_text(f"{digest}  d3d9.dll\n", encoding="ascii")
        print(f"Validated one x86 DLL and legal notices: {digest}")
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"Distribution rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
