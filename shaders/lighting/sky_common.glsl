// Sky radiance seen from the hall: atmosphere (sky-view LUT), sun disk with limb darkening and a
// soft procedural cloud layer. Used by the sky pass and by the sky cubemap capture.
// Requires common.glsl, lighting_ubo.glsl and atmosphere.glsl.

float cloudHash(ivec2 c) { return float(hashU(uint(c.x) * 1597334673u ^ hashU(uint(c.y) * 3812015801u))) * (1.0 / 4294967296.0); }
float cloudValueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    ivec2 c = ivec2(i);
    float a = cloudHash(c), b = cloudHash(c + ivec2(1, 0)), cc = cloudHash(c + ivec2(0, 1)), d = cloudHash(c + ivec2(1, 1));
    return mix(mix(a, b, u.x), mix(cc, d, u.x), u.y);
}
float cloudFbm(vec2 p) {
    float s = 0.0, a = 0.5;
    const mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 5; ++i) {
        s += a * cloudValueNoise(p);
        p = rot * p * 2.03 + vec2(1.7, 9.2);
        a *= 0.5;
    }
    return s / 0.96875;
}

// Thin cumulus layer ~1.5 km above the viewer. rgb = in-scattered radiance (pre-exposed),
// a = transmittance of the layer along d. sunIllum is the pre-exposed solar illuminance at the
// cloud, skyAmbient the pre-exposed average sky radiance above the layer.
vec4 cloudLayer(vec3 d, vec3 sunDir, vec3 sunIllum, vec3 skyAmbient, float coverage, float time) {
    if (coverage <= 0.001 || d.y <= 0.015) return vec4(0.0, 0.0, 0.0, 1.0);
    const float H = 1.5;
    float t = H / d.y;
    vec2 q = d.xz * t * 0.32 + vec2(0.009, 0.0035) * time;
    float lo = 1.0 - coverage, hi = lo + 0.38;
    float dens = smoothstep(lo, hi, cloudFbm(q));
    if (dens <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    vec2 sunOff = sunDir.xz / max(sunDir.y, 0.2) * 0.09;
    float densSun = smoothstep(lo, hi, cloudFbm(q + sunOff));
    float od = dens * 1.1 / max(d.y, 0.06);
    float odS = (densSun * 0.75 + dens * 0.25) * 5.0;
    float Tv = exp(-od);
    float mu = dot(d, sunDir);
    // Multiple scattering octaves (Wrenninge 2013).
    float ms = 0.0, a = 1.0, b = 1.0, c = 1.0;
    for (int i = 0; i < 3; ++i) {
        ms += a * mix(henyeyGreenstein(mu, 0.6 * c), henyeyGreenstein(mu, -0.2 * c), 0.3) * exp(-odS * b);
        a *= 0.55;
        b *= 0.45;
        c *= 0.5;
    }
    float powder = 1.0 - 0.6 * exp(-od * 2.5);
    vec3 L = (sunIllum * ms * powder * 2.4 + skyAmbient * (0.75 + 0.25 * dens)) * (1.0 - Tv);
    float fade = exp(-t / 28.0);
    return vec4(L * fade, mix(1.0, Tv, fade));
}

float skyViewerRadius() { return ATMO_BOTTOM + lighting.sunTOA.w; }

vec3 skyViewLookup(sampler2D skyView, vec3 d, vec3 sunDir) {
    vec2 dh = d.xz, sh = sunDir.xz;
    float ld = length(dh), ls = length(sh);
    float lvc = (ld > 1e-5 && ls > 1e-5) ? dot(dh / ld, sh / ls) : 1.0;
    return textureLod(skyView, skyViewUv(skyViewerRadius(), d.y, lvc), 0.0).rgb;
}

// Pre-exposed sky radiance along world direction d.
vec3 skyRadiance(sampler2D tLut, sampler2D skyView, vec3 d, bool sunDisk, bool clouds) {
    vec3 sunDir = frame.sunDirection.xyz;
    vec3 E = lighting.sunTOA.rgb * lighting.skyParams2.w;
    vec3 c = skyViewLookup(skyView, d, sunDir) * E;
    vec3 sun = vec3(0.0);
    if (sunDisk) {
        float cosA = dot(d, sunDir);
        float th = frame.sunDirection.w;
        float cosR = cos(th);
        if (cosA > cosR) {
            float r = skyViewerRadius();
            vec3 T = atmoTransmittance(tLut, r, d.y);
            if (raySphere(vec3(0.0, r, 0.0), d, ATMO_BOTTOM) > 0.0) T = vec3(0.0);
            // Limb darkening I(mu) = 1 - u (1 - mu), normalised to the disk average 1 - u/3.
            float x = clamp(acos(clamp(cosA, -1.0, 1.0)) / th, 0.0, 1.0);
            float muDisk = sqrt(max(1.0 - x * x, 0.0));
            vec3 u = vec3(0.52, 0.60, 0.70);
            vec3 limb = (1.0 - u * (1.0 - muDisk)) / (1.0 - u / 3.0);
            sun = lighting.sunTOA.rgb * T * limb / (PI * th * th);
        }
    }
    if (clouds) {
        vec3 amb = skyViewLookup(skyView, normalize(vec3(0.3, 1.0, 0.2)), sunDir) * E;
        vec4 cl = cloudLayer(d, sunDir, frame.sunRadiance.rgb, amb, lighting.skyParams2.x, lighting.skyParams2.y);
        c = c * cl.a + cl.rgb;
        sun *= cl.a;
    }
    return c + sun;
}
