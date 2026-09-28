// Test material of the lighting validation scenes ("lightbox", "testbed"): polished stone with
// optional floor tiles and veins. Not a game material (see shaders/materials for those).
//   params[0] = albedo.rgb, roughness
//   params[1] = metallic, specular, clearcoat, clearcoat roughness
//   params[2] = vein colour.rgb, vein strength
//   params[3] = subsurface colour.rgb, subsurface amount
//   params[4] = x tile size (m, 0 = none), y vein frequency (1/m), z subsurface radius (m), w grout darkness
//   params[5] = sheen colour.rgb, sheen roughness
//   params[6] = x anisotropy (along the tangent), y thin thickness (m, 0 = none), z emission (nits x albedo)
void surface(in SurfaceInput i, inout Surface s) {
    vec4 p0 = i.matParams[0], p1 = i.matParams[1], p2 = i.matParams[2], p3 = i.matParams[3], p4 = i.matParams[4];
    s.albedo = p0.rgb;
    s.roughness = p0.a;
    s.metallic = p1.x;
    s.specular = p1.y;
    s.clearcoat = p1.z;
    s.clearcoatRoughness = p1.w;
    s.subsurfaceColor = p3.rgb;
    s.subsurface = p3.a;
    s.subsurfaceRadius = max(p4.z, 1e-4);
    s.sheenColor = i.matParams[5].rgb;
    s.sheenRoughness = i.matParams[5].a;
    vec4 p6 = i.matParams[6];
    s.anisotropy = p6.x;
    s.anisotropyDirWS = i.tangentWS;
    s.thickness = p6.y;
    vec3 q = i.positionOS;
    if (p2.a > 0.0 && p4.y > 0.0) {
        vec3 w = q * p4.y + vec3(i.objectSeed * 37.0, 0.0, i.objectSeed * 11.0);
        w += 0.7 * vec3(gnoise(w * 0.5), gnoise(w * 0.5 + 7.3), gnoise(w * 0.5 + 3.1));
        float v = ridged(w, 3);
        float vein = clamp(pow(v, 5.0) * p2.a, 0.0, 1.0);
        s.albedo = mix(s.albedo, p2.rgb, vein);
    }
    if (p4.x > 0.0) {
        vec2 t = fract(q.xz / p4.x);
        float g = min(min(t.x, 1.0 - t.x), min(t.y, 1.0 - t.y)) * p4.x;
        float grout = 1.0 - smoothstep(0.0008, 0.0025, g);
        s.albedo *= 1.0 - grout * p4.w;
        s.roughness = mix(s.roughness, 0.7, grout);
        s.clearcoat *= 1.0 - grout;
        // Slightly uneven tiles: tilt each tile's coat a hair (breaks the perfect mirror).
        vec2 cell = floor(q.xz / p4.x);
        vec2 tilt = vec2(hash12(cell) - 0.5, hash12(cell + 17.0) - 0.5) * 0.004;
        s.clearcoatNormalWS = normalize(i.normalWS + i.tangentWS * tilt.x + i.bitangentWS * tilt.y);
    }
    s.emission = s.albedo * p6.z;
}
