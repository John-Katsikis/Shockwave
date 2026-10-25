#!/usr/bin/env python3
"""Writes a firefight for measuring particle cost: four effects and the scenes that use them.

Effects (Content/Effects/firefight-*.efkefc), written straight to Effekseer's runtime format
(version 1610, the one laser.efkefc uses), so they're this project's own:
    firefight-rifle      a rifle firing about 12 rounds a second: muzzle flash, flame,
                         sparks, a tracer down range and a puff of smoke per shot
    firefight-impact     bullets landing on cover: a flash, debris sparks, a dust puff
    firefight-smoke      a burning wreck's smoke column: big, slow, alpha-blended puffs
    firefight-grenade    a grenade going off every few seconds: fireball, sparks, smoke

Scenes (Scenes/particle-stress-<N>v<N>.swscene): two teams of N rifles 30 m apart behind
cover, every rifle firing at once, impacts on both lines of cover, smoke columns and
grenades between them, seen from behind one team so most of it overlaps on screen.
The four sizes go from well under to over Effects::kMaxParticles (20,000 alive at once).

    python3 Games/Sample/Tools/MakeParticleStress.py            # 4v4 8v8 16v16 24v24 32v32
    python3 Games/Sample/Tools/MakeParticleStress.py 12 24      # any team sizes

Benchmark one with (docs/Effects.md, "What it costs"):
    build/bin/player --project Games/Sample/Sample.swproject \\
        --scene Games/Sample/Scenes/particle-stress-16v16.swscene \\
        --benchmark 20 --vsync off --fps 0 --json out.json

These effects can't be opened in the Effekseer editor (they carry no editor data);
change them here and run the script again.
"""

import math
import pathlib
import random
import struct
import sys

SAMPLE = pathlib.Path(__file__).resolve().parent.parent
EFFECTS = SAMPLE / "Content" / "Effects"
SCENES = SAMPLE / "Scenes"

# ---- Effekseer's binary format ------------------------------------------------------------
# Field order follows Effekseer 1.7.3's loader for files of version 1610
# (EffectImplemented::LoadBody, EffectNodeImplemented::LoadParameter, EffectNodeSprite).
# Times are in frames at 60 per second, distances in metres (the effect's scale is 1).

VERSION = 1610
TEXTURES = ["Texture/Particle01.png", "Texture/Particle02.png", "Texture/Smoke01.png"]
GLOW, SPARK, SMOKE = range(3)
BLEND_ALPHA, BLEND_ADD = 1, 2
BIND_WHEN_CREATING, BIND_ALWAYS = 1, 2
INFINITE = 2**31 - 1


def i32(*values):
    return struct.pack(f"<{len(values)}i", *values)


def f32(*values):
    return struct.pack(f"<{len(values)}f", *values)


def rng(lo, hi=None):
    """random_float: max, then min."""
    return f32(lo if hi is None else hi, lo)


def rng3(lo, hi=None):
    """random_vector3d: max xyz, then min xyz."""
    hi = lo if hi is None else hi
    return f32(*hi, *lo)


def rgba(c):
    return bytes(max(0, min(255, round(v * 255))) for v in c)


def sprite(texture, blend, *, count, life, rate=(0, 0), delay=(0, 0), bind=BIND_WHEN_CREATING,
           at=((0, 0, 0), (0, 0, 0)), sphere=None, velocity=((0, 0, 0), (0, 0, 0)),
           accel=((0, 0, 0), (0, 0, 0)), size=(1, 1), grow=(0, 0), grow_accel=(0, 0), spin=(0, 0),
           color=(1, 1, 1, 1), end_color=None, fade_in=0, fade_out=0, children=()):
    """One sprite node: count billboards, each living life (min, max) frames, one every rate
    frames after delay. at/velocity/accel are (min, max) xyz ranges in the parent's space;
    sphere=(radius_min, radius_max) spawns on a sphere instead. size is the starting width,
    grow its change per frame. color fades to end_color over a lifetime, alpha fades in and
    out over fade_in/fade_out frames."""
    return dict(kind=2, texture=texture, blend=blend, count=count, life=life, rate=rate, delay=delay,
                bind=bind, at=at, sphere=sphere, velocity=velocity, accel=accel, size=size, grow=grow,
                grow_accel=grow_accel, spin=spin, color=color, end_color=end_color, fade_in=fade_in,
                fade_out=fade_out, children=children)


