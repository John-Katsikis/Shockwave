// Foliage (W2a, docs/Foliage.md): the .swfoliage format and its errors, the 24-byte
// instances, sorting into 16 m cells, the fixed random numbers, density and fade. No GPU.
#include <Assets/FoliageData.h>
#include <Engine/Foliage.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

const char* kGood = "swfoliage 1\n"
                    "# a comment\n"
                    "instances \"foliage/meadow.instances\"\n"
                    "type \"models/grass.glb\"\n"
                    "  distance 60\n"
                    "  fade 15\n"
                    "  shadows false\n"
                    "  wind 0.15\n"
                    "type \"models/rock.glb\"\n";

void format() {
    FoliageData f;
    std::string error;
    check(parseFoliage(kGood, f, error), "the example parses: " + error);
    check(f.instancesFile == "foliage/meadow.instances", "instances read");
    check(f.types.size() == 2, "two types");
    if (f.types.size() == 2) {
        check(
            f.types[0].model == "models/grass.glb" && f.types[0].distance == 60.0f && f.types[0].fade == 15.0f,
            "type 0's distance and fade");
        check(!f.types[0].shadows && std::abs(f.types[0].wind - 0.15f) < 1e-6f, "type 0's shadows and wind");
        check(f.types[1] == FoliageType{"models/rock.glb"}, "type 1 keeps the defaults");
    }
    FoliageData again;
    check(parseFoliage(foliageText(f), again, error), "the canonical text parses: " + error);
    check(foliageText(again) == foliageText(f) && again.types == f.types, "the canonical text round-trips");

    struct Bad {
        std::string text;
        std::string expect;
    };
    const Bad bad[] = {
        {"", "empty"},
        {"swfoliage 2\n", "line 1: only swfoliage version 1"},
        {"swterrain 1\n", "line 1: the first line must be"},
        {"swfoliage 1\ntype \"a.glb\"\n", "instances is missing"},
        {"swfoliage 1\ninstances \"a\"\ninstances \"b\"\n", "line 3: instances given twice"},
        {"swfoliage 1\ninstances \"a\"\ndistance 5\n", "line 3: distance needs a type line"},
        {"swfoliage 1\ninstances \"a\"\ntype \"a.glb\"\n  distance 5\n  distance 6\n", "line 5: distance given twice"},
        {"swfoliage 1\ninstances \"a\"\ntype \"a.glb\"\n  distance 0.5\n", "line 4: distance must be"},
        {"swfoliage 1\ninstances \"a\"\ntype \"a.glb\"\n  wind 2\n", "line 4: wind must be"},
        {"swfoliage 1\ninstances \"a\"\ntype \"a.glb\"\n  shadows maybe\n", "line 4: shadows must be"},
        {"swfoliage 1\ninstances \"a\"\ntype \"a.glb\"\n  colour red\n", "line 4: unknown key colour"},
        {"swfoliage 1\ninstances \"a\"\ntype \"a.glb\"\n  distance 10\n  fade 20\n", "fade is longer"},
    };
    for (const Bad& b : bad) {
        FoliageData x;
        std::string why;
        const bool parsed = parseFoliage(b.text, x, why);
        check(
            !parsed && why.find(b.expect) != std::string::npos,
            "\"" + b.text + "\" fails with \"" + b.expect + "\", got \"" + why + "\"");
    }
}

