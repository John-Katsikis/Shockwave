#pragma once

#include <Engine/Components.h>

#include <glm/glm.hpp>

#include <vector>

namespace shockwave {

// Parent/child links between entities (see Parent in Components.h).
//
// The reading functions are safe from many threads at once while nothing changes the
// world's structure. The ones that change it follow World's rules: one thread, and
// nothing iterating the world meanwhile.

// Chains deeper than this are cut off, which also keeps a hand-edited loop from hanging.
inline constexpr int kMaxHierarchyDepth = 64;

// e's parent if it has a live one, otherwise kNullEntity (top level).
Entity parentOf(const World& world, Entity e);
// True if ancestor is e's parent, grandparent and so on.
bool isAncestor(const World& world, Entity ancestor, Entity e);
// Where e ends up in the world: its Transform combined with every parent's.
glm::mat4 worldMatrix(const World& world, Entity e);
// e's direct children in creation order. Looks at every Parent, so keep it out of hot loops.
std::vector<Entity> childrenOf(const World& world, Entity e);
// e followed by all its descendants, each parent before its children.
std::vector<Entity> subtree(const World& world, Entity e);

// Attaches child to parent, or detaches it with kNullEntity. With keepWorldTransform the
// child stays where it is and its Transform is recalculated relative to the new parent;
// without, its Transform is kept and it jumps to follow the parent. Returns false and
// changes nothing if that would make a loop (an entity under itself or its descendant).
bool setParent(World& world, Entity child, Entity parent, bool keepWorldTransform = true);
// Destroys e and everything under it.
void destroyWithChildren(World& world, Entity e);
// Copies e and everything under it. The copied children belong to the copied parent;
// the copy of e keeps e's parent. Returns the copy of e.
Entity cloneWithChildren(World& world, Entity e);

} // namespace shockwave
