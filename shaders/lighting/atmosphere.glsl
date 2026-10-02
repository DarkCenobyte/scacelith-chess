// Physically based atmosphere after Hillaire 2020, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique" (transmittance LUT, multiple-scattering LUT, sky-view LUT).
// Units are kilometres; the planet centre is the origin and +Y is up at the viewer. Radiance in
// the LUTs is expressed per unit of top-of-atmosphere solar illuminance (multiply by sunTOA).
// Keep the constants in sync with src/render/lighting/atmosphere.cpp (CPU sun colour).

const float ATMO_BOTTOM = 6360.0;
const float ATMO_TOP = 6460.0;
const vec3 ATMO_RAYLEIGH = vec3(5.802, 13.558, 33.1) * 1e-3;
const float ATMO_RAYLEIGH_H = 8.0;
const float ATMO_MIE_SCAT = 3.996e-3;
const float ATMO_MIE_EXT = 4.440e-3;
const float ATMO_MIE_H = 1.2;
const float ATMO_MIE_G = 0.8;
const vec3 ATMO_OZONE = vec3(0.650, 1.881, 0.085) * 1e-3;
const vec3 ATMO_GROUND_ALBEDO = vec3(0.16, 0.17, 0.12);

const vec2 TRANSMITTANCE_LUT_SIZE = vec2(256.0, 64.0);
const vec2 MULTISCAT_LUT_SIZE = vec2(32.0, 32.0);
const vec2 SKYVIEW_LUT_SIZE = vec2(192.0, 108.0);

struct AtmoMedium {
    vec3 scattering;
    vec3 extinction;
    vec3 scatRayleigh;
    float scatMie;
};

AtmoMedium atmoMedium(float h, float mieScale) {
    float dR = exp(-h / ATMO_RAYLEIGH_H);
    float dM = exp(-h / ATMO_MIE_H) * mieScale;
    float dO = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    AtmoMedium m;
    m.scatRayleigh = ATMO_RAYLEIGH * dR;
    m.scatMie = ATMO_MIE_SCAT * dM;
    m.scattering = m.scatRayleigh + vec3(m.scatMie);
    m.extinction = m.scatRayleigh + vec3(ATMO_MIE_EXT * dM) + ATMO_OZONE * dO;
    return m;
}

// Nearest positive intersection with a sphere centred on the origin, -1 when missed.
float raySphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    if (t0 > 0.0) return t0;
    if (t1 > 0.0) return t1;
    return -1.0;
}

float rayleighPhase(float c) { return 3.0 / (16.0 * PI) * (1.0 + c * c); }
float cornetteShanks(float c, float g) {
    float g2 = g * g;
    float k = 3.0 / (8.0 * PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + c * c) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-5), 1.5);
}
float henyeyGreenstein(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-5), 1.5));
}

vec2 atmoSubUv(vec2 uv, vec2 size) { return (uv * (size - 1.0) + 0.5) / size; }
vec2 atmoUnSubUv(vec2 uv, vec2 size) { return (uv * size - 0.5) / (size - 1.0); }

