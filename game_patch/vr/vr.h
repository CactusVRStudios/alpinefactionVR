// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace rf
{
    struct Matrix3;
    struct Vector3;
}

namespace afvr
{
    enum class TimingPhase
    {
        input_sync,
        begin_frame,
        eye_image_wait,
        eye_render,
        eye_release,
        hud_image_wait,
        hud_render,
        hud_release,
        menu_image_wait,
        menu_copy,
        menu_release,
        end_frame,
        count,
    };

    // Register -vr before Red Faction parses its command line.
    void register_command_line();

    // Intercept the local portal/world traversal for stereo rendering.
    void install_render_hook();

    // Register developer VR console commands after AlpineFaction has replaced
    // Red Faction's fixed-size command buffer.
    void register_console_commands();

    // Low-frequency end-to-end diagnostics used to identify whether XR pacing,
    // the RF frame loop, rendering work, or the desktop mirror limits VR.
    void timing_game_frame_begin();
    void timing_game_frame_end();
    void timing_note_xr_wait(double wait_ms, double return_interval_ms,
        double predicted_interval_ms, double runtime_target_hz);
    void timing_note_phase(TimingPhase phase, double duration_ms);
    void timing_note_xr_submission();
    void timing_note_desktop_present(double duration_ms);

    // Called after the original game initialization, when D3D11 is available.
    void after_game_init();

    // Poll local OpenXR runtime/session events once per game frame.
    void update();

    // Copy the completed native RF menu into a spatially anchored OpenXR quad.
    // Called by the existing menu renderer after it has drawn its cursor/widgets.
    void submit_menu_frame();

    // Release all local OpenXR state. Safe to call more than once.
    void shutdown();

    [[nodiscard]] bool is_requested();
    [[nodiscard]] bool is_initialized();
    [[nodiscard]] bool is_session_running();
    [[nodiscard]] bool is_menu_capture_active();
    [[nodiscard]] bool should_update_desktop_mirror();

    // Physical mouse input remains available in RF's main menu, but must not
    // affect gameplay or in-game menus while an OpenXR session is active.
    // VR controller menu input is injected separately and is not blocked.
    [[nodiscard]] bool should_block_physical_mouse_input();

    // Re-neutralize HMD yaw and tracking-space position on the next valid XR
    // view without modifying Red Faction's player/body orientation.
    void recenter_tracking();

    // One-shot VR render coverage diagnostics. Passes: 0 left, 1 right.
    // These count draw calls, never simulation updates.
    void begin_scene_render_pass(int pass);
    void end_scene_render_pass(int pass);
    void note_static_solid_draw();
    void note_movable_solid_draw();
    void note_standard_mesh_draw();
    void note_character_mesh_draw();

    // True only while the local first-person weapon is being submitted to an
    // OpenXR eye. The normal desktop viewmodel remains suppressed in VR mode.
    [[nodiscard]] bool is_rendering_weapon();

    // D3D11 FPGUN material filter. RF arms/hands share the animated VMesh but
    // use dedicated material chunks, allowing VR to keep the moving weapon.
    [[nodiscard]] bool should_render_fpgun_texture(int bitmap_handle);

    // Latest calibrated weapon muzzle position plus OpenXR aim orientation in
    // RF world space, used by local fire paths.
    [[nodiscard]] bool get_weapon_muzzle_pose(rf::Vector3& position, rf::Matrix3& orientation);

    // Final local projectile-creation pose. Rocket and rail projectiles use
    // their visually calibrated laser/muzzle emitter; other weapons retain
    // the established firing pose.
    [[nodiscard]] bool get_weapon_launch_pose(int weapon_type,
        rf::Vector3& position, rf::Matrix3& orientation);

    // Latest local right-controller grip position with the controller aim
    // orientation, used by first-person effects that must follow the hand but
    // are not emitted from the weapon muzzle.
    [[nodiscard]] bool get_right_controller_pose(rf::Vector3& position,
        rf::Matrix3& orientation);

    // Latest center-HMD pose in RF world space. This is cached from the XR view
    // submission for local launch paths that intentionally follow
    // the player's six-degree look direction rather than body/stick yaw.
    [[nodiscard]] bool get_head_pose(rf::Vector3& position, rf::Matrix3& orientation);
    [[nodiscard]] bool is_primary_trigger_active();
}
