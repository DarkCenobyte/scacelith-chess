#include "mesh.h"
#include "../core/log.h"
#include <cstddef>

using namespace m;

AABB MeshData::bounds() const {
    AABB b;
    for (auto& v : vertices) b.add(v.pos);
    return b;
}

void MeshData::append(const MeshData& o, const mat4& t) {
    uint32_t base = uint32_t(vertices.size());
    mat3 nm = normalMatrix(t);
    mat3 tm = t.upper3();
    bool flip = determinant(tm) < 0;
    for (Vertex v : o.vertices) {
        v.pos = transformPoint(t, v.pos);
        v.normal = normalize(nm * v.normal);
        vec3 tg = normalize(tm * v.tangent.xyz());
        v.tangent = vec4(tg, flip ? -v.tangent.w : v.tangent.w);
        vertices.push_back(v);
    }
    for (size_t i = 0; i < o.indices.size(); i += 3) {
        if (flip) {
            indices.push_back(base + o.indices[i]);
            indices.push_back(base + o.indices[i + 2]);
            indices.push_back(base + o.indices[i + 1]);
        } else {
            for (int k = 0; k < 3; ++k) indices.push_back(base + o.indices[i + k]);
        }
    }
}

void MeshData::transform(const mat4& t) {
    MeshData tmp;
    tmp.append(*this, t);
    *this = std::move(tmp);
}

void MeshData::computeNormals(bool smooth) {
    if (!smooth) {
        // Unshare vertices so each triangle gets its face normal.
        std::vector<Vertex> nv;
        std::vector<uint32_t> ni;
        for (size_t i = 0; i < indices.size(); i += 3) {
            Vertex a = vertices[indices[i]], b = vertices[indices[i + 1]], c = vertices[indices[i + 2]];
            vec3 n = normalize(cross(b.pos - a.pos, c.pos - a.pos));
            a.normal = b.normal = c.normal = n;
            for (auto* v : {&a, &b, &c}) { ni.push_back(uint32_t(nv.size())); nv.push_back(*v); }
        }
        vertices.swap(nv);
        indices.swap(ni);
        return;
    }
    for (auto& v : vertices) v.normal = vec3(0);
    for (size_t i = 0; i < indices.size(); i += 3) {
        Vertex &a = vertices[indices[i]], &b = vertices[indices[i + 1]], &c = vertices[indices[i + 2]];
        vec3 n = cross(b.pos - a.pos, c.pos - a.pos);  // area weighted
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (auto& v : vertices) v.normal = normalize(v.normal);
}

void MeshData::computeTangents() {
    std::vector<vec3> tan(vertices.size(), vec3(0)), bit(vertices.size(), vec3(0));
    for (size_t i = 0; i < indices.size(); i += 3) {
        uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        const Vertex &a = vertices[i0], &b = vertices[i1], &c = vertices[i2];
        vec3 e1 = b.pos - a.pos, e2 = c.pos - a.pos;
        vec2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
        float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;
        float r = 1.0f / det;
        vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        vec3 bt = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t k : {i0, i1, i2}) { tan[k] += t; bit[k] += bt; }
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        vec3 n = vertices[i].normal;
        vec3 t = tan[i] - n * dot(n, tan[i]);
        if (length2(t) < 1e-16f) t = orthogonal(n);
        t = normalize(t);
        float w = dot(cross(n, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
        vertices[i].tangent = vec4(t, w);
    }
}

void MeshData::flipWinding() {
    for (size_t i = 0; i < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
    for (auto& v : vertices) v.normal = -v.normal;
}

void Mesh::upload(const MeshData& d, const char* debugName) {
    destroy();
    if (debugName) name = debugName;
    glCreateBuffers(1, &vbo);
    glNamedBufferStorage(vbo, GLsizeiptr(d.vertices.size() * sizeof(Vertex)), d.vertices.data(), 0);
    glCreateBuffers(1, &ibo);
    glNamedBufferStorage(ibo, GLsizeiptr(d.indices.size() * sizeof(uint32_t)), d.indices.data(), 0);
    glCreateVertexArrays(1, &vao);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, sizeof(Vertex));
    glVertexArrayElementBuffer(vao, ibo);
    glEnableVertexArrayAttrib(vao, 0);
    glEnableVertexArrayAttrib(vao, 1);
    glEnableVertexArrayAttrib(vao, 2);
    glEnableVertexArrayAttrib(vao, 3);
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, pos));
    glVertexArrayAttribFormat(vao, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
    glVertexArrayAttribFormat(vao, 2, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, tangent));
    glVertexArrayAttribFormat(vao, 3, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    for (int i = 0; i < 4; ++i) glVertexArrayAttribBinding(vao, GLuint(i), 0);
    indexCount = uint32_t(d.indices.size());
    vertexCount = uint32_t(d.vertices.size());
    bounds = d.bounds();
    if (debugName) glObjectLabel(GL_VERTEX_ARRAY, vao, -1, debugName);
}

void Mesh::destroy() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ibo) glDeleteBuffers(1, &ibo);
    vao = vbo = ibo = 0;
    indexCount = vertexCount = 0;
}

