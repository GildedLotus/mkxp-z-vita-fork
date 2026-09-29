/* Movie frames (THEORAPLAY_VIDFMT_YUVTEX): one RGBA texture holds the Y
 * rows, then rows of Cb and Cr side by side, four samples a texel.
 * v_texCoord is in frame pixels (texSizeInv 1). BT.601 video range to RGB
 * with theoraplay's constants. */

uniform sampler2D texture;

/* 1/texture width, 1/texture height, first chroma row, first Cr texel. */
uniform highp vec4 planeInfo;
/* Last chroma column and row: odd sizes clamp, as the decoder copies. */
uniform highp vec2 chromaMax;

varying vec2 v_texCoord;

const vec4 lanes = vec4(0.0, 1.0, 2.0, 3.0);

void main()
{
	/* Luma: texel p.x / 4 of row p.y, lane p.x mod 4. */
	highp vec2 p = floor(v_texCoord);
	highp float lumaCol = floor(p.x * 0.25);
	highp vec4 lane = vec4(p.x - lumaCol * 4.0);
	vec4 pick = step(lanes - 0.5, lane) - step(lanes + 0.5, lane);
	float y = dot(texture2D(texture, (vec2(lumaCol, p.y) + 0.5) * planeInfo.xy), pick);

	/* Chroma: the same at half resolution, Cb then Cr. */
	highp vec2 c = min(floor(p * 0.5), chromaMax);
	highp float ccol = floor(c.x * 0.25);
	lane = vec4(c.x - ccol * 4.0);
	pick = step(lanes - 0.5, lane) - step(lanes + 0.5, lane);
	highp float chromaRow = (planeInfo.z + c.y + 0.5) * planeInfo.y;
	float u = dot(texture2D(texture, vec2((ccol + 0.5) * planeInfo.x, chromaRow)), pick);
	float v = dot(texture2D(texture, vec2((planeInfo.w + ccol + 0.5) * planeInfo.x, chromaRow)), pick);

	gl_FragColor = vec4(vec3(1.164384 * y) + vec3(-0.874202, 0.531668, -1.085631) +
	                    u * vec3(0.0, -0.391762, 2.017232) +
	                    v * vec3(1.596027, -0.812968, 0.0), 1.0);
}
