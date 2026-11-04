#pragma once
#include <cstdint>
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// the game paused (app::Paused): the last frame shown again with the interface over it; window thread
void Redisplay();
// the presentation interval the frames are paced by (D3DPRESENT_INTERVAL_*), as Reset or the render state sets it
void SetPresentInterval(uint32_t interval);

}  // namespace d3d
