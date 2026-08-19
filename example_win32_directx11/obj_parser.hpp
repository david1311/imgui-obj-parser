#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct vec2 {
    float x = 0.f;
    float y = 0.f;
};

struct obj_data {
    vec3 pos;
    vec3 nrm;
    vec2 uv;
};

struct obj_image_t {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

struct obj_submesh_t
{
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    int texture = -1;
};

struct obj_parser_t {
    std::vector<obj_data> data;
    std::vector<std::uint32_t> indices;
    std::vector<obj_image_t> images;
    std::vector<obj_submesh_t> submeshes;
    vec3 bounds_min{};
    vec3 bounds_max{};
    vec3 bounds_center{};
    float bounds_radius = 1.f;
    std::string error;

    bool load( const char* path );
    void clear( );

    private:
    bool load_glb( const char* path );
    bool load_obj( const char* path );
    void finish_bounds( );
};
