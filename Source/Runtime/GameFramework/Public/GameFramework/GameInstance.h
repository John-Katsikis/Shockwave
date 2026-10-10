#pragma once

// GameInstance: the game's object for the whole run of the editor or player, as in
// Unreal. It outlives Play sessions, so it holds state that carries between them, and
// it picks the GameMode for each scene.

#include <GameFramework/GameMode.h>

#include <memory>

namespace shockwave {

class GameInstance {
public:
    virtual ~GameInstance() = default;
    // engine is null in tools and tests that run without a window.
    virtual void init(Engine* engine) { (void)engine; }
    virtual void shutdown() {}
    // The GameMode for a Play of this scene. The default runs the engine's systems only.
    virtual std::unique_ptr<GameMode> createGameMode(const World& scene) {
        (void)scene;
        return std::make_unique<GameMode>();
    }
    // Subsystems that live as long as the instance.
    SubsystemCollection& subsystems() { return m_subsystems; }

private:
    SubsystemCollection m_subsystems;
};

// A GameInstance whose every scene plays with the same GameMode type.
template <class TGameMode>
class SimpleGameInstance : public GameInstance {
public:
    std::unique_ptr<GameMode> createGameMode(const World&) override { return std::make_unique<TGameMode>(); }
};

} // namespace shockwave
