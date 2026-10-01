#include "patches.h"
#include "vr.h"

// VR first person.
//
// Camera: the head pose drives the gameplay camera. Its yaw and pitch go through the game's camera like the mouse
// camera's do, so culling and everything else that reads the camera keeps working, and the full head rotation (with
// roll) then replaces the view matrix the camera built.
//
// Stereo: the main draw renders the scene twice per frame, once per eye, into the left and right halves of the
// normal 320x240 frame. Each eye uses its own viewport and a projection with the eye's offset from the head folded in
// (the view matrices of every object are built from the head, so the eye offset has to come after them). The host
// submits each half of the frame to the headset as one eye.
//
// Arms: Mega Man's body is hidden except his forearms, which are moved to the controllers: the buster arm on the
// left hand and the other arm on the right. Shots are fired from the hand that's pointing and fly where it points.

typedef struct {
    s16 m[3][3];
    s16 pad;
    s32 t[3];
} VrPsxMatrix;

typedef struct {
    s32 x, y, z;
    s32 pad;
} VrVec3i;

typedef struct {
    s16 x, y, z;
    s16 pad;
} VrSVec3;

extern u8 D_802049B0_1DFDB0[]; // Mega Man.
extern u8 D_80204348_1DF748[]; // Gameplay camera.
extern u8 D_80210B61_1EBF61; // Coordinate precision shift.
extern f32 D_80210B18_1EBF18; // Near plane.
extern f32 D_8021D928_1F8D28; // Far plane.
extern u8 D_802059A0_1E0DA0[]; // Mega Man's shots.
extern Gfx* D_801A90F0_1844F0; // Display list head.

void func_80030AA0_BEA0(VrPsxMatrix* m, VrVec3i* in, VrVec3i* out); // ApplyMatrixLV
void* func_800281C8_35C8(s32 size); // Allocates from the frame's graphics arena.
extern s32 D_801A53D8_1807D8; // Microcode currently loaded by the main draw.
s32 func_80032664_DA64(s32 x, s32 z); // ratan2
void guMtxF2L(float mf[4][4], Mtx* m);

#define PLAYER_POS_X(p) (*(s16*)((p) + 0x14))
#define PLAYER_POS_Y(p) (*(s16*)((p) + 0x16))
#define PLAYER_POS_Z(p) (*(s16*)((p) + 0x18))
#define PLAYER_YAW(p) (*(s16*)((p) + 0x56))
#define PLAYER_BONE_MATRICES(p) ((VrPsxMatrix*)((p) + 0x1C8))
#define PLAYER_BONE_POSITIONS(p) ((VrSVec3*)((p) + 0x3A8))
#define CAMERA_YAW (*(s32*)(D_80204348_1DF748 + 0x10))

#define PLAYER_SHOT_COUNT 16
#define PLAYER_SHOT_SIZE 0x118

// Game units per meter. Mega Man is ~130 units tall.
#define WORLD_SCALE 100.0f
#define EYE_HEIGHT 122.0f
// How far the head can lean away from Mega Man's body, in units. Walking further drags the body's anchor along.
#define LEAN_LIMIT 35.0f
#define CROUCH_LIMIT 70.0f
#define STAND_LIMIT 25.0f
#define SNAP_TURN_ANGLE 0x155 // 30 degrees.
#define NEAR_PLANE 6.0f
// The hand bone sits this far behind the controller's aim point, and shots start this far ahead of it (units).
#define HAND_BACK_OFFSET 6.0f
#define MUZZLE_OFFSET 10.0f
// Laser sights from each hand while armed, in units. They fade out towards the end.
#define LASER_LENGTH 600.0f
#define LASER_WIDTH 0.5f
// Rotation of the arms around the controller's pointing direction, 12-bit angle, tuned by eye.
#define ARM_ROLL 0
// The 2D elements (HUD, text) are drawn at half size in the middle of each eye, shifted this much (in 1/4 pixels)
// towards the nose so they appear a comfortable distance away instead of at infinity.
#define HUD_EYE_SHIFT 8

// The arm bones: upper arm, forearm and hand, left (buster) and right.
static const s32 sArmBones[2][3] = { { 5, 6, 7 }, { 2, 3, 4 } };
// The hips and legs, drawn where the game animates them so the feet (and kicks) show when looking down.
#define LEG_PARTS_MASK 0x7F00
// Where the shoulders sit relative to the eyes (units): below, to the side and behind.
#define SHOULDER_DROP 20.0f
#define SHOULDER_SIDE 16.0f
#define SHOULDER_BACK 4.0f
// Mega Man's arms are much shorter than the player's. The upper arm stretches up to this much to reach the elbow, and
// past that the shoulder slides towards the hand (it's out of view by then).
#define UPPER_ARM_MAX_STRETCH 1.6f
#define UPPER_ARM_MIN_STRETCH 0.5f

static VrFrame sFrame;
static bool sActive = FALSE; // VR drives the camera this frame.
static bool sWasActive = FALSE;
static s32 sStereoFrames = 0; // Set by the camera, counted down by the main draw.
static bool sStereoFrame = FALSE; // The frame being drawn renders both eyes.
static s32 sYaw = 0; // Rotation from the tracking space to the world, around the vertical axis.
static f32 sYawSin = 0.0f;
static f32 sYawCos = 1.0f;
static f32 sAnchor[3]; // Tracking space head position that sits at Mega Man's eyes.
static bool sTurnLatched = FALSE;
static f32 sEye[3]; // World position of the head, in units.
static f32 sHead[3][3]; // World orientation of the head: right, down and forward.
static f32 sHandPos[2][3];
// The camera's position below the game's precision, in view space, and the same for the arm bones (in the world).
static f32 sViewResidual[3];
// How far the head can turn away from the rotation the game draws with before it follows (cosine of 3 degrees).
#define GAME_ROTATION_HOLD_COS 0.99863f
// The rotation the game draws with, which only follows the head once it has turned far enough, and the rotation from it
// to the head's, which the eye projections apply (see vr_apply_view).
static f32 sGameRotation[3][3];
static bool sGameRotationValid = FALSE;
static f32 sViewRotation[3][3];
static bool sViewRotated = FALSE;
static f32 sBoneFraction[16][3];
static f32 sHand[2][3][3];

