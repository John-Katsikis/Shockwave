#pragma once

// What scene files (SceneFile.cpp) and prefabs (Prefab.cpp) share inside the module.

#include <Engine/Components.h>
#include <Engine/Prefab.h>

#include <memory>
#include <string>
#include <unordered_set>

namespace shockwave::detail {

// SceneFile.cpp: a field's value as a scene file writes it (" 1 0 0"); copying a field
// between two components of the same type.
std::string fieldText(const Field& f, const void* data, const Assets* assets);
void copyFieldValue(const Field& f, void* to, const void* from, const Assets* assets);

// Prefab.cpp: makes root a copy of prefab. keep holds "Component.field" names whose
// values root already has and keeps (the ones a scene file wrote); null keeps none.
void expandPrefab(
    World& world, Entity root, std::shared_ptr<const PrefabAsset> prefab, const std::unordered_set<std::string>* keep,
    const Assets* assets);
// True if the field on a copy's root holds something other than the prefab root's value:
// what a scene file writes for the copy.
bool differsFromPrefab(
    const World& world, Entity root, const ComponentInfo& info, const Field& f, const PrefabAsset& prefab,
    const Assets* assets);

}   // namespace shockwave::detail
