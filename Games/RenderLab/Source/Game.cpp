#include <Shockwave/Game.h>

#include <array>
#include <cstdio>

using namespace shockwave;
namespace {
class RenderLab final : public GameMode {
    Entity m_camera;
    FlyCamera m_fly, m_initial;
    bool m_orbit = false;
    double m_elapsed = 0, m_statsTimer = 0;
    void reset(GameContext& game) {
        m_fly = m_initial;
        m_orbit = false;
        m_elapsed = 0;
        game.input().setMouseCaptured(false);
    }
    void quality(GameContext& game, int index) {
        std::string error;
        if (!game.graphics().apply(
                graphicsPreset(static_cast<GraphicsPreset>(index), game.graphics().requested()), error))
            game.ui().set("lab_quality", error);
    }

public:
    void beginPlay(GameContext& game) override {
        m_camera = primaryCamera(game.world);
        if (auto* transform = game.world.get<Transform>(m_camera)) {
            m_fly.position = transform->position;
            const auto forward = transform->rotation * glm::vec3(0, 0, -1);
            m_fly.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
            m_fly.yaw = std::atan2(-forward.x, -forward.z);
            m_fly.moveSpeed = 12;
            m_initial = m_fly;
        }
        game.ui().set(
            "lab_scene", "RENDER LAB / " + std::to_string(game.world.count<ModelRenderer>()) + " imported models");
        game.ui().set("lab_stats", "Warming up...");
        game.ui().set("lab_quality", "Graphics settings: F1 low / F2 medium / F3 high / F4 ultra");
        for (int i = 0; i < 4; ++i)
            game.ui().on("quality_" + std::to_string(i), [this, &game, i](const UIEvent&) { quality(game, i); });
        game.ui().on("reset", [this, &game](const UIEvent&) { reset(game); });
        game.ui().on("orbit", [this](const UIEvent&) { m_orbit = !m_orbit; });
        game.ui().on("full_detail", [&game](const UIEvent&) {
            auto settings = game.graphics().requested();
            settings.lodErrorPixels = settings.lodErrorPixels == 0 ? 1 : 0;
            std::string error;
            game.graphics().apply(settings, error);
        });
    }
    void frame(GameContext& game, double dt) override {
        for (int i = 0; i < 4; ++i)
            if (game.input().wasActionPressed("lab_quality_" + std::to_string(i))) quality(game, i);
        if (game.input().wasActionPressed("lab_reset")) reset(game);
        if (game.input().wasActionPressed("lab_orbit")) m_orbit = !m_orbit;
        if (m_orbit) {
            m_elapsed += dt;
            const float angle = static_cast<float>(m_elapsed * 0.12);
            const float radius = glm::length(glm::vec2(m_initial.position.x, m_initial.position.z));
            m_fly.position = {std::sin(angle) * radius, m_initial.position.y, std::cos(angle) * radius};
            m_fly.lookAt({0, 1, 0});
        } else if (!game.ui().capturesInput())
            m_fly.update(game.input(), dt);
        if (auto* transform = game.world.get<Transform>(m_camera)) {
            transform->position = m_fly.position;
            transform->rotation = glm::quat(glm::vec3(m_fly.pitch, m_fly.yaw, 0));
            if (auto* previous = game.world.get<PreviousTransform>(m_camera)) previous->value = *transform;
        }
        m_statsTimer += dt;
        if (game.engine && m_statsTimer >= 0.25) {
            m_statsTimer = 0;
            auto& r = game.engine->renderer();
            const auto lod = r.lodStats();
            if (r.drawCount() == 0) return;   // shader/asset warmup has not produced a frame yet
            char text[256], fps[48];
            if (game.engine->fps() >= 1)
                std::snprintf(fps, sizeof(fps), "%.0f FPS", game.engine->fps());
            else
                std::snprintf(fps, sizeof(fps), "Warming up");
            std::snprintf(
                text, sizeof(text), "%s | GPU %.2f ms | %u draws | %.2fM / %.2fM triangles", fps,
                static_cast<double>(r.gpuFrameMs()), r.drawCount(), static_cast<double>(lod.triangles) / 1e6,
                static_cast<double>(lod.fullTriangles) / 1e6);
            game.ui().set("lab_stats", text);
            const auto settings = game.graphics().current();
            std::snprintf(
                text, sizeof(text), "Scale %.0f%% | Shadows %s | LOD error %.2g px | %s", settings.renderScale * 100,
                shadowQualityName(static_cast<int>(settings.shadows)), settings.lodErrorPixels,
                m_orbit ? "ORBIT (O to stop)" : "STATIC / FREE CAMERA");
            game.ui().set("lab_quality", text);
        }
    }
    void endPlay(GameContext& game) override { game.input().setMouseCaptured(false); }
};
}   // namespace
SW_GAME("renderlab", RenderLab)
