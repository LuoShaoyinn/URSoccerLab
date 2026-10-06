#!/usr/bin/env python3
"""Consolidate local generated outputs without deleting files.

Run once on a checkout to route Unreal's fixed Saved paths and legacy capture
paths into artifacts/. Re-running is safe; conflicting destinations are rejected.
"""
from __future__ import annotations

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIRECTORIES = {
    'Saved/Tests': 'artifacts/tests',
    'Saved/Benchmarks': 'artifacts/benchmarks',
    'Saved/Diagnostics': 'artifacts/diagnostics',
    'Saved/Logs': 'artifacts/logs',
    'Saved/Crashes': 'artifacts/crashes',
    'Saved/ShaderDebugInfo': 'artifacts/debug/shaders',
    'Saved/MaterialStats': 'artifacts/debug/materials',
    'Saved/Tmp': 'artifacts/tmp',
    'Saved/Cleanup': 'artifacts/archive',
    'Saved/Generated': 'artifacts/generated',
    'Saved/Screenshots': 'artifacts/screenshots',
    'py_example/out': 'artifacts/outputs',
    'runs': 'artifacts/runs',
}
FILES = {'Saved/AutoScreenshot.png': 'artifacts/screenshots/AutoScreenshot.png'}


def main() -> None:
    mappings = {**DIRECTORIES, **FILES}
    # Check every destination before moving anything. Never merge or overwrite.
    for source, destination in mappings.items():
        old, new = ROOT / source, ROOT / destination
        if old.is_symlink():
            if old.resolve() != new.resolve():
                raise RuntimeError(f'Unexpected existing link: {old}')
        elif old.exists() and (new.exists() or new.is_symlink()):
            raise RuntimeError(f'Both paths exist; resolve the conflict first: {old}, {new}')
    for source, destination in mappings.items():
        old, new = ROOT / source, ROOT / destination
        if old.is_symlink():
            continue
        if source in FILES and not old.exists():
            continue
        new.parent.mkdir(parents=True, exist_ok=True)
        if old.exists():
            old.rename(new)
        elif not new.exists():
            new.mkdir()
        old.parent.mkdir(parents=True, exist_ok=True)
        old.symlink_to(os.path.relpath(new, old.parent), target_is_directory=source in DIRECTORIES)
        print(f'{source} -> {destination}')


if __name__ == '__main__':
    main()
