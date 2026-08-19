// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/gr/gr.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11DepthStencilView;
struct ID3D11Texture2D;

namespace afvr
{
    struct D3D11RendererBinding
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;

        [[nodiscard]] explicit operator bool() const
        {
            return device != nullptr && context != nullptr;
        }
    };

    // Returns non-owning pointers to Alpine Faction's existing D3D11 renderer.
    // OpenXR must bind to this device; the VR subsystem never creates another.
    [[nodiscard]] D3D11RendererBinding get_d3d11_renderer_binding();

    void begin_d3d11_eye(ID3D11RenderTargetView* render_target_view,
        ID3D11DepthStencilView* depth_stencil_view, int width, int height,
        float tan_left, float tan_right, float tan_down, float tan_up);
    void finish_d3d11_eye();
    void render_d3d11_world_laser_beam(const rf::Vector3& start,
        const rf::Vector3& end);
    void begin_d3d11_hud(ID3D11RenderTargetView* render_target_view,
        int width, int height);
    void finish_d3d11_hud();
    void mirror_d3d11_eye(ID3D11ShaderResourceView* source_view, int width, int height);
    void end_d3d11_vr_frame();
    [[nodiscard]] bool copy_d3d11_menu(ID3D11Texture2D* destination);
}
