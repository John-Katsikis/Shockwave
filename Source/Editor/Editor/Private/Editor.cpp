// The editor's core: projects, scenes, selection, editing operations, undo and play
// mode. The panels that call into this live in the *Panel.cpp files and Workspace.cpp.
#include "Editor.h"

#include <Assets/BuiltinAssets.h>
#include <UI/Gui.h>
#include <Physics/Physics.h>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;
using shockwave::MeshRenderer;
using shockwave::Name;
using shockwave::Parent;
using shockwave::Transform;

namespace {

const SDL_DialogFileFilter kSceneFilter[] = {{"Shockwave scenes", "swscene"}};
const SDL_DialogFileFilter kProjectFilter[] = {{"Shockwave projects", "swproject"}};
const SDL_DialogFileFilter kImportFilter[] = {{"Images and models", "png;jpg;jpeg;tga;bmp;psd;gif;hdr;pic;pnm;glb;gltf;fbx;obj;dae;stl;ply;blend"},
                                              {"All files", "*"}};

} // namespace

Editor::Editor(EditorOptions options) : m_options(std::move(options)), m_prefs(m_options.prefs) {}

// ---- Startup and shutdown -----------------------------------------------------------------

void Editor::onStart() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsMoveFromTitleBarOnly = true;   // dragging in the viewport is for the scene
    // The dock layout is per user, next to the preferences, not in whatever folder the
    // editor was started from.
    std::error_code ec;
    fs::create_directories(m_options.prefsPath.parent_path(), ec);
    m_iniPath = (m_options.prefsPath.parent_path() / "editor-layout.ini").string();
    io.IniFilename = m_iniPath.c_str();
    // ImGuizmo's colors are sRGB like ImGui's; convert them for the sRGB swapchain too.
    for (ImVec4& c : ImGuizmo::GetStyle().Colors) c = gui::color(c.x, c.y, c.z, c.w);

    // Game types first, so scenes and the inspector know them; then the starter assets
    // the editor and the player share.
    const shockwave::GameModule* module = m_options.gameModule;
    if (module && module->registerTypes) module->registerTypes();
    shockwave::createBuiltinAssets(engine().renderer(), engine().assets());
    if (module && module->createAssets) module->createAssets(engine());
    m_assetFolder = std::make_unique<shockwave::AssetFolder>(engine().renderer(), engine().assets(), engine().jobs());
    const auto factory = module ? module->create : shockwave::GameplayFactory([] { return std::make_unique<shockwave::Gameplay>(); });
    m_session = std::make_unique<shockwave::GameSession>(m_world, engine().jobs(), factory, &engine());
    shockwave::FlyCamera::bindDefaultControls(engine().input());
    applyViewPreferences();

    if (const char* home = std::getenv("HOME")) m_newProjectLocation = (fs::path(home) / "Shockwave Projects").string();

    if (!m_options.path.empty()) {
        const fs::path path = m_options.path;
        if (const auto projectFile = shockwave::findProjectFile(path)) {
            openProject(*projectFile, path.extension() == ".swscene" ? path : fs::path());
        } else {
            error("No Shockwave project at " + path.string() + ". Create one in the project hub.");
        }
    } else if (m_prefs.reopenLastProject && !m_prefs.recentProjects.empty() &&
               fs::exists(m_prefs.recentProjects.front(), ec)) {
        openProject(m_prefs.recentProjects.front());
    }

    if (m_hasProject && m_options.selectOnStart >= 0) {
        select(m_world.entityAt(static_cast<size_t>(m_options.selectOnStart)));
    }
    if (m_hasProject && m_options.playOnStart) play();
}

void Editor::onShutdown() {
    stop();
    if (m_hasProject) {
        rememberCamera();
        saveEditorState();
    }
    savePreferences();
}

bool Editor::onQuitRequested() {
    if (!m_dirty) return true;
    requestAction(PendingAction::Quit);
    return false;
}

void Editor::applyViewPreferences() {
    m_camera.moveSpeed = m_prefs.cameraSpeed;
    m_camera.fovY = glm::radians(std::clamp(m_prefs.fieldOfView, 20.0f, 140.0f));
    engine().renderer().setVsync(m_prefs.vsync);
    engine().renderer().setRenderScale(m_prefs.renderScale);
}

void Editor::savePreferences() {
    m_prefsDirty = false;
    std::string err;
    if (!editor::savePreferences(m_options.prefsPath, m_prefs, err)) warn("Couldn't save preferences: " + err);
}

// ---- Per frame ----------------------------------------------------------------------------

void Editor::onGui() {
    ImGuizmo::BeginFrame();
    const float dt = ImGui::GetIO().DeltaTime;
    m_frameTimes[m_frameTimeIndex] = dt * 1000.0f;
    m_frameTimeIndex = (m_frameTimeIndex + 1) % m_frameTimes.size();

    handleDialogResults();
    if (!m_hasProject) {
        engine().renderer().setViewport(0, 0);
        drawMenuBar();
        drawProjectHub();
        drawUnsavedChangesModal();
        if (m_showPreferences) drawPreferencesWindow();
        if (m_showAbout) drawAboutWindow();
        updateTitle();
        return;
    }

    if (m_retryAssetImports) {
        m_retryAssetImports = false;
        logAssetChanges(m_assetFolder->refresh(true));
    } else {
        logAssetChanges(m_assetFolder->poll(dt));
    }
    m_sceneScanTimer += dt;
    if (m_sceneScanTimer > 2.0) scanScenes();
    autosave(dt);
    handleShortcuts();

    drawMenuBar();
    drawCommandBar();
    drawStatusBar();
    drawDockspace();
    drawViewport();
    m_hierarchyOrderNext.clear();
    if (m_showHierarchy) drawHierarchy();
    m_hierarchyOrder.swap(m_hierarchyOrderNext);
    if (m_showInspector) drawInspector();
    if (m_showContent) drawContentBrowser();
    if (m_showConsole) drawConsole();
    if (m_showStats) drawStatsWindow();
    if (m_showPreferences) drawPreferencesWindow();
    if (m_showProjectSettings) drawProjectSettingsWindow();
    if (m_showControls) drawControlsWindow();
    if (m_showAbout) drawAboutWindow();
    if (m_showDemo) ImGui::ShowDemoWindow(&m_showDemo);
    if (m_showMetrics) ImGui::ShowMetricsWindow(&m_showMetrics);
    gui::focusOutline();
    drawUnsavedChangesModal();
    drawRecoveryModal();
    updateTitle();

    // Preference changes are written once the user lets go of the widget.
    if (m_prefsDirty && !ImGui::IsAnyItemActive()) savePreferences();
}

