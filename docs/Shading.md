# Shading families

Every `.swmaterial` has a `family` ([MaterialSchema.md](MaterialSchema.md)). Most
surfaces are `standard`, glTF's metallic-roughness, and three more cover what it can't:
`transmission` for glass, `skin` for faces and `hair_card` for hair. Each family reads
its own group of values; a value from another family's group does nothing, and `player
--check` and the Material window say so. Everything here is switchable off from the
graphics settings ([Graphics.md](Graphics.md)), and every setting off has an exact,
documented fallback with no pass and no memory.

The maths is in `Renderer/Shading.h` (checked by the `shading` test), and the design
notes are in [Graphics/05-CharacterShading.md](Graphics/05-CharacterShading.md).

## Clearcoat (standard)

A clear varnish over the surface, such as car paint, lacquered wood or a wet look:

    clearcoat 1
    clearcoat_roughness 0.05
    texture clearcoat "textures/coat-mask.png"          # r: the coat's factor, g: its roughness
    texture clearcoat_normal "textures/orange-peel.png"  # optional: its own bumps

The coat is a dielectric with an index of refraction of 1.5 (F0 0.04). It has its own GGX lobe, with its own
roughness and normal (the geometric normal unless it has a normal map). The base under it
gets what the coat lets through, both ways:

    f = (1 - c F(n.l)) (1 - c F(n.v)) f_base + c f_coat

Direct light and the environment use the same split; the environment's reflection for
the coat uses the coat's own roughness and normal. `clearcoat 0`, or no `clearcoat` line,
is plain standard, pixel for pixel. In a white furnace (the `shading` test) a full coat
never adds energy and loses at most 15% at any angle: the split is the documented
approximation from the design notes, not an exact layered BSDF.

**Setting.** `clearcoat false` draws the surface under the coat, exactly as without it.

## Transmission (glass)

    family transmission
    base_color 0.85 1 0.9 1       # the tint
    roughness 0.02                # rough glass blurs what's behind it
    ior 1.5
    transmission_mode solid       # or thin: a sheet, nothing bends
    thickness 0.02                # metres (scaled with the object)
    attenuation_color 0.55 0.75 1
    attenuation_distance 1.2      # attenuation_color after this many metres inside

Glass is drawn after the opaque scene, over a copy of it with mips. The share of light it
passes is `transmission x (1 - Fresnel)` (the same Schlick split as its reflection), times
its colour, times absorption `exp(-sigma thickness)` with
`sigma = -ln(attenuation_color) / attenuation_distance`.

- **Thin** glass, and every glass on `low`, looks straight through.
- **Solid** glass (`medium` and `high`) refracts the view ray once at its front face and
  reads the copy where the ray leaves after `thickness`.
- **Rough** glass reads a blurrier mip of the copy. `high` adds four taps around it for a
  smoother blur.
- **Fallbacks.** Where the ray leaves the screen, glass shows the environment. Where
  something nearer than the glass is at the spot it would read, it looks straight
  through instead, so a hand in front of a window never shows up inside it. `player
  --view transmission` (the editor's *Glass (transmission)* view) colours each pixel by
  source: green is the scene behind, red the environment, blue straight through.

**Setting.** `transmission_quality off` draws simplified glass: blended, tinted, letting
85% of the scene through at `transmission 1`, with no copy and no pass. The same happens
on any frame where the glass pass doesn't run.

### Known limits

- One refraction, at the front face. A solid's back face, and glass behind glass, are not
  traced. Glass behind glass shows the opaque scene only.
- Blended surfaces and particles behind glass don't show through it: the copy is taken
  before them.
- The copy is already fogged, so the light through the glass gets no more fog. The glass
  surface takes the fog in front of it in proportion to the light it doesn't pass.
- Glass casts no shadow, and decals don't land on it.
- `transmission_mode solid` with `double_sided true` warns: the back faces would refract
  as if they were front ones.

## Skin

    family skin
    base_color 0.8 0.55 0.45 1
    skin_radius 3.7 1.4 1.1     # millimetres per channel: red spreads furthest
    skin_strength 1             # 0: the opaque look
    skin_max_pixels 24          # the spread's limit on screen, however close the camera

Only skin's diffuse light spreads. Highlights, eyes, eyebrows and everything that isn't
skin stay sharp. The scene pass draws skin's highlights. A skin pass over the scene's
depth then writes its diffuse light (before the colour) and its colour, and two blurs
(across, then down) spread the light. A composite adds it back, times the colour.

The blur is a Gaussian per channel, with a radius of `skin_radius` projected to pixels at
that depth. It skips neighbours more than a few radii away in depth, and never crosses
from one character's skin to another's: each skin draw on screen gets its own receiver
id (up to 255 a frame). All of this runs only inside the rectangle the skin covers.

**Setting.** `skin_quality low` uses fewer samples than `high`. `off` draws skin as an
ordinary opaque surface, exactly as `skin_strength 0` does, with no pass and no memory.

### Known limits

- The profile is a Gaussian per channel, not a measured diffusion profile, and it lives
  in the material (`skin_radius`), not in a shared profile asset.
- The blur tests depth and receiver, not normals, so a sharp crease can bleed a little.
- There's no light through thin parts, such as red ears against a light.

## Hair cards

    family hair_card
    alpha_mode mask
    double_sided true
    hair_anisotropy 0.8        # 0 round .. 1 a thin band across the strands
    hair_direction v           # the strands run root to tip down the texture (u: left to right)
    texture base_color "textures/strands.png"

Hair is drawn on alpha-tested cards. Their shadows are alpha-tested too, so strands cast
strand-shaped shadows. The highlight uses anisotropic GGX (exact Smith masking) along
the strands. The strands' direction comes from the mesh's tangents: a card whose UVs have
no tangents keeps the round highlight. The environment's reflection bends toward the
band (an approximation, since the environment isn't convolved anisotropically).

**Setting.** `hair_quality low` draws the round highlight of an ordinary surface.

## Eyes

There is no eye family. An eye combines the families: a clear-coated standard (or a
`transmission` thin) cornea over a standard iris.

## Cost and fallbacks at a glance

| Setting | Off draws | Pass | Memory |
|---|---|---|---|
| `clearcoat` | the base surface | none (in the scene pass) | none |
| `transmission_quality` | simplified blended glass | *transmission background copy* and *scene (see-through)* | the copy with mips (about 1.33x the scene colour) |
| `skin_quality` | skin as opaque standard | *skin*, two blurs, a composite | three targets, 20 bytes a pixel |
| `hair_quality` | the round highlight | none | none |

The copy and the skin targets are made the first frame that needs them and freed 300
frames after the last one, or as soon as the setting goes off. Their pipelines are
compiled the first frame that needs them, so a game without glass or skin never pays
for them. A scene without these families renders exactly as before, whatever the
settings.

## Tools

- `player --material-info file.swmaterial`: the family, what it draws as with the
  current settings and on Low, the fallback, and the passes it needs.
- `player --check`: warns about another family's values, blending on glass,
  `transmission 0`, and similar mistakes.
- `player --view transmission` and `--view shading`, and the editor's *Glass
  (transmission)* and *Shading families* views. In the families view, coats are yellow,
  skin is orange with diffusion and brown without, hair is cyan with the band and blue
  without, and glass is green through its pass and magenta when simplified.
- The graphics report's `shading` block: draws per family, which passes ran, and their
  bytes.
- The Material window shows the family's groups only, and what the material draws as.

The glTF importer doesn't read `KHR_materials_clearcoat` or
`KHR_materials_transmission` yet. It warns, and a `.swmaterial` with the model as its
parent adds them.