// Arm bone axes captured from Mega Man's model when VR starts: the forearm direction and a perpendicular reference,
// in each bone's own space.
static bool sArmAxesCaptured = FALSE;
static f32 sArmAxis[2][3][3];
static f32 sArmRef[2][3][3];
static f32 sArmScale[2][3];
static f32 sArmLength[2][2]; // Upper arm and forearm.

static bool sShotTracked[PLAYER_SHOT_COUNT];
// Frames left showing a menu on the floating screen, set when a menu is seen being drawn.
static s32 sMenuFrames = 0;
#define MENU_HOLD_FRAMES 8
static s32 sLastShotHand = 0;
static bool sShotAimed[PLAYER_SHOT_COUNT];

float sinf(float x);
float cosf(float x);

static f32 vr_sqrtf(f32 x) {
    return __builtin_sqrtf(x);
}

static f32 dot3(const f32 a[3], const f32 b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const f32 a[3], const f32 b[3], f32 out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static bool normalize3(f32 v[3]) {
    f32 length = vr_sqrtf(dot3(v, v));
    if (length < 1e-6f) {
        return FALSE;
    }
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return TRUE;
}

static f32 clampf(f32 v, f32 min, f32 max) {
    return v < min ? min : (v > max ? max : v);
}

// Tracking space to world: rotation around the vertical axis by sYaw. A yaw's forward is (sin, 0, cos).
static void to_world(const f32 in[3], f32 out[3]) {
    f32 x = in[0];
    f32 z = in[2];
    out[0] = sYawCos * x + sYawSin * z;
    out[1] = in[1];
    out[2] = -sYawSin * x + sYawCos * z;
}

static s32 yaw_of(const f32 forward[3]) {
    return func_80032664_DA64((s32)(forward[0] * 4096.0f), (s32)(forward[2] * 4096.0f)) & 0xFFF;
}

static s32 pitch_of(const f32 forward[3]) {
    f32 horizontal = vr_sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
    return func_80032664_DA64((s32)(forward[1] * 4096.0f), (s32)(horizontal * 4096.0f));
}

// Bone matrices are 4.12 fixed point and include the model's scale. Bone vector math uses them as float matrices.
static void bone_to_float(const VrPsxMatrix* m, f32 out[3][3]) {
    s32 r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            out[r][c] = m->m[r][c] / 4096.0f;
        }
    }
}

// Transposed matrix times vector: brings a world direction into a bone's own space (up to its scale).
static void apply_transposed(f32 m[3][3], const f32 v[3], f32 out[3]) {
    out[0] = m[0][0] * v[0] + m[1][0] * v[1] + m[2][0] * v[2];
    out[1] = m[0][1] * v[0] + m[1][1] * v[1] + m[2][1] * v[2];
    out[2] = m[0][2] * v[0] + m[1][2] * v[1] + m[2][2] * v[2];
}

static void capture_arm_axes(u8* player) {
    s32 side, i;
    f32 forward[3];

    // Mega Man's facing direction, used as the reference perpendicular to the arm.
    forward[0] = -sinf(PLAYER_YAW(player) * (6.2831853f / 4096.0f));
    forward[1] = 0.0f;
    forward[2] = -cosf(PLAYER_YAW(player) * (6.2831853f / 4096.0f));

    for (side = 0; side < 2; side++) {
        for (i = 0; i < 3; i++) {
            // Each bone points at the next joint; the hand points the way its forearm does.
            s32 from = sArmBones[side][i == 2 ? 1 : i];
            s32 to = sArmBones[side][i == 2 ? 2 : i + 1];
            VrSVec3* a = &PLAYER_BONE_POSITIONS(player)[from];
            VrSVec3* b = &PLAYER_BONE_POSITIONS(player)[to];
            f32 along[3];
            f32 m[3][3];
            f32* axis = sArmAxis[side][i];
            f32* ref = sArmRef[side][i];
            f32 length, d;

            along[0] = (f32)(b->x - a->x);
            along[1] = (f32)(b->y - a->y);
            along[2] = (f32)(b->z - a->z);
            length = vr_sqrtf(dot3(along, along));
            if (!normalize3(along)) {
                along[0] = 0.0f;
                along[1] = 1.0f;
                along[2] = 0.0f;
                length = 18.0f;
            }
            if (i < 2) {
                sArmLength[side][i] = length;
            }

            bone_to_float(&PLAYER_BONE_MATRICES(player)[sArmBones[side][i]], m);
            sArmScale[side][i] = vr_sqrtf(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]);
            apply_transposed(m, along, axis);
            apply_transposed(m, forward, ref);
            normalize3(axis);
            d = dot3(ref, axis);
            ref[0] -= d * axis[0];
            ref[1] -= d * axis[1];
            ref[2] -= d * axis[2];
            if (!normalize3(ref)) {
                // Degenerate: pick any perpendicular.
                f32 other[3] = { 1.0f, 0.0f, 0.0f };
                if (axis[0] > 0.9f || axis[0] < -0.9f) {
                    other[0] = 0.0f;
                    other[1] = 1.0f;
                }
                cross3(axis, other, ref);
                normalize3(ref);
            }
        }
    }
    sArmAxesCaptured = TRUE;
}

