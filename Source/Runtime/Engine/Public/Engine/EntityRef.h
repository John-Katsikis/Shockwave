#pragma once

// EntityRef: an entity together with its world, for game code that works with one entity
// at a time. It's a plain handle: copying it copies the handle, and it owns nothing.
//
//     if (EntityRef crate = findEntity(world, "Crate")) {
//         crate.add(ColorPulse{.base = crate.get<MeshRenderer>().tint});
//         crate.get<Transform>().position.y += 1.0f;
//     }
//
// Systems that touch many entities should still use World::each or a Schedule.

#include <Engine/World.h>

#include <cassert>
#include <string_view>
#include <utility>

namespace shockwave {

class EntityRef {
public:
    EntityRef() = default;
    EntityRef(World& world, Entity entity) : m_world(&world), m_entity(entity) {}

    // False when nothing was found, or the entity has been destroyed since.
    explicit operator bool() const { return m_world && m_world->alive(m_entity); }
    Entity entity() const { return m_entity; }
    World& world() const { return *m_world; }

    template <class T>
    bool has() const {
        return *this && m_world->has<T>(m_entity);
    }
    // The component, which must be there (check with has<T>, or use find<T>).
    template <class T>
    T& get() const {
        T* component = find<T>();
        assert(component && "EntityRef::get: the entity doesn't have this component");
        return *component;
    }
    // The component, or null when it isn't there.
    template <class T>
    T* find() const {
        return *this ? m_world->get<T>(m_entity) : nullptr;
    }
    // Adds the component, or replaces the one that's there.
    template <class T>
    T& add(T value = {}) const {
        assert(*this);
        return m_world->add<T>(m_entity, std::move(value));
    }
    template <class T>
    void remove() const {
        if (*this) m_world->remove<T>(m_entity);
    }
    // Destroys the entity and everything parented under it.
    void destroy() const;

    bool operator==(const EntityRef& other) const { return m_world == other.m_world && m_entity == other.m_entity; }

private:
    World* m_world = nullptr;
    Entity m_entity;
};

// The first entity with this Name, or an empty EntityRef.
EntityRef findEntity(World& world, std::string_view name);

}   // namespace shockwave
