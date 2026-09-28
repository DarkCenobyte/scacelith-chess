// Procedural model container shared by every geometry generator (scene/, character/).
#pragma once
#include "../render/materials/material_library.h"
#include "../render/mesh.h"
#include <string>
#include <vector>

struct ModelPart {
    std::string name;
    MeshData mesh;              // in the model's local space (already includes the part's placement)
    MaterialId material = MaterialId::Default;
    uint32_t flags = 1;         // render::DrawFlags (default DRAW_CAST_SHADOW)
    m::vec4 inst[4] = {};       // default per-instance params for this part
};

struct Model {
    std::vector<ModelPart> parts;
    m::AABB bounds() const {
        m::AABB b;
        for (auto& p : parts) b.add(p.mesh.bounds());
        return b;
    }
};

// GPU version: meshes uploaded once, drawn with a model matrix.
struct GpuModelPart {
    Mesh mesh;
    MaterialId material;
    uint32_t flags;
    m::vec4 inst[4];
};
struct GpuModel {
    std::vector<GpuModelPart> parts;
    void upload(const Model& m);
    void destroy();
};
