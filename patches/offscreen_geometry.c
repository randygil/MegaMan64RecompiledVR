#include "patches.h"
#include "graphics.h"

// The game only draws what can be on screen, but the shadows of the enhanced lighting and of the path tracer also
// need what's around the camera out of view: the cliffs, ruins, props and characters behind it or beside it cast
// shadows on the ground in view, and those shadows came and went as the camera turned. While one of those renderers
// is on, everything within OFFSCREEN_REACH units of the camera is drawn too (the tile culling in us.rev1.toml keeps
// terrain tiles that far behind the camera).

#define OFFSCREEN_REACH 0x1000

extern s16 D_80195E90_171290[3]; // Camera position.
extern s32 D_8017B220_156620[4]; // Terrain window: first column, first row, last column, last row.
extern u8 D_80206B40_1E1F40;     // Column of the map's first cell.
extern u8 D_801BC65A_197A5A;     // Row of the map's first cell.
extern u8 D_80210B61_1EBF61;     // Coordinate precision shift.
extern s8 D_801D8D14_1B4114;     // Shift of the characters' least depth.
extern Gfx* D_801A90F0_1844F0;   // Display list head.

// The terrain draw (func_8003912C) only goes through the cells of a window that func_80038934 places ahead of the
// camera: a square that reaches 1.2 to 1.5 draw distances to each side of a point one draw distance in front of it,
// so it holds just a few hundred units behind the camera. It grows to also hold the cells around the camera. The
// window also decides which props and characters are drawn (func_80038CF8), and the map's zones still clamp it after
// this. Called by the us.rev1.toml hook in func_80038934 right after the window is stored.
void terrain_window_extend(void) {
    s32 x, z, cell;

    if (!recomp_get_offscreen_geometry_needed()) {
        return;
    }

    // Cells of 512 units, counted from a corner 0x8000 units away from the origin like func_80038934 does. Negative
    // first cells end up at 0 when the window is clamped to the map.
    x = D_80195E90_171290[0] + 0x8000;
    z = D_80195E90_171290[2] + 0x8000;
    cell = ((x - OFFSCREEN_REACH) >> 9) - D_80206B40_1E1F40;
    if (cell < D_8017B220_156620[0]) {
        D_8017B220_156620[0] = cell;
    }

    cell = ((z - OFFSCREEN_REACH) >> 9) - D_801BC65A_197A5A;
    if (cell < D_8017B220_156620[1]) {
        D_8017B220_156620[1] = cell;
    }

    cell = ((x + OFFSCREEN_REACH) >> 9) - D_80206B40_1E1F40;
    if (cell > D_8017B220_156620[2]) {
        D_8017B220_156620[2] = cell;
    }

    cell = ((z + OFFSCREEN_REACH) >> 9) - D_801BC65A_197A5A;
    if (cell > D_8017B220_156620[3]) {
        D_8017B220_156620[3] = cell;
    }
}

// The props (func_8003AB60, func_8003AFB0) are skipped when their depth in front of the camera (a quarter of it here)
// is under 1, and the first ones also when their center projects too far outside the screen (screen x and y, or NULL
// for the second ones). Called by the us.rev1.toml hooks before those tests: returns the depth to test, and moves the
// projected center into the screen so the props whose depth is within the reach pass while the renderer needs them.
s32 prop_offscreen_cull(s32 quarterDepth, s16* screen) {
    s32 reach = (OFFSCREEN_REACH / 4) << D_80210B61_1EBF61;
    if (!recomp_get_offscreen_geometry_needed() || (quarterDepth < -reach) || (quarterDepth > reach)) {
        return quarterDepth;
    }

    if (screen != NULL) {
        screen[0] = 0;
        screen[1] = 0;
    }

    return (quarterDepth < 1) ? 1 : quarterDepth;
}

// Characters (func_8003A2EC) go through the same tests, but the ones that pass are marked as seen (0x80 in their byte
// 6), which their behavior and the targeting read. The ones drawn only for the shadows get that mark taken back right
// after it's set, so the game sees them exactly as before.
static bool sActorOffscreen = FALSE;

