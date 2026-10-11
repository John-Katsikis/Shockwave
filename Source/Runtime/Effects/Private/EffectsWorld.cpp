#include <Effects/EffectsWorld.h>

#include <Engine/Components.h>
#include <Engine/Hierarchy.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace shockwave {

namespace {

glm::mat4 placement(const glm::vec3& position, const glm::quat& rotation, float scale) {
    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *
           glm::scale(glm::mat4(1.0f), glm::vec3(scale));
}

}   // namespace

EffectsWorld::EffectsWorld(World& world, Effects* effects) : m_world(world), m_effects(effects) {}

EffectsWorld::~EffectsWorld() { stopAll(); }

EffectsWorld::Handle
EffectsWorld::play(std::string_view effect, const glm::vec3& position, const glm::quat& rotation, float scale) {
    if (!m_effects) return -1;
    const Handle h = m_effects->play(effect, placement(position, rotation, scale));
    if (h >= 0) m_started.push_back(h);
    return h;
}

EffectsWorld::Handle EffectsWorld::playAt(std::string_view effect, Entity entity) {
    if (!m_effects || !m_world.alive(entity) || !m_world.has<Transform>(entity)) return -1;
    const Handle h = m_effects->play(effect, worldMatrix(m_world, entity));
    if (h >= 0) m_started.push_back(h);
    return h;
}

bool EffectsWorld::playing(Handle handle) const { return m_effects && m_effects->exists(handle); }

bool EffectsWorld::stop(Handle handle, bool immediately) {
    if (!playing(handle)) return false;
    m_effects->stop(handle, immediately);
    return true;
}

bool EffectsWorld::move(Handle handle, const glm::vec3& position, const glm::quat& rotation, float scale) {
    if (!playing(handle)) return false;
    m_effects->setTransform(handle, placement(position, rotation, scale));
    return true;
}

bool EffectsWorld::setSpeed(Handle handle, float speed) {
    if (!playing(handle)) return false;
    m_effects->setSpeed(handle, speed);
    return true;
}

bool EffectsWorld::setColor(Handle handle, const glm::vec4& color) {
    if (!playing(handle)) return false;
    m_effects->setColor(handle, color);
    return true;
}

bool EffectsWorld::setInput(Handle handle, int index, float value) {
    if (!playing(handle) || index < 0 || index > 3) return false;
    m_effects->setInput(handle, index, value);
    return true;
}

bool EffectsWorld::trigger(Handle handle, int index) {
    if (!playing(handle) || index < 0 || index > 3) return false;
    m_effects->sendTrigger(handle, index);
    return true;
}

bool EffectsWorld::play(Entity entity) {
    ParticleEffect* effect = m_world.alive(entity) ? m_world.get<ParticleEffect>(entity) : nullptr;
    if (!effect) return false;
    effect->playing = true;
    if (m_effects) m_effects->restart(entity);
    return true;
}

bool EffectsWorld::stop(Entity entity, bool immediately) {
    ParticleEffect* effect = m_world.alive(entity) ? m_world.get<ParticleEffect>(entity) : nullptr;
    if (!effect) return false;
    effect->playing = false;
    if (m_effects && immediately) m_effects->stop(m_effects->handleOf(entity), true);
    return true;
}

bool EffectsWorld::trigger(Entity entity, int index) {
    return m_effects && m_world.alive(entity) && trigger(m_effects->handleOf(entity), index);
}

bool EffectsWorld::setInput(Entity entity, int index, float value) {
    return m_effects && m_world.alive(entity) && setInput(m_effects->handleOf(entity), index, value);
}

void EffectsWorld::stopAll() {
    if (m_effects)
        for (Handle h : m_started) m_effects->stop(h, true);
    m_started.clear();
}

bool EffectsWorld::preload(std::string_view effect) { return m_effects && m_effects->preload(effect); }

uint32_t EffectsWorld::particleCount() const { return m_effects ? m_effects->stats().particles : 0; }

}   // namespace shockwave
