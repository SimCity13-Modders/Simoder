"""@summary Exercise repository boundary violations without touching tracked files."""

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("repository_policy", Path(__file__).parents[1] / "tools/repository/check_policy.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PolicyTests(unittest.TestCase):
    """@summary Distinguish maintained source/docs from retired deployment and mod payloads."""

    def test_supported_files(self):
        """@summary Allow maintained contracts, synthetic tests and runtime defaults."""
        self.assertEqual(module.violations(["docs/architecture.md", "tests/script_tests.cpp",
            "src/mods/mod_manager.cpp", "packaging/config.toon", "tools/distribution/check_runtime.py"]), [])

    def test_ownership_violations(self):
        """@summary Reject each explicitly prohibited ownership category."""
        for name in [".analysis/audit.md", "docs/analyses/report.md", "examples/Test/mod.toml",
                     "mods/Test/mod.toml", "packaging/Test/mod.toml", "Start-Simoder.bat",
                     "src/bootstrap/launcher_main.cpp", "tests/script_worker_smoke.py"]:
            with self.subTest(name=name):
                self.assertTrue(module.violations([name]))


if __name__ == "__main__":
    unittest.main()
