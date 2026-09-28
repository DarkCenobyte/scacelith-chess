// Dust mote sprite: Gaussian footprint (self-antialiased), manual depth test against the scene.
#include "shaders/post/post_common.glsl"
layout(binding = 0) uniform sampler2D uLinearDepth;
in vec2 vLocal;
in vec3 vColor;
in float vDepth;
layout(location = 0) out vec4 outColor;

void main() {
    float sceneZ = texelFetch(uLinearDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (vDepth > sceneZ) discard;
    float r2 = dot(vLocal, vLocal);
    if (r2 > 1.0) discard;
    float a = exp(-r2 * 4.0) - exp(-4.0);
    outColor = vec4(vColor * a, 0.0);
}
