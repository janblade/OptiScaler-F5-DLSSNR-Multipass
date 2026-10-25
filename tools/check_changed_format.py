"""Check changed C/C++ lines without unrelated upstream formatting cleanup.

Uses the PR merge base (or previous push revision). Generated shader byte arrays
are owned by generators and bytecode/rebuild checks, not a C++ formatter.
Run with --base <upstream-ref> --fix locally, then without --fix to check.
"""
import argparse
import difflib
import os
from pathlib import Path, PurePosixPath
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc"}
EXCLUDED = ("OptiScaler/include/", "OptiScaler/framegen/mfgunlock/hde/")


def eligible(name):
    path = PurePosixPath(name)
    return (name.startswith("OptiScaler/") and not name.startswith(EXCLUDED)
            and "external" not in path.parts and path.suffix.lower() in EXTENSIONS
            and not path.name.endswith("_Shader.h"))


def changed_ranges(diff):
    """Parse -U0 hunks; deleted-only ranges contain no new code to format."""
    result = {}
    name = None
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            name = line[6:]
        elif line.startswith("+++ "):
            name = None
        elif name and eligible(name) and line.startswith("@@"):
            match = re.search(r"\+(\d+)(?:,(\d+))? @@", line)
            if match:
                start, count = int(match[1]), int(match[2] or 1)
                if count:
                    result.setdefault(name, []).append((start, start + count - 1))
    return result


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True, encoding="utf-8")


def comparison_base(requested):
    # Initial branch pushes have a zero 'before'. Check against the first parent;
    # root commits compare against the empty tree instead.
    if not requested or set(requested) == {"0"}:
        parents = git("rev-list", "--parents", "-n", "1", "HEAD").split()
        if len(parents) > 1:
            return parents[1]
        return subprocess.check_output(["git", "hash-object", "-t", "tree", "--stdin"],
                                       input=b"", cwd=ROOT).decode().strip()
    return git("merge-base", requested, "HEAD").strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default=os.environ.get("FORMAT_BASE", ""))
    parser.add_argument("--formatter", default="clang-format")
    parser.add_argument("--fix", action="store_true")
    args = parser.parse_args()
    version = subprocess.check_output([args.formatter, "--version"], text=True)
    if not re.search(r"version 20\.1\.8\b", version):
        raise SystemExit("Use clang-format 20.1.8, matching CI: " + version.strip())
    base = comparison_base(args.base)
    diff = git("-c", "core.quotepath=false", "diff", "--no-ext-diff", "--no-textconv", "--no-renames",
               "--unified=0", base, "--", "OptiScaler")
    ranges = changed_ranges(diff)
    failures = 0
    for name, lines in sorted(ranges.items()):
        path = ROOT / name
        original = path.read_bytes()
        formatted = subprocess.check_output([args.formatter, *[f"--lines={a}:{b}" for a, b in lines],
                                             str(path)], cwd=ROOT)
        if original.replace(b"\r\n", b"\n") == formatted.replace(b"\r\n", b"\n"):
            continue
        failures += 1
        if args.fix:
            path.write_bytes(formatted)
            print("Formatted changed lines:", name)
        else:
            print("::error file=" + name + "::Changed lines need clang-format 20.1.8")
            print("".join(difflib.unified_diff(original.decode("utf-8").splitlines(True),
                                               formatted.decode("utf-8").splitlines(True),
                                               fromfile=name, tofile=name + " (formatted)")))
    print(f"Checked {len(ranges)} C/C++ files against {base[:12]}; "
          f"{failures} {'updated' if args.fix else 'need formatting'}; generated shader arrays excluded.")
    return 0 if args.fix or not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