// Play mode runs the project's gameplay and physics at the fixed tick rate, like the player.
void Editor::onUpdate(double dt) {
    if (playState() != PlayState::Playing) return;
    m_session->update(dt);
}

void Editor::onFrame(double dt) {
    if (m_session) m_session->frame(dt);
}

void Editor::onRender(shockwave::Renderer& renderer, double alpha) {
    if (!m_hasProject) {
        renderer.setClearColor({0.0f, 0.0f, 0.0f, 1.0f});
        renderer.setGrid({});
        return;
    }
    renderer.setClearColor({0.0f, 0.0f, 0.0f, 1.0f});   // a scene without an Environment
    const bool sceneCamera = lookingThroughSceneCamera();
    glm::mat4 view = m_camera.view(), proj = projection(renderer.aspectRatio());
    if (sceneCamera) shockwave::cameraMatrices(m_world, shockwave::primaryCamera(m_world), renderer.aspectRatio(), view, proj);
    renderer.setCamera(view, proj);

    shockwave::GridSettings grid;
    grid.enabled = m_prefs.showGrid && !sceneCamera;
    grid.cellSize = 1.0f;
    grid.fadeDistance = std::clamp(m_camera.position.y * 12.0f + 60.0f, 60.0f, 600.0f);
    renderer.setGrid(grid);
    // While editing (or paused) nothing moves on its own, so draw exactly what's there.
    shockwave::renderWorld(m_world, renderer, engine().jobs(), playState() == PlayState::Playing ? alpha : 1.0,
                           &engine().assets());
}

bool Editor::lookingThroughSceneCamera() const {
    return m_viewThroughCamera && shockwave::primaryCamera(m_world).valid();
}

// ---- Projects -----------------------------------------------------------------------------

bool Editor::openProject(const fs::path& path, const fs::path& sceneToOpen) {
    shockwave::Project project;
    std::string err;
    if (!shockwave::loadProject(path, project, err)) {
        error("Couldn't open project: " + err);
        m_hubError = err;
        return false;
    }
    stop();
    if (m_hasProject) {
        rememberCamera();
        saveEditorState();
    }
    m_project = project;
    m_projectEdit = project;
    m_hasProject = true;
    m_hubError.clear();
    m_scenePath.clear();
    m_contentRoot = ContentRoot::Assets;
    m_contentFolder.clear();
    m_prefs.addRecentProject(m_project.file.string());
    savePreferences();
    log("Opened project " + m_project.name + " (" + m_project.root.string() + ")");
    // Gameplay is compiled in: a project written for another module still opens for
    // editing, but Play would run the wrong game, so it's switched off.
    const std::string_view built = m_options.gameModule ? m_options.gameModule->id : std::string_view();
    m_gameModuleMatches = m_project.gameModule == built;
    if (!m_gameModuleMatches) {
        warn("This project's gameplay module is \"" + m_project.gameModule + "\" but the editor was built with \"" +
             std::string(built) + "\". Play is off; rebuild with -DSHOCKWAVE_GAME_SOURCE=<its Game.cpp> to run it.");
    }
    engine().setTickRate(m_project.tickRate);
    applyProjectTheme();
    logAssetChanges(m_assetFolder->open(m_options.assetFolder.empty() ? m_project.assetFolder : fs::path(m_options.assetFolder)));
    loadEditorState();
    scanScenes();

    // The scene asked for, else the one open last time, else the startup scene, else any.
    std::error_code ec;
    fs::path scene = sceneToOpen;
    if (scene.empty() && !m_lastScene.empty() && fs::exists(m_project.root / m_lastScene, ec)) {
        scene = m_project.root / m_lastScene;
    }
    if (scene.empty() && fs::exists(m_project.startupScene, ec)) scene = m_project.startupScene;
    if (scene.empty() && !m_scenes.empty()) scene = m_scenes.front();
    if (scene.empty() || !openScene(scene)) {
        // Give the project a first scene to work in, saved where it expects its startup scene.
        newScene(SceneTemplate::Basic);
        if (!fs::exists(m_project.startupScene, ec)) saveSceneAs(m_project.startupScene);
    }
    return true;
}

// The theme: --theme if given, else the one picked in Preferences, else the project's.
void Editor::applyProjectTheme() {
    std::error_code ec;
    std::string theme = m_options.theme;
    if (theme.empty()) theme = m_prefs.theme;
    if (theme.empty() && m_hasProject && fs::is_regular_file(m_project.theme, ec)) theme = m_project.theme.string();
    if (!theme.empty() && theme != engine().guiThemePath()) engine().setGuiTheme(theme);
}

void Editor::closeProject() {
    if (!m_hasProject) return;
    stop();
    rememberCamera();
    saveEditorState();
    log("Closed project " + m_project.name);
    m_hasProject = false;
    m_world.clear();
    m_scenePath.clear();
    m_selection.clear();
    m_undo.clear();
    m_redo.clear();
    m_dirty = false;
    m_scenes.clear();
}

