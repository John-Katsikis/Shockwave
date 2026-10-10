#include <Engine/Hierarchy.h>

#include <algorithm>
#include <unordered_map>

namespace shockwave {

Entity parentOf(const World& world, Entity e) {
    const Parent* p = world.get<Parent>(e);
    return p && world.alive(p->entity) ? p->entity : kNullEntity;
}

bool isAncestor(const World& world, Entity ancestor, Entity e) {
    Entity p = parentOf(world, e);
    for (int depth = 0; p.valid() && depth < kMaxHierarchyDepth; ++depth) {
        if (p == ancestor) return true;
        p = parentOf(world, p);
    }
    return false;
}

glm::mat4 worldMatrix(const World& world, Entity e) {
    const Transform* t = world.get<Transform>(e);
    glm::mat4 m = t ? t->matrix() : glm::mat4(1.0f);
    Entity p = parentOf(world, e);
    for (int depth = 0; p.valid() && depth < kMaxHierarchyDepth; ++depth) {
        if (const Transform* pt = world.get<Transform>(p)) m = pt->matrix() * m;
        p = parentOf(world, p);
    }
    return m;
}

std::vector<Entity> childrenOf(const World& world, Entity e) {
    std::vector<Entity> children;
    const Pool<Parent>* parents = world.pool<Parent>();
    if (!parents || !world.alive(e)) return children;
    const auto entities = parents->entities();
    const auto links = parents->components();
    for (size_t i = 0; i < entities.size(); ++i) {
        if (links[i].entity == e) children.push_back(entities[i]);
    }
    std::sort(children.begin(), children.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return children;
}

std::vector<Entity> subtree(const World& world, Entity e) {
    std::vector<Entity> result;
    if (!world.alive(e)) return result;
    // Index the links once instead of scanning every Parent per node.
    std::unordered_map<uint32_t, std::vector<Entity>> children;
    if (const Pool<Parent>* parents = world.pool<Parent>()) {
        const auto entities = parents->entities();
        const auto links = parents->components();
        for (size_t i = 0; i < entities.size(); ++i) {
            if (world.alive(links[i].entity)) children[links[i].entity.index].push_back(entities[i]);
        }
        for (auto& [index, list] : children) {
            std::sort(list.begin(), list.end(), [](Entity a, Entity b) { return a.index < b.index; });
        }
    }
    // Depth-first, so each parent comes right before its own children.
    std::vector<std::pair<Entity, int>> stack{{e, 0}};
    while (!stack.empty()) {
        const auto [current, depth] = stack.back();
        stack.pop_back();
        result.push_back(current);
        if (depth >= kMaxHierarchyDepth) continue;
        if (auto it = children.find(current.index); it != children.end()) {
            for (auto c = it->second.rbegin(); c != it->second.rend(); ++c) stack.emplace_back(*c, depth + 1);
        }
    }
    return result;
}

bool setParent(World& world, Entity child, Entity parent, bool keepWorldTransform) {
    if (!world.alive(child)) return false;
    if (parent.valid() && (!world.alive(parent) || parent == child || isAncestor(world, child, parent))) return false;
    if (parentOf(world, child) == parent) return true;

    if (keepWorldTransform) {
        if (Transform* t = world.get<Transform>(child)) {
            const glm::mat4 placed = worldMatrix(world, child);
            const glm::mat4 parentWorld = parent.valid() ? worldMatrix(world, parent) : glm::mat4(1.0f);
            *t = Transform::fromMatrix(glm::inverse(parentWorld) * placed);
        }
    }
    if (parent.valid()) {
        world.add<Parent>(child, {parent});
    } else {
        world.remove<Parent>(child);
    }
    return true;
}

void destroyWithChildren(World& world, Entity e) {
    for (Entity d : subtree(world, e)) world.destroy(d);
}

Entity cloneWithChildren(World& world, Entity e) {
    const std::vector<Entity> originals = subtree(world, e);
    if (originals.empty()) return kNullEntity;
    std::unordered_map<uint32_t, Entity> copies;
    for (Entity original : originals) copies[original.index] = world.clone(original);
    for (Entity original : originals) {
        Parent* link = world.get<Parent>(copies[original.index]);
        if (!link || original == e) continue;
        if (auto it = copies.find(link->entity.index); it != copies.end()) link->entity = it->second;
    }
    return copies[e.index];
}

}   // namespace shockwave
