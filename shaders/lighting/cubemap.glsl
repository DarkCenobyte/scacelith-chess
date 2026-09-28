// Cubemap and sampling helpers shared by the lighting compute passes.

// Direction of texel centre uv in [0,1]^2 on a GL cubemap face (face order +X -X +Y -Y +Z -Z),
// following the GL spec face table (s along texel columns, t along texel rows).
vec3 cubeDir(int face, vec2 uv) {
    vec2 st = uv * 2.0 - 1.0;
    float s = st.x, t = st.y;
    vec3 d;
    if (face == 0) d = vec3(1.0, -t, -s);
    else if (face == 1) d = vec3(-1.0, -t, s);
    else if (face == 2) d = vec3(s, 1.0, t);
    else if (face == 3) d = vec3(s, -1.0, -t);
    else if (face == 4) d = vec3(s, -t, 1.0);
    else d = vec3(-s, -t, -1.0);
    return normalize(d);
}

float cubeAreaElement(float x, float y) { return atan(x * y, sqrt(x * x + y * y + 1.0)); }
// Solid angle of a texel centred at st (in [-1,1]) with half-size h (= 1/size).
float cubeTexelSolidAngle(vec2 st, float h) {
    float x0 = st.x - h, x1 = st.x + h, y0 = st.y - h, y1 = st.y + h;
    return cubeAreaElement(x0, y0) - cubeAreaElement(x0, y1) - cubeAreaElement(x1, y0) + cubeAreaElement(x1, y1);
}

vec2 hammersley(uint i, uint n) {
    uint b = i;
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return vec2(float(i) / float(n), float(b) * 2.3283064365386963e-10);
}

// GGX-distributed half vector around +Z (alpha = perceptual roughness^2).
vec3 importanceSampleGGX(vec2 xi, float a) {
    float phi = TAU * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
    return vec3(sinT * cos(phi), sinT * sin(phi), cosT);
}

mat3 tangentFrame(vec3 n) {
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    return mat3(t, b, n);
}
