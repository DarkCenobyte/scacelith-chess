// Shared geometry-stage code: transforms an object-space vertex into all interpolants the
// fragment stages expect. Used by mesh.vert (untessellated) and mesh.tese.
#include "shaders/include/common.glsl"

out VertexData {
    vec3 posWS;
    vec3 posOS;
    vec3 normalWS;
    vec3 normalOS;
    vec4 tangentWS;
    vec2 uv;
    vec4 curClip;   // unjittered current clip position (motion vectors)
    vec4 prevClip;  // previous frame clip position
    flat int draw;
} vout;

out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};
invariant gl_Position;

#pragma displacement

void emitVertex(vec3 posOS, vec3 nOS, vec4 tOS, vec2 uv, int drawIdx) {
    DrawData dd = draws[drawIdx];
#ifdef MATERIAL_HAS_DISPLACEMENT
    posOS = displace(posOS, nOS, uv, drawIdx);
#endif
    vec4 wp = dd.model * vec4(posOS, 1.0);
    vout.posWS = wp.xyz;
    vout.posOS = posOS;
    mat3 nm = mat3(dd.normalMatrix);
    vout.normalWS = normalize(nm * nOS);
    vout.normalOS = nOS;
    vout.tangentWS = vec4(normalize(mat3(dd.model) * tOS.xyz), tOS.w);
    vout.uv = uv;
    vout.curClip = frame.viewProjNoJitter * wp;
    vout.prevClip = frame.prevViewProj * (dd.prevModel * vec4(posOS, 1.0));
    vout.draw = drawIdx;
    gl_Position = frame.viewProj * wp;
    gl_ClipDistance[0] = dot(wp.xyz, frame.clipPlane.xyz) + frame.clipPlane.w;
}
