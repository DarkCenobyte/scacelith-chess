// Motion blur helpers. Blur vector = half of the screen-space displacement during the exposure
// (pixels), clamped to the tile size (McGuire et al. 2012).
vec2 blurVector(vec2 velocityUV) {
    vec2 v = velocityUV * post.renderSize.xy * (0.5 * post.mb.x);
    float l = length(v);
    if (!(l < 1e6)) return vec2(0.0);  // NaN / Inf
    return l > post.mb.y ? v * (post.mb.y / l) : v;
}
