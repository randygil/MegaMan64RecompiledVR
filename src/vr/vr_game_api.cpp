// Host functions that give the game patches (patches/vr.c) the VR poses.
//
// Poses are converted to the game's axes, which are OpenXR's rotated half a turn around X: +X right, +Y down and +Z
// forward. Orientations are given as three rows, the device's right, down and forward vectors, which is also the
// layout of a PSX style view rotation.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SDL.h"

#include "librecomp/helpers.hpp"
#include "recomp_vr.h"

void vr_set_render_pose(const vr::Pose& head, float tan_half_x, float tan_half_y);

namespace {
    // Layout shared with VrFrame in patches/vr.h. Every field is 32 bits so it can be written directly into rdram.
    struct GameVrFrame {
        int32_t active;
        float ipd;
        float tan_half_x;
        float tan_half_y;
        float turn;
        int32_t hand_valid[2];
        int32_t trigger[2];
        float head[3][3];
        float head_pos[3];
        float hands[2][3][3];
        float hands_pos[2][3];
        int32_t debug; // 1: simulated VR (MM64_VR_DEBUG), 2: also force combat mode (MM64_VR_DEBUG=combat).
    };

    void to_game(const float in[3], float out[3]) {
        out[0] = in[0];
        out[1] = -in[1];
        out[2] = -in[2];
    }

    // Rotates v by the quaternion q (x, y, z, w).
    void rotate(const float q[4], const float v[3], float out[3]) {
        float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
        float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
        float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
        out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
        out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
        out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
    }

    void basis(const vr::Pose& pose, float rows[3][3], float pos[3]) {
        static const float right[3] = { 1.0f, 0.0f, 0.0f };
        static const float down[3] = { 0.0f, -1.0f, 0.0f };
        static const float forward[3] = { 0.0f, 0.0f, -1.0f };
        float v[3];
        rotate(pose.orientation, right, v);
        to_game(v, rows[0]);
        rotate(pose.orientation, down, v);
        to_game(v, rows[1]);
        rotate(pose.orientation, forward, v);
        to_game(v, rows[2]);
        to_game(pose.position, pos);
    }
}

// Quaternion for a yaw (around +Y) then a pitch (around +X), OpenXR axes.
static void yaw_pitch_quaternion(float yaw, float pitch, float out[4]) {
    float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
    float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    // q = q_yaw * q_pitch
    out[0] = cy * sp;
    out[1] = sy * cp;
    out[2] = -sy * sp;
    out[3] = cy * cp;
}

// Simulated head and hands for MM64_VR_DEBUG: the head sways slowly and the hands are held forward, pointing
// slightly inwards, so both show up on screen. Numpad 4 and 6 turn the head, 8 and 2 tilt it, and 5 stops the sway.
static int debug_knob = getenv("MM64_VR_KNOB") != nullptr ? atoi(getenv("MM64_VR_KNOB")) : 0; // MM64_VR_KNOB, numpad 7 and 9: a value for whatever the patches are testing.

static vr::FrameInput debug_frame_input() {
    static const auto start = std::chrono::steady_clock::now();
    static auto last = start;
    static float look_yaw = 0.0f;
    static float look_pitch = 0.0f;
    static bool sway = true;
    static bool sway_key = false;
    static bool knob_keys[2] = { false, false };
    const auto now = std::chrono::steady_clock::now();
    float t = std::chrono::duration<float>(now - start).count();
    const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
    last = now;

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys != nullptr) {
        look_yaw += ((keys[SDL_SCANCODE_KP_4] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_KP_6] ? 1.0f : 0.0f)) * 1.5f * dt;
        look_pitch += ((keys[SDL_SCANCODE_KP_8] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_KP_2] ? 1.0f : 0.0f)) * 1.0f * dt;
        look_pitch = std::clamp(look_pitch, -1.4f, 1.4f);
        if (keys[SDL_SCANCODE_KP_5] && !sway_key) {
            sway = !sway;
        }
        sway_key = keys[SDL_SCANCODE_KP_5] != 0;
        const SDL_Scancode knob_codes[2] = { SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_9 };
        for (int k = 0; k < 2; k++) {
            if (keys[knob_codes[k]] && !knob_keys[k]) {
                debug_knob = std::max(0, debug_knob + (k == 0 ? -1 : 1));
                fprintf(stderr, "[VR] Debug knob %d\n", debug_knob);
            }
            knob_keys[k] = keys[knob_codes[k]] != 0;
        }
    }
    if (!sway) {
        t = 0.0f;
    }
    vr::FrameInput input = {};
    input.ipd = 0.064f;
    input.fov_tan_half_y = std::tan(0.90f);
    input.fov_tan_half_x = input.fov_tan_half_y * (2.0f / 3.0f);
    input.stereo = true;

    const float head_yaw = look_yaw + 0.35f * std::sin(t * 0.4f);
    const float head_pitch = look_pitch - 0.1f + 0.12f * std::sin(t * 0.27f);
    yaw_pitch_quaternion(head_yaw, head_pitch, input.head.orientation);
    input.head.position[1] = 1.6f;
    input.head.valid = true;

    for (int hand = 0; hand < 2; hand++) {
        const float side = hand == 0 ? -1.0f : 1.0f;
        // Hand offset in head space: to the side, below and in front, then rotated with the head.
        float local[3] = { side * 0.16f, -0.28f, -0.38f };
        float world[3];
        rotate(input.head.orientation, local, world);
        input.hands[hand].position[0] = input.head.position[0] + world[0];
        input.hands[hand].position[1] = input.head.position[1] + world[1];
        input.hands[hand].position[2] = input.head.position[2] + world[2];
        yaw_pitch_quaternion(head_yaw + side * 0.12f + 0.25f * std::sin(t * 0.9f + side), head_pitch + 0.1f * std::sin(t * 0.7f), input.hands[hand].orientation);
        input.hands[hand].valid = true;
    }
    return input;
}