void instances() {
    std::vector<FoliageInstance> in(3);
    in[0] = {0, {1.5f, 2.0f, -3.0f}, 0.25f, 1.5f};
    in[1] = {1, {-20.0f, 0.0f, 40.0f}, 3.0f, 0.5f};
    in[2] = {1, {100.0f, -1.0f, 0.0f}, -1.0f, 2.0f};
    const std::vector<uint8_t> bytes = encodeFoliageInstances(in);
    check(bytes.size() == 3 * kFoliageInstanceBytes, "24 bytes an instance");
    check(bytes[2] == 0 && bytes[3] == 0, "the unused field is 0");
    std::vector<FoliageInstance> out;
    std::string error;
    check(decodeFoliageInstances(bytes, 2, out, error), "decodes: " + error);
    check(out.size() == 3, "three back");
    for (size_t i = 0; i < out.size() && i < in.size(); ++i) {
        check(
            out[i].type == in[i].type && out[i].position == in[i].position && out[i].yaw == in[i].yaw &&
                out[i].scale == in[i].scale,
            "instance " + std::to_string(i) + " round-trips");
        check(out[i].random == foliageRandom(uint32_t(i)), "instance " + std::to_string(i) + "'s random number");
    }
    std::vector<uint8_t> shortBytes(bytes.begin(), bytes.end() - 1);
    check(
        !decodeFoliageInstances(shortBytes, 2, out, error) && error.find("not a multiple of 24") != std::string::npos,
        "a count of bytes not divisible by 24 fails: " + error);
    check(
        !decodeFoliageInstances(bytes, 1, out, error) && error.find("instance 1 is type 1") != std::string::npos,
        "a type out of range fails: " + error);

    // Files: the header and the instances file beside it, read back sorted.
    const fs::path content = fs::temp_directory_path() / "shockwave_foliage_test" / "Content";
    fs::remove_all(content.parent_path());
    fs::create_directories(content / "foliage");
    FoliageData f;
    check(parseFoliage(kGood, f, error), "parses");
    f.instances = in;
    check(writeFoliage(content / "foliage/meadow.swfoliage", content, f, error), "writes: " + error);
    check(fs::file_size(content / "foliage/meadow.instances") == 72, "the instances file is 72 bytes");
    FoliageData read;
    check(readFoliage(content / "foliage/meadow.swfoliage", content, read, error), "reads: " + error);
    check(read.instances.size() == 3 && read.cells.size() == 3, "three instances in three cells");
    {
        std::ofstream bad(content / "foliage/meadow.instances", std::ios::binary | std::ios::app);
        bad << 'x';
    }
    check(
        !readFoliage(content / "foliage/meadow.swfoliage", content, read, error) &&
            error.find("not a multiple of 24") != std::string::npos,
        "a damaged instances file fails to read: " + error);
    fs::remove_all(content.parent_path());
}

void cells() {
    FoliageData f;
    f.types.resize(2);
    // Type 1 first in the file, then type 0 across two cells, one instance on a cell edge.
    f.instances = {
        {1, {5.0f, 0.0f, 5.0f}},   {0, {17.0f, 0.0f, 1.0f}}, {0, {1.0f, 0.0f, 1.0f}},
        {0, {16.0f, 2.0f, 15.0f}}, {0, {-0.5f, 0.0f, 3.0f}}, {0, {3.0f, 0.0f, 2.0f}, 0.0f, 3.0f},
    };
    sortFoliage(f);
    check(f.cells.size() == 4, "four cells (type 0: x -1, 0, 1; type 1: 0), got " + std::to_string(f.cells.size()));
    for (size_t i = 1; i < f.instances.size(); ++i) check(f.instances[i - 1].type <= f.instances[i].type, "by type");
    uint32_t next = 0;
    for (const FoliageCell& c : f.cells) {
        check(c.first == next, "cells are contiguous");
        next = c.first + c.count;
        for (uint32_t i = c.first; i < c.first + c.count; ++i) {
            const FoliageInstance& in = f.instances[i];
            check(in.type == c.type, "a cell holds one type");
            check(
                int(std::floor(in.position.x / 16.0f)) == c.coord.x &&
                    int(std::floor(in.position.z / 16.0f)) == c.coord.y,
                "an instance is in its 16 m cell");
            check(in.position.x >= c.bounds.min.x && in.position.x <= c.bounds.max.x, "inside the cell's bounds");
        }
    }
    check(next == f.instances.size(), "every instance is in a cell");
    if (f.cells.size() == 4) {
        check(f.cells[0].type == 0 && f.cells[0].coord == glm::ivec2(-1, 0), "the first cell is type 0 at x -1");
        check(f.cells[1].coord == glm::ivec2(0, 0) && f.cells[1].count == 2, "two at the origin's cell");
        check(f.cells[1].maxScale == 3.0f, "the cell knows its largest scale");
        check(f.cells[2].coord == glm::ivec2(1, 0) && f.cells[2].count == 2, "16 belongs to the next cell");
        check(f.cells[2].bounds.max.y == 2.0f, "a cell's bounds include height");
        check(f.cells[3].type == 1, "type 1 last");
    }
    // Distance to a cell's box.
    {
        FoliageCell c;
        c.bounds = {glm::vec3(0.0f), glm::vec3(10.0f, 1.0f, 10.0f)};
        check(foliageCellDistance(c, {5, 0.5f, 5}) == 0.0f, "inside: 0");
        check(std::abs(foliageCellDistance(c, {13, 0.5f, 14}) - 5.0f) < 1e-5f, "a corner away: 3-4-5");
    }
    // The placement: translate, turn, scale.
    const FoliageInstance in{0, {1, 2, 3}, 1.5707963f, 2.0f};
    const glm::vec4 x = foliageMatrix(in) * glm::vec4(1, 0, 0, 1);
    check(
        std::abs(x.x - 1.0f) < 1e-4f && std::abs(x.y - 2.0f) < 1e-4f && std::abs(x.z - 1.0f) < 1e-4f,
        "a quarter turn about Y takes +X to -Z, scaled by 2");
}

