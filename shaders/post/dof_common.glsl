// Thin-lens circle of confusion. Returns the signed CoC radius in render-resolution pixels
// (negative = in front of the focus plane). post.dof.y = 0.5 * A * f / (zf - f) / sensor * H,
// so CoC(z) = scale * (z - zf) / z (units cancel), clamped to the maximum radius.
float cocRadius(float z) {
    float zf = post.dof.x;
    return clamp(post.dof.y * (z - zf) / max(z, 1e-4), -post.dof.z, post.dof.z);
}