extern "C" void recomp_vr_get_frame(uint8_t* rdram, recomp_context* ctx) {
    GameVrFrame* out = _arg<0, GameVrFrame*>(rdram, ctx);
    const bool debug = vr::debug_enabled();
    vr::FrameInput input = debug ? debug_frame_input() : vr::get_frame_input();
    GameVrFrame frame = {};

    frame.active = ((vr::session_running() || debug) && input.stereo) ? 1 : 0;
    frame.ipd = input.ipd;
    frame.tan_half_x = input.fov_tan_half_x;
    frame.tan_half_y = input.fov_tan_half_y;
    frame.turn = input.turn_axis;
    basis(input.head, frame.head, frame.head_pos);
    for (int hand = 0; hand < 2; hand++) {
        frame.hand_valid[hand] = input.hands[hand].valid ? 1 : 0;
        frame.trigger[hand] = input.trigger[hand] ? 1 : 0;
        basis(input.hands[hand], frame.hands[hand], frame.hands_pos[hand]);
    }

    if (debug) {
        // After a debug warp Mega Man may not be marked active (the area's entry never ran), which leaves the camera
        // to the game.
        if (getenv("MM64_VR_FORCE_ACTIVE") != nullptr) {
            MEM_B(0, (gpr)(int32_t)0x802049B6) = 1;
        }
        const char* mode = getenv("MM64_VR_DEBUG");
        frame.debug = (mode != nullptr && strcmp(mode, "combat") == 0) ? 2 : 1;
        frame.debug |= debug_knob << 8;
    }

    // The frame being built now is rendered from this head pose.
    vr_set_render_pose(input.head, input.fov_tan_half_x, input.fov_tan_half_y);

    // Copy word by word, rdram keeps 32 bit words in host order.
    const uint32_t* src = reinterpret_cast<const uint32_t*>(&frame);
    uint32_t* dst = reinterpret_cast<uint32_t*>(out);
    for (size_t i = 0; i < sizeof(frame) / sizeof(uint32_t); i++) {
        dst[i] = src[i];
    }
}

void vr_set_render_stereo(bool stereo);

// Called by the main draw each frame: whether it rendered both eyes (gameplay) or a regular frame (menus, cutscenes).
extern "C" void recomp_vr_set_stereo(uint8_t* rdram, recomp_context* ctx) {
    vr_set_render_stereo(_arg<0, int32_t>(rdram, ctx) != 0);
}

namespace zelda64::renderer {
    void report_sky_background();
}

// Called when the game draws its 2D sky, which VR leaves out: the renderer still lights the scene as an outdoor one
// and draws its procedural sky, which stays fixed in the world.
extern "C" void recomp_vr_report_sky(uint8_t* rdram, recomp_context* ctx) {
    zelda64::renderer::report_sky_background();
}

extern "C" void recomp_vr_haptic(uint8_t* rdram, recomp_context* ctx) {
    int32_t hand = _arg<0, int32_t>(rdram, ctx);
    int32_t strength = _arg<1, int32_t>(rdram, ctx); // 0 to 100.
    if (hand == 0 || hand == 1) {
        vr::haptic_pulse(hand, strength / 100.0f, 0.05f);
    }
}

// VR debugging: MM64_VR_WARP=<area>[,<entrance>[,<load>]] replaces the area of the game's <load>th area load (1 by
// default, the save's) with that one (4 is Apple Market), so areas deep into the game can be tested in desktop debug
// mode.
extern "C" void recomp_vr_debug_warp(uint8_t* rdram, recomp_context* ctx) {
    static int loads = 0;
    const char* warp = getenv("MM64_VR_WARP");
    if (warp == nullptr) {
        return;
    }
    int area = atoi(warp);
    const char* comma = strchr(warp, ',');
    int entrance = comma != nullptr ? atoi(comma + 1) : 0;
    const char* comma2 = comma != nullptr ? strchr(comma + 1, ',') : nullptr;
    int load = comma2 != nullptr ? atoi(comma2 + 1) : 1;
    if (++loads != load) {
        return;
    }
    printf("[VR] Debug warp to area %d, entrance %d\n", area, entrance);
    MEM_B(0, (gpr)(int32_t)0x801BC438) = (int8_t)area;
    MEM_B(0, (gpr)(int32_t)0x801BC439) = (int8_t)entrance;
}
