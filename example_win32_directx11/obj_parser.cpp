#define _CRT_SECURE_NO_WARNINGS

#include "obj_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4456)
#pragma warning(disable : 4701)
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#pragma warning(pop)

namespace {
    std::string lower_ext( const char* path )
    {
        std::string ext;
        if ( !path )
            return ext;

        const char* dot = std::strrchr( path, '.' );
        if ( !dot )
            return ext;

        ext = dot;
        for ( char& c : ext )
            c = (char) std::tolower( (unsigned char) c );
        return ext;
    }

    int image_index( const cgltf_data* gltf, const cgltf_texture* texture )
    {
        if ( !gltf || !texture || !texture->image )
            return -1;

        return (int) ( texture->image - gltf->images );
    }

    int diffuse_image( const cgltf_data* gltf, const cgltf_material* material )
    {
        if ( !material )
            return -1;

        if ( material->has_pbr_specular_glossiness )
            return image_index( gltf, material->pbr_specular_glossiness.diffuse_texture.texture );

        if ( material->has_pbr_metallic_roughness )
            return image_index( gltf, material->pbr_metallic_roughness.base_color_texture.texture );

        if ( material->normal_texture.texture )
            return image_index( gltf, material->normal_texture.texture );

        return -1;
    }

    bool decode_image( const cgltf_image* image, obj_image_t& out )
    {
        const unsigned char* bytes = nullptr;
        int size = 0;

        if ( image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data ) {
            bytes = (const unsigned char*) image->buffer_view->buffer->data + image->buffer_view->offset;
            size = (int) image->buffer_view->size;
        }
        else if ( image->uri ) {
            return false;
        }

        if ( !bytes || size <= 0 )
            return false;

        int w = 0, h = 0, n = 0;
        unsigned char* rgba = stbi_load_from_memory( bytes, size, &w, &h, &n, 4 );
        if ( !rgba )
            return false;

        out.width = w;
        out.height = h;
        out.rgba.assign( rgba, rgba + (size_t) w * (size_t) h * 4u );
        stbi_image_free( rgba );
        return true;
    }
}

void obj_parser_t::clear( )
{
    data.clear( );
    indices.clear( );
    images.clear( );
    submeshes.clear( );
    bounds_min = {};
    bounds_max = {};
    bounds_center = {};
    bounds_radius = 1.f;
    error.clear( );
}

void obj_parser_t::finish_bounds( )
{
    bounds_min = {1e30f, 1e30f, 1e30f};
    bounds_max = {-1e30f, -1e30f, -1e30f};

    for ( const obj_data& v : data ) {
        bounds_min.x = (std::min) ( bounds_min.x, v.pos.x );
        bounds_min.y = (std::min) ( bounds_min.y, v.pos.y );
        bounds_min.z = (std::min) ( bounds_min.z, v.pos.z );
        bounds_max.x = (std::max) ( bounds_max.x, v.pos.x );
        bounds_max.y = (std::max) ( bounds_max.y, v.pos.y );
        bounds_max.z = (std::max) ( bounds_max.z, v.pos.z );
    }

    bounds_center.x = ( bounds_min.x + bounds_max.x ) * 0.5f;
    bounds_center.y = ( bounds_min.y + bounds_max.y ) * 0.5f;
    bounds_center.z = ( bounds_min.z + bounds_max.z ) * 0.5f;

    const float dx = bounds_max.x - bounds_min.x;
    const float dy = bounds_max.y - bounds_min.y;
    const float dz = bounds_max.z - bounds_min.z;
    bounds_radius = (std::max) ( dx, (std::max) ( dy, dz ) ) * 0.5f;
    if ( bounds_radius < 0.001f )
        bounds_radius = 1.f;
}

bool obj_parser_t::load( const char* path )
{
    clear( );

    if ( !path || !path[0] ) {
        error = "empty path";
        return false;
    }

    const std::string ext = lower_ext( path );
    const bool ok = ( ext == ".glb" || ext == ".gltf" ) ? load_glb( path ) : load_obj( path );
    if ( !ok )
        return false;

    finish_bounds( );
    return true;
}

