"""The format gate covers edits while preserving generated and unrelated files."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "changed_format", Path(__file__).resolve().parents[1] / "tools/check_changed_format.py")
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)


class ChangedFormat(unittest.TestCase):
    def test_sources_and_exclusions(self):
        for name in ["OptiScaler/Config.h", "OptiScaler/shaders/dlssnr/superior/PrivateGuides.inl",
                     "OptiScaler/path with spaces/source.cpp"]:
            self.assertTrue(POLICY.eligible(name), name)
        for name in ["external/source.cpp", "OptiScaler/include/sdk.h",
                     "OptiScaler/framegen/mfgunlock/hde/hde.h", "OptiScaler/external/vendor.h",
                     "OptiScaler/shaders/dlssnr/superior/precompile/host_private_guides_Shader.h",
                     "OptiScaler/shaders/dlssnr/superior/PrivateGuides.hlsl", "tools/source.cpp"]:
            self.assertFalse(POLICY.eligible(name), name)

    def test_hunks_additions_deletions_and_generated_arrays(self):
        diff = """diff --git a/OptiScaler/a.cpp b/OptiScaler/a.cpp
+++ b/OptiScaler/a.cpp
@@ -4,0 +5,2 @@
+a
+b
@@ -8 +10 @@
-old
+new
@@ -20,2 +21,0 @@
-deleted
-deleted
+++ /dev/null
@@ -1 +1 @@
+++ b/OptiScaler/new file.inl
@@ -0,0 +1,3 @@
+++ b/OptiScaler/generated_Shader.h
@@ -0,0 +1,999 @@
"""
        self.assertEqual(POLICY.changed_ranges(diff),
                         {"OptiScaler/a.cpp": [(5, 6), (10, 10)], "OptiScaler/new file.inl": [(1, 3)]})

    def test_git_merge_base_and_working_tree_edits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def run(*args):
                return subprocess.check_output(["git", *args], cwd=root, text=True).strip()
            run("init", "-q")
            run("config", "user.name", "Format test")
            run("config", "user.email", "format-test@example.invalid")
            source = root / "OptiScaler/a.cpp"
            source.parent.mkdir()
            source.write_text("int a;\nint b;\nint c;\n", encoding="utf-8")
            run("add", ".")
            run("commit", "-qm", "base")
            base = run("rev-parse", "HEAD")
            source.write_text("int a;\nint changed;\nint c;\n", encoding="utf-8")
            run("add", ".")
            run("commit", "-qm", "edit")
            with mock.patch.object(POLICY, "ROOT", root):
                self.assertEqual(POLICY.comparison_base(base), base)
                self.assertEqual(POLICY.comparison_base("0" * 40), base)
                diff = POLICY.git("diff", "--unified=0", base, "--", "OptiScaler")
                self.assertEqual(POLICY.changed_ranges(diff), {"OptiScaler/a.cpp": [(2, 2)]})
                # An unrelated existing violation has no changed lines and is not
                # silently rewritten, while a new edit remains part of the gate.
                source.write_text("int a;\nint changed;\nint d;\n", encoding="utf-8")
                diff = POLICY.git("diff", "--unified=0", base, "--", "OptiScaler")
                self.assertEqual(POLICY.changed_ranges(diff), {"OptiScaler/a.cpp": [(2, 3)]})


if __name__ == "__main__":
    unittest.main()
