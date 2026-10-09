#include "Scene.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace editor {

// Composed and decomposed the same way the gizmo does, so values never drift.
glm::mat4 Entity::matrix() const {
    glm::mat4 m(1.0f);
    ImGuizmo::RecomposeMatrixFromComponents(&position.x, &rotation.x, &scale.x, &m[0][0]);
    return m;
}

void Entity::setMatrix(const glm::mat4& m) {
    ImGuizmo::DecomposeMatrixToComponents(&m[0][0], &position.x, &rotation.x, &scale.x);
    // Decomposition leaves float noise (1e-7, -0) that would otherwise show up in the
    // inspector and in saved files.
    for (glm::vec3* v : {&position, &rotation, &scale}) {
        for (int i = 0; i < 3; ++i) {
            if (std::abs((*v)[i]) < 1e-5f) (*v)[i] = 0.0f;
        }
    }
}

namespace {

constexpr int kFormatVersion = 1;

void writeVec(std::ostream& out, const char* key, const float* v, int n) {
    out << "  " << key;
    for (int i = 0; i < n; ++i) out << ' ' << v[i];
    out << '\n';
}

bool readVec(std::istringstream& in, float* v, int n) {
    for (int i = 0; i < n; ++i) {
        if (!(in >> v[i])) return false;
    }
    return true;
}

} // namespace

bool saveScene(const Scene& scene, const std::string& path, std::string& error) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    // Write to a temporary file and rename, so a crash mid-save never eats the scene.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp);
        if (!out) {
            error = "can't write " + temp;
            return false;
        }
        out << std::setprecision(6);
        out << "# Shockwave scene\n";
        out << "shockwave_scene " << kFormatVersion << "\n";
        out << "sky_color " << scene.skyColor.r << ' ' << scene.skyColor.g << ' ' << scene.skyColor.b << "\n";
        out << "camera " << scene.camera.position.x << ' ' << scene.camera.position.y << ' '
            << scene.camera.position.z << ' ' << scene.camera.yaw << ' ' << scene.camera.pitch << "\n";
        for (const Entity& e : scene.entities) {
            out << "\nentity " << std::quoted(e.name) << "\n";
            out << "  mesh " << e.mesh << "\n";
            out << "  texture " << e.texture << "\n";
            writeVec(out, "position", &e.position.x, 3);
            writeVec(out, "rotation", &e.rotation.x, 3);
            writeVec(out, "scale", &e.scale.x, 3);
            writeVec(out, "tint", &e.tint.x, 4);
            out << "  visible " << (e.visible ? 1 : 0) << "\n";
            out << "end\n";
        }
        if (!out) {
            error = "writing " + temp + " failed";
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        error = "can't replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool loadScene(Scene& scene, const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "can't open " + path;
        return false;
    }

    Scene result;
    Entity* current = nullptr;
    std::string line;
    int lineNumber = 0;
    auto fail = [&](const std::string& what) {
        error = path + ":" + std::to_string(lineNumber) + ": " + what;
        return false;
    };

    while (std::getline(in, line)) {
        ++lineNumber;
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key) || key[0] == '#') continue;

        if (key == "shockwave_scene") {
            int version = 0;
            if (!(ls >> version) || version > kFormatVersion) return fail("unsupported scene version");
        } else if (key == "sky_color") {
            if (!readVec(ls, &result.skyColor.r, 3)) return fail("bad sky_color");
        } else if (key == "camera") {
            if (!readVec(ls, &result.camera.position.x, 3) || !(ls >> result.camera.yaw >> result.camera.pitch)) {
                return fail("bad camera");
            }
        } else if (key == "entity") {
            result.entities.emplace_back();
            current = &result.entities.back();
            if (!(ls >> std::quoted(current->name))) return fail("entity needs a name");
        } else if (key == "end") {
            current = nullptr;
        } else if (!current) {
            return fail("'" + key + "' outside an entity");
        } else if (key == "mesh") {
            if (!(ls >> current->mesh)) return fail("bad mesh");
        } else if (key == "texture") {
            if (!(ls >> current->texture)) return fail("bad texture");
        } else if (key == "position") {
            if (!readVec(ls, &current->position.x, 3)) return fail("bad position");
        } else if (key == "rotation") {
            if (!readVec(ls, &current->rotation.x, 3)) return fail("bad rotation");
        } else if (key == "scale") {
            if (!readVec(ls, &current->scale.x, 3)) return fail("bad scale");
        } else if (key == "tint") {
            if (!readVec(ls, &current->tint.x, 4)) return fail("bad tint");
        } else if (key == "visible") {
            int v = 1;
            if (!(ls >> v)) return fail("bad visible");
            current->visible = v != 0;
        }
        // Unknown keys are skipped, so newer files still open in older editors.
    }

    scene = std::move(result);
    return true;
}

Scene makeDefaultScene() {
    Scene scene;
    auto add = [&](const char* name, const char* mesh, const char* texture, glm::vec3 position, glm::vec3 scale,
                   glm::vec4 tint, glm::vec3 rotation = glm::vec3(0.0f)) {
        Entity e;
        e.name = name;
        e.mesh = mesh;
        e.texture = texture;
        e.position = position;
        e.rotation = rotation;
        e.scale = scale;
        e.tint = tint;
        scene.entities.push_back(e);
    };
    add("Floor", "floor", "grid", {0, 0, 0}, {40, 1, 40}, {1, 1, 1, 1});
    add("Crate", "cube", "panel", {0, 0.5f, 0}, {1, 1, 1}, {1.0f, 0.62f, 0.25f, 1}, {0, 20, 0});
    add("Ball", "sphere", "checker", {2.2f, 0.75f, 0.4f}, {1.5f, 1.5f, 1.5f}, {0.35f, 0.65f, 1.0f, 1});
    add("Pillar", "cube", "panel", {-2.5f, 1.5f, -1.5f}, {1, 3, 1}, {0.85f, 0.85f, 0.9f, 1});
    add("Ramp", "cube", "checker", {-0.5f, 0.4f, 2.6f}, {3, 0.2f, 1.6f}, {0.5f, 0.9f, 0.55f, 1}, {12, -15, 0});
    add("Small crate", "cube", "panel", {0.9f, 0.3f, -1.4f}, {0.6f, 0.6f, 0.6f}, {0.95f, 0.35f, 0.4f, 1},
        {0, -35, 0});
    return scene;
}

} // namespace editor
