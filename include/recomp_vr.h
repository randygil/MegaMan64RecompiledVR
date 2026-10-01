#ifndef __RECOMP_VR_H__
#define __RECOMP_VR_H__

#include <cstdint>
#include <memory>

namespace plume {
    struct RenderInterface;
    struct RenderDevice;
    struct RenderCommandQueue;
    struct RenderSwapChain;
}

// OpenXR support. The game renders both eyes side by side into a single 4:3 frame (see patches/vr.c), which RT64
// presents into an OpenXR swap chain instead of the window. Each half of that image is submitted as one eye's view.
namespace vr {
    // Tries to start OpenXR. Must be called before the renderer is created. Returns false when there's no runtime or
    // headset, in which case the game runs normally on the desktop.
    bool init();
    void shutdown();

    // True once init() succeeded, even if the session isn't running yet.
    bool enabled();
    // Development aid: with MM64_VR_DEBUG=1 and no headset, the game renders VR on the desktop window with a
    // simulated head and hands.
    bool debug_enabled();
    // True while the session is running and the headset shows the game.
    bool session_running();
    // True while the game renders VR first person gameplay (as opposed to menus and cutscenes on the floating screen).
    bool gameplay_active();

    // Swap chain factory for RT64 (RT64::Application::Core::createExternalSwapChain).
    std::unique_ptr<plume::RenderSwapChain> create_swap_chain(plume::RenderInterface* render_interface, plume::RenderDevice* device, plume::RenderCommandQueue* command_queue);

    // Processes OpenXR events and updates the controller state. Called regularly from the main (event) thread.
    void update();

    // Pose of a tracked device in the tracking space, using OpenXR axes (+X right, +Y up, -Z forward) and meters.
    struct Pose {
        float orientation[4]; // x, y, z, w quaternion.
        float position[3];
        bool valid;
    };

    // Snapshot for the game, taken once per game frame.
    struct FrameInput {
        Pose head;
        Pose hands[2]; // 0 left, 1 right. The aim pose, pointing forward from the controller.
        float ipd; // Distance between the eyes in meters.
        float fov_tan_half_x; // Tangent of half the horizontal field of view of each eye's image.
        float fov_tan_half_y; // Tangent of half the vertical field of view.
        float turn_axis; // Right thumbstick X, used for turning.
        bool trigger[2]; // Triggers held (left fires the buster, right the special weapon).
        bool stereo; // The session is focused and the game should render both eyes.
    };

    // Returns the latest poses and marks the head pose as the one the next frame is rendered from.
    FrameInput get_frame_input();

    // Field of view the game renders each eye with, as a percentage of the headset's (40 to 100).
    void set_fov_percent(int percent);
    int fov_percent();

    // Vibrates a controller (0 left, 1 right).
    void haptic_pulse(int hand, float amplitude, float seconds);
}

#endif