bool Editor::createNewProject(const fs::path& folder, const std::string& name) {
    shockwave::Project project;
    std::string err;
    const std::string module = m_options.gameModule ? std::string(m_options.gameModule->id) : "game";
    if (!shockwave::createProject(folder, name, module, project, err)) {
        m_hubError = err;
        error("Couldn't create the project: " + err);
        return false;
    }
    m_hubError.clear();
    log("Created project " + project.name + " in " + folder.string());
    // The manifest needs its startup scene to exist before it will load.
    shockwave::World scene;
    makeScene(SceneTemplate::Basic, scene, engine().assets());
    if (!shockwave::saveScene(scene, engine().assets(), project.startupScene.string(), err)) {
        m_hubError = err;
        error("Couldn't write the first scene: " + err);
        return false;
    }
    return openProject(project.file);
}

// Saved/editor.swstate: which scene was open and where the camera was in each scene.
// Per user, so it stays out of version control.
void Editor::loadEditorState() {
    m_sceneCameras.clear();
    m_lastScene.clear();
    std::ifstream in(m_project.statePath() / "editor.swstate");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "last_scene") {
            std::getline(ls >> std::ws, m_lastScene);
        } else if (key == "camera") {
            EditorCamera camera;
            std::string scene;
            if (ls >> camera.position.x >> camera.position.y >> camera.position.z >> camera.yaw >> camera.pitch &&
                std::getline(ls >> std::ws, scene)) {
                m_sceneCameras[scene] = camera;
            }
        }
    }
}

void Editor::saveEditorState() {
    if (!m_hasProject) return;
    std::error_code ec;
    fs::create_directories(m_project.statePath(), ec);
    std::ofstream out(m_project.statePath() / "editor.swstate", std::ios::trunc);
    out << "# Shockwave Editor state for this project, per user. Safe to delete.\n";
    if (!m_lastScene.empty()) out << "last_scene " << m_lastScene << "\n";
    for (const auto& [scene, c] : m_sceneCameras) {
        out << "camera " << c.position.x << ' ' << c.position.y << ' ' << c.position.z << ' ' << c.yaw << ' '
            << c.pitch << ' ' << scene << "\n";
    }
}

void Editor::rememberCamera() {
    if (m_scenePath.empty()) return;
    m_sceneCameras[m_project.relative(m_scenePath)] = {m_camera.position, m_camera.yaw, m_camera.pitch};
}

fs::path Editor::autosavePath() const {
    std::string name = m_scenePath.empty() ? "untitled" : m_project.relative(m_scenePath);
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    if (!name.ends_with(".swscene")) name += ".swscene";
    return m_project.statePath() / "autosave" / name;
}

void Editor::scanScenes() {
    m_sceneScanTimer = 0.0;
    m_scenes.clear();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_project.sceneFolder, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".swscene") m_scenes.push_back(it->path());
    }
    std::sort(m_scenes.begin(), m_scenes.end());

    m_assetFolders.clear();
    const fs::path assets = m_project.assetFolder;
    for (auto it = fs::recursive_directory_iterator(assets, ec); !ec && it != fs::end(it); it.increment(ec)) {
        if (it->path().filename().string().starts_with(".")) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) m_assetFolders.push_back(it->path().lexically_relative(assets).generic_string());
    }
    std::sort(m_assetFolders.begin(), m_assetFolders.end());
}

// ---- Selection ----------------------------------------------------------------------------

void Editor::select(Entity e) {
    const bool alive = m_world.alive(e);
    const bool changed = !(m_selection.size() == 1 && m_selection[0] == e) && !(m_selection.empty() && !alive);
    m_selection.clear();
    if (alive) m_selection.push_back(e);
    m_rangeAnchor = alive ? e : kNullEntity;
    if (changed) m_revealSelection = true;
}

void Editor::toggleSelected(Entity e) {
    if (!m_world.alive(e)) return;
    if (isSelected(e)) {
        std::erase(m_selection, e);
    } else {
        m_selection.push_back(e);
        m_revealSelection = true;
    }
    m_rangeAnchor = e;
}

void Editor::selectRange(Entity to) {
    const auto a = std::find(m_hierarchyOrder.begin(), m_hierarchyOrder.end(), m_rangeAnchor);
    const auto b = std::find(m_hierarchyOrder.begin(), m_hierarchyOrder.end(), to);
    if (a == m_hierarchyOrder.end() || b == m_hierarchyOrder.end()) {
        select(to);
        return;
    }
    const Entity anchor = m_rangeAnchor;
    m_selection.assign(std::min(a, b), std::max(a, b) + 1);
    std::erase(m_selection, to);
    m_selection.push_back(to);   // the clicked row is the primary
    m_rangeAnchor = anchor;
}

void Editor::selectAll() {
    m_selection.clear();
    m_world.forEachEntity([&](Entity e) { m_selection.push_back(e); });
}

bool Editor::isSelected(Entity e) const {
    return std::find(m_selection.begin(), m_selection.end(), e) != m_selection.end();
}

Entity Editor::selected() const {
    for (auto it = m_selection.rbegin(); it != m_selection.rend(); ++it) {
        if (m_world.alive(*it)) return *it;
    }
    return kNullEntity;
}

std::vector<Entity> Editor::selection() const {
    std::vector<Entity> live;
    for (Entity e : m_selection) {
        if (m_world.alive(e)) live.push_back(e);
    }
    return live;
}

std::vector<Entity> Editor::selectionRoots() const {
    const std::vector<Entity> all = selection();
    std::vector<Entity> roots;
    for (Entity e : all) {
        const bool inside = std::any_of(all.begin(), all.end(), [&](Entity other) {
            return !(other == e) && shockwave::isAncestor(m_world, other, e);
        });
        if (!inside) roots.push_back(e);
    }
    return roots;
}

