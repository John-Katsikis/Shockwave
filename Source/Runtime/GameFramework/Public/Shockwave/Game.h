#pragma once

// Everything game code normally needs, in one include:
//
//     #include <Shockwave/Game.h>
//     using namespace shockwave;
//
// Components and SW_COMPONENT, the World and EntityRef, GameMode and SW_GAME, input,
// audio, physics, game UI, logging and glm. See docs/Projects.md.

#include <Audio/Audio.h>
#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/Engine.h>
#include <Engine/EntityRef.h>
#include <Engine/Hierarchy.h>
#include <Engine/Schedule.h>
#include <GameFramework/GameModule.h>
#include <GameUI/UIWorld.h>
#include <Input/Input.h>
#include <Physics/Physics.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
