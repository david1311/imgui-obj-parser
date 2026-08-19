#pragma once

#include "obj_parser.hpp"
#include "imgui.h"

#include <d3d11.h>

struct preview_t {
    bool auto_rotate = true;
    float rotate_speed = 0.55f;
    float yaw = 0.55f;
    float pitch = 0.12f;
    float zoom = 1.f;

    bool init( ID3D11Device* device, ID3D11DeviceContext* context, obj_parser_t& parser );
    void set_device( ID3D11Device* device, ID3D11DeviceContext* context );
    bool load_model( const char* path );
    void shutdown( );
    void draw_window( const char* title );

    std::vector< std::string > model_paths;
    int current_model = 0;

    private:
    struct constants_t {
        float wvp[16];
        float world[16];
        float light_dir[4];
        float cam_pos[4];
    };

    bool create_pipeline( );
    bool create_mesh( obj_parser_t& parser );
    bool create_textures( obj_parser_t& parser );
    bool resize_target( int width, int height );
    void handle_input( const ImVec2& area_min, const ImVec2& area_max );
    void render( int width, int height );

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11SamplerState* sampler = nullptr;

    ID3D11Texture2D* color_tex = nullptr;
    ID3D11RenderTargetView* color_rtv = nullptr;
    ID3D11ShaderResourceView* color_srv = nullptr;
    ID3D11Texture2D* depth_tex = nullptr;
    ID3D11DepthStencilView* depth_dsv = nullptr;

    std::vector< ID3D11ShaderResourceView* > textures;
    std::vector< obj_submesh_t > submeshes;

    UINT index_count = 0;
    int target_w = 0;
    int target_h = 0;
    vec3 center{};
    float radius = 1.f;
    bool orbiting = false;
    bool ready = false;
};
