#pragma once

#include <cstdint>

namespace rivet::render {

// Scheduling hint attached to a render request. Lower value = more urgent.
enum class RenderPriority : std::uint8_t {
    Visible = 0,    // on screen right now
    Impending = 1,  // about to become visible (scroll direction lookahead)
    Prefetch = 2,   // nice to have
};

} // namespace rivet::render