bool vr_camera_update(void* targetPtr, s32* distance, s32* yaw, s32* pitch, bool following, bool controllable) {
    VrVec3i* target = (VrVec3i*)targetPtr;
    u8* player = D_802049B0_1DFDB0;
    f32 lean[3];
    f32 world[3];
    f32 horizontal;
    s32 headYaw;
    s32 r, h;

    recomp_vr_get_frame(&sFrame);
    sActive = FALSE;
    sViewRotated = FALSE;

    // Talking keeps the headset view (the game only takes Mega Man's controls), but menus and cutscenes go to the
    // floating screen.
    if (!sFrame.active || !following || sMenuFrames > 0) {
        sWasActive = FALSE;
        return FALSE;
    }

    // Development: arm Mega Man anywhere to test shooting.
    if ((sFrame.debug & 0xFF) == 2) {
        player[0x171] = 1;
    }

    // Line the tracking space up with Mega Man when VR takes over the camera, so he keeps facing the same way.
    if (!sWasActive) {
        sYaw = (PLAYER_YAW(player) + 0x800) - yaw_of(sFrame.head[2]);
        sAnchor[0] = sFrame.headPos[0];
        sAnchor[1] = sFrame.headPos[1];
        sAnchor[2] = sFrame.headPos[2];
        sArmAxesCaptured = FALSE;
        sGameRotationValid = FALSE;
    }

    // Snap turning with the right stick.
    if (!sTurnLatched && (sFrame.turn > 0.7f || sFrame.turn < -0.7f)) {
        sYaw += sFrame.turn > 0.0f ? SNAP_TURN_ANGLE : -SNAP_TURN_ANGLE;
        sTurnLatched = TRUE;
    } else if (sFrame.turn < 0.3f && sFrame.turn > -0.3f) {
        sTurnLatched = FALSE;
    }
    sYaw &= 0xFFF;
    sYawSin = sinf(sYaw * (6.2831853f / 4096.0f));
    sYawCos = cosf(sYaw * (6.2831853f / 4096.0f));

    for (r = 0; r < 3; r++) {
        to_world(sFrame.head[r], sHead[r]);
    }

    // Leaning moves the eyes away from Mega Man's head, up to a limit. Past it, the anchor follows the player's head so
    // walking around the room doesn't leave the camera behind for good.
    lean[0] = sFrame.headPos[0] - sAnchor[0];
    lean[1] = sFrame.headPos[1] - sAnchor[1];
    lean[2] = sFrame.headPos[2] - sAnchor[2];
    horizontal = vr_sqrtf(lean[0] * lean[0] + lean[2] * lean[2]) * WORLD_SCALE;
    if (horizontal > LEAN_LIMIT) {
        f32 excess = (horizontal - LEAN_LIMIT) / horizontal;
        sAnchor[0] += lean[0] * excess;
        sAnchor[2] += lean[2] * excess;
        lean[0] -= lean[0] * excess;
        lean[2] -= lean[2] * excess;
    }
    lean[0] *= WORLD_SCALE;
    lean[1] = clampf(lean[1] * WORLD_SCALE, -STAND_LIMIT, CROUCH_LIMIT);
    lean[2] *= WORLD_SCALE;
    to_world(lean, world);
    sEye[0] = PLAYER_POS_X(player) + world[0];
    sEye[1] = PLAYER_POS_Y(player) - EYE_HEIGHT + world[1];
    sEye[2] = PLAYER_POS_Z(player) + world[2];

    // Hands, relative to the head.
    for (h = 0; h < 2; h++) {
        f32 offset[3];
        offset[0] = (sFrame.handsPos[h][0] - sFrame.headPos[0]) * WORLD_SCALE;
        offset[1] = (sFrame.handsPos[h][1] - sFrame.headPos[1]) * WORLD_SCALE;
        offset[2] = (sFrame.handsPos[h][2] - sFrame.headPos[2]) * WORLD_SCALE;
        to_world(offset, world);
        sHandPos[h][0] = sEye[0] + world[0];
        sHandPos[h][1] = sEye[1] + world[1];
        sHandPos[h][2] = sEye[2] + world[2];
        for (r = 0; r < 3; r++) {
            to_world(sFrame.hands[h][r], sHand[h][r]);
        }
    }

    // Mega Man faces where the player looks, so the left stick moves relative to the head (sideways input strafes).
    // Looking down (at a chest, say) leaves almost nothing of the forward vector on the ground, and its direction
    // becomes noise. The top of the head points forward then, so the heading comes from forward plus up.
    {
        f32 heading[3];
        heading[0] = sHead[2][0] - sHead[1][0];
        heading[1] = 0.0f;
        heading[2] = sHead[2][2] - sHead[1][2];
        headYaw = yaw_of(heading);
    }
    if (controllable) {
        PLAYER_YAW(player) = (headYaw - 0x800) & 0xFFF;
    }
    CAMERA_YAW = headYaw;

    target->x = (s32)sEye[0];
    target->y = (s32)sEye[1];
    target->z = (s32)sEye[2];
    *distance = 0;
    *yaw = headYaw;
    *pitch = pitch_of(sHead[2]);

    sActive = TRUE;
    sWasActive = TRUE;
    sStereoFrames = 2;
    return TRUE;
}

