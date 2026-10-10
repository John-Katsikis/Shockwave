#include <Engine/EntityRef.h>

#include <Engine/Components.h>
#include <Engine/Hierarchy.h>

namespace shockwave {

void EntityRef::destroy() const {
    if (*this) destroyWithChildren(*m_world, m_entity);
}

EntityRef findEntity(World& world, std::string_view name) {
    EntityRef found;
    world.each<const Name>([&](Entity e, const Name& n) {
        if (!found && n.value == name) found = EntityRef(world, e);
    });
    return found;
}

}   // namespace shockwave
