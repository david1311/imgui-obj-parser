#define IMGUI_DEFINE_MATH_OPERATORS
#include "preview.hpp"

#include "imgui_internal.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace
{
    void release(IUnknown*& object)
    {
        if (object)
        {
            object->Release();
            object = nullptr;
        }
    }

    template<typename T>
    void release_t(T*& object)
    {
        if (object)
        {
            object->Release();
            object = nullptr;
        }
    }

    void store_matrix(float out[16], FXMMATRIX m)
    {
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(out), XMMatrixTranspose(m));
    }

    const char* k_shader = R"(
cbuffer Constants : register(b0)
{
    float4x4 wvp;
    float4x4 world;
    float4 light_dir;
    float4 cam_pos;
};

Texture2D diffuse_tex : register(t0);
SamplerState samp : register(s0);

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD;
    float3 wpos : TEXCOORD1;
};

VSOut vs_main(VSIn input)
{
    VSOut output;
    float4 p = float4(input.pos, 1.0);
    output.pos = mul(p, wvp);
    output.wpos = mul(p, world).xyz;
    output.nrm = normalize(mul(input.nrm, (float3x3)world));
    output.uv = input.uv;
    return output;
}

float4 ps_main(VSOut input) : SV_Target
{
    float4 albedo = diffuse_tex.Sample(samp, input.uv);
    clip(albedo.a - 0.08);
    float3 n = normalize(input.nrm);
    float3 l = normalize(-light_dir.xyz);
    float wrap = saturate(dot(n, l) * 0.5 + 0.5);
    float3 col = albedo.rgb * (0.22 + 0.78 * wrap);
    float3 v = normalize(cam_pos.xyz - input.wpos);
    float3 h = normalize(l + v);
    float spec = pow(saturate(dot(n, h)), 32.0) * 0.08;
    col += spec;
    return float4(col, albedo.a);
}
)";
}

bool preview_t::init( ID3D11Device* in_device, ID3D11DeviceContext* in_context, obj_parser_t& parser )
{
    shutdown( );

    device = in_device;
    context = in_context;
    if ( !device || !context )
        return false;

    center = parser.bounds_center;
    radius = parser.bounds_radius;
    submeshes = parser.submeshes;

    if ( !create_pipeline( ) )
        return false;
    if ( !create_mesh( parser ) )
        return false;
    if ( !create_textures( parser ) )
        return false;

    ready = true;
    return true;
}

void preview_t::set_device( ID3D11Device* in_device, ID3D11DeviceContext* in_context )
{
    device = in_device;
    context = in_context;
}

bool preview_t::load_model( const char* path )
{
    if ( !device || !context )
        return false;

    obj_parser_t parser;
    if ( !parser.load( path ) )
        return false;

    return init( device, context, parser );
}

void preview_t::shutdown( )
{
    ready = false;
    for ( ID3D11ShaderResourceView* srv : textures ) {
        if ( srv )
            srv->Release( );
    }
    textures.clear( );
    submeshes.clear( );

    release_t( vs );
    release_t( ps );
    release_t( layout );
    release_t( vb );
    release_t( ib );
    release_t( cb );
    release_t( raster );
    release_t( depth );
    release_t( blend );
    release_t( sampler );
    release_t( color_rtv );
    release_t( color_srv );
    release_t( color_tex );
    release_t( depth_dsv );
    release_t( depth_tex );

    device = nullptr;
    context = nullptr;
    target_w = target_h = 0;
    index_count = 0;
}

