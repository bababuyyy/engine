#include "assets.h"
#include <cmath>

// Abafa agressivamente os warnings de terceiros (/W4 limpo para o nosso código)
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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

    bool texture_found = false;

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        cgltf_node* node = &data->nodes[n];

        if (!node->mesh) continue;

        float transform[16];
        cgltf_node_transform_world(node, transform);

        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            cgltf_primitive* primitive = &node->mesh->primitives[p];

            if (primitive->type != cgltf_primitive_type_triangles) continue;

            cgltf_accessor* pos_accessor = nullptr;
            cgltf_accessor* uv_accessor = nullptr;
            cgltf_accessor* normal_accessor = nullptr;

            for (cgltf_size i = 0; i < primitive->attributes_count; ++i) {
                if (primitive->attributes[i].type == cgltf_attribute_type_position) {
                    pos_accessor = primitive->attributes[i].data;
                }
                else if (primitive->attributes[i].type == cgltf_attribute_type_texcoord) {
                    uv_accessor = primitive->attributes[i].data;
                }
                else if (primitive->attributes[i].type == cgltf_attribute_type_normal) {
                    normal_accessor = primitive->attributes[i].data;
                }
            }

            if (!pos_accessor) continue;

            size_t base_vertex = result.pos_rgb.size() / 6;
            size_t vert_count = pos_accessor->count;

            if (base_vertex + vert_count > 65535) {
                result.error = "Limite de 65535 vertices excedido na concatenacao (index buffer v1 limit)";
                cgltf_free(data);
                return result;
            }

            float r = 1.0f, g = 1.0f, b = 1.0f;
            if (primitive->material && primitive->material->has_pbr_metallic_roughness) {
                r = primitive->material->pbr_metallic_roughness.base_color_factor[0];
                g = primitive->material->pbr_metallic_roughness.base_color_factor[1];
                b = primitive->material->pbr_metallic_roughness.base_color_factor[2];

                // Limitação v2 mantida: 1 textura base color por .glb (Múltiplas requerem sistema de atlas/multi-material no futuro)
                if (!texture_found) {
                    cgltf_texture_view& tex_view = primitive->material->pbr_metallic_roughness.base_color_texture;
                    if (tex_view.texture && tex_view.texture->image) {
                        cgltf_image* image = tex_view.texture->image;
                        if (image->buffer_view && image->buffer_view->buffer->data) {
                            const uint8_t* img_data = static_cast<const uint8_t*>(image->buffer_view->buffer->data) + image->buffer_view->offset;
                            int comp;

                            unsigned char* pixels = stbi_load_from_memory(img_data, static_cast<int>(image->buffer_view->size), &result.tex_w, &result.tex_h, &comp, 4);
                            if (pixels) {
                                result.tex_rgba.assign(pixels, pixels + (result.tex_w * result.tex_h * 4));
                                stbi_image_free(pixels);
                                texture_found = true;
                            }
                        }
                    }
                }
            }

            // Anexa as posições transformadas no C++ para mesclar a geometria global na cena
            for (cgltf_size i = 0; i < vert_count; ++i) {
                float pos[3] = { 0.0f };
                cgltf_accessor_read_float(pos_accessor, i, pos, 3);

                // Aplica Column-Major Transform (Matrix 4x4 cgltf)
                float vx = pos[0] * transform[0] + pos[1] * transform[4] + pos[2] * transform[8] + transform[12];
                float vy = pos[0] * transform[1] + pos[1] * transform[5] + pos[2] * transform[9] + transform[13];
                float vz = pos[0] * transform[2] + pos[1] * transform[6] + pos[2] * transform[10] + transform[14];

                result.pos_rgb.push_back(vx);
                result.pos_rgb.push_back(vy);
                result.pos_rgb.push_back(vz);
                result.pos_rgb.push_back(r);
                result.pos_rgb.push_back(g);
                result.pos_rgb.push_back(b);

                float uv[2] = { 0.0f, 0.0f };
                if (uv_accessor) cgltf_accessor_read_float(uv_accessor, i, uv, 2);
                result.uv.push_back(uv[0]);
                result.uv.push_back(uv[1]);

                float norm[3] = { 0.0f, 1.0f, 0.0f };
                if (normal_accessor) cgltf_accessor_read_float(normal_accessor, i, norm, 3);

                // Aplica Matrix de Rotação (sub-bloco 3x3)
                float nx = norm[0] * transform[0] + norm[1] * transform[4] + norm[2] * transform[8];
                float ny = norm[0] * transform[1] + norm[1] * transform[5] + norm[2] * transform[9];
                float nz = norm[0] * transform[2] + norm[1] * transform[6] + norm[2] * transform[10];

                // Normaliza após transformar
                float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 0.00001f) {
                    nx /= len; ny /= len; nz /= len;
                }
                else {
                    nx = 0.0f; ny = 1.0f; nz = 0.0f;
                }

                result.normals.push_back(nx);
                result.normals.push_back(ny);
                result.normals.push_back(nz);
            }

            // Anexa e rebaixa os índices de acordo com a base dos vértices dessa primitiva em específico
            if (primitive->indices && primitive->indices->count > 0) {
                std::vector<uint16_t> temp_indices(primitive->indices->count);
                cgltf_accessor_unpack_indices(primitive->indices, temp_indices.data(), sizeof(uint16_t), primitive->indices->count);

                for (uint16_t idx : temp_indices) {
                    result.indices.push_back(static_cast<uint16_t>(idx + base_vertex));
                }
            }
            else {
                for (cgltf_size i = 0; i < vert_count; ++i) {
                    result.indices.push_back(static_cast<uint16_t>(i + base_vertex));
                }
            }
        }
    }

    if (result.pos_rgb.empty()) {
        result.error = "O arquivo GLB nao contem nenhuma malha/primitiva em seus nodes.";
        cgltf_free(data);
        return result;
    }

    result.ok = true;
    cgltf_free(data);
    return result;
}