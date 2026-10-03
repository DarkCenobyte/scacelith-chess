// Mesh data (CPU) and GPU meshes. Every procedural generator in scene/ and character/ outputs a
// MeshData; the renderer uploads it with Mesh::upload.
#pragma once
#include "../math/math.h"
#include "gpu.h"
#include <cstdint>
#include <string>
#include <vector>

// Single vertex format for the whole engine (48 bytes).
// Attribute locations: 0 = position, 1 = normal, 2 = tangent (xyz + handedness w), 3 = uv.
struct Vertex {
    m::vec3 pos;
    m::vec3 normal;
    m::vec4 tangent{1, 0, 0, 1};
    m::vec2 uv;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // triangle list, counter-clockwise front faces

    m::AABB bounds() const;
    void append(const MeshData& other, const m::mat4& transform = m::mat4());
    void transform(const m::mat4& m);
    void computeNormals(bool smooth = true);  // area-weighted
    void computeTangents();                   // from uvs; needs normals
};

struct Mesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    uint32_t indexCount = 0, vertexCount = 0;
    m::AABB bounds;
    std::string name;
    void upload(const MeshData& d, const char* debugName = nullptr);
    void destroy();
    // Binds the VAO (callers draw with glDrawElements(GL_TRIANGLES or GL_PATCHES, indexCount, ...)).
    void bind() const { glBindVertexArray(vao); }
};

// Basic primitives (centred at origin, Y up), mostly for tests and simple props.
namespace prim {
MeshData box(m::vec3 halfExtent);
MeshData roundedBox(m::vec3 halfExtent, float radius, int segments);
MeshData sphere(float radius, int segU = 48, int segV = 24);
MeshData plane(float sizeX, float sizeZ, int subdivX = 1, int subdivZ = 1, float uvScale = 1.0f);  // +Y facing
MeshData cylinder(float radius, float height, int segments = 48, bool caps = true);         // base at y=0
// Surface of revolution around +Y from a 2D profile (x = radius, y = height), profile ordered
// bottom to top. uv.x = angle/2pi, uv.y = normalised arc length. Smooth normals across the
// profile unless consecutive points are duplicated (hard crease).
MeshData lathe(const std::vector<m::vec2>& profile, int segments = 96);
}  // namespace prim
