#pragma once

// Keep CannonBall-SE's existing GLES backend intact and add one presentation
// hook for external replacement textures after the native game frame is drawn.
#define glb glb_base
#include "gl_backend.hpp"
#undef glb

#include "texture_replacement_renderer.hpp"

namespace glb
{
    using namespace glb_base;

    inline void draw(bool useOffscreen, bool drawOverlay)
    {
        glb_base::draw(useOffscreen, drawOverlay);
        texture_replacement_renderer::draw_next_presented_frame();
    }

    inline void shutdown()
    {
        texture_replacement_renderer::shutdown();
        texture_replacement_frame::reset();
        glb_base::shutdown();
    }
}
