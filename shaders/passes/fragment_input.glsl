// Fragment-side interpolants and SurfaceInput construction shared by the mesh passes.
in VertexData {
    vec3 posWS;
    vec3 posOS;
    vec3 normalWS;
    vec3 normalOS;
    vec4 tangentWS;
    vec2 uv;
    vec4 curClip;
    vec4 prevClip;
    flat int draw;
} vin;

SurfaceInput buildSurfaceInput() {
    SurfaceInput i;
    DrawData dd = draws[vin.draw];
    i.positionWS = vin.posWS;
    i.positionOS = vin.posOS;
    vec3 n = normalize(vin.normalWS);
#ifdef MATERIAL_DOUBLE_SIDED
    if (!gl_FrontFacing) n = -n;
#endif
    i.normalWS = n;
    i.normalOS = normalize(vin.normalOS);
    vec3 t = vin.tangentWS.xyz - n * dot(n, vin.tangentWS.xyz);
    t = dot(t, t) > 1e-10 ? normalize(t) : normalize(cross(n, abs(n.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    i.tangentWS = t;
    i.bitangentWS = cross(n, t) * (vin.tangentWS.w < 0.0 ? -1.0 : 1.0);
    i.uv = vin.uv;
    vec3 toCam = frame.cameraPos.xyz - vin.posWS;
    i.viewDistance = length(toCam);
    i.viewDirWS = toCam / max(i.viewDistance, 1e-6);
    for (int k = 0; k < 8; ++k) i.matParams[k] = dd.matParams[k];
    for (int k = 0; k < 4; ++k) i.instParams[k] = dd.instParams[k];
    i.objectSeed = dd.info.x;
    i.objectId = dd.info.w;
    i.frontFacing = gl_FrontFacing;
    i.pixel = gl_FragCoord.xy;
    i.screenUV = gl_FragCoord.xy * frame.resolution.zw;
    i.time = frame.cameraPos.w;
    i.passId = int(frame.passInfo.x);
    return i;
}

vec2 motionVector() {
    vec2 cur = vin.curClip.xy / vin.curClip.w;
    vec2 prev = vin.prevClip.xy / vin.prevClip.w;
    return (cur - prev) * 0.5;
}
