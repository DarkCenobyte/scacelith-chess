// Robot shells: glazed white porcelain / composite (RobotPorcelain) and eyelids (RobotLid).
//
// matParams:
//   [0] glaze body albedo rgb, base roughness
//   [1] clearcoat, clearcoat roughness, specular, subsurface
//   [2] subsurface tint rgb, orange-peel strength
//   [3] soft-touch pad albedo rgb, pad roughness
//   [4] x = seam darkening, y = seam bevel width (m), z = smudge amount, w = albedo variation
// instParams (per part, see src/character/robot_build.h):
//   [0] x = variant (0 shell, 1 pad region on the positive side of seam 0), y = clearcoat scale,
//       z = roughness offset, w = seam half width (m)
//   [1..3] seam planes in bone space: xyz unit normal, w offset (dot(n,p) + w = 0), 0 = unused
// Positions are bone-local (positionOS), so every pattern is glued to the part while it moves.

// Distance (m, along the surface) from the seam where a plane cuts the shell.
float robotSeamDistance(vec4 pl, vec3 p, vec3 nOS, out vec3 dirOS) {
    float d = dot(pl.xyz, p) + pl.w;
    vec3 tg = pl.xyz - nOS * dot(nOS, pl.xyz);
    float l = max(length(tg), 0.2);
    dirOS = tg / l;
    return d / l;
}

void surface(in SurfaceInput i, inout Surface s) {
    vec3 p = i.positionOS;
    vec3 nOS = normalize(i.normalOS);
    vec4 look = i.instParams[0];
    float seamHalf = look.w > 0.0 ? look.w : 0.00018;
    float bevel = i.matParams[4].y;
    mat3 toWS = mat3(draws[uDraw].model);

    // ---- seams: dark hairline gaps with rolled edges that catch the light ---------------------
    float gap = 0.0, near = 0.0, padSide = -1.0;
    vec3 tilt = vec3(0.0);
    for (int k = 1; k < 4; ++k) {
        vec4 pl = i.instParams[k];
        if (dot(pl.xyz, pl.xyz) < 0.25) continue;
        vec3 dirOS;
        float sd = robotSeamDistance(pl, p, nOS, dirOS);
        if (k == 1) padSide = sd;
        float aa = max(fwidth(sd), 1e-6);
        float a = abs(sd);
        // Sub-pixel seams fade to a faint line instead of aliasing.
        float coverage = clamp(seamHalf / aa, 0.0, 1.0);
        float g = 1.0 - smoothstep(seamHalf - aa, seamHalf + aa, a);
        gap = max(gap, g * mix(0.35, 1.0, coverage));
        near = max(near, 1.0 - smoothstep(seamHalf, seamHalf + bevel * 3.0, a));
        // Rounded edge on both sides of the gap: the normal rolls towards the gap.
        float e = clamp((a - seamHalf) / bevel, 0.0, 1.0);
        float roll = (1.0 - e) * (1.0 - e) * step(seamHalf, a) * coverage;
        tilt += -sign(sd) * dirOS * roll * 1.4;
    }

#ifdef ROBOT_LID
    // Eyelid margin: the rounded edge beyond the margin plane (seam 0) is a dark satin gasket, so
    // the lids read as a crisp line around the eye.
    float lidEdge = 0.0;
    {
        vec4 pl = i.instParams[1];
        float m = dot(pl.xyz, p) + pl.w;
        lidEdge = 1.0 - smoothstep(0.00035, 0.0009, m);
        gap = 0.0;
        tilt = vec3(0.0);
    }
#endif
    // Face variant: closed-mouth line (a soft crease drawn analytically, crisp at any distance).
    if (look.x > 1.5) {
        float mx = p.x / 0.0185;
        float my = p.y - (0.0195 - 0.0006 * mx * mx);
        float fade = (1.0 - smoothstep(0.70, 1.0, abs(mx))) * step(0.055, p.z);
        float aa = max(fwidth(my), 1e-6);
        float line = (1.0 - smoothstep(0.00012, 0.00012 + aa * 1.5, abs(my))) * fade;
        float lip = (1.0 - smoothstep(0.0, 0.0012, abs(my))) * fade;
        gap = max(gap, line * 0.55);
        near = max(near, lip * 0.6);
        tilt += vec3(0.0, -sign(my), 0.0) * lip * (1.0 - smoothstep(0.0001, 0.0012, abs(my))) * 0.5;
    }

    // ---- base glaze ----------------------------------------------------------------------------
    float seed = i.objectSeed;
    float lowF = fbm(p * 11.0 + seed * 17.0, 3);
    float midF = gnoise(p * 70.0 + seed * 5.0);
    vec3 albedo = i.matParams[0].rgb * (1.0 + i.matParams[4].w * lowF);
    float rough = clamp(i.matParams[0].a + look.z + 0.04 * midF, 0.05, 1.0);
    float coat = i.matParams[1].x * (look.y > 0.0 ? look.y : 1.0);
    float coatRough = i.matParams[1].y;
    // Faint handling smudges: sparse blobs where the glaze is slightly hazier.
    float smudge = smoothstep(0.25, 0.75, fbm(p * 23.0 + vec3(seed * 31.0, 0.0, 0.0), 3) * 0.5 + 0.5);
    coatRough = mix(coatRough, coatRough * 3.0 + 0.04, smudge * i.matParams[4].z);

    // Soft-touch pads (finger pads, palm): matte silicone-like, no glaze.
    if (abs(look.x - 1.0) < 0.5 && padSide > 0.0) {
        float fade = smoothstep(0.0, 0.0006, padSide);
        albedo = mix(albedo, i.matParams[3].rgb * (1.0 + 0.03 * lowF), fade);
        rough = mix(rough, i.matParams[3].a + 0.05 * midF, fade);
        coat *= 1.0 - fade;
    }

    // Micro surface: orange peel of the glaze (very low amplitude).
    vec3 nGeom = i.normalWS;
    float peel = gnoise(p * 900.0) * 0.6 + gnoise(p * 2300.0) * 0.4;
    vec3 nCoat = bumpFromHeight(i.positionWS, nGeom, peel * 0.00002, i.matParams[2].a);
    vec3 nBase = nGeom;
    if (dot(tilt, tilt) > 0.0) {
        vec3 tws = toWS * tilt;
        nBase = normalize(nBase + tws);
        nCoat = normalize(nCoat + tws);
    }

#ifdef ROBOT_LID
    albedo = mix(albedo, vec3(0.075, 0.068, 0.066), lidEdge);
    rough = mix(rough, 0.62, lidEdge);
    coat *= 1.0 - lidEdge;
#endif
    s.albedo = albedo * mix(1.0, 0.08, gap) * (1.0 - 0.06 * i.matParams[4].x * near);
    s.roughness = mix(rough, 0.6, gap);
    s.specular = i.matParams[1].z;
    s.clearcoat = coat * (1.0 - gap);
    s.clearcoatRoughness = coatRough;
    s.normalWS = nBase;
    s.clearcoatNormalWS = nCoat;
    s.occlusion = mix(1.0, 0.25, gap) * (1.0 - 0.10 * near);
    s.subsurface = i.matParams[1].w * (1.0 - gap);
    s.subsurfaceColor = i.matParams[2].rgb;
    s.subsurfaceRadius = 0.004;
}
