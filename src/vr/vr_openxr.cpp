// OpenXR support: session, a swap chain that RT64 renders into, controller input and poses.
//
// Rendering: the game draws each eye into one half of its usual 4:3 frame (patches/vr.c), so RT64 needs no stereo
// support of its own. RT64 presents that frame into an OpenXR swap chain image instead of the window, and each half is
// submitted as one eye's view of a projection layer. Menus and cutscenes, where the game isn't rendering stereo, are
// shown on a floating screen (a quad layer) instead.
//
// Timing: the game samples the head pose once per game frame and renders from it. That pose is tied to the RT64
// workload built from the frame's display lists, and the frame is submitted with it when presented, so the
// runtime's reprojection corrects for any head movement since then.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plume_vulkan.h"
#include "rhi/rt64_render_hooks.h"

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#ifdef __ANDROID__
#include <jni.h>
#include "SDL2/SDL_system.h"
#define XR_USE_PLATFORM_ANDROID
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "recomp_vr.h"
#include "recomp_ui.h"

namespace {
    // Size of the side by side frame relative to the headset's recommended size for one eye (in height).
    constexpr float RenderScale = 0.85f;
    // Half field of view used until the runtime reports the headset's.
    constexpr float DefaultHalfFov = 0.90f; // ~51.5 degrees.
    // Refresh rates to ask for, best first. The game runs at 30 fps, so a multiple of it shows every frame for the same
    // time; at 72 Hz frames alternate between two and three refreshes, which makes moving things judder.
    constexpr float PreferredRefreshRates[] = { 90.0f, 60.0f };
    // Floating screen used for menus and cutscenes.
    constexpr float ScreenDistance = 2.2f;
    constexpr float ScreenWidth = 2.6f;

    struct EyeView {
        XrPosef pose;
    };

    struct RenderPose {
        uint64_t workload_id;
        XrPosef head;
        bool stereo;
        float tan_half_x; // Field of view the eyes were rendered with.
        float tan_half_y;
    };

    struct State {
        XrInstance instance = XR_NULL_HANDLE;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        XrSession session = XR_NULL_HANDLE;
        XrSpace local_space = XR_NULL_HANDLE;
        XrSpace view_space = XR_NULL_HANDLE;
        XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
        std::atomic<bool> running = false;
        std::atomic<bool> focused = false;
        bool refresh_rate_ext = false;
        float refresh_rate = 72.0f;
        uint32_t recommended_width = 1440;
        uint32_t recommended_height = 1584;

        PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions = nullptr;
        PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions = nullptr;
        PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device = nullptr;
        PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements = nullptr;
        PFN_xrGetDisplayRefreshRateFB get_display_refresh_rate = nullptr;
        PFN_xrEnumerateDisplayRefreshRatesFB enumerate_display_refresh_rates = nullptr;
        PFN_xrRequestDisplayRefreshRateFB request_display_refresh_rate = nullptr;

        // Field of view: the headset's (the widest of both eyes, made symmetric), scaled by the user's setting.
        std::atomic<int> fov_percent = 100;
        float headset_tan_half_x = 0.0f;
        float headset_tan_half_y = 0.0f;
        // A menu of the port (not the game's) is open: the game is shown on the floating screen under it.
        std::atomic<bool> ui_open = false;

        // Frame pacing statistics, from the present thread.
        std::chrono::steady_clock::time_point last_present;
        std::chrono::steady_clock::time_point stats_start;
        int stats_frames = 0;
        int stats_slow = 0;
        float stats_max_ms = 0.0f;
        float stats_wait_ms = 0.0f;
        std::atomic<int> stats_late_poses = 0;

        // Input.
        XrActionSet action_set = XR_NULL_HANDLE;
        XrAction aim_pose = XR_NULL_HANDLE;
        XrAction trigger = XR_NULL_HANDLE;
        XrAction squeeze = XR_NULL_HANDLE;
        XrAction thumbstick = XR_NULL_HANDLE;
        XrAction thumbstick_click = XR_NULL_HANDLE;
        XrAction button_lower = XR_NULL_HANDLE; // A on the right controller, X on the left.
        XrAction button_upper = XR_NULL_HANDLE; // B on the right controller, Y on the left.
        XrAction menu = XR_NULL_HANDLE;
        XrAction haptic = XR_NULL_HANDLE;
        XrPath hand_paths[2] = {};
        XrSpace hand_spaces[2] = {};

        // Latest frame timing, from the present thread.
        std::atomic<XrTime> predicted_display_time = 0;
        std::atomic<XrDuration> predicted_display_period = 0;
        std::atomic<float> ipd = 0.064f;

        // Poses handed to the game, waiting for the workload they're rendered in.
        std::mutex pose_mutex;
        XrPosef pending_head = { {0, 0, 0, 1}, {0, 0, 0} };
        float pending_tan_half_x = 1.0f;
        float pending_tan_half_y = 1.0f;
        bool pending_stereo = false;
        // Frames the game has built (their display lists are on the way to RT64), with the pose each was built from.
        std::deque<RenderPose> built_frames;
        RenderPose last_built = { 0, { {0, 0, 0, 1}, {0, 0, 0} }, false, 1.0f, 1.0f };
        std::deque<RenderPose> render_poses;
        RenderPose presenting = { 0, { {0, 0, 0, 1}, {0, 0, 0} }, false, 1.0f, 1.0f };

        // Controller state for the game, from the main thread.
        std::mutex input_mutex;
        vr::Pose hands[2] = {};
        float turn_axis = 0.0f;
        bool trigger_held[2] = {};
        SDL_Joystick* joystick = nullptr;
        int joystick_index = -1;

