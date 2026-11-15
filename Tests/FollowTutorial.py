#!/usr/bin/env python3
"""Follows docs/ShippingTutorial.md literally (S15): every block marked for it, in order.

    python3 Tests/FollowTutorial.py docs/ShippingTutorial.md /absolute/path/to/new/TwoRooms

Run it from the engine folder, the tutorial's own working folder. The game folder must not
exist yet: the tutorial starts from nothing. Markers, an HTML comment on the line before a
fenced block:

    <!-- tutorial: file Config/Levels.swlevels -->   writes the block to $GAME/<that path>
    <!-- tutorial: run -->                           runs the block with bash -e
    <!-- tutorial: run expect "regex" -->            ... and its output must match

Blocks without a marker are illustrations (opening the editor, sample output) and are
skipped. Each run block gets GAME in its environment, as the reader would have set it in
step 1. Exits 1 at the first step that fails, naming its line in the page.
"""
import os
import re
import subprocess
import sys

MARKER = re.compile(r'^<!-- tutorial: (file (?P<path>\S+)|run(?: expect "(?P<expect>[^"]+)")?) -->$')
FENCE = re.compile(r'^```')


def steps(text):
    """(line, kind, argument, expect, body) for every marked block, in order."""
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        m = MARKER.match(lines[i].strip())
        if not m:
            i += 1
            continue
        marker_line = i + 1
        i += 1
        if i >= len(lines) or not FENCE.match(lines[i]):
            raise SystemExit(f'line {marker_line}: a tutorial marker must be followed by a fenced block')
        i += 1
        body = []
        while i < len(lines) and not FENCE.match(lines[i]):
            body.append(lines[i])
            i += 1
        if i >= len(lines):
            raise SystemExit(f'line {marker_line}: the block never ends')
        i += 1
        if m.group('path'):
            yield marker_line, 'file', m.group('path'), None, '\n'.join(body) + '\n'
        else:
            yield marker_line, 'run', None, m.group('expect'), '\n'.join(body) + '\n'


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    page, game = sys.argv[1], os.path.abspath(sys.argv[2])
    if os.path.exists(game):
        raise SystemExit(f'{game} already exists: the tutorial starts from an empty folder')
    with open(page, encoding='utf-8') as f:
        text = f.read()
    env = dict(os.environ, GAME=game)
    count = 0
    for line, kind, path, expect, body in steps(text):
        count += 1
        if kind == 'file':
            target = os.path.normpath(os.path.join(game, path))
            if not target.startswith(game + os.sep):
                raise SystemExit(f'line {line}: {path} is outside the game folder')
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, 'w', encoding='utf-8', newline='\n') as f:
                f.write(body)
            print(f'== line {line}: wrote {path}', flush=True)
            continue
        print(f'== line {line}: running\n{body}', flush=True)
        run = subprocess.run(['bash', '-e', '-c', body], env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors='replace')
        sys.stdout.write(run.stdout[-6000:])
        if run.returncode != 0:
            raise SystemExit(f'line {line}: the commands failed (exit {run.returncode})')
        if expect and not re.search(expect, run.stdout):
            raise SystemExit(f'line {line}: expected "{expect}" in the output')
    if count == 0:
        raise SystemExit(f'{page} has no tutorial steps')
    print(f'followed {count} steps of {page}: every one passed')


if __name__ == '__main__':
    main()