// ---- Editing ------------------------------------------------------------------------------

std::string Editor::displayName(Entity e) const {
    const Name* name = m_world.get<Name>(e);
    return name && !name->value.empty() ? name->value : "Entity " + std::to_string(e.index);
}

std::string Editor::kindLabel(Entity e) {
    if (m_world.has<shockwave::Camera>(e)) return "Camera";
    if (m_world.has<shockwave::Environment>(e)) return "Environment";
    if (const auto* light = m_world.get<shockwave::Light>(e)) {
        static const char* kinds[] = {"Directional light", "Point light", "Spot light"};
        const int type = static_cast<int>(light->type);
        return type >= 0 && type < 3 ? kinds[type] : "Light";
    }
    if (const MeshRenderer* r = m_world.get<MeshRenderer>(e)) {
        const std::string mesh = engine().assets().meshName(r->mesh);
        return mesh.empty() ? "Mesh" : fs::path(mesh).stem().string();
    }
    if (const auto* m = m_world.get<shockwave::ModelRenderer>(e)) {
        const std::string model = engine().assets().modelName(m->model);
        return model.empty() ? "Model" : fs::path(model).stem().string();
    }
    if (m_world.has<shockwave::CharacterController>(e)) return "Character";
    if (m_world.has<shockwave::PhysicsSettings>(e)) return "Physics";
    if (m_world.has<shockwave::Collider>(e)) {
        const auto* body = m_world.get<shockwave::RigidBody>(e);
        return body && body->sensor ? "Trigger" : "Collider";
    }
    return {};
}

std::string Editor::uniqueName(const std::string& base) const {
    auto taken = [&](const std::string& name) {
        bool found = false;
        if (const auto* names = m_world.pool<Name>()) {
            for (const Name& n : names->components()) found |= n.value == name;
        }
        return found;
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        const std::string name = base + " " + std::to_string(n);
        if (!taken(name)) return name;
    }
}

glm::vec3 Editor::placementPoint() const {
    // Where the view meets the ground, if that's reasonably close; otherwise a few units
    // ahead, dropped to the ground.
    const glm::vec3 forward = m_camera.forward();
    glm::vec3 at = m_camera.position + forward * 6.0f;
    if (forward.y < -0.05f) {
        const float t = -m_camera.position.y / forward.y;
        if (t > 0.0f && t < 40.0f) at = m_camera.position + forward * t;
    }
    at.y = 0.0f;
    if (m_prefs.snap && m_prefs.snapMove > 0.0f) {
        at.x = std::round(at.x / m_prefs.snapMove) * m_prefs.snapMove;
        at.z = std::round(at.z / m_prefs.snapMove) * m_prefs.snapMove;
    }
    return at;
}

Entity Editor::createEntity(CreateKind kind, Entity parent) {
    if (parent.valid() && !m_world.alive(parent)) parent = kNullEntity;
    pushUndo(m_world);
    const Entity e = m_world.create();
    const shockwave::Assets& assets = engine().assets();
    const glm::vec3 ground = placementPoint();
    auto addMesh = [&](const char* name, const char* mesh, float lift) {
        m_world.add<Name>(e, {uniqueName(name)});
        m_world.add<Transform>(e, {parent.valid() ? glm::vec3(0.0f) : ground + glm::vec3(0.0f, lift, 0.0f)});
        MeshRenderer renderer;
        renderer.mesh = assets.findMesh(mesh);
        m_world.add<MeshRenderer>(e, renderer);
    };
    switch (kind) {
    case CreateKind::Empty:
        m_world.add<Name>(e, {uniqueName("Entity")});
        m_world.add<Transform>(e, {parent.valid() ? glm::vec3(0.0f) : ground});
        break;
    case CreateKind::Cube: addMesh("Cube", "cube", 0.5f); break;
    case CreateKind::Sphere: addMesh("Sphere", "sphere", 0.5f); break;
    case CreateKind::Plane: addMesh("Plane", "plane", 0.0f); break;
    case CreateKind::Camera: {
        // Starts out seeing what the viewport sees.
        m_world.add<Name>(e, {uniqueName("Camera")});
        Transform t = Transform::fromMatrix(glm::inverse(m_camera.view()));
        if (parent.valid()) t = Transform::fromMatrix(glm::inverse(shockwave::worldMatrix(m_world, parent)) * t.matrix());
        m_world.add<Transform>(e, t);
        shockwave::Camera camera;
        camera.primary = !shockwave::primaryCamera(m_world).valid();
        m_world.add<shockwave::Camera>(e, camera);
        break;
    }
    case CreateKind::DirectionalLight:
    case CreateKind::PointLight:
    case CreateKind::SpotLight: {
        shockwave::Light light;
        Transform t;
        if (kind == CreateKind::DirectionalLight) {
            // Only the direction matters; slanted like an afternoon sun, above the scene.
            m_world.add<Name>(e, {uniqueName("Sun")});
            light.type = shockwave::Light::Type::Directional;
            light.intensity = 2.5f;
            t.position = glm::vec3(0.0f, 10.0f, 0.0f);
            t.rotation = shockwave::fromEulerDegrees({-50.0f, 35.0f, 0.0f});
        } else if (kind == CreateKind::PointLight) {
            m_world.add<Name>(e, {uniqueName("Point light")});
            light.type = shockwave::Light::Type::Point;
            light.intensity = 4.0f;
            light.range = 8.0f;
            t.position = placementPoint() + glm::vec3(0.0f, 2.0f, 0.0f);
        } else {
            // Above where the camera looks, pointing down at it.
            m_world.add<Name>(e, {uniqueName("Spot light")});
            light.type = shockwave::Light::Type::Spot;
            light.intensity = 10.0f;
            light.range = 12.0f;
            t.position = placementPoint() + glm::vec3(0.0f, 4.0f, 0.0f);
            t.rotation = shockwave::fromEulerDegrees({-90.0f, 0.0f, 0.0f});
        }
        if (parent.valid()) t = Transform::fromMatrix(glm::inverse(shockwave::worldMatrix(m_world, parent)) * t.matrix());
        m_world.add<Transform>(e, t);
        m_world.add<shockwave::Light>(e, light);
        break;
    }
    case CreateKind::Environment:
        m_world.add<Name>(e, {uniqueName("Environment")});
        m_world.add<shockwave::Environment>(e);
        if (m_world.count<shockwave::Environment>() > 1) warn("The scene now has two Environments; only one is used");
        break;
    }
    if (parent.valid() && m_world.has<Transform>(e)) m_world.add<Parent>(e, {parent});
    select(e);
    log("Created " + displayName(e) + (parent.valid() ? " under " + displayName(parent) : ""));
    return e;
}

