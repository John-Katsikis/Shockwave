// The graphics release tier: see ReleaseTier.h and docs/ReleaseTier.md.
#include <Engine/ReleaseTier.h>

#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace shockwave {
namespace {

constexpr GraphicsPreset kPresets[] = {
    GraphicsPreset::Low, GraphicsPreset::Medium, GraphicsPreset::High, GraphicsPreset::Ultra};

// Splits a line into words, keeping "quoted text" (with \" and \\) as one. Stops at an
// unquoted #. False on an unclosed quote.
bool splitLine(const std::string& line, std::vector<std::string>& words) {
    words.clear();
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '#') break;
        std::string word;
        if (c == '"') {
            ++i;
            bool closed = false;
            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    word += line[i + 1];
                    i += 2;
                } else if (line[i] == '"') {
                    closed = true;
                    ++i;
                    break;
                } else {
                    word += line[i++];
                }
            }
            if (!closed) return false;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') word += line[i++];
        }
        words.push_back(std::move(word));
    }
    return true;
}

std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << std::quoted(text);
    return out.str();
}

std::string valueList(const GraphicsSettingInfo& info) {
    std::string out;
    for (size_t i = 0; i < info.choices.size(); ++i)
        out += (i ? (i + 1 == info.choices.size() ? " or " : ", ") : "") + std::string(info.choices[i]);
    return out;
}

bool boolSetting(const GraphicsSettingInfo& info) {
    return info.choices.size() == 2 && std::strcmp(info.choices[0], "true") == 0;
}

Diagnostic problem(std::string code, const std::string& shown, int line, std::string object, std::string message) {
    Diagnostic d;
    d.code = std::move(code);
    d.file = shown;
    d.line = line;
    d.object = std::move(object);
    d.message = std::move(message);
    return d;
}

std::optional<GraphicsPreset> presetNamed(const std::string& name) {
    for (const GraphicsPreset preset : kPresets)
        if (name == graphicsPresetName(preset)) return preset;
    return std::nullopt;
}

}   // namespace

const char* tierUseName(TierUse use) {
    switch (use) {
    case TierUse::Optional: return "optional";
    case TierUse::Mandatory: return "mandatory";
    }
    return "?";
}

const TierEffect* ReleaseTier::find(const std::string& key) const {
    for (const TierEffect& effect : effects)
        if (effect.key == key) return &effect;
    return nullptr;
}

std::filesystem::path releaseTierPath(const Project& project) { return project.root / "Config" / "ReleaseTier.swtier"; }

int tierRank(const GraphicsSettingInfo& info, const std::string& value) {
    for (size_t i = 0; i < info.choices.size(); ++i)
        if (value == info.choices[i]) return boolSetting(info) ? int(1 - i) : int(i);
    return -1;
}

bool tierSetting(const GraphicsSettingInfo& info) {
    if (info.choices.empty() || std::strcmp(info.category, "Quality") != 0) return false;
    // The same picture either way: how it's drawn is not an art decision.
    for (const char* same : {"light_culling", "instancing", "occlusion_culling"})
        if (std::strcmp(info.key, same) == 0) return false;
    return true;
}

bool parseReleaseTier(const std::string& text, const std::string& shown, ReleaseTier& out, Diagnostic& error) {
    ReleaseTier tier;
    auto fail = [&](int line, std::string object, std::string message) {
        error = problem("ReleaseTierSyntax", shown, line, std::move(object), std::move(message));
        return false;
    };
    std::istringstream in(text);
    std::string line;
    std::vector<std::string> words;
    int number = 0;
    bool header = false;
    while (std::getline(in, line)) {
        ++number;
        if (!splitLine(line, words)) return fail(number, "", "a quote isn't closed");
        if (words.empty()) continue;
        if (!header) {
            if (words.size() != 2 || words[0] != "swtier")
                return fail(number, "", "the first line must be \"swtier 1\"");
            if (words[1] != "1")
                return fail(number, "swtier", "version " + words[1] + " isn't supported (this build reads version 1)");
            header = true;
            continue;
        }
        const std::string& word = words[0];
        if (word == "fallback") {
            if (tier.fallbackLine) return fail(number, word, "fallback is given twice");
            const auto preset = words.size() == 2 ? presetNamed(words[1]) : std::nullopt;
            if (!preset) return fail(number, word, "fallback takes one preset: low, medium, high or ultra");
            tier.fallback = *preset;
            tier.fallbackLine = number;
            continue;
        }
        TierEffect effect;
        effect.line = number;
        if (word == "optional")
            effect.use = TierUse::Optional;
        else if (word == "mandatory")
            effect.use = TierUse::Mandatory;
        else
            return fail(number, word, "unknown word \"" + word + "\": expected fallback, optional or mandatory");
        if (words.size() < 2) return fail(number, word, word + " needs a graphics setting");
        effect.key = words[1];
        const GraphicsSettingInfo* info = findGraphicsSetting(effect.key);
        if (!info)
            return fail(number, effect.key, "\"" + effect.key + "\" isn't a graphics setting (docs/GraphicsSchema.md)");
        if (!tierSetting(*info))
            return fail(
                number, effect.key,
                effect.key +
                    " isn't an effect a tier decides: it has no named quality values, or it never changes the picture");
        if (tier.find(effect.key)) return fail(number, effect.key, effect.key + " is listed twice");
        if (effect.use == TierUse::Optional) {
            if (words.size() != 2) return fail(number, effect.key, "optional takes only the setting");
        } else {
            if (words.size() != 4 || words[3].empty())
                return fail(
                    number, effect.key,
                    "mandatory takes the setting, the lowest value that will do and a reason in quotes");
            effect.minimum = words[2];
            effect.reason = words[3];
            const int rank = tierRank(*info, effect.minimum);
            if (rank < 0)
                return fail(
                    number, effect.key,
                    "\"" + effect.minimum + "\" isn't a value of " + effect.key + " (" + valueList(*info) + ")");
            if (rank == 0)
                return fail(
                    number, effect.key,
                    effect.minimum + " is the lowest " + effect.key + " there is: that's optional, not mandatory");
        }
        tier.effects.push_back(std::move(effect));
    }
    if (!header) return fail(0, "", "the file is empty: the first line must be \"swtier 1\"");
    if (!tier.fallbackLine) return fail(0, "fallback", "fallback is missing: name the lowest preset the game supports");
    out = std::move(tier);
    return true;
}

