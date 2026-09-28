// UI quads (src/ui/ui_draw.cpp). Positions are physical pixels with a top-left origin.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;   // straight alpha, display (sRGB) values
layout(location = 3) in vec4 aP0;
layout(location = 4) in vec4 aP1;

uniform vec2 uViewport;

out vec2 vUV;
out vec4 vColor;
flat out vec4 vP0;
flat out vec4 vP1;

void main() {
    vec2 ndc = aPos / uViewport * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
    vP0 = aP0;
    vP1 = aP1;
}
