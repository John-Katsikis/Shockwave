// The graphics release tier (S10, docs/ReleaseTier.md): Config/ReleaseTier.swtier reading
// strictly and round-tripping, how values rank, what the check finds against the presets
// and a project's defaults, the release gate, and the presets all drawing at native
// resolution. No window, GPU or game module.
#include <Engine/GraphicsSettings.h>
#include <Engine/ReleaseTier.h>
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool parses(const std::string& text, ReleaseTier& tier, Diagnostic& error) {
    return parseReleaseTier(text, "Config/ReleaseTier.swtier", tier, error);
}
// The message a broken file is refused with, "" when it reads.
std::string refusal(const std::string& text, int* line = nullptr) {
    ReleaseTier tier;
    Diagnostic error;
    if (parses(text, tier, error)) return "";
    if (line) *line = error.line;
    CHECK(error.code == "ReleaseTierSyntax" && error.file == "Config/ReleaseTier.swtier");
    return error.message;
}

void testPresetsAreNative() {
    // No preset lowers the resolution (the graphics direction: native, no upscaler). The
    // player's own render_scale survives a preset only because presets set it to 1.
    for (const GraphicsPreset preset :
         {GraphicsPreset::Low, GraphicsPreset::Medium, GraphicsPreset::High, GraphicsPreset::Ultra}) {
        CHECK(graphicsPreset(preset).renderScale == 1.0f);
        const auto match = matchingGraphicsPreset(graphicsPreset(preset));
        CHECK(match && *match == preset);
    }
    // Low and medium still differ from each other and from high in what they draw.
    CHECK(!(graphicsPreset(GraphicsPreset::Low) == graphicsPreset(GraphicsPreset::Medium)));
    CHECK(!(graphicsPreset(GraphicsPreset::Medium) == graphicsPreset(GraphicsPreset::High)));
}

void testRanks() {
    const GraphicsSettingInfo* shadows = findGraphicsSetting("shadow_quality");
    const GraphicsSettingInfo* baked = findGraphicsSetting("baked_lighting");
    CHECK(shadows && baked);
    CHECK(tierRank(*shadows, "off") == 0 && tierRank(*shadows, "low") == 1 && tierRank(*shadows, "ultra") == 4);
    CHECK(tierRank(*shadows, "best") == -1);
    CHECK(tierRank(*baked, "false") == 0 && tierRank(*baked, "true") == 1);   // true is listed first
    CHECK(tierSetting(*shadows) && tierSetting(*baked));
    for (const char* key : {"render_scale", "vsync", "frame_limit", "instancing", "occlusion_culling", "light_culling"})
        CHECK(!tierSetting(*findGraphicsSetting(key)));
}

void testParse() {
    ReleaseTier tier;
    Diagnostic error;
    CHECK(parses(
        "swtier 1\n# a comment\n\nfallback medium   # trailing\n"
        "mandatory shadow_quality low \"shadows are how \\\"rooms\\\" read\"\noptional bloom_quality\n",
        tier, error));
    CHECK(tier.fallback == GraphicsPreset::Medium && tier.fallbackLine == 4 && tier.effects.size() == 2);
    CHECK(tier.effects[0].key == "shadow_quality" && tier.effects[0].use == TierUse::Mandatory);
    CHECK(tier.effects[0].minimum == "low" && tier.effects[0].reason == "shadows are how \"rooms\" read");
    CHECK(tier.effects[0].line == 5 && tier.effects[1].use == TierUse::Optional);
    CHECK(tier.find("bloom_quality") && !tier.find("fog_quality"));

    // What it writes reads back the same.
    ReleaseTier again;
    CHECK(parses(releaseTierText(tier), again, error));
    CHECK(again.fallback == tier.fallback && again.effects.size() == 2);
    CHECK(again.effects[0].reason == tier.effects[0].reason && again.effects[0].minimum == "low");

    // Strict: every mistake is refused, with its line.
    int line = 0;
    CHECK(contains(refusal(""), "empty"));
    CHECK(contains(refusal("fallback low\n", &line), "swtier 1") && line == 1);
    CHECK(contains(refusal("swtier 2\nfallback low\n"), "version 2"));
    CHECK(contains(refusal("swtier 1\n"), "fallback is missing"));
    CHECK(contains(refusal("swtier 1\nfallback tiny\n", &line), "low, medium, high or ultra") && line == 2);
    CHECK(contains(refusal("swtier 1\nfallback low\nfallback high\n", &line), "twice") && line == 3);
    CHECK(contains(refusal("swtier 1\nfallback low\nrequired bloom_quality\n"), "unknown word"));
    CHECK(contains(refusal("swtier 1\nfallback low\noptional sparkle\n"), "isn't a graphics setting"));
    CHECK(contains(refusal("swtier 1\nfallback low\noptional render_scale\n"), "isn't an effect a tier decides"));
    CHECK(contains(refusal("swtier 1\nfallback low\noptional instancing\n"), "isn't an effect a tier decides"));
    CHECK(contains(refusal("swtier 1\nfallback low\noptional bloom_quality\noptional bloom_quality\n"), "twice"));
    CHECK(contains(refusal("swtier 1\nfallback low\noptional bloom_quality high\n"), "only the setting"));
    CHECK(contains(refusal("swtier 1\nfallback low\nmandatory bloom_quality low\n"), "reason in quotes"));
    CHECK(contains(refusal("swtier 1\nfallback low\nmandatory bloom_quality low \"\"\n"), "reason in quotes"));
    CHECK(contains(refusal("swtier 1\nfallback low\nmandatory bloom_quality huge \"x\"\n"), "isn't a value of"));
    CHECK(contains(refusal("swtier 1\nfallback low\nmandatory bloom_quality off \"x\"\n"), "that's optional"));
    CHECK(contains(refusal("swtier 1\nfallback low\nmandatory bloom_quality low \"x\n"), "quote isn't closed"));
}

