// SPDX-License-Identifier: MPL-2.0
#include "vr.h"

#include <cmath>
#include <array>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "../rf/gr/gr.h"
#include "../rf/bmpman.h"
#include "../main/main.h"
#include "../rf/multi.h"
#include "../rf/os/os.h"
#include "../rf/entity.h"
#include "../rf/player/player.h"
#include "../rf/os/frametime.h"
#include "../rf/gameseq.h"
#include "../rf/input.h"
#include "../rf/collide.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"
#include "../rf/hud.h"
#include "../rf/level.h"
#include "../hud/hud_internal.h"
#include "../input/control_input_filter.h"
#include "../input/input.h"
#include "../misc/alpine_settings.h"
#include "../os/console.h"
#include "vr_render_bridge.h"

#ifdef AF_ENABLE_OPENXR
#include "openxr_context.h"
#endif

namespace afvr
{
    namespace
    {
        bool g_requested = false;
#ifdef AF_ENABLE_OPENXR
        // Explicit lifetime avoids invoking OpenXR from a C++ static destructor
        // while the injected DLL is being detached under the loader lock.
        OpenXrContext* g_openxr = nullptr;
#endif

        struct SceneRenderStats
        {
            uint32_t portal_traversals = 0;
            uint32_t static_solids = 0;
            uint32_t movable_solids = 0;
            uint32_t standard_meshes = 0;
            uint32_t character_meshes = 0;
        };

        std::array<SceneRenderStats, 3> g_scene_render_stats{};
        int g_scene_render_pass = -1;
        bool g_scene_render_stats_collecting = false;
        bool g_scene_render_stats_logged = false;
        uint32_t g_scene_render_diagnostic_frames = 0;
        float g_hmd_relative_yaw = 0.0f;
        float g_hmd_relative_forward_y = 0.0f;
        rf::Matrix3 g_hmd_relative_orientation{
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f},
        };
        bool g_hmd_relative_orientation_valid = false;
        struct VrResolvedPlayerPose
        {
            bool valid = false;
            rf::Vector3 body_world_position{};
            rf::Matrix3 body_world_orientation{};
            rf::Matrix3 visual_body_world_orientation{};
            rf::Vector3 head_local_offset{};
            float head_local_yaw = 0.0f;
            rf::Vector3 head_world_position{};
            rf::Matrix3 head_world_orientation{};
            rf::Vector3 tracking_reference_position{};
            float tracking_reference_yaw = 0.0f;
        };
        VrResolvedPlayerPose g_resolved_player_pose{};
        struct VrTurnDiagnostic
        {
            bool pending = false;
            bool logged = false;
            int requested_frame = -1;
            rf::Vector3 body_position_before{};
            rf::Vector3 local_head_offset_before{};
            float body_yaw_before = 0.0f;
            rf::Vector3 head_world_position_before{};
        };
        VrTurnDiagnostic g_turn_diagnostic{};
        float g_visual_body_yaw_offset = 0.0f;
        int g_body_follow_frame = -1;
        bool g_body_follow_logged = false;
        bool g_resolved_pose_logged = false;
        bool g_mounted_aim_active = false;
        bool g_mounted_aim_follows_host = false;
        int g_mounted_aim_host_handle = -1;
        rf::Matrix3 g_mounted_neutral_orientation{};
        rf::Matrix3 g_mounted_neutral_relative_to_host{};
        rf::Matrix3 g_mounted_native_aim_orientation{};
        bool g_mounted_native_aim_valid = false;
        bool g_mounted_aim_logged = false;
        bool g_vehicle_controls_logged = false;
        bool g_local_attachment_active = false;
        int g_local_attachment_entity_handle = -1;
        bool g_vehicle_exit_horizon_stabilization = false;
        int g_vehicle_exit_level_frames = 0;
        bool g_movement_input_logged = false;
        bool g_ladder_input_logged = false;
        bool g_snap_turn_logged = false;
        bool g_smooth_turn_logged = false;
        bool g_snap_turn_latched = false;
        bool g_rendering_weapon = false;
        bool g_rendering_fpgun_body = false;
        bool g_weapon_pose_valid = false;
        bool g_weapon_aim_pose_valid = false;
        bool g_two_hand_support_available = false;
        bool g_two_hand_weapon_active = false;
        int g_two_hand_weapon_id = -1;
        bool g_two_hand_calibration_active = false;
        int g_two_hand_calibration_weapon_id = -1;
        int g_current_weapon_id = -1;
        bool g_debug_weapon_at_hmd = false;
        int g_weapon_render_eye = -1;
        rf::Vector3 g_weapon_render_position{};
        rf::Matrix3 g_weapon_render_orientation{};
        rf::Vector3 g_weapon_fire_position{};
        rf::Matrix3 g_weapon_fire_orientation{};
        bool g_laser_emitter_pose_valid = false;
        rf::Vector3 g_laser_emitter_position{};
        rf::Matrix3 g_laser_emitter_orientation{};
        bool g_right_controller_pose_valid = false;
        rf::Vector3 g_right_controller_position{};
        rf::Matrix3 g_right_controller_orientation{};
        bool g_controller_grip_world_valid = false;
        bool g_controller_aim_world_valid = false;
        bool g_left_controller_grip_world_valid = false;
        bool g_left_controller_aim_world_valid = false;
        rf::Vector3 g_controller_grip_world_position{};
        rf::Matrix3 g_controller_grip_world_orientation{};
        rf::Vector3 g_controller_aim_world_position{};
        rf::Matrix3 g_controller_aim_world_orientation{};
        rf::Vector3 g_left_controller_grip_world_position{};
        rf::Matrix3 g_left_controller_grip_world_orientation{};
        rf::Vector3 g_left_controller_aim_world_position{};
        rf::Matrix3 g_left_controller_aim_world_orientation{};
        bool g_head_pose_valid = false;
        rf::Vector3 g_head_position{};
        rf::Matrix3 g_head_orientation{};
        bool g_weapon_render_requested_logged = false;
        bool g_player_render_reached_logged = false;
        bool g_weapon_mesh_render_reached_logged = false;
        bool g_final_weapon_transform_logged = false;
        std::array<bool, 2> g_weapon_eye_draw_logged{};
        std::array<bool, 64> g_two_hand_weapon_logged{};
        bool g_trigger_pressed = false;
        bool g_trigger_just_pressed = false;
        bool g_previous_trigger_pressed = false;
        bool g_trigger_action_logged = false;
        bool g_frame_limiter_bypassed = false;
        bool g_multiplayer_best_effort_logged = false;
        bool g_menu_capture_active = false;
        bool g_scope_capture_active = false;
        bool g_scope_view_base_valid = false;
        rf::Vector3 g_scope_base_eye_position{};
        rf::Matrix3 g_scope_player_view_base{};
        std::chrono::steady_clock::time_point g_steamvr_menu_chord_started{};
        bool g_steamvr_menu_chord_timing = false;
        rf::Vector3 g_roomscale_world_correction{};
        int g_roomscale_collision_frame = -1;
        bool g_roomscale_collision_logged = false;
        bool g_roomscale_reconciliation_logged = false;
        std::chrono::steady_clock::time_point g_next_desktop_mirror_update{};
        int g_desktop_mirror_decision_frame = -1;
        bool g_desktop_mirror_update_due = false;
        bool g_singleplayer_death_menu_active = false;
        bool g_menu_pointer_using_controller = true;
        bool g_hud_capture_active = false;
        int g_hud_rendered_frame = -1;
        bool g_menu_pointer_valid = false;
        int g_menu_pointer_x = 0;
        int g_menu_pointer_y = 0;
        bool g_previous_reload = false;
        bool g_previous_jump = false;
        bool g_previous_crouch = false;
        bool g_previous_holster = false;
        bool g_previous_flashlight = false;
        bool g_previous_menu_button = false;
        bool g_previous_left_grip = false;
        bool g_previous_primary_fire = false;
        bool g_previous_secondary_fire = false;
        bool g_fire_mode_secondary = false;
        bool g_fire_blocked_until_trigger_release = false;
        bool g_gameplay_input_blocked_until_release = false;
        bool g_reload_pressed = false;
        bool g_reload_just_pressed = false;
        bool g_shake_reload_just_pressed = false;
        bool g_previous_right_grip_y_valid = false;
        float g_previous_right_grip_y = 0.0f;
        float g_previous_right_grip_velocity = 0.0f;
        float g_shake_reload_cooldown = 0.0f;
        bool g_jump_pressed = false;
        bool g_jump_just_pressed = false;
        bool g_crouch_pressed = false;
        bool g_crouch_just_pressed = false;
        bool g_holster_pressed = false;
        bool g_holster_just_pressed = false;
        bool g_flashlight_pressed = false;
        bool g_flashlight_just_pressed = false;
        bool g_menu_button_pressed = false;
        bool g_menu_button_just_pressed = false;
        bool g_left_grip_pressed = false;
        bool g_left_grip_just_pressed = false;
        bool g_primary_fire_pressed = false;
        bool g_primary_fire_just_pressed = false;
        bool g_secondary_fire_pressed = false;
        bool g_secondary_fire_just_pressed = false;
        bool g_weapon_cycle_latched = false;
        bool g_previous_weapon_pulse = false;
        bool g_next_weapon_pulse = false;
        bool g_jump_semantic_logged = false;
        bool g_crouch_semantic_logged = false;
        bool g_holster_semantic_logged = false;
        bool g_flashlight_semantic_logged = false;
        bool g_menu_semantic_logged = false;
        bool g_laser_sight_enabled = false;
        bool g_previous_laser_toggle_pressed = false;
        int g_laser_trace_frame = -1;
        bool g_laser_trace_valid = false;
        bool g_laser_trace_hit = false;
        bool g_laser_coordinate_audit_logged = false;
        rf::Vector3 g_laser_trace_start{};
        rf::Vector3 g_laser_trace_end{};
        bool g_input_debug = false;
        rf::GameState g_last_menu_state = rf::GS_INIT;
        bool g_previous_forward = false;
        bool g_previous_backward = false;
        bool g_previous_strafe_left = false;
        bool g_previous_strafe_right = false;
        float g_weapon_aim_yaw_correction_degrees = -1.5f;

        struct TimingDiagnostics
        {
            using Clock = std::chrono::steady_clock;

            Clock::time_point window_start{};
            Clock::time_point game_frame_start{};
            uint64_t game_frames = 0;
            uint64_t xr_waits = 0;
            uint64_t xr_submissions = 0;
            uint64_t desktop_presents = 0;
            double game_cpu_ms = 0.0;
            double rf_frametime_ms = 0.0;
            double xr_wait_ms = 0.0;
            double xr_wait_return_interval_ms = 0.0;
            double xr_predicted_interval_ms = 0.0;
            uint64_t xr_wait_return_intervals = 0;
            uint64_t xr_predicted_intervals = 0;
            static constexpr size_t phase_count =
                static_cast<size_t>(TimingPhase::count);
            std::array<double, phase_count> phase_ms{};
            std::array<uint64_t, phase_count> phase_samples{};
            double desktop_present_ms = 0.0;
            double runtime_target_hz = 0.0;

            void reset(Clock::time_point now)
            {
                window_start = now;
                game_frame_start = {};
                game_frames = 0;
                xr_waits = 0;
                xr_submissions = 0;
                desktop_presents = 0;
                game_cpu_ms = 0.0;
                rf_frametime_ms = 0.0;
                xr_wait_ms = 0.0;
                xr_wait_return_interval_ms = 0.0;
                xr_predicted_interval_ms = 0.0;
                xr_wait_return_intervals = 0;
                xr_predicted_intervals = 0;
                phase_ms.fill(0.0);
                phase_samples.fill(0);
                desktop_present_ms = 0.0;
            }
        };

        TimingDiagnostics g_timing;

        void maybe_log_timing(TimingDiagnostics::Clock::time_point now)
        {
#ifdef AF_ENABLE_OPENXR
            if (!g_openxr || !g_openxr->is_session_running()) {
                g_timing.reset(now);
                return;
            }
#else
            g_timing.reset(now);
            return;
#endif
            if (g_timing.window_start == TimingDiagnostics::Clock::time_point{}) {
                g_timing.reset(now);
                return;
            }

            const double elapsed_seconds = std::chrono::duration<double>(
                now - g_timing.window_start).count();
            constexpr double report_interval_seconds = 5.0;
            if (elapsed_seconds < report_interval_seconds) {
                return;
            }

            const double game_fps = g_timing.game_frames / elapsed_seconds;
            const double submission_fps = g_timing.xr_submissions / elapsed_seconds;
            const double game_interval_ms = g_timing.game_frames > 0
                ? elapsed_seconds * 1000.0 / g_timing.game_frames : 0.0;
            const double game_cpu_ms = g_timing.game_frames > 0
                ? g_timing.game_cpu_ms / g_timing.game_frames : 0.0;
            const double rf_frametime_ms = g_timing.game_frames > 0
                ? g_timing.rf_frametime_ms / g_timing.game_frames : 0.0;
            const double xr_wait_ms = g_timing.xr_waits > 0
                ? g_timing.xr_wait_ms / g_timing.xr_waits : 0.0;
            const double xr_return_interval_ms = g_timing.xr_wait_return_intervals > 0
                ? g_timing.xr_wait_return_interval_ms /
                    g_timing.xr_wait_return_intervals : 0.0;
            const double xr_predicted_interval_ms = g_timing.xr_predicted_intervals > 0
                ? g_timing.xr_predicted_interval_ms /
                    g_timing.xr_predicted_intervals : 0.0;
            const double present_ms = g_timing.desktop_presents > 0
                ? g_timing.desktop_present_ms / g_timing.desktop_presents : 0.0;
            const auto phase_average = [](TimingPhase phase) {
                const size_t index = static_cast<size_t>(phase);
                return g_timing.phase_samples[index] > 0
                    ? g_timing.phase_ms[index] / g_timing.phase_samples[index]
                    : 0.0;
            };

            xlog::info(
                "[AFVR] Timing: runtime target {:.2f} Hz; XR submissions {:.2f} fps; RF game frames {:.2f} fps",
                g_timing.runtime_target_hz, submission_fps, game_fps);
            xlog::info(
                "[AFVR] Timing: xrWaitFrame avg {:.3f} ms; wait-return interval {:.3f} ms; predicted-time interval {:.3f} ms; game interval {:.3f} ms",
                xr_wait_ms, xr_return_interval_ms, xr_predicted_interval_ms,
                game_interval_ms);
            xlog::info(
                "[AFVR] Timing: CPU frame {:.3f} ms; RF frametime {:.3f} ms; frametime_min {:.3f} ms; configured maxfps {}; desktop Present {:.3f} ms",
                game_cpu_ms, rf_frametime_ms,
                static_cast<double>(rf::frametime_min) * 1000.0,
                g_alpine_game_config.max_fps, present_ms);
            xlog::info(
                "[AFVR] XR phases: input {:.3f} ms; begin {:.3f} ms; end {:.3f} ms",
                phase_average(TimingPhase::input_sync),
                phase_average(TimingPhase::begin_frame),
                phase_average(TimingPhase::end_frame));
            xlog::info(
                "[AFVR] XR world per eye: image wait {:.3f} ms; render {:.3f} ms; release {:.3f} ms; HUD wait/render/release {:.3f}/{:.3f}/{:.3f} ms",
                phase_average(TimingPhase::eye_image_wait),
                phase_average(TimingPhase::eye_render),
                phase_average(TimingPhase::eye_release),
                phase_average(TimingPhase::hud_image_wait),
                phase_average(TimingPhase::hud_render),
                phase_average(TimingPhase::hud_release));
            xlog::info(
                "[AFVR] XR menu: image wait {:.3f} ms; copy {:.3f} ms; release {:.3f} ms",
                phase_average(TimingPhase::menu_image_wait),
                phase_average(TimingPhase::menu_copy),
                phase_average(TimingPhase::menu_release));
            g_timing.reset(now);
        }

        using PortalRenderArgument = void*;

        struct StereoRoomRenderState
        {
            rf::GRoom* room = nullptr;
            rf::GRoom* render_parent = nullptr;
            bool rendered_normal = false;
            bool rendered_alpha = false;
        };

        struct StereoPortalRoomState
        {
            rf::GRoom* room = nullptr;
            int render_depth = 0xFFFF;
        };

        constexpr int max_portal_render_rooms = 1024;
        auto& portal_render_room_count = addr_as_ref<int>(0x009BB57C);
        auto& portal_render_rooms =
            addr_as_ref<rf::GRoom*[max_portal_render_rooms]>(0x009A8548);
        std::vector<StereoPortalRoomState> g_left_eye_portal_rooms;

        void make_room_object_clip_conservative(rf::GRoom* room)
        {
            if (!room) {
                return;
            }
            room->clip_wnd = {
                0.0f,
                0.0f,
                static_cast<float>(rf::gr::screen_width()),
                static_cast<float>(rf::gr::screen_height()),
            };
        }

        void reconcile_stereo_portal_rooms()
        {
            if (g_scene_render_pass == 0) {
                g_left_eye_portal_rooms.clear();
                g_left_eye_portal_rooms.reserve(portal_render_room_count);
                for (int i = 0; i < portal_render_room_count; ++i) {
                    rf::GRoom* room = portal_render_rooms[i];
                    if (!room) {
                        continue;
                    }
                    g_left_eye_portal_rooms.push_back({room, room->render_depth});
                    // Once traversal has accepted a room, do not let its narrow
                    // monocular portal rectangle reject meshes in only one eye.
                    make_room_object_clip_conservative(room);
                }
                return;
            }

            if (g_scene_render_pass != 1) {
                return;
            }

            int rooms_added_from_left = 0;
            for (const StereoPortalRoomState& left_state : g_left_eye_portal_rooms) {
                bool already_visible = false;
                for (int i = 0; i < portal_render_room_count; ++i) {
                    if (portal_render_rooms[i] == left_state.room) {
                        already_visible = true;
                        break;
                    }
                }
                if (!already_visible &&
                    portal_render_room_count < max_portal_render_rooms) {
                    portal_render_rooms[portal_render_room_count++] = left_state.room;
                    left_state.room->visited_this_search = true;
                    left_state.room->render_depth = left_state.render_depth;
                    ++rooms_added_from_left;
                }
            }

            for (int i = 0; i < portal_render_room_count; ++i) {
                make_room_object_clip_conservative(portal_render_rooms[i]);
            }

            static bool merge_logged = false;
            if (rooms_added_from_left > 0 && !merge_logged) {
                merge_logged = true;
                xlog::info(
                    "[AFVR] Stereo portal union added {} left-visible room(s) to the right-eye pass",
                    rooms_added_from_left);
            }
        }

        void reset_stereo_render_state()
        {
            // RF normally calls this once immediately before its single portal
            // traversal. A stereo frame has independent traversals, so each
            // pass needs fresh render-only visited/drawn flags as well.
            addr_as_ref<void()>(0x00488200)();

            // The stock renderer refreshes room_to_render_with only when either
            // last-frame field is older than rf::frame_count. Both OpenXR eyes
            // deliberately render in the same game frame, so without this reset
            // the right eye inherits the detail-room parent selected by the left
            // eye. That makes doors and props disappear or remain black until
            // both eyes happen to traverse the same parent room.
            if (rf::level.geometry) {
                for (rf::GRoom* room : rf::level.geometry->all_rooms) {
                    if (!room) {
                        continue;
                    }
                    room->room_to_render_with = nullptr;
                    room->last_frame_rendered_normal = -1;
                    room->last_frame_rendered_alpha = -1;
                }
            }
        }

        void capture_eye_room_render_state(
            std::vector<StereoRoomRenderState>& states)
        {
            states.clear();
            if (!rf::level.geometry) {
                return;
            }

            states.reserve(rf::level.geometry->all_rooms.size());
            for (rf::GRoom* room : rf::level.geometry->all_rooms) {
                if (!room) {
                    continue;
                }
                const bool rendered_normal =
                    room->last_frame_rendered_normal == rf::frame_count;
                const bool rendered_alpha =
                    room->last_frame_rendered_alpha == rf::frame_count;
                if (rendered_normal || rendered_alpha) {
                    states.push_back({
                        room, room->room_to_render_with,
                        rendered_normal, rendered_alpha,
                    });
                }
            }
        }

        void merge_eye_room_render_state(
            const std::vector<StereoRoomRenderState>& states)
        {
            // Keep RF's post-render visibility state as the union of both eyes.
            // room_to_render_with remains the right-eye choice when that room was
            // visible there, otherwise the valid left-eye parent is restored.
            for (const StereoRoomRenderState& state : states) {
                rf::GRoom* room = state.room;
                if (!room) {
                    continue;
                }
                const bool rendered_in_right_eye =
                    room->last_frame_rendered_normal == rf::frame_count ||
                    room->last_frame_rendered_alpha == rf::frame_count;
                if (state.rendered_normal) {
                    room->last_frame_rendered_normal = rf::frame_count;
                }
                if (state.rendered_alpha) {
                    room->last_frame_rendered_alpha = rf::frame_count;
                }
                if (!rendered_in_right_eye) {
                    room->room_to_render_with = state.render_parent;
                }
            }
        }

#ifdef AF_ENABLE_OPENXR
        struct TrackingOrigin
        {
            bool valid = false;
            XrVector3f position{};
            float yaw = 0.0f;
        };

        TrackingOrigin g_tracking_origin;
        XrVector3f g_latest_center_tracking_position{};
        bool g_latest_center_tracking_position_valid = false;
        bool g_recenter_requested = true;
        bool g_head_rotation_logged = false;

        struct VrWeaponCalibration
        {
            int weapon_id;
            rf::Vector3 grip_position;
            rf::Vector3 grip_rotation;
            rf::Vector3 pivot_position;
            rf::Vector3 pivot_rotation;
            rf::Vector3 muzzle_position;
            rf::Vector3 muzzle_direction;
            rf::Vector3 laser_position;
            rf::Vector3 laser_rotation;
            rf::Vector3 muzzle_rotation;
        };