// Bruneton's transmittance parametrisation.
vec2 transmittanceUv(float r, float mu) {
    float H = sqrt(ATMO_TOP * ATMO_TOP - ATMO_BOTTOM * ATMO_BOTTOM);
    float rho = sqrt(max(r * r - ATMO_BOTTOM * ATMO_BOTTOM, 0.0));
    float disc = r * r * (mu * mu - 1.0) + ATMO_TOP * ATMO_TOP;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float dMin = ATMO_TOP - r, dMax = rho + H;
    return vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / H);
}
void transmittanceParams(vec2 uv, out float r, out float mu) {
    float H = sqrt(ATMO_TOP * ATMO_TOP - ATMO_BOTTOM * ATMO_BOTTOM);
    float rho = H * uv.y;
    r = sqrt(rho * rho + ATMO_BOTTOM * ATMO_BOTTOM);
    float dMin = ATMO_TOP - r, dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

vec3 atmoTransmittance(sampler2D lut, float r, float mu) {
    vec2 uv = atmoSubUv(transmittanceUv(r, mu), TRANSMITTANCE_LUT_SIZE);
    return textureLod(lut, uv, 0.0).rgb;
}

vec3 atmoMultiScattering(sampler2D lut, float r, float muS) {
    vec2 uv = vec2(muS * 0.5 + 0.5, (r - ATMO_BOTTOM) / (ATMO_TOP - ATMO_BOTTOM));
    return textureLod(lut, atmoSubUv(clamp(uv, 0.0, 1.0), MULTISCAT_LUT_SIZE), 0.0).rgb;
}

// Sky-view LUT parametrisation around the viewer (lat-long, non-linear towards the horizon,
// azimuth relative to the sun: u = 0 faces the sun).
vec2 skyViewUv(float r, float viewZenithCos, float lightViewCos) {
    float vHorizon = sqrt(max(r * r - ATMO_BOTTOM * ATMO_BOTTOM, 0.0));
    float beta = acos(clamp(vHorizon / r, -1.0, 1.0));
    float zha = PI - beta;
    float angle = acos(clamp(viewZenithCos, -1.0, 1.0));
    float v;
    if (angle < zha) {
        float c = 1.0 - sqrt(max(1.0 - angle / zha, 0.0));
        v = c * 0.5;
    } else {
        float c = sqrt(clamp((angle - zha) / beta, 0.0, 1.0));
        v = c * 0.5 + 0.5;
    }
    float u = sqrt(clamp(-lightViewCos * 0.5 + 0.5, 0.0, 1.0));
    return atmoSubUv(vec2(u, v), SKYVIEW_LUT_SIZE);
}
void skyViewParams(vec2 uvTex, float r, out float viewZenithCos, out float lightViewCos) {
    vec2 uv = clamp(atmoUnSubUv(uvTex, SKYVIEW_LUT_SIZE), 0.0, 1.0);
    float vHorizon = sqrt(max(r * r - ATMO_BOTTOM * ATMO_BOTTOM, 0.0));
    float beta = acos(clamp(vHorizon / r, -1.0, 1.0));
    float zha = PI - beta;
    float angle;
    if (uv.y < 0.5) {
        float c = 1.0 - 2.0 * uv.y;
        c = 1.0 - c * c;
        angle = zha * c;
    } else {
        float c = uv.y * 2.0 - 1.0;
        angle = zha + beta * c * c;
    }
    viewZenithCos = cos(angle);
    float c = uv.x * uv.x;
    lightViewCos = -(c * 2.0 - 1.0);
}

// Single + multiple scattering along a ray (analytic per-step integration). Returns in-scattered
// luminance per unit solar illuminance; 'throughput' receives the ray transmittance.
vec3 atmoIntegrate(vec3 ro, vec3 rd, vec3 sunDir, int steps, float mieScale, bool uniformPhase, bool addGround,
                   sampler2D tLut, sampler2D msLut, bool useMs, out vec3 throughput, out vec3 fms) {
    throughput = vec3(1.0);
    fms = vec3(0.0);
    float tBottom = raySphere(ro, rd, ATMO_BOTTOM);
    float tTop = raySphere(ro, rd, ATMO_TOP);
    float tMax;
    if (tBottom < 0.0) {
        if (tTop < 0.0) return vec3(0.0);
        tMax = tTop;
    } else {
        tMax = tTop > 0.0 ? min(tTop, tBottom) : tBottom;
    }
    float mu = dot(rd, sunDir);
    float phaseR = uniformPhase ? 1.0 / (4.0 * PI) : rayleighPhase(mu);
    float phaseM = uniformPhase ? 1.0 / (4.0 * PI) : cornetteShanks(mu, ATMO_MIE_G);
    vec3 L = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        float s0 = float(i) / float(steps), s1 = float(i + 1) / float(steps);
        float t0 = tMax * s0 * s0, t1 = tMax * s1 * s1;
        float t = mix(t0, t1, 0.3);
        float dt = t1 - t0;
        vec3 P = ro + rd * t;
        float r = length(P);
        vec3 up = P / r;
        AtmoMedium m = atmoMedium(r - ATMO_BOTTOM, mieScale);
        vec3 sampleT = exp(-m.extinction * dt);
        float muS = dot(sunDir, up);
        vec3 tSun = atmoTransmittance(tLut, r, muS);
        float earth = raySphere(P, sunDir, ATMO_BOTTOM) > 0.0 ? 0.0 : 1.0;
        vec3 phaseScat = m.scatRayleigh * phaseR + vec3(m.scatMie * phaseM);
        vec3 ms = useMs ? atmoMultiScattering(msLut, r, muS) : vec3(0.0);
        vec3 S = earth * tSun * phaseScat + ms * m.scattering;
        vec3 ext = max(m.extinction, vec3(1e-7));
        L += throughput * (S - S * sampleT) / ext;
        fms += throughput * (m.scattering - m.scattering * sampleT) / ext;
        throughput *= sampleT;
    }
    if (addGround && tBottom > 0.0 && (tTop < 0.0 || tBottom <= tTop + 1e-3)) {
        vec3 P = ro + rd * tBottom;
        vec3 up = normalize(P);
        float NoL = clamp(dot(up, sunDir), 0.0, 1.0);
        vec3 tSun = atmoTransmittance(tLut, length(P), dot(sunDir, up));
        // Sun + a crude sky-irradiance term (keeps the distant ground from going black at dusk).
        L += throughput * ATMO_GROUND_ALBEDO * INV_PI * (tSun * NoL + vec3(0.02, 0.03, 0.05) * clamp(dot(sunDir, up) + 0.2, 0.0, 1.0));
    }
    return L;
}
