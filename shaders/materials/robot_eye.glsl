// Robot eyes: one file, three variants (defines EYE_SCLERA, EYE_IRIS, EYE_CORNEA).
// Geometry is in eye-bone space: origin = eyeball centre, +Z = gaze direction.
//
// matParams:
//   [0] eyeball radius, cornea radius, cornea centre z, limbus radius
//   [1] iris plane z, cornea IOR, pupil radius min, pupil radius max
//   [2] iris outer (ciliary) colour
//   [3] iris inner (collarette) colour
//   [4] sclera colour
//   [5] vessel colour
// instParams[0].x = pupil dilation in [0,1] (iris only; set per frame by submitRobot).
//
// The iris mesh IS the cornea cap: the shader refracts the view ray at the cornea surface and
// traces it to the iris plane (depth + parallax for free, exact at every angle). The transparent
// cornea layer on top only adds the wet highlight / reflections and a faint limbal haze.

vec3 eyeToOS(vec3 vWS) { return normalize(transpose(mat3(draws[uDraw].model)) * vWS); }
vec3 eyeToWS(vec3 vOS) { return normalize(mat3(draws[uDraw].model) * vOS); }

// Iris colour and height at iris-plane point q (eye space), pupil radius pr.
vec3 irisAlbedo(vec2 q, float pr, float limbusR, vec3 outerC, vec3 innerC, float seed, out float isPupil, out float height) {
    float rho = length(q);
    float r = rho / limbusR;                       // 0 centre .. 1 limbus
    vec2 dir = q / max(rho, 1e-6);
    float pn = pr / limbusR;
    // Iris stroma stretches radially with the pupil: map r to a fixed-pattern coordinate.
    float t = clamp((r - pn) / max(1.0 - pn, 1e-3), 0.0, 1.0);
    // Radial fibres: high angular frequency, low radial frequency.
    vec3 fp = vec3(dir * 26.0, t * 2.2 + seed * 7.0);
    float fib = gnoise(fp) * 0.55 + gnoise(fp * vec3(2.1, 2.1, 1.3) + 3.1) * 0.30 + gnoise(fp * vec3(4.3, 4.3, 1.1) + 7.7) * 0.15;
    float fine = gnoise(vec3(dir * 70.0, t * 4.0 + seed));
    // Collarette: the zig-zag ring between the pupillary and ciliary zones.
    float coll = 0.34 + 0.035 * gnoise(vec3(dir * 9.0, seed * 3.0));
    float inner = 1.0 - smoothstep(coll - 0.05, coll + 0.04, t);
    vec3 c = mix(outerC, innerC, inner * 0.85);
    c *= 0.72 + 0.55 * fib + 0.12 * fine;
    // Crypts (dark lacunae) around the collarette and in the ciliary zone.
    float crypt = smoothstep(0.35, 0.62, gnoise(vec3(dir * 11.0, t * 5.0 + seed * 11.0)));
    crypt *= smoothstep(0.18, 0.40, t) * (1.0 - smoothstep(0.80, 0.95, t));
    c *= 1.0 - 0.45 * crypt;
    // Contraction furrows (faint concentric rings) in the outer iris.
    c *= 1.0 - 0.10 * smoothstep(0.55, 1.0, t) * (0.5 + 0.5 * sin(t * 38.0 + fib * 2.0));
    // Limbal ring: dark rim where the iris meets the sclera.
    c *= mix(1.0, 0.28, smoothstep(0.80, 0.99, r));
    // Pupillary ruff: thin dark-brown rim around the pupil.
    float ruff = 1.0 - smoothstep(pn, pn + 0.06, r);
    c = mix(c, vec3(0.05, 0.03, 0.02), ruff * 0.8);
    isPupil = 1.0 - smoothstep(pn - 0.012, pn + 0.004, r);
    height = fib * 0.5 + inner * 0.5 - crypt;
    return c;
}

