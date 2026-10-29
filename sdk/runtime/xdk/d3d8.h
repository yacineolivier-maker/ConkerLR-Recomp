#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// the game paused (app::Paused): the last frame shown again with the interface over it; window thread
void Redisplay();

}  // namespace d3d