        std::mutex event_mutex;
    };

    State state;

    bool xr_check(XrResult result, const char* what) {
        if (XR_FAILED(result)) {
            char text[XR_MAX_RESULT_STRING_SIZE] = {};
            if (state.instance != XR_NULL_HANDLE) {
                xrResultToString(state.instance, result, text);
            }
            fprintf(stderr, "[VR] %s failed: %d %s\n", what, (int)result, text);
            return false;
        }
        return true;
    }

    std::vector<std::string> split_extensions(const std::string& list) {
        std::vector<std::string> result;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find(' ', start);
            if (end == std::string::npos) {
                end = list.size();
            }
            if (end > start) {
                result.emplace_back(list.substr(start, end - start));
            }
            start = end + 1;
        }
        return result;
    }

    XrPath to_path(const char* path) {
        XrPath result = XR_NULL_PATH;
        xrStringToPath(state.instance, path, &result);
        return result;
    }

    // Quaternion helpers (x, y, z, w).
    XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
        // v' = v + 2w(q x v) + 2(q x (q x v))
        XrVector3f u = { q.x, q.y, q.z };
        XrVector3f t = { 2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z), 2.0f * (u.x * v.y - u.y * v.x) };
        return {
            v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
            v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x),
        };
    }

    bool create_actions() {
        XrActionSetCreateInfo set_info{ XR_TYPE_ACTION_SET_CREATE_INFO };
        strcpy(set_info.actionSetName, "gameplay");
        strcpy(set_info.localizedActionSetName, "Gameplay");
        if (!xr_check(xrCreateActionSet(state.instance, &set_info, &state.action_set), "xrCreateActionSet")) {
            return false;
        }

        state.hand_paths[0] = to_path("/user/hand/left");
        state.hand_paths[1] = to_path("/user/hand/right");

        auto make_action = [](XrAction* out, XrActionType type, const char* name, const char* localized) {
            XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
            info.actionType = type;
            strcpy(info.actionName, name);
            strcpy(info.localizedActionName, localized);
            info.countSubactionPaths = 2;
            info.subactionPaths = state.hand_paths;
            return xr_check(xrCreateAction(state.action_set, &info, out), name);
        };

        bool ok = true;
        ok &= make_action(&state.aim_pose, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose");
        ok &= make_action(&state.trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger");
        ok &= make_action(&state.squeeze, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip");
        ok &= make_action(&state.thumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick");
        ok &= make_action(&state.thumbstick_click, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick Click");
        ok &= make_action(&state.button_lower, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_lower", "A / X");
        ok &= make_action(&state.button_upper, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_upper", "B / Y");
        ok &= make_action(&state.menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu");
        ok &= make_action(&state.haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration");
        if (!ok) {
            return false;
        }

        // Touch controllers (Quest 2, 3, Pro and compatible).
        {
            std::vector<XrActionSuggestedBinding> bindings = {
                { state.aim_pose, to_path("/user/hand/left/input/aim/pose") },
                { state.aim_pose, to_path("/user/hand/right/input/aim/pose") },
                { state.trigger, to_path("/user/hand/left/input/trigger/value") },
                { state.trigger, to_path("/user/hand/right/input/trigger/value") },
                { state.squeeze, to_path("/user/hand/left/input/squeeze/value") },
                { state.squeeze, to_path("/user/hand/right/input/squeeze/value") },
                { state.thumbstick, to_path("/user/hand/left/input/thumbstick") },
                { state.thumbstick, to_path("/user/hand/right/input/thumbstick") },
                { state.thumbstick_click, to_path("/user/hand/left/input/thumbstick/click") },
                { state.thumbstick_click, to_path("/user/hand/right/input/thumbstick/click") },
                { state.button_lower, to_path("/user/hand/left/input/x/click") },
                { state.button_lower, to_path("/user/hand/right/input/a/click") },
                { state.button_upper, to_path("/user/hand/left/input/y/click") },
                { state.button_upper, to_path("/user/hand/right/input/b/click") },
                { state.menu, to_path("/user/hand/left/input/menu/click") },
                { state.haptic, to_path("/user/hand/left/output/haptic") },
                { state.haptic, to_path("/user/hand/right/output/haptic") },
            };
            XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
            suggested.interactionProfile = to_path("/interaction_profiles/oculus/touch_controller");
            suggested.suggestedBindings = bindings.data();
            suggested.countSuggestedBindings = (uint32_t)bindings.size();
            xr_check(xrSuggestInteractionProfileBindings(state.instance, &suggested), "touch bindings");
        }

        // Generic fallback with a pose, select and menu per hand.
        {
            std::vector<XrActionSuggestedBinding> bindings = {
                { state.aim_pose, to_path("/user/hand/left/input/aim/pose") },
                { state.aim_pose, to_path("/user/hand/right/input/aim/pose") },
                { state.trigger, to_path("/user/hand/left/input/select/click") },
                { state.trigger, to_path("/user/hand/right/input/select/click") },
                { state.menu, to_path("/user/hand/left/input/menu/click") },
                { state.haptic, to_path("/user/hand/left/output/haptic") },
                { state.haptic, to_path("/user/hand/right/output/haptic") },
            };
            XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
            suggested.interactionProfile = to_path("/interaction_profiles/khr/simple_controller");
            suggested.suggestedBindings = bindings.data();
            suggested.countSuggestedBindings = (uint32_t)bindings.size();
            xr_check(xrSuggestInteractionProfileBindings(state.instance, &suggested), "simple bindings");
        }

        return true;
    }

    bool attach_actions() {
        for (int hand = 0; hand < 2; hand++) {
            XrActionSpaceCreateInfo space_info{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            space_info.action = state.aim_pose;
            space_info.subactionPath = state.hand_paths[hand];
            space_info.poseInActionSpace.orientation.w = 1.0f;
            if (!xr_check(xrCreateActionSpace(state.session, &space_info, &state.hand_spaces[hand]), "xrCreateActionSpace")) {
                return false;
            }
        }

        XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attach.countActionSets = 1;
        attach.actionSets = &state.action_set;
        return xr_check(xrAttachSessionActionSets(state.session, &attach), "xrAttachSessionActionSets");
    }

    // Virtual game controller fed by the VR controllers, so both the game and the menus see them as a regular pad
    // through SDL. Buttons follow SDL's game controller order.
    void create_virtual_controller() {
        SDL_VirtualJoystickDesc desc;
        SDL_zero(desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.name = "VR Controllers";
        desc.vendor_id = 0x2833; // Oculus.
        desc.product_id = 0x0001;
        state.joystick_index = SDL_JoystickAttachVirtualEx(&desc);
        if (state.joystick_index < 0) {
            fprintf(stderr, "[VR] Failed to create the virtual controller: %s\n", SDL_GetError());
            return;
        }
        state.joystick = SDL_JoystickOpen(state.joystick_index);
    }

    float get_float(XrAction action, int hand) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = action;
        info.subactionPath = state.hand_paths[hand];
        XrActionStateFloat value{ XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_FAILED(xrGetActionStateFloat(state.session, &info, &value)) || !value.isActive) {
            return 0.0f;
        }
        return value.currentState;
    }

    bool get_bool(XrAction action, int hand) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = action;
        info.subactionPath = state.hand_paths[hand];
        XrActionStateBoolean value{ XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_FAILED(xrGetActionStateBoolean(state.session, &info, &value)) || !value.isActive) {
            return false;
        }
        return value.currentState;
    }

    XrVector2f get_vector(XrAction action, int hand) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = action;
        info.subactionPath = state.hand_paths[hand];
        XrActionStateVector2f value{ XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_FAILED(xrGetActionStateVector2f(state.session, &info, &value)) || !value.isActive) {
            return { 0.0f, 0.0f };
        }
        return value.currentState;
    }

    Sint16 to_axis(float value) {
        return (Sint16)std::clamp(value * 32767.0f, -32768.0f, 32767.0f);
    }

    void update_controllers() {
        if (!state.running || !state.focused) {
            if (state.joystick != nullptr) {
                for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++) {
                    SDL_JoystickSetVirtualButton(state.joystick, button, 0);
                }
                for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++) {
                    SDL_JoystickSetVirtualAxis(state.joystick, axis, 0);
                }
            }
            return;
        }

        XrActiveActionSet active{ state.action_set, XR_NULL_PATH };
        XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_FAILED(xrSyncActions(state.session, &sync))) {
            return;
        }

        constexpr int Left = 0;
        constexpr int Right = 1;
        XrVector2f left_stick = get_vector(state.thumbstick, Left);
        XrVector2f right_stick = get_vector(state.thumbstick, Right);

        const bool left_trigger = get_float(state.trigger, Left) > 0.5f;
        const bool right_trigger = get_float(state.trigger, Right) > 0.5f;
        {
            std::lock_guard lock(state.input_mutex);
            state.turn_axis = right_stick.x;
            state.trigger_held[Left] = left_trigger;
            state.trigger_held[Right] = right_trigger;
        }

        if (state.joystick == nullptr) {
            return;
        }

        // Left trigger: buster (B, bound to the pad's West/X). Right trigger: special weapon (C-Left, North/Y).
        // A: jump (South/A). B and X: interact (C-Down, East/B). Y: map (C-Up, bound to the right stick click).
        // Grips: lock-on / strafe (Z and R, the shoulders). Menu: pause. Left stick: movement, and its click is L.
        // Right stick click: the port's settings menu.
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_X, left_trigger);
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_Y, right_trigger);
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_A, get_bool(state.button_lower, Right));
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_B, get_bool(state.button_upper, Right) || get_bool(state.button_lower, Left));
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_RIGHTSTICK, get_bool(state.button_upper, Left));
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, get_float(state.squeeze, Left) > 0.5f);
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, get_float(state.squeeze, Right) > 0.5f);
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_START, get_bool(state.menu, Left));
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_LEFTSTICK, get_bool(state.thumbstick_click, Left));
        // Right stick click: the port's menu (settings), which is bound to Back.
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_BACK, get_bool(state.thumbstick_click, Right));
        SDL_JoystickSetVirtualAxis(state.joystick, SDL_CONTROLLER_AXIS_LEFTX, to_axis(left_stick.x));
        SDL_JoystickSetVirtualAxis(state.joystick, SDL_CONTROLLER_AXIS_LEFTY, to_axis(-left_stick.y));

        // The right stick navigates menus when the game isn't rendering stereo; in game it turns (see get_frame_input).
        bool in_game;
        {
            std::lock_guard lock(state.pose_mutex);
            in_game = state.pending_stereo;
        }
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_DPAD_UP, !in_game && right_stick.y > 0.6f);
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, !in_game && right_stick.y < -0.6f);
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_DPAD_LEFT, !in_game && right_stick.x < -0.6f);
        SDL_JoystickSetVirtualButton(state.joystick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, !in_game && right_stick.x > 0.6f);
    }

    vr::Pose locate(XrSpace space, XrTime time) {
        vr::Pose pose = {};
        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xrLocateSpace(space, state.local_space, time, &location))) {
            constexpr XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            pose.valid = (location.locationFlags & needed) == needed;
            pose.orientation[0] = location.pose.orientation.x;
            pose.orientation[1] = location.pose.orientation.y;
            pose.orientation[2] = location.pose.orientation.z;
            pose.orientation[3] = location.pose.orientation.w;
            pose.position[0] = location.pose.position.x;
            pose.position[1] = location.pose.position.y;
            pose.position[2] = location.pose.position.z;
        }
        if (!pose.valid) {
            pose.orientation[3] = 1.0f;
        }
        return pose;
    }

    void handle_session_state(XrSessionState new_state) {
        state.session_state = new_state;
        switch (new_state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (xr_check(xrBeginSession(state.session, &begin), "xrBeginSession")) {
                state.running = true;
                printf("[VR] Session running\n");
            }
            break;
        }
        case XR_SESSION_STATE_FOCUSED:
            state.focused = true;
            break;
        case XR_SESSION_STATE_VISIBLE:
            state.focused = false;
            break;
        case XR_SESSION_STATE_STOPPING:
            state.focused = false;
            state.running = false;
            xr_check(xrEndSession(state.session), "xrEndSession");
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING: {
            state.focused = false;
            state.running = false;
            SDL_Event quit;
            SDL_zero(quit);
            quit.type = SDL_QUIT;
            SDL_PushEvent(&quit);
            break;
        }
        default:
            break;
        }
    }

    void poll_events() {
        std::lock_guard lock(state.event_mutex);
        XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(state.instance, &event) == XR_SUCCESS) {
            switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                handle_session_state(changed.state);
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                handle_session_state(XR_SESSION_STATE_LOSS_PENDING);
                break;
            default:
                break;
            }
            event = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    // RT64 hooks: tie the pose the game rendered with to the workload built from its display lists.
    void on_workload_created(uint64_t workload_id) {
        std::lock_guard lock(state.pose_mutex);
        // RT64 builds workloads on its own thread, and by then the game may be reading the poses for its next frame,
        // so each workload takes the pose saved when its frame was built. A workload without a frame of its own (if
        // any) reuses the last one.
        if (!state.built_frames.empty()) {
            state.last_built = state.built_frames.front();
            state.built_frames.pop_front();
        }
        RenderPose pose = state.last_built;
        pose.workload_id = workload_id;
        if (std::memcmp(&pose.head, &state.pending_head, sizeof(XrPosef)) != 0) {
            state.stats_late_poses++;
        }
        state.render_poses.push_back(pose);
        while (state.render_poses.size() > 16) {
            state.render_poses.pop_front();
        }
    }

    void on_workload_present(uint64_t workload_id) {
        std::lock_guard lock(state.pose_mutex);
        while (!state.render_poses.empty() && state.render_poses.front().workload_id <= workload_id) {
            state.presenting = state.render_poses.front();
            state.render_poses.pop_front();
        }
    }

    // The swap chain RT64 presents into: images of an OpenXR swap chain holding both eyes side by side.
    struct XrSwapChain : plume::RenderSwapChain {
        plume::VulkanDevice* device = nullptr;
        plume::VulkanCommandQueue* queue = nullptr;
        XrSwapchain swapchain = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<plume::VulkanTexture> textures;
        std::unique_ptr<plume::RenderCommandList> transition_list;
        std::unique_ptr<plume::RenderCommandFence> transition_fence;
        bool frame_begun = false;
        bool image_acquired = false;
        VkFormat view_format = VK_FORMAT_B8G8R8A8_UNORM;
        XrFrameState frame_state{ XR_TYPE_FRAME_STATE };

        ~XrSwapChain() override {
            if (swapchain != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain);
            }
        }

        bool create(plume::VulkanDevice* vk_device, plume::VulkanCommandQueue* vk_queue) {
            device = vk_device;
            queue = vk_queue;

            uint32_t format_count = 0;
            xrEnumerateSwapchainFormats(state.session, 0, &format_count, nullptr);
            std::vector<int64_t> formats(format_count);
            xrEnumerateSwapchainFormats(state.session, format_count, &format_count, formats.data());

            // RT64 renders gamma encoded colors into B8G8R8A8_UNORM targets. Use an sRGB swap chain viewed as UNORM so the
            // runtime reads those colors as sRGB, or a plain UNORM one if that's all there is. RGBA formats are a last
            // resort: RT64's pipelines are built for BGRA targets, which most drivers accept for RGBA views.
            struct FormatChoice { int64_t format; VkFormat view; bool mutable_format; };
            static const FormatChoice choices[] = {
                { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, true },
                { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, false },
                { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, true },
                { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, false },
            };
            const FormatChoice* choice = nullptr;
            for (const FormatChoice& c : choices) {
                if (std::find(formats.begin(), formats.end(), c.format) != formats.end()) {
                    choice = &c;
                    break;
                }
            }
            if (choice == nullptr) {
                fprintf(stderr, "[VR] The runtime doesn't offer an 8 bit RGBA or BGRA swap chain format.\n");
                return false;
            }
            if (choice->view == VK_FORMAT_R8G8B8A8_UNORM) {
                fprintf(stderr, "[VR] Warning: no BGRA swap chain format, using RGBA.\n");
            }
            const int64_t chosen = choice->format;
            const bool mutable_format = choice->mutable_format;
            view_format = choice->view;

            // Both eyes side by side in a 4:3 image, so each half is 2:3.
            height = (uint32_t)(state.recommended_height * RenderScale) & ~1u;
            width = (height * 4 / 3) & ~1u;

            XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                (mutable_format ? XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT : 0);
            info.format = chosen;
            info.sampleCount = 1;
            info.width = width;
            info.height = height;
            info.faceCount = 1;
            info.arraySize = 1;
            info.mipCount = 1;
            if (!xr_check(xrCreateSwapchain(state.session, &info, &swapchain), "xrCreateSwapchain")) {
                return false;
            }

            uint32_t image_count = 0;
            xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr);
            std::vector<XrSwapchainImageVulkanKHR> images(image_count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
            if (!xr_check(xrEnumerateSwapchainImages(swapchain, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "xrEnumerateSwapchainImages")) {
                return false;
            }

            textures.resize(image_count);
            for (uint32_t i = 0; i < image_count; i++) {
                textures[i] = plume::VulkanTexture(device, images[i].image);
                textures[i].desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
                textures[i].desc.format = plume::RenderFormat::B8G8R8A8_UNORM;
                textures[i].desc.width = width;
                textures[i].desc.height = height;
                textures[i].desc.depth = 1;
                textures[i].desc.mipLevels = 1;
                textures[i].desc.arraySize = 1;
                textures[i].desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
                textures[i].fillSubresourceRange();
                textures[i].createImageView(view_format);
            }

            transition_list = queue->createCommandList();
            transition_fence = device->createCommandFence();
            printf("[VR] Swap chain %ux%u, %u images, format %lld\n", width, height, image_count, (long long)chosen);
            return true;
        }

        bool begin_frame() {
            XrFrameWaitInfo wait_info{ XR_TYPE_FRAME_WAIT_INFO };
            frame_state = { XR_TYPE_FRAME_STATE };
            auto wait_start = std::chrono::steady_clock::now();
            if (!xr_check(xrWaitFrame(state.session, &wait_info, &frame_state), "xrWaitFrame")) {
                return false;
            }
            state.stats_wait_ms += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
            state.predicted_display_time = frame_state.predictedDisplayTime;
            state.predicted_display_period = frame_state.predictedDisplayPeriod;

            XrFrameBeginInfo begin_info{ XR_TYPE_FRAME_BEGIN_INFO };
            XrResult result;
            {
                std::lock_guard lock(*queue->queue->mutex);
                result = xrBeginFrame(state.session, &begin_info);
            }
            if (!xr_check(result, "xrBeginFrame")) {
                return false;
            }
            frame_begun = true;
            return true;
        }

        void end_frame(bool with_image) {
            RenderPose pose;
            {
                std::lock_guard lock(state.pose_mutex);
                pose = state.presenting;
            }

            XrCompositionLayerProjectionView views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
            XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
            const XrCompositionLayerBaseHeader* layers[1] = {};
            uint32_t layer_count = 0;

            if (with_image && frame_state.shouldRender) {
                if (pose.stereo) {
                    // The eyes sit half the IPD to each side of the head pose the game rendered from.
                    const float half_ipd = state.ipd * 0.5f;
                    const float half_fov_x = std::atan(pose.tan_half_x);
                    const float half_fov_y = std::atan(pose.tan_half_y);
                    for (int eye = 0; eye < 2; eye++) {
                        XrVector3f offset = rotate(pose.head.orientation, { eye == 0 ? -half_ipd : half_ipd, 0.0f, 0.0f });
                        views[eye].pose.orientation = pose.head.orientation;
                        views[eye].pose.position = { pose.head.position.x + offset.x, pose.head.position.y + offset.y, pose.head.position.z + offset.z };
                        views[eye].fov = { -half_fov_x, half_fov_x, half_fov_y, -half_fov_y };
                        views[eye].subImage.swapchain = swapchain;
                        views[eye].subImage.imageRect.offset = { eye == 0 ? 0 : (int32_t)(width / 2), 0 };
                        views[eye].subImage.imageRect.extent = { (int32_t)(width / 2), (int32_t)height };
                    }
                    projection.space = state.local_space;
                    projection.viewCount = 2;
                    projection.views = views;
                    layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);
                }
                else {
                    quad.space = state.local_space;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain = swapchain;
                    quad.subImage.imageRect.offset = { 0, 0 };
                    quad.subImage.imageRect.extent = { (int32_t)width, (int32_t)height };
                    quad.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
                    quad.pose.position = { 0.0f, 0.0f, -ScreenDistance };
                    quad.size = { ScreenWidth, ScreenWidth * 3.0f / 4.0f };
                    layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
                }
            }

            XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
            end_info.displayTime = frame_state.predictedDisplayTime;
            end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end_info.layerCount = layer_count;
            end_info.layers = layers;
            {
                std::lock_guard lock(*queue->queue->mutex);
                xr_check(xrEndFrame(state.session, &end_info), "xrEndFrame");
            }
            frame_begun = false;
        }

        bool present(uint32_t texture_index, plume::RenderCommandSemaphore** wait_semaphores, uint32_t wait_semaphore_count) override {
            if (!image_acquired) {
                return false;
            }

            // RT64 leaves the image ready to present. OpenXR wants it back as a color attachment.
            transition_list->begin();
            transition_list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(&textures[texture_index], plume::RenderTextureLayout::COLOR_WRITE));
            transition_list->end();
            const plume::RenderCommandList* list = transition_list.get();
            queue->executeCommandLists(&list, 1, wait_semaphores, wait_semaphore_count, nullptr, 0, transition_fence.get());
            queue->waitForCommandFence(transition_fence.get());

            XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            {
                std::lock_guard lock(*queue->queue->mutex);
                xr_check(xrReleaseSwapchainImage(swapchain, &release), "xrReleaseSwapchainImage");
            }
            image_acquired = false;

            end_frame(true);
            log_pacing();
            return true;
        }

        // Every few seconds, logs how evenly frames are presented: the game should present one every 33 ms.
        void log_pacing() {
            auto now = std::chrono::steady_clock::now();
            if (state.stats_frames == 0 && state.stats_start.time_since_epoch().count() == 0) {
                state.stats_start = now;
            }
            else {
                float ms = std::chrono::duration<float, std::milli>(now - state.last_present).count();
                state.stats_max_ms = std::max(state.stats_max_ms, ms);
                if (ms > 40.0f) {
                    state.stats_slow++;
                }
            }
            state.last_present = now;
            state.stats_frames++;
            float elapsed = std::chrono::duration<float>(now - state.stats_start).count();
            if (elapsed >= 5.0f) {
                printf("[VR] %.1f fps, %d frames over 40 ms, longest %.1f ms, waiting for the headset %.1f ms per frame, %.0f Hz, %d frames built before the pose moved on\n",
                    state.stats_frames / elapsed, state.stats_slow, state.stats_max_ms, state.stats_wait_ms / state.stats_frames, state.refresh_rate, state.stats_late_poses.exchange(0));
                state.stats_start = now;
                state.stats_frames = 0;
                state.stats_slow = 0;
                state.stats_max_ms = 0.0f;
                state.stats_wait_ms = 0.0f;
            }
        }

        void wait() override {}

        bool resize() override {
            return state.running;
        }

        bool needsResize() const override {
            return false;
        }

        void setVsyncEnabled(bool) override {}

        bool isVsyncEnabled() const override {
            return true;
        }

        uint32_t getWidth() const override {
            return width;
        }

        uint32_t getHeight() const override {
            return height;
        }

        plume::RenderTexture* getTexture(uint32_t texture_index) override {
            return &textures[texture_index];
        }

        uint32_t getTextureCount() const override {
            return (uint32_t)textures.size();
        }

        bool acquireTexture(plume::RenderCommandSemaphore* signal_semaphore, uint32_t* texture_index) override {
            if (!state.running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return false;
            }

            if (image_acquired) {
                // The previous image was never presented; hand it back so the swap chain doesn't run out.
                XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                std::lock_guard lock(*queue->queue->mutex);
                xrReleaseSwapchainImage(swapchain, &release);
                image_acquired = false;
            }

            if (frame_begun) {
                // A frame was begun without presenting an image; finish it empty.
                end_frame(false);
            }

            if (!begin_frame()) {
                return false;
            }

            XrSwapchainImageAcquireInfo acquire{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (!xr_check(xrAcquireSwapchainImage(swapchain, &acquire, texture_index), "xrAcquireSwapchainImage")) {
                end_frame(false);
                return false;
            }

            XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wait.timeout = XR_INFINITE_DURATION;
            if (!xr_check(xrWaitSwapchainImage(swapchain, &wait), "xrWaitSwapchainImage")) {
                end_frame(false);
                return false;
            }
            image_acquired = true;

            // RT64 waits on this semaphore before drawing into the image, which is ready now.
            VkSemaphore semaphore = static_cast<plume::VulkanCommandSemaphore*>(signal_semaphore)->vk;
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &semaphore;
            {
                std::lock_guard lock(*queue->queue->mutex);
                vkQueueSubmit(queue->queue->vk, 1, &submit, VK_NULL_HANDLE);
            }
            return true;
        }

        plume::RenderWindow getWindow() const override {
            return {};
        }

        bool isEmpty() const override {
            return !state.running;
        }

        uint32_t getRefreshRate() const override {
            return (uint32_t)std::lround(state.refresh_rate);
        }
    };
}

