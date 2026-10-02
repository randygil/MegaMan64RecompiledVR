#ifndef __ZELDA_CONFIG_H__
#define __ZELDA_CONFIG_H__

#include <filesystem>
#include <string_view>
#include "ultramodern/config.hpp"
#include "recomp_input.h"

namespace zelda64 {
    constexpr std::u8string_view program_id = u8"MegaMan64Recompiled";
    constexpr std::string_view program_name = "Mega Man 64: Recompiled";

    // TODO: Move loading configs to the runtime once we have a way to allow per-project customization.
    void load_config();
    void save_config();
    
    void reset_input_bindings();
    void reset_cont_input_bindings();
    void reset_kb_input_bindings();
    void reset_single_input_binding(recomp::InputDevice device, recomp::GameInput input);

    std::filesystem::path get_app_folder_path();
    
    bool get_debug_mode_enabled();
    bool get_path_tracing_enabled();
    void set_path_tracing_enabled(bool enabled);
    // Shadows and lighting with classic raster techniques (RT64's enhanced lighting).
    bool get_enhanced_lighting_enabled();
    void set_enhanced_lighting_enabled(bool enabled);
    // Quality of the enhanced lighting: 0 low, 1 medium, 2 high, 3 ultra.
    int get_lighting_quality();
    void set_lighting_quality(int quality);
    // Strength of the path tracer's visual effects: 0 off, 1 subtle, 2 full.
    int get_path_tracing_effects();
    void set_path_tracing_effects(int level);
    // Whether outdoor scenes use the path tracer's procedural sky instead of the game's.
    bool get_path_tracing_sky();
    void set_path_tracing_sky(bool enhanced);
    void set_debug_mode_enabled(bool enabled);
    
    enum class FilmGrainMode {
        On,
        Off,
        OptionCount
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(zelda64::FilmGrainMode, {
        {zelda64::FilmGrainMode::On, "On"},
        {zelda64::FilmGrainMode::Off, "Off"}
    });

    enum class TargetingMode {
        Switch,
        Hold,
        OptionCount
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(zelda64::TargetingMode, {
        {zelda64::TargetingMode::Switch, "Switch"},
        {zelda64::TargetingMode::Hold, "Hold"}
    });

    TargetingMode get_targeting_mode();
    void set_targeting_mode(TargetingMode mode);

    enum class AimInvertMode {
        On,
        Off,
        OptionCount
    };

    enum class RadioBoxMode {
        Expand,
        Original,
        OptionCount
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(zelda64::RadioBoxMode, {
        {zelda64::RadioBoxMode::Expand, "Expand"},
        {zelda64::RadioBoxMode::Original, "Original"}
    });

    NLOHMANN_JSON_SERIALIZE_ENUM(zelda64::AimInvertMode, {
        {zelda64::AimInvertMode::On, "On"},
        {zelda64::AimInvertMode::Off, "Off"},
    });

    RadioBoxMode get_radio_comm_box_mode();
    void set_radio_comm_box_mode(RadioBoxMode mode);

    AimInvertMode get_analog_camera_invert_mode();
    void set_analog_camera_invert_mode(AimInvertMode mode);

    enum class AnalogCamMode {
        On,
        Off,
		OptionCount
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(zelda64::AnalogCamMode, {
        {zelda64::AnalogCamMode::On, "On"},
        {zelda64::AnalogCamMode::Off, "Off"}
    });

    // Values are shared with patches/mouse_camera.h.
    enum class MouseCameraMode {
        Off,
        ThirdPerson,
        FirstPerson,
        OptionCount
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(zelda64::MouseCameraMode, {
        {zelda64::MouseCameraMode::Off, "Off"},
        {zelda64::MouseCameraMode::ThirdPerson, "ThirdPerson"},
        {zelda64::MouseCameraMode::FirstPerson, "FirstPerson"}
    });

    enum class FirstPersonStrafeMode {
        On,
        Off,
        OptionCount
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(zelda64::FirstPersonStrafeMode, {
        {zelda64::FirstPersonStrafeMode::On, "On"},
        {zelda64::FirstPersonStrafeMode::Off, "Off"}
    });

    FirstPersonStrafeMode get_first_person_strafe_mode();
    void set_first_person_strafe_mode(FirstPersonStrafeMode mode);

    // Horizontal field of view in degrees (at 4:3) used while the mouse camera is on.
    int get_mouse_camera_fov();
    void set_mouse_camera_fov(int fov);

    // VR builds: field of view each eye is rendered with, as a percentage of the headset's (40 to 100).
    int get_vr_fov();
    void set_vr_fov(int percent);

    MouseCameraMode get_mouse_camera_mode();
    void set_mouse_camera_mode(MouseCameraMode mode);
    // Swaps between first and third person. Does nothing while the mouse camera is off.
    void toggle_mouse_camera_perspective();

    FilmGrainMode get_film_grain_mode();
    void set_film_grain_mode(FilmGrainMode mode);

    AimInvertMode get_invert_y_axis_mode();
    void set_invert_y_axis_mode(AimInvertMode mode);

    void open_quit_game_prompt();
};

#endif