namespace prim {

MeshData box(vec3 h) {
    MeshData d;
    struct Face { vec3 n, u, v; } faces[] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    for (auto& f : faces) {
        uint32_t base = uint32_t(d.vertices.size());
        for (int k = 0; k < 4; ++k) {
            float su = (k == 1 || k == 2) ? 1.0f : -1.0f, sv = (k >= 2) ? 1.0f : -1.0f;
            Vertex v;
            v.pos = (f.n + f.u * su + f.v * sv) * h;
            v.normal = f.n;
            v.tangent = vec4(f.u, 1);
            v.uv = vec2(su * 0.5f + 0.5f, sv * 0.5f + 0.5f);
            d.vertices.push_back(v);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) d.indices.push_back(base + i);
    }
    return d;
}

MeshData roundedBox(vec3 h, float r, int seg) {
    // Sphere-projected cube: build a subdivided cube and push vertices onto the rounded surface.
    MeshData d;
    int n = std::max(2, seg * 2 + 2);
    vec3 inner = max(h - vec3(r), vec3(0));
    struct Face { vec3 n, u, v; } faces[] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    for (auto& f : faces) {
        uint32_t base = uint32_t(d.vertices.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) {
                // Uniform grid over the face, in [-1, 1].
                float su = float(i) / n * 2 - 1, sv = float(j) / n * 2 - 1;
                vec3 p = (f.n + f.u * su + f.v * sv) * h;
                vec3 q = vec3(clamp(p.x, -inner.x, inner.x), clamp(p.y, -inner.y, inner.y), clamp(p.z, -inner.z, inner.z));
                vec3 dir = p - q;
                vec3 nrm = length2(dir) > 1e-12f ? normalize(dir) : f.n;
                Vertex v;
                v.pos = q + nrm * r;
                v.normal = nrm;
                v.tangent = vec4(f.u, 1);
                v.uv = vec2(su * 0.5f + 0.5f, sv * 0.5f + 0.5f);
                d.vertices.push_back(v);
            }
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                uint32_t a = base + uint32_t(j * (n + 1) + i), b = a + 1, c = a + uint32_t(n + 1), e = c + 1;
                for (uint32_t k : {a, b, e, a, e, c}) d.indices.push_back(k);
            }
    }
    return d;
}

MeshData sphere(float radius, int su, int sv) {
    MeshData d;
    for (int j = 0; j <= sv; ++j) {
        float v = float(j) / sv, th = v * PI;
        for (int i = 0; i <= su; ++i) {
            float u = float(i) / su, ph = u * TAU;
            vec3 n{std::sin(th) * std::cos(ph), std::cos(th), -std::sin(th) * std::sin(ph)};
            Vertex vx;
            vx.pos = n * radius;
            vx.normal = n;
            vx.tangent = vec4(normalize(vec3(-std::sin(ph), 0, -std::cos(ph))), 1);
            vx.uv = vec2(u, 1 - v);
            d.vertices.push_back(vx);
        }
    }
    for (int j = 0; j < sv; ++j)
        for (int i = 0; i < su; ++i) {
            uint32_t a = uint32_t(j * (su + 1) + i), b = a + 1, c = a + uint32_t(su + 1), e = c + 1;
            for (uint32_t k : {a, c, b, b, c, e}) d.indices.push_back(k);
        }
    return d;
}