std::vector<Diagnostic> problems(const std::string& text, const GraphicsSettings& defaults = {}) {
    ReleaseTier tier;
    Diagnostic error;
    CHECK(parses(text, tier, error));
    std::vector<Diagnostic> out;
    validateReleaseTier(tier, defaults, "Config/ReleaseTier.swtier", out);
    return out;
}

void testValidate() {
    // The low preset has low shadows and no ambient occlusion.
    CHECK(problems("swtier 1\nfallback low\nmandatory shadow_quality low \"x\"\n").empty());
    auto found = problems("swtier 1\nfallback low\nmandatory ambient_occlusion low \"contact reads depth\"\n");
    // Both the fallback preset and the defaults (ambient occlusion is off by default) miss it.
    CHECK(found.size() == 2 && found[0].code == "ReleaseTierFallbackTooLow" && found[0].line == 3);
    CHECK(found[0].object == "ambient_occlusion" && contains(found[0].message, "contact reads depth"));
    CHECK(contains(found[0].message, "fallback tier low draws with off"));
    CHECK(found[1].code == "ReleaseTierDefaultTooLow");
    // Raising the fallback fixes the first; the project's own default fixes the second.
    GraphicsSettings defaults;
    defaults.ambientOcclusion = AmbientOcclusion::Medium;
    CHECK(problems("swtier 1\nfallback medium\nmandatory ambient_occlusion low \"x\"\n", defaults).empty());
    found = problems("swtier 1\nfallback low\nmandatory ambient_occlusion low \"x\"\n", defaults);
    CHECK(found.size() == 1 && found[0].code == "ReleaseTierFallbackTooLow");
    // A setting presets leave alone is judged by the project's value in the fallback too.
    defaults = {};
    defaults.bakedLighting = false;
    found = problems("swtier 1\nfallback low\nmandatory baked_lighting true \"the levels are baked\"\n", defaults);
    CHECK(found.size() == 2);
    CHECK(problems("swtier 1\nfallback low\nmandatory baked_lighting true \"the levels are baked\"\n").empty());

    // The fallback tier is the preset over the project's choices.
    ReleaseTier tier;
    tier.fallback = GraphicsPreset::Low;
    defaults = {};
    defaults.bloomScale = 0.5f;
    defaults.renderScale = 0.75f;
    const GraphicsSettings fallback = fallbackTierSettings(tier, defaults);
    CHECK(fallback.shadows == ShadowQuality::Low && fallback.bloomScale == 0.5f && fallback.renderScale == 1.0f);
}

const ReleaseGate* findGate(const ReleaseReport& report, const std::string& id) {
    for (const ReleaseGate& gate : report.gates)
        if (gate.id == id) return &gate;
    return nullptr;
}

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

