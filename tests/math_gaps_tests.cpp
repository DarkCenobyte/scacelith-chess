// The math helpers the other tests leave out: ray against box (hit from outside, from inside, a
// ray parallel to a face outside its slab, a box behind) and against sphere (hit, from inside,
// missed, behind), and the rotation about Z.
#include "test.h"
#include "math/math.h"

#include <cmath>

namespace {

bool nearly(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

}  // namespace

TEST(math_gaps_ray_box) {
    m::AABB box;
    box.add(m::vec3(0, 0, 0));
    box.add(m::vec3(1, 1, 1));
    CHECK(nearly(m::rayAABB({m::vec3(-5, 0.5f, 0.5f), m::vec3(1, 0, 0)}, box), 5.0f));
    CHECK(nearly(m::rayAABB({m::vec3(0.5f, 4, 0.5f), m::vec3(0, -1, 0)}, box), 3.0f));
    CHECK(nearly(m::rayAABB({m::vec3(0.5f, 0.5f, 0.5f), m::vec3(0, 0, 1)}, box), 0.0f));    // from inside
    CHECK(m::rayAABB({m::vec3(-5, 2, 0.5f), m::vec3(1, 0, 0)}, box) < 0.0f);              // parallel, above
    CHECK(m::rayAABB({m::vec3(5, 0.5f, 0.5f), m::vec3(1, 0, 0)}, box) < 0.0f);            // box behind
    CHECK(m::rayAABB({m::vec3(-5, -5, 0.5f), m::normalize(m::vec3(1, 2, 0))}, box) < 0.0f);   // passes beside
    const float diag = m::rayAABB({m::vec3(-1, -1, -1), m::normalize(m::vec3(1, 1, 1))}, box);
    CHECK(nearly(diag, std::sqrt(3.0f), 1e-4f));
}

TEST(math_gaps_ray_sphere) {
    const m::vec3 c(0, 0, 0);
    CHECK(nearly(m::raySphere({m::vec3(0, 0, -5), m::vec3(0, 0, 1)}, c, 1.0f), 4.0f));
    CHECK(nearly(m::raySphere({m::vec3(0, 0, 0), m::vec3(0, 0, 1)}, c, 1.0f), 1.0f));      // from inside: the far side
    CHECK(m::raySphere({m::vec3(0, 2, -5), m::vec3(0, 0, 1)}, c, 1.0f) < 0.0f);          // missed
    CHECK(m::raySphere({m::vec3(0, 0, 5), m::vec3(0, 0, 1)}, c, 1.0f) < 0.0f);           // behind
    CHECK(nearly(m::raySphere({m::vec3(0, 1, -5), m::vec3(0, 0, 1)}, c, 1.0f), 5.0f, 1e-3f));   // grazing
}

TEST(math_gaps_rotate_z) {
    const float a = 0.5f * m::PI;
    const m::mat4 r = m::rotateZ(a);
    const m::vec3 x = m::transformPoint(r, m::vec3(1, 0, 0));
    CHECK(nearly(x.x, 0.0f) && nearly(x.y, 1.0f) && nearly(x.z, 0.0f));
    const m::vec3 z = m::transformPoint(r, m::vec3(0, 0, 2));
    CHECK(nearly(z.x, 0.0f) && nearly(z.y, 0.0f) && nearly(z.z, 2.0f));
    const m::mat4 same = m::rotateAxis(m::vec3(0, 0, 1), 0.3f), rz = m::rotateZ(0.3f);
    for (int col = 0; col < 4; ++col) {
        CHECK(nearly(same.c[col].x, rz.c[col].x) && nearly(same.c[col].y, rz.c[col].y));
        CHECK(nearly(same.c[col].z, rz.c[col].z) && nearly(same.c[col].w, rz.c[col].w));
    }
}
