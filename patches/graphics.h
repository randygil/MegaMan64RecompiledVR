#ifndef __PATCH_GRAPHICS_H__
#define __PATCH_GRAPHICS_H__

#include "patch_helpers.h"

DECLARE_FUNC(void, recomp_get_window_resolution, u32*, u32*);
DECLARE_FUNC(float, recomp_get_target_aspect_ratio, float);
DECLARE_FUNC(float, recomp_get_target_hud_aspect_ratio, float);
DECLARE_FUNC(s32, recomp_get_target_framerate, s32);
DECLARE_FUNC(s32, recomp_high_precision_fb_enabled);
DECLARE_FUNC(float, recomp_get_resolution_scale);
// Whether the renderer needs the geometry around the camera that's out of view (shadows of the enhanced lighting and
// the path tracer), so the game draws more than what's on screen.
DECLARE_FUNC(s32, recomp_get_offscreen_geometry_needed);

#endif