        // RF weapon IDs are stable table indices for the stock campaign. These
        // entries are deliberately provisional: every weapon uses one transform
        // path and unknown/mod weapons receive the visible fallback below.
        // Move values are live Quest 3 calibrations. Undercover/special variants
        // inherit their matching campaign weapon because they are not separately
        // available in the normal singleplayer weapon cycle.
        const std::array g_weapon_calibrations{
            VrWeaponCalibration{0x00, {0.260f, 0.200f, -0.300f}, {}, {0.080f, -0.065f, 0.200f}, {}, {0.080f, -0.020f, 0.600f}, {0.0f, 0.0f, 1.0f}, {0.080f, -0.050f, 0.450f}, {0.0f, -0.02618f, 0.0f}}, // remote charge (laser excluded)
            VrWeaponCalibration{0x02, {-0.190f, -0.100f, -0.500f}, {}, {0.080f, -0.065f, 0.200f}, {}, {0.080f, -0.020f, 0.600f}, {0.0f, 0.0f, 1.0f}, {0.080f, -0.050f, 0.450f}, {0.0f, -0.02618f, 0.0f}}, // riot stick (laser excluded)
            VrWeaponCalibration{0x03, {-0.190f, 0.030f, -0.700f}, {0.0f, -0.013963f, 0.0f}, {0.055f, -0.045f, 0.105f}, {}, {0.055f, -0.015f, 0.310f}, {0.0f, 0.0f, 1.0f}, {0.295f, 0.035f, 1.045f}, {0.0f, -0.02618f, 0.0f}, {0.0f, -0.017453f, 0.0f}}, // handgun
            VrWeaponCalibration{0x04, {-0.190f, 0.000f, -0.750f}, {}, {0.055f, -0.045f, 0.125f}, {}, {0.055f, -0.015f, 0.390f}, {0.0f, 0.0f, 1.0f}, {0.055f, -0.045f, 0.310f}, {0.0f, -0.02618f, 0.0f}}, // undercover handgun
            VrWeaponCalibration{0x05, {-0.110f, 0.300f, 0.670f}, {-0.017453f, 0.0f, 0.0f}, {0.105f, -0.085f, 0.275f}, {}, {0.105f, -0.030f, 0.980f}, {0.0f, 0.0f, 1.0f}, {0.205f, -0.265f, 0.560f}, {0.0f, -0.02618f, 0.0f}, {0.0f, 0.027925f, 0.0f}}, // shotgun
            VrWeaponCalibration{0x06, {-0.190f, 0.200f, -0.600f}, {-0.010472f, -0.005236f, 0.0f}, {0.110f, -0.090f, 0.300f}, {}, {0.110f, -0.025f, 0.900f}, {0.0f, 0.0f, 1.0f}, {0.300f, -0.220f, 2.380f}, {0.0f, -0.02618f, 0.0f}, {0.0f, 0.034907f, 0.0f}}, // sniper rifle
            VrWeaponCalibration{0x07, {-0.090f, 0.180f, -0.250f}, {}, {0.140f, -0.105f, 0.325f}, {}, {0.230f, 0.010f, 0.950f}, {0.0f, 0.0f, 1.0f}, {0.230f, -0.250f, 1.300f}, {0.0f, -0.02618f, 0.0f}, {0.0f, 0.008727f, 0.0f}}, // rocket launcher
            VrWeaponCalibration{0x08, {-0.140f, 0.300f, -0.550f}, {-0.017453f, -0.008727f, 0.0f}, {0.105f, -0.085f, 0.265f}, {}, {0.105f, -0.025f, 0.790f}, {0.0f, 0.0f, 1.0f}, {0.285f, -0.240f, 1.600f}, {0.0f, -0.02618f, 0.0f}, {0.0f, 0.017453f, 0.0f}}, // assault rifle
            VrWeaponCalibration{0x09, {-0.390f, 0.290f, -0.650f}, {-0.017453f, -0.026180f, 0.0f}, {0.085f, -0.070f, 0.220f}, {}, {0.085f, -0.020f, 0.620f}, {0.0f, 0.0f, 1.0f}, {0.405f, -0.270f, 1.420f}, {0.0f, -0.02618f, 0.0f}, {0.0f, -0.078540f, 0.0f}}, // machine pistol
            VrWeaponCalibration{0x0A, {-0.390f, 0.250f, -0.750f}, {-0.017453f, -0.026180f, 0.0f}, {0.085f, -0.070f, 0.220f}, {}, {0.085f, -0.020f, 0.620f}, {0.0f, 0.0f, 1.0f}, {0.425f, -0.210f, 1.370f}, {0.0f, -0.02618f, 0.0f}, {0.0f, -0.069813f, 0.0f}}, // special machine pistol
            VrWeaponCalibration{0x0B, {-0.390f, 0.000f, -0.300f}, {}, {0.080f, -0.065f, 0.200f}, {}, {0.080f, -0.020f, 0.600f}, {0.0f, 0.0f, 1.0f}, {0.080f, -0.050f, 0.450f}, {0.0f, -0.02618f, 0.0f}}, // grenade (laser excluded)
            VrWeaponCalibration{0x0C, {-0.090f, -1.210f, -0.400f}, {}, {0.080f, -0.065f, 0.200f}, {}, {0.080f, -0.020f, 0.600f}, {0.0f, 0.0f, 1.0f}, {0.080f, -0.050f, 0.430f}, {0.0f, -0.02618f, 0.0f}}, // flamethrower
            VrWeaponCalibration{0x0D, {-0.140f, -0.050f, -0.350f}, {}, {0.080f, -0.065f, 0.200f}, {}, {0.080f, -0.020f, 0.600f}, {0.0f, 0.0f, 1.0f}, {0.080f, -0.050f, 0.450f}, {0.0f, -0.02618f, 0.0f}}, // riot shield (laser excluded)
            VrWeaponCalibration{0x0E, {1.160f, 0.200f, 0.100f}, {}, {0.120f, -0.095f, 0.310f}, {}, {0.120f, -0.020f, 0.920f}, {0.0f, 0.0f, 1.0f}, {-1.120f, -0.315f, 0.700f}, {0.0f, -0.02618f, 0.0f}, {0.0f, -0.026180f, 0.0f}}, // rail gun
            VrWeaponCalibration{0x0F, {-0.040f, 0.200f, -0.050f}, {}, {0.120f, -0.095f, 0.300f}, {}, {0.120f, -0.025f, 0.860f}, {0.0f, 0.0f, 1.0f}, {0.030f, -0.180f, 1.550f}, {0.0f, -0.02618f, 0.0f}, {0.0f, -0.043633f, 0.0f}}, // heavy machine gun
            VrWeaponCalibration{0x10, {-0.190f, 0.200f, -0.350f}, {-0.005236f, -0.017453f, 0.0f}, {0.110f, -0.090f, 0.285f}, {}, {0.110f, -0.025f, 0.850f}, {0.0f, 0.0f, 1.0f}, {0.290f, -0.120f, 1.750f}, {0.0f, -0.02618f, 0.0f}, {0.0f, -0.061087f, 0.0f}}, // scoped assault rifle
            VrWeaponCalibration{0x11, {0.210f, 0.100f, -0.750f}, {}, {0.145f, -0.110f, 0.335f}, {}, {0.145f, 0.010f, 1.000f}, {0.0f, 0.0f, 1.0f}, {-0.235f, -0.040f, 3.160f}, {0.0f, -0.02618f, 0.0f}, {0.0f, -0.052360f, 0.0f}}, // shoulder cannon
        };
        const VrWeaponCalibration g_fallback_weapon_calibration{
            -1, {-0.190f, 0.000f, -0.750f}, {}, {0.080f, -0.065f, 0.200f}, {},
            {0.080f, -0.020f, 0.600f}, {0.0f, 0.0f, 1.0f},
            {0.080f, -0.050f, 0.450f}, {0.0f, -0.02618f, 0.0f},
        };
        std::array<bool, 64> g_weapon_calibration_logged{};
        std::array<VrWeaponCalibration, 64> g_live_weapon_calibrations{};
        std::array<bool, 64> g_live_weapon_calibration_active{};

        struct VrTwoHandCalibration
        {
            int weapon_id;
            float support_fraction;
            float capture_radius;
            float maximum_hand_line_weight;
            rf::Vector3 support_position{};
            bool use_support_position = false;
        };

        // Measured support positions are weapon-local controller captures from
        // the alpha 0.7 calibration pass. Uncaptured guns retain the derived
        // primary-grip-to-muzzle fallback.
        const std::array g_two_hand_calibrations{
            VrTwoHandCalibration{0x03, 0.12f, 0.20f, 0.30f}, // handgun
            VrTwoHandCalibration{0x04, 0.12f, 0.20f, 0.30f}, // undercover handgun
            VrTwoHandCalibration{0x05, 0.55f, 0.28f, 1.00f, {0.2572f, -0.3771f, 0.0084f}, true}, // shotgun
            VrTwoHandCalibration{0x06, 0.52f, 0.28f, 1.00f, {0.2810f, -0.2803f, 1.2149f}, true}, // sniper rifle
            VrTwoHandCalibration{0x07, 0.45f, 0.30f, 0.95f, {0.2572f, -0.2974f, 0.8442f}, true}, // rocket launcher
            VrTwoHandCalibration{0x08, 0.52f, 0.27f, 1.00f, {0.2825f, -0.3366f, 1.1209f}, true}, // assault rifle
            VrTwoHandCalibration{0x09, 0.38f, 0.24f, 0.75f, {0.4461f, -0.3409f, 1.1667f}, true}, // machine pistol
            VrTwoHandCalibration{0x0A, 0.38f, 0.24f, 0.75f, {0.4576f, -0.3529f, 1.2405f}, true}, // special machine pistol
            VrTwoHandCalibration{0x0C, 0.48f, 0.30f, 0.90f}, // flamethrower
            VrTwoHandCalibration{0x0E, 0.52f, 0.29f, 1.00f, {-1.0995f, -0.3596f, 0.5678f}, true}, // rail gun
            VrTwoHandCalibration{0x0F, 0.50f, 0.30f, 1.00f, {0.0864f, -0.2529f, 0.7003f}, true}, // heavy machine gun
            VrTwoHandCalibration{0x10, 0.52f, 0.28f, 1.00f, {0.3310f, -0.2111f, 0.9334f}, true}, // scoped assault rifle
            VrTwoHandCalibration{0x11, 0.45f, 0.31f, 0.95f}, // shoulder cannon
        };
        const VrTwoHandCalibration g_fallback_two_hand_calibration{
            -1, 0.50f, 0.28f, 1.00f,
        };
        std::array<VrTwoHandCalibration, 64> g_live_two_hand_calibrations{};
        std::array<bool, 64> g_live_two_hand_calibration_active{};

        const VrWeaponCalibration& base_weapon_calibration(int weapon_id)
        {
            auto found = std::ranges::find_if(g_weapon_calibrations,
                [weapon_id](const VrWeaponCalibration& calibration) {
                    return calibration.weapon_id == weapon_id;
                });
            return found != g_weapon_calibrations.end()
                ? *found : g_fallback_weapon_calibration;
        }

        const VrWeaponCalibration& weapon_calibration(int weapon_id)
        {
            if (weapon_id >= 0 &&
                weapon_id < static_cast<int>(g_live_weapon_calibrations.size()) &&
                g_live_weapon_calibration_active[weapon_id]) {
                return g_live_weapon_calibrations[weapon_id];
            }
            return base_weapon_calibration(weapon_id);
        }

        int current_local_weapon_id()
        {
            if (!rf::local_player) {
                return -1;
            }
            auto* entity = rf::entity_from_handle(rf::local_player->entity_handle);
            return entity ? entity->ai.current_primary_weapon : -1;
        }

        bool native_weapon_scope_is_active()
        {
            if (!rf::local_player) {
                return false;
            }
            const int weapon_id = current_local_weapon_id();
            if (weapon_id != rf::sniper_rifle_weapon_type &&
                weapon_id != rf::scope_assault_rifle_weapon_type) {
                return false;
            }

            const auto& fpgun = rf::local_player->fpgun_data;
            return rf::player_fpgun_is_zoomed(rf::local_player) ||
                fpgun.zooming_in ||
                (fpgun.zoom_factor > 1.001f && !fpgun.zooming_out);
        }

        const VrTwoHandCalibration* base_two_hand_calibration(int weapon_id)
        {
            auto found = std::ranges::find_if(g_two_hand_calibrations,
                [weapon_id](const VrTwoHandCalibration& calibration) {
                    return calibration.weapon_id == weapon_id;
                });
            if (found != g_two_hand_calibrations.end()) {
                return &*found;
            }

            // Known non-guns and throwables never acquire the support hand.
            if (weapon_id == 0x00 || weapon_id == 0x01 ||
                weapon_id == 0x02 || weapon_id == 0x0B || weapon_id == 0x0D) {
                return nullptr;
            }
            if (weapon_id < 0 || weapon_id >= rf::num_weapon_types) {
                return nullptr;
            }

            // Mod weapons opt in by behaving like a gun. Gravity/remote-charge,
            // detonator, and melee flags identify authored non-gun handling.
            const int excluded_flags = rf::WTF_GRAVITY | rf::WTF_REMOTE_CHARGE |
                rf::WTF_DETONATOR | rf::WTF_MELEE;
            return (rf::weapon_types[weapon_id].flags & excluded_flags) == 0
                ? &g_fallback_two_hand_calibration : nullptr;
        }

        const VrTwoHandCalibration* two_hand_calibration(int weapon_id)
        {
            if (weapon_id >= 0 &&
                weapon_id < static_cast<int>(g_live_two_hand_calibrations.size()) &&
                g_live_two_hand_calibration_active[weapon_id]) {
                return &g_live_two_hand_calibrations[weapon_id];
            }
            return base_two_hand_calibration(weapon_id);
        }

        VrTwoHandCalibration* editable_two_hand_calibration(int weapon_id)
        {
            if (weapon_id < 0 ||
                weapon_id >= static_cast<int>(g_live_two_hand_calibrations.size())) {
                return nullptr;
            }
            const VrTwoHandCalibration* base = base_two_hand_calibration(weapon_id);
            if (!base) {
                return nullptr;
            }
            if (!g_live_two_hand_calibration_active[weapon_id]) {
                g_live_two_hand_calibrations[weapon_id] = *base;
                g_live_two_hand_calibrations[weapon_id].weapon_id = weapon_id;
                g_live_two_hand_calibration_active[weapon_id] = true;
            }
            return &g_live_two_hand_calibrations[weapon_id];
        }

        VrWeaponCalibration* editable_weapon_calibration()
        {
            const int weapon_id = current_local_weapon_id();
            if (weapon_id < 0 ||
                weapon_id >= static_cast<int>(g_live_weapon_calibrations.size())) {
                rf::console::print("No calibratable local weapon is equipped");
                return nullptr;
            }
            if (!g_live_weapon_calibration_active[weapon_id]) {
                g_live_weapon_calibrations[weapon_id] =
                    base_weapon_calibration(weapon_id);
                g_live_weapon_calibrations[weapon_id].weapon_id = weapon_id;
                g_live_weapon_calibration_active[weapon_id] = true;
            }
            return &g_live_weapon_calibrations[weapon_id];
        }

        rf::Vector3 transform_direction(const rf::Matrix3& basis, const rf::Vector3& value)
        {
            return basis.rvec * value.x + basis.uvec * value.y + basis.fvec * value.z;
        }

        rf::Vector3 direction_in_basis(const rf::Matrix3& basis,
            const rf::Vector3& direction)
        {
            return {
                basis.rvec.dot_prod(direction),
                basis.uvec.dot_prod(direction),
                basis.fvec.dot_prod(direction),
            };
        }

        rf::Matrix3 compose_orientation(const rf::Matrix3& base, const rf::Matrix3& relative)
        {
            return {
                transform_direction(base, relative.rvec),
                transform_direction(base, relative.uvec),
                transform_direction(base, relative.fvec),
            };
        }

        rf::Matrix3 relative_orientation(const rf::Matrix3& base,
            const rf::Matrix3& world)
        {
            const auto to_base_space = [&](const rf::Vector3& direction) {
                return rf::Vector3{
                    base.rvec.dot_prod(direction),
                    base.uvec.dot_prod(direction),
                    base.fvec.dot_prod(direction),
                };
            };
            return {
                to_base_space(world.rvec),
                to_base_space(world.uvec),
                to_base_space(world.fvec),
            };
        }

        bool get_local_mounted_aim_context(rf::Entity*& entity,
            rf::Entity*& host, bool& follows_host)
        {
            entity = nullptr;
            host = nullptr;
            follows_host = false;
            if (!rf::local_player) {
                return false;
            }

            entity = rf::entity_from_handle(rf::local_player->entity_handle);
            if (!entity) {
                return false;
            }

            const bool on_turret = rf::entity_is_on_turret(entity);
            const bool jeep_gunner = rf::entity_is_jeep_gunner(entity);
            if (!on_turret && !jeep_gunner) {
                return false;
            }

            host = rf::entity_from_handle(entity->host_handle);
            if (!host) {
                return false;
            }

            // A jeep gunner's neutral view follows the moving vehicle body.
            // A placed turret is itself the rotating host, so its entry
            // orientation must remain fixed or its aim would be applied twice.
            follows_host = jeep_gunner;
            return true;
        }

        void clear_mounted_aim_state()
        {
            g_mounted_aim_active = false;
            g_mounted_aim_follows_host = false;
            g_mounted_aim_host_handle = -1;
            g_mounted_native_aim_valid = false;
        }

        rf::Matrix3 level_yaw_orientation(const rf::Matrix3& orientation)
        {
            rf::Vector3 forward{
                orientation.fvec.x, 0.0f, orientation.fvec.z};
            float forward_length = forward.len();
            if (forward_length <= 0.001f) {
                // At almost +/-90 degrees of vehicle pitch the forward vector
                // no longer carries a reliable horizontal heading. RF's right
                // vector still does, so reconstruct forward from it instead.
                rf::Vector3 right{
                    orientation.rvec.x, 0.0f, orientation.rvec.z};
                const float right_length = right.len();
                if (right_length <= 0.001f) {
                    return {
                        {1.0f, 0.0f, 0.0f},
                        {0.0f, 1.0f, 0.0f},
                        {0.0f, 0.0f, 1.0f},
                    };
                }
                right /= right_length;
                forward = {-right.z, 0.0f, right.x};
                forward_length = 1.0f;
            }
            forward /= forward_length;
            const rf::Vector3 right{forward.z, 0.0f, -forward.x};
            return {
                right,
                {0.0f, 1.0f, 0.0f},
                forward,
            };
        }

        void update_local_attachment_transition()
        {
            auto* entity = rf::local_player
                ? rf::entity_from_handle(rf::local_player->entity_handle)
                : nullptr;
            if (!entity) {
                g_local_attachment_active = false;
                g_local_attachment_entity_handle = -1;
                g_vehicle_exit_horizon_stabilization = false;
                g_vehicle_exit_level_frames = 0;
                return;
            }

            const bool attached = rf::entity_in_vehicle(entity) ||
                rf::entity_is_on_turret(entity) ||
                rf::entity_is_jeep_gunner(entity);
            const bool same_player_entity =
                entity->handle == g_local_attachment_entity_handle;
            if (g_local_attachment_active && !attached && same_player_entity &&
                !rf::entity_is_dying(entity)) {
                // Pitchable vehicles (notably the submarine) can leave their
                // camera/control pitch and bank on the player entity for the
                // first on-foot frame. VR supplies head pitch/roll separately,
                // so retain body/eye yaw and discard only those stale axes.
                auto& control = entity->control_data;
                control.phb.x = 0.0f;
                control.phb.z = 0.0f;
                control.delta_phb.x = 0.0f;
                control.delta_phb.z = 0.0f;
                control.eye_phb.x = 0.0f;
                control.eye_phb.z = 0.0f;
                control.delta_eye_phb.x = 0.0f;
                control.delta_eye_phb.z = 0.0f;
                control.automobile_eye_phb = {};

                clear_mounted_aim_state();
                g_vehicle_exit_horizon_stabilization = true;
                g_vehicle_exit_level_frames = 0;
                g_roomscale_world_correction = {};
                g_roomscale_collision_frame = -1;
                g_head_pose_valid = false;
                g_weapon_pose_valid = false;
                g_weapon_aim_pose_valid = false;
                xlog::info(
                    "[AFVR] Mounted exit detected; cleared residual vehicle pitch/roll and stabilized the on-foot horizon");
            }
            else if (attached) {
                g_vehicle_exit_horizon_stabilization = false;
                g_vehicle_exit_level_frames = 0;
            }

            g_local_attachment_active = attached;
            g_local_attachment_entity_handle = entity->handle;
        }

        rf::Matrix3 mounted_vr_view_base(const rf::Matrix3& native_view)
        {
            rf::Entity* entity = nullptr;
            rf::Entity* host = nullptr;
            bool follows_host = false;
            if (!get_local_mounted_aim_context(entity, host, follows_host)) {
                clear_mounted_aim_state();
                auto* local_entity = rf::local_player
                    ? rf::entity_from_handle(rf::local_player->entity_handle)
                    : nullptr;
                if (g_vehicle_exit_horizon_stabilization && local_entity &&
                    !rf::entity_in_vehicle(local_entity) &&
                    !rf::entity_is_on_turret(local_entity)) {
                    constexpr float level_epsilon = 0.002f;
                    const bool native_view_is_level =
                        std::abs(native_view.fvec.y) <= level_epsilon &&
                        std::abs(native_view.rvec.y) <= level_epsilon &&
                        native_view.uvec.y >= 1.0f - level_epsilon;
                    g_vehicle_exit_level_frames = native_view_is_level
                        ? g_vehicle_exit_level_frames + 1 : 0;
                    const rf::Matrix3 leveled_view =
                        level_yaw_orientation(native_view);
                    if (g_vehicle_exit_level_frames >= 2) {
                        g_vehicle_exit_horizon_stabilization = false;
                        g_vehicle_exit_level_frames = 0;
                        xlog::info(
                            "[AFVR] Native on-foot camera is level after mounted exit");
                    }
                    return leveled_view;
                }
                return native_view;
            }

            if (!g_mounted_aim_active ||
                g_mounted_aim_host_handle != host->handle ||
                g_mounted_aim_follows_host != follows_host) {
                g_mounted_aim_active = true;
                g_mounted_aim_follows_host = follows_host;
                g_mounted_aim_host_handle = host->handle;
                g_mounted_neutral_orientation = native_view;
                g_mounted_neutral_relative_to_host =
                    relative_orientation(host->orient, native_view);
                g_mounted_native_aim_valid = false;
                xlog::info("[AFVR] Mounted HMD aim activated for {}",
                    follows_host ? "jeep gunner" : "turret");
            }
            else if (g_mounted_aim_follows_host) {
                g_mounted_neutral_orientation = compose_orientation(
                    host->orient, g_mounted_neutral_relative_to_host);
            }

            // RF's flat mounted camera is the authoritative current turret aim.
            // Keep it for the next simulation input pass, but render the HMD
            // from the neutral mount frame so that aim rotation is not doubled.
            g_mounted_native_aim_orientation = native_view;
            g_mounted_native_aim_valid = true;
            return g_mounted_neutral_orientation;
        }

        rf::Matrix3 euler_rotation_matrix(const rf::Vector3& rotation)
        {
            const float pitch_sin = std::sin(rotation.x);
            const float pitch_cos = std::cos(rotation.x);
            const float yaw_sin = std::sin(rotation.y);
            const float yaw_cos = std::cos(rotation.y);
            const float roll_sin = std::sin(rotation.z);
            const float roll_cos = std::cos(rotation.z);
            const rf::Matrix3 pitch{
                {1.0f, 0.0f, 0.0f},
                {0.0f, pitch_cos, pitch_sin},
                {0.0f, -pitch_sin, pitch_cos},
            };
            const rf::Matrix3 yaw{
                {yaw_cos, 0.0f, -yaw_sin},
                {0.0f, 1.0f, 0.0f},
                {yaw_sin, 0.0f, yaw_cos},
            };
            const rf::Matrix3 roll{
                {roll_cos, roll_sin, 0.0f},
                {-roll_sin, roll_cos, 0.0f},
                {0.0f, 0.0f, 1.0f},
            };
            return compose_orientation(
                compose_orientation(yaw, pitch), roll);
        }

