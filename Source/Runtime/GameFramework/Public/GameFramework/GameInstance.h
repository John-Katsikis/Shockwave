#pragma once

// GameInstance: the game's object for the whole run of the editor or player, as in
// Unreal. It outlives Play sessions, so it holds state that carries between them, and
// it picks the GameMode for each scene.

#include <GameFramework/GameMode.h>

#include <memory>
#include <string>
#include <vector>

namespace shockwave {

struct SaveInfo;

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
    // Steps an older save's data up one schema, fromSchema to fromSchema + 1, when the
    // project's save_schema has gone up (docs/SaveGames.md#compatibility). It runs on a job
    // worker with the save detached from any world: change only `save` (its values,
    // campaign, title), touch nothing else. False, with error, refuses the save. The
    // default leaves the data as it was.
    virtual bool migrateSave(int fromSchema, SaveInfo& save, std::string& error) const {
        (void)fromSchema;
        (void)save;
        (void)error;
        return true;
    }
    // What's wrong with a scene's game content, for player --check and the editor's level
    // Validate (docs/Levels.md#checks): one line per problem, without the file name, like
    // "objective \"core\" needs a target". The scene is loaded but not playing; touch
    // nothing. They're reported as gameplay problems, apart from missing files, circuits
    // and navigation. The default finds nothing.
    virtual std::vector<std::string> checkScene(const World& scene) const {
        (void)scene;
        return {};
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

}   // namespace shockwave
