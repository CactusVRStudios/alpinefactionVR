#pragma once

#include <source_location>
#include <concepts>
#include <optional>
#include <d3d11.h>
#include <common/ComPtr.h>
#include <common/DynamicLinkLibrary.h>
#include <xlog/xlog.h>
#include "../../rf/gr/gr.h"
#include "gr_d3d11_transform.h"

namespace rf
{
    struct GSolid;
    struct GRoom;
    struct VifMesh;
    struct VifLodMesh;
    struct MeshMaterial;
    struct MeshRenderParams;
    struct CharacterInstance;
}

namespace gr::d3d11
{
    class StateManager;
    class ShaderManager;
    class TextureManager;
    class DynamicGeometryRenderer;
    class RenderContext;
    class SolidRenderer;
    class MeshRenderer;
    class EntityShadowRenderer;
    class OutlineRenderer;
    class GammaPass;

    class Renderer
    {
    public:
        Renderer(HWND hwnd);
        ~Renderer();
        void window_active();
        void window_inactive();
        void set_fullscreen_state(bool fullscreen);
        void bitmap(int bm_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y, rf::gr::Mode mode);
        void bitmap(int bm_handle, float x, float y, float w, float h, float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::gr::Mode mode);
        void page_in(int bm_handle);
        void clear();
        void zbuffer_clear();
        void set_clip();
        void set_far_clip(bool enabled);
        void flip();
        void flush_outlines_before_fpgun();
        void texture_save_cache();
        void texture_flush_cache(bool force);
        void texture_flush_non_user_cache();
        void texture_mark_dirty(int bm_handle);
        void texture_add_ref(int bm_handle);
        void texture_remove_ref(int bm_handle);
        void reset_sampler_states();
        bool lock(int bm_handle, int section, rf::gr::LockInfo *lock);
        void unlock(rf::gr::LockInfo *lock);
        void get_texel(int bm_handle, float u, float v, rf::gr::Color *clr);
        bool set_render_target(int bm_handle);
        rf::bm::Format read_back_buffer(int x, int y, int w, int h, rf::ubyte *data);
        void tmapper(int nv, const rf::gr::Vertex **vertices, int vertex_attributes, rf::gr::Mode mode);
        void line_3d(const rf::gr::Vertex& v0, const rf::gr::Vertex& v1, rf::gr::Mode mode);
        void line_2d(float x1, float y1, float x2, float y2, rf::gr::Mode mode);
        bool poly(int nv, rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode, bool constant_sw, float sw);
        void project_vertex(rf::gr::Vertex* v);
        void setup_3d(Projection proj);
        void render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms);
        void render_movable_solid(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient);
        void render_alpha_detail_room(rf::GRoom *room, rf::GSolid *solid);
        void render_sky_room(rf::GRoom *room, rf::Vector3& out_sky_transform_pos, rf::Matrix3& out_sky_transform_orient);
        void render_room_liquid_surface(rf::GSolid* solid, rf::GRoom* room);
        void clear_solid_cache();
        void reset_solid_cache_after_boolean();
        void render_v3d_vif(rf::VifLodMesh *lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::MeshRenderParams& params, bool skip_ambient_cache = false);
        void render_character_vif(rf::VifLodMesh *lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::CharacterInstance *ci, const rf::MeshRenderParams& params, bool skip_ambient_cache = false);
        void clear_vif_cache(rf::VifLodMesh *lod_mesh);
        void fog_set();
        void page_in_v3d_mesh(rf::VifLodMesh* lod_mesh, rf::MeshMaterial* materials = nullptr, int num_materials = 0);
        void page_in_character_mesh(rf::VifLodMesh* lod_mesh);
        void page_in_solid(rf::GSolid* solid);
        void page_in_movable_solid(rf::GSolid* solid);
        void flush_caches();
        void reset_static_vertex_color_tracking();
        void clear_mesh_lights();
        void set_pow2_tex_active(bool active);
        float z_far() const;
        bool supports_sample_count(uint32_t sample_count);
        uint32_t get_sample_count() const;
        void flush_frame_buffers();
        bool supports_exclusive_fullscreen() const;
        [[nodiscard]] ID3D11Device* device() const { return device_; }
        [[nodiscard]] ID3D11DeviceContext* device_context() const { return context_; }
        void begin_vr_eye(ID3D11RenderTargetView* render_target_view,
            ID3D11DepthStencilView* depth_stencil_view, int width, int height,
            const Projection& projection);
        void finish_vr_eye();
        void render_vr_world_laser_beam(const rf::Vector3& start,
            const rf::Vector3& end);
        void begin_vr_hud(ID3D11RenderTargetView* render_target_view,
            int width, int height);
        void finish_vr_hud();
        void mirror_vr_eye(ID3D11ShaderResourceView* source_view,
            int source_width, int source_height);
        void end_vr_frame();
        [[nodiscard]] bool copy_presented_target(ID3D11Texture2D* destination);

    private:
        void init_device();
        void init_swap_chain(HWND hwnd);
        void init_back_buffer(const uint32_t sample_count);
        void init_scene_texture();
        void init_depth_stencil_buffer(const uint32_t sample_count);
        void flush_outlines_before_2d();

