// SPDX-License-Identifier: MPL-2.0
#include "openxr_context.h"
#include "vr.h"

#include <chrono>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <common/ComPtr.h>
#include <common/version/version.h>
#include <xlog/xlog.h>
#include "../rf/gr/gr.h"

namespace afvr
{
    namespace
    {
        constexpr XrVersion requested_api_version = XR_MAKE_VERSION(1, 0, 0);

        const char* session_state_name(XrSessionState state)
        {
            switch (state) {
                case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
                case XR_SESSION_STATE_IDLE: return "IDLE";
                case XR_SESSION_STATE_READY: return "READY";
                case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
                case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
                case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
                case XR_SESSION_STATE_STOPPING: return "STOPPING";
                case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
                case XR_SESSION_STATE_EXITING: return "EXITING";
                default: return "UNRECOGNIZED";
            }
        }

        std::string format_version(XrVersion version)
        {
            return std::format("{}.{}.{}",
                XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), XR_VERSION_PATCH(version));
        }

        bool has_extension(const std::vector<XrExtensionProperties>& extensions, std::string_view name)
        {
            return std::ranges::any_of(extensions, [name](const XrExtensionProperties& extension) {
                return std::string_view{extension.extensionName} == name;
            });
        }

        bool luid_equal(const LUID& lhs, const LUID& rhs)
        {
            return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
        }

        const char* format_name(DXGI_FORMAT format)
        {
            switch (format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
                case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
                case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
                default: return "unrecognized";
            }
        }

        DXGI_FORMAT gamma_space_view_format(DXGI_FORMAT swapchain_format)
        {
            switch (swapchain_format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                default:
                    return swapchain_format;
            }
        }

        const char* blend_mode_name(XrEnvironmentBlendMode mode)
        {
            switch (mode) {
                case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: return "OPAQUE";
                case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE: return "ADDITIVE";
                case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return "ALPHA_BLEND";
                default: return "UNRECOGNIZED";
            }
        }

        constexpr float menu_distance_m = 1.5f;
        constexpr float menu_width_m = 1.6f;
        constexpr float hud_distance_m = 2.4f;
        constexpr float hud_width_m = 2.75f;

