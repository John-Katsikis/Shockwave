#!/usr/bin/env python3
"""Run repeatable RenderLab workloads and record configuration alongside engine timings."""
import argparse
import json
from pathlib import Path
import subprocess
import time

PROJECT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--player', type=Path, default=PROJECT.parents[1]/'build/bin/player')
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--scenes', nargs='+', default=['gallery', 'stress-64', 'stress-256', 'stress-1024'],
                        choices=['materials','gallery','stress-64','stress-256','stress-1024'])
    parser.add_argument('--profile', choices=['high','low','full-detail'], default='high')
    parser.add_argument('--output', type=Path, default=PROJECT/'Saved/Benchmarks')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 120:
        parser.error('--seconds must be in 1..120')
    if not (PROJECT/'Content/Library/gun_01/model.gltf').is_file():
        parser.error('run Tools/PrepareAssets.py first')
    args.output.mkdir(parents=True,exist_ok=True)
    settings = {'high':(1,'high',1),'low':(.67,'low',2),'full-detail':(1,'high',0)}[args.profile]
    scale, shadows, lod = settings
    config = args.output / f'{args.profile}.swgraphics'
    config.write_text(f'swgraphics 1\nrender_scale {scale}\nshadow_quality {shadows}\nlod_error_pixels {lod}\nvsync false\nframe_limit 0\nlow_latency false\n')
    cache = args.player.resolve().parents[1]/'CMakeCache.txt'
    build_type = 'unknown'
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith('CMAKE_BUILD_TYPE:STRING='):
                build_type = line.partition('=')[2] or '(unset)'
    reports = []
    for name in args.scenes:
        path = args.output / f'{name}-{args.profile}.json'
        command = [str(args.player.resolve()),'--project',str(PROJECT/'RenderLab.swproject'),
                   '--scene',str(PROJECT/'Scenes'/f'{name}.swscene'),'--graphics',str(config.resolve()),
                   '--size','1280x720','--benchmark',str(args.seconds),'--json',str(path.resolve())]
        start = time.monotonic()
        with path.with_suffix('.log').open('w') as log:
            subprocess.run(command,stdout=log,stderr=log,check=True,timeout=300)
        report = json.loads(path.read_text())
        report['renderlab'] = {'profile':args.profile,'render_scale':scale,'shadow_quality':shadows,
            'lod_error_pixels':lod,'output_resolution':[1280,720],'build_type':build_type,
            'command':command,'total_process_seconds':time.monotonic()-start,
            'note':'Engine timings exclude startup and warmup; rolling frame window may cover less than requested duration.'}
        path.write_text(json.dumps(report,indent=2)+'\n')
        reports.append({'scene':name,'profile':args.profile,'fps':report['average_fps'],
                        'gpu_ms':report['gpu_ms'],'low_1_percent_fps':report['low_1_percent_fps']})
        print(f'{name} / {args.profile}: {report["average_fps"]:.1f} FPS, GPU {report["gpu_ms"]:.2f} ms',flush=True)
    (args.output/f'summary-{args.profile}.json').write_text(json.dumps(reports,indent=2)+'\n')

if __name__ == '__main__':
    main()
