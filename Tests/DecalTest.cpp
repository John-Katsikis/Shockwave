// Projected decals (G08, A03; docs/Decals.md): where a decal lands in its box, the texture
// orientation, the grazing fade, how decals compose and apply, which ones a frame keeps under
// a budget, the box culling, the Decal component's translation, impact marks (orientation,
// sticking to a receiver, eviction, lifetimes, receivers that die or are reused) and the
// decals_quality setting. No GPU: these run anywhere. The expected values are worked out by
// hand here, not by calling the helpers twice.

#include <Engine/Decals.h>
#include <Engine/GraphicsSettings.h>
#include <Engine/Hierarchy.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
bool near(float a, float b, float eps = 1e-5f) { return std::abs(a - b) <= eps; }
bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps)));
}

// A decal size w x h x d at position, facing +Z (unrotated).
DecalDesc box(glm::vec3 position, glm::vec3 size = glm::vec3(1.0f), uint64_t order = 0) {
    DecalDesc d;
    d.boxToWorld = glm::scale(glm::translate(glm::mat4(1.0f), position), size);
    d.order = order;
    return d;
}

void testBoxAndUv() {
    // A 2 x 1 x 0.5 box at (10, 0, 0), turned 90 degrees about Y: its +Z faces world +X.
    DecalDesc d;
    d.boxToWorld = glm::scale(
        glm::rotate(glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}), glm::radians(90.0f), {0.0f, 1.0f, 0.0f}),
        {2.0f, 1.0f, 0.5f});
    check(validDecal(d), "a turned, stretched box is valid");
    // Local x runs along world -Z after the turn: world (10, 0, -1) is local x = +0.5.
    glm::vec3 local;
    check(decalLocal(d, {10.0f, 0.0f, -1.0f}, local) && near(local, {0.5f, 0.0f, 0.0f}), "box edge along its x");
    check(decalLocal(d, {10.2f, 0.4f, 0.0f}, local) && near(local, {0.0f, 0.4f, 0.4f}), "a point inside the box");
    check(!decalLocal(d, {10.3f, 0.0f, 0.0f}, local), "past its projection depth is outside");
    check(!decalLocal(d, {10.0f, 0.0f, -1.1f}, local), "past its width is outside");
    check(!decalLocal(d, {10.0f, 0.6f, 0.0f}, local), "past its height is outside");
    // Texture: u along +x, v down -y, so the top-left of the texture is at (-x, +y).
    const glm::vec2 topLeft = decalUv({-0.5f, 0.5f, 0.0f}), bottomRight = decalUv({0.5f, -0.5f, 0.0f});
    check(near(topLeft.x, 0.0f) && near(topLeft.y, 0.0f), "uv (0, 0) is the top left");
    check(near(bottomRight.x, 1.0f) && near(bottomRight.y, 1.0f), "uv (1, 1) is the bottom right");
    check(near(decalFacing(d), {1.0f, 0.0f, 0.0f}), "the box faces along its +Z, here world +X");
}

void testGrazingFade() {
    DecalDesc d = box({0.0f, 0.0f, 0.0f});
    d.angleFadeDegrees = 60.0f;
    auto at = [](float degrees) {
        const float r = glm::radians(degrees);
        return glm::vec3(std::sin(r), 0.0f, std::cos(r));   // turned from +Z by degrees
    };
    check(near(decalAngleFade(d, at(0.0f)), 1.0f), "facing the decal: full");
    check(near(decalAngleFade(d, at(59.0f)), 1.0f), "inside the fade angle: full");
    // cos 75 / cos 60 = 0.258819 / 0.5
    check(near(decalAngleFade(d, at(75.0f)), 0.517638f, 1e-4f), "between the fade angle and side-on: linear in cosine");
    check(near(decalAngleFade(d, at(90.0f)), 0.0f, 1e-5f), "side-on: nothing");
    check(decalAngleFade(d, at(135.0f)) == 0.0f, "facing away: nothing");
    check(decalAngleFade(d, at(180.0f)) == 0.0f, "the back of a wall: nothing");
    d.angleFadeDegrees = 0.0f;
    check(near(decalAngleFade(d, at(60.0f)), 0.5f, 1e-5f), "fade angle 0 fades from facing straight on");
}

