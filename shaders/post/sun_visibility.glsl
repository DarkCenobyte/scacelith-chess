// Sun visibility of a world point through the shadow cascades (one compare tap, 1 outside them),
// shared by the volumetric sun shafts and the dust motes. Include after post_common.glsl.
layout(binding = 5) uniform sampler2DArrayShadow uShadow;  // compare sampler (LEQUAL), standard depth

float sunVisibility(vec3 p) {
    int n = int(frame.shadowParams.x);
    for (int c = 0; c < n; ++c) {
        vec3 s = (frame.shadowMatrix[c] * vec4(p, 1.0)).xyz;
        if (all(greaterThan(s.xy, vec2(0.005))) && all(lessThan(s.xy, vec2(0.995))) && s.z > 0.0 && s.z < 1.0)
            return texture(uShadow, vec4(s.xy, float(c), s.z - 0.0002));
    }
    return 1.0;
}