void surface(in SurfaceInput i, inout Surface s) {
    vec3 p = i.positionOS;
    float R = i.matParams[0].x, cR = i.matParams[0].y, cz = i.matParams[0].z, limbusR = i.matParams[0].w;
    float seed = i.objectSeed;

#if defined(EYE_IRIS)
    vec3 v = eyeToOS(i.viewDirWS);                 // towards the camera
    vec3 n = normalize(p - vec3(0.0, 0.0, cz));    // cornea normal
    vec3 tr = refract(-v, n, 1.0 / i.matParams[1].y);
    float irisZ = i.matParams[1].x;
    float tt = (irisZ - p.z) / min(tr.z, -1e-3);
    vec3 q = p + tr * tt;
    float dil = clamp(i.instParams[0].x, 0.0, 1.0);
    float pr = mix(i.matParams[1].z, i.matParams[1].w, dil);
    float isPupil, h;
    vec3 c = irisAlbedo(q.xy, pr, limbusR, i.matParams[2].rgb, i.matParams[3].rgb, seed, isPupil, h);
    float rho = length(q.xy);
    // Beyond the limbus (grazing views) the ray reaches the sclera behind the cornea edge.
    c = mix(c, i.matParams[4].rgb * 0.55, smoothstep(limbusR * 0.99, limbusR * 1.04, rho));
    c = mix(c, vec3(0.004), isPupil);
    s.albedo = c;
    s.roughness = mix(0.55, 0.3, isPupil);
    s.specular = 0.35;
    // Light reaches the iris through the cornea: shade with the iris plane normal, gently bumped
    // by the fibres, so the iris is lit like a textured disc, not like the cornea bulge.
    vec3 nIris = vec3(0.0, 0.0, 1.0);
    s.normalWS = eyeToWS(nIris);
    s.clearcoatNormalWS = i.normalWS;
    s.clearcoat = 0.0;
    // Dim with depth in the socket: the lids shade the top of the iris.
    s.occlusion = mix(0.75, 1.0, smoothstep(-0.8, 0.2, -q.y / limbusR));
    s.subsurface = 0.0;
#elif defined(EYE_SCLERA)
    // Wet sclera: warm off-white, bluish-grey near the cornea, faint vessels towards the corners.
    vec3 dirE = normalize(p);
    float fromAxis = acos(clamp(dirE.z, -1.0, 1.0));              // 0 at the cornea centre
    float limbusAngle = asin(clamp(limbusR / R, 0.0, 1.0));
    float away = smoothstep(limbusAngle + 0.05, limbusAngle + 1.1, fromAxis);
    vec3 c = i.matParams[4].rgb * (0.97 + 0.05 * fbm(p * 900.0 + seed, 3));
    c = mix(c * vec3(0.90, 0.93, 0.98), c, smoothstep(limbusAngle, limbusAngle + 0.25, fromAxis));
    // Vessels: thin ridged-noise lines, stronger towards the eye corners (left / right).
    float side = smoothstep(0.2, 0.9, abs(dirE.x));
    float ves = 1.0 - smoothstep(0.0, 0.06, abs(gnoise(p * 520.0 + vec3(seed * 9.0)) + 0.35 * gnoise(p * 1300.0)));
    float vesFine = 1.0 - smoothstep(0.0, 0.04, abs(gnoise(p * 1100.0 + 5.0)));
    float vesMask = away * (0.35 + 0.65 * side);
    c = mix(c, i.matParams[5].rgb, clamp(ves * 0.22 + vesFine * 0.10, 0.0, 1.0) * vesMask);
    // Slightly warmer and darker towards the back (only visible at the lid corners).
    c *= mix(1.0, 0.82, smoothstep(0.9, 1.6, fromAxis));
    s.albedo = c;
    s.roughness = 0.38;
    s.specular = 0.5;
    s.clearcoat = 1.0;                  // tear film
    s.clearcoatRoughness = 0.06;
    s.clearcoatNormalWS = i.normalWS;
    s.subsurface = 0.35;
    s.subsurfaceColor = vec3(1.0, 0.82, 0.76);
    s.subsurfaceRadius = 0.002;
    // Shadowing by the lids: the upper and lower parts of the globe sit under the lid shells.
    s.occlusion = mix(1.0, 0.45, smoothstep(0.25, 0.75, abs(dirE.y)));
#elif defined(EYE_CORNEA)
    // Clear cornea: almost fully transparent, but its reflection must stay at full strength. The
    // forward pass outputs premultiplied (colour * alpha): with alpha a, F0 is raised by 1/a so the
    // visible reflectance is that of the cornea (n = 1.376 -> F0 ~ 0.025).
    vec3 n = normalize(p - vec3(0.0, 0.0, cz));
    float rho = length(p.xy) / limbusR;
    float haze = smoothstep(0.82, 1.0, rho);      // faint grey limbal haze at the cornea edge
    float a = mix(0.10, 0.22, haze);
    s.alpha = a;
    s.albedo = vec3(0.30, 0.31, 0.33) * haze * 0.25 / a;
    s.roughness = 0.02;
    s.specular = sqrt(0.025 / a / 0.16);
    s.normalWS = eyeToWS(n);
    s.clearcoat = 0.0;
    s.subsurface = 0.0;
    s.occlusion = 1.0;
#else
    s.albedo = i.matParams[4].rgb;
    s.roughness = 0.3;
#endif
}
