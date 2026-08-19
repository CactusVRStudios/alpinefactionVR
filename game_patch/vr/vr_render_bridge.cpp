// SPDX-License-Identifier: MPL-2.0
#include "vr_render_bridge.h"

#include "../graphics/d3d11/gr_d3d11.h"

namespace afvr
{
    D3D11RendererBinding get_d3d11_renderer_binding()
    {
        return {
            gr::d3d11::get_renderer_device(),
            gr::d3d11::get_renderer_device_context(),
        };
    }

    void begin_d3d11_eye(ID3D11RenderTargetView* render_target_view,
        ID3D11DepthStencilView* depth_stencil_view, int width, int height,
        float tan_left, float tan_right, float tan_down, float tan_up)
    {
        constexpr float near_plane = 0.1f;
        auto* device = gr::d3d11::get_renderer_device();
        if (!device) {
            return;
        }

        // AFVR TODO: Plumb RF's current fog-selected far plane through this
        // bridge. The ordinary campaign fallback is 1700 m.
        gr::d3d11::Projection projection{
            tan_left, tan_right, tan_down, tan_up,
            near_plane, 1700.0f,
        };
        gr::d3d11::begin_renderer_vr_eye(
            render_target_view, depth_stencil_view, width, height, projection);
    }

    void end_d3d11_vr_frame()
    {
        gr::d3d11::end_renderer_vr_frame();
    }

    void finish_d3d11_eye()
    {
        gr::d3d11::finish_renderer_vr_eye();
    }

    void render_d3d11_world_laser_beam(const rf::Vector3& start,
        const rf::Vector3& end)
    {
        gr::d3d11::render_renderer_vr_world_laser_beam(start, end);
    }

    void mirror_d3d11_eye(ID3D11ShaderResourceView* source_view, int width, int height)
    {
        gr::d3d11::mirror_renderer_vr_eye(source_view, width, height);
    }

    void begin_d3d11_hud(ID3D11RenderTargetView* render_target_view,
        int width, int height)
    {
        gr::d3d11::begin_renderer_vr_hud(render_target_view, width, height);
    }

    void finish_d3d11_hud()
    {
        gr::d3d11::finish_renderer_vr_hud();
    }

    bool copy_d3d11_menu(ID3D11Texture2D* destination)
    {
        return gr::d3d11::copy_renderer_presented_target(destination);
    }
}
