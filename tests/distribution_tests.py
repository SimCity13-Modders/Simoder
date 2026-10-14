"""@summary Exercise distribution rejection using the real locally built staged runtime."""

import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile

spec = importlib.util.spec_from_file_location("runtime_distribution", Path(__file__).parents[1] / "tools/distribution/check_runtime.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def rejected(root, expected):
    """@summary Require the distribution gate to report the specific boundary violation."""
    try:
        module.verify(root)
    except ValueError as error:
        if expected not in str(error):
            raise AssertionError(str(error)) from error
        return
    raise AssertionError("Invalid distribution was accepted")


def main():
    """@summary Preserve original staging while checking tampering, missing notices and extra binaries."""
    with tempfile.TemporaryDirectory(prefix="simoder-distribution-tests-") as temporary:
        root = Path(temporary) / "stage"
        shutil.copytree(sys.argv[1], root)
        digest = module.verify(root)
        checksum = root / "SHA256SUMS.txt"
        checksum.write_text(f"{digest}  d3d9.dll\n", encoding="ascii")
        (root / "worker.exe").write_bytes(b"unexpected")
        rejected(root, "extra=")
        (root / "worker.exe").unlink()
        notice = root / "licenses/LICENSE"
        content = notice.read_bytes()
        notice.unlink()
        rejected(root, "missing=")
        notice.write_bytes(content)
        dll = root / "d3d9.dll"
        original = dll.read_bytes()
        dll.write_bytes(original + b"modified")
        rejected(root, "checksum")
        dll.write_bytes(original)
        checksum.unlink()
        changed = bytearray(original)
        changed[:2] = b"XX"
        dll.write_bytes(changed)
        rejected(root, "PE image")
        dll.write_bytes(original)
        module.verify(root)
        print("Distribution boundary and tampering checks passed.")


if __name__ == "__main__":
    main()