void vr_apply_view(void* viewPtr) {
    VrPsxMatrix* view = (VrPsxMatrix*)viewPtr;
    VrVec3i offset;
    s32 shift = D_80210B61_1EBF61;
    f32 gameRotation[3][3];
    f32 viewError[3];
    s32 r, c, k;

    if (!sActive) {
        return;
    }

    // The game works out where everything is relative to the camera in whole units, rotated by the view in 4.12 fixed
    // point, so with the head always turning a little every object and terrain tile would jump by up to a unit on its
    // own from one frame to the next, and the textures would crawl. So the rotation the game draws with (and culls
    // with) only follows the head once it has turned a few degrees away, which keeps those positions still, and the
    // eye projections turn the rest of the way, in floating point.
    if (sGameRotationValid) {
        f32 trace = dot3(sHead[0], sGameRotation[0]) + dot3(sHead[1], sGameRotation[1]) +
                    dot3(sHead[2], sGameRotation[2]);
        if ((trace - 1.0f) * 0.5f < GAME_ROTATION_HOLD_COS) {
            sGameRotationValid = FALSE;
        }
    }
    if (!sGameRotationValid) {
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                sGameRotation[r][c] = sHead[r][c];
            }
        }
        sGameRotationValid = TRUE;
    }
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            view->m[r][c] = (s16)clampf(sGameRotation[r][c] * 4096.0f, -32767.0f, 32767.0f);
            gameRotation[r][c] = view->m[r][c] / 4096.0f;
        }
    }
    // From the game's view to the head's: head * game^T.
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            sViewRotation[r][c] = dot3(sHead[r], gameRotation[c]);
        }
    }
    sViewRotated = TRUE;

    // The camera's position is also in whole units (scaled by the precision shift), and the view's translation is
    // rounded. What's left over of both is applied in the eye projections too.
    {
        f32 unit = (f32)(1 << shift);
        f32 residual[3];
        offset.x = (s32)(-sEye[0] * unit);
        offset.y = (s32)(-sEye[1] * unit);
        offset.z = (s32)(-sEye[2] * unit);
        residual[0] = sEye[0] + offset.x / unit;
        residual[1] = sEye[1] + offset.y / unit;
        residual[2] = sEye[2] + offset.z / unit;
        func_80030AA0_BEA0(view, &offset, (VrVec3i*)view->t);
        for (r = 0; r < 3; r++) {
            viewError[r] = (view->t[r] - (gameRotation[r][0] * offset.x + gameRotation[r][1] * offset.y +
                                          gameRotation[r][2] * offset.z)) / unit;
        }
        for (r = 0; r < 3; r++) {
            sViewResidual[r] = dot3(sHead[r], residual);
            for (k = 0; k < 3; k++) {
                sViewResidual[r] += sViewRotation[r][k] * viewError[k];
            }
        }
    }
}

// Adds the part of an arm bone's position that doesn't fit in the game's whole units to the part's view matrix, so the
// arms follow the controllers smoothly.
void vr_adjust_part(s32 bone, void* partMtxPtr) {
    VrPsxMatrix* partMtx = (VrPsxMatrix*)partMtxPtr;
    f32 unit = (f32)(1 << D_80210B61_1EBF61);
    s32 r;

    if (!sActive || bone < 0 || bone >= 16) {
        return;
    }
    for (r = 0; r < 3; r++) {
        partMtx->t[r] += (s32)(dot3(sGameRotation[r], sBoneFraction[bone]) * unit);
    }
}

void vr_begin_frame_draw(void) {
    if (sMenuFrames > 0) {
        sMenuFrames--;
    }
    sStereoFrame = sStereoFrames > 0;
    if (sStereoFrames > 0) {
        sStereoFrames--;
    }
    recomp_vr_set_stereo(sStereoFrame ? 1 : 0);
}

s32 vr_debug_knob(void) {
    return sFrame.debug >> 8;
}

bool vr_stereo_frame(void) {
    return sStereoFrame;
}

bool vr_first_person_active(void) {
    return sActive || sStereoFrame;
}

// Builds a bone matrix that maps the bone's captured axis to the frame's forward direction (the frame's rows are right,
// down and forward), stretched along that axis by the given factor.
static void aim_bone(VrPsxMatrix* out, s32 side, s32 bone, const f32 frame[3][3], f32 stretch) {
    const f32* axis = sArmAxis[side][bone];
    const f32* ref = sArmRef[side][bone];
    f32 axis3[3];
    f32 localThird[3];
    f32 worldUp[3];
    f32 worldThird[3];
    f32 rollSin = sinf(ARM_ROLL * (6.2831853f / 4096.0f));
    f32 rollCos = cosf(ARM_ROLL * (6.2831853f / 4096.0f));
    f32 scale = sArmScale[side][bone];
    s32 r, c;

    axis3[0] = axis[0];
    axis3[1] = axis[1];
    axis3[2] = axis[2];
    cross3(axis3, ref, localThird);

    // Frame: forward, up (the reference faces up, rolled by ARM_ROLL) and their cross product.
    for (r = 0; r < 3; r++) {
        worldUp[r] = -frame[1][r] * rollCos + frame[0][r] * rollSin;
    }
    cross3(frame[2], worldUp, worldThird);

    // R = W * L^T, with L's columns (axis, ref, third) and W's (forward, up, third). Stretching along the axis scales
    // the forward term.
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            f32 v = frame[2][r] * axis3[c] * stretch + worldUp[r] * ref[c] + worldThird[r] * localThird[c];
            out->m[r][c] = (s16)clampf(v * scale * 4096.0f, -32767.0f, 32767.0f);
        }
    }
}

// A frame (right, down, forward) pointing along dir, with its down direction as close to the given one as possible.
static bool segment_frame(const f32 dir[3], const f32 down[3], f32 out[3][3]) {
    f32 d;
    s32 k;

    for (k = 0; k < 3; k++) {
        out[2][k] = dir[k];
    }
    if (!normalize3(out[2])) {
        return FALSE;
    }
    d = dot3(down, out[2]);
    for (k = 0; k < 3; k++) {
        out[1][k] = down[k] - d * out[2][k];
    }
    if (!normalize3(out[1])) {
        return FALSE;
    }
    cross3(out[1], out[2], out[0]);
    return TRUE;
}