bool obj_parser_t::load_glb( const char* path )
{
    cgltf_options options = {};
    cgltf_data* gltf = nullptr;
    cgltf_result result = cgltf_parse_file( &options, path, &gltf );
    if ( result != cgltf_result_success || !gltf ) {
        error = "failed to parse glTF/GLB";
        return false;
    }

    result = cgltf_load_buffers( &options, gltf, path );
    if ( result != cgltf_result_success ) {
        error = "failed to load glTF buffers";
        cgltf_free( gltf );
        return false;
    }

    images.resize( gltf->images_count );
    for ( cgltf_size i = 0; i < gltf->images_count; ++i )
        decode_image( &gltf->images[i], images[i] );

    data.reserve( 32768 );
    indices.reserve( 131072 );

    for ( cgltf_size mesh_i = 0; mesh_i < gltf->meshes_count; ++mesh_i ) {
        const cgltf_mesh* mesh = &gltf->meshes[mesh_i];
        for ( cgltf_size prim_i = 0; prim_i < mesh->primitives_count; ++prim_i ) {
            const cgltf_primitive* prim = &mesh->primitives[prim_i];
            if ( prim->type != cgltf_primitive_type_triangles )
                continue;

            const cgltf_accessor* pos_acc = nullptr;
            const cgltf_accessor* nrm_acc = nullptr;
            const cgltf_accessor* uv_acc = nullptr;

            for ( cgltf_size a = 0; a < prim->attributes_count; ++a ) {
                const cgltf_attribute* attr = &prim->attributes[a];
                if ( attr->type == cgltf_attribute_type_position )
                    pos_acc = attr->data;
                else if ( attr->type == cgltf_attribute_type_normal )
                    nrm_acc = attr->data;
                else if ( attr->type == cgltf_attribute_type_texcoord && attr->index == 0 )
                    uv_acc = attr->data;
            }

            if ( !pos_acc )
                continue;

            const std::uint32_t base = (std::uint32_t) data.size( );
            data.resize( data.size( ) + pos_acc->count );

            for ( cgltf_size v = 0; v < pos_acc->count; ++v ) {
                obj_data& dst = data[base + v];
                float p[3] = {};
                cgltf_accessor_read_float( pos_acc, v, p, 3 );
                dst.pos = {p[0], p[1], p[2]};

                if ( nrm_acc ) {
                    float n[3] = {0.f, 1.f, 0.f};
                    cgltf_accessor_read_float( nrm_acc, v, n, 3 );
                    dst.nrm = {n[0], n[1], n[2]};
                }
                else {
                    dst.nrm = {0.f, 1.f, 0.f};
                }

                if ( uv_acc ) {
                    float uv[2] = {};
                    cgltf_accessor_read_float( uv_acc, v, uv, 2 );
                    dst.uv = {uv[0], uv[1]};
                }
            }

            obj_submesh_t sub{};
            sub.index_offset = (std::uint32_t) indices.size( );
            sub.texture = diffuse_image( gltf, prim->material );

            if ( prim->indices ) {
                for ( cgltf_size i = 0; i < prim->indices->count; ++i )
                    indices.push_back( base + (std::uint32_t) cgltf_accessor_read_index( prim->indices, i ) );
            }
            else {
                for ( cgltf_size i = 0; i < pos_acc->count; ++i )
                    indices.push_back( base + (std::uint32_t) i );
            }

            sub.index_count = (std::uint32_t) indices.size( ) - sub.index_offset;
            if ( sub.index_count > 0 )
                submeshes.push_back( sub );
        }
    }

    cgltf_free( gltf );

    if ( data.empty( ) || indices.empty( ) ) {
        error = "glTF contained no triangle mesh";
        return false;
    }

    return true;
}

bool obj_parser_t::load_obj( const char* path )
{
    std::ifstream file( path );
    if ( !file ) {
        error = "failed to open OBJ";
        return false;
    }

    std::vector<vec3> positions;
    std::vector<vec3> normals;
    std::vector<vec2> uvs;
    std::unordered_map<std::string, std::uint32_t> unique;

    auto parse_index = [] ( const std::string& token, int count ) -> int
    {
        if ( token.empty( ) )
            return -1;
        const int value = std::atoi( token.c_str( ) );
        if ( value > 0 )
            return value - 1;
        if ( value < 0 )
            return count + value;
        return -1;
    };

    std::string line;
    while ( std::getline( file, line ) ) {
        if ( line.empty( ) || line[0] == '#' )
            continue;

        std::istringstream ss( line );
        std::string tag;
        ss >> tag;

        if ( tag == "v" ) {
            vec3 v{};
            ss >> v.x >> v.y >> v.z;
            positions.push_back( v );
        }
        else if ( tag == "vn" ) {
            vec3 n{};
            ss >> n.x >> n.y >> n.z;
            normals.push_back( n );
        }
        else if ( tag == "vt" ) {
            vec2 t{};
            ss >> t.x >> t.y;
            uvs.push_back( t );
        }
        else if ( tag == "f" ) {
            std::vector<std::uint32_t> face;
            std::string token;
            while ( ss >> token ) {
                const auto slash1 = token.find( '/' );
                const auto slash2 = token.find( '/', slash1 == std::string::npos ? 0 : slash1 + 1 );

                std::string i_v = token;
                std::string i_t;
                std::string i_n;
                if ( slash1 != std::string::npos ) {
                    i_v = token.substr( 0, slash1 );
                    if ( slash2 != std::string::npos ) {
                        i_t = token.substr( slash1 + 1, slash2 - slash1 - 1 );
                        i_n = token.substr( slash2 + 1 );
                    }
                    else {
                        i_t = token.substr( slash1 + 1 );
                    }
                }

                const int vi = parse_index( i_v, (int) positions.size( ) );
                const int ti = parse_index( i_t, (int) uvs.size( ) );
                const int ni = parse_index( i_n, (int) normals.size( ) );
                if ( vi < 0 || vi >= (int) positions.size( ) )
                    continue;

                const std::string key = std::to_string( vi ) + "/" + std::to_string( ti ) + "/" + std::to_string( ni );
                auto found = unique.find( key );
                if ( found == unique.end( ) ) {
                    obj_data vtx{};
                    vtx.pos = positions[vi];
                    if ( ni >= 0 && ni < (int) normals.size( ) )
                        vtx.nrm = normals[ni];
                    else
                        vtx.nrm = {0.f, 1.f, 0.f};
                    if ( ti >= 0 && ti < (int) uvs.size( ) )
                        vtx.uv = {uvs[ti].x, 1.f - uvs[ti].y};
                    found = unique.emplace( key, (std::uint32_t) data.size( ) ).first;
                    data.push_back( vtx );
                }
                face.push_back( found->second );
            }

            for ( size_t i = 1; i + 1 < face.size( ); ++i ) {
                indices.push_back( face[0] );
                indices.push_back( face[i] );
                indices.push_back( face[i + 1] );
            }
        }
    }

    if ( data.empty( ) || indices.empty( ) ) {
        error = "obj contained no faces";
        return false;
    }

    obj_submesh_t sub{};
    sub.index_offset = 0;
    sub.index_count = (std::uint32_t) indices.size( );
    sub.texture = -1;
    submeshes.push_back( sub );
    return true;
}
