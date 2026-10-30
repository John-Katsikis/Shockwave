# Decals

A decal paints onto the surfaces around it: a poster on a wall, grime in a corner, a
bullet hole, a scorch mark. It's a box. Whatever opaque surface the camera sees inside
the box takes the decal's colour (and optionally its roughness and metallic), so a decal
follows bumps, corners and steps without anyone modelling it onto them.

Decals are switched by the player's `decals_quality` graphics setting (docs/Graphics.md).
Off draws none and costs nothing. Scenes without decals look the same at any setting.

## The Decal component

Give an entity a Transform and a **Decal** (Add Component > Rendering). The box is the
entity's Transform, scale included, times `size` (metres): width, height and how deep it
reaches. It **projects along the entity's -Z**, so it lands on surfaces facing its +Z.
The texture reads upright with u along +X and v down -Y. The editor draws a selected
decal's box, heavier on the face it projects from, with an arrow along the projection and
a tick towards the top of the texture.

| Field | Meaning |
|---|---|
| `material` | A `.swmaterial`: its base colour texture and colour (alpha is coverage), and its roughness and metallic. Its normal, emissive and other maps aren't decal channels and are ignored. |
| `texture`, `tint` | Without a material: the colour texture (alpha is coverage), times tint. With one, tint multiplies the material's colour. |
| `size` | Metres, along X, Y and Z. Keep Z small: just enough to cover the surface's bumps. |
| `opacity` | 0..1. |
| `albedo` | Changes base colour (on by default). |
| `roughness_metallic` | Changes roughness and metallic, together, to the material's (or `roughness` and `metallic` without one). A wet patch, a polished scuff. |
| `sort_order` | Where decals overlap, higher draws over lower. Equal ones draw in the scene's order, and impact marks after authored decals, oldest first. |
| `receiver_mask` | Eight layer bits; the decal lands only on surfaces sharing one (below). 255, the default, is every layer. |
| `angle_fade` | Surfaces turned this many degrees away from facing the decal start to fade; side-on surfaces get nothing, so a wall decal doesn't smear down the floor. |
| `essential` | A gameplay marker (an objective arrow): kept before decoration when the budget is short. Off still draws none, so don't rely on a decal alone for anything a player must see. |
| `enabled` | |

`player --check` rejects a zero or negative size, opacity outside 0..1, a decal that
changes nothing (both channels off), a mask past 255 and a material that won't resolve.

### Placing decals in the editor

Hold **Alt** while dropping a material or texture from the content browser onto a
surface: instead of being assigned to the object, it becomes a Decal at the point you
dropped it, facing out of the surface (its top towards world up, or -Z on floors and
ceilings), a metre square and 20 cm deep. Undo removes it. Move, rotate, scale, snap and
duplicate it like any entity; the *Decals* shading view (and `player --view decals`)
shows where decals land: red where colour lands, green where roughness and metallic
do, yellow where both.

## Receivers

Every opaque mesh and model is on decal layer 1. A **DecalReceiver** component puts it on
other layers (`layers`, eight bits; 0 takes no decals). A decal lands only where its
`receiver_mask` shares a bit with the surface's layers. So for a sign on a door, put the
door on layer 2 (`DecalReceiver`, layers 2), parent the decal to the door (or its hinge),
and give it `receiver_mask 2`: the box can reach past the door's edges and into the wall
behind, and only the door takes it.

Transparent (blended) surfaces never take decals: they'd show the decal floating on glass
while the surface behind showed it too.

## Impact marks

Gameplay code makes marks with `spawnImpactDecal` (Engine/Decals.h): the hit point and
surface normal, what was hit, the Decal to use, a seed and a lifetime.

```cpp
ImpactDecal impact;
impact.position = hit.point;
impact.normal = hit.normal;
impact.receiver = hit.entity;   // stuck to it: a mark on a door swings with the door
impact.decal = bulletHole;      // a Decal set up once, say from a prefab
impact.decal.size = {0.12f, 0.12f, 0.1f};
impact.seed = shotId;           // turns each mark differently, the same on every machine
impact.lifetime = 20.0f;        // seconds, the last `fade` of them fading out
spawnImpactDecal(world, impact);
```

The mark is an ordinary entity: a Transform (under the receiver), the Decal and a
**DecalLifetime** (`seconds`, `fade`; 0 seconds lasts until removed). It's destroyed when
its time is up, and when the entity it's stuck to is destroyed, so marks never move to a
new entity that reuses the old one's slot. A world keeps at most 256 marks by default
(`maxImpactDecals`); making another removes the oldest. It changes the world's structure,
so call it from an exclusive system or outside the schedule. Marks on skinned characters
need a different approach (they'd slide over the skin) and aren't supported.

## How it's drawn

After the surface prepass (depth, normals and each surface's receiver layers), each
decal draws its box's far faces, so every pixel that might lie inside is shaded once,
even with the camera inside the box. For the visible surface at that pixel the shader
checks it's inside the box, on a shared layer and facing the decal, then blends the
decal's colour and coverage "over" what earlier decals left, in two small buffers.
Opaque surfaces then apply them once, before lighting:

    albedo = base (1 - coverage) + premultiplied decal colour

so decals are lit, shadowed and reflected like the surface they're on, in linear colour.
Per frame the renderer culls decals outside the view, then keeps up to the budget
(essential first, then nearest, then the scene order) and draws them in sort order.
`--graphics-report` lists how many were submitted, visible, drawn and over budget, and
the buffers' memory under *surface buffers*.

The rules are in Renderer/Decals.h and checked by Tests/DecalTest.cpp; the decal-wall
graphics fixture (Tests/fixtures/graphics) is the visual check: docs/GraphicsVerification.md.