Entity Editor::spawnMesh(const std::string& mesh, const glm::vec3& position) {
    pushUndo(m_world);
    const Entity e = m_world.create();
    std::string base = fs::path(mesh).stem().string();
    if (!base.empty()) base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
    m_world.add<Name>(e, {uniqueName(base)});
    m_world.add<Transform>(e, {position});
    MeshRenderer renderer;
    renderer.mesh = engine().assets().findMesh(mesh);
    m_world.add<MeshRenderer>(e, renderer);
    select(e);
    log("Placed " + displayName(e));
    return e;
}

Entity Editor::spawnModel(const std::string& model, const glm::vec3& position) {
    const auto handle = engine().assets().findModel(model);
    if (!handle.valid()) {
        warn(model + " hasn't been imported");
        return kNullEntity;
    }
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<Name>(e, {uniqueName(fs::path(model).stem().string())});
    m_world.add<Transform>(e, {position});
    m_world.add<shockwave::ModelRenderer>(e, {handle});
    select(e);
    log("Placed " + displayName(e));
    return e;
}

bool Editor::reparent(Entity child, Entity parent) {
    if (!m_world.alive(child) || shockwave::parentOf(m_world, child) == parent) return false;
    if (parent.valid() && (parent == child || shockwave::isAncestor(m_world, child, parent))) {
        warn("Can't put " + displayName(child) + " under " + displayName(parent) + ": it's inside it");
        return false;
    }
    shockwave::World before = m_world;
    // Keep it where it is on screen; its Transform is recalculated relative to the new parent.
    if (!shockwave::setParent(m_world, child, parent)) return false;
    pushUndo(std::move(before));
    log(parent.valid() ? "Attached " + displayName(child) + " to " + displayName(parent)
                       : "Detached " + displayName(child));
    return true;
}

// Dragging one of several selected rows moves the whole selection.
bool Editor::reparentSelection(Entity dragged, Entity parent) {
    if (!isSelected(dragged) || selection().size() < 2) return reparent(dragged, parent);
    shockwave::World before = m_world;
    int moved = 0;
    for (Entity e : selectionRoots()) {
        if (e == parent || shockwave::parentOf(m_world, e) == parent) continue;
        if (parent.valid() && shockwave::isAncestor(m_world, e, parent)) continue;
        moved += shockwave::setParent(m_world, e, parent) ? 1 : 0;
    }
    if (moved == 0) return false;
    pushUndo(std::move(before));
    log((parent.valid() ? "Attached " + std::to_string(moved) + " entities to " + displayName(parent)
                        : "Detached " + std::to_string(moved) + " entities"));
    return true;
}

bool Editor::applyTexture(Entity e, const std::string& texture) {
    MeshRenderer* renderer = m_world.get<MeshRenderer>(e);
    const auto handle = engine().assets().findTexture(texture);
    if (!renderer || !handle) {
        if (m_world.alive(e)) warn(displayName(e) + " has no MeshRenderer to show " + texture + " on");
        return false;
    }
    pushUndo(m_world);
    renderer->texture = *handle;
    log("Gave " + displayName(e) + " the texture " + texture);
    return true;
}

void Editor::duplicateSelected() {
    const std::vector<Entity> roots = selectionRoots();
    if (roots.empty()) return;
    pushUndo(m_world);
    std::vector<Entity> copies;
    for (Entity e : roots) {
        const Entity copy = shockwave::cloneWithChildren(m_world, e);
        if (Name* name = m_world.get<Name>(copy)) name->value = uniqueName(name->value);
        copies.push_back(copy);
    }
    m_selection = copies;
    m_rangeAnchor = copies.back();
    log(copies.size() == 1 ? "Duplicated as " + displayName(copies[0])
                           : "Duplicated " + std::to_string(copies.size()) + " entities");
}

void Editor::deleteSelected() {
    const std::vector<Entity> roots = selectionRoots();
    if (roots.empty()) return;
    pushUndo(m_world);
    size_t count = 0;
    const std::string first = displayName(roots[0]);
    for (Entity e : roots) {
        count += shockwave::subtree(m_world, e).size();
        shockwave::destroyWithChildren(m_world, e);
    }
    select(kNullEntity);
    log(count == 1 ? "Deleted " + first : "Deleted " + std::to_string(count) + " entities");
}

// Puts the selection under a new empty entity at their centre, without moving anything.
void Editor::groupSelected() {
    const std::vector<Entity> roots = selectionRoots();
    if (roots.empty()) return;
    pushUndo(m_world);
    Entity commonParent = shockwave::parentOf(m_world, roots[0]);
    glm::vec3 centre(0.0f);
    for (Entity e : roots) {
        if (!(shockwave::parentOf(m_world, e) == commonParent)) commonParent = kNullEntity;
        centre += glm::vec3(shockwave::worldMatrix(m_world, e)[3]);
    }
    centre /= static_cast<float>(roots.size());
    const Entity group = m_world.create();
    m_world.add<Name>(group, {uniqueName("Group")});
    const glm::mat4 parentWorld = commonParent.valid() ? shockwave::worldMatrix(m_world, commonParent) : glm::mat4(1.0f);
    m_world.add<Transform>(group, {glm::vec3(glm::inverse(parentWorld) * glm::vec4(centre, 1.0f))});
    if (commonParent.valid()) m_world.add<Parent>(group, {commonParent});
    for (Entity e : roots) shockwave::setParent(m_world, e, group);
    select(group);
    log("Grouped " + std::to_string(roots.size()) + " entities under " + displayName(group));
}