void testComposition() {
    // Partial alpha on its own: 0.2 * (1 - 0.25) + 1 * 0.25 = 0.4.
    DecalAccumulation one;
    composeDecal(one, glm::vec3(1.0f), 0.25f);
    check(near(applyDecals(glm::vec3(0.2f), one), glm::vec3(0.4f)), "one quarter-opaque white decal on 0.2 grey");
    check(near(applyDecals(glm::vec3(0.2f), DecalAccumulation{}), glm::vec3(0.2f)), "no decals: the surface as it was");

    // Two overlapping, red (a = 0.5) then blue (a = 0.8) over base b:
    //   b (1 - 0.5)(1 - 0.8) + red 0.5 (1 - 0.8) + blue 0.8
    const glm::vec3 base(0.5f, 0.5f, 0.5f), red(1.0f, 0.0f, 0.0f), blue(0.0f, 0.0f, 1.0f);
    DecalAccumulation two;
    composeDecal(two, red, 0.5f);
    composeDecal(two, blue, 0.8f);
    const glm::vec3 expected = base * 0.5f * 0.2f + red * 0.5f * 0.2f + blue * 0.8f;
    check(near(applyDecals(base, two), expected), "two decals compose like painting one over the other");
    check(near(two.alpha, 1.0f - 0.5f * 0.2f), "their coverage is 1 - (1 - 0.5)(1 - 0.8)");
    DecalAccumulation swapped;
    composeDecal(swapped, blue, 0.8f);
    composeDecal(swapped, red, 0.5f);
    check(!near(applyDecals(base, swapped), expected), "the order matters: the later one is on top");
    check(near(swapped.alpha, two.alpha), "coverage doesn't depend on the order");

    // Linear colour: half-covering black with white gives linear 0.5, which is sRGB 0.735,
    // not sRGB 0.5 (what composing encoded values would give).
    DecalAccumulation half;
    composeDecal(half, glm::vec3(1.0f), 0.5f);
    const float linear = applyDecals(glm::vec3(0.0f), half).x;
    const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    check(near(linear, 0.5f) && near(encoded, 0.7354f, 1e-3f), "decals blend in linear colour");

    // Opacity is clamped, never more than covering.
    DecalAccumulation over;
    composeDecal(over, glm::vec3(1.0f), 2.0f);
    check(near(over.alpha, 1.0f), "opacity above 1 covers fully, no more");
}

void testValidation() {
    check(validDecal(box({0, 0, 0})), "a unit box is valid");
    check(!validDecal(box({0, 0, 0}, {0.0f, 1.0f, 1.0f})), "zero width is rejected");
    check(!validDecal(box({0, 0, 0}, {1.0f, 1.0f, 0.0f})), "zero depth is rejected");
    check(!validDecal(box({0, 0, 0}, {1.0f, NAN, 1.0f})), "a non-finite size is rejected");
    check(!validDecal(box({INFINITY, 0, 0})), "a non-finite position is rejected");
    DecalDesc opaque = box({0, 0, 0});
    opaque.color.a = 1.5f;
    check(!validDecal(opaque), "opacity above 1 is rejected");
    opaque.color.a = -0.1f;
    check(!validDecal(opaque), "negative opacity is rejected");

    Decal decal;
    check(decalProblem(decal, nullptr).empty(), "the default Decal is fine");
    decal.size = {1.0f, 0.0f, 1.0f};
    check(!decalProblem(decal, nullptr).empty(), "a Decal with zero height is a problem");
    decal.size = {1.0f, -1.0f, 1.0f};
    check(!decalProblem(decal, nullptr).empty(), "a Decal with negative height is a problem");
    decal = {};
    decal.opacity = 1.01f;
    check(!decalProblem(decal, nullptr).empty(), "a Decal more than opaque is a problem");
    decal = {};
    decal.albedo = false;
    check(!decalProblem(decal, nullptr).empty(), "a Decal that changes nothing is a problem");
    decal.roughnessMetallic = true;
    check(decalProblem(decal, nullptr).empty(), "roughness and metallic alone is fine");
    decal = {};
    decal.receiverMask = 256;
    check(!decalProblem(decal, nullptr).empty(), "a receiver mask past 8 layers is a problem");
}

