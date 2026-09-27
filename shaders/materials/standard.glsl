// Generic PBR material driven by parameters.
//   params[0] = albedo.rgb, roughness
//   params[1] = metallic, specular, clearcoat, clearcoatRoughness
//   params[2] = emission.rgb (nits), subsurface
//   params[3] = sheen.rgb, sheenRoughness
void surface(in SurfaceInput i, inout Surface s) {
    s.albedo = i.matParams[0].rgb;
    s.roughness = i.matParams[0].a;
    s.metallic = i.matParams[1].x;
    s.specular = i.matParams[1].y;
    s.clearcoat = i.matParams[1].z;
    s.clearcoatRoughness = i.matParams[1].w;
    s.emission = i.matParams[2].rgb;
    s.subsurface = i.matParams[2].a;
    s.sheenColor = i.matParams[3].rgb;
    s.sheenRoughness = i.matParams[3].a;
}