static void set_bone_position(u8* player, s32 bone, const f32 pos[3]) {
    VrSVec3* p = &PLAYER_BONE_POSITIONS(player)[bone];
    p->x = (s16)pos[0];
    p->y = (s16)pos[1];
    p->z = (s16)pos[2];
    sBoneFraction[bone][0] = pos[0] - p->x;
    sBoneFraction[bone][1] = pos[1] - p->y;
    sBoneFraction[bone][2] = pos[2] - p->z;
}

void vr_update_arms(u8* player) {
    s32 side, k;
    f32 bodyForward[3], bodyRight[3];

    if (!sActive) {
        return;
    }
    if (!sArmAxesCaptured) {
        capture_arm_axes(player);
    }

    // The body faces where the head does, on the ground plane.
    bodyForward[0] = -sinf(PLAYER_YAW(player) * (6.2831853f / 4096.0f));
    bodyForward[1] = 0.0f;
    bodyForward[2] = -cosf(PLAYER_YAW(player) * (6.2831853f / 4096.0f));
    bodyRight[0] = bodyForward[2];
    bodyRight[1] = 0.0f;
    bodyRight[2] = -bodyForward[0];

    for (side = 0; side < 2; side++) {
        const f32 (*hand)[3] = sHand[side];
        f32 wrist[3], elbow[3], shoulder[3], upper[3];
        f32 upperFrame[3][3];
        f32 sideSign = side == 0 ? -1.0f : 1.0f;
        f32 upperLength = sArmLength[side][0];
        f32 forearmLength = sArmLength[side][1];
        f32 reach, stretch, offset;

        if (!sFrame.handValid[side]) {
            continue;
        }

        // The forearm points where the controller does, like holding it. The armed buster arm's forearm is the
        // cannon, so it's moved forward to sit on the controller instead of reaching back towards the face.
        offset = (side == 0 && player[0x171] != 0) ? forearmLength * 0.6f : -HAND_BACK_OFFSET;
        for (k = 0; k < 3; k++) {
            wrist[k] = sHandPos[side][k] + hand[2][k] * offset;
            elbow[k] = wrist[k] - hand[2][k] * forearmLength;
            shoulder[k] = sEye[k] + (k == 1 ? SHOULDER_DROP : 0.0f) + bodyRight[k] * SHOULDER_SIDE * sideSign -
                          bodyForward[k] * SHOULDER_BACK;
            upper[k] = elbow[k] - shoulder[k];
        }

        // The upper arm spans the shoulder to the elbow, stretched within limits.
        reach = vr_sqrtf(dot3(upper, upper));
        stretch = upperLength > 0.0f ? reach / upperLength : 1.0f;
        if (stretch > UPPER_ARM_MAX_STRETCH) {
            f32 slide = (reach - upperLength * UPPER_ARM_MAX_STRETCH) / reach;
            for (k = 0; k < 3; k++) {
                shoulder[k] += upper[k] * slide;
                upper[k] -= upper[k] * slide;
            }
            stretch = UPPER_ARM_MAX_STRETCH;
        }
        if (stretch < UPPER_ARM_MIN_STRETCH) {
            stretch = UPPER_ARM_MIN_STRETCH;
        }

        set_bone_position(player, sArmBones[side][0], shoulder);
        set_bone_position(player, sArmBones[side][1], elbow);
        set_bone_position(player, sArmBones[side][2], wrist);
        if (segment_frame(upper, hand[1], upperFrame)) {
            aim_bone(&PLAYER_BONE_MATRICES(player)[sArmBones[side][0]], side, 0, upperFrame, stretch);
        }
        aim_bone(&PLAYER_BONE_MATRICES(player)[sArmBones[side][1]], side, 1, hand, 1.0f);
        aim_bone(&PLAYER_BONE_MATRICES(player)[sArmBones[side][2]], side, 2, hand, 1.0f);
    }
}

// The arms (moved to the controllers) and the legs. The head and torso would sit around the camera.
bool vr_draw_part(s32 part) {
    s32 side, i;

    for (side = 0; side < 2; side++) {
        for (i = 0; i < 3; i++) {
            if (part == sArmBones[side][i]) {
                return sFrame.handValid[side];
            }
        }
    }
    return (LEG_PARTS_MASK & (1 << part)) != 0;
}