// Moves the selection to the editor camera: the quick way to set up a game camera.
void Editor::alignSelectedToView() {
    const Entity e = selected();
    Transform* t = m_world.get<Transform>(e);
    if (!t) return;
    pushUndo(m_world);
    const Entity parent = shockwave::parentOf(m_world, e);
    const glm::mat4 parentWorld = parent.valid() ? shockwave::worldMatrix(m_world, parent) : glm::mat4(1.0f);
    const Transform local = Transform::fromMatrix(glm::inverse(parentWorld) * glm::inverse(m_camera.view()));
    t->position = local.position;
    t->rotation = local.rotation;
    log("Moved " + displayName(e) + " to the view");
}

// Frames everything selected: the world-space box round their meshes (or positions).
void Editor::focusSelected() {
    const std::vector<Entity> all = selection();
    bool any = false;
    glm::vec3 lo(0.0f), hi(0.0f);
    auto grow = [&](const glm::vec3& p) {
        lo = any ? glm::min(lo, p) : p;
        hi = any ? glm::max(hi, p) : p;
        any = true;
    };
    for (Entity e : all) {
        if (!m_world.has<Transform>(e)) continue;
        const glm::mat4 m = shockwave::worldMatrix(m_world, e);
        shockwave::Bounds box;
        if (!entityBounds(e, box)) {
            grow(glm::vec3(m[3]));
            continue;
        }
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                                   (i & 4) ? box.max.z : box.min.z);
            grow(glm::vec3(m * glm::vec4(corner, 1.0f)));
        }
    }
    if (!any) return;
    const glm::vec3 centre = (lo + hi) * 0.5f;
    const float radius = std::max(glm::length(hi - lo) * 0.5f, 0.5f);
    const float distance = std::clamp(radius / std::tan(m_camera.fovY * 0.5f) * 1.1f, 1.5f, 2000.0f);
    m_camera.position = centre - m_camera.forward() * distance;
}

void Editor::beginRename(Entity e) {
    if (!m_world.alive(e)) return;
    m_renaming = e;
    m_renameBuffer = displayName(e);
    m_focusRename = true;
    m_revealSelection = true;
}

void Editor::trackEdit() {
    // Snapshot when a widget starts editing and commit one undo step when it finishes,
    // so a whole drag is a single undo instead of one per frame.
    if (ImGui::IsItemActivated()) m_editStart = m_world;
    if (ImGui::IsItemDeactivatedAfterEdit()) pushUndo(m_editStart);
}

void Editor::pushUndo(shockwave::World before) {
    if (playing()) return;   // play-mode changes are thrown away on Stop anyway
    constexpr size_t kMaxUndo = 200;
    m_undo.push_back(std::move(before));
    if (m_undo.size() > kMaxUndo) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

void Editor::undo() {
    if (m_undo.empty() || playing()) return;
    m_redo.push_back(std::move(m_world));
    m_world = std::move(m_undo.back());
    m_undo.pop_back();
    m_dirty = true;
}

void Editor::redo() {
    if (m_redo.empty() || playing()) return;
    m_undo.push_back(std::move(m_world));
    m_world = std::move(m_redo.back());
    m_redo.pop_back();
    m_dirty = true;
}

void Editor::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) requestAction(PendingAction::NewBasicScene);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) requestAction(PendingAction::OpenSceneDialog);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) saveScene();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global)) showSaveSceneDialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global)) {
        redo();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global)) playState() == PlayState::Playing ? stop() : play();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, global)) pause();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma, global)) m_showPreferences = true;

    // The rest act on the scene, so not while typing.
    if (io.WantTextInput) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global)) duplicateSelected();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, global)) selectAll();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G, global)) groupSelected();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, global)) alignSelectedToView();

    // Single keys only when the camera isn't flying.
    if (m_looking || io.KeyCtrl || io.KeyAlt) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOperation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOperation = ImGuizmo::SCALE;
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        m_gizmoMode = m_gizmoMode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && !io.KeyShift) {
        m_prefs.showGrid = !m_prefs.showGrid;
        m_prefsDirty = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) beginRename(selected());
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) m_viewThroughCamera = !m_viewThroughCamera;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) m_showControls = !m_showControls;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        select(kNullEntity);
    }
}

// ---- Play mode ----------------------------------------------------------------------------

void Editor::play() {
    if (!m_session || playState() == PlayState::Playing) return;
    if (!m_gameModuleMatches) {
        warn("Play is off: this project's gameplay module isn't the one compiled into the editor");
        return;
    }
    const bool resuming = playState() == PlayState::Paused;
    try {
        m_session->start();
    } catch (const std::exception& e) {
        error(std::string("Couldn't start gameplay: ") + e.what());
        return;
    }
    if (resuming) {
        log("Resumed");
        return;
    }
    if (const auto* physics = m_session->physics()) {
        for (const auto& problem : physics->diagnostics()) warn(displayName(problem.entity) + ": " + problem.message);
    }
    m_viewBeforePlay = m_viewThroughCamera;
    if (m_prefs.playFromSceneCamera) {
        if (shockwave::primaryCamera(m_world).valid()) {
            m_viewThroughCamera = true;
        } else {
            warn("The scene has no Camera, so Play shows the editor's view. Add one with Create > Camera.");
        }
    }
    log("Playing the project's gameplay and physics. Stop restores the scene; F8 switches cameras.");
}

