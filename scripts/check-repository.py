#!/usr/bin/env python3
"""Check public source hygiene without executing project tools or accessing devices."""
import argparse
import ast
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit


def check(root):
    errors = []
    total = 0
    count = 0
    tracked_result = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'],
                                    capture_output=True, text=True)
    tracked = set(tracked_result.stdout.split('\0')) if tracked_result.returncode == 0 else set()
    blocked_dirs = {'__pycache__', '.venv', 'node_modules', 'staging', 'backups',
                    'analysis', 'runtime', 'runs', 'private', 'local', 'build', 'dist'}
    artifact = re.compile(
        r'\.(?:iso|img|raw|qcow2|vmdk|vdi|dmg|o|a|so(?:\.[\d.]+)?|ko|dtb|dtbo|cpio|'
        r'log|ppm|pyc|pyo|zip|7z|tgz|gz|xz|zst|tar|pem|key)$', re.I)
    secret = re.compile(
        r'-----BEGIN (?:OPENSSH |RSA |EC |DSA )?PRIVATE KEY-----|'
        r'gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,}|'
        r'\bsk-[A-Za-z0-9_-]{25,}')
    personal = re.compile('/' + r'Users/[^/\s]+/|/' + r'Volumes/[^/\n]+/')
    for directory, dirs, names in os.walk(root, followlinks=False):
        # Git metadata is local state, never repository source content.
        dirs[:] = sorted(d for d in dirs if d != '.git')
        for name in list(dirs):
            path = Path(directory) / name
            rel = path.relative_to(root)
            if path.is_symlink() or name in blocked_dirs or name.endswith(('.utm', '.sparsebundle')):
                errors.append(f'{rel}: unexpected directory or symlink')
                dirs.remove(name)
        for name in sorted(names):
            path = Path(directory) / name
            rel = path.relative_to(root)
            if name == '.git':
                continue
            # Finder may recreate these while browsing; .gitignore excludes them.
            # Still reject them if they have actually entered Git's index.
            if name == '.DS_Store' and rel.as_posix() not in tracked:
                continue
            if path.is_symlink():
                errors.append(f'{rel}: symlinks are not allowed')
                continue
            data = path.read_bytes()
            total += len(data)
            count += 1
            if len(data) > 1024 * 1024:
                errors.append(f'{rel}: exceeds 1 MiB file limit')
            if (artifact.search(name) or name == '.DS_Store' or name == 'Image'
                    or name.startswith(('vmlinuz', 'initrd', 'id_rsa', 'id_ed25519'))
                    or (name.startswith('.env') and name != '.env.example')):
                errors.append(f'{rel}: external/generated/private file')
            try:
                text = data.decode('utf-8')
            except UnicodeDecodeError:
                errors.append(f'{rel}: unexpected binary file')
                continue
            if '\0' in text:
                errors.append(f'{rel}: unexpected NUL bytes')
            for line_no, line in enumerate(text.splitlines(), 1):
                if secret.search(line):
                    errors.append(f'{rel}:{line_no}: possible credential (value omitted)')
                if personal.search(line):
                    errors.append(f'{rel}:{line_no}: personal absolute path')
            if path.suffix == '.py':
                try:
                    ast.parse(text, filename=str(rel))
                except SyntaxError as exc:
                    errors.append(f'{rel}:{exc.lineno}: invalid Python syntax')
            first_line = text.splitlines()[0] if text else ''
            shell = ('bash' if 'bash' in first_line else 'sh')
            if path.suffix == '.sh' or (first_line.startswith('#!') and
                                      re.search(r'\b(?:bash|sh)\b', first_line)):
                result = subprocess.run([shell, '-n', str(path)], capture_output=True, text=True)
                if result.returncode:
                    errors.append(f'{rel}: invalid shell syntax: {result.stderr.strip()}')
            if path.suffix == '.md':
                for target in re.findall(r'\[[^\]]*\]\(([^\s)]+)\)', text):
                    target = target.strip('<>')
                    url = urlsplit(target)
                    if url.scheme or url.netloc or not url.path:
                        continue
                    dest = (path.parent / unquote(url.path)).resolve()
                    if not dest.is_relative_to(root) or not dest.exists():
                        errors.append(f'{rel}: missing or external relative link: {target}')
    if total > 10 * 1024 * 1024:
        errors.append('Repository source exceeds 10 MiB total limit')
    return errors, count, total


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        parser.error('--root must be an existing directory')
    problems, count, total = check(root)
    if problems:
        print('\n'.join(problems), file=sys.stderr)
        sys.exit(1)
    print(f'OK: {count} source files, {total / 1024:.1f} KiB; hygiene, links and syntax passed.')