        rf::Matrix3 xr_orientation_to_rf(const XrQuaternionf& value)
        {
            const float xx = value.x * value.x;
            const float yy = value.y * value.y;
            const float zz = value.z * value.z;
            const float xy = value.x * value.y;
            const float xz = value.x * value.z;
            const float yz = value.y * value.z;
            const float xw = value.x * value.w;
            const float yw = value.y * value.w;
            const float zw = value.z * value.w;

            // OpenXR uses -Z forward while RF uses +Z forward. Reflect both
            // the world and local Z axes (S*R*S).
            return {
                {1.0f - 2.0f * (yy + zz), 2.0f * (xy + zw), -2.0f * (xz - yw)},
                {2.0f * (xy - zw), 1.0f - 2.0f * (xx + zz), -2.0f * (yz + xw)},
                {-2.0f * (xz + yw), -2.0f * (yz - xw), 1.0f - 2.0f * (xx + yy)},
            };
        }

        rf::Matrix3 tracking_yaw_neutralizer()
        {
            return euler_rotation_matrix({0.0f, -g_tracking_origin.yaw, 0.0f});
        }

        void capture_tracking_origin(const XrPosef& hmd_pose)
        {
            const rf::Matrix3 hmd_orientation =
                xr_orientation_to_rf(hmd_pose.orientation);
            const bool replacing_valid_origin = g_tracking_origin.valid;
            const float preserved_height_reference = g_tracking_origin.position.y;
            g_tracking_origin.valid = true;
            g_tracking_origin.position = hmd_pose.position;
            if (replacing_valid_origin) {
                // Recenter is horizontal: retain the original standing-height
                // reference so live HMD height is never zeroed or recalibrated.
                g_tracking_origin.position.y = preserved_height_reference;
            }
            g_tracking_origin.yaw = std::atan2(
                hmd_orientation.fvec.x, hmd_orientation.fvec.z);
            g_latest_center_tracking_position = hmd_pose.position;
            g_latest_center_tracking_position_valid = true;
            g_recenter_requested = false;
            g_visual_body_yaw_offset = 0.0f;
            g_body_follow_frame = -1;
            g_roomscale_world_correction = {};
            g_roomscale_collision_frame = -1;
            if (replacing_valid_origin) {
                xlog::info(
                    "[AFVR] Recenter captured from valid HMD pose; horizontal offset and local yaw are now zero");
            }
            else {
                xlog::info(
                    "[AFVR] Initial tracking origin and player height captured from valid HMD pose");
            }
            xlog::info(
                "[AFVR] Tracking origin position=({:.3f}, {:.3f}, {:.3f}), neutral yaw={:.2f} degrees",
                hmd_pose.position.x, hmd_pose.position.y, hmd_pose.position.z,
                g_tracking_origin.yaw * 57.2957795131f);
        }

        rf::Vector3 relative_tracking_position(const XrVector3f& position)
        {
            const rf::Vector3 converted_delta{
                position.x - g_tracking_origin.position.x,
                position.y - g_tracking_origin.position.y,
                -(position.z - g_tracking_origin.position.z),
            };
            return transform_direction(tracking_yaw_neutralizer(), converted_delta);
        }

        rf::Vector3 move_roomscale_body_with_collision(rf::Entity* entity,
            const rf::Vector3& requested_delta)
        {
            const float movement_length = requested_delta.len();
            if (!entity || movement_length <= 0.0001f) {
                return {};
            }

            const rf::Vector3 direction = requested_delta / movement_length;
            float accepted_fraction = 1.0f;
            constexpr int collision_flags = rf::CF_PROCESS_INVISIBLE_FACES;
            constexpr float wall_skin = 0.01f;

            const auto sweep_sphere = [&](const rf::Vector3& local_center,
                                          float radius) {
                rf::Vector3 start = entity->pos +
                    transform_direction(entity->orient, local_center);
                rf::Vector3 desired = start + requested_delta;
                rf::PCollisionOut collision{};
                collision.obj_handle = -1;
                if (!rf::collide_sphereline_world(
                        &start, &desired, std::max(radius, 0.01f),
                        collision_flags, entity, nullptr, &collision)) {
                    return;
                }

                // A horizontal room-scale step must not be rejected by a
                // floor/ceiling contact tangent to the requested movement.
                const rf::Vector3 horizontal_normal{
                    collision.hit_normal.x, 0.0f, collision.hit_normal.z};
                if (horizontal_normal.len() <= 0.0001f ||
                    horizontal_normal.dot_prod(direction) >= -0.0001f) {
                    return;
                }

                float collision_distance = collision.hit_time * movement_length;
                if (!std::isfinite(collision.hit_time) ||
                    collision.hit_time < 0.0f || collision.hit_time > 1.0f) {
                    collision_distance = std::clamp(
                        (collision.hit_point - start).dot_prod(direction),
                        0.0f, movement_length);
                }
                const float safe_distance = std::max(
                    collision_distance - wall_skin, 0.0f);
                accepted_fraction = std::min(
                    accepted_fraction, safe_distance / movement_length);
            };

            if (entity->p_data.cspheres.size() > 0) {
                for (int i = 0; i < entity->p_data.cspheres.size(); ++i) {
                    const auto& sphere = entity->p_data.cspheres[i];
                    sweep_sphere(sphere.center, sphere.radius);
                }
            }
            else {
                sweep_sphere({}, std::max(entity->p_data.radius, 0.16f));
            }

            const rf::Vector3 body_position_before = entity->pos;
            const rf::Vector3 physics_position_before = entity->p_data.pos;
            const rf::Vector3 physics_next_position_before =
                entity->p_data.next_pos;
            const rf::Vector3 accepted_request =
                requested_delta * std::clamp(accepted_fraction, 0.0f, 1.0f);
            rf::Vector3 new_position = body_position_before + accepted_request;

            // Object::move is RF's object-position seam: it updates the object
            // position, bounding box and room membership. Preserve the native
            // physics trajectory by translating both physics positions by the
            // exact same collision-constrained amount.
            entity->move(&new_position);
            const rf::Vector3 accepted_delta =
                entity->pos - body_position_before;
            entity->p_data.pos = physics_position_before + accepted_delta;
            entity->p_data.next_pos =
                physics_next_position_before + accepted_delta;
            return accepted_delta;
        }

        rf::Vector3 reconcile_roomscale_body(const XrVector3f& raw_hmd_position,
            const rf::Matrix3& view_base)
        {
            if (!g_tracking_origin.valid || rf::is_multi ||
                g_menu_capture_active ||
                rf::gameseq_get_state() != rf::GS_GAMEPLAY ||
                !rf::local_player) {
                return {};
            }

            auto* entity = rf::entity_from_handle(
                rf::local_player->entity_handle);
            if (!entity || rf::entity_is_dying(entity) ||
                rf::entity_in_vehicle(entity) ||
                rf::entity_is_on_turret(entity) ||
                rf::entity_is_jeep_gunner(entity)) {
                return {};
            }

            const rf::Vector3 local_head_offset =
                relative_tracking_position(raw_hmd_position);
            const rf::Vector3 local_horizontal{
                local_head_offset.x, 0.0f, local_head_offset.z};
            constexpr float reconciliation_epsilon = 0.015f;
            if (local_horizontal.len() <= reconciliation_epsilon) {
                return {};
            }

            const rf::Matrix3 body_yaw = level_yaw_orientation(view_base);
            const rf::Vector3 requested_body_delta =
                transform_direction(body_yaw, local_horizontal);
            const rf::Vector3 body_position_before = entity->pos;
            const rf::Vector3 accepted_body_delta =
                move_roomscale_body_with_collision(
                    entity, requested_body_delta);

            // Consume only the movement RF actually accepted. Convert that
            // world delta back through body yaw and the inverse recenter-yaw
            // neutralizer to advance the raw OpenXR reference. Head and hands
            // consequently remain fixed in world space without per-device fixes.
            rf::Vector3 accepted_local =
                direction_in_basis(body_yaw, accepted_body_delta);
            accepted_local.y = 0.0f;
            const rf::Vector3 accepted_tracking_rf = transform_direction(
                euler_rotation_matrix({0.0f, g_tracking_origin.yaw, 0.0f}),
                accepted_local);
            g_tracking_origin.position.x += accepted_tracking_rf.x;
            g_tracking_origin.position.z -= accepted_tracking_rf.z;

            const rf::Vector3 remaining_local =
                relative_tracking_position(raw_hmd_position);
            if (!g_roomscale_reconciliation_logged) {
                g_roomscale_reconciliation_logged = true;
                xlog::info(
                    "[AFVR][ROOMSCALE] raw_hmd=({:.3f},{:.3f},{:.3f}) tracking_ref=({:.3f},{:.3f},{:.3f}) local_horizontal=({:.3f},{:.3f}) requested=({:.3f},{:.3f},{:.3f}) accepted=({:.3f},{:.3f},{:.3f}) body_before=({:.3f},{:.3f},{:.3f}) body_after=({:.3f},{:.3f},{:.3f}) remaining_local=({:.3f},{:.3f})",
                    raw_hmd_position.x, raw_hmd_position.y,
                    raw_hmd_position.z,
                    g_tracking_origin.position.x,
                    g_tracking_origin.position.y,
                    g_tracking_origin.position.z,
                    local_horizontal.x, local_horizontal.z,
                    requested_body_delta.x, requested_body_delta.y,
                    requested_body_delta.z,
                    accepted_body_delta.x, accepted_body_delta.y,
                    accepted_body_delta.z,
                    body_position_before.x, body_position_before.y,
                    body_position_before.z,
                    entity->pos.x, entity->pos.y, entity->pos.z,
                    remaining_local.x, remaining_local.z);
            }
            return accepted_body_delta;
        }

        void begin_turn_diagnostic(const rf::Entity* entity)
        {
            if (!entity || g_turn_diagnostic.pending ||
                g_turn_diagnostic.logged || !g_tracking_origin.valid ||
                !g_latest_center_tracking_position_valid ||
                !g_head_pose_valid) {
                return;
            }

            g_turn_diagnostic.pending = true;
            g_turn_diagnostic.requested_frame = rf::frame_count;
            g_turn_diagnostic.body_position_before = entity->pos;
            g_turn_diagnostic.local_head_offset_before =
                relative_tracking_position(g_latest_center_tracking_position);
            g_turn_diagnostic.body_yaw_before = std::atan2(
                entity->orient.fvec.x, entity->orient.fvec.z);
            g_turn_diagnostic.head_world_position_before = g_head_position;
        }

        float wrap_yaw(float yaw)
        {
            constexpr float two_pi = 6.28318530718f;
            return std::remainder(yaw, two_pi);
        }

        void update_resolved_player_pose(const rf::Vector3& head_local_offset)
        {
            auto* entity = rf::local_player
                ? rf::entity_from_handle(rf::local_player->entity_handle)
                : nullptr;
            if (!entity || !g_head_pose_valid) {
                g_resolved_player_pose.valid = false;
                return;
            }

            const bool body_follow_allowed =
                !rf::entity_in_vehicle(entity) &&
                !rf::entity_is_on_turret(entity) &&
                !rf::entity_is_jeep_gunner(entity);
            if (g_body_follow_frame != rf::frame_count) {
                g_body_follow_frame = rf::frame_count;
                if (body_follow_allowed) {
                    constexpr float degrees_to_radians = 0.0174532925199f;
                    constexpr float deadzone = 30.0f * degrees_to_radians;
                    constexpr float target_relative_yaw =
                        15.0f * degrees_to_radians;
                    constexpr float follow_speed =
                        120.0f * degrees_to_radians;
                    const float relative_to_visual_body = wrap_yaw(
                        g_hmd_relative_yaw - g_visual_body_yaw_offset);
                    if (std::abs(relative_to_visual_body) > deadzone) {
                        const float target_offset = wrap_yaw(
                            g_hmd_relative_yaw -
                            std::copysign(target_relative_yaw,
                                relative_to_visual_body));
                        const float error = wrap_yaw(
                            target_offset - g_visual_body_yaw_offset);
                        const float maximum_step = follow_speed *
                            std::max(rf::frametime, 0.0f);
                        g_visual_body_yaw_offset = wrap_yaw(
                            g_visual_body_yaw_offset +
                            std::clamp(error, -maximum_step, maximum_step));
                        if (!g_body_follow_logged) {
                            g_body_follow_logged = true;
                            xlog::info(
                                "[AFVR] Visual body follow active: 30 degree deadzone, 15 degree target, 120 degrees/second");
                        }
                    }
                }
                else {
                    g_visual_body_yaw_offset = 0.0f;
                }
            }

            const rf::Matrix3 visual_yaw =
                euler_rotation_matrix({0.0f, g_visual_body_yaw_offset, 0.0f});
            g_resolved_player_pose.valid = true;
            g_resolved_player_pose.body_world_position = entity->pos;
            g_resolved_player_pose.body_world_orientation = entity->orient;
            g_resolved_player_pose.visual_body_world_orientation =
                compose_orientation(entity->orient, visual_yaw);
            g_resolved_player_pose.head_local_offset = head_local_offset;
            g_resolved_player_pose.head_local_yaw = g_hmd_relative_yaw;
            g_resolved_player_pose.head_world_position = g_head_position;
            g_resolved_player_pose.head_world_orientation = g_head_orientation;
            g_resolved_player_pose.tracking_reference_position = {
                g_tracking_origin.position.x,
                g_tracking_origin.position.y,
                g_tracking_origin.position.z,
            };
            g_resolved_player_pose.tracking_reference_yaw =
                g_tracking_origin.yaw;
            if (g_turn_diagnostic.pending &&
                rf::frame_count != g_turn_diagnostic.requested_frame) {
                g_turn_diagnostic.pending = false;
                g_turn_diagnostic.logged = true;
                const float body_yaw_after = std::atan2(
                    entity->orient.fvec.x, entity->orient.fvec.z);
                xlog::info(
                    "[AFVR][TURN_PIVOT] body_pos before=({:.3f},{:.3f},{:.3f}) after=({:.3f},{:.3f},{:.3f}); head_local before=({:.3f},{:.3f},{:.3f}) after=({:.3f},{:.3f},{:.3f}); body_yaw before={:.2f} after={:.2f}; head_world before=({:.3f},{:.3f},{:.3f}) after=({:.3f},{:.3f},{:.3f})",
                    g_turn_diagnostic.body_position_before.x,
                    g_turn_diagnostic.body_position_before.y,
                    g_turn_diagnostic.body_position_before.z,
                    entity->pos.x, entity->pos.y, entity->pos.z,
                    g_turn_diagnostic.local_head_offset_before.x,
                    g_turn_diagnostic.local_head_offset_before.y,
                    g_turn_diagnostic.local_head_offset_before.z,
                    head_local_offset.x, head_local_offset.y,
                    head_local_offset.z,
                    g_turn_diagnostic.body_yaw_before * 57.2957795131f,
                    body_yaw_after * 57.2957795131f,
                    g_turn_diagnostic.head_world_position_before.x,
                    g_turn_diagnostic.head_world_position_before.y,
                    g_turn_diagnostic.head_world_position_before.z,
                    g_head_position.x, g_head_position.y,
                    g_head_position.z);
            }
            if (!g_resolved_pose_logged) {
                g_resolved_pose_logged = true;
                const float body_yaw = std::atan2(
                    entity->orient.fvec.x, entity->orient.fvec.z);
                constexpr float radians_to_degrees = 57.2957795131f;
                xlog::info(
                    "[AFVR] Resolved pose body_pos=({:.3f},{:.3f},{:.3f}) body_yaw={:.2f} head_local=({:.3f},{:.3f},{:.3f}) head_yaw={:.2f} head_world=({:.3f},{:.3f},{:.3f}) tracking_ref=({:.3f},{:.3f},{:.3f}) tracking_yaw={:.2f}",
                    entity->pos.x, entity->pos.y, entity->pos.z,
                    body_yaw * radians_to_degrees,
                    head_local_offset.x, head_local_offset.y,
                    head_local_offset.z,
                    g_hmd_relative_yaw * radians_to_degrees,
                    g_head_position.x, g_head_position.y, g_head_position.z,
                    g_tracking_origin.position.x,
                    g_tracking_origin.position.y,
                    g_tracking_origin.position.z,
                    g_tracking_origin.yaw * radians_to_degrees);
            }
        }

        rf::Matrix3 relative_tracking_orientation(const XrQuaternionf& orientation)
        {
            // Remove only the neutral yaw. Current pitch and roll remain live,
            // including at the instant the origin is captured.
            return compose_orientation(
                tracking_yaw_neutralizer(), xr_orientation_to_rf(orientation));
        }

        bool transform_tracked_pose_to_world(const XrPosef& tracked_pose,
            const rf::Vector3& base_position, const rf::Matrix3& base_orientation,
            rf::Vector3& world_position, rf::Matrix3& world_orientation)
        {
            if (!g_tracking_origin.valid) {
                return false;
            }

            const rf::Vector3 relative_rf_position =
                relative_tracking_position(tracked_pose.position);
            const rf::Matrix3 relative_rf_orientation =
                relative_tracking_orientation(tracked_pose.orientation);

            world_position = base_position +
                transform_direction(base_orientation, relative_rf_position);
            world_position += g_roomscale_world_correction;
            world_orientation = compose_orientation(
                base_orientation, relative_rf_orientation);
            return true;
        }

        rf::Vector3 roomscale_collision_correction(
            const rf::Vector3& base_position,
            const rf::Matrix3& base_orientation,
            const rf::Vector3& relative_center_position)
        {
            if (g_menu_capture_active ||
                rf::gameseq_get_state() != rf::GS_GAMEPLAY ||
                !rf::local_player) {
                return {};
            }

            auto* entity = rf::entity_from_handle(rf::local_player->entity_handle);
            if (!entity || rf::entity_is_dying(entity) ||
                rf::entity_in_vehicle(entity) ||
                rf::entity_is_on_turret(entity)) {
                return {};
            }

            rf::Vector3 start = base_position;
            rf::Vector3 desired = base_position +
                transform_direction(base_orientation, relative_center_position);
            rf::Vector3 movement = desired - start;
            const float movement_length = movement.len();
            if (movement_length <= 0.001f) {
                return {};
            }

            const rf::Vector3 direction = movement / movement_length;
            rf::PCollisionOut collision{};
            collision.obj_handle = -1;
            constexpr float head_radius = 0.16f;
            constexpr int collision_flags =
                rf::CF_PROCESS_INVISIBLE_FACES;
            bool collision_hit = rf::collide_sphereline_world(
                    &start, &desired, head_radius, collision_flags,
                    entity, nullptr, &collision);
            if (!collision_hit) {
                collision = {};
                collision.obj_handle = -1;
                collision_hit = rf::collide_linesegment_world(
                    start, desired, collision_flags, &collision);
            }
            if (!collision_hit) {
                return {};
            }

            float collision_distance = collision.hit_time * movement_length;
            if (!std::isfinite(collision.hit_time) ||
                collision.hit_time < 0.0f || collision.hit_time > 1.0f) {
                collision_distance = std::clamp(
                    (collision.hit_point - start).dot_prod(direction),
                    0.0f, movement_length);
            }
            constexpr float wall_skin = 0.02f;
            const float safe_distance = std::max(
                collision_distance - wall_skin, 0.0f);
            const rf::Vector3 constrained = start + direction * safe_distance;
            if (!g_roomscale_collision_logged) {
                g_roomscale_collision_logged = true;
                xlog::info(
                    "[AFVR] Room-scale HMD collision pushback activated (hit time {:.3f}, object {})",
                    collision.hit_time, collision.obj_handle);
            }
            return constrained - desired;
        }

        rf::Vector3 normalized_or(rf::Vector3 value, const rf::Vector3& fallback)
        {
            const float length = value.len();
            if (length <= 0.0001f) {
                return fallback;
            }
            return value / length;
        }

        float smooth_step(float value)
        {
            value = std::clamp(value, 0.0f, 1.0f);
            return value * value * (3.0f - 2.0f * value);
        }

        rf::Matrix3 solve_two_hand_aim_orientation(
            const rf::Vector3& primary_position,
            const rf::Matrix3& primary_aim,
            const rf::Vector3& support_position,
            const rf::Matrix3& support_aim,
            float maximum_hand_line_weight)
        {
            // Averaging both aim poses provides a stable result for pistols and
            // near-overlapping hands. As hand separation grows, long guns move
            // toward the physical primary-to-support baseline.
            const rf::Vector3 averaged_forward = normalized_or(
                primary_aim.fvec + support_aim.fvec, primary_aim.fvec);
            const rf::Vector3 hand_delta = support_position - primary_position;
            const float hand_separation = hand_delta.len();
            const rf::Vector3 hand_forward = normalized_or(
                hand_delta, averaged_forward);
            const float line_alignment = std::clamp(
                averaged_forward.dot_prod(hand_forward), 0.0f, 1.0f);
            const float separation_factor = smooth_step(
                (hand_separation - 0.07f) / 0.23f);
            const float alignment_factor = smooth_step(
                (line_alignment - 0.15f) / 0.70f);
            const float hand_line_weight = std::clamp(
                maximum_hand_line_weight * separation_factor * alignment_factor,
                0.0f, 1.0f);
            const rf::Vector3 forward = normalized_or(
                averaged_forward * (1.0f - hand_line_weight) +
                    hand_forward * hand_line_weight,
                primary_aim.fvec);

            // Preserve natural controller roll while rebuilding a strictly
            // orthonormal RF basis around the solved barrel direction.
            rf::Vector3 reference_up = normalized_or(
                primary_aim.uvec + support_aim.uvec, primary_aim.uvec);
            reference_up -= forward * reference_up.dot_prod(forward);
            if (reference_up.len() <= 0.0001f) {
                reference_up = forward.cross(primary_aim.rvec);
            }
            reference_up = normalized_or(reference_up, primary_aim.uvec);
            const rf::Vector3 right = normalized_or(
                reference_up.cross(forward), primary_aim.rvec);
            const rf::Vector3 up = normalized_or(
                forward.cross(right), primary_aim.uvec);
            return {right, up, forward};
        }

