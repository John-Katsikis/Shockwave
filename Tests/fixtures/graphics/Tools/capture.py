"""Capture the graphics fixtures, or compare a new capture with the recorded baseline.

    python3 Tests/fixtures/graphics/Tools/capture.py --out /tmp/capture
        Renders every scene and camera at 1280x720 and 1920x1080, render scale 0.5 and 1,
        with player --fixed-step (frame 30 is the same moment every run). Writes the PNGs,
        a capture.json with each image's statistics and SHA-256, and short benchmarks: one
        unpaced (vsync off, for throughput) and one paced at 60 fps (for frame-time jitter).

    python3 Tests/fixtures/graphics/Tools/capture.py --out /tmp/capture --record
        Also copies the result to Reference/: baseline.json, plus the 1280x720 render
        scale 1 image of each scene's first camera. Do this only for an intended change in
        the baseline, and say why in the commit.

    python3 Tests/fixtures/graphics/Tools/capture.py --out /tmp/capture --compare
        Captures, then compares with Reference/: every committed image through
        player --image-diff (RMSE tolerance --tolerance, default 1.0), every other image by
        its mean colour. Exits 1 on any difference past tolerance.

Uses build/bin/player (or --player). The display needs a Vulkan driver; under CI or a
headless machine run it through xvfb-run with a software driver such as llvmpipe.
Software and hardware drivers rasterize differently: compare captures made on the same
driver, which baseline.json records.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

fixture = Path(__file__).resolve().parents[1]
repo = fixture.parents[2]
project = fixture / 'Graphics.swproject'

# Scene: cameras to look through (the first is the scene's primary camera).
SCENES = {
    'thin-geometry': ['Main Camera', 'Grazing'],
    'room-doorway': ['Inside', 'Outside'],
    'material-spheres': ['Main Camera'],
    'transparency': ['Main Camera', 'Side'],
    'lights-overlap': ['Main Camera'],
    'motion': ['Orbit camera', 'Fixed'],
    'decal-wall': ['Main Camera'],
    'dense-occluder': ['Main Camera', 'Overhead'],
}
SIZES = ['1280x720', '1920x1080']
SCALES = ['1', '0.5']
FRAME = 30
BENCH_SECONDS = 3


def slug(text):
    return text.lower().replace(' ', '-')


def run(player, args, env):
    command = [str(player), '--project', str(project), '--no-user-graphics'] + args
    if os.environ.get('DISPLAY') is None and os.environ.get('WAYLAND_DISPLAY') is None and shutil.which('xvfb-run'):
        command = ['xvfb-run', '-a'] + command
    result = subprocess.run(command, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f'failed: {" ".join(command)}\n{result.stdout}\n{result.stderr}')
    return result.stdout


def stats(player, image, env):
    out = subprocess.run([str(player), '--image-stats', str(image)], env=env, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(out.stderr)
    return json.loads(out.stdout)


def capture(player, out, env):
    images = []
    for scene, cameras in SCENES.items():
        for camera in cameras:
            for size in SIZES:
                for scale in SCALES:
                    name = f'{scene}--{slug(camera)}--{size}--{scale}.png'
                    path = out / name
                    run(player, ['--scene', str(fixture / 'Scenes' / f'{scene}.swscene'), '--camera', camera,
                                 '--size', size, '--render-scale', scale, '--fixed-step', '--frames', str(FRAME),
                                 '--screenshot', str(path)], env)
                    entry = {'file': name, 'scene': scene, 'camera': camera, 'size': size, 'render_scale': float(scale),
                             'frame': FRAME, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                    entry.update(stats(player, path, env))
                    images.append(entry)
                    print(f'{name}: luma {entry["mean_luma"]:.2f}')
    bench = []
    for scene in SCENES:
        for paced in (False, True):
            json_path = out / f'{scene}--bench--{"paced" if paced else "unpaced"}.json'
            args = ['--scene', str(fixture / 'Scenes' / f'{scene}.swscene'), '--size', '1280x720',
                    '--benchmark', str(BENCH_SECONDS), '--json', str(json_path), '--vsync', 'off']
            if paced:
                args += ['--fps', '60']
            run(player, args, env)
            report = json.loads(json_path.read_text())
            keep = {k: report[k] for k in ('gpu', 'present_mode', 'frame_limit', 'frames', 'average_fps', 'average_ms', 'low_1_percent_fps', 'worst_ms', 'jitter_ms', 'cpu_ms', 'gpu_ms', 'draws') if k in report}
            keep.update(scene=scene, paced=paced)
            bench.append(keep)
            print(f'{scene} {"paced" if paced else "unpaced"}: {keep.get("average_ms", "?")} ms, jitter {keep.get("jitter_ms", "?")}')
    gpus = subprocess.run([str(player), '--gpus'], env=env, capture_output=True, text=True).stdout.strip()
    commit = subprocess.run(['git', '-C', str(repo), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', '-C', str(repo), 'status', '--porcelain', '--', 'Source'], capture_output=True,
                           text=True).stdout.strip() != ''
    result = {'commit': commit, 'source_modified': dirty, 'gpus': gpus, 'frame': FRAME,
              'command': 'Tests/fixtures/graphics/Tools/capture.py', 'images': images, 'benchmarks': bench}
    (out / 'capture.json').write_text(json.dumps(result, indent=1) + '\n')
    return result


def committed(entry):
    return entry['size'] == SIZES[0] and entry['render_scale'] == 1.0 and entry['camera'] == SCENES[entry['scene']][0]


def compare(player, out, result, tolerance, env):
    reference = json.loads((fixture / 'Reference/baseline.json').read_text())
    old = {e['file']: e for e in reference['images']}
    failures = 0
    for entry in result['images']:
        before = old.get(entry['file'])
        if before is None:
            print(f'NEW   {entry["file"]}')
            continue
        if committed(entry):
            diff = subprocess.run([str(player), '--image-diff', str(fixture / 'Reference' / entry['file']),
                                   str(out / entry['file']), '--tolerance', str(tolerance)], env=env,
                                  capture_output=True, text=True)
            report = json.loads(diff.stdout)
            ok = diff.returncode == 0
            print(f'{"ok  " if ok else "FAIL"}  {entry["file"]}: rmse {report["rmse"]:.3f}, max {report["max_diff"]}, '
                  f'{report["changed_percent"]:.3f}% changed')
        else:
            delta = max(abs(a - b) for a, b in zip(entry['mean_rgb'], before['mean_rgb']))
            ok = delta <= tolerance
            same = entry['sha256'] == before['sha256']
            print(f'{"ok  " if ok else "FAIL"}  {entry["file"]}: mean colour off by {delta:.3f}{" (identical)" if same else ""}')
        failures += not ok
    print(f'{failures} image(s) past tolerance {tolerance}')
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--player', type=Path, default=repo / 'build/bin/player')
    parser.add_argument('--record', action='store_true')
    parser.add_argument('--compare', action='store_true')
    parser.add_argument('--tolerance', type=float, default=1.0)
    args = parser.parse_args()
    env = dict(os.environ, SDL_AUDIO_DRIVER='dummy')
    args.out.mkdir(parents=True, exist_ok=True)
    result = capture(args.player, args.out, env)
    if args.record:
        reference = fixture / 'Reference'
        reference.mkdir(exist_ok=True)
        for old in reference.glob('*.png'):
            old.unlink()
        for entry in result['images']:
            if committed(entry):
                shutil.copy(args.out / entry['file'], reference / entry['file'])
        (reference / 'baseline.json').write_text(json.dumps(result, indent=1) + '\n')
        print(f'Recorded {reference}')
    if args.compare:
        sys.exit(1 if compare(args.player, args.out, result, args.tolerance, env) else 0)


if __name__ == '__main__':
    main()
