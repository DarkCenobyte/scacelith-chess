#include "model.h"

void GpuModel::upload(const Model& m) {
    destroy();
    parts.resize(m.parts.size());
    for (size_t i = 0; i < m.parts.size(); ++i) {
        parts[i].mesh.upload(m.parts[i].mesh, m.parts[i].name.c_str());
        parts[i].material = m.parts[i].material;
        parts[i].flags = m.parts[i].flags;
        for (int k = 0; k < 4; ++k) parts[i].inst[k] = m.parts[i].inst[k];
    }
}

void GpuModel::destroy() {
    for (auto& p : parts) p.mesh.destroy();
    parts.clear();
}
