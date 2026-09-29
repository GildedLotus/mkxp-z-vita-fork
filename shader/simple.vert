
uniform mat4 projMat;

uniform vec2 texSizeInv;
uniform vec2 translation;

attribute vec2 position;
attribute vec2 texCoord;

varying vec2 v_texCoord;
#ifdef FINAL_PRESENTATION_PROBE
varying highp vec2 v_finalTexCoord;
#endif

void main()
{
	gl_Position = projMat * vec4(position + translation, 0, 1);

	v_texCoord = texCoord * texSizeInv;
#ifdef FINAL_PRESENTATION_PROBE
	v_finalTexCoord = texCoord * texSizeInv;
#endif
}
