// Scoresheet page fields (game/scoresheet.cpp): quads of the printed form (rules, labels) and of
// the handwritten glyphs, drawn into a page layer (or, with ENTRY, into the texture of the entry
// being written). Positions are page millimetres; the target covers page x in [0, width / density]
// and y from uOrigin.y down.
layout(location = 0) in vec4 aPosUv;  // xy = page mm; zw = atlas uv (glyphs) or rule-local mm
layout(location = 1) in vec4 aP0;     // x = mode (0 handwriting, 1 print, 2 rule), y = page mm per unit of the
                                      // atlas value (glyph pxRange / atlas texels per mm),
                                      // z = dilation (mm), w = pressure
layout(location = 2) in vec4 aP1;     // rules: xy = half size (mm); ENTRY glyphs: x = first key, y = last key
                                      // of the pen stroke band inking this quad, z = time the band ends (s)

uniform vec2 uOrigin;    // page mm at the target's texel (0, 0)
uniform float uDensity;  // target texels per mm
uniform vec2 uSize;      // target size (texels)

out vec2 vUv;
out vec2 vPage;
flat out vec4 vP0;
flat out vec4 vP1;

void main() {
    vec2 t = (aPosUv.xy - uOrigin) * uDensity;
    // Texel row 0 = page top (texture v = 0): NDC y = -1 is window row 0.
    gl_Position = vec4(t / uSize * 2.0 - 1.0, 0.0, 1.0);
    vUv = aPosUv.zw;
    vPage = aPosUv.xy;
    vP0 = aP0;
    vP1 = aP1;
}
