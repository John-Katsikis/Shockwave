// Colour grading, exposure and fog tools (A05, A06, docs/PostProcess.md):
//   --postprocess-schema   the .swpostprocess reference (docs/PostProcessSchema.md)
//   --postprocess-info     a profile with its parents applied, and its LUT, as JSON
//   --lut-export-neutral   an identity .cube to grade in another tool
// and player --check for PostProcessVolume components and every profile in Content.
#include "Player.h"

#include <Engine/Components.h>
#include <Engine/PostProcess.h>
#include <Renderer/PostProcess.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace player {

namespace {

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", c);
            out += escape;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}

void writeText(const fs::path& file, const std::string& text) {
    if (file == "-") {
        std::fputs(text.c_str(), stdout);
        return;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("can't write " + file.string());
}

// The Content folder a file sits in (the nearest folder named Content above it), else its
// own folder: profiles name their parents and LUTs inside Content.
fs::path contentFolderOf(const fs::path& file) {
    const fs::path absolute = fs::absolute(file).lexically_normal();
    for (fs::path p = absolute.parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
        if (p.filename() == "Content") return p;
    return absolute.parent_path();
}

template <typename T>
const T& member(const shockwave::PostProcessValues& values, size_t offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(&values) + offset);
}

std::string valueJson(const shockwave::PostProcessParamInfo& p, const shockwave::PostProcessValues& values) {
    using shockwave::PostProcessValueType;
    switch (p.type) {
    case PostProcessValueType::Bool: return member<bool>(values, p.offset) ? "true" : "false";
    case PostProcessValueType::Enum: {
        const int i = member<int>(values, p.offset);
        return quote(i >= 0 && size_t(i) < p.options.size() ? p.options[size_t(i)] : "?");
    }
    case PostProcessValueType::Path: return quote(member<std::string>(values, p.offset));
    case PostProcessValueType::Float: return number(member<float>(values, p.offset));
    case PostProcessValueType::Vec2:
    case PostProcessValueType::Color3: {
        const float* v = &member<float>(values, p.offset);
        const int n = p.type == PostProcessValueType::Vec2 ? 2 : 3;
        std::string out = "[";
        for (int c = 0; c < n; ++c) out += (c ? ", " : "") + number(v[c]);
        return out + "]";
    }
    }
    return "null";
}

}   // namespace

int postProcessSchema(const fs::path& file) {
    writeText(file, shockwave::postProcessSchemaMarkdown());
    return 0;
}

int postProcessInfo(const fs::path& profile, const fs::path& json) {
    const fs::path content = contentFolderOf(profile);
    shockwave::PostProcessLibrary library;
    library.setContentFolder(content);
    const std::string name = fs::absolute(profile).lexically_normal().lexically_relative(content).generic_string();
    std::string error;
    const auto resolved = library.profile(name, &error);
    std::string out = "{\"profile\": " + quote(name) + ", \"content\": " + quote(content.generic_string()) +
                      ", \"valid\": " + (resolved ? "true" : "false");
    if (!resolved) out += ", \"error\": " + quote(error);
    std::string lutError;
    std::shared_ptr<const shockwave::PostProcessLibrary::LoadedLut> lut;
    if (resolved) {
        out += ", \"chain\": [";
        for (size_t i = 0; i < resolved->chain.size(); ++i) out += (i ? ", " : "") + quote(resolved->chain[i]);
        out += "], \"values\": {";
        const auto& params = shockwave::postProcessParams();
        bool first = true;
        for (size_t i = 0; i < params.size(); ++i) {
            if (!((resolved->set >> i) & 1u)) continue;
            out += (first ? "" : ", ") + quote(params[i].key) + ": " + valueJson(params[i], resolved->values);
            first = false;
        }
        out += "}";
        if (!resolved->values.lut.empty()) {
            lut = library.lut(resolved->values.lut, &lutError);
            out += ", \"lut\": {\"file\": " + quote(resolved->values.lut) + ", \"valid\": " + (lut ? "true" : "false");
            if (lut)
                out += ", \"title\": " + quote(lut->lut.title) + ", \"size\": " + std::to_string(lut->lut.size) +
                       ", \"domain_min\": [" + number(lut->lut.domainMin.x) + ", " + number(lut->lut.domainMin.y) +
                       ", " + number(lut->lut.domainMin.z) + "], \"domain_max\": [" + number(lut->lut.domainMax.x) +
                       ", " + number(lut->lut.domainMax.y) + ", " + number(lut->lut.domainMax.z) + "]";
            else
                out += ", \"error\": " + quote(lutError);
            out += "}";
        }
    }
    out += "}\n";
    if (json.empty()) {
        // Readable on the terminal; --json for the full report.
        if (!resolved) {
            std::printf("%s: %s\n", name.c_str(), error.c_str());
        } else {
            std::printf("%s", name.c_str());
            for (size_t i = 1; i < resolved->chain.size(); ++i) std::printf(" <- %s", resolved->chain[i].c_str());
            std::printf("\n");
            shockwave::PostProcessFile flat;
            flat.values = resolved->values;
            flat.set = resolved->set;
            std::string text = shockwave::writePostProcess(flat);
            text = text.substr(text.find('\n') + 1);   // without the version line
            std::fputs(text.c_str(), stdout);
            if (!resolved->values.lut.empty()) {
                if (lut)
                    std::printf("lut %s: %u^3\n", resolved->values.lut.c_str(), lut->lut.size);
                else
                    std::printf("lut %s: %s\n", resolved->values.lut.c_str(), lutError.c_str());
            }
        }
    } else {
        writeText(json, out);
    }
    return resolved && (resolved->values.lut.empty() || lut) ? 0 : 1;
}

