#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace editor {

// Editor-side scene data. This moves onto the engine's entity system when that lands;
// until then the editor owns it and draws it through the renderer.
struct Entity {
    std::string name = "Entity";
    std::string mesh = "cube";      // built-in mesh name (see Editor::m_meshes)
    std::string texture = "white";  // built-in texture name
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};       // Euler angles in degrees
    glm::vec3 scale{1.0f};
    glm::vec4 tint{1.0f};
    bool visible = true;

    glm::mat4 matrix() const;
    void setMatrix(const glm::mat4& m);
};

struct SceneCamera {
    glm::vec3 position{4.0f, 2.8f, 5.5f};
    float yaw = 0.63f;     // radians, same convention as shockwave::FlyCamera
    float pitch = -0.33f;
};

struct Scene {
    std::vector<Entity> entities;
    glm::vec4 skyColor{0.32f, 0.42f, 0.58f, 1.0f};
    SceneCamera camera;
};

// Scenes are plain text (.swscene), one property per line, so they diff and merge
// cleanly in version control. Returns false and sets error on failure.
bool saveScene(const Scene& scene, const std::string& path, std::string& error);
bool loadScene(Scene& scene, const std::string& path, std::string& error);

Scene makeDefaultScene();

} // namespace editor