void Editor::pause() {
    if (!m_session) return;
    if (playState() == PlayState::Playing) {
        m_session->pause();
        log("Paused");
    } else if (playState() == PlayState::Paused) {
        play();
    }
}

void Editor::stop() {
    if (!m_session || !playing()) return;
    try {
        m_session->stop();
    } catch (const std::exception& e) {
        error(std::string("Gameplay cleanup failed: ") + e.what());
    }
    m_gizmoWasUsing = false;
    m_viewThroughCamera = m_viewBeforePlay;
    log("Stopped after " + std::to_string(m_session->ticks()) + " simulation ticks");
}

// ---- Scene files --------------------------------------------------------------------------

std::string Editor::sceneDisplayName() const {
    return m_scenePath.empty() ? "Untitled" : m_scenePath.filename().string();
}

void Editor::newScene(SceneTemplate kind) {
    stop();
    rememberCamera();
    makeScene(kind, m_world, engine().assets());
    m_scenePath.clear();
    const EditorCamera camera;
    m_camera.position = camera.position;
    m_camera.yaw = camera.yaw;
    m_camera.pitch = camera.pitch;
    m_undo.clear();
    m_redo.clear();
    select(kNullEntity);
    m_dirty = false;
    m_viewThroughCamera = false;
    log(kind == SceneTemplate::Basic ? "New scene from the Basic template" : "New empty scene");
}

bool Editor::openScene(const fs::path& path) {
    stop();
    shockwave::World loaded;
    std::string err;
    std::vector<std::string> warnings;
    // Scenes saved by older editors carry the camera in the file.
    EditorCamera fileCamera;
    bool hasFileCamera = false;
    shockwave::SceneFileHooks hooks;
    hooks.readHeader = [&](const std::string& key, std::istream& in) {
        if (key == "editor_camera" || key == "camera") {
            in >> fileCamera.position.x >> fileCamera.position.y >> fileCamera.position.z >> fileCamera.yaw >>
                fileCamera.pitch;
            hasFileCamera = true;
        }
    };
    if (!shockwave::loadScene(loaded, engine().assets(), path.string(), err, hooks, &warnings)) {
        error("Couldn't open scene: " + err);
        return false;
    }
    rememberCamera();
    for (const std::string& w : warnings) warn(w);
    m_world = std::move(loaded);
    m_scenePath = fs::absolute(path).lexically_normal();
    const std::string relative = m_project.relative(m_scenePath);
    EditorCamera camera = hasFileCamera ? fileCamera : EditorCamera();
    if (const auto it = m_sceneCameras.find(relative); it != m_sceneCameras.end()) camera = it->second;
    m_camera.position = camera.position;
    m_camera.yaw = camera.yaw;
    m_camera.pitch = camera.pitch;
    m_undo.clear();
    m_redo.clear();
    select(kNullEntity);
    m_dirty = false;
    m_viewThroughCamera = false;
    m_autosaveTimer = 0.0;
    m_lastScene = relative;
    m_prefs.addRecentScene(m_scenePath.string());
    m_prefsDirty = true;
    saveEditorState();
    log("Opened " + relative + " (" + std::to_string(m_world.entityCount()) + " entities)");

    // An autosave newer than the file means the editor stopped with unsaved changes.
    std::error_code ec;
    const fs::path autosaved = autosavePath();
    if (fs::exists(autosaved, ec) && fs::last_write_time(autosaved, ec) > fs::last_write_time(m_scenePath, ec)) {
        m_recoveryPath = autosaved;
        m_openRecoveryModal = true;
    }
    return true;
}

bool Editor::saveScene() {
    if (!m_hasProject) return false;
    if (playing()) {
        warn("Stop playing before saving: the scene is mid-simulation");
        return false;
    }
    if (m_scenePath.empty()) {
        showSaveSceneDialog();
        return false;
    }
    return saveSceneAs(m_scenePath);
}

bool Editor::saveSceneAs(fs::path path) {
    if (playing()) {
        warn("Stop playing before saving: the scene is mid-simulation");
        return false;
    }
    if (path.extension() != ".swscene") path += ".swscene";
    path = fs::absolute(path).lexically_normal();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path oldAutosave = autosavePath();
    std::string err;
    if (!shockwave::saveScene(m_world, engine().assets(), path.string(), err)) {
        error("Couldn't save: " + err);
        return false;
    }
    fs::remove(oldAutosave, ec);
    m_scenePath = path;
    fs::remove(autosavePath(), ec);
    m_dirty = false;
    m_autosaveTimer = 0.0;
    const std::string relative = m_project.relative(m_scenePath);
    if (fs::path(relative).is_absolute()) warn(relative + " is outside the project folder; other machines won't find it");
    m_lastScene = relative;
    rememberCamera();
    saveEditorState();
    m_prefs.addRecentScene(m_scenePath.string());
    m_prefsDirty = true;
    scanScenes();
    log("Saved " + relative);

    // A project without a working startup scene gets this one.
    if (!fs::is_regular_file(m_project.startupScene, ec)) {
        m_project.startupScene = m_scenePath;
        m_projectEdit.startupScene = m_scenePath;
        if (shockwave::saveProject(m_project, err)) log("Startup scene set to " + relative);
    }
    return true;
}

void Editor::autosave(double frameDt) {
    if (m_prefs.autosaveMinutes <= 0 || !m_dirty || playing()) return;
    m_autosaveTimer += frameDt;
    if (m_autosaveTimer < m_prefs.autosaveMinutes * 60.0) return;
    m_autosaveTimer = 0.0;
    const fs::path path = autosavePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::string err;
    if (shockwave::saveScene(m_world, engine().assets(), path.string(), err)) {
        log("Autosaved " + sceneDisplayName());
    } else {
        warn("Autosave failed: " + err);
    }
}

