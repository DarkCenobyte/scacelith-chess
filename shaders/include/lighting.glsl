// Scene lighting for forward passes. Owned by the render-lighting work package; this baseline
// provides sun + cascaded PCF shadows + point lights + hemisphere/planar ambient so materials can
// be authored immediately. Entry point: vec3 shadeSurface(SurfaceInput i, Surface s).
#include "shaders/include/brdf.glsl"

layout(binding = 8) uniform sampler2DArrayShadow uShadowMap;
layout(binding = 9) uniform sampler2DArray uShadowDepth;
layout(binding = 12) uniform sampler2D uAO;
layout(binding = 13) uniform sampler2DArray uPlanar;

const vec2 kPoisson16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

int shadowCascadeFor(vec3 p) {
    int n = int(frame.shadowParams.x);
    for (int c = 0; c < n; ++c) {
        vec3 uvz = (frame.shadowMatrix[c] * vec4(p, 1.0)).xyz;
        if (all(greaterThan(uvz.xy, vec2(0.02))) && all(lessThan(uvz.xy, vec2(0.98))) && uvz.z < 0.999) return c;
    }
    return -1;
}

float sunShadow(vec3 posWS, vec3 nGeom, vec2 pixel) {
    vec3 L = frame.sunDirection.xyz;
    int c = shadowCascadeFor(posWS);
    if (c < 0) return 1.0;
    float radius = frame.shadowCascade[c].w;
    float texel = 2.0 * radius / float(textureSize(uShadowDepth, 0).x);
    float NoL = saturate(dot(nGeom, L));
    vec3 p = posWS + nGeom * texel * (1.5 + 2.0 * (1.0 - NoL)) + L * texel * 0.5;
    vec3 uvz = (frame.shadowMatrix[c] * vec4(p, 1.0)).xyz;
    float spread = (c == 0 ? 2.5 : 1.5) * texel / (2.0 * radius);  // filter radius in uv
    float rot = ign(pixel) * TAU;
    mat2 R = mat2(cos(rot), sin(rot), -sin(rot), cos(rot));
    float sum = 0.0;
    for (int k = 0; k < 16; ++k) {
        vec2 o = R * kPoisson16[k] * spread;
        sum += texture(uShadowMap, vec4(uvz.xy + o, float(c), uvz.z));
    }
    return sum / 16.0;
}

vec3 f0Of(Surface s) {
    vec3 dielectric = vec3(0.16 * s.specular * s.specular);
    return mix(dielectric, s.albedo, s.metallic);
}

// Direct lighting from one light with unit-less radiance 'radiance' (already includes attenuation).
vec3 evalDirect(SurfaceInput i, Surface s, vec3 L, vec3 radiance, float shadow) {
    vec3 N = s.normalWS, V = i.viewDirWS;
    vec3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoL = dot(N, L);
    float NoH = saturate(dot(N, H)), LoH = saturate(dot(L, H)), VoH = saturate(dot(V, H));
    float a = max(s.roughness * s.roughness, 0.002);
    vec3 f0 = f0Of(s);
    vec3 color = vec3(0.0);
    float NoLc = saturate(NoL);
    // Specular (GGX)
    vec3 F = F_Schlick(f0, VoH);
    vec3 spec = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoLc, a) * F;
    // Diffuse with optional wrap for subsurface materials.
    vec3 diffColor = s.albedo * (1.0 - s.metallic);
    float wrap = s.subsurface * 0.5;
    float NoLw = saturate((NoL + wrap) / ((1.0 + wrap) * (1.0 + wrap)));
    vec3 diff = diffColor * Fd_Burley(NoV, NoLc, LoH, s.roughness);
    vec3 sss = diffColor * s.subsurfaceColor * (NoLw - NoLc) * INV_PI * s.subsurface;
    // Sheen
    vec3 sheen = s.sheenColor * D_Charlie(s.sheenRoughness, NoH) * V_Neubelt(NoV, NoLc);
    color = (diff + spec * (1.0 - max(s.sheenColor.r, max(s.sheenColor.g, s.sheenColor.b)) * 0.0) + sheen) * NoLc + sss;
    // Clear coat
    if (s.clearcoat > 0.0) {
        vec3 Nc = s.clearcoatNormalWS;
        float NoHc = saturate(dot(Nc, H)), NoLcc = saturate(dot(Nc, L));
        float ac = max(s.clearcoatRoughness * s.clearcoatRoughness, 0.002);
        float Fc = F_Schlick(0.04, 1.0, VoH) * s.clearcoat;
        float coat = D_GGX(NoHc, ac) * V_Kelemen(LoH) * Fc;
        color = color * (1.0 - Fc) + coat * NoLcc;
    }
    // Thin translucency (light from behind through cloth / thin porcelain)
    if (s.thickness > 0.0) {
        float back = saturate(-NoL);
        color += diffColor * s.subsurfaceColor * back * INV_PI * exp(-s.thickness * 40.0) * s.subsurface;
    }
    return color * radiance * shadow;
}