bool preview_t::create_pipeline( )
{
    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    HRESULT hr = D3DCompile( k_shader, std::strlen( k_shader ), nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs_blob, &error_blob );
    if ( FAILED( hr ) ) {
        if ( error_blob )
            error_blob->Release( );
        return false;
    }
    if ( error_blob )
        error_blob->Release( );

    hr = D3DCompile( k_shader, std::strlen( k_shader ), nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps_blob, &error_blob );
    if ( FAILED( hr ) ) {
        vs_blob->Release( );
        if ( error_blob )
            error_blob->Release( );
        return false;
    }
    if ( error_blob )
        error_blob->Release( );

    device->CreateVertexShader( vs_blob->GetBufferPointer( ), vs_blob->GetBufferSize( ), nullptr, &vs );
    device->CreatePixelShader( ps_blob->GetBufferPointer( ), ps_blob->GetBufferSize( ), nullptr, &ps );

    D3D11_INPUT_ELEMENT_DESC elems[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout( elems, 3, vs_blob->GetBufferPointer( ), vs_blob->GetBufferSize( ), &layout );
    vs_blob->Release( );
    ps_blob->Release( );

    D3D11_BUFFER_DESC cbd{};
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.ByteWidth = sizeof( constants_t );
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer( &cbd, nullptr, &cb );

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState( &rd, &raster );

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState( &dd, &depth );

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState( &bd, &blend );

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState( &sd, &sampler );

    return vs && ps && layout && cb && raster && depth && blend && sampler;
}

bool preview_t::create_mesh( obj_parser_t& parser )
{
    if ( parser.data.empty( ) || parser.indices.empty( ) )
        return false;

    D3D11_BUFFER_DESC vbd{};
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.ByteWidth = (UINT) ( parser.data.size( ) * sizeof( obj_data ) );
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA vinit{};
    vinit.pSysMem = parser.data.data( );
    if ( FAILED( device->CreateBuffer( &vbd, &vinit, &vb ) ) )
        return false;

    D3D11_BUFFER_DESC ibd{};
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ibd.ByteWidth = (UINT) ( parser.indices.size( ) * sizeof( std::uint32_t ) );
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA iinit{};
    iinit.pSysMem = parser.indices.data( );
    if ( FAILED( device->CreateBuffer( &ibd, &iinit, &ib ) ) )
        return false;

    index_count = (UINT) parser.indices.size( );
    return true;
}

bool preview_t::create_textures( obj_parser_t& parser )
{
    textures.resize( parser.images.size( ), nullptr );

    for ( size_t i = 0; i < parser.images.size( ); ++i ) {
        const obj_image_t& image = parser.images[i];
        if ( image.rgba.empty( ) || image.width <= 0 || image.height <= 0 )
            continue;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = (UINT) image.width;
        desc.Height = (UINT) image.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sub{};
        sub.pSysMem = image.rgba.data( );
        sub.SysMemPitch = (UINT) image.width * 4u;

        ID3D11Texture2D* tex = nullptr;
        if ( FAILED( device->CreateTexture2D( &desc, &sub, &tex ) ) || !tex )
            continue;

        device->CreateShaderResourceView( tex, nullptr, &textures[i] );
        tex->Release( );
    }

    unsigned char white[4] = {220, 220, 230, 255};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = white;
    sub.SysMemPitch = 4;
    ID3D11Texture2D* tex = nullptr;
    if ( SUCCEEDED( device->CreateTexture2D( &desc, &sub, &tex ) ) && tex ) {
        ID3D11ShaderResourceView* fallback = nullptr;
        device->CreateShaderResourceView( tex, nullptr, &fallback );
        tex->Release( );
        textures.push_back( fallback );
    }

    return true;
}

bool preview_t::resize_target( int width, int height )
{
    width = (std::max) ( width, 8 );
    height = (std::max) ( height, 8 );
    if ( width == target_w && height == target_h && color_rtv && depth_dsv )
        return true;

    release_t( color_rtv );
    release_t( color_srv );
    release_t( color_tex );
    release_t( depth_dsv );
    release_t( depth_tex );

    target_w = width;
    target_h = height;

    D3D11_TEXTURE2D_DESC cd{};
    cd.Width = (UINT) width;
    cd.Height = (UINT) height;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if ( FAILED( device->CreateTexture2D( &cd, nullptr, &color_tex ) ) )
        return false;
    device->CreateRenderTargetView( color_tex, nullptr, &color_rtv );
    device->CreateShaderResourceView( color_tex, nullptr, &color_srv );

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = (UINT) width;
    dd.Height = (UINT) height;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if ( FAILED( device->CreateTexture2D( &dd, nullptr, &depth_tex ) ) )
        return false;
    device->CreateDepthStencilView( depth_tex, nullptr, &depth_dsv );

    return color_rtv && color_srv && depth_dsv;
}

void preview_t::handle_input( const ImVec2& area_min, const ImVec2& area_max )
{
    const bool hovered = ImGui::IsWindowHovered( ) && ImGui::IsMouseHoveringRect( area_min, area_max, false );
    if ( hovered && ImGui::IsMouseDragging( ImGuiMouseButton_Left ) ) {
        const ImVec2 delta = ImGui::GetIO( ).MouseDelta;
        yaw += delta.x * 0.01f;
        pitch = ImClamp( pitch + delta.y * 0.01f, -1.2f, 1.2f );
        orbiting = true;
        auto_rotate = false;
    }
    else if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) ) {
        orbiting = false;
    }

    if ( hovered && ImGui::GetIO( ).MouseWheel != 0.f ) {
        const float wheel = ImGui::GetIO( ).MouseWheel;
        zoom = ImClamp( zoom * ( wheel > 0.f ? 1.12f : 1.f / 1.12f ), 0.45f, 3.5f );
    }
}