// Called by the us.rev1.toml hook before the depth and screen tests of the characters, like prop_offscreen_cull.
s32 actor_offscreen_cull(s32 quarterDepth, s16* screen) {
    s32 reach = (OFFSCREEN_REACH / 4) << D_80210B61_1EBF61;

    sActorOffscreen = FALSE;
    if (!recomp_get_offscreen_geometry_needed() || (quarterDepth < -reach) || (quarterDepth > reach)) {
        return quarterDepth;
    }

    // The game's own tests (in front of the camera, and the projected center not too far off screen) decide whether
    // it keeps the mark of being seen.
    sActorOffscreen = (quarterDepth < (1 >> D_801D8D14_1B4114)) || ((u16)(screen[0] + 0x160) >= 0x401) ||
        ((u16)(screen[1] + 0x88) >= 0x401);
    screen[0] = 0;
    screen[1] = 0;
    return (quarterDepth < 1) ? 1 : quarterDepth;
}

// Called by the us.rev1.toml hook right after a character is marked as seen.
void actor_offscreen_unmark(u8* actor) {
    if (sActorOffscreen) {
        actor[6] &= 0x7F;
        sActorOffscreen = FALSE;
    }
}

// The trees and bushes of the terrain (records drawn in fade mode 2) go through a walker (func_8007D798) that drops
// every quad with a corner closer to the camera than a threshold, so the cards it passes through don't fill the view.
// That drops the ones behind the camera too, which can't be seen but cast shadows. Called by the us.rev1.toml hook
// with the offsets of the quad's four corners in the walker's table of depths: returns the depth the walker compares
// with the threshold, the nearest corner's like the game, or a far one for a quad entirely behind the camera.
s32 quad_offscreen_depth(s32 offset0, s32 offset1, s32 offset2, s32 offset3) {
    const u8* depths = (const u8*)0x8017C05C;
    s32 d0 = *(const s16*)(depths + offset0);
    s32 d1 = *(const s16*)(depths + offset1);
    s32 d2 = *(const s16*)(depths + offset2);
    s32 d3 = *(const s16*)(depths + offset3);
    s32 nearest = d0;
    s32 farthest = d0;

    nearest = (d1 < nearest) ? d1 : nearest;
    nearest = (d2 < nearest) ? d2 : nearest;
    nearest = (d3 < nearest) ? d3 : nearest;
    farthest = (d1 > farthest) ? d1 : farthest;
    farthest = (d2 > farthest) ? d2 : farthest;
    farthest = (d3 > farthest) ? d3 : farthest;

    if ((farthest < 0) && (nearest > -(OFFSCREEN_REACH << D_80210B61_1EBF61)) && recomp_get_offscreen_geometry_needed()) {
        return 0x7FFF;
    }

    return nearest;
}

// Records drawn in fade mode 3 (walls and big props around the paths) go through another walker (func_8007DA78) that
// hides what's between the camera and Mega Man: quads whose farthest corner is closer than 0xC0 units (shifted), and
// quads closer than the threshold that cover the middle of the screen. The first rule also drops whatever is entirely
// behind the camera, and the projected corners of those quads are meaningless for the second. Called by the
// us.rev1.toml hook with the farthest corner's depth: returns the depth the walker goes on with, a far one for a quad
// entirely behind the camera so it's drawn as is.
s32 quad_offscreen_far_depth(s32 farthest) {
    if ((farthest < 0) && (farthest > -(OFFSCREEN_REACH << D_80210B61_1EBF61)) && recomp_get_offscreen_geometry_needed()) {
        return 0x7FFF;
    }

    return farthest;
}

// The quads both walkers still hide (the ones right in front of the camera or between it and Mega Man) are emitted
// as shadow casters only, so the walls and trees the camera goes past keep their shadows. Called by the us.rev1.toml
// hooks with the G_TRI2 of a quad a walker just dropped, after the vertices it uses were loaded. The walkers also drop
// the G_TRI2 commands whose second word is negative, which aren't geometry the game draws.
void quad_shadow_only(const Gfx* quad) {
    if (((s32)quad->words.w1 < 0) || !recomp_get_offscreen_geometry_needed()) {
        return;
    }

    gEXSetShadowOnly(D_801A90F0_1844F0++, 1);
    *D_801A90F0_1844F0++ = *quad;
    gEXSetShadowOnly(D_801A90F0_1844F0++, 0);
}
