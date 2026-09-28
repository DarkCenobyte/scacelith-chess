// Debug material for the robot viewer (--wire): shaded surface with triangle edges drawn from
// barycentric coordinates stored in the uv channel.
void surface(in SurfaceInput i, inout Surface s) {
    vec3 b = vec3(i.uv, 1.0 - i.uv.x - i.uv.y);
    vec3 w = fwidth(b);
    vec3 e = smoothstep(vec3(0.0), w * 1.2, b);
    float edge = 1.0 - min(e.x, min(e.y, e.z));
    s.albedo = mix(i.matParams[0].rgb, vec3(0.02, 0.05, 0.12), edge);
    s.roughness = 0.5;
}