def group(*, count, life, rate=(0, 0), delay=(0, 0), at=((0, 0, 0), (0, 0, 0)), children=()):
    """A node that draws nothing: its children spawn each time it does (one shot, one burst)."""
    return dict(kind=0, count=count, life=life, rate=rate, delay=delay, bind=BIND_ALWAYS, at=at,
                sphere=None, velocity=((0, 0, 0), (0, 0, 0)), accel=((0, 0, 0), (0, 0, 0)), children=children)


def node_bytes(n, numbers):
    # Type, drawn, and the node's slot in the effect's draw list: each node needs its own.
    out = [i32(n["kind"], 1, next(numbers))]
    # ParameterCommonValues: equation refs (unused), count, translation/rotation/scale
    # binding, removal rules, life, spawn interval and first spawn delay.
    out += [i32(80), i32(-1, -1, -1, -1, -1, -1, -1), i32(n["count"], n["bind"], n["bind"], n["bind"]),
            i32(1, 0, 0), i32(n["life"][1], n["life"][0]), rng(*n["rate"]), rng(*n["delay"])]
    # Translation: position, velocity, acceleration (PVA).
    out += [i32(1, 96), i32(*[-1] * 6), rng3((0, 0, 0)), rng3(*n["velocity"]), rng3(*n["accel"])]
    out += [i32(4)] + [i32(0) + f32(1, 0, 0, 0, 0, 0, 0) + i32(0)] * 4   # four empty local force fields
    if n["kind"] == 2:
        spin = n["spin"]
        out += [i32(1, 96), i32(*[-1] * 6), rng3((0, 0, 0), (0, 0, 2 * math.pi) if spin != (0, 0) else (0, 0, 0)),
                rng3((0, 0, spin[0]), (0, 0, spin[1])), rng3((0, 0, 0))]
        out += [i32(3, 24), rng(*n["size"]), rng(*n["grow"]), rng(*n["grow_accel"])]   # one scale for x and y
    else:
        out += [i32(0, 16, -1) + f32(0, 0, 0), i32(0, 16, -1) + f32(1, 1, 1)]
    if n["sphere"]:
        out += [i32(0, 1), rng(*n["sphere"]), rng(-math.pi, math.pi), rng(-math.pi, math.pi)]
    else:
        out += [i32(0, 0), rng3(*n["at"])]
    out += [f32(0), i32(0, 0), f32(1, 3.4028234663852886e38), i32(0, 0), f32(1)]   # depth: defaults
    texture = n.get("texture", -1)
    out += [i32(0), f32(1), i32(texture, *[-1] * 6), i32(n.get("blend", BLEND_ALPHA))]
    out += [i32(1, 1) * 7, i32(1, 0)]   # linear, clamped textures; depth test, no depth write
    fade_in, fade_out = n.get("fade_in", 0), n.get("fade_out", 0)
    out += [i32(1) + f32(fade_in, 0, 0, 1) if fade_in else i32(0)]
    out += [i32(1) + f32(fade_out, 0, 0, 1) if fade_out else i32(0)]
    out += [i32(0, 0, 0), f32(0.01), i32(0, -1, 0, 0), f32(0.01)]   # no UV animation
    out += [i32(0), f32(1), i32(0, 0), i32(0), i32(0), f32(0, 0, 0)]   # colour, distortion, custom data, cutoff, falloff, soft
    if n["kind"] == 2:
        out += [i32(2, 0, 0)]   # sprite, drawing order, camera-facing billboard
        if n["end_color"]:
            out += [i32(2), b"\0\0" + rgba(n["color"]) + rgba(n["color"]), b"\0\0" + rgba(n["end_color"]) * 2,
                    f32(0, 0, 1)]
        else:
            out += [i32(0), rgba(n["color"])]
        out += [i32(0), i32(1), f32(-0.5, -0.5, 0.5, -0.5, -0.5, 0.5, 0.5, 0.5)]
    else:
        out += [i32(0)]
    out += [i32(0), i32(len(n["children"]))]   # no sound
    return b"".join(out) + b"".join(node_bytes(c, numbers) for c in n["children"])


