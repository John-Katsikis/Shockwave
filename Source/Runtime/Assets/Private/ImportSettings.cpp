#include <Assets/ImportSettings.h>

#include <fstream>
#include <set>
#include <sstream>

namespace shockwave {

namespace fs = std::filesystem;

const char* textureCompressionName(TextureCompression mode) {
    switch (mode) {
    case TextureCompression::Off: return "off";
    case TextureCompression::High: return "high";
    case TextureCompression::Small: return "small";
    }
    return "high";
}

bool parseTextureCompression(std::string_view name, TextureCompression& mode) {
    for (auto candidate : {TextureCompression::Off, TextureCompression::High, TextureCompression::Small}) {
        if (name == textureCompressionName(candidate)) {
            mode = candidate;
            return true;
        }
    }
    return false;
}

const char* textureCategoryName(TextureCategory category) {
    return category == TextureCategory::Ui ? "ui" : "material";
}

bool parseTextureCategory(std::string_view name, TextureCategory& category) {
    for (auto candidate : {TextureCategory::Material, TextureCategory::Ui}) {
        if (name == textureCategoryName(candidate)) {
            category = candidate;
            return true;
        }
    }
    return false;
}

fs::path importSettingsPath(const fs::path& asset) { return fs::path(asset.string() + ".swimport"); }
bool isImportSettingsFile(const fs::path& path) { return path.extension() == ".swimport"; }

bool loadImportSettings(const fs::path& file, ImportSettings& settings, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    std::ifstream in(file);
    if (!in) {
        error = "can't open " + file.string();
        return false;
    }
    ImportSettings result = settings;
    std::set<std::string> seen;
    bool versionSeen = false;
    std::string line;
    for (int number = 1; std::getline(in, line); ++number) {
        auto fail = [&](const std::string& message) {
            error = file.filename().string() + ":" + std::to_string(number) + ": " + message;
            return false;
        };
        std::istringstream row(line);
        std::string key;
        if (!(row >> key) || key[0] == '#') continue;
        if (!seen.insert(key).second) return fail("duplicate key '" + key + "'");
        if (key == "shockwave_import") {
            int version = 0;
            if (!(row >> version) || version != 1) return fail("unsupported version; expected shockwave_import 1");
            versionSeen = true;
        } else if (!versionSeen) {
            return fail("expected shockwave_import 1 first");
        } else if (key == "compression") {
            std::string value;
            row >> value;
            if (!parseTextureCompression(value, result.compression))
                return fail("compression must be high, small or off");
        } else if (key == "lods") {
            int value = -1;
            if (!(row >> value) || value < 0 || value > 7) return fail("lods must be a whole number from 0 to 7");
            result.lods = static_cast<uint32_t>(value);
        } else if (key == "category") {
            std::string value;
            row >> value;
            if (!parseTextureCategory(value, result.category)) return fail("category must be material or ui");
        } else {
            return fail("unknown key '" + key + "' (compression, lods, category)");
        }
        row >> std::ws;
        if (row.peek() != EOF && row.peek() != '#') return fail("unexpected text after the value");
    }
    if (!versionSeen) {
        error = file.filename().string() + ": expected shockwave_import 1";
        return false;
    }
    settings = result;
    return true;
}

bool saveImportSettings(
    const fs::path& asset, const ImportSettings& settings, const ImportSettings& defaults, std::string& error) {
    error.clear();
    const fs::path file = importSettingsPath(asset);
    std::error_code ec;
    if (settings == defaults) {
        fs::remove(file, ec);
        if (ec) error = "can't remove " + file.string() + ": " + ec.message();
        return !ec;
    }
    std::ostringstream out;
    out << "# Import settings for " << asset.filename().string() << " (docs/Optimisation.md)\n"
        << "shockwave_import 1\n";
    if (settings.compression != defaults.compression)
        out << "compression " << textureCompressionName(settings.compression) << "\n";
    if (settings.lods != defaults.lods) out << "lods " << settings.lods << "\n";
    if (settings.category != defaults.category) out << "category " << textureCategoryName(settings.category) << "\n";
    // Through a hidden temporary file, so the asset folder never reads half of one.
    const fs::path temp = file.parent_path() / ("." + file.filename().string() + ".tmp");
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        stream << out.str();
        if (!stream) {
            error = "can't write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        error = "can't write " + file.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

}   // namespace shockwave
