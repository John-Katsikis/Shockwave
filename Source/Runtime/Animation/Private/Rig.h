#pragma once

// A model made ready for ozz-animation: its nodes as an ozz skeleton (joint i is model
// node i) and its clips as ozz animations. Built once per loaded model and shared.

#include <Assets/Model.h>
#include <Core/Log.h>

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>

#include <memory>
#include <string>
#include <vector>

namespace shockwave {
SW_LOG_CATEGORY(LogAnimation);
}

namespace shockwave::animation {

struct Rig {
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;   // null: see error
    std::vector<ozz::unique_ptr<ozz::animation::Animation>> clips;   // ModelData::animations order; null: see clipErrors
    std::vector<std::string> clipErrors;
    std::string error;
    float buildMs = 0;
};

// The rig of a loaded model, built on first use. Thread-safe. A reloaded model is a new
// ModelAsset and gets a new rig; old ones go when their model does.
std::shared_ptr<const Rig> rigFor(const std::shared_ptr<const ModelAsset>& model);
uint32_t rigCount();

}   // namespace shockwave::animation