def count_nodes(nodes):
    return sum(1 + count_nodes(n["children"]) for n in nodes)


def effect_file(*nodes):
    body = [b"SKFE", i32(VERSION), i32(len(TEXTURES))]
    for path in TEXTURES:
        body += [i32(len(path) + 1), (path + "\0").encode("utf-16-le")]
    body += [i32(0) * 7]   # normal, distortion images, sounds, models, materials, curves, procedural models
    body += [i32(4), f32(0, 0, 0, 0), i32(0)]   # dynamic inputs, no equations
    body += [i32(count_nodes(nodes), 0), f32(1), i32(-1), i32(0)]   # magnification 1, random seed, no culling
    numbers = iter(range(count_nodes(nodes)))
    body += [i32(-1, len(nodes))] + [node_bytes(n, numbers) for n in nodes]
    runtime = b"".join(body)
    return b"EFKE" + i32(0) + b"BIN_" + i32(len(runtime)) + runtime


# ---- The effects --------------------------------------------------------------------------
# Each effect fires along its +Z axis. Gravity is 9.81 m/s^2 = 0.0027 m/frame^2.
G = 0.0027
SMOKE_GREY = (0.42, 0.4, 0.38, 0.55)


def rifle():
    shot = [
        sprite(GLOW, BLEND_ADD, count=2, life=(2, 3), bind=BIND_ALWAYS, size=(0.5, 0.8),
               color=(1, 0.75, 0.4, 1)),   # muzzle flash
        sprite(GLOW, BLEND_ADD, count=6, life=(3, 5), velocity=((-0.02, -0.02, 0.04), (0.02, 0.02, 0.12)),
               size=(0.12, 0.2), grow=(-0.02, -0.01), color=(1, 0.55, 0.15, 1)),   # flame
        sprite(SPARK, BLEND_ADD, count=12, life=(8, 18), velocity=((-0.06, -0.03, 0.05), (0.06, 0.08, 0.2)),
               accel=((0, -G, 0), (0, -G, 0)), size=(0.03, 0.05), color=(1, 0.8, 0.45, 1),
               end_color=(1, 0.3, 0.05, 0)),   # sparks
        sprite(GLOW, BLEND_ADD, count=1, life=(14, 16), velocity=((0, 0, 2.2), (0, 0, 2.4)), size=(0.12, 0.14),
               color=(1, 0.85, 0.5, 1)),   # tracer, about 140 m/s
        sprite(SMOKE, BLEND_ALPHA, count=6, life=(50, 90), velocity=((-0.006, 0.002, 0.004), (0.006, 0.01, 0.02)),
               size=(0.15, 0.3), grow=(0.008, 0.015), spin=(-0.03, 0.03), color=SMOKE_GREY, fade_in=4,
               fade_out=30),   # muzzle smoke
    ]
    # One shot every 4-6 frames (about 12 a second); the first shot waits up to half a
    # second so rifles that start together don't fire in step.
    return effect_file(group(count=INFINITE, life=(4, 4), rate=(4, 6), delay=(0, 30), children=shot))


def impact(width):
    hit = [
        sprite(GLOW, BLEND_ADD, count=1, life=(2, 4), bind=BIND_ALWAYS, size=(0.25, 0.4), color=(1, 0.8, 0.5, 1)),
        sprite(SPARK, BLEND_ADD, count=20, life=(10, 24), velocity=((-0.08, -0.02, 0.01), (0.08, 0.1, 0.08)),
               accel=((0, -G, 0), (0, -G, 0)), size=(0.025, 0.045), color=(1, 0.85, 0.55, 1),
               end_color=(1, 0.35, 0.1, 0)),   # debris sparks
        sprite(SMOKE, BLEND_ALPHA, count=6, life=(50, 80), velocity=((-0.008, 0, 0.004), (0.008, 0.008, 0.02)),
               size=(0.2, 0.35), grow=(0.01, 0.016), spin=(-0.02, 0.02), color=(0.55, 0.5, 0.44, 0.5),
               fade_in=3, fade_out=40),   # dust
    ]
    # Hits land anywhere across a stretch of cover width metres wide and 1.2 m high.
    spread = ((-width / 2, 0.1, 0), (width / 2, 1.3, 0))
    return effect_file(group(count=INFINITE, life=(4, 4), rate=(3, 5), delay=(0, 20), at=spread, children=hit))