        XrVector3f rotate_vector(const XrQuaternionf& q, const XrVector3f& v)
        {
            const XrVector3f qv{q.x, q.y, q.z};
            const XrVector3f uv{
                qv.y * v.z - qv.z * v.y,
                qv.z * v.x - qv.x * v.z,
                qv.x * v.y - qv.y * v.x,
            };
            const XrVector3f uuv{
                qv.y * uv.z - qv.z * uv.y,
                qv.z * uv.x - qv.x * uv.z,
                qv.x * uv.y - qv.y * uv.x,
            };
            return {
                v.x + 2.0f * (q.w * uv.x + uuv.x),
                v.y + 2.0f * (q.w * uv.y + uuv.y),
                v.z + 2.0f * (q.w * uv.z + uuv.z),
            };
        }
    }

    OpenXrContext::~OpenXrContext()
    {
        shutdown();
    }

    bool OpenXrContext::initialize(ID3D11Device* device)
    {
        shutdown();

        try {
            if (!device) {
                throw std::runtime_error("Alpine Faction D3D11 device is null");
            }

            xlog::info("[AFVR] Initializing OpenXR");
            create_instance();
            select_system();
            validate_graphics_device(device);
            create_session(device);
            create_actions();
            create_reference_space();
            create_swapchains(device);
            return true;
        }
        catch (const std::exception& error) {
            xlog::error("[AFVR] {}", error.what());
            shutdown();
            return false;
        }
    }

    void OpenXrContext::create_instance()
    {
        uint32_t extension_count = 0;
        check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr),
            "xrEnumerateInstanceExtensionProperties(count)");

        std::vector<XrExtensionProperties> extensions(extension_count,
            XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        check(xrEnumerateInstanceExtensionProperties(
            nullptr, extension_count, &extension_count, extensions.data()),
            "xrEnumerateInstanceExtensionProperties(list)");

        if (!has_extension(extensions, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
            throw std::runtime_error("OpenXR runtime does not expose XR_KHR_D3D11_enable");
        }

        std::vector<const char*> enabled_extensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        display_refresh_rate_supported_ =
            has_extension(extensions, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        if (display_refresh_rate_supported_) {
            enabled_extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        }
        XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::snprintf(create_info.applicationInfo.applicationName,
            sizeof(create_info.applicationInfo.applicationName), "AlpineFaction VR");
        create_info.applicationInfo.applicationVersion =
            (VERSION_MAJOR << 24) | (VERSION_MINOR << 16) | VERSION_PATCH;
        std::snprintf(create_info.applicationInfo.engineName,
            sizeof(create_info.applicationInfo.engineName), "Alpine Faction");
        create_info.applicationInfo.engineVersion =
            (VERSION_MAJOR << 24) | (VERSION_MINOR << 16) | VERSION_PATCH;
        // VR-1 uses only OpenXR 1.0 functionality. Requesting 1.0 keeps the
        // bootstrap compatible with PC runtimes that have not moved to 1.1.
        create_info.applicationInfo.apiVersion = requested_api_version;
        create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
        create_info.enabledExtensionNames = enabled_extensions.data();

        XrResult result = xrCreateInstance(&create_info, &instance_);
        if (XR_FAILED(result)) {
            throw std::runtime_error(std::format("xrCreateInstance failed ({})", static_cast<int>(result)));
        }

        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        check(xrGetInstanceProperties(instance_, &properties), "xrGetInstanceProperties");
        xlog::info("[AFVR] Runtime: {} {}", properties.runtimeName, format_version(properties.runtimeVersion));
        xlog::info("[AFVR] Requested OpenXR API: {} (SDK headers {})",
            format_version(requested_api_version), format_version(XR_CURRENT_API_VERSION));
        xlog::info("[AFVR] Enabled extension: {}", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        if (display_refresh_rate_supported_) {
            xlog::info("[AFVR] Enabled extension: {}",
                XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
            PFN_xrVoidFunction function = nullptr;
            check(xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB", &function),
                "xrGetInstanceProcAddr(xrEnumerateDisplayRefreshRatesFB)");
            enumerate_display_refresh_rates_ =
                reinterpret_cast<PFN_xrEnumerateDisplayRefreshRatesFB>(function);
            check(xrGetInstanceProcAddr(instance_, "xrGetDisplayRefreshRateFB", &function),
                "xrGetInstanceProcAddr(xrGetDisplayRefreshRateFB)");
            get_display_refresh_rate_ =
                reinterpret_cast<PFN_xrGetDisplayRefreshRateFB>(function);
        }
    }

    void OpenXrContext::select_system()
    {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        check(xrGetSystem(instance_, &system_info, &system_id_), "xrGetSystem(HMD)");
        xlog::info("[AFVR] HMD system acquired");

        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        check(xrGetSystemProperties(instance_, system_id_, &properties), "xrGetSystemProperties");
        xlog::info("[AFVR] System: {} (vendor {}, id {})",
            properties.systemName, properties.vendorId, system_id_);

        PFN_xrVoidFunction function = nullptr;
        check(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR", &function),
            "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
        get_d3d11_graphics_requirements_ =
            reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(function);
        if (!get_d3d11_graphics_requirements_) {
            throw std::runtime_error("OpenXR returned a null D3D11 graphics requirements function");
        }
    }

    void OpenXrContext::validate_graphics_device(ID3D11Device* device)
    {
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        check(get_d3d11_graphics_requirements_(instance_, system_id_, &requirements),
            "xrGetD3D11GraphicsRequirementsKHR");

        ComPtr<IDXGIDevice> dxgi_device;
        HRESULT result = device->QueryInterface(&dxgi_device);
        if (FAILED(result)) {
            throw std::runtime_error(std::format("ID3D11Device::QueryInterface(IDXGIDevice) failed (0x{:08X})",
                static_cast<unsigned>(result)));
        }

        ComPtr<IDXGIAdapter> adapter;
        result = dxgi_device->GetAdapter(&adapter);
        if (FAILED(result)) {
            throw std::runtime_error(std::format("IDXGIDevice::GetAdapter failed (0x{:08X})",
                static_cast<unsigned>(result)));
        }

        DXGI_ADAPTER_DESC adapter_desc{};
        result = adapter->GetDesc(&adapter_desc);
        if (FAILED(result)) {
            throw std::runtime_error(std::format("IDXGIAdapter::GetDesc failed (0x{:08X})",
                static_cast<unsigned>(result)));
        }

        xlog::info("[AFVR] Runtime adapter LUID: {:08X}:{:08X}",
            static_cast<unsigned>(requirements.adapterLuid.HighPart), requirements.adapterLuid.LowPart);
        xlog::info("[AFVR] Renderer adapter LUID: {:08X}:{:08X}",
            static_cast<unsigned>(adapter_desc.AdapterLuid.HighPart), adapter_desc.AdapterLuid.LowPart);
        xlog::info("[AFVR] Runtime minimum D3D feature level: 0x{:X}; renderer feature level: 0x{:X}",
            static_cast<unsigned>(requirements.minFeatureLevel),
            static_cast<unsigned>(device->GetFeatureLevel()));

        if (!luid_equal(adapter_desc.AdapterLuid, requirements.adapterLuid)) {
            throw std::runtime_error("OpenXR runtime and Alpine Faction D3D11 renderer use different adapters");
        }
        if (device->GetFeatureLevel() < requirements.minFeatureLevel) {
            throw std::runtime_error("Alpine Faction D3D11 feature level is below the OpenXR runtime requirement");
        }

        xlog::info("[AFVR] D3D11 adapter validated; reusing Alpine Faction's existing device");
    }

    void OpenXrContext::create_session(ID3D11Device* device)
    {
        XrGraphicsBindingD3D11KHR graphics_binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        graphics_binding.device = device;

        XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
        create_info.next = &graphics_binding;
        create_info.systemId = system_id_;
        check(xrCreateSession(instance_, &create_info, &session_), "xrCreateSession");
        xlog::info("[AFVR] OpenXR session created");
    }

    void OpenXrContext::report_display_refresh_rate()
    {
        if (!display_refresh_rate_supported_ || !enumerate_display_refresh_rates_ ||
            !get_display_refresh_rate_) {
            xlog::info(
                "[AFVR] Display refresh is runtime-controlled; following xrWaitFrame timing");
            return;
        }

        uint32_t rate_count = 0;
        XrResult result = enumerate_display_refresh_rates_(
            session_, 0, &rate_count, nullptr);
        if (XR_FAILED(result) || rate_count == 0) {
            xlog::warn("[AFVR] Could not enumerate runtime display refresh rates ({})",
                static_cast<int>(result));
            return;
        }
        std::vector<float> rates(rate_count);
        result = enumerate_display_refresh_rates_(
            session_, rate_count, &rate_count, rates.data());
        if (XR_FAILED(result)) {
            xlog::warn("[AFVR] Could not read runtime display refresh rates ({})",
                static_cast<int>(result));
            return;
        }
        std::string rate_list;
        for (size_t index = 0; index < rates.size(); ++index) {
            if (index > 0) {
                rate_list += ", ";
            }
            rate_list += std::format("{:.2f}", rates[index]);
        }
        xlog::info("[AFVR] Runtime-supported display refresh rates: {} Hz", rate_list);

        float current_rate = 0.0f;
        result = get_display_refresh_rate_(session_, &current_rate);
        if (XR_SUCCEEDED(result)) {
            xlog::info("[AFVR] Following runtime-selected display refresh rate: {:.2f} Hz",
                current_rate);
        }
        else {
            xlog::warn("[AFVR] Could not read the runtime-selected display refresh rate ({})",
                static_cast<int>(result));
        }
    }

    void OpenXrContext::create_reference_space()
    {
        uint32_t space_count = 0;
        check(xrEnumerateReferenceSpaces(session_, 0, &space_count, nullptr),
            "xrEnumerateReferenceSpaces(count)");
        std::vector<XrReferenceSpaceType> spaces(space_count);
        check(xrEnumerateReferenceSpaces(session_, space_count, &space_count, spaces.data()),
            "xrEnumerateReferenceSpaces(list)");

        if (std::ranges::find(spaces, XR_REFERENCE_SPACE_TYPE_LOCAL) == spaces.end()) {
            throw std::runtime_error("OpenXR runtime does not expose the required LOCAL reference space");
        }

        XrReferenceSpaceCreateInfo create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        create_info.poseInReferenceSpace.orientation.w = 1.0f;
        check(xrCreateReferenceSpace(session_, &create_info, &reference_space_),
            "xrCreateReferenceSpace(LOCAL)");
        xlog::info("[AFVR] Reference space: LOCAL");
        xlog::info("[AFVR] LOCAL reference space created");
    }

    void OpenXrContext::create_actions()
    {
        check(xrStringToPath(instance_, "/user/hand/left", &hand_paths_[0]),
            "xrStringToPath(left hand)");
        check(xrStringToPath(instance_, "/user/hand/right", &hand_paths_[1]),
            "xrStringToPath(right hand)");
        check(xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller",
            &touch_interaction_profile_), "xrStringToPath(Touch profile)");
        check(xrStringToPath(instance_, "/interaction_profiles/valve/index_controller",
            &index_interaction_profile_), "xrStringToPath(Index profile)");

        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::snprintf(set_info.actionSetName, sizeof(set_info.actionSetName), "gameplay");
        std::snprintf(set_info.localizedActionSetName,
            sizeof(set_info.localizedActionSetName), "AlpineFaction VR Gameplay");
        set_info.priority = 0;
        check(xrCreateActionSet(instance_, &set_info, &gameplay_action_set_),
            "xrCreateActionSet(gameplay)");

        auto create_action = [&](XrAction& action, XrActionType type,
            const char* name, const char* localized_name, const std::vector<XrPath>& subactions) {
            XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
            action_info.actionType = type;
            std::snprintf(action_info.actionName, sizeof(action_info.actionName), "%s", name);
            std::snprintf(action_info.localizedActionName,
                sizeof(action_info.localizedActionName), "%s", localized_name);
            action_info.countSubactionPaths = static_cast<uint32_t>(subactions.size());
            action_info.subactionPaths = subactions.data();
            check(xrCreateAction(gameplay_action_set_, &action_info, &action),
                std::format("xrCreateAction({})", name).c_str());
        };

        const std::vector<XrPath> left_hand{hand_paths_[0]};
        const std::vector<XrPath> right_hand{hand_paths_[1]};
        const std::vector<XrPath> both_hands{hand_paths_[0], hand_paths_[1]};
        create_action(left_thumbstick_action_, XR_ACTION_TYPE_VECTOR2F_INPUT,
            "left_thumbstick", "Left Thumbstick", left_hand);
        create_action(right_thumbstick_action_, XR_ACTION_TYPE_VECTOR2F_INPUT,
            "right_thumbstick", "Right Thumbstick", right_hand);
        create_action(left_thumbstick_click_action_, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "left_thumbstick_click", "Holster Weapon", left_hand);
        create_action(right_thumbstick_click_action_, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "right_thumbstick_click", "Toggle Laser Sight", right_hand);
        create_action(grip_pose_action_, XR_ACTION_TYPE_POSE_INPUT,
            "grip_pose", "Grip Pose", both_hands);
        create_action(aim_pose_action_, XR_ACTION_TYPE_POSE_INPUT,
            "aim_pose", "Aim Pose", both_hands);
        create_action(right_trigger_action_, XR_ACTION_TYPE_FLOAT_INPUT,
            "right_trigger", "Right Trigger", right_hand);
        create_action(reload_action_, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "reload", "Reload", left_hand);
        create_action(jump_action_, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "jump", "Jump", right_hand);
        create_action(crouch_action_, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "crouch", "Crouch", right_hand);
        create_action(flashlight_action_, XR_ACTION_TYPE_BOOLEAN_INPUT,
            "flashlight", "Toggle Flashlight or Headlight", left_hand);
        create_action(grip_action_, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip", both_hands);
        create_action(menu_action_, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", left_hand);
        create_action(index_menu_force_action_, XR_ACTION_TYPE_FLOAT_INPUT,
            "index_menu_force", "Menu (Index Trackpad)", left_hand);

        auto path = [&](const char* value) {
            XrPath result = XR_NULL_PATH;
            check(xrStringToPath(instance_, value, &result), "xrStringToPath(binding)");
            return result;
        };
        const std::array touch_bindings{
            XrActionSuggestedBinding{left_thumbstick_action_, path("/user/hand/left/input/thumbstick")},
            XrActionSuggestedBinding{right_thumbstick_action_, path("/user/hand/right/input/thumbstick")},
            XrActionSuggestedBinding{left_thumbstick_click_action_, path("/user/hand/left/input/thumbstick/click")},
            XrActionSuggestedBinding{right_thumbstick_click_action_, path("/user/hand/right/input/thumbstick/click")},
            XrActionSuggestedBinding{grip_pose_action_, path("/user/hand/left/input/grip/pose")},
            XrActionSuggestedBinding{grip_pose_action_, path("/user/hand/right/input/grip/pose")},
            XrActionSuggestedBinding{aim_pose_action_, path("/user/hand/left/input/aim/pose")},
            XrActionSuggestedBinding{aim_pose_action_, path("/user/hand/right/input/aim/pose")},
            XrActionSuggestedBinding{right_trigger_action_, path("/user/hand/right/input/trigger/value")},
            XrActionSuggestedBinding{crouch_action_, path("/user/hand/right/input/a/click")},
            XrActionSuggestedBinding{jump_action_, path("/user/hand/right/input/b/click")},
            XrActionSuggestedBinding{reload_action_, path("/user/hand/left/input/x/click")},
            XrActionSuggestedBinding{flashlight_action_, path("/user/hand/left/input/y/click")},
            XrActionSuggestedBinding{grip_action_, path("/user/hand/left/input/squeeze/value")},
            XrActionSuggestedBinding{grip_action_, path("/user/hand/right/input/squeeze/value")},
            XrActionSuggestedBinding{menu_action_, path("/user/hand/left/input/menu/click")},
        };
        const std::array index_bindings{
            XrActionSuggestedBinding{left_thumbstick_action_, path("/user/hand/left/input/thumbstick")},
            XrActionSuggestedBinding{right_thumbstick_action_, path("/user/hand/right/input/thumbstick")},
            XrActionSuggestedBinding{left_thumbstick_click_action_, path("/user/hand/left/input/thumbstick/click")},
            XrActionSuggestedBinding{right_thumbstick_click_action_, path("/user/hand/right/input/thumbstick/click")},
            XrActionSuggestedBinding{grip_pose_action_, path("/user/hand/left/input/grip/pose")},
            XrActionSuggestedBinding{grip_pose_action_, path("/user/hand/right/input/grip/pose")},
            XrActionSuggestedBinding{aim_pose_action_, path("/user/hand/left/input/aim/pose")},
            XrActionSuggestedBinding{aim_pose_action_, path("/user/hand/right/input/aim/pose")},
            XrActionSuggestedBinding{right_trigger_action_, path("/user/hand/right/input/trigger/value")},
            XrActionSuggestedBinding{crouch_action_, path("/user/hand/right/input/a/click")},
            XrActionSuggestedBinding{jump_action_, path("/user/hand/right/input/b/click")},
            XrActionSuggestedBinding{reload_action_, path("/user/hand/left/input/a/click")},
            XrActionSuggestedBinding{flashlight_action_, path("/user/hand/left/input/b/click")},
            XrActionSuggestedBinding{grip_action_, path("/user/hand/left/input/squeeze/value")},
            XrActionSuggestedBinding{grip_action_, path("/user/hand/right/input/squeeze/value")},
            // Index has no dedicated application-menu button. Use a firm left
            // trackpad press so left A remains available for reload.
            XrActionSuggestedBinding{index_menu_force_action_, path("/user/hand/left/input/trackpad/force")},
        };
        auto suggest_bindings = [&](XrPath interaction_profile,
            const auto& bindings, const char* profile_name) {
            XrInteractionProfileSuggestedBinding suggested{
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested.interactionProfile = interaction_profile;
            suggested.countSuggestedBindings =
                static_cast<uint32_t>(bindings.size());
            suggested.suggestedBindings = bindings.data();
            check(xrSuggestInteractionProfileBindings(instance_, &suggested),
                std::format("xrSuggestInteractionProfileBindings({})",
                    profile_name).c_str());
        };
        suggest_bindings(touch_interaction_profile_, touch_bindings, "Touch");
        suggest_bindings(index_interaction_profile_, index_bindings, "Index");

        XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach_info.countActionSets = 1;
        attach_info.actionSets = &gameplay_action_set_;
        check(xrAttachSessionActionSets(session_, &attach_info),
            "xrAttachSessionActionSets(gameplay)");

        for (size_t hand = 0; hand < hand_paths_.size(); ++hand) {
            XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            space_info.poseInActionSpace.orientation.w = 1.0f;
            space_info.subactionPath = hand_paths_[hand];
            space_info.action = grip_pose_action_;
            check(xrCreateActionSpace(session_, &space_info, &grip_spaces_[hand]),
                "xrCreateActionSpace(grip)");
            space_info.action = aim_pose_action_;
            check(xrCreateActionSpace(session_, &space_info, &aim_spaces_[hand]),
                "xrCreateActionSpace(aim)");
        }
        xlog::info("[AFVR] OpenXR action set created");
    }

    bool OpenXrContext::sync_actions()
    {
        if (!session_running_ || gameplay_action_set_ == XR_NULL_HANDLE) {
            return false;
        }

        try {
            XrActiveActionSet active_action_set{gameplay_action_set_, XR_NULL_PATH};
            XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
            sync_info.countActiveActionSets = 1;
            sync_info.activeActionSets = &active_action_set;
            check(xrSyncActions(session_, &sync_info), "xrSyncActions");

            auto get_vector = [&](XrAction action, XrPath subaction) {
                XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
                get_info.action = action;
                get_info.subactionPath = subaction;
                XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
                check(xrGetActionStateVector2f(session_, &get_info, &state),
                    "xrGetActionStateVector2f");
                return state.isActive ? state.currentState : XrVector2f{};
            };
            auto get_float = [&](XrAction action, XrPath subaction) {
                XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
                get_info.action = action;
                get_info.subactionPath = subaction;
                XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
                check(xrGetActionStateFloat(session_, &get_info, &state),
                    "xrGetActionStateFloat");
                return state.isActive ? state.currentState : 0.0f;
            };
            auto get_boolean = [&](XrAction action, XrPath subaction) {
                XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
                get_info.action = action;
                get_info.subactionPath = subaction;
                XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                check(xrGetActionStateBoolean(session_, &get_info, &state),
                    "xrGetActionStateBoolean");
                return state.isActive && state.currentState;
            };

            input_state_.left_thumbstick = get_vector(left_thumbstick_action_, hand_paths_[0]);
            input_state_.right_thumbstick = get_vector(right_thumbstick_action_, hand_paths_[1]);
            input_state_.left_thumbstick_click =
                get_boolean(left_thumbstick_click_action_, hand_paths_[0]);
            input_state_.right_thumbstick_click =
                get_boolean(right_thumbstick_click_action_, hand_paths_[1]);
            input_state_.right_trigger = get_float(right_trigger_action_, hand_paths_[1]);
            input_state_.grip[0] = get_float(grip_action_, hand_paths_[0]);
            input_state_.grip[1] = get_float(grip_action_, hand_paths_[1]);
            input_state_.reload = get_boolean(reload_action_, hand_paths_[0]);
            input_state_.jump = get_boolean(jump_action_, hand_paths_[1]);
            input_state_.crouch = get_boolean(crouch_action_, hand_paths_[1]);
            input_state_.flashlight = get_boolean(flashlight_action_, hand_paths_[0]);
            input_state_.menu = get_boolean(menu_action_, hand_paths_[0]) ||
                get_float(index_menu_force_action_, hand_paths_[0]) >= 0.65f;

            if (!interaction_profile_logged_) {
                for (XrPath hand_path : hand_paths_) {
                    XrInteractionProfileState profile_state{
                        XR_TYPE_INTERACTION_PROFILE_STATE};
                    check(xrGetCurrentInteractionProfile(session_, hand_path, &profile_state),
                        "xrGetCurrentInteractionProfile");
                    if (profile_state.interactionProfile == XR_NULL_PATH) {
                        continue;
                    }
                    interaction_profile_logged_ = true;
                    if (profile_state.interactionProfile == touch_interaction_profile_) {
                        xlog::info("[AFVR] Oculus Touch profile active: left stick holster, left X reload, left Y flashlight, right A crouch, right B jump");
                    }
                    else if (profile_state.interactionProfile == index_interaction_profile_) {
                        xlog::info("[AFVR] Valve Index profile active: left stick holster, left A reload, left B flashlight, right A crouch, right B jump, left trackpad menu");
                    }
                    else {
                        std::array<char, XR_MAX_PATH_LENGTH> profile_name{};
                        uint32_t profile_name_length = 0;
                        check(xrPathToString(instance_, profile_state.interactionProfile,
                            static_cast<uint32_t>(profile_name.size()),
                            &profile_name_length, profile_name.data()),
                            "xrPathToString(interaction profile)");
                        xlog::info("[AFVR] Active interaction profile: {}",
                            profile_name.data());
                    }
                    if (interaction_profile_logged_) {
                        break;
                    }
                }
            }
            return true;
        }
        catch (const std::exception& error) {
            xlog::error("[AFVR] OpenXR input sync failed: {}", error.what());
            return false;
        }
    }

    void OpenXrContext::locate_hand_poses(XrTime time)
    {
        constexpr XrSpaceLocationFlags required =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        for (size_t hand = 0; hand < hand_paths_.size(); ++hand) {
            XrSpaceLocation grip_location{XR_TYPE_SPACE_LOCATION};
            XrSpaceLocation aim_location{XR_TYPE_SPACE_LOCATION};
            check(xrLocateSpace(grip_spaces_[hand], reference_space_, time, &grip_location),
                "xrLocateSpace(grip)");
            check(xrLocateSpace(aim_spaces_[hand], reference_space_, time, &aim_location),
                "xrLocateSpace(aim)");
            input_state_.grip_pose_valid[hand] =
                (grip_location.locationFlags & required) == required;
            input_state_.aim_pose_valid[hand] =
                (aim_location.locationFlags & required) == required;
            if (input_state_.grip_pose_valid[hand]) {
                input_state_.grip_poses[hand] = grip_location.pose;
            }
            if (input_state_.aim_pose_valid[hand]) {
                input_state_.aim_poses[hand] = aim_location.pose;
            }
            if (!controller_pose_logged_[hand] &&
                input_state_.grip_pose_valid[hand] && input_state_.aim_pose_valid[hand]) {
                controller_pose_logged_[hand] = true;
                xlog::info("[AFVR] {} controller pose valid", hand == 0 ? "left" : "right");
            }
        }
    }

    void OpenXrContext::create_swapchains(ID3D11Device* device)
    {
        uint32_t view_count = 0;
        check(xrEnumerateViewConfigurationViews(instance_, system_id_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr),
            "xrEnumerateViewConfigurationViews(count)");
        if (view_count != eye_swapchains_.size()) {
            throw std::runtime_error(std::format(
                "OpenXR PRIMARY_STEREO reported {} views; AlpineFaction VR requires 2", view_count));
        }

        std::vector<XrViewConfigurationView> view_configs(
            view_count, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
        check(xrEnumerateViewConfigurationViews(instance_, system_id_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, view_configs.data()),
            "xrEnumerateViewConfigurationViews(list)");

        uint32_t format_count = 0;
        check(xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr),
            "xrEnumerateSwapchainFormats(count)");
        std::vector<int64_t> formats(format_count);
        check(xrEnumerateSwapchainFormats(session_, format_count, &format_count, formats.data()),
            "xrEnumerateSwapchainFormats(list)");

        constexpr std::array preferred_formats{
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
        };
        DXGI_FORMAT selected_format = DXGI_FORMAT_UNKNOWN;
        for (DXGI_FORMAT preferred : preferred_formats) {
            if (std::ranges::find(formats, static_cast<int64_t>(preferred)) != formats.end()) {
                selected_format = preferred;
                break;
            }
        }
        if (selected_format == DXGI_FORMAT_UNKNOWN) {
            throw std::runtime_error("OpenXR runtime exposes no supported RGBA8/BGRA8 color format");
        }
        xlog::info("[AFVR] Selected swapchain format: {} ({})",
            format_name(selected_format), static_cast<int>(selected_format));
        const DXGI_FORMAT view_format = gamma_space_view_format(selected_format);
        if (view_format != selected_format) {
            xlog::info(
                "[AFVR] XR color pipeline: swapchain declared {}; RF writes and mirror samples through {} views",
                format_name(selected_format), format_name(view_format));
        }
        else {
            xlog::warn(
                "[AFVR] Runtime exposes no sRGB RGBA8 target; falling back to {}",
                format_name(selected_format));
        }

        uint32_t blend_mode_count = 0;
        check(xrEnumerateEnvironmentBlendModes(instance_, system_id_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blend_mode_count, nullptr),
            "xrEnumerateEnvironmentBlendModes(count)");
        std::vector<XrEnvironmentBlendMode> blend_modes(blend_mode_count);
        check(xrEnumerateEnvironmentBlendModes(instance_, system_id_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            blend_mode_count, &blend_mode_count, blend_modes.data()),
            "xrEnumerateEnvironmentBlendModes(list)");
        if (blend_modes.empty()) {
            throw std::runtime_error("OpenXR runtime exposes no environment blend modes");
        }
        auto opaque = std::ranges::find(blend_modes, XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        environment_blend_mode_ = opaque != blend_modes.end() ? *opaque : blend_modes.front();

        for (uint32_t eye = 0; eye < view_count; ++eye) {
            auto& swapchain = eye_swapchains_[eye];
            const auto& view_config = view_configs[eye];
            swapchain.width = static_cast<int32_t>(view_config.recommendedImageRectWidth);
            swapchain.height = static_cast<int32_t>(view_config.recommendedImageRectHeight);
            swapchain.format = selected_format;

            XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            create_info.format = static_cast<int64_t>(selected_format);
            create_info.sampleCount = 1;
            create_info.width = swapchain.width;
            create_info.height = swapchain.height;
            create_info.faceCount = 1;
            create_info.arraySize = 1;
            create_info.mipCount = 1;
            check(xrCreateSwapchain(session_, &create_info, &swapchain.handle),
                "xrCreateSwapchain");

            uint32_t image_count = 0;
            check(xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr),
                "xrEnumerateSwapchainImages(count)");
            swapchain.images.assign(image_count,
                XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            check(xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
                "xrEnumerateSwapchainImages(list)");

            swapchain.render_target_views.resize(image_count);
            swapchain.shader_resource_views.resize(image_count);
            swapchain.depth_textures.resize(image_count);
            swapchain.depth_stencil_views.resize(image_count);
            for (uint32_t image = 0; image < image_count; ++image) {
                D3D11_TEXTURE2D_DESC color_desc{};
                swapchain.images[image].texture->GetDesc(&color_desc);
                if (image == 0) {
                    xlog::info("[AFVR] Eye {} color texture: format {}, array {}, samples {}, bind 0x{:X}",
                        eye, static_cast<int>(color_desc.Format), color_desc.ArraySize,
                        color_desc.SampleDesc.Count, color_desc.BindFlags);
                }

                D3D11_RENDER_TARGET_VIEW_DESC render_target_desc{};
                // RF's legacy shaders already produce gamma-space values. Keep
                // the OpenXR swapchain declared sRGB for the compositor, but
                // write through a UNORM view to avoid a second sRGB encode.
                render_target_desc.Format = view_format;
                if (color_desc.ArraySize > 1) {
                    if (color_desc.SampleDesc.Count > 1) {
                        render_target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                        render_target_desc.Texture2DMSArray.FirstArraySlice = 0;
                        render_target_desc.Texture2DMSArray.ArraySize = color_desc.ArraySize;
                    }
                    else {
                        render_target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                        render_target_desc.Texture2DArray.MipSlice = 0;
                        render_target_desc.Texture2DArray.FirstArraySlice = 0;
                        render_target_desc.Texture2DArray.ArraySize = color_desc.ArraySize;
                    }
                }
                else if (color_desc.SampleDesc.Count > 1) {
                    render_target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
                }
                else {
                    render_target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    render_target_desc.Texture2D.MipSlice = 0;
                }

                HRESULT result = device->CreateRenderTargetView(
                    swapchain.images[image].texture, &render_target_desc,
                    &swapchain.render_target_views[image]);
                if (FAILED(result)) {
                    throw std::runtime_error(std::format(
                        "ID3D11Device::CreateRenderTargetView failed (0x{:08X})",
                        static_cast<unsigned>(result)));
                }

                D3D11_SHADER_RESOURCE_VIEW_DESC shader_resource_desc{};
                // Sample stored gamma-space values without decoding them. This
                // view is used only by the throttled desktop mirror.
                shader_resource_desc.Format = view_format;
                shader_resource_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                shader_resource_desc.Texture2D.MostDetailedMip = 0;
                shader_resource_desc.Texture2D.MipLevels = 1;
                result = device->CreateShaderResourceView(
                    swapchain.images[image].texture, &shader_resource_desc,
                    &swapchain.shader_resource_views[image]);
                if (FAILED(result)) {
                    throw std::runtime_error(std::format(
                        "ID3D11Device::CreateShaderResourceView failed (0x{:08X})",
                        static_cast<unsigned>(result)));
                }

                D3D11_TEXTURE2D_DESC depth_desc{};
                depth_desc.Width = swapchain.width;
                depth_desc.Height = swapchain.height;
                depth_desc.MipLevels = 1;
                depth_desc.ArraySize = 1;
                depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                depth_desc.SampleDesc.Count = 1;
                depth_desc.Usage = D3D11_USAGE_DEFAULT;
                depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                result = device->CreateTexture2D(&depth_desc, nullptr,
                    &swapchain.depth_textures[image]);
                if (FAILED(result)) {
                    throw std::runtime_error(std::format(
                        "ID3D11Device::CreateTexture2D(VR depth) failed (0x{:08X})",
                        static_cast<unsigned>(result)));
                }
                result = device->CreateDepthStencilView(swapchain.depth_textures[image], nullptr,
                    &swapchain.depth_stencil_views[image]);
                if (FAILED(result)) {
                    throw std::runtime_error(std::format(
                        "ID3D11Device::CreateDepthStencilView(VR depth) failed (0x{:08X})",
                        static_cast<unsigned>(result)));
                }
            }

            xlog::info("[AFVR] Eye {} swapchain: {}x{}, {}, {} images (runtime recommends {} sample{})",
                eye, swapchain.width, swapchain.height, format_name(selected_format), image_count,
                view_config.recommendedSwapchainSampleCount,
                view_config.recommendedSwapchainSampleCount == 1 ? "" : "s");
        }

        // The legacy menu is already composed in BGRA on the desktop target.
        // Use the matching OpenXR format so D3D11 can copy it without a second
        // UI renderer or any widget changes.
        DXGI_FORMAT menu_format = DXGI_FORMAT_UNKNOWN;
        for (DXGI_FORMAT candidate : {
                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                DXGI_FORMAT_B8G8R8A8_UNORM}) {
            if (std::ranges::find(formats, static_cast<int64_t>(candidate)) != formats.end()) {
                menu_format = candidate;
                break;
            }
        }
        if (menu_format != DXGI_FORMAT_UNKNOWN) {
            menu_swapchain_.width = std::max(rf::gr::screen.max_w, 1);
            menu_swapchain_.height = std::max(rf::gr::screen.max_h, 1);
            menu_swapchain_.format = menu_format;
            XrSwapchainCreateInfo menu_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            menu_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            menu_info.format = static_cast<int64_t>(menu_format);
            menu_info.sampleCount = 1;
            menu_info.width = menu_swapchain_.width;
            menu_info.height = menu_swapchain_.height;
            menu_info.faceCount = 1;
            menu_info.arraySize = 1;
            menu_info.mipCount = 1;
            check(xrCreateSwapchain(session_, &menu_info, &menu_swapchain_.handle),
                "xrCreateSwapchain(menu)");
            uint32_t menu_image_count = 0;
            check(xrEnumerateSwapchainImages(menu_swapchain_.handle, 0,
                &menu_image_count, nullptr), "xrEnumerateSwapchainImages(menu count)");
            menu_swapchain_.images.assign(menu_image_count,
                XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            check(xrEnumerateSwapchainImages(menu_swapchain_.handle, menu_image_count,
                &menu_image_count, reinterpret_cast<XrSwapchainImageBaseHeader*>(
                    menu_swapchain_.images.data())),
                "xrEnumerateSwapchainImages(menu list)");
            xlog::info("[AFVR] Menu quad swapchain: {}x{}, {}, {} images",
                menu_swapchain_.width, menu_swapchain_.height,
                format_name(menu_format), menu_image_count);
        }
        else {
            xlog::warn("[AFVR] Runtime exposes no BGRA8 format for the RF menu quad layer");
        }

        // The HUD is rendered directly into a separate alpha-capable target.
        // It deliberately does not share the menu swapchain: gameplay HUD and
        // menu visibility are mutually exclusive, independently owned states.
        hud_swapchain_.width = std::max(rf::gr::screen.max_w, 1);
        hud_swapchain_.height = std::max(rf::gr::screen.max_h, 1);
        hud_swapchain_.format = selected_format;
        XrSwapchainCreateInfo hud_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        hud_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        hud_info.format = static_cast<int64_t>(selected_format);
        hud_info.sampleCount = 1;
        hud_info.width = hud_swapchain_.width;
        hud_info.height = hud_swapchain_.height;
        hud_info.faceCount = 1;
        hud_info.arraySize = 1;
        hud_info.mipCount = 1;
        check(xrCreateSwapchain(session_, &hud_info, &hud_swapchain_.handle),
            "xrCreateSwapchain(HUD)");
        uint32_t hud_image_count = 0;
        check(xrEnumerateSwapchainImages(hud_swapchain_.handle, 0,
            &hud_image_count, nullptr), "xrEnumerateSwapchainImages(HUD count)");
        hud_swapchain_.images.assign(hud_image_count,
            XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        check(xrEnumerateSwapchainImages(hud_swapchain_.handle, hud_image_count,
            &hud_image_count, reinterpret_cast<XrSwapchainImageBaseHeader*>(
                hud_swapchain_.images.data())),
            "xrEnumerateSwapchainImages(HUD list)");
        hud_swapchain_.render_target_views.resize(hud_image_count);
        const DXGI_FORMAT hud_view_format = gamma_space_view_format(selected_format);
        for (uint32_t image = 0; image < hud_image_count; ++image) {
            D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
            view_desc.Format = hud_view_format;
            view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            view_desc.Texture2D.MipSlice = 0;
            HRESULT result = device->CreateRenderTargetView(
                hud_swapchain_.images[image].texture, &view_desc,
                &hud_swapchain_.render_target_views[image]);
            if (FAILED(result)) {
                throw std::runtime_error(std::format(
                    "ID3D11Device::CreateRenderTargetView(HUD) failed (0x{:08X})",
                    static_cast<unsigned>(result)));
            }
        }
        xlog::info("[AFVR] HUD quad swapchain: {}x{}, {}, {} images; {:.2f} m wide at {:.1f} m",
            hud_swapchain_.width, hud_swapchain_.height,
            format_name(selected_format), hud_image_count,
            hud_width_m, hud_distance_m);
        xlog::info("[AFVR] Environment blend mode: {}", blend_mode_name(environment_blend_mode_));
    }

    bool OpenXrContext::poll_events()
    {
        if (instance_ == XR_NULL_HANDLE) {
            return false;
        }

        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (true) {
            XrResult result = xrPollEvent(instance_, &event);
            if (result == XR_EVENT_UNAVAILABLE) {
                break;
            }
            if (XR_FAILED(result)) {
                xlog::error("[AFVR] xrPollEvent failed ({})", static_cast<int>(result));
                return false;
            }

            switch (event.type) {
                case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
                    auto& lost = reinterpret_cast<const XrEventDataEventsLost&>(event);
                    xlog::warn("[AFVR] OpenXR runtime reported {} lost events", lost.lostEventCount);
                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    xlog::warn("[AFVR] OpenXR instance loss is pending");
                    exit_requested_ = true;
                    break;
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                    handle_session_state_changed(
                        reinterpret_cast<const XrEventDataSessionStateChanged&>(event));
                    break;
#ifdef XR_FB_display_refresh_rate
                case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                    const auto& refresh_event =
                        reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(event);
                    xlog::info(
                        "[AFVR] Runtime changed display refresh rate from {:.2f} to {:.2f} Hz; frame pacing follows automatically",
                        refresh_event.fromDisplayRefreshRate,
                        refresh_event.toDisplayRefreshRate);
                    break;
                }
#endif
                default:
                    xlog::trace("[AFVR] OpenXR event type {}", static_cast<int>(event.type));
                    break;
            }

            event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }

        return !exit_requested_;
    }

    void OpenXrContext::handle_session_state_changed(const XrEventDataSessionStateChanged& event)
    {
        if (event.session != XR_NULL_HANDLE && event.session != session_) {
            xlog::warn("[AFVR] Ignoring state change for an unknown OpenXR session");
            return;
        }

        session_state_ = event.state;
        xlog::info("[AFVR] Session state: {}", session_state_name(session_state_));

        switch (session_state_) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult result = xrBeginSession(session_, &begin_info);
                if (XR_FAILED(result)) {
                    xlog::error("[AFVR] xrBeginSession failed ({})", static_cast<int>(result));
                    exit_requested_ = true;
                }
                else {
                    session_running_ = true;
                    xlog::info("[AFVR] OpenXR session begun");
                    report_display_refresh_rate();
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                if (session_running_) {
                    if (frame_begun_) {
                        XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
                        frame_end_info.displayTime = waited_frame_state_.predictedDisplayTime;
                        frame_end_info.environmentBlendMode = environment_blend_mode_;
                        const XrResult frame_result = xrEndFrame(session_, &frame_end_info);
                        if (XR_FAILED(frame_result)) {
                            xlog::warn("[AFVR] xrEndFrame during session stop failed ({})",
                                static_cast<int>(frame_result));
                        }
                        frame_waited_ = false;
                        frame_begun_ = false;
                    }
                    XrResult result = xrEndSession(session_);
                    if (XR_FAILED(result)) {
                        xlog::error("[AFVR] xrEndSession failed ({})", static_cast<int>(result));
                        exit_requested_ = true;
                    }
                    session_running_ = false;
                    xlog::info("[AFVR] OpenXR session ended");
                }
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                exit_requested_ = true;
                break;
            default:
                break;
        }
    }

    bool OpenXrContext::wait_frame()
    {
        if (!session_running_ || exit_requested_) {
            return false;
        }

        try {
            // A non-rendering RF frame may never reach either the world or menu
            // submission hook. Complete its already-begun XR frame without
            // layers before waiting again.
            if (frame_waited_) {
                XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
                end_info.displayTime = waited_frame_state_.predictedDisplayTime;
                end_info.environmentBlendMode = environment_blend_mode_;
                const auto end_start = std::chrono::steady_clock::now();
                check(xrEndFrame(session_, &end_info), "xrEndFrame(empty)");
                timing_note_phase(TimingPhase::end_frame,
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - end_start).count());
                frame_waited_ = false;
                frame_begun_ = false;
            }

            XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frame_state{XR_TYPE_FRAME_STATE};
            using Clock = std::chrono::steady_clock;
            static Clock::time_point previous_wait_return{};
            static XrTime previous_predicted_display_time = 0;
            const auto wait_start = Clock::now();
            check(xrWaitFrame(session_, &wait_info, &frame_state), "xrWaitFrame");
            const auto wait_return = Clock::now();
            const double wait_ms = std::chrono::duration<double, std::milli>(
                wait_return - wait_start).count();
            double return_interval_ms = previous_wait_return == Clock::time_point{}
                ? 0.0
                : std::chrono::duration<double, std::milli>(
                    wait_return - previous_wait_return).count();
            if (return_interval_ms > 1000.0) {
                return_interval_ms = 0.0;
            }
            previous_wait_return = wait_return;
            double predicted_interval_ms = 0.0;
            if (previous_predicted_display_time > 0 &&
                frame_state.predictedDisplayTime > previous_predicted_display_time) {
                predicted_interval_ms = static_cast<double>(
                    frame_state.predictedDisplayTime - previous_predicted_display_time) /
                    1'000'000.0;
                if (predicted_interval_ms > 1000.0) {
                    predicted_interval_ms = 0.0;
                }
            }
            previous_predicted_display_time = frame_state.predictedDisplayTime;
            const double runtime_target_hz = frame_state.predictedDisplayPeriod > 0
                ? 1'000'000'000.0 /
                    static_cast<double>(frame_state.predictedDisplayPeriod)
                : 0.0;
            timing_note_xr_wait(wait_ms, return_interval_ms,
                predicted_interval_ms, runtime_target_hz);
            waited_frame_state_ = frame_state;

            // Begin immediately after the runtime pacing wait. Delaying Begin
            // until RF reaches its portal/menu render hook made Virtual Desktop
            // account the intervening game work outside the runtime's frame
            // budget, producing a lower submission cadence than the headset.
            XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            const auto begin_start = Clock::now();
            check(xrBeginFrame(session_, &begin_info), "xrBeginFrame");
            timing_note_phase(TimingPhase::begin_frame,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - begin_start).count());
            frame_waited_ = true;
            frame_begun_ = true;
            if (!first_wait_frame_logged_) {
                first_wait_frame_logged_ = true;
                xlog::info("[AFVR] First xrWaitFrame succeeded");
            }
            if (!first_begin_frame_logged_) {
                first_begin_frame_logged_ = true;
                xlog::info("[AFVR] xrBeginFrame now follows xrWaitFrame immediately");
            }
            if (!display_period_logged_ && frame_state.predictedDisplayPeriod > 0) {
                display_period_logged_ = true;
                constexpr double nanoseconds_per_millisecond = 1'000'000.0;
                constexpr double nanoseconds_per_second = 1'000'000'000.0;
                const double period_ms = static_cast<double>(
                    frame_state.predictedDisplayPeriod) / nanoseconds_per_millisecond;
                const double refresh_hz = nanoseconds_per_second /
                    static_cast<double>(frame_state.predictedDisplayPeriod);
                xlog::info("[AFVR] XR display period: {:.3f} ms", period_ms);
                xlog::info("[AFVR] XR target refresh approximately: {:.2f} Hz", refresh_hz);
            }
            return true;
        }
        catch (const std::exception& error) {
            xlog::error("[AFVR] OpenXR frame wait failed: {}", error.what());
            exit_requested_ = true;
            frame_waited_ = false;
            frame_begun_ = false;
            return false;
        }
    }

    bool OpenXrContext::render_frame(const OpenXrEyeRenderCallback& render_eye,
        const OpenXrHudRenderCallback& render_hud)
    {
        if (!session_running_ || exit_requested_) {
            return false;
        }
        if (!frame_waited_ && !wait_frame()) {
            return false;
        }

        const XrFrameState frame_state = waited_frame_state_;
        frame_waited_ = false;
        bool frame_begun = frame_begun_;
        bool hud_image_acquired = false;
        const XrTime predicted_display_time = frame_state.predictedDisplayTime;
        try {
            std::array<XrCompositionLayerProjectionView, 2> projection_views{};
            std::array<const XrCompositionLayerBaseHeader*, 2> submitted_layers{};
            uint32_t submitted_layer_count = 0;
            bool projection_submitted = false;
            XrCompositionLayerProjection projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            XrCompositionLayerQuad hud_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};

            if (frame_state.shouldRender) {
                XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
                locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locate_info.displayTime = frame_state.predictedDisplayTime;
                locate_info.space = reference_space_;
                XrViewState view_state{XR_TYPE_VIEW_STATE};
                std::array<XrView, 2> views{
                    XrView{XR_TYPE_VIEW},
                    XrView{XR_TYPE_VIEW},
                };
                uint32_t view_count = 0;
                check(xrLocateViews(session_, &locate_info, &view_state,
                    static_cast<uint32_t>(views.size()), &view_count, views.data()),
                    "xrLocateViews");

                constexpr XrViewStateFlags required_flags =
                    XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
                if (view_count == views.size() &&
                    (view_state.viewStateFlags & required_flags) == required_flags) {
                    locate_hand_poses(frame_state.predictedDisplayTime);
                    if (!first_valid_views_logged_) {
                        first_valid_views_logged_ = true;
                        xlog::info("[AFVR] First valid stereo views located");
                    }
                    XrPosef center_pose = views[0].pose;
                    center_pose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                    center_pose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                    center_pose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
                    for (uint32_t eye = 0; eye < views.size(); ++eye) {
                        auto& swapchain = eye_swapchains_[eye];
                        uint32_t image_index = 0;
                        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        const auto image_wait_start = std::chrono::steady_clock::now();
                        check(xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &image_index),
                            "xrAcquireSwapchainImage");
                        if (!first_eye_acquired_logged_[eye]) {
                            first_eye_acquired_logged_[eye] = true;
                            xlog::info("[AFVR] First {}-eye image acquired", eye == 0 ? "left" : "right");
                        }

                        bool image_acquired = true;
                        try {
                            XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                            image_wait.timeout = XR_INFINITE_DURATION;
                            check(xrWaitSwapchainImage(swapchain.handle, &image_wait),
                                "xrWaitSwapchainImage");
                            timing_note_phase(TimingPhase::eye_image_wait,
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - image_wait_start).count());

                            const auto render_start = std::chrono::steady_clock::now();
                            render_eye(OpenXrEyeRenderInfo{
                                eye,
                                views[eye],
                                center_pose,
                                swapchain.render_target_views[image_index],
                                swapchain.shader_resource_views[image_index],
                                swapchain.depth_stencil_views[image_index],
                                swapchain.width,
                                swapchain.height,
                            });
                            timing_note_phase(TimingPhase::eye_render,
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - render_start).count());

                            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            const auto release_start = std::chrono::steady_clock::now();
                            check(xrReleaseSwapchainImage(swapchain.handle, &release_info),
                                "xrReleaseSwapchainImage");
                            timing_note_phase(TimingPhase::eye_release,
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - release_start).count());
                            image_acquired = false;
                        }
                        catch (...) {
                            if (image_acquired) {
                                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                                xrReleaseSwapchainImage(swapchain.handle, &release_info);
                            }
                            throw;
                        }

                        auto& layer_view = projection_views[eye];
                        layer_view = XrCompositionLayerProjectionView{
                            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                        layer_view.pose = views[eye].pose;
                        layer_view.fov = views[eye].fov;
                        layer_view.subImage.swapchain = swapchain.handle;
                        layer_view.subImage.imageRect.extent = {
                            swapchain.width, swapchain.height};
                        layer_view.subImage.imageArrayIndex = 0;
                    }

                    projection_layer.space = reference_space_;
                    projection_layer.viewCount = static_cast<uint32_t>(projection_views.size());
                    projection_layer.views = projection_views.data();
                    submitted_layers[submitted_layer_count++] =
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection_layer);
                    projection_submitted = true;

                    if (hud_active_ && render_hud &&
                        hud_swapchain_.handle != XR_NULL_HANDLE) {
                        uint32_t hud_image_index = 0;
                        XrSwapchainImageAcquireInfo acquire_info{
                            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        const auto hud_wait_start = std::chrono::steady_clock::now();
                        check(xrAcquireSwapchainImage(hud_swapchain_.handle,
                            &acquire_info, &hud_image_index),
                            "xrAcquireSwapchainImage(HUD)");
                        hud_image_acquired = true;
                        XrSwapchainImageWaitInfo image_wait{
                            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        image_wait.timeout = XR_INFINITE_DURATION;
                        check(xrWaitSwapchainImage(hud_swapchain_.handle, &image_wait),
                            "xrWaitSwapchainImage(HUD)");
                        timing_note_phase(TimingPhase::hud_image_wait,
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - hud_wait_start).count());
                        const auto hud_render_start = std::chrono::steady_clock::now();
                        render_hud(OpenXrHudRenderInfo{
                            hud_swapchain_.render_target_views[hud_image_index],
                            hud_swapchain_.width,
                            hud_swapchain_.height,
                        });
                        timing_note_phase(TimingPhase::hud_render,
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - hud_render_start).count());
                        XrSwapchainImageReleaseInfo release_info{
                            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        const auto hud_release_start = std::chrono::steady_clock::now();
                        check(xrReleaseSwapchainImage(hud_swapchain_.handle, &release_info),
                            "xrReleaseSwapchainImage(HUD)");
                        timing_note_phase(TimingPhase::hud_release,
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - hud_release_start).count());
                        hud_image_acquired = false;

                        hud_layer.layerFlags =
                            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        hud_layer.space = reference_space_;
                        hud_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        hud_layer.pose = center_pose;
                        const XrVector3f forward = rotate_vector(
                            center_pose.orientation, {0.0f, 0.0f, -hud_distance_m});
                        hud_layer.pose.position.x += forward.x;
                        hud_layer.pose.position.y += forward.y;
                        hud_layer.pose.position.z += forward.z;
                        hud_layer.size.width = hud_width_m;
                        hud_layer.size.height = hud_width_m *
                            static_cast<float>(hud_swapchain_.height) /
                            static_cast<float>(hud_swapchain_.width);
                        hud_layer.subImage.swapchain = hud_swapchain_.handle;
                        hud_layer.subImage.imageRect.extent = {
                            hud_swapchain_.width, hud_swapchain_.height};
                        hud_layer.subImage.imageArrayIndex = 0;
                        submitted_layers[submitted_layer_count++] =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud_layer);
                    }
                }
            }

            XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
            end_info.displayTime = frame_state.predictedDisplayTime;
            end_info.environmentBlendMode = environment_blend_mode_;
            end_info.layerCount = submitted_layer_count;
            end_info.layers = submitted_layer_count ? submitted_layers.data() : nullptr;
            const auto end_start = std::chrono::steady_clock::now();
            check(xrEndFrame(session_, &end_info), "xrEndFrame");
            timing_note_phase(TimingPhase::end_frame,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - end_start).count());
            frame_begun = false;
            frame_begun_ = false;

            if (projection_submitted && !first_frame_logged_) {
                first_frame_logged_ = true;
                xlog::info("[AFVR] First stereo world frame submitted (both eyes)");
                xlog::info("[AFVR] First Red Faction world render submitted");
            }
            if (projection_submitted && !first_projection_layer_logged_) {
                first_projection_layer_logged_ = true;
                xlog::info("[AFVR] First projection layer submitted");
                xlog::info("[AFVR] XR frame submission started");
            }
            if (submitted_layer_count > 1 && !first_hud_layer_logged_) {
                first_hud_layer_logged_ = true;
                xlog::info("[AFVR] First gameplay HUD OpenXR quad layer submitted");
            }
            if (projection_submitted) {
                timing_note_xr_submission();
                ++submitted_frame_count_;
                if (submitted_frame_count_ >= 300 && !sustained_submission_logged_) {
                    sustained_submission_logged_ = true;
                    xlog::info("[AFVR] Stereo submission heartbeat: 300 frames completed");
                }
            }
            return projection_submitted;
        }
        catch (const std::exception& error) {
            if (hud_image_acquired) {
                XrSwapchainImageReleaseInfo release_info{
                    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(hud_swapchain_.handle, &release_info);
            }
            if (frame_begun) {
                XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
                end_info.displayTime = predicted_display_time;
                end_info.environmentBlendMode = environment_blend_mode_;
                xrEndFrame(session_, &end_info);
            }
            frame_begun_ = false;
            xlog::error("[AFVR] OpenXR frame failed: {}", error.what());
            exit_requested_ = true;
            return false;
        }
    }

    void OpenXrContext::set_menu_active(bool active)
    {
        if (menu_active_ == active) {
            return;
        }
        menu_active_ = active;
        menu_pose_valid_ = false;
    }

    void OpenXrContext::set_hud_active(bool active)
    {
        if (hud_active_ == active) {
            return;
        }
        hud_active_ = active;
        xlog::info("[AFVR] Gameplay HUD quad {}",
            active ? "enabled" : "hidden");
    }

    bool OpenXrContext::get_menu_pointer(float& u, float& v) const
    {
        constexpr size_t right_hand = 1;
        if (!menu_active_ || !menu_pose_valid_ ||
            !input_state_.aim_pose_valid[right_hand] ||
            menu_swapchain_.width <= 0 || menu_swapchain_.height <= 0) {
            return false;
        }

        const XrPosef& aim = input_state_.aim_poses[right_hand];
        const XrVector3f world_origin{
            aim.position.x - menu_pose_.position.x,
            aim.position.y - menu_pose_.position.y,
            aim.position.z - menu_pose_.position.z,
        };
        const XrVector3f world_direction = rotate_vector(
            aim.orientation, {0.0f, 0.0f, -1.0f});
        const XrQuaternionf inverse_menu{
            -menu_pose_.orientation.x,
            -menu_pose_.orientation.y,
            -menu_pose_.orientation.z,
            menu_pose_.orientation.w,
        };
        const XrVector3f local_origin = rotate_vector(inverse_menu, world_origin);
        const XrVector3f local_direction = rotate_vector(inverse_menu, world_direction);
        if (std::abs(local_direction.z) < 0.0001f) {
            return false;
        }
        const float distance = -local_origin.z / local_direction.z;
        if (distance <= 0.0f) {
            return false;
        }
        const float aspect = static_cast<float>(menu_swapchain_.width) /
            static_cast<float>(menu_swapchain_.height);
        const float menu_height_m = menu_width_m / aspect;
        const float local_x = local_origin.x + local_direction.x * distance;
        const float local_y = local_origin.y + local_direction.y * distance;
        u = local_x / menu_width_m + 0.5f;
        v = 0.5f - local_y / menu_height_m;
        return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
    }

    bool OpenXrContext::render_menu_frame(const OpenXrMenuRenderCallback& render_menu)
    {
        if (!session_running_ || exit_requested_ || !menu_active_ ||
            menu_swapchain_.handle == XR_NULL_HANDLE) {
            return false;
        }
        if (!frame_waited_ && !wait_frame()) {
            return false;
        }

        const XrFrameState frame_state = waited_frame_state_;
        frame_waited_ = false;
        bool frame_begun = frame_begun_;
        bool image_acquired = false;
        uint32_t image_index = 0;
        try {
            const XrCompositionLayerBaseHeader* submitted_layer = nullptr;
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            if (frame_state.shouldRender) {
                XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
                locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locate_info.displayTime = frame_state.predictedDisplayTime;
                locate_info.space = reference_space_;
                XrViewState view_state{XR_TYPE_VIEW_STATE};
                std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
                uint32_t view_count = 0;
                check(xrLocateViews(session_, &locate_info, &view_state,
                    static_cast<uint32_t>(views.size()), &view_count, views.data()),
                    "xrLocateViews(menu)");
                constexpr XrViewStateFlags required_flags =
                    XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
                if (view_count == views.size() &&
                    (view_state.viewStateFlags & required_flags) == required_flags) {
                    locate_hand_poses(frame_state.predictedDisplayTime);
                    XrPosef center_pose = views[0].pose;
                    center_pose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                    center_pose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                    center_pose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
                    if (!menu_pose_valid_) {
                        menu_pose_ = center_pose;
                        const XrVector3f forward = rotate_vector(
                            center_pose.orientation, {0.0f, 0.0f, -menu_distance_m});
                        menu_pose_.position.x += forward.x;
                        menu_pose_.position.y += forward.y;
                        menu_pose_.position.z += forward.z;
                        menu_pose_valid_ = true;
                        xlog::info(
                            "[AFVR] RF menu quad anchored {:.1f} m ahead of the HMD; width {:.3f} m",
                            menu_distance_m, menu_width_m);
                    }

                    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    const auto menu_wait_start = std::chrono::steady_clock::now();
                    check(xrAcquireSwapchainImage(menu_swapchain_.handle,
                        &acquire_info, &image_index), "xrAcquireSwapchainImage(menu)");
                    image_acquired = true;
                    XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    image_wait.timeout = XR_INFINITE_DURATION;
                    check(xrWaitSwapchainImage(menu_swapchain_.handle, &image_wait),
                        "xrWaitSwapchainImage(menu)");
                    timing_note_phase(TimingPhase::menu_image_wait,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - menu_wait_start).count());
                    const auto menu_copy_start = std::chrono::steady_clock::now();
                    render_menu(OpenXrMenuRenderInfo{
                        menu_swapchain_.images[image_index].texture,
                        menu_swapchain_.width,
                        menu_swapchain_.height,
                    });
                    timing_note_phase(TimingPhase::menu_copy,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - menu_copy_start).count());
                    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    const auto menu_release_start = std::chrono::steady_clock::now();
                    check(xrReleaseSwapchainImage(menu_swapchain_.handle, &release_info),
                        "xrReleaseSwapchainImage(menu)");
                    timing_note_phase(TimingPhase::menu_release,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - menu_release_start).count());
                    image_acquired = false;

                    quad.space = reference_space_;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.pose = menu_pose_;
                    quad.size.width = menu_width_m;
                    quad.size.height = menu_width_m *
                        static_cast<float>(menu_swapchain_.height) /
                        static_cast<float>(menu_swapchain_.width);
                    quad.subImage.swapchain = menu_swapchain_.handle;
                    quad.subImage.imageRect.extent = {
                        menu_swapchain_.width, menu_swapchain_.height};
                    quad.subImage.imageArrayIndex = 0;
                    submitted_layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                }
            }

            XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
            end_info.displayTime = frame_state.predictedDisplayTime;
            end_info.environmentBlendMode = environment_blend_mode_;
            end_info.layerCount = submitted_layer ? 1u : 0u;
            end_info.layers = submitted_layer ? &submitted_layer : nullptr;
            const auto end_start = std::chrono::steady_clock::now();
            check(xrEndFrame(session_, &end_info), "xrEndFrame(menu)");
            timing_note_phase(TimingPhase::end_frame,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - end_start).count());
            frame_begun = false;
            frame_begun_ = false;
            if (submitted_layer) {
                timing_note_xr_submission();
                if (!first_menu_layer_logged_) {
                    first_menu_layer_logged_ = true;
                    xlog::info("[AFVR] First RF menu OpenXR quad layer submitted");
                }
            }
            return submitted_layer != nullptr;
        }
        catch (const std::exception& error) {
            if (image_acquired) {
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(menu_swapchain_.handle, &release_info);
            }
            if (frame_begun) {
                XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
                end_info.displayTime = frame_state.predictedDisplayTime;
                end_info.environmentBlendMode = environment_blend_mode_;
                xrEndFrame(session_, &end_info);
            }
            frame_begun_ = false;
            xlog::error("[AFVR] OpenXR menu frame failed: {}", error.what());
            exit_requested_ = true;
            return false;
        }
    }

    void OpenXrContext::destroy_swapchains()
    {
        for (auto& swapchain : eye_swapchains_) {
            swapchain.depth_stencil_views.clear();
            swapchain.depth_textures.clear();
            swapchain.shader_resource_views.clear();
            swapchain.render_target_views.clear();
            swapchain.images.clear();
            if (swapchain.handle != XR_NULL_HANDLE) {
                XrResult result = xrDestroySwapchain(swapchain.handle);
                if (XR_FAILED(result)) {
                    xlog::warn("[AFVR] xrDestroySwapchain failed ({})", static_cast<int>(result));
                }
                swapchain.handle = XR_NULL_HANDLE;
            }
            swapchain.width = 0;
            swapchain.height = 0;
            swapchain.format = DXGI_FORMAT_UNKNOWN;
        }
        menu_swapchain_.images.clear();
        menu_swapchain_.render_target_views.clear();
        if (menu_swapchain_.handle != XR_NULL_HANDLE) {
            XrResult result = xrDestroySwapchain(menu_swapchain_.handle);
            if (XR_FAILED(result)) {
                xlog::warn("[AFVR] xrDestroySwapchain(menu) failed ({})", static_cast<int>(result));
            }
            menu_swapchain_.handle = XR_NULL_HANDLE;
        }
        menu_swapchain_.width = 0;
        menu_swapchain_.height = 0;
        menu_swapchain_.format = DXGI_FORMAT_UNKNOWN;
        menu_active_ = false;
        menu_pose_valid_ = false;

        hud_swapchain_.render_target_views.clear();
        hud_swapchain_.images.clear();
        if (hud_swapchain_.handle != XR_NULL_HANDLE) {
            XrResult result = xrDestroySwapchain(hud_swapchain_.handle);
            if (XR_FAILED(result)) {
                xlog::warn("[AFVR] xrDestroySwapchain(HUD) failed ({})",
                    static_cast<int>(result));
            }
            hud_swapchain_.handle = XR_NULL_HANDLE;
        }
        hud_swapchain_.width = 0;
        hud_swapchain_.height = 0;
        hud_swapchain_.format = DXGI_FORMAT_UNKNOWN;
        hud_active_ = false;
    }

    void OpenXrContext::shutdown()
    {
        if (session_ != XR_NULL_HANDLE && frame_begun_) {
            XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
            end_info.displayTime = waited_frame_state_.predictedDisplayTime;
            end_info.environmentBlendMode = environment_blend_mode_;
            const XrResult result = xrEndFrame(session_, &end_info);
            if (XR_FAILED(result)) {
                xlog::warn("[AFVR] xrEndFrame during shutdown failed ({})",
                    static_cast<int>(result));
            }
            frame_waited_ = false;
            frame_begun_ = false;
        }

        destroy_swapchains();

        for (XrSpace& space : grip_spaces_) {
            if (space != XR_NULL_HANDLE) {
                xrDestroySpace(space);
                space = XR_NULL_HANDLE;
            }
        }
        for (XrSpace& space : aim_spaces_) {
            if (space != XR_NULL_HANDLE) {
                xrDestroySpace(space);
                space = XR_NULL_HANDLE;
            }
        }

        if (reference_space_ != XR_NULL_HANDLE) {
            XrResult result = xrDestroySpace(reference_space_);
            if (XR_FAILED(result)) {
                xlog::warn("[AFVR] xrDestroySpace failed ({})", static_cast<int>(result));
            }
            reference_space_ = XR_NULL_HANDLE;
        }

        if (session_ != XR_NULL_HANDLE) {
            XrResult result = xrDestroySession(session_);
            if (XR_FAILED(result)) {
                xlog::warn("[AFVR] xrDestroySession failed ({})", static_cast<int>(result));
            }
            session_ = XR_NULL_HANDLE;
        }

        if (gameplay_action_set_ != XR_NULL_HANDLE) {
            XrResult result = xrDestroyActionSet(gameplay_action_set_);
            if (XR_FAILED(result)) {
                xlog::warn("[AFVR] xrDestroyActionSet failed ({})", static_cast<int>(result));
            }
            gameplay_action_set_ = XR_NULL_HANDLE;
        }

        if (instance_ != XR_NULL_HANDLE) {
            XrResult result = xrDestroyInstance(instance_);
            if (XR_FAILED(result)) {
                xlog::warn("[AFVR] xrDestroyInstance failed ({})", static_cast<int>(result));
            }
            instance_ = XR_NULL_HANDLE;
            xlog::info("[AFVR] OpenXR shutdown complete");
        }

        system_id_ = XR_NULL_SYSTEM_ID;
        session_state_ = XR_SESSION_STATE_UNKNOWN;
        session_running_ = false;
        exit_requested_ = false;
        frame_waited_ = false;
        frame_begun_ = false;
        waited_frame_state_ = XrFrameState{XR_TYPE_FRAME_STATE};
        display_refresh_rate_supported_ = false;
        first_frame_logged_ = false;
        first_wait_frame_logged_ = false;
        display_period_logged_ = false;
        first_begin_frame_logged_ = false;
        first_valid_views_logged_ = false;
        first_eye_acquired_logged_.fill(false);
        first_projection_layer_logged_ = false;
        first_menu_layer_logged_ = false;
        first_hud_layer_logged_ = false;
        submitted_frame_count_ = 0;
        sustained_submission_logged_ = false;
        interaction_profile_logged_ = false;
        controller_pose_logged_.fill(false);
        input_state_ = {};
        hand_paths_.fill(XR_NULL_PATH);
        touch_interaction_profile_ = XR_NULL_PATH;
        index_interaction_profile_ = XR_NULL_PATH;
        left_thumbstick_action_ = XR_NULL_HANDLE;
        right_thumbstick_action_ = XR_NULL_HANDLE;
        left_thumbstick_click_action_ = XR_NULL_HANDLE;
        right_thumbstick_click_action_ = XR_NULL_HANDLE;
        grip_pose_action_ = XR_NULL_HANDLE;
        aim_pose_action_ = XR_NULL_HANDLE;
        right_trigger_action_ = XR_NULL_HANDLE;
        reload_action_ = XR_NULL_HANDLE;
        jump_action_ = XR_NULL_HANDLE;
        crouch_action_ = XR_NULL_HANDLE;
        flashlight_action_ = XR_NULL_HANDLE;
        grip_action_ = XR_NULL_HANDLE;
        menu_action_ = XR_NULL_HANDLE;
        index_menu_force_action_ = XR_NULL_HANDLE;
        environment_blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        get_d3d11_graphics_requirements_ = nullptr;
        enumerate_display_refresh_rates_ = nullptr;
        get_display_refresh_rate_ = nullptr;
    }

    void OpenXrContext::check(XrResult result, const char* operation) const
    {
        if (XR_SUCCEEDED(result)) {
            return;
        }

        if (instance_ != XR_NULL_HANDLE) {
            std::array<char, XR_MAX_RESULT_STRING_SIZE> result_string{};
            if (XR_SUCCEEDED(xrResultToString(instance_, result, result_string.data()))) {
                throw std::runtime_error(std::format("{} failed: {} ({})",
                    operation, result_string.data(), static_cast<int>(result)));
            }
        }

        throw std::runtime_error(std::format("{} failed ({})", operation, static_cast<int>(result)));
    }
}