        void update_weapon_pose(const rf::Vector3& base_position,
            const rf::Matrix3& base_orientation)
        {
            const auto& input = g_openxr->input_state();
            constexpr size_t left_hand = 0;
            constexpr size_t right_hand = 1;
            g_laser_emitter_pose_valid = false;
            g_left_controller_grip_world_valid = false;
            g_left_controller_aim_world_valid = false;
            rf::Vector3 controller_grip_position{};
            rf::Matrix3 controller_grip_orientation{};
            g_weapon_pose_valid = input.grip_pose_valid[right_hand] &&
                transform_tracked_pose_to_world(input.grip_poses[right_hand],
                    base_position, base_orientation,
                    controller_grip_position, controller_grip_orientation);
            rf::Vector3 controller_aim_position{};
            rf::Matrix3 controller_aim_orientation{};
            g_weapon_aim_pose_valid = input.aim_pose_valid[right_hand] &&
                transform_tracked_pose_to_world(input.aim_poses[right_hand],
                    base_position, base_orientation,
                    controller_aim_position, controller_aim_orientation);
            g_controller_grip_world_valid = g_weapon_pose_valid;
            g_controller_aim_world_valid = g_weapon_aim_pose_valid;
            if (g_controller_grip_world_valid) {
                g_controller_grip_world_position = controller_grip_position;
                g_controller_grip_world_orientation = controller_grip_orientation;
            }
            if (g_controller_aim_world_valid) {
                g_controller_aim_world_position = controller_aim_position;
                g_controller_aim_world_orientation = controller_aim_orientation;
            }
            g_right_controller_pose_valid = false;
            if (!g_weapon_pose_valid || !g_weapon_aim_pose_valid) {
                g_two_hand_support_available = false;
                g_two_hand_weapon_active = false;
                g_two_hand_weapon_id = -1;
                return;
            }

            rf::Vector3 support_grip_position{};
            rf::Matrix3 support_grip_orientation{};
            const bool support_grip_pose_valid =
                input.grip_pose_valid[left_hand] &&
                transform_tracked_pose_to_world(input.grip_poses[left_hand],
                    base_position, base_orientation,
                    support_grip_position, support_grip_orientation);
            rf::Vector3 support_aim_position{};
            rf::Matrix3 support_aim_orientation = controller_aim_orientation;
            const bool support_aim_pose_valid =
                input.aim_pose_valid[left_hand] &&
                transform_tracked_pose_to_world(input.aim_poses[left_hand],
                    base_position, base_orientation,
                    support_aim_position, support_aim_orientation);
            g_left_controller_grip_world_valid = support_grip_pose_valid;
            g_left_controller_aim_world_valid = support_aim_pose_valid;
            if (support_grip_pose_valid) {
                g_left_controller_grip_world_position = support_grip_position;
                g_left_controller_grip_world_orientation = support_grip_orientation;
            }
            if (support_aim_pose_valid) {
                g_left_controller_aim_world_position = support_aim_position;
                g_left_controller_aim_world_orientation = support_aim_orientation;
            }

            // Effects such as ejected casings use the physical primary-grip
            // position and the final solved weapon orientation.
            g_right_controller_position = controller_grip_position;
            g_right_controller_pose_valid = true;

            g_current_weapon_id = current_local_weapon_id();
            const auto& calibration = weapon_calibration(g_current_weapon_id);
            const VrTwoHandCalibration* two_hand =
                two_hand_calibration(g_current_weapon_id);

            // Quest Touch's grip pose supplies the physical controller position,
            // while its aim pose supplies the barrel-compatible orientation that
            // was validated in the previous live build. The grip orientation points
            // the RF meshes upward and must not be used as the weapon basis.
            rf::Matrix3 solved_aim_orientation = controller_aim_orientation;
            rf::Matrix3 grip_anchor_orientation = compose_orientation(
                controller_aim_orientation,
                euler_rotation_matrix(calibration.grip_rotation));
            rf::Vector3 grip_anchor_position = controller_grip_position +
                transform_direction(grip_anchor_orientation,
                    calibration.grip_position);
            g_weapon_render_orientation = compose_orientation(
                grip_anchor_orientation,
                euler_rotation_matrix(calibration.pivot_rotation));
            g_weapon_render_position = grip_anchor_position - transform_direction(
                g_weapon_render_orientation, calibration.pivot_position);

            // Compute the acquisition target from the current one-hand pose.
            // Once grabbed, retain the support grip until squeeze release even
            // if recoil or a large hand motion moves outside the capture sphere.
            g_two_hand_support_available = false;
            if (!two_hand || !support_grip_pose_valid || g_menu_capture_active ||
                g_two_hand_calibration_active) {
                g_two_hand_weapon_active = false;
                g_two_hand_weapon_id = -1;
            }
            else {
                const rf::Vector3 model_support_position =
                    two_hand->use_support_position
                    ? two_hand->support_position
                    : calibration.pivot_position +
                        (calibration.muzzle_position - calibration.pivot_position) *
                            two_hand->support_fraction;
                const rf::Vector3 support_target = g_weapon_render_position +
                    transform_direction(g_weapon_render_orientation,
                        model_support_position);
                g_two_hand_support_available =
                    (support_grip_position - support_target).len() <=
                    two_hand->capture_radius;

                if (!g_left_grip_pressed) {
                    g_two_hand_weapon_active = false;
                    g_two_hand_weapon_id = -1;
                }
                else if (g_two_hand_weapon_id != g_current_weapon_id) {
                    g_two_hand_weapon_active = g_two_hand_support_available;
                    g_two_hand_weapon_id = g_two_hand_weapon_active
                        ? g_current_weapon_id : -1;
                }

                if (g_two_hand_weapon_active) {
                    solved_aim_orientation = solve_two_hand_aim_orientation(
                        controller_grip_position, controller_aim_orientation,
                        support_grip_position,
                        support_aim_pose_valid
                            ? support_aim_orientation : controller_aim_orientation,
                        two_hand->maximum_hand_line_weight);
                    grip_anchor_orientation = compose_orientation(
                        solved_aim_orientation,
                        euler_rotation_matrix(calibration.grip_rotation));
                    grip_anchor_position = controller_grip_position +
                        transform_direction(grip_anchor_orientation,
                            calibration.grip_position);
                    g_weapon_render_orientation = compose_orientation(
                        grip_anchor_orientation,
                        euler_rotation_matrix(calibration.pivot_rotation));
                    g_weapon_render_position = grip_anchor_position -
                        transform_direction(g_weapon_render_orientation,
                            calibration.pivot_position);

                    if (g_current_weapon_id >= 0 &&
                        g_current_weapon_id < static_cast<int>(g_two_hand_weapon_logged.size()) &&
                        !g_two_hand_weapon_logged[g_current_weapon_id]) {
                        g_two_hand_weapon_logged[g_current_weapon_id] = true;
                        xlog::info(
                            "[AFVR] Weapon {} acquired two-hand support grip; visual, muzzle, and fire direction share the solved pose",
                            g_current_weapon_id);
                    }
                }
            }

            g_right_controller_orientation = solved_aim_orientation;

            // Muzzle position follows the visual weapon. Apply one small shared
            // horizontal correction to the platform aim pose; live testing found
            // the uncorrected Quest Touch ray consistently landed to the right.
            g_weapon_fire_position = g_weapon_render_position + transform_direction(
                g_weapon_render_orientation, calibration.muzzle_position);
            constexpr float degrees_to_radians = 0.0174532925199f;
            const rf::Matrix3 muzzle_orientation = compose_orientation(
                solved_aim_orientation,
                euler_rotation_matrix(calibration.muzzle_rotation));
            g_weapon_fire_orientation = compose_orientation(
                muzzle_orientation,
                euler_rotation_matrix({
                    0.0f,
                    g_weapon_aim_yaw_correction_degrees * degrees_to_radians,
                    0.0f,
                }));

            // The visible weapon root is authoritative. Resolve the dedicated
            // weapon-local laser attachment only after one/two-hand blending so
            // the beam cannot drift back to the raw controller pose.
            g_laser_emitter_position = g_weapon_render_position +
                transform_direction(g_weapon_render_orientation,
                    calibration.laser_position);
            const rf::Matrix3 laser_muzzle_orientation = compose_orientation(
                g_weapon_render_orientation,
                euler_rotation_matrix(calibration.muzzle_rotation));
            g_laser_emitter_orientation = compose_orientation(
                laser_muzzle_orientation,
                euler_rotation_matrix(calibration.laser_rotation));
            g_laser_emitter_pose_valid = true;

            if (g_current_weapon_id >= 0 &&
                g_current_weapon_id < static_cast<int>(g_weapon_calibration_logged.size()) &&
                !g_weapon_calibration_logged[g_current_weapon_id]) {
                g_weapon_calibration_logged[g_current_weapon_id] = true;
                xlog::info(
                    "[AFVR] Weapon {} uses {} VR calibration; final one/two-hand weapon root owns muzzle and laser emitter",
                    g_current_weapon_id,
                    calibration.weapon_id >= 0 ? "stock" : "fallback");
            }
        }

        bool current_weapon_supports_laser()
        {
            if (g_current_weapon_id < 0 ||
                g_current_weapon_id == rf::remote_charge_weapon_type ||
                g_current_weapon_id == rf::remote_charge_det_weapon_type ||
                g_current_weapon_id == rf::grenade_weapon_type ||
                g_current_weapon_id == rf::riot_shield_weapon_type) {
                return false;
            }
            return !rf::weapon_is_melee(g_current_weapon_id);
        }

        void audit_weapon_fire_origin(bool print_to_console)
        {
            static std::array<bool, 64> automatically_logged{};
            const int weapon_id = g_current_weapon_id;
            if (!g_weapon_pose_valid || !g_weapon_aim_pose_valid ||
                weapon_id < 0) {
                if (print_to_console) {
                    rf::console::print(
                        "VR fire-origin audit requires a tracked equipped weapon");
                }
                return;
            }
            if (!print_to_console &&
                weapon_id < static_cast<int>(automatically_logged.size()) &&
                automatically_logged[weapon_id]) {
                return;
            }
            if (weapon_id < static_cast<int>(automatically_logged.size())) {
                automatically_logged[weapon_id] = true;
            }

            const rf::Vector3 fire_direction = normalized_or(
                g_weapon_fire_orientation.fvec, {0.0f, 0.0f, 1.0f});
            rf::Vector3 close_trace_end =
                g_weapon_fire_position + fire_direction * 2.0f;
            rf::LevelCollisionOut collision{};
            collision.obj_handle = -1;
            rf::Object* local_entity = rf::local_player
                ? rf::entity_from_handle(rf::local_player->entity_handle)
                : nullptr;
            const bool close_trace_hit = rf::collide_linesegment_level_for_multi(
                g_weapon_fire_position, close_trace_end,
                local_entity, nullptr, &collision, 0.0f, true, 1.0f);
            const float close_hit_distance = close_trace_hit
                ? (collision.hit_point - g_weapon_fire_position).len() : 2.0f;
            const rf::Vector3 laser_delta =
                g_laser_emitter_position - g_weapon_fire_position;

            xlog::info(
                "[AFVR][FIRE_ORIGIN] weapon={} fire=({:.4f},{:.4f},{:.4f}) laser=({:.4f},{:.4f},{:.4f}) laser_delta=({:.4f},{:.4f},{:.4f}) independent=true close_ray_hit={} object={} distance={:.3f}m",
                weapon_id,
                g_weapon_fire_position.x, g_weapon_fire_position.y,
                g_weapon_fire_position.z,
                g_laser_emitter_position.x, g_laser_emitter_position.y,
                g_laser_emitter_position.z,
                laser_delta.x, laser_delta.y, laser_delta.z,
                close_trace_hit, collision.obj_handle, close_hit_distance);
            if (print_to_console) {
                rf::console::print(
                    "VR fire point: {:.3f} {:.3f} {:.3f}",
                    g_weapon_fire_position.x, g_weapon_fire_position.y,
                    g_weapon_fire_position.z);
                rf::console::print(
                    "VR visual laser: {:.3f} {:.3f} {:.3f} (separation {:.3f} m)",
                    g_laser_emitter_position.x, g_laser_emitter_position.y,
                    g_laser_emitter_position.z, laser_delta.len());
                rf::console::print(
                    "2 m fire ray: {} object {} at {:.3f} m",
                    close_trace_hit ? "hit" : "clear",
                    collision.obj_handle, close_hit_distance);
            }
        }

        void update_laser_trace()
        {
            g_laser_trace_frame = rf::frame_count;
            g_laser_trace_valid = false;
            g_laser_trace_hit = false;
            if (!g_laser_sight_enabled || !g_laser_emitter_pose_valid ||
                !current_weapon_supports_laser()) {
                return;
            }

            const rf::Vector3 direction = normalized_or(
                g_laser_emitter_orientation.fvec,
                {0.0f, 0.0f, 1.0f});
            g_laser_trace_start = g_laser_emitter_position;
            g_laser_trace_end = g_laser_trace_start + direction * 1000.0f;

            rf::LevelCollisionOut collision{};
            collision.obj_handle = -1;
            rf::Object* local_entity = nullptr;
            if (rf::local_player) {
                local_entity = rf::entity_from_handle(rf::local_player->entity_handle);
            }
            if (rf::collide_linesegment_level_for_multi(
                    g_laser_trace_start, g_laser_trace_end,
                    local_entity, nullptr, &collision,
                    0.0f, true, 1.0f)) {
                g_laser_trace_hit = true;
                g_laser_trace_end = collision.hit_point;
            }
            // A valid resolved emitter owns this frame's shared world-space
            // trace. The beam renderer rejects a degenerate sub-5 mm segment.
            g_laser_trace_valid = true;

            if (g_laser_trace_valid && !g_laser_coordinate_audit_logged) {
                g_laser_coordinate_audit_logged = true;
                constexpr size_t right_hand = 1;
                const auto& input = g_openxr->input_state();
                const auto& grip_local = input.grip_poses[right_hand];
                const auto& aim_local = input.aim_poses[right_hand];
                const rf::Vector3 provisional_muzzle = g_weapon_fire_position;
                const rf::Vector3 firing_direction = normalized_or(
                    g_weapon_fire_orientation.fvec, {0.0f, 0.0f, 1.0f});
                const auto& calibration = weapon_calibration(g_current_weapon_id);
                constexpr float radians_to_degrees = 57.2957795131f;
                const rf::Vector3 laser_rotation_degrees =
                    calibration.laser_rotation * radians_to_degrees;
                const rf::Vector3 grip_from_head = g_head_pose_valid
                    ? g_controller_grip_world_position - g_head_position
                    : rf::Vector3{};
                xlog::info("[AFVR][LASER] Coordinate audit (single diagnostic frame)");
                xlog::info(
                    "[AFVR][LASER] controller grip OpenXR local pos=({:.4f},{:.4f},{:.4f}) quat=({:.4f},{:.4f},{:.4f},{:.4f})",
                    grip_local.position.x, grip_local.position.y, grip_local.position.z,
                    grip_local.orientation.x, grip_local.orientation.y,
                    grip_local.orientation.z, grip_local.orientation.w);
                xlog::info(
                    "[AFVR][LASER] controller grip RF world pos=({:.4f},{:.4f},{:.4f}) basis R=({:.4f},{:.4f},{:.4f}) U=({:.4f},{:.4f},{:.4f}) F=({:.4f},{:.4f},{:.4f})",
                    g_controller_grip_world_position.x,
                    g_controller_grip_world_position.y,
                    g_controller_grip_world_position.z,
                    g_controller_grip_world_orientation.rvec.x,
                    g_controller_grip_world_orientation.rvec.y,
                    g_controller_grip_world_orientation.rvec.z,
                    g_controller_grip_world_orientation.uvec.x,
                    g_controller_grip_world_orientation.uvec.y,
                    g_controller_grip_world_orientation.uvec.z,
                    g_controller_grip_world_orientation.fvec.x,
                    g_controller_grip_world_orientation.fvec.y,
                    g_controller_grip_world_orientation.fvec.z);
                xlog::info(
                    "[AFVR][LASER] controller aim OpenXR local pos=({:.4f},{:.4f},{:.4f}) quat=({:.4f},{:.4f},{:.4f},{:.4f})",
                    aim_local.position.x, aim_local.position.y, aim_local.position.z,
                    aim_local.orientation.x, aim_local.orientation.y,
                    aim_local.orientation.z, aim_local.orientation.w);
                xlog::info(
                    "[AFVR][LASER] controller aim RF world pos=({:.4f},{:.4f},{:.4f}) basis R=({:.4f},{:.4f},{:.4f}) U=({:.4f},{:.4f},{:.4f}) F=({:.4f},{:.4f},{:.4f})",
                    g_controller_aim_world_position.x,
                    g_controller_aim_world_position.y,
                    g_controller_aim_world_position.z,
                    g_controller_aim_world_orientation.rvec.x,
                    g_controller_aim_world_orientation.rvec.y,
                    g_controller_aim_world_orientation.rvec.z,
                    g_controller_aim_world_orientation.uvec.x,
                    g_controller_aim_world_orientation.uvec.y,
                    g_controller_aim_world_orientation.uvec.z,
                    g_controller_aim_world_orientation.fvec.x,
                    g_controller_aim_world_orientation.fvec.y,
                    g_controller_aim_world_orientation.fvec.z);
                xlog::info(
                    "[AFVR][LASER] weapon visual root RF world pos=({:.4f},{:.4f},{:.4f}) basis R=({:.4f},{:.4f},{:.4f}) U=({:.4f},{:.4f},{:.4f}) F=({:.4f},{:.4f},{:.4f})",
                    g_weapon_render_position.x, g_weapon_render_position.y,
                    g_weapon_render_position.z,
                    g_weapon_render_orientation.rvec.x,
                    g_weapon_render_orientation.rvec.y,
                    g_weapon_render_orientation.rvec.z,
                    g_weapon_render_orientation.uvec.x,
                    g_weapon_render_orientation.uvec.y,
                    g_weapon_render_orientation.uvec.z,
                    g_weapon_render_orientation.fvec.x,
                    g_weapon_render_orientation.fvec.y,
                    g_weapon_render_orientation.fvec.z);
                xlog::info(
                    "[AFVR][LASER] previous prototype origin (provisional calibrated muzzle) RF world=({:.4f},{:.4f},{:.4f}); existing firing direction RF world=({:.4f},{:.4f},{:.4f})",
                    provisional_muzzle.x, provisional_muzzle.y,
                    provisional_muzzle.z, firing_direction.x,
                    firing_direction.y, firing_direction.z);
                xlog::info(
                    "[AFVR][LASER] HMD center RF world=({:.4f},{:.4f},{:.4f}); grip-minus-HMD=({:.4f},{:.4f},{:.4f}) distance={:.3f}m",
                    g_head_position.x, g_head_position.y, g_head_position.z,
                    grip_from_head.x, grip_from_head.y, grip_from_head.z,
                    grip_from_head.len());
                xlog::info(
                    "[AFVR][LASER] weapon-local emitter pos=({:.4f},{:.4f},{:.4f}) rotation_deg=({:.2f},{:.2f},{:.2f}); resolved RF world pos=({:.4f},{:.4f},{:.4f}) direction=({:.4f},{:.4f},{:.4f})",
                    calibration.laser_position.x,
                    calibration.laser_position.y,
                    calibration.laser_position.z,
                    laser_rotation_degrees.x,
                    laser_rotation_degrees.y,
                    laser_rotation_degrees.z,
                    g_laser_emitter_position.x,
                    g_laser_emitter_position.y,
                    g_laser_emitter_position.z,
                    direction.x, direction.y, direction.z);
                xlog::info(
                    "[AFVR][LASER] shared RF world beam start=({:.4f},{:.4f},{:.4f}) direction=({:.4f},{:.4f},{:.4f}) end=({:.4f},{:.4f},{:.4f}) hit={} length={:.2f}m",
                    g_laser_trace_start.x, g_laser_trace_start.y,
                    g_laser_trace_start.z, direction.x, direction.y,
                    direction.z, g_laser_trace_end.x, g_laser_trace_end.y,
                    g_laser_trace_end.z, g_laser_trace_hit,
                    (g_laser_trace_end - g_laser_trace_start).len());
            }
        }

        void render_laser_sight()
        {
            if (g_laser_trace_frame != rf::frame_count) {
                // Resolve one weapon-emitter world ray and reuse its exact
                // endpoints for both eyes.
                update_laser_trace();
            }
            if (!g_laser_trace_valid) {
                return;
            }

            render_d3d11_world_laser_beam(
                g_laser_trace_start, g_laser_trace_end);
        }

        float conservative_cpu_horizontal_fov(const XrFovf& fov)
        {
            const float horizontal_half_angle = std::max(
                std::abs(fov.angleLeft), std::abs(fov.angleRight));
            const float vertical_half_angle = std::max(
                std::abs(fov.angleDown), std::abs(fov.angleUp));
            const float desktop_aspect = static_cast<float>(rf::gr::screen.clip_width) /
                static_cast<float>(std::max(rf::gr::screen.clip_height, 1));
            const float horizontal_half_angle_for_vertical =
                std::atan(std::tan(vertical_half_angle) * desktop_aspect);
            constexpr float radians_to_degrees = 57.2957795131f;
            return std::min(170.0f, 2.0f * radians_to_degrees *
                std::max(horizontal_half_angle, horizontal_half_angle_for_vertical));
        }

        float apply_stick_deadzone(float value)
        {
            constexpr float deadzone = 0.18f;
            if (std::abs(value) <= deadzone) {
                return 0.0f;
            }
            return std::copysign((std::abs(value) - deadzone) / (1.0f - deadzone), value);
        }

        bool is_singleplayer_death_menu_active()
        {
            if (rf::is_multi) {
                return false;
            }

            const rf::GameState state = rf::gameseq_get_state();
            if (state == rf::GS_END_GAME) {
                return true;
            }

            // RF can keep the interactive singleplayer death overlay inside
            // GS_GAMEPLAY rather than transitioning to GS_GAME_OVER immediately.
            // Wait until the player is fully dead so the preceding stereo death
            // camera/fade remains visible as normal.
            return state == rf::GS_GAMEPLAY && rf::local_player &&
                rf::player_is_dead(rf::local_player);
        }

        bool is_supported_vr_menu_state()
        {
            switch (rf::gameseq_get_state()) {
                case rf::GS_MAIN_MENU:
                case rf::GS_EXTRAS_MENU:
                case rf::GS_SAVE_GAME_MENU:
                case rf::GS_LOAD_GAME_MENU:
                case rf::GS_OPTIONS_MENU:
                case rf::GS_MULTI_MENU:
                case rf::GS_MULTI_LEVEL_DOWNLOAD:
                case rf::GS_MULTI_LIMBO_JUST_JOINED:
                case rf::GS_MULTI_SERVER_LIST:
                case rf::GS_MULTI_SPLITSCREEN:
                case rf::GS_MULTI_CREATE_GAME:
                case rf::GS_MULTI_GETTING_STATE_INFO:
                case rf::GS_MULTI_LIMBO:
                case rf::GS_HELP:
                case rf::GS_GAME_OVER:
                case rf::GS_MESSAGE_LOG:
                    return true;
                default:
                    return false;
            }
        }

        void log_input_edge(const char* action, bool just_pressed)
        {
            if (g_input_debug && just_pressed) {
                xlog::info("[AFVR] Input: {}", action);
            }
        }

        void update_game_frame_limiter(bool bypass)
        {
            if (bypass) {
                // xrWaitFrame is the only pacing wait while the XR session is
                // running. Keep the configured flat-mode maxfps untouched.
                rf::frametime_min = 0.0f;
                if (!g_frame_limiter_bypassed) {
                    g_frame_limiter_bypassed = true;
                    xlog::info("[AFVR] Game-side FPS limiter bypassed for VR");
                }
            }
            else if (g_frame_limiter_bypassed) {
                g_frame_limiter_bypassed = false;
                rf::frametime_min = 1.0f /
                    static_cast<float>(g_alpine_game_config.max_fps);
            }
        }
#endif