def smoke_column():
    return effect_file(
        sprite(SMOKE, BLEND_ALPHA, count=INFINITE, life=(180, 260), rate=(2, 3),
               at=((-0.6, 0, -0.6), (0.6, 0.3, 0.6)), velocity=((-0.006, 0.025, -0.006), (0.006, 0.04, 0.006)),
               accel=((0.0004, 0, 0), (0.0008, 0, 0)), size=(1.0, 1.6), grow=(0.012, 0.02),
               spin=(-0.01, 0.01), color=(0.22, 0.21, 0.2, 0.6), end_color=(0.4, 0.39, 0.38, 0.5),
               fade_in=20, fade_out=90),
        sprite(GLOW, BLEND_ADD, count=INFINITE, life=(20, 40), rate=(1, 2),
               at=((-0.5, 0, -0.5), (0.5, 0.2, 0.5)), velocity=((-0.005, 0.02, -0.005), (0.005, 0.04, 0.005)),
               size=(0.6, 1.0), grow=(-0.012, -0.006), color=(1, 0.45, 0.12, 0.8)),   # fire at the base
    )


def grenade():
    blast = [
        sprite(GLOW, BLEND_ADD, count=24, life=(10, 22), sphere=(0.1, 0.6),
               velocity=((-0.05, 0.0, -0.05), (0.05, 0.08, 0.05)), size=(1.0, 1.8), grow=(0.02, 0.05),
               color=(1, 0.6, 0.2, 1), end_color=(0.8, 0.2, 0.05, 0)),   # fireball
        sprite(SPARK, BLEND_ADD, count=160, life=(20, 45), velocity=((-0.22, 0.02, -0.22), (0.22, 0.3, 0.22)),
               accel=((0, -G, 0), (0, -G, 0)), size=(0.04, 0.07), color=(1, 0.85, 0.5, 1),
               end_color=(1, 0.3, 0.05, 0)),   # shrapnel sparks
        sprite(SMOKE, BLEND_ALPHA, count=30, life=(120, 200), sphere=(0.2, 1.2),
               velocity=((-0.03, 0.01, -0.03), (0.03, 0.05, 0.03)), size=(1.2, 2.0), grow=(0.015, 0.03),
               grow_accel=(-0.0001, -0.0001), spin=(-0.015, 0.015), color=(0.3, 0.28, 0.26, 0.7),
               fade_in=6, fade_out=90),   # smoke
    ]
    # Every 2.5-4 seconds, starting at a random point in the first two seconds.
    return effect_file(group(count=INFINITE, life=(4, 4), rate=(150, 240), delay=(0, 120), children=blast))


# ---- The scene ----------------------------------------------------------------------------
GAP = 30.0   # metres between the two lines of cover
SPACING = 2.2   # metres between rifles in a line


def entity(name, *components):
    lines = [f'entity "{name}"']
    for component in components:
        lines.append(f"  {component[0]}")
        lines += [f"    {field}" for field in component[1:]]
    return "\n".join(lines + ["end", ""])


def effect(name, file, position, yaw=0, scale=1):
    return entity(name, ("Transform", f"position {position[0]:.2f} {position[1]:.2f} {position[2]:.2f}",
                         f"rotation 0 {yaw:g} 0"),
                  ("ParticleEffect", f'effect "Effects/{file}"', "playing 1", "loop 1", "speed 1",
                   f"scale {scale:g}", "color 1 1 1 1"))