// Shots: when a shot appears, it's moved to the muzzle of the closest hand and sent where that hand points, at the
// speed the game gave it.
void vr_aim_shots(void) {
    s32 i;

    for (i = 0; i < PLAYER_SHOT_COUNT; i++) {
        u8* shot = &D_802059A0_1E0DA0[i * PLAYER_SHOT_SIZE];
        s32* pos = (s32*)(shot + 0x1C);
        s16* vel = (s16*)(shot + 0x44);
        f32 speed, best;
        s32 hand, h, k;
        f32 muzzle[3];

        if (!(shot[0] & 1)) {
            sShotTracked[i] = FALSE;
            continue;
        }
        if (sShotTracked[i]) {
            // No homing: the game's lock-on would curve the shot away from where the hand pointed.
            if (sActive && sShotAimed[i]) {
                *(u32*)(shot + 0xC0) = 0;
                *(u32*)(shot + 0xC4) = 0;
            }
            continue;
        }
        sShotAimed[i] = FALSE;
        sShotTracked[i] = TRUE;
        if (!sActive) {
            continue;
        }

        // The hand whose trigger is held fired it (the right trigger is the special weapon). If both or neither are,
        // the closest hand to where the game spawned the shot.
        hand = 0;
        best = 1e30f;
        for (h = 0; h < 2 && sFrame.trigger[0] == sFrame.trigger[1]; h++) {
            f32 dx = (pos[0] >> 16) - sHandPos[h][0];
            f32 dy = (pos[1] >> 16) - sHandPos[h][1];
            f32 dz = (pos[2] >> 16) - sHandPos[h][2];
            f32 d = dx * dx + dy * dy + dz * dz;
            if (sFrame.handValid[h] && d < best) {
                best = d;
                hand = h;
            }
        }
        if (sFrame.trigger[0] != sFrame.trigger[1]) {
            hand = sFrame.trigger[1] ? 1 : 0;
        }
        if (!sFrame.handValid[hand]) {
            continue;
        }
        sLastShotHand = hand;

        speed = vr_sqrtf((f32)vel[0] * vel[0] + (f32)vel[1] * vel[1] + (f32)vel[2] * vel[2]);
        for (k = 0; k < 3; k++) {
            muzzle[k] = sHandPos[hand][k] + sHand[hand][2][k] * MUZZLE_OFFSET;
            vel[k] = (s16)clampf(sHand[hand][2][k] * speed, -32767.0f, 32767.0f);
            pos[k] = (s32)(muzzle[k] * 65536.0f);
        }
        *(s16*)(shot + 0x14) = (s16)muzzle[0];
        *(s16*)(shot + 0x16) = (s16)muzzle[1];
        *(s16*)(shot + 0x18) = (s16)muzzle[2];
        *(s16*)(shot + 0x90) = (s16)muzzle[0];
        *(s16*)(shot + 0x92) = (s16)muzzle[1];
        *(s16*)(shot + 0x94) = (s16)muzzle[2];
        *(u32*)(shot + 0xC0) = 0;
        *(u32*)(shot + 0xC4) = 0;
        sShotAimed[i] = TRUE;
        recomp_vr_haptic(hand, 60);
    }
}

void vr_register_hit(void) {
    if (sActive) {
        recomp_vr_haptic(sLastShotHand, 100);
    }
}

static void mtx_l2f(Mtx* m, float mf[4][4]) {
    s32* m1 = &m->m[0][0];
    s32* m2 = &m->m[2][0];
    s32 r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 2; c++) {
            s32 hi = *m1++;
            s32 lo = *m2++;
            mf[r][2 * c] = (float)(s32)((hi & 0xFFFF0000) | ((lo >> 16) & 0xFFFF)) / 65536.0f;
            mf[r][2 * c + 1] = (float)(s32)((hi << 16) | (lo & 0xFFFF)) / 65536.0f;
        }
    }
}

// Replaces the game's perspective with one eye's: the field of view of the eye's image and the eye's offset from the
// head. Same PSX conventions as the game's matrix (Y down, w = z * scale).
void vr_eye_projection(Mtx* projection, u16* perspNorm, s32 eye) {
    float mf[4][4];
    float scale;
    float near = NEAR_PLANE;
    float far = D_8021D928_1F8D28;
    float eyeOffset = (eye == 0 ? -0.5f : 0.5f) * sFrame.ipd * WORLD_SCALE;
    s32 r, c;

    mtx_l2f(projection, mf);
    scale = mf[2][3];
    if (scale <= 0.0f || far <= near) {
        scale = 1.0f;
        far = 20000.0f;
    }

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            mf[r][c] = 0.0f;
        }
    }
    mf[0][0] = scale / sFrame.tanHalfX;
    mf[1][1] = -scale / sFrame.tanHalfY;
    mf[2][2] = -(near + far) / (near - far) * scale;
    mf[2][3] = scale;
    mf[3][2] = 2.0f * near * far / (near - far) * scale;
    // The eye sits eyeOffset to the right of the head (plus the camera's position below the game's precision), so
    // everything it sees is moved the other way.
    {
        f32 d[3];
        d[0] = eyeOffset + sViewResidual[0];
        d[1] = sViewResidual[1];
        d[2] = sViewResidual[2];
        for (c = 0; c < 4; c++) {
            mf[3][c] -= d[0] * mf[0][c] + d[1] * mf[1][c] + d[2] * mf[2][c];
        }
    }
    // Before all that, the rotation from the game's view to the head's (row vectors, so its transpose multiplies from
    // the left).
    if (sViewRotated) {
        f32 rotated[3][4];
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 4; c++) {
                rotated[r][c] = sViewRotation[0][r] * mf[0][c] + sViewRotation[1][r] * mf[1][c] +
                                sViewRotation[2][r] * mf[2][c];
            }
        }
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 4; c++) {
                mf[r][c] = rotated[r][c];
            }
        }
    }
    guMtxF2L(mf, projection);

    *perspNorm = (u16)((2.0f * 65536.0f) / (near + far));
    if (*perspNorm == 0) {
        *perspNorm = 1;
    }
}

#define SKY_POOL 3

// Menus that set their own viewport can't be fitted into the eyes, so they're shown on the floating screen.
void vr_scan_task_output(Gfx* start, Gfx* end, s32 pool) {
    Gfx* g;

    if (pool < VR_HUD_FIRST_POOL || !sFrame.active) {
        return;
    }
    for (g = start; g < end; g++) {
        u32 w0 = g->words.w0;
        u32 op = w0 >> 24;
        // G_MOVEMEM of a viewport (F3DEX2). HUD elements loading an orthographic projection are fine: they go through
        // the HUD viewport.
        if (op == 0xDC && (w0 & 0xFF) == 0x08) {
            sMenuFrames = MENU_HOLD_FRAMES;
            return;
        }
    }
}