void preview_t::render( int width, int height )
{
    if ( !ready || !resize_target( width, height ) )
        return;

    if ( auto_rotate && !orbiting )
        yaw += ImGui::GetIO( ).DeltaTime * rotate_speed;

    const float dist = (std::max) ( radius * 2.6f / zoom, 0.2f );
    const XMVECTOR target = XMVectorSet( center.x, center.y, center.z, 0.f );
    const XMMATRIX rot = XMMatrixRotationX( pitch ) * XMMatrixRotationY( yaw );
    const XMVECTOR offset = XMVector3Transform( XMVectorSet( 0.f, 0.f, -dist, 0.f ), rot );
    const XMVECTOR eye = XMVectorAdd( target, offset );
    const XMVECTOR up = XMVectorSet( 0.f, 1.f, 0.f, 0.f );

    const float aspect = (float) width / (float) height;
    const XMMATRIX view = XMMatrixLookAtLH( eye, target, up );
    const XMMATRIX proj = XMMatrixPerspectiveFovLH( XMConvertToRadians( 32.f ), aspect, dist * 0.02f, dist * 8.f );
    const XMMATRIX world = XMMatrixIdentity( );
    const XMMATRIX wvp = world * view * proj;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if ( SUCCEEDED( context->Map( cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped ) ) ) {
        constants_t constants{};
        store_matrix( constants.wvp, wvp );
        store_matrix( constants.world, world );
        constants.light_dir[0] = 0.35f;
        constants.light_dir[1] = -0.85f;
        constants.light_dir[2] = 0.35f;
        constants.light_dir[3] = 0.f;
        XMStoreFloat4( reinterpret_cast<XMFLOAT4*>( constants.cam_pos ), eye );
        std::memcpy( mapped.pData, &constants, sizeof( constants ) );
        context->Unmap( cb, 0 );
    }

    const float clear[4] = {0.08f, 0.08f, 0.10f, 1.f};
    context->OMSetRenderTargets( 1, &color_rtv, depth_dsv );
    context->ClearRenderTargetView( color_rtv, clear );
    context->ClearDepthStencilView( depth_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f, 0 );

    D3D11_VIEWPORT vp{};
    vp.Width = (float) width;
    vp.Height = (float) height;
    vp.MaxDepth = 1.f;
    context->RSSetViewports( 1, &vp );

    const UINT stride = sizeof( obj_data );
    const UINT vb_offset = 0;
    context->IASetInputLayout( layout );
    context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    context->IASetVertexBuffers( 0, 1, &vb, &stride, &vb_offset );
    context->IASetIndexBuffer( ib, DXGI_FORMAT_R32_UINT, 0 );
    context->VSSetShader( vs, nullptr, 0 );
    context->PSSetShader( ps, nullptr, 0 );
    context->VSSetConstantBuffers( 0, 1, &cb );
    context->PSSetConstantBuffers( 0, 1, &cb );
    context->PSSetSamplers( 0, 1, &sampler );
    context->RSSetState( raster );
    context->OMSetDepthStencilState( depth, 0 );
    const float blend_factor[4] = {};
    context->OMSetBlendState( blend, blend_factor, 0xffffffff );

    ID3D11ShaderResourceView* fallback = textures.empty( ) ? nullptr : textures.back( );
    if ( submeshes.empty( ) ) {
        context->PSSetShaderResources( 0, 1, &fallback );
        context->DrawIndexed( index_count, 0, 0 );
    }
    else {
        for ( const obj_submesh_t& sub : submeshes ) {
            ID3D11ShaderResourceView* srv = fallback;
            if ( sub.texture >= 0 && sub.texture < (int) textures.size( ) - 1 && textures[sub.texture] )
                srv = textures[sub.texture];
            context->PSSetShaderResources( 0, 1, &srv );
            context->DrawIndexed( sub.index_count, sub.index_offset, 0 );
        }
    }

    ID3D11ShaderResourceView* null_srv = nullptr;
    context->PSSetShaderResources( 0, 1, &null_srv );
    context->OMSetRenderTargets( 0, nullptr, nullptr );
}

void preview_t::draw_window(const char* title)
{
    ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoTitleBar);

    ImGui::Checkbox("auto rotate", &auto_rotate);
    ImGui::SameLine();
    ImGui::SliderFloat("speed", &rotate_speed, 0.f, 2.f, "%.2f");

    if ( !model_paths.empty( ) ) {
        auto get_filename = []( const std::string& path ) -> std::string {
            size_t sep = path.find_last_of( "/\\" );
            return sep != std::string::npos ? path.substr( sep + 1 ) : path;
        };

        if ( current_model < 0 || current_model >= (int)model_paths.size( ) )
            current_model = 0;

        std::string preview_label = get_filename( model_paths[current_model] );
        if ( ImGui::BeginCombo( "model", preview_label.c_str( ) ) ) {
            for ( int i = 0; i < (int)model_paths.size( ); ++i ) {
                const bool selected = ( i == current_model );
                std::string label = get_filename( model_paths[i] );
                if ( ImGui::Selectable( ( label + "##" + std::to_string( i ) ).c_str( ), selected ) ) {
                    if ( current_model != i ) {
                        current_model = i;
                        load_model( model_paths[current_model].c_str( ) );
                    }
                }
                if ( selected )
                    ImGui::SetItemDefaultFocus( );
            }
            ImGui::EndCombo( );
        }
    }

    ImGui::TextUnformatted("drag to orbit, wheel to zoom");

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int w = (int)avail.x;
    const int h = (int)(std::max)(avail.y, 16.f);
    const ImVec2 image_min = ImGui::GetCursorScreenPos();
    const ImVec2 image_max = ImVec2(image_min.x + avail.x, image_min.y + (float)h);

    handle_input(image_min, image_max);
    render(w, h);

    if ( color_srv ) {
        ImGui::Image( ImTextureRef( (ImTextureID) (std::uintptr_t) color_srv ), ImVec2( (float) w, (float) h ) );
    }

    ImGui::End();
}