// SDL calls back from another thread (or later on this one); results are picked up in
// handleDialogResults at the start of a frame.
namespace {

struct PendingDialog {
    std::mutex* mutex;
    void* results;   // std::vector<Editor::DialogResult>*, type-erased to keep it private
    int kind;
    std::string* error;
};

} // namespace

#define SW_DIALOG_CALLBACK                                                                            \
    [](void* user, const char* const* files, int) {                                                   \
        auto* pending = static_cast<PendingDialog*>(user);                                            \
        {                                                                                             \
            std::lock_guard lock(*pending->mutex);                                                    \
            if (!files) {                                                                             \
                *pending->error = SDL_GetError();                                                     \
            } else if (files[0]) {                                                                    \
                std::vector<std::string> list;                                                        \
                for (const char* const* f = files; *f; ++f) list.emplace_back(*f);                    \
                static_cast<std::vector<DialogResult>*>(pending->results)                             \
                    ->push_back({static_cast<DialogKind>(pending->kind), std::move(list)});           \
            }                                                                                         \
        }                                                                                             \
        delete pending;                                                                               \
    }

void Editor::showOpenSceneDialog() {
    auto* pending = new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::OpenScene), &m_dialogError};
    const std::string folder = m_project.sceneFolder.string();
    SDL_ShowOpenFileDialog(SW_DIALOG_CALLBACK, pending, engine().window().handle(), kSceneFilter, 1, folder.c_str(), false);
}

void Editor::showSaveSceneDialog() {
    if (!m_hasProject) return;
    auto* pending = new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::SaveScene), &m_dialogError};
    const std::string location =
        m_scenePath.empty() ? (m_project.sceneFolder / "untitled.swscene").string() : m_scenePath.string();
    SDL_ShowSaveFileDialog(SW_DIALOG_CALLBACK, pending, engine().window().handle(), kSceneFilter, 1, location.c_str());
}

void Editor::showOpenProjectDialog() {
    auto* pending = new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::OpenProject), &m_dialogError};
    SDL_ShowOpenFileDialog(SW_DIALOG_CALLBACK, pending, engine().window().handle(), kProjectFilter, 1,
                           m_newProjectLocation.c_str(), false);
}

void Editor::showNewProjectFolderDialog() {
    auto* pending =
        new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::NewProjectFolder), &m_dialogError};
    SDL_ShowOpenFolderDialog(SW_DIALOG_CALLBACK, pending, engine().window().handle(), m_newProjectLocation.c_str(), false);
}

void Editor::showImportDialog(const fs::path& folder) {
    m_importFolder = folder;
    auto* pending = new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::Import), &m_dialogError};
    SDL_ShowOpenFileDialog(SW_DIALOG_CALLBACK, pending, engine().window().handle(), kImportFilter, 2, nullptr, true);
}

#undef SW_DIALOG_CALLBACK

void Editor::handleDialogResults() {
    std::vector<DialogResult> results;
    std::string err;
    {
        std::lock_guard lock(m_dialogMutex);
        results.swap(m_dialogResults);
        err.swap(m_dialogError);
    }
    if (!err.empty()) error("File dialog unavailable (" + err + "). It needs xdg-desktop-portal or zenity.");
    for (const DialogResult& r : results) {
        switch (r.kind) {
        case DialogKind::OpenScene: openScene(r.files[0]); break;   // unsaved changes were already confirmed
        case DialogKind::SaveScene: saveSceneAs(r.files[0]); break;
        case DialogKind::OpenProject: requestAction(PendingAction::OpenProject, r.files[0]); break;
        case DialogKind::NewProjectFolder: m_newProjectLocation = r.files[0]; break;
        case DialogKind::Import: {
            std::error_code ec;
            fs::create_directories(m_importFolder, ec);
            for (const std::string& file : r.files) {
                const fs::path target = m_importFolder / fs::path(file).filename();
                fs::copy_file(file, target, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    error("Couldn't import " + file + ": " + ec.message());
                } else {
                    log("Copied " + fs::path(file).filename().string() + " into " + m_project.relative(m_importFolder));
                }
            }
            logAssetChanges(m_assetFolder->refresh());
            break;
        }
        }
    }
}

void Editor::requestAction(PendingAction action, const std::string& path) {
    if (!m_dirty) {
        runAction(action, path);
        return;
    }
    m_pendingAction = action;
    m_pendingPath = path;
    m_openUnsavedModal = true;
}

void Editor::runAction(PendingAction action, const std::string& path) {
    switch (action) {
    case PendingAction::NewBasicScene: newScene(SceneTemplate::Basic); break;
    case PendingAction::NewEmptyScene: newScene(SceneTemplate::Empty); break;
    case PendingAction::OpenSceneDialog: showOpenSceneDialog(); break;
    case PendingAction::OpenScene: openScene(path); break;
    case PendingAction::OpenProject: {
        const auto file = shockwave::findProjectFile(path);
        if (file) {
            m_dirty = false;   // confirmed
            openProject(*file);
        } else {
            error("No Shockwave project at " + path);
        }
        break;
    }
    case PendingAction::CloseProject:
        m_dirty = false;
        closeProject();
        break;
    case PendingAction::Quit: engine().quit(); break;
    case PendingAction::None: break;
    }
}

void Editor::updateTitle() {
    std::string title = "Shockwave Editor";
    if (m_hasProject) {
        title = m_project.name + " - " + sceneDisplayName() + (m_dirty ? " *" : "") + (playing() ? " [Playing]" : "") +
                " - Shockwave Editor";
    }
    if (title != m_title) {
        engine().window().setTitle(title);
        m_title = title;
    }
}

} // namespace editor
