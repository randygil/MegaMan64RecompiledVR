#ifndef VR_H
#define VR_H

#include "patch_helpers.h"

#ifdef MIPS
// VR poses from the host (src/vr/vr_game_api.cpp), in the game's axes (+X right, +Y down, +Z forward) and meters.
// Orientations are the device's right, down and forward vectors. Every field is 32 bits.
typedef struct {
    s32 active; // The headset is showing the game and gameplay should render both eyes.
    f32 ipd; // Distance between the eyes.
    f32 tanHalfX; // Tangents of half the field of view of each eye's image.
    f32 tanHalfY;
    f32 turn; // Right thumbstick X.
    s32 handValid[2]; // 0 left, 1 right.
    s32 trigger[2]; // Triggers held: the left one fires the buster, the right one the special weapon.
    f32 head[3][3];
    f32 headPos[3];
    f32 hands[2][3][3]; // Aim poses: forward points where the controller points.
    f32 handsPos[2][3];
    s32 debug; // Development: 1 simulated VR, 2 also forces combat mode.
} VrFrame;
#else
typedef struct VrFrame VrFrame;
#endif

DECLARE_FUNC(void, recomp_vr_get_frame, VrFrame* out);
// Tells the host whether the frame being built renders both eyes side by side.
DECLARE_FUNC(void, recomp_vr_set_stereo, s32 stereo);
// Short vibration on a controller (0 left, 1 right), strength from 0 to 100.
DECLARE_FUNC(void, recomp_vr_haptic, s32 hand, s32 strength);

#ifdef MIPS
struct PsxMatrix;

// Camera: called by the gameplay camera. Returns TRUE if VR drives it this frame.
// following: the camera follows Mega Man. controllable: the player also controls him (not while talking).
bool vr_camera_update(void* target, s32* distance, s32* yaw, s32* pitch, bool following, bool controllable);
// Replaces the view matrix built by the camera with the head's.
void vr_apply_view(void* view);
// Whether the current frame renders both eyes.
bool vr_stereo_frame(void);
// Places Mega Man's arms at the controllers. Called before he's drawn.
void vr_update_arms(u8* player);
// Adds the precision the game's units can't hold to the matrix of an arm bone that's been moved to a controller.
void vr_adjust_part(s32 bone, void* partMtx);
// Whether a Mega Man part is drawn in VR first person (only the arms are).
bool vr_draw_part(s32 part);
// Whether VR first person is active (camera driven by the head, body hidden except the arms).
bool vr_first_person_active(void);
// Moves new shots to the hand that fired them and sends them where it points.
void vr_aim_shots(void);
// A shot hit an enemy: buzzes the hand that fired last.
void vr_register_hit(void);
// Main draw: replaces the game's projection with one eye's.
s32 vr_debug_knob(void);
void vr_eye_projection(Mtx* projection, u16* perspNorm, s32 eye);
// Main draw: fits the 2D rectangles a task drew between start and end into the eye's half of the screen.
void vr_fix_rects(Gfx* start, Gfx* end, s32 eye, s32 pool);
// Main draw: laser sights from the hands, drawn at the end of each eye.
void vr_draw_lasers(Mtx* projection, Mtx* identMtx);
// Main draw: looks at what a task drew for menus, which are shown on the floating screen instead of in stereo.
void vr_scan_task_output(Gfx* start, Gfx* end, s32 pool);
// The HUD and text are drawn by the pools from this one on.
#define VR_HUD_FIRST_POOL 10
// Main draw: the viewport the HUD pools use in stereo.
void vr_hud_viewport(Vp* out, const Vp* game, s32 eye);
// Main draw: in stereo, fills the frame with the fog color, which replaces the 2D sky.
void vr_draw_background(u8 r, u8 g, u8 b);
// Main draw: called once per frame before drawing.
void vr_begin_frame_draw(void);
#endif

#endif