def box(name, position, scale, tint):
    return entity(name, ("Transform", f"position {position[0]:.2f} {position[1]:.2f} {position[2]:.2f}",
                         f"scale {scale[0]:g} {scale[1]:g} {scale[2]:g}"),
                  ("MeshRenderer", "mesh cube", "texture panel", f"tint {tint}"))


def scene(team):
    rand = random.Random(team)
    width = (team - 1) * SPACING
    out = [
        "shockwave_scene 2",
        "",
        f"# Particle stress test: a {team}v{team} firefight. {team * 2} rifles firing about twelve rounds a",
        "# second, impacts on both lines of cover, smoke columns and grenades. Generated by",
        "# Tools/MakeParticleStress.py; regenerate rather than editing by hand.",
        "",
        entity("Environment", ("Environment", "sky_color 0.32 0.36 0.42 1", "ground_color 0.08 0.07 0.06 1",
                               "ambient_intensity 0.6", "shadow_distance 80")),
        # A player just behind team A, looking down range: the near rifles' flashes and smoke
        # fill the screen and the far team's sit behind them. In the bigger fights the ends of
        # the lines are off screen; they're still simulated and built, like in a real match.
        entity("Main Camera", ("Transform", f"position 0 4 {GAP / 2 + 10:.1f}", "rotation -10 0 0"),
               ("Camera", "fov 70", "near_plane 0.1", "primary 1")),
        entity("Sun", ("Transform", "position 0 30 0", "rotation -40 30 0"),
               ("Light", "type Directional", "color 1 0.92 0.8 1", "intensity 2.2")),
        entity("Floor", ("Transform", "position 0 -0.25 0", "scale 160 0.5 160"),
               ("MeshRenderer", "mesh cube", "texture grid", "tint 0.36 0.34 0.3 1")),
    ]
    for side, z, yaw in (("A", GAP / 2, 180), ("B", -GAP / 2, 0)):
        toward = -1 if side == "A" else 1   # the direction this team fires in, along z
        cover = z + toward * 1.5
        out.append(box(f"Cover {side}", (0, 0.6, cover), (width + 4, 1.2, 0.6), "0.5 0.48 0.44 1"))
        for k in range(team):
            x = -width / 2 + k * SPACING
            out.append(effect(f"Rifle {side}{k + 1}", "firefight-rifle.efkefc",
                              (x, 1.45, z + toward * 0.6), yaw + rand.uniform(-4, 4)))
        # The other team's bullets land on this team's cover, on the side facing them.
        for k in range(team):
            x = -width / 2 + k * SPACING
            out.append(effect(f"Impacts on {side} {k + 1}", "firefight-impact.efkefc",
                              (x, 0, cover + toward * 0.32), yaw + 180))
    for k in range(max(1, team // 4)):   # burning wrecks between the lines
        x, z = rand.uniform(-width / 2 - 2, width / 2 + 2), rand.uniform(-GAP / 3, GAP / 3)
        out.append(box(f"Wreck {k + 1}", (x, 0.5, z), (2.4, 1, 1.6), "0.2 0.2 0.22 1"))
        out.append(effect(f"Smoke {k + 1}", "firefight-smoke.efkefc", (x, 1, z)))
    for k in range(max(1, team // 4)):
        out.append(effect(f"Grenade {k + 1}", "firefight-grenade.efkefc",
                          (rand.uniform(-width / 2, width / 2), 0.3,
                           rand.choice([-1, 1]) * rand.uniform(GAP / 2 - 6, GAP / 2 - 2))))
    return "\n".join(out)


def main():
    for name, data in [("firefight-rifle", rifle()), ("firefight-impact", impact(SPACING)),
                       ("firefight-smoke", smoke_column()), ("firefight-grenade", grenade())]:
        path = EFFECTS / f"{name}.efkefc"
        path.write_bytes(data)
        print(path)
    for team in [int(arg) for arg in sys.argv[1:]] or [4, 8, 16, 24, 32]:
        path = SCENES / f"particle-stress-{team}v{team}.swscene"
        path.write_text(scene(team))
        print(path)


if __name__ == "__main__":
    main()