vec3 hemisphereAmbient(vec3 n) {
    float t = n.y * 0.5 + 0.5;
    return mix(frame.ambientGround.rgb, frame.ambientSky.rgb, t);
}

vec3 samplePlanar(int layer, vec3 posWS, vec3 N, float roughness) {
    vec4 clip = frame.planarViewProj[layer] * vec4(posWS, 1.0);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    // Distort by the shading normal (bumps / imperfections in the polished surface).
    vec3 pn = frame.planarPlanes[layer].xyz;
    uv += (N - pn * dot(N, pn)).xz * 0.25;
    float lod = roughness * 6.0;
    return textureLod(uPlanar, vec3(uv, float(layer)), lod).rgb;
}

vec3 ambientSpecular(SurfaceInput i, Surface s, vec3 R, float planarLayer) {
#if !defined(PASS_PLANAR) && !defined(PASS_PROBE)
    if (planarLayer >= 0.0 && frame.passInfo.w > planarLayer) return samplePlanar(int(planarLayer), i.positionWS, s.normalWS, s.roughness);
#endif
    return hemisphereAmbient(R);
}

vec3 shadeSurface(SurfaceInput i, Surface s, float planarLayer) {
    vec3 N = s.normalWS, V = i.viewDirWS;
    float NoV = max(dot(N, V), 1e-4);
    float shadow = sunShadow(i.positionWS, i.normalWS, i.pixel);
    vec3 color = evalDirect(i, s, frame.sunDirection.xyz, frame.sunRadiance.rgb, shadow);
    int nl = int(frame.passInfo.z);
    for (int k = 0; k < nl; ++k) {
        PointLightData pl = pointLights[k];
        vec3 d = pl.position - i.positionWS;
        float dist2 = dot(d, d);
        float falloff = sq(saturate(1.0 - sq(dist2 / (pl.radius * pl.radius)))) / max(dist2, 1e-4);
        color += evalDirect(i, s, d * inversesqrt(dist2), pl.color * pl.intensity * falloff * frame.exposure.x, 1.0);
    }
    float ao = s.occlusion;
#ifdef PASS_MAIN
    ao *= texture(uAO, i.screenUV).r;
#endif
    vec3 diffColor = s.albedo * (1.0 - s.metallic);
    color += diffColor * hemisphereAmbient(N) * ao * (1.0 + s.subsurface * 0.3);
    vec3 f0 = f0Of(s);
    vec2 dfg = envBRDFApprox(NoV, s.roughness);
    vec3 R = reflect(-V, N);
    float specOcc = saturate(pow(NoV + ao, exp2(-16.0 * s.roughness - 1.0)) - 1.0 + ao);
    color += ambientSpecular(i, s, R, planarLayer) * (f0 * dfg.x + dfg.y) * specOcc;
    if (s.clearcoat > 0.0) {
        float NoVc = max(dot(s.clearcoatNormalWS, V), 1e-4);
        float Fc = F_Schlick(0.04, 1.0, NoVc) * s.clearcoat;
        vec3 Rc = reflect(-V, s.clearcoatNormalWS);
        Surface sc = s;
        sc.roughness = s.clearcoatRoughness;
        sc.normalWS = s.clearcoatNormalWS;
        color = color * (1.0 - Fc) + ambientSpecular(i, sc, Rc, planarLayer) * Fc * specOcc;
    }
    color += s.emission * frame.exposure.x;
    return color;
}