int lutExportNeutral(uint32_t size, const fs::path& output) {
    if (size != 17 && size != 33 && size != 65) throw UsageError("--lut-export-neutral takes 17, 33 or 65");
    shockwave::Lut lut = shockwave::neutralLut(size);
    lut.title = "Shockwave neutral " + std::to_string(size);
    writeText(output, shockwave::writeCubeLut(lut));
    if (output != "-") std::printf("Wrote %s\n", output.string().c_str());
    return 0;
}

void Player::checkPostProcessVolumes(
    shockwave::World& world, const std::string& shown, std::vector<std::string>& problems,
    std::vector<std::string>& checkedProfiles) {
    world.each<const shockwave::PostProcessVolume>(
        [&](shockwave::Entity e, const shockwave::PostProcessVolume& volume) {
            const shockwave::Name* name = world.get<shockwave::Name>(e);
            const std::string where = shown + ": entity '" + (name ? name->value : "?") + "': PostProcessVolume ";
            if (!volume.unbounded && !world.has<shockwave::Transform>(e))
                problems.push_back(where + "needs a Transform to be placed (or unbounded true)");
            if (volume.profile.empty()) {
                problems.push_back(where + "has no profile");
                return;
            }
            checkedProfiles.push_back(volume.profile);
            std::string error;
            const auto profile = m_postProcess->profile(volume.profile, &error);
            if (!profile) {
                problems.push_back(where + volume.profile + ": " + error);
                return;
            }
            if (!profile->values.lut.empty() && !m_postProcess->lut(profile->values.lut, &error))
                problems.push_back(where + volume.profile + ": lut " + profile->values.lut + ": " + error);
            if (volume.enabled && volume.weight <= 0.0f)
                std::printf("warning %sweight 0 applies nothing\n", where.c_str());
        });
}

void Player::checkPostProcessFiles(
    const std::vector<std::string>& checkedProfiles, std::vector<std::string>& problems) {
    for (const std::string& file : m_postProcess->profileFiles()) {
        if (std::find(checkedProfiles.begin(), checkedProfiles.end(), file) != checkedProfiles.end()) continue;
        const std::string shown = m_project.relative(m_project.assetFolder / file);
        std::string error;
        const auto profile = m_postProcess->profile(file, &error);
        if (!profile)
            problems.push_back(shown + ": " + error);
        else if (!profile->values.lut.empty() && !m_postProcess->lut(profile->values.lut, &error))
            problems.push_back(shown + ": lut " + profile->values.lut + ": " + error);
        else
            std::printf("ok %s\n", shown.c_str());
    }
}

}   // namespace player
