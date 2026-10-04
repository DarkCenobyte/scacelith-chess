// Palm cross-sections (x extent of the palm SDF at y, z).
#include "geo.h"
int main() {
    static ArmGeo G;
    G.build(false);
    for (float y : {-0.020f, -0.035f, -0.050f, -0.065f, -0.078f}) {
        std::printf("y %.3f:", y);
        for (float z = -0.036f; z <= 0.0401f; z += 0.004f) {
            float lo = 1, hi = -1;
            for (float x = -0.03f; x <= 0.03f; x += 0.0002f)
                if (G.exact(2, vec3(x, y, z)) < 0) { lo = std::min(lo, x); hi = std::max(hi, x); }
            if (hi < lo) std::printf(" z%+.3f:-", z);
            else std::printf(" z%+.3f:[%+.4f,%+.4f]", z, lo, hi);
        }
        std::printf("\n");
    }
}
