// Test material of the lighting validation scenes: clear window glass (transparent pass).
//   params[0] = tint.rgb, roughness
//   params[1] = x ior, y transmission, z coverage (alpha)
void surface(in SurfaceInput i, inout Surface s) {
    s.albedo = i.matParams[0].rgb;
    s.roughness = i.matParams[0].a;
    s.ior = i.matParams[1].x;
    s.transmission = i.matParams[1].y;
    s.alpha = i.matParams[1].z;
    s.specular = 0.5;
}
