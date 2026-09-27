// BRDF building blocks (Filament-style conventions).

float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * f * f + 1e-7);
}
float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-7);
}
vec3 F_Schlick(vec3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}
float F_Schlick(float f0, float f90, float VoH) { return f0 + (f90 - f0) * pow(1.0 - VoH, 5.0); }
float Fd_Burley(float NoV, float NoL, float LoH, float roughness) {
    float f90 = 0.5 + 2.0 * roughness * LoH * LoH;
    return F_Schlick(1.0, f90, NoL) * F_Schlick(1.0, f90, NoV) * INV_PI;
}
// Kelemen visibility for the clear coat layer.
float V_Kelemen(float LoH) { return 0.25 / max(LoH * LoH, 1e-4); }
// Charlie sheen distribution + Neubelt visibility (cloth).
float D_Charlie(float roughness, float NoH) {
    float invAlpha = 1.0 / max(roughness * roughness, 1e-4);
    float cos2h = NoH * NoH;
    float sin2h = max(1.0 - cos2h, 0.0078125);
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * PI);
}
float V_Neubelt(float NoV, float NoL) { return 1.0 / (4.0 * (NoL + NoV - NoL * NoV) + 1e-5); }
// Anisotropic GGX.
float D_GGX_Aniso(float at, float ab, float ToH, float BoH, float NoH) {
    float a2 = at * ab;
    vec3 d = vec3(ab * ToH, at * BoH, a2 * NoH);
    float d2 = dot(d, d);
    float b2 = a2 / max(d2, 1e-7);
    return a2 * b2 * b2 * INV_PI;
}
float V_SmithGGXCorrelated_Aniso(float at, float ab, float ToV, float BoV, float ToL, float BoL, float NoV, float NoL) {
    float lv = NoL * length(vec3(at * ToV, ab * BoV, NoV));
    float ll = NoV * length(vec3(at * ToL, ab * BoL, NoL));
    return 0.5 / max(lv + ll, 1e-7);
}
// Analytic approximation of the split-sum DFG term (Karis mobile), used until a LUT is bound.
vec2 envBRDFApprox(float NoV, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}