bool readReleaseTier(const std::filesystem::path& file, const std::string& shown, ReleaseTier& out, Diagnostic& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = problem("ReleaseTierUnreadable", shown, 0, "", "can't open the file");
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parseReleaseTier(text.str(), shown, out, error);
}

std::string releaseTierText(const ReleaseTier& tier) {
    std::string out = "swtier 1\n# The graphics release tier (docs/ReleaseTier.md): what this game ships with.\n";
    out += std::string("fallback ") + graphicsPresetName(tier.fallback) + "\n";
    for (const TierEffect& effect : tier.effects) {
        out += std::string(tierUseName(effect.use)) + " " + effect.key;
        if (effect.use == TierUse::Mandatory) out += " " + effect.minimum + " " + quoted(effect.reason);
        out += "\n";
    }
    return out;
}

void validateReleaseTier(
    const ReleaseTier& tier, const GraphicsSettings& projectDefaults, const std::string& shown,
    std::vector<Diagnostic>& out) {
    const GraphicsSettings fallback = fallbackTierSettings(tier, projectDefaults);
    const std::string fallbackName = graphicsPresetName(tier.fallback);
    for (const TierEffect& effect : tier.effects) {
        const GraphicsSettingInfo* info = findGraphicsSetting(effect.key);
        if (!info) continue;   // parse refuses these
        const std::string atDefault = info->get(projectDefaults);
        if (effect.use == TierUse::Mandatory) {
            const int need = tierRank(*info, effect.minimum);
            const std::string atFallback = info->get(fallback);
            if (tierRank(*info, atFallback) < need)
                out.push_back(problem(
                    "ReleaseTierFallbackTooLow", shown, effect.line, effect.key,
                    effect.key + " must be at least " + effect.minimum + " (" + effect.reason +
                        "), but the fallback tier " + fallbackName + " draws with " + atFallback +
                        ": raise the fallback or lower the minimum"));
            if (tierRank(*info, atDefault) < need)
                out.push_back(problem(
                    "ReleaseTierDefaultTooLow", shown, effect.line, effect.key,
                    effect.key + " must be at least " + effect.minimum + " (" + effect.reason +
                        "), but the project's default is " + atDefault + " (Config/Graphics.swgraphics)"));
        }
    }
}

GraphicsSettings fallbackTierSettings(const ReleaseTier& tier, const GraphicsSettings& projectDefaults) {
    return graphicsPreset(tier.fallback, projectDefaults);
}

ReleaseTierReport releaseTierReport(const Project& project) {
    ReleaseTierReport report;
    const std::filesystem::path path = releaseTierPath(project);
    report.file = project.relative(path);
    std::error_code ec;
    report.present = std::filesystem::exists(path, ec);
    if (!report.present) return report;
    Diagnostic error;
    if (!readReleaseTier(path, report.file, report.tier, error)) {
        report.diagnostics.push_back(std::move(error));
        return report;
    }
    report.readable = true;
    std::string why;
    if (!projectGraphicsSettings(project, report.defaults, why)) {
        report.diagnostics.push_back(problem(
            "ReleaseTierNoDefaults", report.file, 0, "", "the project's graphics settings don't read (" + why + ")"));
        return report;
    }
    validateReleaseTier(report.tier, report.defaults, report.file, report.diagnostics);
    int mandatory = 0, optional = 0;
    for (const GraphicsSettingInfo& info : graphicsSettingInfo()) {
        if (!tierSetting(info)) continue;
        const TierEffect* effect = report.tier.find(info.key);
        const TierUse use = effect ? effect->use : TierUse::Optional;
        (use == TierUse::Mandatory ? mandatory : optional)++;
    }
    report.summary = std::string("fallback ") + graphicsPresetName(report.tier.fallback) + "; " +
                     std::to_string(mandatory) + " mandatory, " + std::to_string(optional) + " optional";
    return report;
}