        FunHook<bool(rf::Player*)> g_player_fpgun_render_hook{
            0x004AB1A0,
            [](rf::Player* player) {
#ifdef AF_ENABLE_OPENXR
                if (g_openxr && g_openxr->is_session_running() &&
                    player == rf::local_player) {
                    if (!g_rendering_weapon) {
                        if (!g_player_render_reached_logged) {
                            g_player_render_reached_logged = true;
                            xlog::info("[AFVR] player_render reached");
                        }
                        // The desktop camera viewmodel is intentionally absent in
                        // VR. Each eye receives the tracked world-scale submission.
                        return false;
                    }
                }
#endif
                return g_player_fpgun_render_hook.call_target(player);
            },
        };

        bool render_tracked_fpgun(rf::Player* player)
        {
            // The explicit stereo path calls the original through its trampoline,
            // so all state that RF consumes before the body VMesh call must be
            // overridden here rather than in the hook wrapper above.
            const rf::Vector3 saved_position = player->fpgun_data.fpgun_pos;
            const rf::Matrix3 saved_orientation = player->fpgun_data.fpgun_orient;
            const bool saved_show_silencer = player->fpgun_data.show_silencer;
            player->fpgun_data.fpgun_pos = g_weapon_render_position;
            player->fpgun_data.fpgun_orient = g_weapon_render_orientation;

            // Desktop first-person silencers are intentionally omitted from the
            // VR floating-weapon presentation. Restore the gameplay state after
            // rendering so weapon logic is not changed.
            player->fpgun_data.show_silencer = false;

            static bool glock_composition_logged = false;
            if (g_current_weapon_id == 0x03 && !glock_composition_logged) {
                glock_composition_logged = true;
                auto* entity = rf::entity_from_handle(player->entity_handle);
                xlog::info(
                    "[AFVR] Glock FPGUN composition: animated body VMesh '{}'; hand/arm material chunks filtered; separate silencer attachment suppressed in VR (RF requested={}, entity bitfield 0x{:X})",
                    player->weapon_mesh_handle
                        ? rf::vmesh_get_name(player->weapon_mesh_handle) : "<null>",
                    saved_show_silencer,
                    entity ? entity->weapon_silencer_bitfield : 0);
            }

            const bool rendered = g_player_fpgun_render_hook.call_target(player);
            player->fpgun_data.fpgun_pos = saved_position;
            player->fpgun_data.fpgun_orient = saved_orientation;
            player->fpgun_data.show_silencer = saved_show_silencer;
            return rendered;
        }

        CallHook<void(rf::VMesh*, rf::Vector3*, rf::Matrix3*, void*)>
            g_player_fpgun_vmesh_render_hook{
                0x004ABBC8,
                [](rf::VMesh* mesh, rf::Vector3* position,
                    rf::Matrix3* orientation, void* params) {
#ifdef AF_ENABLE_OPENXR
                    if (g_rendering_weapon && g_weapon_pose_valid) {
                        if (!g_weapon_mesh_render_reached_logged) {
                            g_weapon_mesh_render_reached_logged = true;
                            xlog::info("[AFVR] weapon mesh render reached");
                        }
                        g_rendering_fpgun_body = true;
                        g_player_fpgun_vmesh_render_hook.call_target(
                            mesh, &g_weapon_render_position,
                            &g_weapon_render_orientation, params);
                        g_rendering_fpgun_body = false;
                        if (!g_final_weapon_transform_logged) {
                            g_final_weapon_transform_logged = true;
                            xlog::info(
                                "[AFVR] final weapon transform applied position=({:.3f}, {:.3f}, {:.3f})",
                                g_weapon_render_position.x,
                                g_weapon_render_position.y,
                                g_weapon_render_position.z);
                        }
                        if (g_weapon_render_eye >= 0 && g_weapon_render_eye < 2 &&
                            !g_weapon_eye_draw_logged[g_weapon_render_eye]) {
                            g_weapon_eye_draw_logged[g_weapon_render_eye] = true;
                            xlog::info("[AFVR] weapon draw submitted eye={}",
                                g_weapon_render_eye);
                        }
                        return;
                    }
#endif
                    g_player_fpgun_vmesh_render_hook.call_target(
                        mesh, position, orientation, params);
                },
            };

        CallHook<void(rf::VMesh*, rf::Vector3*, rf::Matrix3*, void*)>
            g_player_fpgun_silencer_render_hook{
                0x004ABD89,
                [](rf::VMesh* mesh, rf::Vector3* position,
                    rf::Matrix3* orientation, void* params) {
#ifdef AF_ENABLE_OPENXR
                    // RF renders the Glock silencer as a second VMesh after the
                    // animated weapon body. Masking show_silencer normally skips
                    // this call; blocking the attachment call as well makes the
                    // VR policy unconditional if RF changes that state mid-frame.
                    if (g_rendering_weapon && g_current_weapon_id == 0x03) {
                        static bool silencer_draw_blocked_logged = false;
                        if (!silencer_draw_blocked_logged) {
                            silencer_draw_blocked_logged = true;
                            xlog::info("[AFVR] Glock silencer attachment draw blocked in VR");
                        }
                        return;
                    }
#endif
                    g_player_fpgun_silencer_render_hook.call_target(
                        mesh, position, orientation, params);
                },
            };

        ControlInputInjection vr_control_input_injection(
            [[maybe_unused]] rf::ControlConfig* controls,
            [[maybe_unused]] rf::ControlConfigAction action)
        {
            ControlInputInjection injected{};
#ifdef AF_ENABLE_OPENXR
            if (!g_openxr || !g_openxr->is_session_running() ||
                !rf::local_player || controls != &rf::local_player->settings.controls ||
                g_menu_capture_active || g_gameplay_input_blocked_until_release) {
                return injected;
            }

            const char* diagnostic_name = nullptr;
            if (action == get_af_control(
                    rf::AlpineControlConfigAction::AF_ACTION_FLASHLIGHT)) {
                injected.down = injected.just_pressed = g_flashlight_just_pressed;
                diagnostic_name = "Toggle Headlamp";
            }
            else switch (action) {
                case rf::CC_ACTION_PRIMARY_ATTACK:
                    injected.down = g_primary_fire_pressed;
                    injected.just_pressed = g_primary_fire_just_pressed;
                    diagnostic_name = "Primary Fire";
                    if (injected.down && !g_trigger_action_logged) {
                        g_trigger_action_logged = true;
                        xlog::info("[AFVR] Right trigger entered RF's local primary-attack action");
                    }
                    break;
                case rf::CC_ACTION_SECONDARY_ATTACK:
                    injected.down = g_secondary_fire_pressed;
                    injected.just_pressed = g_secondary_fire_just_pressed;
                    diagnostic_name = "Alternate Fire";
                    break;
                case rf::CC_ACTION_JUMP:
                    injected.down = injected.just_pressed = g_jump_just_pressed;
                    diagnostic_name = "Jump";
                    break;
                case rf::CC_ACTION_RELOAD:
                    injected.down = injected.just_pressed = g_reload_just_pressed;
                    diagnostic_name = "Reload";
                    break;
                case rf::CC_ACTION_CROUCH:
                    // RF distinguishes hold-crouch from toggle-crouch through
                    // PlayerSettings::toggle_crouch. Preserve the entire OpenXR
                    // button hold and supply its rising edge separately so both
                    // stock modes behave like a keyboard binding.
                    injected.down = g_crouch_pressed;
                    injected.just_pressed = g_crouch_just_pressed;
                    diagnostic_name = "Crouch";
                    break;
                case rf::CC_ACTION_HIDE_WEAPON:
                    injected.down = injected.just_pressed = g_holster_just_pressed;
                    diagnostic_name = "Holster Weapon";
                    break;
                case rf::CC_ACTION_USE:
                    // Left squeeze is contextual: near a gun's support point it
                    // acquires the foregrip and must not also activate the world.
                    // Away from the weapon it remains the ordinary Use action.
                    // Once mounted, always preserve Use so a stale support-grip
                    // state can never prevent exiting a turret or vehicle.
                    if (auto* entity = rf::entity_from_handle(
                            rf::local_player->entity_handle);
                        entity && (rf::entity_in_vehicle(entity) ||
                            rf::entity_is_on_turret(entity))) {
                        injected.down = injected.just_pressed =
                            g_left_grip_just_pressed;
                    }
                    else {
                        injected.down = injected.just_pressed =
                            g_left_grip_just_pressed &&
                            !g_two_hand_support_available &&
                            !g_two_hand_weapon_active;
                    }
                    diagnostic_name = "Use";
                    break;
                case rf::CC_ACTION_PREV_WEAPON:
                    injected.down = injected.just_pressed = g_previous_weapon_pulse;
                    diagnostic_name = "Previous Weapon";
                    break;
                case rf::CC_ACTION_NEXT_WEAPON:
                    injected.down = injected.just_pressed = g_next_weapon_pulse;
                    diagnostic_name = "Next Weapon";
                    break;
                default:
                    break;
            }
            if (diagnostic_name) {
                log_input_edge(diagnostic_name, injected.just_pressed);
            }
#endif
            return injected;
        }

        bool vr_control_input_down_injection(
            rf::ControlConfig* controls, rf::ControlConfigAction action)
        {
#ifdef AF_ENABLE_OPENXR
            return g_openxr && g_openxr->is_session_running() &&
                rf::local_player && controls == &rf::local_player->settings.controls &&
                !g_menu_capture_active && !g_gameplay_input_blocked_until_release &&
                action == rf::CC_ACTION_CROUCH && g_crouch_pressed;
#else
            (void)controls;
            (void)action;
            return false;
#endif
        }

        FunHook<int(int&, int&, int&)> g_mouse_get_pos_hook{
            0x0051E450,
            [](int& x, int& y, int& z) {
#ifdef AF_ENABLE_OPENXR
                if (g_menu_capture_active &&
                    (rf::mouse_delta_x != 0 || rf::mouse_delta_y != 0) &&
                    !g_trigger_just_pressed) {
                    // A real mouse movement takes ownership of the menu cursor.
                    // A subsequent controller trigger edge switches ownership
                    // back before RF performs its hit test.
                    g_menu_pointer_using_controller = false;
                }
                if (g_menu_capture_active && g_menu_pointer_valid &&
                    g_menu_pointer_using_controller) {
                    x = g_menu_pointer_x;
                    y = g_menu_pointer_y;
                    z = 0;
                    return 1;
                }
                if (should_block_physical_mouse_input()) {
                    x = rf::gr::screen_width() / 2;
                    y = rf::gr::screen_height() / 2;
                    z = 0;
                    return 1;
                }
#endif
                return g_mouse_get_pos_hook.call_target(x, y, z);
            },
        };

        FunHook<int(int)> g_mouse_was_button_pressed_hook{
            0x0051E5D0,
            [](int button) {
#ifdef AF_ENABLE_OPENXR
                if (g_menu_capture_active && button == 0 &&
                    g_menu_pointer_valid && g_trigger_just_pressed) {
                    return 1;
                }
                if (should_block_physical_mouse_input()) {
                    return 0;
                }
#endif
                return g_mouse_was_button_pressed_hook.call_target(button);
            },
        };

        FunHook<bool(int)> g_mouse_button_down_hook{
            0x0051E530,
            [](int button) {
                if (should_block_physical_mouse_input()) {
                    return false;
                }
                return g_mouse_button_down_hook.call_target(button);
            },
        };

        FunHook<bool(int)> g_mouse_button_double_clicked_hook{
            0x0051E550,
            [](int button) {
                if (should_block_physical_mouse_input()) {
                    return false;
                }
                return g_mouse_button_double_clicked_hook.call_target(button);
            },
        };

        FunHook<int(int)> g_mouse_button_press_count_hook{
            0x0051E590,
            [](int button) {
                if (should_block_physical_mouse_input()) {
                    return 0;
                }
                return g_mouse_button_press_count_hook.call_target(button);
            },
        };

        FunHook<int(int)> g_mouse_button_release_count_hook{
            0x0051E5B0,
            [](int button) {
                if (should_block_physical_mouse_input()) {
                    return 0;
                }
                return g_mouse_button_release_count_hook.call_target(button);
            },
        };

        FunHook<bool(int)> g_mouse_button_released_hook{
            0x0051E600,
            [](int button) {
                if (should_block_physical_mouse_input()) {
                    return false;
                }
                return g_mouse_button_released_hook.call_target(button);
            },
        };

        CallHook<void(rf::Player*)> g_gameplay_hud_do_frame_hook{
            0x00432A18,
            [](rf::Player* player) {
#ifdef AF_ENABLE_OPENXR
                if (g_openxr && g_openxr->is_session_running() &&
                    g_hud_capture_active) {
                    // The normal call still provides a safe fallback on a frame
                    // where OpenXR declined rendering or had no valid views.
                    if (g_hud_rendered_frame != rf::frame_count) {
                        g_gameplay_hud_do_frame_hook.call_target(player);
                    }
                    return;
                }
#endif
                g_gameplay_hud_do_frame_hook.call_target(player);
            },
        };

        CallHook<void(PortalRenderArgument, PortalRenderArgument,
            PortalRenderArgument, PortalRenderArgument)> g_portal_render_hook{
            0x00431FF8,
            [](PortalRenderArgument viewer, PortalRenderArgument room,
                PortalRenderArgument visibility, PortalRenderArgument optional_clip) {
                bool stereo_world_rendered = false;
#ifdef AF_ENABLE_OPENXR
                if (g_openxr && g_openxr->is_session_running() &&
                    !g_rendering_weapon && !g_menu_capture_active &&
                    !g_scope_capture_active) {
                    const rf::Vector3 base_eye_pos = rf::gr::eye_pos;
                    rf::Vector3 reconciled_base_eye_pos = base_eye_pos;
                    const rf::Matrix3 base_eye_matrix = rf::gr::eye_matrix;
                    const float base_horizontal_fov = addr_as_ref<float>(0x0059613C);
                    bool renderer_was_redirected = false;
                    bool roomscale_reconciled = false;
                    std::vector<StereoRoomRenderState> left_eye_room_state;

                    // AFVR TODO: Replace RF's shared desktop CPU frustum with a
                    // conservative stereo union if eye-edge portal culling is visible.
                    const bool frame_submitted =
                        g_openxr->render_frame([&](const OpenXrEyeRenderInfo& eye) {
                            g_latest_center_tracking_position =
                                eye.center_pose.position;
                            g_latest_center_tracking_position_valid = true;
                            if (!g_tracking_origin.valid || g_recenter_requested) {
                                capture_tracking_origin(eye.center_pose);
                            }

                            const rf::Matrix3 vr_view_base =
                                mounted_vr_view_base(base_eye_matrix);
                            if (!roomscale_reconciled) {
                                reconciled_base_eye_pos += reconcile_roomscale_body(
                                    eye.center_pose.position, vr_view_base);
                                roomscale_reconciled = true;
                            }

                            const rf::Vector3 relative_rf_position =
                                relative_tracking_position(eye.view.pose.position);
                            const rf::Matrix3 relative_rf_orientation =
                                relative_tracking_orientation(eye.view.pose.orientation);
                            g_hmd_relative_orientation = relative_rf_orientation;
                            g_hmd_relative_orientation_valid = true;
                            if (!g_head_rotation_logged &&
                                (std::abs(relative_rf_orientation.fvec.x) > 0.01f ||
                                    std::abs(relative_rf_orientation.fvec.y) > 0.01f)) {
                                g_head_rotation_logged = true;
                                xlog::info("[AFVR] Tracked HMD rotation observed in the RF camera path");
                            }
                            g_hmd_relative_yaw = std::atan2(
                                relative_rf_orientation.fvec.x,
                                relative_rf_orientation.fvec.z);
                            g_hmd_relative_forward_y =
                                std::clamp(relative_rf_orientation.fvec.y, -1.0f, 1.0f);

                            const rf::Vector3 relative_center_position =
                                relative_tracking_position(
                                    eye.center_pose.position);
                            if (g_roomscale_collision_frame != rf::frame_count) {
                                g_roomscale_world_correction =
                                    roomscale_collision_correction(
                                        reconciled_base_eye_pos, vr_view_base,
                                        relative_center_position);
                                g_roomscale_collision_frame = rf::frame_count;
                            }

                            rf::Vector3 eye_pos = reconciled_base_eye_pos +
                                transform_direction(vr_view_base, relative_rf_position) +
                                g_roomscale_world_correction;
                            rf::Matrix3 eye_matrix = compose_orientation(
                                vr_view_base, relative_rf_orientation);

                            // center_pose is identical for both eye callbacks.
                            // Cache it in world space for simulation paths that
                            // execute before the next stereo render traversal.
                            g_head_pose_valid = transform_tracked_pose_to_world(
                                eye.center_pose, reconciled_base_eye_pos, vr_view_base,
                                g_head_position, g_head_orientation);
                            update_resolved_player_pose(relative_center_position);

                            update_weapon_pose(reconciled_base_eye_pos, vr_view_base);
                            if (g_debug_weapon_at_hmd && g_weapon_pose_valid) {
                                // Development discriminator: this bypasses the
                                // controller translation and places the weapon
                                // root at a known point in the active XR view.
                                rf::Vector3 hmd_center_position{};
                                rf::Matrix3 hmd_center_orientation{};
                                if (transform_tracked_pose_to_world(
                                        eye.center_pose, reconciled_base_eye_pos,
                                        base_eye_matrix,
                                        hmd_center_position, hmd_center_orientation)) {
                                    g_weapon_render_position = hmd_center_position +
                                        hmd_center_orientation.fvec * 0.5f;
                                    g_weapon_render_orientation = hmd_center_orientation;
                                }
                            }

                            // Refresh RF's CPU view transform and conservative
                            // symmetric frustum before the GPU gets its exact
                            // asymmetric OpenXR projection.
                            rf::gr::setup_3d(eye_matrix, eye_pos,
                                conservative_cpu_horizontal_fov(eye.view.fov), true, true);
                            reset_stereo_render_state();

                            begin_d3d11_eye(
                                eye.render_target_view, eye.depth_stencil_view,
                                eye.width, eye.height,
                                std::tan(eye.view.fov.angleLeft),
                                std::tan(eye.view.fov.angleRight),
                                std::tan(eye.view.fov.angleDown),
                                std::tan(eye.view.fov.angleUp));
                            renderer_was_redirected = true;
                            begin_scene_render_pass(static_cast<int>(eye.eye_index));
                            g_portal_render_hook.call_target(
                                viewer, room, visibility, optional_clip);
                            if (eye.eye_index == 0) {
                                capture_eye_room_render_state(left_eye_room_state);
                            }
                            else if (eye.eye_index == 1) {
                                merge_eye_room_render_state(left_eye_room_state);
                            }
                            if (g_weapon_pose_valid && rf::local_player) {
                                if (auto* entity = rf::entity_from_handle(
                                        rf::local_player->entity_handle);
                                    entity && entity->ai.current_primary_weapon >= 0) {
                                    if (!g_weapon_render_requested_logged) {
                                        g_weapon_render_requested_logged = true;
                                        xlog::info("[AFVR] Generic VR FPGUN rendering active");
                                    }
                                    g_weapon_render_eye = static_cast<int>(eye.eye_index);
                                    g_rendering_weapon = true;
                                    render_tracked_fpgun(rf::local_player);
                                    g_rendering_weapon = false;
                                    g_weapon_render_eye = -1;
                                }
                            }
                            render_laser_sight();
                            end_scene_render_pass(static_cast<int>(eye.eye_index));
                            finish_d3d11_eye();
                            if (eye.eye_index == 1 && should_update_desktop_mirror()) {
                                mirror_d3d11_eye(
                                    eye.shader_resource_view, eye.width, eye.height);
                            }
                        }, [&](const OpenXrHudRenderInfo& hud) {
                            if (!g_hud_capture_active || !rf::local_player) {
                                return;
                            }
                            // Damage indicators and other directional HUD pieces
                            // must see RF's center gameplay camera, not the right
                            // eye globals left by the final stereo traversal.
                            rf::Matrix3 hud_eye_matrix = base_eye_matrix;
                            rf::Vector3 hud_eye_pos = reconciled_base_eye_pos;
                            rf::gr::setup_3d(hud_eye_matrix, hud_eye_pos,
                                base_horizontal_fov, true, true);
                            begin_d3d11_hud(
                                hud.render_target_view, hud.width, hud.height);
                            // This is RF's native singleplayer HUD routine. It is
                            // deliberately invoked once here, outside both eye
                            // passes, and the original later call site is gated.
                            g_gameplay_hud_do_frame_hook.call_target(rf::local_player);
                            if (rf::hud_render_weapon_cycle) {
                                weapon_select_render();
                                static bool weapon_cycle_logged = false;
                                if (!weapon_cycle_logged) {
                                    weapon_cycle_logged = true;
                                    xlog::info("[AFVR] Weapon cycle rendered in the shared HUD quad");
                                }
                            }
                            finish_d3d11_hud();
                            g_hud_rendered_frame = rf::frame_count;
                        });
                    stereo_world_rendered = frame_submitted;

                    rf::Matrix3 restored_eye_matrix = base_eye_matrix;
                    rf::Vector3 restored_eye_pos = reconciled_base_eye_pos;
                    rf::gr::setup_3d(restored_eye_matrix, restored_eye_pos,
                        base_horizontal_fov, true, true);
                    if (renderer_was_redirected) {
                        end_d3d11_vr_frame();
                    }
                }
#endif

                if (!stereo_world_rendered) {
#ifdef AF_ENABLE_OPENXR
                    if (g_openxr && g_openxr->is_session_running() &&
                        g_scope_capture_active && !g_rendering_weapon) {
                        const rf::Vector3 native_eye_position = rf::gr::eye_pos;
                        const rf::Matrix3 native_eye_matrix = rf::gr::eye_matrix;
                        g_scope_base_eye_position = native_eye_position;
                        g_scope_player_view_base =
                            mounted_vr_view_base(native_eye_matrix);
                        g_scope_view_base_valid = true;

                        // RF has already selected the zoomed horizontal FOV for
                        // this native pass. Keep that exact FOV/scope overlay,
                        // but use the last predicted HMD pose so opening the
                        // quad does not snap back to the body-facing camera.
                        if (g_head_pose_valid) {
                            rf::Matrix3 scope_eye_matrix = g_head_orientation;
                            rf::Vector3 scope_eye_position = g_head_position;
                            rf::gr::setup_3d(scope_eye_matrix, scope_eye_position,
                                addr_as_ref<float>(0x0059613C), true, true);
                        }
                    }
#endif
                    g_portal_render_hook.call_target(viewer, room, visibility, optional_clip);
                }
            },
        };

        CallHook<void(PortalRenderArgument, PortalRenderArgument,
            PortalRenderArgument)> g_portal_room_search_hook{
            0x004D4635,
            [](PortalRenderArgument solid, PortalRenderArgument room,
                PortalRenderArgument optional_clip) {
                g_portal_room_search_hook.call_target(
                    solid, room, optional_clip);
                reconcile_stereo_portal_rooms();
            },
        };

