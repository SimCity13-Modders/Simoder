"""@summary Run the production Luau/PROP fixture host from a directory without compiler helpers."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    """@summary Copy only the statically linked test host and require successful script execution."""
    with tempfile.TemporaryDirectory(prefix="simoder-embedded-smoke-") as temporary:
        executable = Path(temporary) / "script-fixture.exe"
        shutil.copyfile(sys.argv[1], executable)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=90)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
