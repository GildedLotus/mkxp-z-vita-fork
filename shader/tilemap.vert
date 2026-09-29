uniform mat4 projMat;

uniform vec2 texSizeInv;
uniform vec2 translation;

uniform highp int aniIndex;

attribute vec2 position;
attribute vec2 texCoord;

varying vec2 v_texCoord;

const int nAutotiles = 7;
const float tileW = 32.0;
const float tileH = 32.0;
const float autotileW = 3.0*tileW;
const float autotileH = 4.0*tileW;
const float atAreaW = autotileW;
const float atAreaH = autotileH*float(nAutotiles);
const float atAniOffsetX = 3.0*tileW;
const float atAniOffsetY = tileH;

uniform lowp int atFrames[nAutotiles];

void main()
{
    vec2 tex = texCoord;
    /* Rows below the autotile area, and its bottom edge, index past
     * atFrames[nAutotiles]; clamp so every read is in range. */
    lowp int atIndex = int(min(tex.y / autotileH, float(nAutotiles - 1)));

    highp float pred = (tex.x <= atAreaW && tex.y <= atAreaH) ? 1.0 : 0.0;
    /* Float arithmetic: Cg's vertex profile has no integer divide by a
     * variable. Operands are small integers; +0.5 keeps floor() exact even
     * if the GPU divides by multiplying with a reciprocal. nFrames >= 1
     * keeps every value finite: 0 * inf would be NaN even when pred is 0. */
    highp float ani = float(aniIndex);
    highp float nFrames = max(float(atFrames[atIndex]), 1.0);
    highp float frame = ani - nFrames * floor((ani + 0.5) / nFrames);
    highp float atRow = floor((frame + 0.5) / 8.0);
    highp float atCol = frame - 8.0 * atRow;
    tex.x += atAniOffsetX * atCol * pred;
    tex.y += atAniOffsetY * atRow * pred;

    gl_Position = projMat * vec4(position + translation, 0, 1);

    v_texCoord = tex * texSizeInv;
}