        CallHook<bool(const rf::Vector3&, float)> g_room_object_frustum_cull_hook{
            0x004D35FD,
            [](const rf::Vector3& position, float radius) {
                // Portal traversal has already limited these submissions to
                // visible rooms. RF's single-view sphere test can reject a
                // stereo-eye object at this point, so make the eye passes
                // conservative and leave final clipping to D3D11.
                if (g_scene_render_pass == 0 || g_scene_render_pass == 1) {
                    return false;
                }
                return g_room_object_frustum_cull_hook.call_target(position, radius);
            },
        };

        CallHook<void(rf::Player*, rf::ControlInfo*)> g_local_player_controls_hook{
            0x004A615D,
            [](rf::Player* player, rf::ControlInfo* controls) {
                g_local_player_controls_hook.call_target(player, controls);
#ifdef AF_ENABLE_OPENXR
                if (!g_openxr || !g_openxr->is_session_running() ||
                    player != rf::local_player || !controls) {
                    return;
                }

                const auto& input = g_openxr->input_state();
                const float stick_x = apply_stick_deadzone(input.left_thumbstick.x);
                const float stick_y = apply_stick_deadzone(input.left_thumbstick.y);
                auto* entity = rf::entity_from_handle(player->entity_handle);
                const bool in_vehicle = entity && rf::entity_in_vehicle(entity);
                const bool jeep_gunner = entity && rf::entity_is_jeep_gunner(entity);
                const bool mounted_aim = entity &&
                    (rf::entity_is_on_turret(entity) || jeep_gunner);
                const bool vehicle_driver = in_vehicle && !jeep_gunner;
                const bool ladder_movement = entity && !in_vehicle &&
                    !mounted_aim && g_head_pose_valid &&
                    (entity->current_climb_region || rf::entity_is_climbing(entity));
                const float head_forward_horizontal = ladder_movement
                    ? std::sqrt(std::max(0.0f,
                        1.0f - g_hmd_relative_forward_y * g_hmd_relative_forward_y))
                    : 1.0f;
                const bool forward = stick_y > 0.0f;
                const bool backward = stick_y < 0.0f;
                const bool strafe_left = stick_x < 0.0f;
                const bool strafe_right = stick_x > 0.0f;
                log_input_edge("Forward", forward && !g_previous_forward);
                log_input_edge("Backward", backward && !g_previous_backward);
                log_input_edge("Strafe Left", strafe_left && !g_previous_strafe_left);
                log_input_edge("Strafe Right", strafe_right && !g_previous_strafe_right);
                g_previous_forward = forward;
                g_previous_backward = backward;
                g_previous_strafe_left = strafe_left;
                g_previous_strafe_right = strafe_right;
                const float yaw_sin = std::sin(g_hmd_relative_yaw);
                const float yaw_cos = std::cos(g_hmd_relative_yaw);
                if (vehicle_driver) {
                    // The native hook redirects this ControlInfo to the vehicle
                    // for drivers. Keep movement in vehicle-local space so
                    // looking around does not alter throttle or ground steering.
                    controls->move.x += stick_x;
                    controls->move.z += stick_y;
                }
                else if (!mounted_aim) {
                    controls->move.x += stick_x * yaw_cos +
                        stick_y * yaw_sin * head_forward_horizontal;
                    controls->move.z += stick_y * yaw_cos * head_forward_horizontal -
                        stick_x * yaw_sin;
                }
                if (ladder_movement) {
                    controls->move.y += stick_y * g_hmd_relative_forward_y;
                    if (!g_ladder_input_logged && std::abs(stick_y) > 0.0f &&
                        std::abs(g_hmd_relative_forward_y) > 0.05f) {
                        g_ladder_input_logged = true;
                        xlog::info(
                            "[AFVR] Ladder movement follows tracked HMD pitch");
                    }
                }
                if (ladder_movement) {
                    const float movement_length = std::sqrt(
                        controls->move.x * controls->move.x +
                        controls->move.y * controls->move.y +
                        controls->move.z * controls->move.z);
                    if (movement_length > 1.0f) {
                        controls->move.x /= movement_length;
                        controls->move.y /= movement_length;
                        controls->move.z /= movement_length;
                    }
                }
                else {
                    const float horizontal_length = std::sqrt(
                        controls->move.x * controls->move.x +
                        controls->move.z * controls->move.z);
                    if (horizontal_length > 1.0f) {
                        controls->move.x /= horizontal_length;
                        controls->move.z /= horizontal_length;
                    }
                }
                if (!g_movement_input_logged &&
                    (std::abs(stick_x) > 0.0f || std::abs(stick_y) > 0.0f)) {
                    g_movement_input_logged = true;
                    xlog::info("[AFVR] Movement input received");
                }

                const bool cycle_axis_dominant =
                    !in_vehicle && !mounted_aim &&
                    std::abs(input.right_thumbstick.y) >= 0.7f &&
                    std::abs(input.right_thumbstick.y) >=
                        std::abs(input.right_thumbstick.x) + 0.1f;
                const float turn_x = cycle_axis_dominant
                    ? 0.0f : input.right_thumbstick.x;
                if (entity) {
                    if (mounted_aim && g_mounted_aim_active &&
                        g_mounted_native_aim_valid &&
                        g_hmd_relative_orientation_valid) {
                        const rf::Matrix3 desired_aim = compose_orientation(
                            g_mounted_neutral_orientation,
                            g_hmd_relative_orientation);
                        const rf::Vector3& desired_forward = desired_aim.fvec;
                        const float local_right =
                            g_mounted_native_aim_orientation.rvec.dot_prod(desired_forward);
                        const float local_up =
                            g_mounted_native_aim_orientation.uvec.dot_prod(desired_forward);
                        const float local_forward =
                            g_mounted_native_aim_orientation.fvec.dot_prod(desired_forward);
                        const float yaw_error = std::atan2(local_right, local_forward);
                        const float pitch_error = std::atan2(local_up,
                            std::sqrt(std::max(0.0f,
                                local_right * local_right +
                                local_forward * local_forward)));

                        // ControlInfo::rot is RF's normalized native steering
                        // input. A proportional angular error gives mounted aim
                        // fast convergence without depending on headset FPS.
                        constexpr float mounted_aim_gain = 3.0f;
                        controls->rot.y = std::clamp(
                            controls->rot.y + yaw_error * mounted_aim_gain,
                            -1.0f, 1.0f);
                        controls->rot.x = std::clamp(
                            controls->rot.x + pitch_error * mounted_aim_gain,
                            -1.0f, 1.0f);
                        if (!g_mounted_aim_logged &&
                            (std::abs(yaw_error) > 0.01f ||
                                std::abs(pitch_error) > 0.01f)) {
                            g_mounted_aim_logged = true;
                            xlog::info(
                                "[AFVR] Mounted turret is receiving HMD yaw/pitch aim");
                        }
                    }
                    else if (vehicle_driver) {
                        controls->rot.y = std::clamp(
                            controls->rot.y + apply_stick_deadzone(
                                input.right_thumbstick.x),
                            -1.0f, 1.0f);
                        controls->rot.x = std::clamp(
                            controls->rot.x + apply_stick_deadzone(
                                input.right_thumbstick.y),
                            -1.0f, 1.0f);
                        if (!g_vehicle_controls_logged &&
                            (std::abs(input.left_thumbstick.x) > 0.0f ||
                                std::abs(input.left_thumbstick.y) > 0.0f ||
                                std::abs(input.right_thumbstick.x) > 0.0f ||
                                std::abs(input.right_thumbstick.y) > 0.0f)) {
                            g_vehicle_controls_logged = true;
                            xlog::info(
                                "[AFVR] Vehicle controls active: left stick movement, right stick steering");
                        }
                    }

                    // Ordinary smooth/snap body turning applies only while on
                    // foot. Mounted roles consume rotation through ControlInfo.
                    if (in_vehicle || mounted_aim) {
                        g_snap_turn_latched = false;
                        return;
                    }

                    constexpr float pi = 3.14159265359f;
                    constexpr float degrees_to_radians = pi / 180.0f;
                    if (g_game_config.vr_turn_mode == GameConfig::VrTurnMode::smooth) {
                        const float smooth_x = apply_stick_deadzone(turn_x);
                        const float yaw_delta = smooth_x *
                            static_cast<float>(g_game_config.vr_smooth_turn_degrees_per_second.value()) *
                            degrees_to_radians * rf::frametime;
                        if (std::abs(yaw_delta) > 0.000001f) {
                            begin_turn_diagnostic(entity);
                        }
                        entity->control_data.phb.y += yaw_delta;
                        if (entity->control_data.phb.y > pi) {
                            entity->control_data.phb.y -= 2.0f * pi;
                        }
                        else if (entity->control_data.phb.y < -pi) {
                            entity->control_data.phb.y += 2.0f * pi;
                        }
                        g_snap_turn_latched = false;
                        if (!g_smooth_turn_logged && std::abs(smooth_x) > 0.0f) {
                            g_smooth_turn_logged = true;
                            xlog::info("[AFVR] Smooth turn active ({} degrees/second)",
                                g_game_config.vr_smooth_turn_degrees_per_second.value());
                        }
                    }
                    else if (!g_snap_turn_latched && std::abs(turn_x) >= 0.7f) {
                        const float snap_radians =
                            static_cast<float>(g_game_config.vr_snap_turn_degrees.value()) *
                            degrees_to_radians;
                        const float yaw_delta =
                            std::copysign(snap_radians, turn_x);
                        begin_turn_diagnostic(entity);
                        entity->control_data.phb.y += yaw_delta;
                        if (entity->control_data.phb.y > pi) {
                            entity->control_data.phb.y -= 2.0f * pi;
                        }
                        else if (entity->control_data.phb.y < -pi) {
                            entity->control_data.phb.y += 2.0f * pi;
                        }
                        g_snap_turn_latched = true;
                        if (!g_snap_turn_logged) {
                            g_snap_turn_logged = true;
                            xlog::info("[AFVR] Snap turn triggered ({} degrees)",
                                g_game_config.vr_snap_turn_degrees.value());
                        }
                    }
                    else if (g_snap_turn_latched && std::abs(turn_x) <= 0.35f) {
                        g_snap_turn_latched = false;
                    }
                }
#endif
            },
        };

        rf::CmdLineParam& get_vr_command_line_param()
        {
            // A function-local static is required: constructing RF objects from
            // DllMain/global initialization can crash dependency checks.
            static rf::CmdLineParam param{"-vr", "Enable OpenXR VR mode", false};
            return param;
        }

#ifdef AF_ENABLE_OPENXR
        float* calibration_axis(rf::Vector3& value, std::string axis)
        {
            std::ranges::transform(axis, axis.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (axis == "x" || axis == "pitch") {
                return &value.x;
            }
            if (axis == "y" || axis == "yaw") {
                return &value.y;
            }
            if (axis == "z" || axis == "roll") {
                return &value.z;
            }
            rf::console::print("Invalid axis '{}'; use x/y/z or pitch/yaw/roll", axis);
            return nullptr;
        }

        void report_weapon_calibration(int weapon_id,
            const VrWeaponCalibration& calibration)
        {
            constexpr float radians_to_degrees = 57.2957795131f;
            const rf::Vector3 rotation_degrees =
                calibration.pivot_rotation * radians_to_degrees;
            const rf::Vector3 laser_rotation_degrees =
                calibration.laser_rotation * radians_to_degrees;
            const rf::Vector3 muzzle_rotation_degrees =
                calibration.muzzle_rotation * radians_to_degrees;
            rf::console::print("VR weapon {} live calibration:", weapon_id);
            rf::console::print("  move   {:.3f} {:.3f} {:.3f}",
                calibration.grip_position.x, calibration.grip_position.y,
                calibration.grip_position.z);
            rf::console::print("  pivot  {:.3f} {:.3f} {:.3f}",
                calibration.pivot_position.x, calibration.pivot_position.y,
                calibration.pivot_position.z);
            rf::console::print("  rotate {:.1f} {:.1f} {:.1f} degrees",
                rotation_degrees.x, rotation_degrees.y, rotation_degrees.z);
            rf::console::print("  muzzle {:.3f} {:.3f} {:.3f}",
                calibration.muzzle_position.x, calibration.muzzle_position.y,
                calibration.muzzle_position.z);
            rf::console::print("  muzzle rotation {:.1f} {:.1f} {:.1f} degrees",
                muzzle_rotation_degrees.x, muzzle_rotation_degrees.y,
                muzzle_rotation_degrees.z);
            rf::console::print("  laser  {:.3f} {:.3f} {:.3f}",
                calibration.laser_position.x, calibration.laser_position.y,
                calibration.laser_position.z);
            rf::console::print("  laser rotation {:.1f} {:.1f} {:.1f} degrees",
                laser_rotation_degrees.x, laser_rotation_degrees.y,
                laser_rotation_degrees.z);
            xlog::info(
                "[AFVR] CALIBRATION weapon={} move=({:.3f},{:.3f},{:.3f}) pivot=({:.3f},{:.3f},{:.3f}) rotation_deg=({:.1f},{:.1f},{:.1f}) muzzle=({:.3f},{:.3f},{:.3f}) muzzle_rotation_deg=({:.1f},{:.1f},{:.1f}) laser=({:.3f},{:.3f},{:.3f}) laser_rotation_deg=({:.1f},{:.1f},{:.1f})",
                weapon_id,
                calibration.grip_position.x, calibration.grip_position.y,
                calibration.grip_position.z,
                calibration.pivot_position.x, calibration.pivot_position.y,
                calibration.pivot_position.z,
                rotation_degrees.x, rotation_degrees.y, rotation_degrees.z,
                calibration.muzzle_position.x, calibration.muzzle_position.y,
                calibration.muzzle_position.z,
                muzzle_rotation_degrees.x, muzzle_rotation_degrees.y,
                muzzle_rotation_degrees.z,
                calibration.laser_position.x, calibration.laser_position.y,
                calibration.laser_position.z,
                laser_rotation_degrees.x, laser_rotation_degrees.y,
                laser_rotation_degrees.z);
        }

        void nudge_weapon_vector(rf::Vector3 VrWeaponCalibration::*member,
            std::string axis, float delta)
        {
            auto* calibration = editable_weapon_calibration();
            if (!calibration) {
                return;
            }
            auto* component = calibration_axis(calibration->*member, std::move(axis));
            if (!component) {
                return;
            }
            *component += delta;
            report_weapon_calibration(calibration->weapon_id, *calibration);
        }

        void report_two_hand_calibration(int weapon_id,
            const VrTwoHandCalibration& calibration)
        {
            xlog::info(
                "[AFVR] TWO_HAND_CALIBRATION weapon={} support_position=({:.4f},{:.4f},{:.4f}) capture_radius={:.3f} support_fraction={:.3f} maximum_hand_line_weight={:.3f}",
                weapon_id,
                calibration.support_position.x,
                calibration.support_position.y,
                calibration.support_position.z,
                calibration.capture_radius,
                calibration.support_fraction,
                calibration.maximum_hand_line_weight);
        }

        bool capture_two_hand_calibration_sample()
        {
            if (!g_two_hand_calibration_active || !g_openxr) {
                return false;
            }
            const int weapon_id = current_local_weapon_id();
            if (weapon_id != g_two_hand_calibration_weapon_id) {
                rf::console::print(
                    "Two-hand calibration cancelled because the equipped weapon changed");
                g_two_hand_calibration_active = false;
                g_two_hand_calibration_weapon_id = -1;
                return false;
            }

            const auto& input = g_openxr->input_state();
            constexpr size_t left_hand = 0;
            constexpr size_t right_hand = 1;
            const bool poses_valid =
                input.grip_pose_valid[left_hand] &&
                input.grip_pose_valid[right_hand] &&
                input.aim_pose_valid[left_hand] &&
                input.aim_pose_valid[right_hand] &&
                g_left_controller_grip_world_valid &&
                g_left_controller_aim_world_valid &&
                g_controller_grip_world_valid &&
                g_controller_aim_world_valid &&
                g_weapon_pose_valid && g_weapon_aim_pose_valid;
            if (!poses_valid) {
                rf::console::print(
                    "Two-hand calibration needs valid tracking for both controllers; keep holding the pose and press right trigger again");
                xlog::warn(
                    "[AFVR] TWO_HAND_CALIBRATION weapon={} confirmation rejected: controller pose unavailable",
                    weapon_id);
                return false;
            }

            auto* calibration = editable_two_hand_calibration(weapon_id);
            if (!calibration) {
                rf::console::print(
                    "Weapon {} does not support two-hand calibration", weapon_id);
                g_two_hand_calibration_active = false;
                g_two_hand_calibration_weapon_id = -1;
                return false;
            }

            const rf::Vector3 support_position = direction_in_basis(
                g_weapon_render_orientation,
                g_left_controller_grip_world_position - g_weapon_render_position);
            const rf::Vector3 left_from_right_aim = direction_in_basis(
                g_controller_aim_world_orientation,
                g_left_controller_grip_world_position -
                    g_controller_grip_world_position);
            const auto& weapon = weapon_calibration(weapon_id);
            const rf::Vector3 authored_support_axis =
                weapon.muzzle_position - weapon.pivot_position;
            const float authored_axis_length_squared =
                authored_support_axis.dot_prod(authored_support_axis);
            float projected_fraction = calibration->support_fraction;
            float perpendicular_distance = 0.0f;
            if (authored_axis_length_squared > 0.000001f) {
                const rf::Vector3 from_pivot =
                    support_position - weapon.pivot_position;
                projected_fraction = from_pivot.dot_prod(authored_support_axis) /
                    authored_axis_length_squared;
                const rf::Vector3 projected = weapon.pivot_position +
                    authored_support_axis * projected_fraction;
                perpendicular_distance = (support_position - projected).len();
            }

            calibration->support_position = support_position;
            calibration->use_support_position = true;
            report_two_hand_calibration(weapon_id, *calibration);

            const XrPosef& left_grip = input.grip_poses[left_hand];
            const XrPosef& right_grip = input.grip_poses[right_hand];
            const XrPosef& left_aim = input.aim_poses[left_hand];
            const XrPosef& right_aim = input.aim_poses[right_hand];
            xlog::info(
                "[AFVR] TWO_HAND_SAMPLE weapon={} left_grip_tracking=({:.4f},{:.4f},{:.4f}) right_grip_tracking=({:.4f},{:.4f},{:.4f}) left_aim_tracking=({:.4f},{:.4f},{:.4f}) right_aim_tracking=({:.4f},{:.4f},{:.4f})",
                weapon_id,
                left_grip.position.x, left_grip.position.y, left_grip.position.z,
                right_grip.position.x, right_grip.position.y, right_grip.position.z,
                left_aim.position.x, left_aim.position.y, left_aim.position.z,
                right_aim.position.x, right_aim.position.y, right_aim.position.z);
            xlog::info(
                "[AFVR] TWO_HAND_SAMPLE weapon={} left_grip_orientation=({:.5f},{:.5f},{:.5f},{:.5f}) right_grip_orientation=({:.5f},{:.5f},{:.5f},{:.5f}) left_aim_orientation=({:.5f},{:.5f},{:.5f},{:.5f}) right_aim_orientation=({:.5f},{:.5f},{:.5f},{:.5f})",
                weapon_id,
                left_grip.orientation.x, left_grip.orientation.y,
                left_grip.orientation.z, left_grip.orientation.w,
                right_grip.orientation.x, right_grip.orientation.y,
                right_grip.orientation.z, right_grip.orientation.w,
                left_aim.orientation.x, left_aim.orientation.y,
                left_aim.orientation.z, left_aim.orientation.w,
                right_aim.orientation.x, right_aim.orientation.y,
                right_aim.orientation.z, right_aim.orientation.w);
            xlog::info(
                "[AFVR] TWO_HAND_SAMPLE weapon={} left_from_right_aim=({:.4f},{:.4f},{:.4f}) controller_separation={:.4f} projected_support_fraction={:.4f} perpendicular_offset={:.4f}",
                weapon_id,
                left_from_right_aim.x, left_from_right_aim.y,
                left_from_right_aim.z,
                (g_left_controller_grip_world_position -
                    g_controller_grip_world_position).len(),
                projected_fraction, perpendicular_distance);

            rf::console::print(
                "Captured two-hand calibration for weapon {}. The support point is active for this session and stored in AlpineFaction.log",
                weapon_id);
            g_two_hand_calibration_active = false;
            g_two_hand_calibration_weapon_id = -1;
            g_two_hand_weapon_active = false;
            g_two_hand_weapon_id = -1;
            return true;
        }
#endif

        ConsoleCommand2 g_vr_recenter_cmd{
            "vr_recenter",
            []() {
                recenter_tracking();
                rf::console::print("VR tracking recenter requested");
            },
            "Recenter OpenXR yaw and local tracking position",
        };

        ConsoleCommand2 g_vr_weapon_debug_hmd_cmd{
            "vr_weapon_debug_hmd",
            []() {
                g_debug_weapon_at_hmd = !g_debug_weapon_at_hmd;
                rf::console::print(
                    "VR weapon 0.5 m HMD debug placement is {}",
                    g_debug_weapon_at_hmd ? "enabled" : "disabled");
            },
            "Toggle a development-only weapon placement 0.5 m ahead of the HMD",
        };

        ConsoleCommand2 g_vr_input_debug_cmd{
            "vr_input_debug",
            []() {
                g_input_debug = !g_input_debug;
                rf::console::print("VR input diagnostics are {}",
                    g_input_debug ? "enabled" : "disabled");
            },
            "Toggle edge-triggered VR gameplay input diagnostics",
        };

#ifdef AF_ENABLE_OPENXR
        ConsoleCommand2 g_vr_weapon_move_cmd{
            "vr_weapon_move",
            [](std::string axis, float delta) {
                nudge_weapon_vector(&VrWeaponCalibration::grip_position,
                    std::move(axis), delta);
            },
            "Move the equipped VR weapon in aim-local metres",
            "vr_weapon_move <x|y|z> <delta>; x=right, y=up, z=forward",
        };

        ConsoleCommand2 g_vr_weapon_pivot_cmd{
            "vr_weapon_pivot",
            [](std::string axis, float delta) {
                nudge_weapon_vector(&VrWeaponCalibration::pivot_position,
                    std::move(axis), delta);
            },
            "Adjust the equipped weapon's authored grip pivot in metres",
            "vr_weapon_pivot <x|y|z> <delta>",
        };

        ConsoleCommand2 g_vr_weapon_rotate_cmd{
            "vr_weapon_rotate",
            [](std::string axis, float degrees) {
                constexpr float degrees_to_radians = 0.0174532925199f;
                nudge_weapon_vector(&VrWeaponCalibration::pivot_rotation,
                    std::move(axis), degrees * degrees_to_radians);
            },
            "Rotate the equipped VR weapon around its calibrated pivot",
            "vr_weapon_rotate <pitch|yaw|roll> <degrees>",
        };

        ConsoleCommand2 g_vr_muzzle_move_cmd{
            "vr_muzzle_move",
            [](std::string axis, float delta) {
                nudge_weapon_vector(&VrWeaponCalibration::muzzle_position,
                    std::move(axis), delta);
            },
            "Move the equipped weapon's firing origin in weapon-local metres",
            "vr_muzzle_move <x|y|z> <delta>; x=right, y=up, z=forward",
        };

        ConsoleCommand2 g_vr_muzzle_rotate_cmd{
            "vr_muzzle_rotate",
            [](std::string axis, float degrees) {
                constexpr float degrees_to_radians = 0.0174532925199f;
                nudge_weapon_vector(&VrWeaponCalibration::muzzle_rotation,
                    std::move(axis), degrees * degrees_to_radians);
            },
            "Rotate the equipped weapon's firing direction in aim-local space",
            "vr_muzzle_rotate <pitch|yaw|roll> <degrees>",
        };

        ConsoleCommand2 g_vr_laser_move_cmd{
            "vr_laser_move",
            [](std::string axis, float delta) {
                nudge_weapon_vector(&VrWeaponCalibration::laser_position,
                    std::move(axis), delta);
            },
            "Move the equipped gun's laser emitter in weapon-local metres",
            "vr_laser_move <x|y|z> <delta>; x=right, y=up, z=forward",
        };

        ConsoleCommand2 g_vr_laser_rotate_cmd{
            "vr_laser_rotate",
            [](std::string axis, float degrees) {
                constexpr float degrees_to_radians = 0.0174532925199f;
                nudge_weapon_vector(&VrWeaponCalibration::laser_rotation,
                    std::move(axis), degrees * degrees_to_radians);
            },
            "Rotate the equipped gun's weapon-local laser emitter",
            "vr_laser_rotate <pitch|yaw|roll> <degrees>",
        };

        ConsoleCommand2 g_vr_fire_origin_audit_cmd{
            "vr_fire_origin_audit",
            []() {
                audit_weapon_fire_origin(true);
            },
            "Compare the independent gameplay fire point with the visual laser emitter and trace two metres forward",
        };

        ConsoleCommand2 g_vr_aim_yaw_cmd{
            "vr_aim_yaw",
            [](float degrees) {
                g_weapon_aim_yaw_correction_degrees =
                    std::clamp(degrees, -10.0f, 10.0f);
                rf::console::print(
                    "VR shared weapon aim yaw correction: {:.2f} degrees",
                    g_weapon_aim_yaw_correction_degrees);
                xlog::info(
                    "[AFVR] Shared weapon aim yaw correction set to {:.2f} degrees",
                    g_weapon_aim_yaw_correction_degrees);
            },
            "Set the shared horizontal VR weapon aim correction in degrees",
            "vr_aim_yaw <degrees>; negative moves shots left, positive moves shots right",
        };

        ConsoleCommand2 g_vr_two_hand_calibrate_cmd{
            "vr_two_hand_calibrate",
            []() {
                const int weapon_id = current_local_weapon_id();
                if (!base_two_hand_calibration(weapon_id)) {
                    rf::console::print(
                        "The equipped weapon does not support two-hand calibration");
                    return;
                }
                g_two_hand_calibration_active = true;
                g_two_hand_calibration_weapon_id = weapon_id;
                g_two_hand_weapon_active = false;
                g_two_hand_weapon_id = -1;
                rf::console::print(
                    "Two-hand calibration armed for weapon {}: hold both controllers in your natural firing pose, then press the right trigger to capture",
                    weapon_id);
                xlog::info(
                    "[AFVR] Two-hand calibration armed for weapon {}; awaiting right-trigger confirmation",
                    weapon_id);
            },
            "Capture both controller poses for the equipped gun's two-hand support point",
        };

        ConsoleCommand2 g_vr_weapon_calibration_cmd{
            "vr_weapon_calibration",
            []() {
                const int weapon_id = current_local_weapon_id();
                if (weapon_id < 0) {
                    rf::console::print("No local weapon is equipped");
                    return;
                }
                report_weapon_calibration(weapon_id,
                    weapon_calibration(weapon_id));
            },
            "Print the equipped weapon's live VR calibration to console and log",
        };

        ConsoleCommand2 g_vr_weapon_calibration_all_cmd{
            "vr_weapon_calibration_all",
            []() {
                int reported = 0;
                int two_hand_reported = 0;
                xlog::info("[AFVR] CALIBRATION SNAPSHOT BEGIN");
                for (int weapon_id = 0;
                    weapon_id < static_cast<int>(g_live_weapon_calibrations.size());
                    ++weapon_id) {
                    if (!g_live_weapon_calibration_active[weapon_id]) {
                        continue;
                    }
                    report_weapon_calibration(weapon_id,
                        g_live_weapon_calibrations[weapon_id]);
                    ++reported;
                }
                xlog::info("[AFVR] TWO_HAND_CALIBRATION SNAPSHOT BEGIN");
                for (int weapon_id = 0;
                    weapon_id < static_cast<int>(g_live_two_hand_calibrations.size());
                    ++weapon_id) {
                    if (!g_live_two_hand_calibration_active[weapon_id]) {
                        continue;
                    }
                    report_two_hand_calibration(weapon_id,
                        g_live_two_hand_calibrations[weapon_id]);
                    ++two_hand_reported;
                }
                xlog::info(
                    "[AFVR] TWO_HAND_CALIBRATION SNAPSHOT END ({} weapons)",
                    two_hand_reported);
                xlog::info(
                    "[AFVR] CALIBRATION SNAPSHOT END ({} weapon transforms, {} two-hand supports)",
                    reported, two_hand_reported);
                rf::console::print(
                    "Stored {} weapon and {} two-hand live calibrations in AlpineFaction.log",
                    reported, two_hand_reported);
            },
            "Store every changed weapon's final VR calibration in AlpineFaction.log",
        };

        ConsoleCommand2 g_vr_weapon_reset_cmd{
            "vr_weapon_reset",
            []() {
                const int weapon_id = current_local_weapon_id();
                if (weapon_id < 0 ||
                    weapon_id >= static_cast<int>(g_live_weapon_calibration_active.size())) {
                    rf::console::print("No calibratable local weapon is equipped");
                    return;
                }
                g_live_weapon_calibration_active[weapon_id] = false;
                rf::console::print("Reset VR weapon {} to its built-in calibration", weapon_id);
                report_weapon_calibration(weapon_id,
                    base_weapon_calibration(weapon_id));
            },
            "Reset the equipped weapon's live VR calibration",
        };
#endif

    }