#ifdef __ANDROID__
// The activity as a global JNI reference: the loader and the runtime keep it and may use it from other threads.
static jobject android_activity() {
    static jobject activity = nullptr;
    if (activity == nullptr) {
        JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
        jobject local = (jobject)SDL_AndroidGetActivity();
        activity = env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
    }
    return activity;
}
#endif

bool vr::init() {
#ifdef __ANDROID__
    {
        PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
        if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initialize_loader)) || initialize_loader == nullptr) {
            fprintf(stderr, "[VR] xrInitializeLoaderKHR not found.\n");
            return false;
        }
        JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
        JavaVM* vm = nullptr;
        env->GetJavaVM(&vm);
        XrLoaderInitInfoAndroidKHR loader_info{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        loader_info.applicationVM = vm;
        loader_info.applicationContext = android_activity();
        if (!xr_check(initialize_loader((const XrLoaderInitInfoBaseHeaderKHR*)&loader_info), "xrInitializeLoaderKHR")) {
            return false;
        }
    }
#endif

    uint32_t extension_count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr))) {
        fprintf(stderr, "[VR] No OpenXR runtime available.\n");
        return false;
    }
    std::vector<XrExtensionProperties> available(extension_count, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, available.data());
    auto has_extension = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const XrExtensionProperties& p) { return strcmp(p.extensionName, name) == 0; });
    };

    if (!has_extension(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
        fprintf(stderr, "[VR] The OpenXR runtime doesn't support Vulkan.\n");
        return false;
    }

    std::vector<const char*> extensions = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
#ifdef __ANDROID__
    extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
    state.refresh_rate_ext = has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (state.refresh_rate_ext) {
        extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }

    XrInstanceCreateInfo create_info{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(create_info.applicationInfo.applicationName, "Mega Man 64 Recompiled");
    create_info.applicationInfo.applicationVersion = 1;
    strcpy(create_info.applicationInfo.engineName, "RT64");
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create_info.enabledExtensionCount = (uint32_t)extensions.size();
    create_info.enabledExtensionNames = extensions.data();
#ifdef __ANDROID__
    XrInstanceCreateInfoAndroidKHR android_info{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    {
        JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
        env->GetJavaVM((JavaVM**)&android_info.applicationVM);
        android_info.applicationActivity = android_activity();
        create_info.next = &android_info;
    }
#endif
    if (!xr_check(xrCreateInstance(&create_info, &state.instance), "xrCreateInstance")) {
        return false;
    }

    XrSystemGetInfo system_info{ XR_TYPE_SYSTEM_GET_INFO };
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xr_check(xrGetSystem(state.instance, &system_info, &state.system), "xrGetSystem")) {
        xrDestroyInstance(state.instance);
        state.instance = XR_NULL_HANDLE;
        return false;
    }

    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(state.instance, state.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> config_views(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(state.instance, state.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, config_views.data());
    if (view_count > 0) {
        state.recommended_width = config_views[0].recommendedImageRectWidth;
        state.recommended_height = config_views[0].recommendedImageRectHeight;
    }

    xrGetInstanceProcAddr(state.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&state.get_instance_extensions);
    xrGetInstanceProcAddr(state.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&state.get_device_extensions);
    xrGetInstanceProcAddr(state.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&state.get_graphics_device);
    xrGetInstanceProcAddr(state.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&state.get_graphics_requirements);
    if (state.refresh_rate_ext) {
        xrGetInstanceProcAddr(state.instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&state.get_display_refresh_rate);
        xrGetInstanceProcAddr(state.instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&state.enumerate_display_refresh_rates);
        xrGetInstanceProcAddr(state.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&state.request_display_refresh_rate);
    }

    XrGraphicsRequirementsVulkanKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    xr_check(state.get_graphics_requirements(state.instance, state.system, &requirements), "xrGetVulkanGraphicsRequirementsKHR");

    // Vulkan extensions the runtime needs, added to the ones plume creates the instance and device with.
    auto get_list = [](PFN_xrGetVulkanInstanceExtensionsKHR fn) {
        uint32_t size = 0;
        fn(state.instance, state.system, 0, &size, nullptr);
        std::string list(size, '\0');
        fn(state.instance, state.system, size, &size, list.data());
        list.resize(strlen(list.c_str()));
        return split_extensions(list);
    };
    plume::VulkanHooks.instanceExtensions = get_list(state.get_instance_extensions);
    plume::VulkanHooks.deviceExtensions = get_list(state.get_device_extensions);
    plume::VulkanHooks.pickPhysicalDevice = [](VkInstance instance) {
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        xr_check(state.get_graphics_device(state.instance, state.system, instance, &physical_device), "xrGetVulkanGraphicsDeviceKHR");
        return physical_device;
    };
    for (const std::string& e : plume::VulkanHooks.instanceExtensions) {
        printf("[VR] Vulkan instance extension: %s\n", e.c_str());
    }
    for (const std::string& e : plume::VulkanHooks.deviceExtensions) {
        printf("[VR] Vulkan device extension: %s\n", e.c_str());
    }

    if (!create_actions()) {
        return false;
    }

    RT64::SetRenderHookWorkload(on_workload_created, on_workload_present);
    printf("[VR] OpenXR ready, recommended eye size %ux%u\n", state.recommended_width, state.recommended_height);
    return true;
}

void vr::shutdown() {
    if (state.session != XR_NULL_HANDLE) {
        xrDestroySession(state.session);
        state.session = XR_NULL_HANDLE;
    }
    if (state.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(state.instance);
        state.instance = XR_NULL_HANDLE;
    }
}

bool vr::enabled() {
    return state.instance != XR_NULL_HANDLE;
}

bool vr::session_running() {
    return state.running;
}

bool vr::debug_enabled() {
    static const bool debug = !enabled() && getenv("MM64_VR_DEBUG") != nullptr;
    return debug && !enabled();
}

bool vr::gameplay_active() {
    if (!state.running && !debug_enabled()) {
        return false;
    }
    std::lock_guard lock(state.pose_mutex);
    return state.pending_stereo;
}

std::unique_ptr<plume::RenderSwapChain> vr::create_swap_chain(plume::RenderInterface* render_interface, plume::RenderDevice* device, plume::RenderCommandQueue* command_queue) {
    if (!enabled()) {
        return nullptr;
    }

    auto* vk_interface = static_cast<plume::VulkanInterface*>(render_interface);
    auto* vk_device = static_cast<plume::VulkanDevice*>(device);
    auto* vk_queue = static_cast<plume::VulkanCommandQueue*>(command_queue);

    XrGraphicsBindingVulkanKHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    binding.instance = vk_interface->instance;
    binding.physicalDevice = vk_device->physicalDevice;
    binding.device = vk_device->vk;
    binding.queueFamilyIndex = vk_queue->familyIndex;
    binding.queueIndex = vk_queue->queueIndex;

    XrSessionCreateInfo session_info{ XR_TYPE_SESSION_CREATE_INFO };
    session_info.next = &binding;
    session_info.systemId = state.system;
    if (!xr_check(xrCreateSession(state.instance, &session_info, &state.session), "xrCreateSession")) {
        return nullptr;
    }

    XrReferenceSpaceCreateInfo space_info{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    xr_check(xrCreateReferenceSpace(state.session, &space_info, &state.local_space), "local space");
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xr_check(xrCreateReferenceSpace(state.session, &space_info, &state.view_space), "view space");

    if (!attach_actions()) {
        return nullptr;
    }

    if (state.enumerate_display_refresh_rates != nullptr && state.request_display_refresh_rate != nullptr) {
        uint32_t count = 0;
        state.enumerate_display_refresh_rates(state.session, 0, &count, nullptr);
        std::vector<float> rates(count);
        state.enumerate_display_refresh_rates(state.session, count, &count, rates.data());
        for (float preferred : PreferredRefreshRates) {
            auto found = std::find_if(rates.begin(), rates.end(), [&](float r) { return std::fabs(r - preferred) < 0.5f; });
            if (found != rates.end()) {
                if (xr_check(state.request_display_refresh_rate(state.session, *found), "xrRequestDisplayRefreshRateFB")) {
                    printf("[VR] Refresh rate set to %.0f Hz\n", *found);
                    break;
                }
            }
        }
    }

    if (state.get_display_refresh_rate != nullptr) {
        float rate = 0.0f;
        if (XR_SUCCEEDED(state.get_display_refresh_rate(state.session, &rate)) && rate > 0.0f) {
            state.refresh_rate = rate;
        }
    }

    auto swap_chain = std::make_unique<XrSwapChain>();
    if (!swap_chain->create(vk_device, vk_queue)) {
        return nullptr;
    }
    return swap_chain;
}

void vr::update() {
    if (!enabled()) {
        return;
    }
    if (state.session != XR_NULL_HANDLE) {
        // The virtual pad is created here, on the thread that handles SDL events, once SDL's joysticks are up.
        if (state.joystick_index < 0 && SDL_WasInit(SDL_INIT_JOYSTICK)) {
            create_virtual_controller();
        }
        poll_events();
        update_controllers();
        state.ui_open = recompui::is_context_capturing_input();
    }
}

void vr::set_fov_percent(int percent) {
    state.fov_percent = std::clamp(percent, 40, 100);
}

int vr::fov_percent() {
    return state.fov_percent;
}

vr::FrameInput vr::get_frame_input() {
    FrameInput input = {};
    input.fov_tan_half_x = std::tan(DefaultHalfFov);
    input.fov_tan_half_y = std::tan(DefaultHalfFov);
    input.head.orientation[3] = 1.0f;

    if (!state.running || state.local_space == XR_NULL_HANDLE) {
        return input;
    }

    // The frame the game builds now is shown about a frame after the last predicted display time.
    XrTime time = state.predicted_display_time + state.predicted_display_period;
    if (state.predicted_display_time == 0) {
        return input;
    }

    XrViewLocateInfo locate_info{ XR_TYPE_VIEW_LOCATE_INFO };
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = time;
    locate_info.space = state.local_space;
    XrViewState view_state{ XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t view_count = 0;
    if (XR_SUCCEEDED(xrLocateViews(state.session, &locate_info, &view_state, 2, &view_count, views)) && view_count == 2) {
        const XrVector3f& a = views[0].pose.position;
        const XrVector3f& b = views[1].pose.position;
        float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        if (ipd > 0.04f && ipd < 0.09f) {
            state.ipd = ipd;
        }

        // The widest field of view of both eyes, made symmetric so both eyes are rendered the same way.
        float tan_x = 0.0f, tan_y = 0.0f;
        for (const XrView& view : views) {
            tan_x = std::max({ tan_x, std::tan(-view.fov.angleLeft), std::tan(view.fov.angleRight) });
            tan_y = std::max({ tan_y, std::tan(view.fov.angleUp), std::tan(-view.fov.angleDown) });
        }
        if (tan_x > 0.3f && tan_x < 4.0f && tan_y > 0.3f && tan_y < 4.0f) {
            state.headset_tan_half_x = tan_x;
            state.headset_tan_half_y = tan_y;
        }
    }
    if (state.headset_tan_half_x > 0.0f) {
        const float scale = state.fov_percent / 100.0f;
        input.fov_tan_half_x = std::tan(std::atan(state.headset_tan_half_x) * scale);
        input.fov_tan_half_y = std::tan(std::atan(state.headset_tan_half_y) * scale);
    }

    input.head = locate(state.view_space, time);
    input.hands[0] = locate(state.hand_spaces[0], time);
    input.hands[1] = locate(state.hand_spaces[1], time);
    {
        std::lock_guard lock(state.input_mutex);
        input.turn_axis = state.turn_axis;
        input.trigger[0] = state.trigger_held[0];
        input.trigger[1] = state.trigger_held[1];
    }
    input.ipd = state.ipd;
    input.stereo = state.focused && input.head.valid && !state.ui_open;
    return input;
}

// Called by the game (through recomp_vr_get_frame) with the pose it renders the current frame from.
void vr_set_render_pose(const vr::Pose& head, float tan_half_x, float tan_half_y) {
    std::lock_guard lock(state.pose_mutex);
    state.pending_tan_half_x = tan_half_x;
    state.pending_tan_half_y = tan_half_y;
    state.pending_head.orientation = { head.orientation[0], head.orientation[1], head.orientation[2], head.orientation[3] };
    state.pending_head.position = { head.position[0], head.position[1], head.position[2] };
}

// Called by the game's main draw (through recomp_vr_set_stereo): whether the frame being built renders both eyes.
void vr_set_render_stereo(bool stereo) {
    std::lock_guard lock(state.pose_mutex);
    state.pending_stereo = stereo;
    state.built_frames.push_back({ 0, state.pending_head, stereo, state.pending_tan_half_x, state.pending_tan_half_y });
    while (state.built_frames.size() > 3) {
        state.built_frames.pop_front();
    }
}

void vr::haptic_pulse(int hand, float amplitude, float seconds) {
    if (!state.running || !state.focused) {
        return;
    }
    XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
    vibration.amplitude = amplitude;
    vibration.duration = (XrDuration)(seconds * 1e9);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
    info.action = state.haptic;
    info.subactionPath = state.hand_paths[hand];
    xrApplyHapticFeedback(state.session, &info, reinterpret_cast<XrHapticBaseHeader*>(&vibration));
}