MeshData plane(float sx, float sz, int nx, int nz, float uvScale) {
    MeshData d;
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nx; ++i) {
            float u = float(i) / nx, v = float(j) / nz;
            Vertex vx;
            vx.pos = vec3((u - 0.5f) * sx, 0, (0.5f - v) * sz);
            vx.normal = vec3(0, 1, 0);
            vx.tangent = vec4(1, 0, 0, 1);
            vx.uv = vec2(u * sx, v * sz) * uvScale;
            d.vertices.push_back(vx);
        }
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i) {
            uint32_t a = uint32_t(j * (nx + 1) + i), b = a + 1, c = a + uint32_t(nx + 1), e = c + 1;
            for (uint32_t k : {a, b, e, a, e, c}) d.indices.push_back(k);
        }
    return d;
}

MeshData cylinder(float radius, float height, int seg, bool caps) {
    std::vector<vec2> prof;
    if (caps) { prof.push_back({0, 0}); prof.push_back({radius, 0}); }
    prof.push_back({radius, 0});
    prof.push_back({radius, height});
    if (caps) { prof.push_back({radius, height}); prof.push_back({0, height}); }
    return lathe(prof, seg);
}

MeshData lathe(const std::vector<vec2>& prof, int seg) {
    MeshData d;
    int np = int(prof.size());
    if (np < 2) return d;
    // Arc length parametrisation for v.
    std::vector<float> arc(np, 0.0f);
    for (int i = 1; i < np; ++i) arc[i] = arc[i - 1] + length(prof[i] - prof[i - 1]);
    float total = std::max(arc.back(), 1e-6f);
    // 2D profile normals: average of adjacent segment normals unless a duplicate point marks a crease.
    auto segNormal = [&](int a, int b) {
        vec2 t = prof[b] - prof[a];
        vec2 n{t.y, -t.x};
        float l = length(n);
        return l > 1e-9f ? n / l : vec2(0, 0);
    };
    std::vector<vec2> nrm(np);
    for (int i = 0; i < np; ++i) {
        vec2 n(0, 0);
        bool dupPrev = i > 0 && length(prof[i] - prof[i - 1]) < 1e-7f;
        bool dupNext = i < np - 1 && length(prof[i + 1] - prof[i]) < 1e-7f;
        if (i > 0 && !dupPrev) n += segNormal(i - 1, i);
        if (i < np - 1 && !dupNext) n += segNormal(i, i + 1);
        if (length(n) < 1e-9f) {
            if (dupPrev && i < np - 1) n = segNormal(i, i + 1);
            else if (dupNext && i > 0) n = segNormal(i - 1, i);
        }
        nrm[i] = length(n) > 1e-9f ? normalize(n) : vec2(0, 1);
    }
    for (int i = 0; i < np; ++i) {
        for (int s = 0; s <= seg; ++s) {
            float u = float(s) / seg, a = u * TAU;
            float c = std::cos(a), sn = std::sin(a);
            Vertex v;
            v.pos = vec3(prof[i].x * c, prof[i].y, -prof[i].x * sn);
            v.normal = normalize(vec3(nrm[i].x * c, nrm[i].y, -nrm[i].x * sn));
            v.tangent = vec4(-sn, 0, -c, 1);
            v.uv = vec2(u, arc[i] / total);
            d.vertices.push_back(v);
        }
    }
    for (int i = 0; i < np - 1; ++i) {
        if (length(prof[i + 1] - prof[i]) < 1e-7f) continue;  // crease duplicate
        for (int s = 0; s < seg; ++s) {
            uint32_t a = uint32_t(i * (seg + 1) + s), b = a + 1, c = a + uint32_t(seg + 1), e = c + 1;
            for (uint32_t k : {a, b, e, a, e, c}) d.indices.push_back(k);
        }
    }
    return d;
}
}  // namespace prim
