#pragma once

// Everything game code normally needs, in one include:
//
//     #include <Shockwave/Game.h>
//     using namespace shockwave;
//
// Components and SW_COMPONENT, the World and EntityRef, GameMode and SW_GAME, input,
// audio, physics, game UI, navigation, logging and glm. See docs/Projects.md.

#include <Animation/AnimationWorld.h>
#include <Audio/Audio.h>
#include <Core/Log.h>
#include <Effects/EffectsWorld.h>
#include <Engine/Components.h>
#include <Engine/Engine.h>
#include <Engine/EntityRef.h>
#include <Engine/Hierarchy.h>
#include <Engine/Prefab.h>
#include <Engine/Schedule.h>
#include <GameFramework/GameModule.h>
#include <GameUI/UIWorld.h>
#include <Input/Input.h>
#include <Navigation/Navigation.h>
#include <Physics/Physics.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