void vr_draw_background(u8 r, u8 g, u8 b) {
    u16 color = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);

    gDPPipeSync(D_801A90F0_1844F0++);
    gDPSetCycleType(D_801A90F0_1844F0++, G_CYC_FILL);
    gDPSetRenderMode(D_801A90F0_1844F0++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetFillColor(D_801A90F0_1844F0++, ((u32)color << 16) | color);
    gDPFillRectangle(D_801A90F0_1844F0++, 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);
    gDPPipeSync(D_801A90F0_1844F0++);
    gDPSetCycleType(D_801A90F0_1844F0++, G_CYC_1CYCLE);
}

// 10.2 fixed point screen coordinates: half size, centered in the eye's half of the frame.
// The HUD is scaled by HUD_SCALE_NUM / HUD_SCALE_DEN around the center of each eye.
#define HUD_SCALE_NUM 2
#define HUD_SCALE_DEN 5

static s32 hud_center_x(s32 eye) {
    return (SCREEN_WIDTH / 4 + eye * (SCREEN_WIDTH / 2)) * 4 + (eye == 0 ? HUD_EYE_SHIFT : -HUD_EYE_SHIFT);
}

static u32 fix_x(u32 x, s32 eye) {
    s32 shifted = hud_center_x(eye) + ((s32)x - (SCREEN_WIDTH / 2) * 4) * HUD_SCALE_NUM / HUD_SCALE_DEN;
    return (u32)(shifted < 0 ? 0 : shifted) & 0xFFF;
}

static u32 fix_y(u32 y) {
    s32 shifted = (SCREEN_HEIGHT / 2) * 4 + ((s32)y - (SCREEN_HEIGHT / 2) * 4) * HUD_SCALE_NUM / HUD_SCALE_DEN;
    return (u32)(shifted < 0 ? 0 : shifted) & 0xFFF;
}

// Viewport for the HUD pools in stereo: covers the same area the HUD rectangles are scaled into, so HUD elements drawn
// with their own (orthographic) projection line up with the rectangles.
void vr_hud_viewport(Vp* out, const Vp* game, s32 eye) {
    *out = *game;
    out->vp.vscale[0] = (SCREEN_WIDTH / 2) * 4 * HUD_SCALE_NUM / HUD_SCALE_DEN;
    out->vp.vtrans[0] = hud_center_x(eye);
    out->vp.vscale[1] = (SCREEN_HEIGHT / 2) * 4 * HUD_SCALE_NUM / HUD_SCALE_DEN;
    out->vp.vtrans[1] = (SCREEN_HEIGHT / 2) * 4;
}

static s16 hud_step(s16 step) {
    s32 scaled = step * HUD_SCALE_DEN / HUD_SCALE_NUM;
    return (s16)(scaled > 0x7FFF ? 0x7FFF : (scaled < -0x8000 ? -0x8000 : scaled));
}

static u32 fix_coords(u32 word, s32 eye, bool hud) {
    u32 x = (word >> 12) & 0xFFF;
    u32 y = word & 0xFFF;
    if (!hud) {
        // Backgrounds (the sky) cover the whole eye: squeezed horizontally, identical in both eyes like anything at
        // an infinite distance.
        return (word & 0xFF000FFF) | ((((x >> 1) + eye * (SCREEN_WIDTH / 2) * 4) & 0xFFF) << 12);
    }
    return (word & 0xFF000000) | (fix_x(x, eye) << 12) | fix_y(y);
}

static u32 fix_scissor(u32 word, s32 eye) {
    u32 x = (word >> 12) & 0xFFF;
    return (word & 0xFF000FFF) | ((((x >> 1) + eye * (SCREEN_WIDTH / 2) * 4) & 0xFFF) << 12);
}

static s16 double_step(s16 step) {
    s32 doubled = step * 2;
    return (s16)(doubled > 0x7FFF ? 0x7FFF : (doubled < -0x8000 ? -0x8000 : doubled));
}

void vr_fix_rects(Gfx* start, Gfx* end, s32 eye, s32 pool) {
    Gfx* g;
    // The HUD and text are drawn by the last pools, backgrounds by the first ones.
    bool hud = pool >= VR_HUD_FIRST_POOL;

    for (g = start; g < end; g++) {
        u32 op = g->words.w0 >> 24;
        switch (op) {
            case 0xE4: // G_TEXRECT
            case 0xE5: // G_TEXRECTFLIP
                // The sky is a flat picture that scrolls with the game's camera and field of view, so in the headset
                // it slides at a different rate than the world when turning, which is very uncomfortable. It's left
                // out, and vr_draw_background fills its place with the fog color instead.
                if (pool == SKY_POOL) {
                    Gfx* last = (g + 2 < end) ? g + 2 : g;
                    for (; g <= last; g++) {
                        g->words.w0 = 0; // G_NOOP
                        g->words.w1 = 0;
                    }
                    g--;
                    break;
                }
                g->words.w0 = fix_coords(g->words.w0, eye, hud);
                g->words.w1 = fix_coords(g->words.w1, eye, hud);
                // The texture steps follow in the second RDP half command (F3DEX2 0xF1, F3DEX 0xB3). Screen X is
                // halved in both cases, screen Y only for the HUD. Flipped rectangles swap which step goes with X.
                if (g + 2 < end) {
                    u32 halfOp = g[2].words.w0 >> 24;
                    if (halfOp == 0xF1 || halfOp == 0xB3) {
                        u32 steps = g[2].words.w1;
                        s16 dsdx = (s16)(steps >> 16);
                        s16 dtdy = (s16)(steps & 0xFFFF);
                        bool flip = op == 0xE5;
                        if (hud) {
                            dsdx = hud_step(dsdx);
                            dtdy = hud_step(dtdy);
                        } else if (!flip) {
                            dsdx = double_step(dsdx);
                        } else {
                            dtdy = double_step(dtdy);
                        }
                        g[2].words.w1 = ((u32)(u16)dsdx << 16) | (u16)dtdy;
                    }
                    g += 2;
                }
                break;
            case 0xF6: // G_FILLRECT
                g->words.w0 = fix_coords(g->words.w0, eye, hud);
                g->words.w1 = fix_coords(g->words.w1, eye, hud);
                break;
            case 0xED: // G_SETSCISSOR
                // Scissors can also be set for the 3D scene, so only squeeze them into the eye's half horizontally.
                g->words.w0 = fix_scissor(g->words.w0, eye);
                g->words.w1 = fix_scissor(g->words.w1, eye);
                break;
            default:
                break;
        }
    }
}

