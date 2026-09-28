// Material surface contract. A material file (shaders/materials/*.glsl) implements:
//     void surface(in SurfaceInput i, inout Surface s);
// It receives geometry data in SurfaceInput and fills the Surface (pre-initialised with
// defaultSurface()). It must not do lighting: the renderer lights the Surface in every pass.
//
// Guidance for photoreal materials:
//  * Keep albedo in physically plausible ranges (white marble ~0.8, black marble ~0.03-0.05).
//  * roughness is perceptual (alpha = roughness^2). Polished marble ~0.05-0.12 + clearcoat.
//  * Use positionOS for volumetric/3D patterns (marble veins continue across edges, stable under
//    animation) and objectSeed to vary each object.
//  * normalWS is the shading normal; perturb it with detail normals (tangent frame provided).

struct SurfaceInput {
    vec3 positionWS;
    vec3 positionOS;       // object space (mesh space before the model matrix)
    vec3 normalWS;         // interpolated vertex normal, normalized, flipped for back faces if double sided
    vec3 normalOS;
    vec3 tangentWS;        // orthonormalised against normalWS
    vec3 bitangentWS;
    vec2 uv;
    vec3 viewDirWS;        // surface -> camera, normalized
    float viewDistance;
    vec4 matParams[8];     // Material::params
    vec4 instParams[4];    // DrawItem::inst
    float objectSeed;      // [0,1) stable per object id
    float objectId;
    bool frontFacing;
    vec2 screenUV;
    vec2 pixel;            // gl_FragCoord.xy
    float time;
    int passId;            // PASS_ID_*
};

struct Surface {
    vec3 albedo;              // linear base colour
    float alpha;              // coverage / opacity (transparent materials, alpha test)
    vec3 normalWS;            // shading normal
    float roughness;          // perceptual roughness [0.02, 1]
    float metallic;           // 0 dielectric, 1 metal
    float specular;           // dielectric reflectance: F0 = 0.16 * specular^2 (0.5 -> 0.04)
    float occlusion;          // cavity / baked AO [0,1]
    float clearcoat;          // clear coat layer strength [0,1] (varnish, polish, glaze)
    float clearcoatRoughness;
    vec3 clearcoatNormalWS;   // usually the geometric normal (smooth coat over a bumpy base)
    vec3 sheenColor;          // cloth sheen colour (0 = none)
    float sheenRoughness;
    float subsurface;         // [0,1] subsurface / translucency amount
    vec3 subsurfaceColor;     // scattering tint
    float subsurfaceRadius;   // mean free path (m), marble ~0.004-0.02
    float thickness;          // > 0: thin-surface transmission (cloth, porcelain rims, lampshades)
    float anisotropy;         // [-1,1] along anisotropyDirWS
    vec3 anisotropyDirWS;
    vec3 emission;            // emitted radiance in nits (renderer applies exposure)
    float ior;                // index of refraction (transparent/glass)
    float transmission;       // [0,1] fraction transmitted (glass)
};

Surface defaultSurface(SurfaceInput i) {
    Surface s;
    s.albedo = vec3(0.5);
    s.alpha = 1.0;
    s.normalWS = i.normalWS;
    s.roughness = 0.5;
    s.metallic = 0.0;
    s.specular = 0.5;
    s.occlusion = 1.0;
    s.clearcoat = 0.0;
    s.clearcoatRoughness = 0.05;
    s.clearcoatNormalWS = i.normalWS;
    s.sheenColor = vec3(0.0);
    s.sheenRoughness = 0.5;
    s.subsurface = 0.0;
    s.subsurfaceColor = vec3(1.0);
    s.subsurfaceRadius = 0.01;
    s.thickness = 0.0;
    s.anisotropy = 0.0;
    s.anisotropyDirWS = i.tangentWS;
    s.emission = vec3(0.0);
    s.ior = 1.5;
    s.transmission = 0.0;
    return s;
}

// Tangent-space normal (xy in [-1,1], z up) to world.
vec3 perturbNormal(SurfaceInput i, vec3 nTS) {
    return normalize(i.tangentWS * nTS.x + i.bitangentWS * nTS.y + i.normalWS * nTS.z);
}
// Bump mapping from a height function's screen-space derivatives (no tangents needed).
vec3 bumpFromHeight(vec3 posWS, vec3 n, float h, float strength) {
    vec3 dpdx = dFdx(posWS), dpdy = dFdy(posWS);
    float dhdx = dFdx(h), dhdy = dFdy(h);
    vec3 r1 = cross(dpdy, n), r2 = cross(n, dpdx);
    float det = dot(dpdx, r1);
    vec3 grad = sign(det) * (dhdx * r1 + dhdy * r2);
    return normalize(abs(det) * n - strength * grad);
}
