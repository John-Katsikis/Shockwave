#include "Editor.h"

#include <UI/Gui.h>

namespace editor {
void Editor::stopAudioPreview() {
    if (m_audioPreview && m_previewVoice) m_audioPreview->stop(m_previewVoice);
    m_previewVoice = 0;
    m_previewClip.clear();
}
void Editor::previewAudio(const std::string& clip, shockwave::AudioPlayback options) {
    stopAudioPreview();
    options.spatial = false;   // audition without needing a game listener
    if (m_audioPreview) m_previewVoice = m_audioPreview->play(engine().assets().audioClip(clip), options);
    if (m_previewVoice) m_previewClip = clip;
    if (!m_previewVoice) warn("Cannot preview " + clip + (m_audioPreview ? ": " + m_audioPreview->error() : ""));
}
shockwave::Entity Editor::spawnAudio(const std::string& clip, const glm::vec3& position) {
    if (!engine().assets().audioClip(clip)) return shockwave::kNullEntity;
    pushUndo(m_world);
    const auto entity = m_world.create();
    m_world.add<shockwave::Name>(entity, {uniqueName(std::filesystem::path(clip).stem().string())});
    m_world.add<shockwave::Transform>(entity, {position});
    m_world.add<shockwave::AudioSource>(entity, {.clip = clip});
    select(entity);
    log("Placed audio source " + clip);
    return entity;
}
}   // namespace editor