std::string releaseTierReportText(const ReleaseTierReport& report) {
    std::string out;
    if (!report.present)
        return "no " + report.file + ": this project hasn't chosen a graphics release tier (docs/ReleaseTier.md)\n";
    if (report.readable) {
        out += report.file + ": " + report.summary + "\n\n";
        char row[256];
        std::snprintf(
            row, sizeof row, "%-22s %-10s %-8s %-9s %-9s %-9s %-9s %-9s\n", "setting", "use", "minimum", "fallback",
            "low", "medium", "high", "ultra");
        out += row;
        const GraphicsSettings& defaults = report.defaults;
        const GraphicsSettings fallback = fallbackTierSettings(report.tier, defaults);
        for (const GraphicsSettingInfo& info : graphicsSettingInfo()) {
            if (!tierSetting(info)) continue;
            const TierEffect* effect = report.tier.find(info.key);
            std::string values[4];
            for (int i = 0; i < 4; ++i) values[i] = info.get(graphicsPreset(kPresets[i], defaults));
            std::snprintf(
                row, sizeof row, "%-22s %-10s %-8s %-9s %-9s %-9s %-9s %-9s\n", info.key,
                tierUseName(effect ? effect->use : TierUse::Optional),
                effect && effect->use == TierUse::Mandatory ? effect->minimum.c_str() : "-", info.get(fallback).c_str(),
                values[0].c_str(), values[1].c_str(), values[2].c_str(), values[3].c_str());
            out += row;
        }
        bool reasons = false;
        for (const TierEffect& effect : report.tier.effects) {
            if (effect.use == TierUse::Optional) continue;
            if (!reasons) out += "\n";
            reasons = true;
            out += std::string(tierUseName(effect.use)) + " " + effect.key + ": " + effect.reason + "\n";
        }
    }
    if (!report.diagnostics.empty()) out += "\n";
    for (const Diagnostic& d : report.diagnostics)
        out += std::string(diagnosticSeverityName(d.severity)) + " " + d.text() + "\n";
    return out;
}

std::string releaseTierReportJson(const ReleaseTierReport& report) {
    std::string out = "{\n  \"file\": " + diagnosticJsonString(report.file) +
                      ",\n  \"present\": " + (report.present ? "true" : "false") +
                      ",\n  \"readable\": " + (report.readable ? "true" : "false");
    if (report.readable) {
        out += std::string(",\n  \"fallback\": \"") + graphicsPresetName(report.tier.fallback) +
               "\",\n  \"summary\": " + diagnosticJsonString(report.summary) + ",\n  \"settings\": [";
        const GraphicsSettings& defaults = report.defaults;
        const GraphicsSettings fallback = fallbackTierSettings(report.tier, defaults);
        bool first = true;
        for (const GraphicsSettingInfo& info : graphicsSettingInfo()) {
            if (!tierSetting(info)) continue;
            const TierEffect* effect = report.tier.find(info.key);
            out += first ? "\n    " : ",\n    ";
            first = false;
            out +=
                std::string("{\"key\": \"") + info.key + "\", \"use\": \"" +
                tierUseName(effect ? effect->use : TierUse::Optional) + "\", \"minimum\": " +
                (effect && effect->use == TierUse::Mandatory ? diagnosticJsonString(effect->minimum) : "null") +
                ", \"reason\": " + (effect && !effect->reason.empty() ? diagnosticJsonString(effect->reason) : "null") +
                ", \"fallback\": " + diagnosticJsonString(info.get(fallback));
            for (const GraphicsPreset preset : kPresets)
                out += std::string(", \"") + graphicsPresetName(preset) +
                       "\": " + diagnosticJsonString(info.get(graphicsPreset(preset, defaults)));
            out += "}";
        }
        out += "\n  ]";
    }
    out += ",\n  \"diagnostics\": [";
    for (size_t i = 0; i < report.diagnostics.size(); ++i)
        out += (i ? ",\n    " : "\n    ") + diagnosticJson(report.diagnostics[i]);
    out += report.diagnostics.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

void releaseTierInputs(const Project& project, ReleaseInputs& inputs) {
    const ReleaseTierReport report = releaseTierReport(project);
    inputs.tierChecked = true;
    inputs.tierFile = report.file;
    inputs.tierPresent = report.present;
    inputs.tierSummary = report.summary;
    inputs.tierProblems = report.diagnostics;
    inputs.tierFallback = report.readable ? int(report.tier.fallback) : -1;
}

}   // namespace shockwave