void testReportAndGate() {
    const fs::path root = fs::temp_directory_path() / "shockwave-release-tier-test";
    fs::remove_all(root);
    fs::create_directories(root / "Config");
    Project project;
    project.root = root;
    project.name = "Tier Test";

    // No file: nothing decided, nothing wrong.
    ReleaseTierReport report = releaseTierReport(project);
    CHECK(!report.present && report.diagnostics.empty() && report.file == "Config/ReleaseTier.swtier");
    CHECK(contains(releaseTierReportText(report), "hasn't chosen a graphics release tier"));
    ReleaseInputs inputs;
    releaseTierInputs(project, inputs);
    CHECK(inputs.tierChecked && !inputs.tierPresent && inputs.tierProblems.empty() && inputs.tierFallback == -1);

    // A good file.
    write(root / "Config/ReleaseTier.swtier", "swtier 1\nfallback low\nmandatory shadow_quality low \"the sun\"\n");
    report = releaseTierReport(project);
    CHECK(report.present && report.readable && report.diagnostics.empty());
    CHECK(contains(report.summary, "fallback low; 1 mandatory, ") && contains(report.summary, " optional"));
    const std::string text = releaseTierReportText(report);
    CHECK(
        contains(text, "shadow_quality         mandatory  low      low") &&
        contains(text, "mandatory shadow_quality: the sun"));
    const std::string json = releaseTierReportJson(report);
    CHECK(
        contains(json, "\"fallback\": \"low\"") &&
        contains(json, "\"key\": \"shadow_quality\", \"use\": \"mandatory\""));
    CHECK(contains(json, "\"diagnostics\": []"));
    releaseTierInputs(project, inputs);
    CHECK(inputs.tierPresent && inputs.tierFallback == 0 && inputs.tierSummary == report.summary);

    // The project's own settings are what the defaults are judged by.
    write(root / "Config/Graphics.swgraphics", "swgraphics 1\nshadow_quality off\n");
    report = releaseTierReport(project);
    CHECK(report.diagnostics.size() == 1 && report.diagnostics[0].code == "ReleaseTierDefaultTooLow");
    fs::remove(root / "Config/Graphics.swgraphics");

    // A file that doesn't read.
    write(root / "Config/ReleaseTier.swtier", "swtier 1\nfallback low\nmandatory sparkle low \"x\"\n");
    report = releaseTierReport(project);
    CHECK(report.present && !report.readable && report.diagnostics.size() == 1 && report.diagnostics[0].line == 3);
    CHECK(contains(releaseTierReportJson(report), "\"readable\": false"));

    // The gate, through a contract.
    ReleaseReport release;
    release.present = release.readable = true;
    release.file = "Config/Release.swrelease";
    release.settings.graphicsTier = ReleaseGraphicsTier::Medium;
    ReleaseInputs gateInputs;
    releaseGates(release, gateInputs);
    CHECK(findGate(release, "release_tier") && findGate(release, "release_tier")->status == GateStatus::Unverified);
    gateInputs.tierChecked = true;
    releaseGates(release, gateInputs);
    CHECK(findGate(release, "release_tier")->status == GateStatus::Unverified);
    CHECK(contains(findGate(release, "release_tier")->reason, "no Config/ReleaseTier.swtier"));
    gateInputs.tierPresent = true;
    gateInputs.tierSummary = "fallback low; 1 mandatory, 19 optional";
    gateInputs.tierFallback = 0;
    releaseGates(release, gateInputs);
    CHECK(findGate(release, "release_tier")->status == GateStatus::Passed);
    CHECK(
        findGate(release, "release_tier")->reason == gateInputs.tierSummary &&
        findGate(release, "release_tier")->batch == "S10");
    // The contract can't qualify its minimum hardware below the fallback.
    gateInputs.tierFallback = 2;
    releaseGates(release, gateInputs);
    CHECK(findGate(release, "release_tier")->status == GateStatus::Failed);
    CHECK(contains(findGate(release, "release_tier")->reason, "graphics_tier medium"));
    gateInputs.tierFallback = 0;
    Diagnostic bad;
    bad.code = "ReleaseTierFallbackTooLow";
    bad.file = "Config/ReleaseTier.swtier";
    bad.line = 3;
    bad.message = "too low";
    gateInputs.tierProblems.push_back(bad);
    releaseGates(release, gateInputs);
    CHECK(findGate(release, "release_tier")->status == GateStatus::Failed);
    CHECK(contains(findGate(release, "release_tier")->reason, "Config/ReleaseTier.swtier:3: too low"));
    fs::remove_all(root);
}

}   // namespace

int main() {
    testPresetsAreNative();
    testRanks();
    testParse();
    testValidate();
    testReportAndGate();
    if (failures) {
        std::fprintf(stderr, "release_tier: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::puts("release_tier: ok");
    return 0;
}
