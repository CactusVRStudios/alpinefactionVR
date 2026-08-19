// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <d3d11.h>
#include <array>
#include <functional>
#include <vector>
#include <common/ComPtr.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace afvr
{
    struct OpenXrInputState
    {
        XrVector2f left_thumbstick{};
        XrVector2f right_thumbstick{};
        bool left_thumbstick_click = false;
        bool right_thumbstick_click = false;
        std::array<XrPosef, 2> grip_poses{};
        std::array<XrPosef, 2> aim_poses{};
        std::array<bool, 2> grip_pose_valid{};
        std::array<bool, 2> aim_pose_valid{};
        float left_trigger = 0.0f;
        float right_trigger = 0.0f;
        std::array<float, 2> grip{};
        bool index_profile_active = false;
        bool reload = false;
        bool jump = false;
        bool crouch = false;
        bool flashlight = false;
        bool menu = false;
    };

    struct OpenXrEyeRenderInfo
    {
        uint32_t eye_index;
        XrView view;
        XrPosef center_pose;
        ID3D11RenderTargetView* render_target_view;
        ID3D11ShaderResourceView* shader_resource_view;
        ID3D11DepthStencilView* depth_stencil_view;
        int width;
        int height;
    };

    using OpenXrEyeRenderCallback = std::function<void(const OpenXrEyeRenderInfo&)>;

    struct OpenXrHudRenderInfo
    {
        ID3D11RenderTargetView* render_target_view;
        int width;
        int height;
    };

    using OpenXrHudRenderCallback = std::function<void(const OpenXrHudRenderInfo&)>;

    struct OpenXrMenuRenderInfo
    {
        ID3D11Texture2D* texture;
        XrPosef center_pose;
        int width;
        int height;
    };

    using OpenXrMenuRenderCallback = std::function<void(const OpenXrMenuRenderInfo&)>;

    class OpenXrContext
    {
    public:
        OpenXrContext() = default;
        ~OpenXrContext();

        OpenXrContext(const OpenXrContext&) = delete;
        OpenXrContext& operator=(const OpenXrContext&) = delete;

        [[nodiscard]] bool initialize(ID3D11Device* device);
        [[nodiscard]] bool poll_events();
        [[nodiscard]] bool sync_actions();
        [[nodiscard]] bool wait_frame();
        [[nodiscard]] bool render_frame(const OpenXrEyeRenderCallback& render_eye,
            const OpenXrHudRenderCallback& render_hud = {});
        [[nodiscard]] bool render_menu_frame(const OpenXrMenuRenderCallback& render_menu);
        void set_menu_active(bool active);
        void set_hud_active(bool active);
        [[nodiscard]] bool get_menu_pointer(float& u, float& v) const;
        void shutdown();

        [[nodiscard]] bool is_initialized() const { return instance_ != XR_NULL_HANDLE; }
        [[nodiscard]] bool is_session_running() const { return session_running_; }
        [[nodiscard]] XrSessionState session_state() const { return session_state_; }
        [[nodiscard]] const OpenXrInputState& input_state() const { return input_state_; }
        [[nodiscard]] bool is_steamvr_runtime() const { return steamvr_runtime_; }

    private:
        void create_instance();
        void select_system();
        void validate_graphics_device(ID3D11Device* device);
        void create_session(ID3D11Device* device);
        void create_reference_space();
        void create_actions();
        void create_swapchains(ID3D11Device* device);
        void report_display_refresh_rate();
        void destroy_swapchains();
        void locate_hand_poses(XrTime time);
        void handle_session_state_changed(const XrEventDataSessionStateChanged& event);

        void check(XrResult result, const char* operation) const;

        XrInstance instance_ = XR_NULL_HANDLE;
        XrSystemId system_id_ = XR_NULL_SYSTEM_ID;
        XrSession session_ = XR_NULL_HANDLE;
        XrSpace reference_space_ = XR_NULL_HANDLE;
        XrActionSet gameplay_action_set_ = XR_NULL_HANDLE;
        XrAction left_thumbstick_action_ = XR_NULL_HANDLE;
        XrAction right_thumbstick_action_ = XR_NULL_HANDLE;
        XrAction left_thumbstick_click_action_ = XR_NULL_HANDLE;
        XrAction right_thumbstick_click_action_ = XR_NULL_HANDLE;
        XrAction grip_pose_action_ = XR_NULL_HANDLE;
        XrAction aim_pose_action_ = XR_NULL_HANDLE;
        XrAction left_trigger_action_ = XR_NULL_HANDLE;
        XrAction right_trigger_action_ = XR_NULL_HANDLE;
        XrAction reload_action_ = XR_NULL_HANDLE;
        XrAction jump_action_ = XR_NULL_HANDLE;
        XrAction crouch_action_ = XR_NULL_HANDLE;
        XrAction flashlight_action_ = XR_NULL_HANDLE;
        XrAction grip_action_ = XR_NULL_HANDLE;
        XrAction menu_action_ = XR_NULL_HANDLE;
        std::array<XrPath, 2> hand_paths_{};
        XrPath touch_interaction_profile_ = XR_NULL_PATH;
        XrPath index_interaction_profile_ = XR_NULL_PATH;
        std::array<XrSpace, 2> grip_spaces_{};
        std::array<XrSpace, 2> aim_spaces_{};
        OpenXrInputState input_state_{};
        XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
        XrEnvironmentBlendMode environment_blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        bool session_running_ = false;
        bool exit_requested_ = false;
        bool steamvr_runtime_ = false;
        bool display_refresh_rate_supported_ = false;
        bool frame_waited_ = false;
        bool frame_begun_ = false;
        XrFrameState waited_frame_state_{XR_TYPE_FRAME_STATE};
        bool first_frame_logged_ = false;
        bool first_wait_frame_logged_ = false;
        bool display_period_logged_ = false;
        bool first_begin_frame_logged_ = false;
        bool first_valid_views_logged_ = false;
        std::array<bool, 2> first_eye_acquired_logged_{};
        bool first_projection_layer_logged_ = false;
        uint32_t submitted_frame_count_ = 0;
        bool sustained_submission_logged_ = false;
        bool interaction_profile_logged_ = false;
        std::array<bool, 2> controller_pose_logged_{};
        PFN_xrGetD3D11GraphicsRequirementsKHR get_d3d11_graphics_requirements_ = nullptr;
        PFN_xrEnumerateDisplayRefreshRatesFB enumerate_display_refresh_rates_ = nullptr;
        PFN_xrGetDisplayRefreshRateFB get_display_refresh_rate_ = nullptr;

        struct EyeSwapchain
        {
            XrSwapchain handle = XR_NULL_HANDLE;
            int32_t width = 0;
            int32_t height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::vector<XrSwapchainImageD3D11KHR> images;
            std::vector<ComPtr<ID3D11RenderTargetView>> render_target_views;
            std::vector<ComPtr<ID3D11ShaderResourceView>> shader_resource_views;
            std::vector<ComPtr<ID3D11Texture2D>> depth_textures;
            std::vector<ComPtr<ID3D11DepthStencilView>> depth_stencil_views;
        };

        std::array<EyeSwapchain, 2> eye_swapchains_;

        struct QuadSwapchain
        {
            XrSwapchain handle = XR_NULL_HANDLE;
            int32_t width = 0;
            int32_t height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::vector<XrSwapchainImageD3D11KHR> images;
            std::vector<ComPtr<ID3D11RenderTargetView>> render_target_views;
        };

        QuadSwapchain menu_swapchain_;
        QuadSwapchain hud_swapchain_;
        bool menu_active_ = false;
        bool hud_active_ = false;
        bool menu_pose_valid_ = false;
        XrPosef menu_pose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        bool first_menu_layer_logged_ = false;
        bool first_hud_layer_logged_ = false;
    };
}
