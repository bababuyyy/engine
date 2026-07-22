#include "assets.h"

// Abafa agressivamente os warnings de terceiros (/W4 limpo para o nosso código)
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

MeshData assets_load_glb(const char* path) {
    MeshData result;
    result.ok = false;

    cgltf_options options = {};
    cgltf_data* data = nullptr;

    cgltf_result parse_res = cgltf_parse_file(&options, path, &data);
    if (parse_res != cgltf_result_success) {
        result.error = "Falha no cgltf_parse_file";
        return result;
    }

    cgltf_result load_res = cgltf_load_buffers(&options, data, path);
    if (load_res != cgltf_result_success) {
        result.error = "Falha no cgltf_load_buffers";
        cgltf_free(data);
        return result;
    }

    if (data->meshes_count == 0 || data->meshes[0].primitives_count == 0) {
        result.error = "Sem malhas ou primitivas no arquivo";
        cgltf_free(data);
        return result;
    }

    cgltf_primitive* primitive = &data->meshes[0].primitives[0];

    if (primitive->type != cgltf_primitive_type_triangles) {
        result.error = "A primitiva nao e composta de triangulos";
        cgltf_free(data);
        return result;
    }

    cgltf_accessor* pos_accessor = nullptr;
    for (cgltf_size i = 0; i < primitive->attributes_count; ++i) {
        if (primitive->attributes[i].type == cgltf_attribute_type_position) {
            pos_accessor = primitive->attributes[i].data;
            break;
        }
    }

    if (!pos_accessor) {
        result.error = "Atributo POSITION ausente";
        cgltf_free(data);
        return result;
    }

    if (pos_accessor->count > 65535) {
        result.ok = false; "Limite de 65535 vertices excedido (index buffer v1 limit)";
        cgltf_free(data);
        return result;
    }

    float r = 1.0f, g = 1.0f, b = 1.0f;
    if (primitive->material && primitive->material->has_pbr_metallic_roughness) {
        r = primitive->material->pbr_metallic_roughness.base_color_factor[0];
        g = primitive->material->pbr_metallic_roughness.base_color_factor[1];
        b = primitive->material->pbr_metallic_roughness.base_color_factor[2];
    }

    result.pos_rgb.resize(pos_accessor->count * 6);
    for (cgltf_size i = 0; i < pos_accessor->count; ++i) {
        cgltf_accessor_read_float(pos_accessor, i, &result.pos_rgb[i * 6], 3);
        result.pos_rgb[i * 6 + 3] = r;
        result.pos_rgb[i * 6 + 4] = g;
        result.pos_rgb[i * 6 + 5] = b;
    }

    if (primitive->indices && primitive->indices->count > 0) {
        result.indices.resize(primitive->indices->count);
        cgltf_accessor_unpack_indices(primitive->indices, result.indices.data(), sizeof(uint16_t), primitive->indices->count);
    }
    else {
        result.indices.resize(pos_accessor->count);
        for (cgltf_size i = 0; i < pos_accessor->count; ++i) {
            result.indices[i] = static_cast<uint16_t>(i);
        }
    }

    result.ok = true;
    cgltf_free(data);
    return result;
}