void testComponentTranslation() {
    Decal decal;
    decal.size = {2.0f, 1.0f, 0.25f};
    decal.opacity = 0.8f;
    decal.tint = {0.5f, 1.0f, 1.0f, 0.5f};
    decal.roughnessMetallic = true;
    decal.roughness = 0.9f;
    decal.metallic = 0.25f;
    decal.sortOrder = 3;
    decal.receiverMask = 0x1F2;   // bits past 8 layers are dropped
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f});
    const DecalDesc d = decalDesc(decal, world, nullptr, 7, 0.5f);
    check(
        near(glm::length(glm::vec3(d.boxToWorld[0])), 2.0f) && near(glm::length(glm::vec3(d.boxToWorld[2])), 0.25f),
        "the box is the entity's matrix times the size");
    check(near(glm::vec3(d.boxToWorld[3]), {1.0f, 2.0f, 3.0f}), "the box is centred on the entity");
    check(near(d.color.a, 0.8f * 0.5f * 0.5f), "alpha is opacity times tint alpha times the lifetime fade");
    check(near(d.color.r, 0.5f), "tint multiplies the colour");
    check(d.channels == (kDecalAlbedo | kDecalRoughnessMetallic), "both channels");
    check(near(d.roughness, 0.9f) && near(d.metallic, 0.25f), "roughness and metallic without a material");
    check(d.receiverMask == 0xF2u && d.sortOrder == 3 && d.order == 7, "mask, sort order and order carried over");

    MaterialAsset material;
    material.baseColor = {0.5f, 0.5f, 0.5f, 0.5f};
    material.baseColorTexture = TextureHandle{42};
    material.draw.roughness = 0.1f;
    material.draw.metallic = 1.0f;
    const DecalDesc m = decalDesc(decal, world, &material, 7);
    check(m.texture.id == 42 && near(m.roughness, 0.1f) && near(m.metallic, 1.0f), "a material supplies the surface");
    check(near(m.color.a, 0.5f * 0.5f * 0.8f) && near(m.color.r, 0.25f), "material colour times tint and opacity");

    // Authored decals keep scene order; impact marks draw after them, oldest first.
    DecalLifetime first, second;
    first.spawn = 1;
    second.spawn = 2;
    check(decalOrder(Entity{5, 0}, nullptr) < decalOrder(Entity{100000, 0}, nullptr), "authored decals by entity");
    check(
        decalOrder(Entity{100000, 0}, nullptr) < decalOrder(Entity{0, 0}, &first), "impact marks after authored ones");
    check(decalOrder(Entity{9, 0}, &first) < decalOrder(Entity{1, 0}, &second), "impact marks in the order made");
}

void testSelection() {
    // Sorted by sort order, then order, whatever order they're handed over in.
    std::vector<DecalDesc> decals = {
        box({0, 0, 0}, glm::vec3(1), 5), box({0, 0, 0}, glm::vec3(1), 2), box({0, 0, 0}, glm::vec3(1), 9)};
    decals[2].sortOrder = -1;
    std::vector<uint32_t> kept = selectDecals(decals, glm::vec3(0.0f), 16);
    check(kept == std::vector<uint32_t>{2, 1, 0}, "drawing order: sort order, then order");

    // Ten decals at x = 1..10, budget 4, the farthest one essential: it and the three
    // nearest are kept.
    decals.clear();
    for (int i = 0; i < 10; ++i) decals.push_back(box({float(i + 1), 0, 0}, glm::vec3(1), uint64_t(100 + i)));
    decals[9].essential = true;
    kept = selectDecals(decals, glm::vec3(0.0f), 4);
    std::vector<uint64_t> orders;
    for (uint32_t i : kept) orders.push_back(decals[i].order);
    check(orders == std::vector<uint64_t>{100, 101, 102, 109}, "budget keeps the essential one, then the nearest");
    // The same decals handed over backwards keep the same ones.
    std::vector<DecalDesc> reversed(decals.rbegin(), decals.rend());
    std::vector<uint64_t> again;
    for (uint32_t i : selectDecals(reversed, glm::vec3(0.0f), 4)) again.push_back(reversed[i].order);
    check(again == orders, "pruning doesn't depend on the order they arrive in");
    // Equal distances: the lower order wins, so ties settle the same way every frame.
    decals = {box({1, 0, 0}, glm::vec3(1), 8), box({-1, 0, 0}, glm::vec3(1), 3)};
    kept = selectDecals(decals, glm::vec3(0.0f), 1);
    check(kept.size() == 1 && decals[kept[0]].order == 3, "equal distances: the lower order is kept");
    check(selectDecals(decals, glm::vec3(0.0f), 0).empty(), "budget 0 (decals off) keeps none");
    check(
        decalBudget(DecalQuality::Off) == 0 && decalBudget(DecalQuality::Low) == 64 &&
            decalBudget(DecalQuality::Medium) == 256 && decalBudget(DecalQuality::High) == 1024,
        "budgets per quality");
}

