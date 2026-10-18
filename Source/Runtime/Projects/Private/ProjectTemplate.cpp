#include <Projects/Project.h>

#include <array>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace shockwave {
std::string projectModuleId(std::string_view name) {
    std::string id = "project_";
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            id += c;
        else if (id.back() != '_')
            id += '_';
    }
    while (id.size() > 8 && id.back() == '_') id.pop_back();
    return id == "project_" ? "project_game" : id;
}

bool createGameProject(
    const std::filesystem::path& folder, const std::string& name, const std::string& gameModule,
    const std::filesystem::path& templateFolder, Project& project, std::string& error) {
    namespace fs = std::filesystem;
    error.clear();
    if (name.empty() || name.find_first_of("\r\n") != std::string::npos) {
        error = "the project needs a name on one line";
        return false;
    }
    if (gameModule.empty() || gameModule.front() < 'a' || gameModule.front() > 'z' ||
        gameModule.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
        error = "module id must start with a lowercase letter and contain only a-z, 0-9 and underscores";
        return false;
    }
    std::error_code ec;
    const fs::path root = fs::absolute(folder, ec).lexically_normal();
    if (ec) {
        error = "can't resolve project folder: " + ec.message();
        return false;
    }
    if (fs::exists(root, ec) && (!fs::is_directory(root, ec) || !fs::is_empty(root, ec))) {
        error = "new project folder must be empty: " + root.string();
        return false;
    }
    if (ec) {
        error = "can't inspect project folder: " + ec.message();
        return false;
    }
    const std::array<std::pair<const char*, const char*>, 7> files{
        {{"CMakeLists.txt.in", "CMakeLists.txt"},
         {"Source/Game.cpp.in", "Source/Game.cpp"},
         {"Scenes/main.swscene", "Scenes/main.swscene"},
         {"Config/Input.swinput", "Config/Input.swinput"},
         {"README.md", "README.md"},
         {"gitignore.in", ".gitignore"},
         {"Content/.gitkeep", "Content/.gitkeep"}}};
    std::vector<std::pair<fs::path, std::string>> contents;
    for (const auto& [source, destination] : files) {
        std::ifstream input(templateFolder / source);
        if (!input) {
            error = "missing project template: " + (templateFolder / source).string();
            return false;
        }
        std::ostringstream text;
        text << input.rdbuf();
        if (input.bad()) {
            error = "can't read project template: " + std::string(source);
            return false;
        }
        std::string data = text.str();
        constexpr std::string_view token = "@GAME_MODULE@";
        for (size_t at = 0; (at = data.find(token, at)) != std::string::npos; at += gameModule.size())
            data.replace(at, token.size(), gameModule);
        contents.emplace_back(root / destination, std::move(data));
    }
    // The manifest is written last: an interrupted scaffold cannot masquerade as a
    // complete project. Failure leaves files for inspection and never overwrites a retry.
    for (const auto& [path, data] : contents) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "can't create " + path.parent_path().string() + ": " + ec.message();
            return false;
        }
        std::ofstream output(path, std::ios::binary);
        output << data;
        output.close();
        if (!output) {
            error = "can't write " + path.string();
            return false;
        }
    }
    Project result;
    if (!createProject(root, name, gameModule, result, error)) return false;
    project = std::move(result);
    return true;
}
}   // namespace shockwave