// A meadow of n type-0 instances in one cell, sorted.
FoliageData meadow(uint32_t n, float distance, float fade) {
    FoliageData f;
    FoliageType type;
    type.model = "models/grass.glb";
    type.distance = distance;
    type.fade = fade;
    f.types.push_back(type);
    for (uint32_t i = 0; i < n; ++i) {
        FoliageInstance in;
        in.position = {float(i % 100) * 0.1f, 0.0f, float(i / 100) * 0.1f};
        in.random = foliageRandom(i);
        f.instances.push_back(in);
    }
    sortFoliage(f);
    return f;
}

void density() {
    // The numbers are fixed and spread evenly.
    check(foliageRandom(7) == foliageRandom(7), "the same number every time");
    double sum = 0;
    for (uint32_t i = 0; i < 10000; ++i) {
        const float r = foliageRandom(i);
        check(r >= 0.0f && r < 1.0f, "in 0..1");
        sum += r;
    }
    check(std::abs(sum / 10000 - 0.5) < 0.02, "averages a half");

    const FoliageData f = meadow(10000, 100.0f, 20.0f);
    check(f.cells.size() == 1, "one cell");
    std::vector<uint32_t> half, again, all, none;
    foliageCellKeeps(f, 0, 10.0f, 0.5f, 1.0f, half);
    foliageCellKeeps(f, 0, 10.0f, 0.5f, 1.0f, again);
    foliageCellKeeps(f, 0, 10.0f, 1.0f, 1.0f, all);
    foliageCellKeeps(f, 0, 10.0f, 0.0f, 1.0f, none);
    check(half == again, "density 0.5 keeps the same instances every run");
    check(
        std::abs(double(half.size()) / 10000.0 - 0.5) < 0.03,
        "density 0.5 keeps about half, got " + std::to_string(half.size()));
    check(all.size() == 10000, "density 1 keeps them all");
    check(none.empty(), "density 0 keeps none");
    // Beyond the distance, none; foliage_distance scales it.
    std::vector<uint32_t> far, scaled;
    foliageCellKeeps(f, 0, 100.0f, 1.0f, 1.0f, far);
    foliageCellKeeps(f, 0, 100.0f, 1.0f, 1.5f, scaled);
    check(far.empty(), "nothing at the distance");
    check(scaled.size() == 10000, "foliage_distance 1.5 draws it at 100 m");
}

void fade() {
    check(foliageFade(50, 100, 20) == 0.0f, "nearer than the fade: nothing dropped");
    check(std::abs(foliageFade(90, 100, 20) - 0.5f) < 1e-6f, "half way through the fade: half");
    check(foliageFade(100, 100, 20) == 1.0f, "at the distance: all dropped");
    check(foliageFade(99, 100, 0) == 0.0f, "no fade: none dropped before the distance");
    const FoliageData f = meadow(5000, 100.0f, 20.0f);
    std::vector<uint32_t> nearer, middle, farther;
    foliageCellKeeps(f, 0, 82.0f, 1.0f, 1.0f, nearer);
    foliageCellKeeps(f, 0, 90.0f, 1.0f, 1.0f, middle);
    foliageCellKeeps(f, 0, 98.0f, 1.0f, 1.0f, farther);
    check(
        nearer.size() > middle.size() && middle.size() > farther.size() && !farther.empty(), "thins out with distance");
    const std::set<uint32_t> nearSet(nearer.begin(), nearer.end()), middleSet(middle.begin(), middle.end());
    bool subset = true;
    for (uint32_t i : middle) subset = subset && nearSet.count(i);
    for (uint32_t i : farther) subset = subset && middleSet.count(i);
    check(subset, "fade keeps a subset of what a nearer distance keeps");
    check(std::abs(double(middle.size()) / 5000.0 - 0.5) < 0.03, "half kept half way through the fade");
    // With density too: a subset of both.
    std::vector<uint32_t> both;
    foliageCellKeeps(f, 0, 90.0f, 0.5f, 1.0f, both);
    for (uint32_t i : both) subset = subset && middleSet.count(i) && f.instances[i].random < 0.5f;
    check(subset && both.empty(), "density below the fade fraction keeps none (the kept numbers are above it)");
}

}   // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "all" || which == "format") format();
    if (which == "all" || which == "instances") instances();
    if (which == "all" || which == "cells") cells();
    if (which == "all" || which == "density") density();
    if (which == "all" || which == "fade") fade();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