void testCulling() {
    // Camera at the origin looking down -Z, reverse-Z infinite projection.
    const float f = 1.0f / std::tan(glm::radians(30.0f));
    glm::mat4 projection(0.0f);
    projection[0][0] = f;
    projection[1][1] = f;
    projection[2][3] = -1.0f;
    projection[3][2] = 0.1f;   // near plane
    check(decalMayBeVisible(box({0, 0, -5}), projection), "in front of the camera");
    check(!decalMayBeVisible(box({0, 0, 5}), projection), "behind the camera");
    check(!decalMayBeVisible(box({50, 0, -5}), projection), "far off to the side");
    check(decalMayBeVisible(box({0, 0, 0}, glm::vec3(4.0f)), projection), "the camera inside the box still sees it");
    check(decalMayBeVisible(box({0, 0, -1000}), projection), "however far away (no far plane)");
}

void testImpactOrientation() {
    // A wall facing +X: the mark's +Z is the normal, +Y stays world up, +X follows.
    glm::mat3 r = glm::mat3_cast(impactDecalRotation({1.0f, 0.0f, 0.0f}, 0));
    check(
        near(r[2], {1.0f, 0.0f, 0.0f}) && near(r[1], {0.0f, 1.0f, 0.0f}) && near(r[0], {0.0f, 0.0f, -1.0f}),
        "upright on a wall facing +X");
    r = glm::mat3_cast(impactDecalRotation({0.0f, 0.0f, 2.0f}, 0));
    check(near(r[2], {0.0f, 0.0f, 1.0f}) && near(r[1], {0.0f, 1.0f, 0.0f}), "upright on a wall facing +Z, any length");
    // A floor: +Y can't be up, so the texture's top points to world -Z.
    r = glm::mat3_cast(impactDecalRotation({0.0f, 1.0f, 0.0f}, 0));
    check(near(r[2], {0.0f, 1.0f, 0.0f}) && near(r[1], {0.0f, 0.0f, -1.0f}), "on a floor the top points to -Z");
    // A seed turns it about the normal only, the same way every time.
    const glm::quat a = impactDecalRotation({1.0f, 0.0f, 0.0f}, 12345),
                    b = impactDecalRotation({1.0f, 0.0f, 0.0f}, 12345);
    check(near(glm::mat3_cast(a)[2], {1.0f, 0.0f, 0.0f}), "a seeded mark still faces along the normal");
    check(a == b, "the same seed turns the same way");
    check(
        !near(glm::mat3_cast(a)[1], glm::mat3_cast(impactDecalRotation({1.0f, 0.0f, 0.0f}, 12346))[1]),
        "neighbouring seeds turn differently");
}