        HWND hwnd_;
        DynamicLinkLibrary d3d11_lib_;
        ComPtr<ID3D11Device> device_;
        ComPtr<IDXGISwapChain> swap_chain_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<ID3D11Texture2D> back_buffer_;
        ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;
        ComPtr<ID3D11Texture2D> scene_texture_;
        ComPtr<ID3D11ShaderResourceView> scene_texture_srv_;
        ComPtr<ID3D11Texture2D> msaa_render_target_;
        ComPtr<ID3D11Texture2D> default_render_target_;
        ComPtr<ID3D11RenderTargetView> default_render_target_view_;
        ComPtr<ID3D11DepthStencilView> depth_stencil_view_;
        std::unique_ptr<StateManager> state_manager_;
        std::unique_ptr<ShaderManager> shader_manager_;
        std::unique_ptr<TextureManager> texture_manager_;
        std::unique_ptr<DynamicGeometryRenderer> dyn_geo_renderer_;
        std::unique_ptr<RenderContext> render_context_;
        std::unique_ptr<SolidRenderer> solid_renderer_;
        std::unique_ptr<MeshRenderer> mesh_renderer_;
        std::unique_ptr<EntityShadowRenderer> entity_shadow_renderer_;
        std::unique_ptr<OutlineRenderer> outline_renderer_;
        std::unique_ptr<GammaPass> gamma_pass_;
        int render_target_bm_handle_ = -1;
        bool skip_gamma_pass_ = false;
        bool low_frame_latency_ = false;
        bool allow_tearing_ = false;
        bool frame_latency_stall_logged_ = false;
        HANDLE frame_latency_wait_handle_ = nullptr;
        UINT swap_chain_flags_ = 0;
        std::optional<Projection> vr_saved_projection_;
        ID3D11RenderTargetView* vr_eye_render_target_view_ = nullptr;
        ID3D11DepthStencilView* vr_eye_depth_stencil_view_ = nullptr;
        int vr_eye_width_ = 0;
        int vr_eye_height_ = 0;
        std::optional<Projection> vr_active_eye_projection_;
        rf::Vector3 vr_active_eye_position_{};
        rf::Matrix3 vr_active_eye_orientation_{};
        ComPtr<ID3D11Buffer> vr_mirror_vertex_buffer_;
        ComPtr<ID3D11Buffer> vr_world_debug_vertex_buffer_;
        bool vr_world_debug_renderer_logged_ = false;
        bool vr_mirror_logged_ = false;
        bool vr_present_queue_busy_logged_ = false;
    };

    // Non-owning access for OpenXR. The VR path always uses Alpine's existing
    // D3D11 device and never creates a second graphics device.
    [[nodiscard]] ID3D11Device* get_renderer_device();
    [[nodiscard]] ID3D11DeviceContext* get_renderer_device_context();
    void begin_renderer_vr_eye(ID3D11RenderTargetView* render_target_view,
        ID3D11DepthStencilView* depth_stencil_view, int width, int height,
        const Projection& projection);
    void finish_renderer_vr_eye();
    void render_renderer_vr_world_laser_beam(const rf::Vector3& start,
        const rf::Vector3& end);
    void begin_renderer_vr_hud(ID3D11RenderTargetView* render_target_view,
        int width, int height);
    void finish_renderer_vr_hud();
    void mirror_renderer_vr_eye(ID3D11ShaderResourceView* source_view,
        int source_width, int source_height);
    void end_renderer_vr_frame();
    [[nodiscard]] bool copy_renderer_presented_target(ID3D11Texture2D* destination);

    void init_error(ID3D11Device* device);
    void fatal_gr_error(
        HRESULT hr,
        const std::source_location& loc = std::source_location::current()
    );

    template <std::invocable F>
    inline void check_hr(
        const HRESULT hr,
        F&& what,
        const std::source_location& loc = std::source_location::current()
    ) {
        if (FAILED(hr)) {
            what();
            fatal_gr_error(hr, loc);
        }
    }

    #define DF_GR_D3D11_CHECK_HR(code) { \
        const char* const func_name = __func__; \
        ::gr::d3d11::check_hr( \
            code, \
            [=] { \
                ::xlog::error( \
                    "D3D11 call failed: {} (function {} in line {})", \
                    #code, \
                    func_name, \
                    __LINE__ \
                ); \
            } \
        ); \
    }

    static inline int pack_color(const rf::Color& color)
    {
        // assume little endian
        return (color.alpha << 24) | (color.blue << 16) | (color.green << 8) | color.red;
    }

    static inline std::array<int, 2> normalize_texture_handles_for_mode(rf::gr::Mode mode, const std::array<int, 2>& textures)
    {
        switch (mode.get_texture_source()) {
            case rf::gr::TEXTURE_SOURCE_NONE:
                return {-1, -1};
            case rf::gr::TEXTURE_SOURCE_WRAP:
            case rf::gr::TEXTURE_SOURCE_CLAMP:
            case rf::gr::TEXTURE_SOURCE_CLAMP_NO_FILTERING:
                return {textures[0], -1};
            default:
                return textures;
        }
    }
}
