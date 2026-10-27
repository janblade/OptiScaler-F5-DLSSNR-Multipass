"""Check or fix the formatting of the lines you changed, with the same clang-format the CI uses.

Only changed lines are looked at, so older unformatted code never fails a check. Files listed in
.clang-format-ignore (third-party code, generated shader blobs) are skipped.

Setup once:  python -m pip install -r tools/requirements-format.txt
Check:       python tools/format_check.py [--base origin/release/X.Y.Z]
Fix:         python tools/format_check.py --fix [--base origin/release/X.Y.Z]

Without --head the check covers everything since --base, committed or not (default base: where the branch left
origin/main). With --head (the CI) it covers the commits between --base and --head only.
Exit 0 = formatted, 1 = lines need formatting (the diff is printed), 2 = setup or git error.
"""
import argparse
from importlib import metadata
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
requirements = Path(__file__).with_name('requirements-format.txt')


def pinned_version():
    for line in requirements.read_text().splitlines():
        if line.startswith('clang-format=='):
            return line.split('==', 1)[1].strip()
    raise SystemExit(f'{requirements} has no clang-format== line')


def git(*args):
    result = subprocess.run(['git', *args], cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr.strip(), file=sys.stderr)
        raise SystemExit(2)
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--base', help='commit or branch to compare against (default: merge base with origin/main)')
    parser.add_argument('--head', help='last commit to check; leave out to include uncommitted changes')
    parser.add_argument('--fix', action='store_true', help='format the changed lines in place')
    opts = parser.parse_args()

    want = pinned_version()
    try:
        have = metadata.version('clang-format')
    except metadata.PackageNotFoundError:
        have = None
    if have != want:
        print(f'Needs clang-format {want} (found {have or "none"}), the version the CI uses. Install it with:\n'
              f'    {Path(sys.executable).name} -m pip install -r tools/requirements-format.txt', file=sys.stderr)
        return 2
    import clang_format

    if opts.fix and opts.head:
        parser.error('--fix works on the working tree; leave out --head')
    base = opts.base or git('merge-base', 'origin/main', 'HEAD')
    command = [sys.executable, '-m', 'clang_format.git_clang_format',
               '--binary', clang_format.get_executable('clang-format')]
    # --force: the fix is made from the working copy itself, so unstaged edits are kept, not lost.
    command += ['--force'] if opts.fix else ['--diff', '--quiet']
    command += [base] + ([opts.head] if opts.head else []) + ['--', 'OptiScaler/']

    # core.safecrlf=false: no "LF will be replaced by CRLF" warning per file on Windows checkouts.
    env = dict(os.environ, GIT_CONFIG_COUNT='1', GIT_CONFIG_KEY_0='core.safecrlf', GIT_CONFIG_VALUE_0='false')
    result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True)
    output = result.stdout.strip()
    if result.returncode not in (0, 1):
        print(result.stderr.strip() or output, file=sys.stderr)
        return 2
    if opts.fix:
        print(output)
        return 0
    if not output:
        print('Changed lines are formatted.')
        return 0
    print(output)
    print(f'\nThese changed lines are not formatted (clang-format {want}). Fix them with:\n'
          f'    python tools/format_check.py --fix --base <the branch your PR targets>')
    return 1


if __name__ == '__main__':
    sys.exit(main())