    void register_command_line()
    {
        get_vr_command_line_param();
    }

    void install_render_hook()
    {
        // Alpine already owns 0x0043D4F0. Compose through its input-filter
        // registry instead of installing the prototype's competing FunHook.
        control_input_filter_add_press_injection(&vr_control_input_injection);
        control_input_filter_add_down_injection(&vr_control_input_down_injection);
        g_mouse_get_pos_hook.install();
        g_mouse_was_button_pressed_hook.install();
        g_mouse_button_down_hook.install();
        g_mouse_button_double_clicked_hook.install();
        g_mouse_button_press_count_hook.install();
        g_mouse_button_release_count_hook.install();
        g_mouse_button_released_hook.install();
        g_player_fpgun_vmesh_render_hook.install();
        g_player_fpgun_silencer_render_hook.install();
        g_player_fpgun_render_hook.install();
        g_local_player_controls_hook.install();
        g_room_object_frustum_cull_hook.install();
        g_portal_room_search_hook.install();
        g_portal_render_hook.install();
        g_gameplay_hud_do_frame_hook.install();
    }

    void register_console_commands()
    {
        g_vr_recenter_cmd.register_cmd();
        g_vr_weapon_debug_hmd_cmd.register_cmd();
        g_vr_input_debug_cmd.register_cmd();
#ifdef AF_ENABLE_OPENXR
        g_vr_weapon_move_cmd.register_cmd();
        g_vr_weapon_pivot_cmd.register_cmd();
        g_vr_weapon_rotate_cmd.register_cmd();
        g_vr_muzzle_move_cmd.register_cmd();
        g_vr_muzzle_rotate_cmd.register_cmd();
        g_vr_laser_move_cmd.register_cmd();
        g_vr_laser_rotate_cmd.register_cmd();
        g_vr_fire_origin_audit_cmd.register_cmd();
        g_vr_aim_yaw_cmd.register_cmd();
        g_vr_two_hand_calibrate_cmd.register_cmd();
        g_vr_weapon_calibration_cmd.register_cmd();
        g_vr_weapon_calibration_all_cmd.register_cmd();
        g_vr_weapon_reset_cmd.register_cmd();
#endif
    }

    void timing_game_frame_begin()
    {
#ifdef AF_ENABLE_OPENXR
        const auto now = TimingDiagnostics::Clock::now();
        if (!g_openxr || !g_openxr->is_session_running()) {
            g_timing.reset(now);
            return;
        }
        if (g_timing.window_start == TimingDiagnostics::Clock::time_point{}) {
            g_timing.reset(now);
        }
        g_timing.game_frame_start = now;
        ++g_timing.game_frames;
#endif
    }

    void timing_game_frame_end()
    {
#ifdef AF_ENABLE_OPENXR
        const auto now = TimingDiagnostics::Clock::now();
        if (g_timing.game_frame_start != TimingDiagnostics::Clock::time_point{}) {
            g_timing.game_cpu_ms += std::chrono::duration<double, std::milli>(
                now - g_timing.game_frame_start).count();
            g_timing.rf_frametime_ms +=
                static_cast<double>(rf::frametime) * 1000.0;
            g_timing.game_frame_start = {};
        }
        maybe_log_timing(now);
#endif
    }

    void timing_note_xr_wait(double wait_ms, double return_interval_ms,
        double predicted_interval_ms, double runtime_target_hz)
    {
#ifdef AF_ENABLE_OPENXR
        ++g_timing.xr_waits;
        g_timing.xr_wait_ms += wait_ms;
        if (return_interval_ms > 0.0) {
            ++g_timing.xr_wait_return_intervals;
            g_timing.xr_wait_return_interval_ms += return_interval_ms;
        }
        if (predicted_interval_ms > 0.0) {
            ++g_timing.xr_predicted_intervals;
            g_timing.xr_predicted_interval_ms += predicted_interval_ms;
        }
        if (runtime_target_hz > 0.0) {
            g_timing.runtime_target_hz = runtime_target_hz;
        }
#else
        (void)wait_ms;
        (void)return_interval_ms;
        (void)predicted_interval_ms;
        (void)runtime_target_hz;
#endif
    }

    void timing_note_phase(TimingPhase phase, double duration_ms)
    {
#ifdef AF_ENABLE_OPENXR
        const size_t index = static_cast<size_t>(phase);
        if (index < g_timing.phase_count) {
            ++g_timing.phase_samples[index];
            g_timing.phase_ms[index] += duration_ms;
        }
#else
        (void)phase;
        (void)duration_ms;
#endif
    }

    void timing_note_xr_submission()
    {
#ifdef AF_ENABLE_OPENXR
        ++g_timing.xr_submissions;
#endif
    }

    void timing_note_desktop_present(double duration_ms)
    {
#ifdef AF_ENABLE_OPENXR
        ++g_timing.desktop_presents;
        g_timing.desktop_present_ms += duration_ms;
#else
        (void)duration_ms;
#endif
    }

    void after_game_init()
    {
        g_requested = get_vr_command_line_param().found();
#ifdef AF_ENABLE_OPENXR
        xlog::info("[AFVR] AlpineFaction VR game patch loaded");
        xlog::info("[AFVR] Effective game command line: {}", GetCommandLineA());
        xlog::info("[AFVR] -vr detected: {}", g_requested ? "yes" : "no");
        const char* renderer_name = "unknown";
        switch (g_game_config.renderer.value()) {
            case GameConfig::Renderer::d3d8: renderer_name = "Direct3D 8"; break;
            case GameConfig::Renderer::d3d9: renderer_name = "Direct3D 9"; break;
            case GameConfig::Renderer::d3d11: renderer_name = "Direct3D 11"; break;
        }
        xlog::info("[AFVR] Active renderer: {}", renderer_name);
#endif
        if (!g_requested) {
            return;
        }

        xlog::info("[AFVR] VR mode requested");
        xlog::info("[AFVR] Client multiplayer is enabled on a best-effort basis");

        if (rf::is_dedicated_server) {
            xlog::warn("[AFVR] Dedicated servers are not supported in VR mode; continuing without VR");
            return;
        }

        if (g_game_config.renderer != GameConfig::Renderer::d3d11) {
            xlog::warn("[AFVR] AlpineFaction VR requires the D3D11 renderer; VR initialization skipped");
            return;
        }

#ifdef AF_ENABLE_OPENXR
        auto binding = get_d3d11_renderer_binding();
        if (!binding) {
            xlog::error("[AFVR] The Alpine Faction D3D11 device/context is unavailable; VR initialization skipped");
            return;
        }

        auto* openxr = new OpenXrContext;
        if (!openxr->initialize(binding.device)) {
            delete openxr;
            xlog::warn("[AFVR] OpenXR initialization failed; continuing in flat mode");
            return;
        }

        g_openxr = openxr;
        xlog::info("[AFVR] OpenXR bootstrap initialized");
#else
        xlog::warn("[AFVR] This build was compiled without OpenXR support; continuing in flat mode");
#endif
    }

    void update()
    {
#ifdef AF_ENABLE_OPENXR
        if (g_openxr && !g_openxr->is_session_running()) {
            g_tracking_origin.valid = false;
            g_latest_center_tracking_position_valid = false;
            g_recenter_requested = true;
            g_head_rotation_logged = false;
            g_head_pose_valid = false;
            g_hmd_relative_orientation_valid = false;
            g_resolved_player_pose = {};
            g_turn_diagnostic = {};
            g_visual_body_yaw_offset = 0.0f;
            g_body_follow_frame = -1;
            g_resolved_pose_logged = false;
            g_roomscale_world_correction = {};
            g_roomscale_collision_frame = -1;
            g_roomscale_reconciliation_logged = false;
            clear_mounted_aim_state();
        }
        if (g_openxr && !g_openxr->poll_events()) {
            xlog::warn("[AFVR] OpenXR requested shutdown; returning to flat mode");
            delete g_openxr;
            g_openxr = nullptr;
        }
        if (g_openxr && rf::is_multi && !g_multiplayer_best_effort_logged) {
            g_multiplayer_best_effort_logged = true;
            xlog::warn(
                "[AFVR] Multiplayer VR is experimental and continues without compatibility guarantees");
        }
        update_game_frame_limiter(
            g_openxr && g_openxr->is_session_running());
        if (g_openxr) {
            if (g_openxr->is_session_running()) {
                update_local_attachment_transition();
                (void)g_openxr->wait_frame();
            }
            const auto input_sync_start = std::chrono::steady_clock::now();
            (void)g_openxr->sync_actions();
            timing_note_phase(TimingPhase::input_sync,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - input_sync_start).count());
            const rf::GameState game_state = rf::gameseq_get_state();
            const bool menu_was_active = g_menu_capture_active;
            g_singleplayer_death_menu_active =
                is_singleplayer_death_menu_active();
            g_menu_capture_active = g_openxr->is_session_running() &&
                (g_singleplayer_death_menu_active || is_supported_vr_menu_state());
            if (g_menu_capture_active != menu_was_active) {
                g_roomscale_world_correction = {};
                g_roomscale_collision_frame = -1;
                xlog::info(
                    "[AFVR] Menu {} without changing the tracking/recenter reference",
                    g_menu_capture_active ? "open" : "close");
            }
            if (g_menu_capture_active && !menu_was_active) {
                g_menu_pointer_using_controller = true;
                if (g_singleplayer_death_menu_active) {
                    xlog::info("[AFVR] Singleplayer death menu captured in the OpenXR quad");
                }
            }
            if (g_menu_capture_active) {
                // GS_GAMEPLAY normally owns a centered relative mouse. The
                // singleplayer death overlay is an interactive 2D menu even
                // when it has not changed game-sequence state, so release and
                // show the cursor just as RF does for ordinary menu states.
                if (rf::keep_mouse_centered) {
                    rf::mouse_keep_centered_disable();
                }
                rf::mouse_set_visible(true);
            }
            if (g_menu_capture_active) {
                g_two_hand_support_available = false;
                g_two_hand_weapon_active = false;
                g_two_hand_weapon_id = -1;
            }
            const bool scope_was_active = g_scope_capture_active;
            g_scope_capture_active = g_openxr->is_session_running() &&
                game_state == rf::GS_GAMEPLAY &&
                !g_menu_capture_active && native_weapon_scope_is_active();
            if (g_scope_capture_active != scope_was_active) {
                g_scope_view_base_valid = false;
                xlog::info("[AFVR] Native rifle zoom switched to {} rendering",
                    g_scope_capture_active ? "native OpenXR quad" : "stereo VR");
            }
            g_openxr->set_scope_active(g_scope_capture_active);
            g_hud_capture_active = g_openxr->is_session_running() &&
                game_state == rf::GS_GAMEPLAY &&
                !g_menu_capture_active && !g_scope_capture_active;
            g_openxr->set_hud_active(g_hud_capture_active);
            if (g_menu_capture_active && game_state != g_last_menu_state) {
                g_openxr->set_menu_active(false);
                g_openxr->set_menu_active(true);
            }
            else {
                g_openxr->set_menu_active(g_menu_capture_active);
            }
            g_last_menu_state = game_state;
            if (!g_menu_capture_active) {
                g_menu_pointer_valid = false;
            }
            const auto& input = g_openxr->input_state();
            g_shake_reload_just_pressed = false;
            if (g_game_config.vr_shake_reload &&
                g_openxr->is_session_running() &&
                !g_menu_capture_active && input.grip_pose_valid[1]) {
                const float current_y = input.grip_poses[1].position.y;
                const float frame_seconds = std::max(rf::frametime, 0.0001f);
                const float downward_threshold =
                    static_cast<float>(g_game_config.vr_shake_reload_threshold_cm_s) / 100.0f;
                if (g_previous_right_grip_y_valid) {
                    const float velocity =
                        (current_y - g_previous_right_grip_y) / frame_seconds;
                    g_shake_reload_cooldown = std::max(
                        0.0f, g_shake_reload_cooldown - frame_seconds);
                    if (g_shake_reload_cooldown <= 0.0f &&
                        velocity <= -downward_threshold &&
                        g_previous_right_grip_velocity > -downward_threshold * 0.5f) {
                        g_shake_reload_just_pressed = true;
                        g_shake_reload_cooldown = 0.30f;
                        xlog::info(
                            "[AFVR] Shake reload triggered by right controller downward motion");
                    }
                    g_previous_right_grip_velocity = velocity;
                }
                else {
                    g_previous_right_grip_velocity = 0.0f;
                }
                g_previous_right_grip_y = current_y;
                g_previous_right_grip_y_valid = true;
            }
            else {
                g_previous_right_grip_y_valid = false;
                g_previous_right_grip_velocity = 0.0f;
                g_shake_reload_cooldown = 0.0f;
            }
            const bool steamvr_menu_chord_buttons =
                g_openxr->is_steamvr_runtime() &&
                input.reload && input.crouch;
            bool steamvr_menu_chord_held = false;
            if (steamvr_menu_chord_buttons) {
                if (!g_steamvr_menu_chord_timing) {
                    g_steamvr_menu_chord_timing = true;
                    g_steamvr_menu_chord_started =
                        std::chrono::steady_clock::now();
                }
                constexpr auto menu_chord_hold =
                    std::chrono::milliseconds(600);
                steamvr_menu_chord_held =
                    std::chrono::steady_clock::now() -
                        g_steamvr_menu_chord_started >= menu_chord_hold;
            }
            else {
                g_steamvr_menu_chord_timing = false;
                g_steamvr_menu_chord_started = {};
            }
            const bool trigger_pressed =
                g_openxr->is_session_running() &&
                input.right_trigger >= 0.55f;
            const bool index_alt_trigger_pressed =
                g_openxr->is_session_running() &&
                input.index_profile_active && input.left_trigger >= 0.55f;
            g_trigger_just_pressed =
                trigger_pressed && !g_previous_trigger_pressed;
            if (g_menu_capture_active && g_trigger_just_pressed) {
                g_menu_pointer_using_controller = true;
            }
            g_trigger_pressed = trigger_pressed;
            g_previous_trigger_pressed = trigger_pressed;

            g_reload_pressed = g_openxr->is_session_running() && input.reload &&
                !steamvr_menu_chord_buttons;
            g_jump_pressed = g_openxr->is_session_running() && input.jump;
            g_crouch_pressed = g_openxr->is_session_running() && input.crouch &&
                !steamvr_menu_chord_buttons;
            g_holster_pressed =
                g_openxr->is_session_running() && input.left_thumbstick_click;
            g_flashlight_pressed =
                g_openxr->is_session_running() && input.flashlight;
            g_menu_button_pressed = g_openxr->is_session_running() &&
                (g_openxr->is_steamvr_runtime()
                    ? steamvr_menu_chord_held : input.menu);
            const bool laser_toggle_pressed =
                g_openxr->is_session_running() && input.right_thumbstick_click;
            if (laser_toggle_pressed && !g_previous_laser_toggle_pressed) {
                g_laser_sight_enabled = !g_laser_sight_enabled;
                g_laser_trace_frame = -1;
                if (g_laser_sight_enabled) {
                    g_laser_coordinate_audit_logged = false;
                }
                xlog::info("[AFVR] Laser sight {} (right thumbstick click)",
                    g_laser_sight_enabled ? "enabled" : "disabled");
            }
            g_previous_laser_toggle_pressed = laser_toggle_pressed;
            g_left_grip_pressed = g_openxr->is_session_running() && input.grip[0] >= 0.55f;
            g_reload_just_pressed =
                (g_reload_pressed && !g_previous_reload) || g_shake_reload_just_pressed;
            g_jump_just_pressed = g_jump_pressed && !g_previous_jump;
            g_crouch_just_pressed = g_crouch_pressed && !g_previous_crouch;
            g_holster_just_pressed = g_holster_pressed && !g_previous_holster;
            g_flashlight_just_pressed =
                g_flashlight_pressed && !g_previous_flashlight;
            g_menu_button_just_pressed = g_menu_button_pressed &&
                !g_previous_menu_button;
            g_left_grip_just_pressed = g_left_grip_pressed && !g_previous_left_grip;
            g_previous_reload = g_reload_pressed;
            g_previous_jump = g_jump_pressed;
            g_previous_crouch = g_crouch_pressed;
            g_previous_holster = g_holster_pressed;
            g_previous_flashlight = g_flashlight_pressed;
            g_previous_menu_button = g_menu_button_pressed;
            g_previous_left_grip = g_left_grip_pressed;

            // Record the first edge of the controls under live acceptance so a
            // normal release log can distinguish OpenXR binding delivery from
            // RF action injection without enabling noisy input debugging.
            if (g_jump_just_pressed && !g_jump_semantic_logged) {
                g_jump_semantic_logged = true;
                xlog::info("[AFVR] OpenXR jump action received (right B)");
            }
            if (g_crouch_just_pressed && !g_crouch_semantic_logged) {
                g_crouch_semantic_logged = true;
                xlog::info("[AFVR] OpenXR crouch action received (right A)");
            }
            if (g_holster_just_pressed && !g_holster_semantic_logged) {
                g_holster_semantic_logged = true;
                xlog::info("[AFVR] OpenXR holster action received (left thumbstick click)");
            }
            if (g_flashlight_just_pressed && !g_flashlight_semantic_logged) {
                g_flashlight_semantic_logged = true;
                xlog::info("[AFVR] OpenXR flashlight action received (left secondary face button)");
            }
            if (g_menu_button_just_pressed && !g_menu_semantic_logged) {
                g_menu_semantic_logged = true;
                xlog::info("[AFVR] OpenXR pause/menu action received");
            }

            const bool right_grip_pressed =
                g_openxr->is_session_running() && input.grip[1] >= 0.55f;
            if (g_two_hand_calibration_active &&
                current_local_weapon_id() != g_two_hand_calibration_weapon_id) {
                rf::console::print(
                    "Two-hand calibration cancelled because the equipped weapon changed");
                xlog::info(
                    "[AFVR] Two-hand calibration cancelled after weapon change");
                g_two_hand_calibration_active = false;
                g_two_hand_calibration_weapon_id = -1;
            }
            if (g_two_hand_calibration_active && g_trigger_just_pressed) {
                // Calibration confirmation owns this entire trigger press. Keep
                // it out of both primary and alternate fire until release.
                (void)capture_two_hand_calibration_sample();
                g_fire_blocked_until_trigger_release = true;
            }
            if (g_menu_capture_active) {
                g_gameplay_input_blocked_until_release = true;
            }
            else if (g_gameplay_input_blocked_until_release &&
                !trigger_pressed && !right_grip_pressed && !g_left_grip_pressed &&
                !g_reload_pressed && !g_jump_pressed && !g_crouch_pressed &&
                !g_holster_pressed && !g_flashlight_pressed &&
                !steamvr_menu_chord_buttons &&
                input.left_trigger <= 0.35f &&
                std::abs(input.right_thumbstick.y) <= 0.35f) {
                g_gameplay_input_blocked_until_release = false;
            }
            if (g_menu_capture_active &&
                (trigger_pressed || index_alt_trigger_pressed)) {
                g_fire_blocked_until_trigger_release = true;
            }
            else if (!trigger_pressed && !index_alt_trigger_pressed) {
                g_fire_blocked_until_trigger_release = false;
            }
            if (g_trigger_just_pressed) {
                g_fire_mode_secondary =
                    !input.index_profile_active && right_grip_pressed;
            }
            else if (!trigger_pressed) {
                g_fire_mode_secondary = false;
            }
            g_primary_fire_pressed = trigger_pressed && !g_fire_mode_secondary &&
                !index_alt_trigger_pressed &&
                !g_menu_capture_active && !g_two_hand_calibration_active &&
                !g_fire_blocked_until_trigger_release;
            g_secondary_fire_pressed =
                ((trigger_pressed && g_fire_mode_secondary) ||
                    index_alt_trigger_pressed) &&
                !g_menu_capture_active && !g_two_hand_calibration_active &&
                !g_fire_blocked_until_trigger_release;
            g_primary_fire_just_pressed = g_primary_fire_pressed &&
                !g_previous_primary_fire;
            g_secondary_fire_just_pressed = g_secondary_fire_pressed &&
                !g_previous_secondary_fire;
            g_previous_primary_fire = g_primary_fire_pressed;
            g_previous_secondary_fire = g_secondary_fire_pressed;

            g_previous_weapon_pulse = false;
            g_next_weapon_pulse = false;
            auto* local_entity = rf::local_player
                ? rf::entity_from_handle(rf::local_player->entity_handle)
                : nullptr;
            const bool local_mounted = local_entity &&
                (rf::entity_in_vehicle(local_entity) ||
                    rf::entity_is_on_turret(local_entity));
            if (!g_menu_capture_active &&
                !g_gameplay_input_blocked_until_release && !local_mounted) {
                const float cycle_y = input.right_thumbstick.y;
                const float turn_x = input.right_thumbstick.x;
                const bool vertical_dominant =
                    std::abs(cycle_y) >= std::abs(turn_x) + 0.1f;
                if (!g_weapon_cycle_latched && vertical_dominant &&
                    std::abs(cycle_y) >= 0.7f) {
                    g_previous_weapon_pulse = cycle_y > 0.0f;
                    g_next_weapon_pulse = cycle_y < 0.0f;
                    g_weapon_cycle_latched = true;
                }
                else if (g_weapon_cycle_latched && std::abs(cycle_y) <= 0.35f) {
                    g_weapon_cycle_latched = false;
                }
            }
            else if (std::abs(input.right_thumbstick.y) <= 0.35f) {
                g_weapon_cycle_latched = false;
            }

            if ((g_menu_capture_active && g_jump_just_pressed) ||
                g_menu_button_just_pressed) {
                // RF has no exported menu-back semantic. Feed Escape into its
                // internal key event queue (never into Windows input).
                rf::key_process_event(rf::KEY_ESC, 1, 0);
                rf::key_process_event(rf::KEY_ESC, 0, 0);
                log_input_edge(g_menu_capture_active ? "Menu Back" : "Pause Menu", true);
            }
        }
#endif
    }

    void submit_menu_frame()
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_openxr || !g_openxr->is_session_running() || !g_menu_capture_active) {
            return;
        }
        static bool copy_failure_logged = false;
        (void)g_openxr->render_menu_frame([&](const OpenXrMenuRenderInfo& menu) {
            g_latest_center_tracking_position = menu.center_pose.position;
            g_latest_center_tracking_position_valid = true;
            if (!g_tracking_origin.valid || g_recenter_requested) {
                capture_tracking_origin(menu.center_pose);
                g_roomscale_world_correction = {};
                g_roomscale_collision_frame = -1;
            }
            if (!copy_d3d11_menu(menu.texture) && !copy_failure_logged) {
                copy_failure_logged = true;
                xlog::error("[AFVR] RF menu target could not be copied to the OpenXR quad");
            }
            float u = 0.0f;
            float v = 0.0f;
            g_menu_pointer_valid = g_openxr->get_menu_pointer(u, v);
            if (g_menu_pointer_valid) {
                g_menu_pointer_x = std::clamp(
                    static_cast<int>(u * rf::gr::screen_width()),
                    0, std::max(rf::gr::screen_width() - 1, 0));
                g_menu_pointer_y = std::clamp(
                    static_cast<int>(v * rf::gr::screen_height()),
                    0, std::max(rf::gr::screen_height() - 1, 0));
            }
        });