// World position to the space the game draws in: the head's view space, taken back through what the eye projections
// apply on top of the game's view (except the eye offset, which each eye's projection has too).
static void to_view(const f32 world[3], f32 out[3]) {
    f32 d[3], head[3];
    s32 r;
    d[0] = world[0] - sEye[0];
    d[1] = world[1] - sEye[1];
    d[2] = world[2] - sEye[2];
    for (r = 0; r < 3; r++) {
        head[r] = dot3(sHead[r], d) + sViewResidual[r];
    }
    for (r = 0; r < 3; r++) {
        out[r] = sViewRotation[0][r] * head[0] + sViewRotation[1][r] * head[1] + sViewRotation[2][r] * head[2];
    }
}

static void set_vertex(Vtx* v, const f32 pos[3], u8 alpha) {
    v->v.ob[0] = (s16)clampf(pos[0], -32767.0f, 32767.0f);
    v->v.ob[1] = (s16)clampf(pos[1], -32767.0f, 32767.0f);
    v->v.ob[2] = (s16)clampf(pos[2], -32767.0f, 32767.0f);
    v->v.flag = 0;
    v->v.tc[0] = 0;
    v->v.tc[1] = 0;
    v->v.cn[0] = 255;
    v->v.cn[1] = 40;
    v->v.cn[2] = 30;
    v->v.cn[3] = alpha;
}

// Draws a thin beam out of each armed hand, facing the camera, after the eye's scene.
void vr_draw_lasers(Mtx* projection, Mtx* identMtx) {
    u8* player = D_802049B0_1DFDB0;
    static float identity[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    Mtx* modelview;
    Vtx* verts;
    s32 h, k;

    if (!sActive || player[0x171] == 0) {
        return;
    }
    modelview = func_800281C8_35C8(sizeof(Mtx));
    verts = func_800281C8_35C8(sizeof(Vtx) * 8);
    if (modelview == NULL || verts == NULL) {
        return;
    }
    guMtxF2L(identity, modelview);

    gDPPipeSync(D_801A90F0_1844F0++);
    gEXMatrixGroupSkipAll(D_801A90F0_1844F0++, G_EX_ID_IGNORE, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    // set_eye_state has loaded the regular 3D microcode.
    gSPMatrix(D_801A90F0_1844F0++, projection, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(D_801A90F0_1844F0++, identMtx, G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
    gSPMatrix(D_801A90F0_1844F0++, modelview, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPTexture(D_801A90F0_1844F0++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(D_801A90F0_1844F0++, G_LIGHTING | G_FOG | G_TEXTURE_GEN | G_CULL_BOTH);
    gSPSetGeometryMode(D_801A90F0_1844F0++, G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER);
    gDPSetCycleType(D_801A90F0_1844F0++, G_CYC_1CYCLE);
    gDPSetRenderMode(D_801A90F0_1844F0++, G_RM_ZB_XLU_SURF, G_RM_ZB_XLU_SURF2);
    gDPSetCombineMode(D_801A90F0_1844F0++, G_CC_SHADE, G_CC_SHADE);

    for (h = 0; h < 2; h++) {
        f32 start[3], end[3], a[3], b[3], dir[3], mid[3], side[3];
        f32 p[3];

        if (!sFrame.handValid[h]) {
            continue;
        }
        for (k = 0; k < 3; k++) {
            start[k] = sHandPos[h][k] + sHand[h][2][k] * MUZZLE_OFFSET;
            end[k] = start[k] + sHand[h][2][k] * LASER_LENGTH;
        }
        to_view(start, a);
        to_view(end, b);
        for (k = 0; k < 3; k++) {
            dir[k] = b[k] - a[k];
            mid[k] = (a[k] + b[k]) * 0.5f;
        }
        // Widen the beam sideways, perpendicular to both its direction and the line of sight.
        cross3(dir, mid, side);
        if (!normalize3(side)) {
            continue;
        }
        for (k = 0; k < 3; k++) {
            side[k] *= LASER_WIDTH;
        }

        for (k = 0; k < 3; k++) {
            p[k] = a[k] - side[k];
        }
        set_vertex(&verts[h * 4 + 0], p, 170);
        for (k = 0; k < 3; k++) {
            p[k] = a[k] + side[k];
        }
        set_vertex(&verts[h * 4 + 1], p, 170);
        for (k = 0; k < 3; k++) {
            p[k] = b[k] - side[k];
        }
        set_vertex(&verts[h * 4 + 2], p, 0);
        for (k = 0; k < 3; k++) {
            p[k] = b[k] + side[k];
        }
        set_vertex(&verts[h * 4 + 3], p, 0);

        gSPVertex(D_801A90F0_1844F0++, &verts[h * 4], 4, 0);
        gSP2Triangles(D_801A90F0_1844F0++, 0, 1, 2, 0, 1, 3, 2, 0);
    }

    gDPPipeSync(D_801A90F0_1844F0++);
    gEXPopMatrixGroup(D_801A90F0_1844F0++, G_MTX_MODELVIEW);
}