void testImpactMarks() {
    World world;
    Decal decal;
    decal.size = {0.2f, 0.2f, 0.1f};

    // Stuck to a door that moves: the mark's world position follows it.
    const Entity door = world.create();
    world.add<Transform>(door, {{5.0f, 0.0f, 0.0f}});
    ImpactDecal impact;
    impact.position = {5.0f, 1.0f, 0.5f};
    impact.normal = {0.0f, 0.0f, 1.0f};
    impact.receiver = door;
    impact.decal = decal;
    impact.lifetime = 10.0f;
    impact.fade = 2.0f;
    const Entity mark = spawnImpactDecal(world, impact);
    check(world.alive(mark) && parentOf(world, mark) == door, "a mark is stuck to what it hit");
    check(near(glm::vec3(worldMatrix(world, mark)[3]), {5.0f, 1.0f, 0.5f}), "it starts where it hit");
    world.get<Transform>(door)->position.x = 7.0f;
    check(near(glm::vec3(worldMatrix(world, mark)[3]), {7.0f, 1.0f, 0.5f}), "it moves with the door");

    // Lifetime: full until the fade, half way through the fade at 9 s, gone at 10 s.
    updateDecalLifetimes(world, 7.0f);
    check(world.alive(mark) && near(decalLifetimeFade(*world.get<DecalLifetime>(mark)), 1.0f), "full before the fade");
    updateDecalLifetimes(world, 2.0f);
    check(
        world.alive(mark) && near(decalLifetimeFade(*world.get<DecalLifetime>(mark)), 0.5f),
        "half way through the fade");
    updateDecalLifetimes(world, 1.0f);
    check(!world.alive(mark), "gone when its time is up");

    // A receiver that's destroyed takes its marks with it, and an entity reusing its index
    // doesn't inherit them.
    const Entity second = spawnImpactDecal(world, impact);
    world.destroy(door);
    // Worlds hold a freed index back until over a thousand are free; free that many.
    std::vector<Entity> spare;
    for (int i = 0; i < 1100; ++i) spare.push_back(world.create());
    for (Entity e : spare) world.destroy(e);
    Entity reused = world.create();
    for (int i = 0; i < 4096 && reused.index != door.index; ++i) reused = world.create();   // until it comes back
    world.add<Transform>(reused);
    check(reused.index == door.index && reused.generation != door.generation, "the door's index is reused");
    check(parentOf(world, second) != reused, "a reused entity isn't the mark's receiver");
    updateDecalLifetimes(world, 0.0f);
    check(!world.alive(second), "a destroyed receiver's marks go with it");

    // Lasting marks: lifetime 0 never expires.
    impact.receiver = kNullEntity;
    impact.lifetime = 0.0f;
    const Entity lasting = spawnImpactDecal(world, impact);
    updateDecalLifetimes(world, 1.0e6f);
    check(
        world.alive(lasting) && near(decalLifetimeFade(*world.get<DecalLifetime>(lasting)), 1.0f),
        "lifetime 0 lasts until removed");
    world.destroy(lasting);

    // Past the cap the oldest go first.
    std::vector<Entity> marks;
    for (int i = 0; i < 5; ++i) {
        impact.position.x = float(i);
        marks.push_back(spawnImpactDecal(world, impact, 3));
    }
    check(world.count<DecalLifetime>() == 3, "no more than the cap");
    check(
        !world.alive(marks[0]) && !world.alive(marks[1]) && world.alive(marks[2]) && world.alive(marks[3]) &&
            world.alive(marks[4]),
        "the oldest were removed");
    check(
        world.get<DecalLifetime>(marks[4])->spawn > world.get<DecalLifetime>(marks[2])->spawn,
        "later marks draw over earlier ones");

    // Bad input makes nothing.
    impact.normal = glm::vec3(0.0f);
    check(!spawnImpactDecal(world, impact).valid(), "a zero normal makes no mark");
    impact.normal = {0.0f, 1.0f, 0.0f};
    impact.decal.size.x = 0.0f;
    check(!spawnImpactDecal(world, impact).valid(), "a zero-size mark isn't made");
}

void testRegistration() {
    const ComponentInfo* decal = findComponent("Decal");
    check(decal && decal->category == "Rendering", "Decal is registered");
    check(findComponent("DecalReceiver") && findComponent("DecalLifetime"), "DecalReceiver and DecalLifetime too");
    if (decal) {
        std::vector<std::string> names;
        for (const Field& f : decal->fields) names.push_back(f.name);
        for (const char* expected :
             {"material", "texture", "size", "opacity", "tint", "albedo", "roughness_metallic", "sort_order",
              "receiver_mask", "angle_fade", "essential", "enabled"})
            check(std::find(names.begin(), names.end(), expected) != names.end(), std::string("Decal has ") + expected);
    }
}

void testSetting() {
    GraphicsSettings s;
    std::string error;
    check(s.decals == DecalQuality::High, "decals default to high");
    check(setGraphicsSetting(s, "decals_quality", "off", error) && s.decals == DecalQuality::Off, "decals_quality off");
    check(
        !setGraphicsSetting(s, "decals_quality", "ultra", error) && s.decals == DecalQuality::Off,
        "an unknown value is refused and changes nothing");
    check(graphicsPreset(GraphicsPreset::Low).decals == DecalQuality::Low, "the low preset draws few decals");
    check(graphicsPreset(GraphicsPreset::Medium).decals == DecalQuality::Medium, "medium preset");
    check(graphicsPreset(GraphicsPreset::Ultra).decals == DecalQuality::High, "ultra preset");
    const GraphicsSettingInfo* info = findGraphicsSetting("decals_quality");
    check(info && info->preset && info->apply == GraphicsApply::Targets, "a preset key that makes targets");
}

}   // namespace

int main() {
    testBoxAndUv();
    testGrazingFade();
    testComposition();
    testValidation();
    testComponentTranslation();
    testSelection();
    testCulling();
    testImpactOrientation();
    testImpactMarks();
    testRegistration();
    testSetting();
    if (failures) {
        std::fprintf(stderr, "%d decal check(s) failed\n", failures);
        return 1;
    }
    std::printf("decal tests passed\n");
    return 0;
}