#endif
    }

    void submit_scope_frame()
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_openxr || !g_openxr->is_session_running() ||
            !g_scope_capture_active) {
            return;
        }
        static bool copy_failure_logged = false;
        (void)g_openxr->render_scope_frame([&](const OpenXrMenuRenderInfo& scope) {
            g_latest_center_tracking_position = scope.center_pose.position;
            g_latest_center_tracking_position_valid = true;
            if (!g_tracking_origin.valid || g_recenter_requested) {
                capture_tracking_origin(scope.center_pose);
                g_roomscale_world_correction = {};
                g_roomscale_collision_frame = -1;
            }
            if (g_scope_view_base_valid) {
                const rf::Matrix3 relative_orientation =
                    relative_tracking_orientation(scope.center_pose.orientation);
                g_hmd_relative_orientation = relative_orientation;
                g_hmd_relative_orientation_valid = true;
                g_hmd_relative_yaw = std::atan2(
                    relative_orientation.fvec.x, relative_orientation.fvec.z);
                g_hmd_relative_forward_y =
                    std::clamp(relative_orientation.fvec.y, -1.0f, 1.0f);
                const rf::Vector3 relative_center_position =
                    relative_tracking_position(scope.center_pose.position);
                g_roomscale_world_correction = roomscale_collision_correction(
                    g_scope_base_eye_position, g_scope_player_view_base,
                    relative_center_position);
                g_roomscale_collision_frame = rf::frame_count;
                g_head_pose_valid = transform_tracked_pose_to_world(
                    scope.center_pose, g_scope_base_eye_position,
                    g_scope_player_view_base,
                    g_head_position, g_head_orientation);
                update_weapon_pose(
                    g_scope_base_eye_position, g_scope_player_view_base);
            }
            if (!copy_d3d11_menu(scope.texture) && !copy_failure_logged) {
                copy_failure_logged = true;
                xlog::error(
                    "[AFVR] Native rifle scope target could not be copied to the OpenXR quad");
            }
        });
#endif
    }

    void shutdown()
    {
#ifdef AF_ENABLE_OPENXR
        update_game_frame_limiter(false);
        if (g_openxr) {
            delete g_openxr;
            g_openxr = nullptr;
        }
#endif
        g_requested = false;
#ifdef AF_ENABLE_OPENXR
        g_tracking_origin.valid = false;
        g_recenter_requested = true;
        g_head_rotation_logged = false;
        g_hmd_relative_yaw = 0.0f;
        g_hmd_relative_forward_y = 0.0f;
        g_hmd_relative_orientation = {
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f},
        };
        g_hmd_relative_orientation_valid = false;
        g_resolved_player_pose = {};
        g_turn_diagnostic = {};
        g_visual_body_yaw_offset = 0.0f;
        g_body_follow_frame = -1;
        g_body_follow_logged = false;
        g_resolved_pose_logged = false;
        clear_mounted_aim_state();
        g_mounted_aim_logged = false;
        g_vehicle_controls_logged = false;
        g_local_attachment_active = false;
        g_local_attachment_entity_handle = -1;
        g_vehicle_exit_horizon_stabilization = false;
        g_vehicle_exit_level_frames = 0;
        g_movement_input_logged = false;
        g_ladder_input_logged = false;
        g_snap_turn_logged = false;
        g_smooth_turn_logged = false;
        g_snap_turn_latched = false;
        g_rendering_weapon = false;
        g_rendering_fpgun_body = false;
        g_weapon_pose_valid = false;
        g_weapon_aim_pose_valid = false;
        g_laser_emitter_pose_valid = false;
        g_laser_emitter_position = {};
        g_laser_emitter_orientation = {};
        g_two_hand_support_available = false;
        g_two_hand_weapon_active = false;
        g_two_hand_weapon_id = -1;
        g_two_hand_calibration_active = false;
        g_two_hand_calibration_weapon_id = -1;
        g_right_controller_pose_valid = false;
        g_controller_grip_world_valid = false;
        g_controller_aim_world_valid = false;
        g_left_controller_grip_world_valid = false;
        g_left_controller_aim_world_valid = false;
        g_controller_grip_world_position = {};
        g_controller_grip_world_orientation = {};
        g_controller_aim_world_position = {};
        g_controller_aim_world_orientation = {};
        g_left_controller_grip_world_position = {};
        g_left_controller_grip_world_orientation = {};
        g_left_controller_aim_world_position = {};
        g_left_controller_aim_world_orientation = {};
        g_head_pose_valid = false;
        g_latest_center_tracking_position_valid = false;
        g_current_weapon_id = -1;
        g_debug_weapon_at_hmd = false;
        g_weapon_render_eye = -1;
        g_weapon_render_requested_logged = false;
        g_player_render_reached_logged = false;
        g_weapon_mesh_render_reached_logged = false;
        g_final_weapon_transform_logged = false;
        g_weapon_eye_draw_logged = {};
        g_two_hand_weapon_logged = {};
        g_trigger_pressed = false;
        g_trigger_just_pressed = false;
        g_previous_trigger_pressed = false;
        g_trigger_action_logged = false;
        g_frame_limiter_bypassed = false;
        g_multiplayer_best_effort_logged = false;
        g_menu_capture_active = false;
        g_scope_capture_active = false;
        g_scope_view_base_valid = false;
        g_scope_base_eye_position = {};
        g_scope_player_view_base = {};
        g_steamvr_menu_chord_started = {};
        g_steamvr_menu_chord_timing = false;
        g_roomscale_world_correction = {};
        g_roomscale_collision_frame = -1;
        g_roomscale_collision_logged = false;
        g_roomscale_reconciliation_logged = false;
        g_next_desktop_mirror_update = {};
        g_desktop_mirror_decision_frame = -1;
        g_desktop_mirror_update_due = false;
        g_singleplayer_death_menu_active = false;
        g_menu_pointer_using_controller = true;
        g_hud_capture_active = false;
        g_hud_rendered_frame = -1;
        g_menu_pointer_valid = false;
        g_previous_reload = false;
        g_previous_jump = false;
        g_previous_crouch = false;
        g_previous_holster = false;
        g_previous_flashlight = false;
        g_previous_menu_button = false;
        g_previous_left_grip = false;
        g_previous_primary_fire = false;
        g_previous_secondary_fire = false;
        g_fire_mode_secondary = false;
        g_fire_blocked_until_trigger_release = false;
        g_gameplay_input_blocked_until_release = false;
        g_reload_pressed = false;
        g_reload_just_pressed = false;
        g_shake_reload_just_pressed = false;
        g_previous_right_grip_y_valid = false;
        g_previous_right_grip_y = 0.0f;
        g_previous_right_grip_velocity = 0.0f;
        g_shake_reload_cooldown = 0.0f;
        g_jump_pressed = false;
        g_jump_just_pressed = false;
        g_crouch_pressed = false;
        g_crouch_just_pressed = false;
        g_holster_pressed = false;
        g_holster_just_pressed = false;
        g_flashlight_pressed = false;
        g_flashlight_just_pressed = false;
        g_menu_button_pressed = false;
        g_menu_button_just_pressed = false;
        g_left_grip_pressed = false;
        g_left_grip_just_pressed = false;
        g_primary_fire_pressed = false;
        g_primary_fire_just_pressed = false;
        g_secondary_fire_pressed = false;
        g_secondary_fire_just_pressed = false;
        g_weapon_cycle_latched = false;
        g_previous_weapon_pulse = false;
        g_next_weapon_pulse = false;
        g_jump_semantic_logged = false;
        g_crouch_semantic_logged = false;
        g_holster_semantic_logged = false;
        g_flashlight_semantic_logged = false;
        g_menu_semantic_logged = false;
        g_laser_sight_enabled = false;
        g_previous_laser_toggle_pressed = false;
        g_laser_trace_frame = -1;
        g_laser_trace_valid = false;
        g_laser_trace_hit = false;
        g_laser_coordinate_audit_logged = false;
        g_laser_trace_start = {};
        g_laser_trace_end = {};
        g_previous_forward = false;
        g_previous_backward = false;
        g_previous_strafe_left = false;
        g_previous_strafe_right = false;
        g_last_menu_state = rf::GS_INIT;
        g_weapon_calibration_logged = {};
        g_live_weapon_calibration_active = {};
        g_live_two_hand_calibration_active = {};
#endif
    }

    bool is_requested()
    {
        return g_requested;
    }

    bool is_initialized()
    {
#ifdef AF_ENABLE_OPENXR
        return g_openxr && g_openxr->is_initialized();
#else
        return false;
#endif
    }

    bool is_session_running()
    {
#ifdef AF_ENABLE_OPENXR
        return g_openxr && g_openxr->is_session_running();
#else
        return false;
#endif
    }

    bool is_menu_capture_active()
    {
#ifdef AF_ENABLE_OPENXR
        return g_openxr && g_openxr->is_session_running() && g_menu_capture_active;
#else
        return false;
#endif
    }

    bool is_scope_capture_active()
    {
#ifdef AF_ENABLE_OPENXR
        return g_openxr && g_openxr->is_session_running() &&
            g_scope_capture_active;
#else
        return false;
#endif
    }

    bool should_update_desktop_mirror()
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_openxr || !g_openxr->is_session_running()) {
            return false;
        }
        if (g_desktop_mirror_decision_frame == rf::frame_count) {
            return g_desktop_mirror_update_due;
        }

        g_desktop_mirror_decision_frame = rf::frame_count;
        const auto now = std::chrono::steady_clock::now();
        g_desktop_mirror_update_due =
            g_next_desktop_mirror_update == std::chrono::steady_clock::time_point{} ||
            now >= g_next_desktop_mirror_update;
        if (g_desktop_mirror_update_due) {
            // Never catch up missed desktop frames. About 30 Hz is sufficient
            // for the console and spectator validation while preserving GPU
            // headroom for the runtime-paced headset submission.
            g_next_desktop_mirror_update = now + std::chrono::milliseconds(33);
        }
        return g_desktop_mirror_update_due;
#else
        return false;
#endif
    }

    bool should_block_physical_mouse_input()
    {
#ifdef AF_ENABLE_OPENXR
        return g_openxr && g_openxr->is_session_running() &&
            !g_menu_capture_active &&
            rf::gameseq_get_state() != rf::GS_MAIN_MENU;
#else
        return false;
#endif
    }

    void recenter_tracking()
    {
#ifdef AF_ENABLE_OPENXR
        if (g_openxr) {
            g_recenter_requested = true;
            g_roomscale_world_correction = {};
            g_roomscale_collision_frame = -1;
            if (g_menu_capture_active) {
                g_openxr->set_menu_active(false);
                g_openxr->set_menu_active(true);
            }
        }
#endif
    }

    void begin_scene_render_pass(int pass)
    {
        if (pass < 0 || pass >= static_cast<int>(g_scene_render_stats.size())) {
            return;
        }
        g_scene_render_pass = pass;
        if (pass == 0 && !g_scene_render_stats_logged) {
            g_scene_render_stats = {};
            g_scene_render_stats_collecting = true;
        }
        if (!g_scene_render_stats_collecting) {
            return;
        }
        ++g_scene_render_stats[pass].portal_traversals;
    }

    void end_scene_render_pass(int pass)
    {
        if (g_scene_render_pass != pass) {
            return;
        }
        g_scene_render_pass = -1;
        if (!g_scene_render_stats_collecting) {
            return;
        }
        if (pass == 1 && !g_scene_render_stats_logged) {
            ++g_scene_render_diagnostic_frames;
            const bool characters_reached = std::ranges::any_of(
                g_scene_render_stats, [](const SceneRenderStats& stats) {
                    return stats.character_meshes > 0;
                });
            if (!characters_reached && g_scene_render_diagnostic_frames < 120) {
                g_scene_render_stats_collecting = false;
                return;
            }
            constexpr std::array names{"left", "right"};
            for (size_t index = 0; index < names.size(); ++index) {
                const auto& stats = g_scene_render_stats[index];
                xlog::info("[AFVR] First-frame {} draws: portal {}, static {}, movers {}, standard meshes {}, character meshes {}",
                    names[index], stats.portal_traversals, stats.static_solids,
                    stats.movable_solids, stats.standard_meshes, stats.character_meshes);
            }
            g_scene_render_stats_logged = true;
            g_scene_render_stats_collecting = false;
        }
    }

    void note_static_solid_draw()
    {
        if (g_scene_render_stats_collecting && g_scene_render_pass >= 0) {
            ++g_scene_render_stats[g_scene_render_pass].static_solids;
        }
    }

    void note_movable_solid_draw()
    {
        if (g_scene_render_stats_collecting && g_scene_render_pass >= 0) {
            ++g_scene_render_stats[g_scene_render_pass].movable_solids;
        }
    }

    void note_standard_mesh_draw()
    {
        if (g_scene_render_stats_collecting && g_scene_render_pass >= 0) {
            ++g_scene_render_stats[g_scene_render_pass].standard_meshes;
        }
    }

    void note_character_mesh_draw()
    {
        if (g_scene_render_stats_collecting && g_scene_render_pass >= 0) {
            ++g_scene_render_stats[g_scene_render_pass].character_meshes;
        }
    }

    bool is_rendering_weapon()
    {
        return g_rendering_weapon;
    }

    bool should_render_fpgun_texture(int bitmap_handle)
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_rendering_fpgun_body || bitmap_handle < 0) {
            return true;
        }
        const char* filename = rf::bm::get_filename(bitmap_handle);
        if (!filename || !filename[0]) {
            return true;
        }
        std::string normalized{filename};
        std::ranges::transform(normalized, normalized.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const bool desktop_hand_material =
            normalized == "hand1st.tga" || normalized == "envirohand.tga";

        // Stock fp_glock.v3c contains body, both desktop arms/hands, moving
        // clip, muzzle/silencer tags and bones in one animated VMesh. Report
        // its distinct renderable material chunks once while filtering only
        // the two dedicated hand materials.
        if (g_current_weapon_id == 0x03 && g_weapon_render_eye == 0) {
            static std::unordered_set<std::string> reported_materials;
            if (reported_materials.emplace(normalized).second) {
                xlog::info("[AFVR] Glock FPGUN material '{}' -> {}",
                    normalized, desktop_hand_material ? "hidden desktop hands/arms" : "kept weapon/detail");
            }
        }
        return !desktop_hand_material;
#else
        (void)bitmap_handle;
        return true;
#endif
    }

    bool get_weapon_muzzle_pose(rf::Vector3& position, rf::Matrix3& orientation)
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_openxr || !g_openxr->is_session_running() ||
            !g_weapon_pose_valid || !g_weapon_aim_pose_valid) {
            return false;
        }
        position = g_weapon_fire_position;
        orientation = g_weapon_fire_orientation;
        audit_weapon_fire_origin(false);
        return true;
#else
        (void)position;
        (void)orientation;
        return false;
#endif
    }

    bool get_weapon_launch_pose(int weapon_type, rf::Vector3& position,
        rf::Matrix3& orientation)
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_openxr || !g_openxr->is_session_running() ||
            !g_weapon_pose_valid || !g_weapon_aim_pose_valid) {
            return false;
        }
        // Projectile creation is gameplay. It always uses the independently
        // calibrated fire point and can never inherit the visual laser emitter.
        (void)weapon_type;
        position = g_weapon_fire_position;
        orientation = g_weapon_fire_orientation;
        audit_weapon_fire_origin(false);
        return true;
#else
        (void)weapon_type;
        (void)position;
        (void)orientation;
        return false;
#endif
    }

    bool get_head_pose(rf::Vector3& position, rf::Matrix3& orientation)
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_openxr || !g_openxr->is_session_running() ||
            !g_head_pose_valid) {
            return false;
        }
        position = g_head_position;
        orientation = g_head_orientation;
        return true;
#else
        (void)position;
        (void)orientation;
        return false;
#endif
    }

    bool get_visual_body_pose(rf::Vector3& position,
        rf::Matrix3& orientation)
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_openxr || !g_openxr->is_session_running() ||
            !g_resolved_player_pose.valid) {
            return false;
        }
        position = g_resolved_player_pose.body_world_position;
        orientation = g_resolved_player_pose.visual_body_world_orientation;
        return true;
#else
        (void)position;
        (void)orientation;
        return false;
#endif
    }

    bool get_right_controller_pose(rf::Vector3& position,
        rf::Matrix3& orientation)
    {
#ifdef AF_ENABLE_OPENXR
        if (!g_openxr || !g_openxr->is_session_running() ||
            !g_right_controller_pose_valid) {
            return false;
        }
        position = g_right_controller_position;
        orientation = g_right_controller_orientation;
        return true;
#else
        (void)position;
        (void)orientation;
        return false;
#endif
    }

    bool is_primary_trigger_active()
    {
#ifdef AF_ENABLE_OPENXR
        return g_trigger_pressed;
#else
        return false;
#endif
    }
}
