
uniform sampler2D texture;

varying vec2 v_texCoord;
#ifdef FINAL_PRESENTATION_PROBE
varying highp vec2 v_finalTexCoord;
#endif

#ifdef FINAL_PRESENTATION_PROBE
uniform int finalPresentationHighp;
uniform highp vec2 finalOrigin;
uniform highp vec2 finalExtent;
uniform highp vec2 finalTexture;
uniform highp vec2 finalInvM;
uniform highp vec2 finalInvT;
#endif

void main()
{
#ifdef FINAL_PRESENTATION_PROBE
	if (finalPresentationHighp >= 2) {
		highp vec2 i = floor(gl_FragCoord.xy) - finalOrigin;
		highp vec2 M = 2.0 * finalExtent;
		highp vec2 N = vec2((2.0*i.x+1.0)*finalTexture.x,
		                    (2.0*finalExtent.y-2.0*i.y-1.0)*finalTexture.y);
		// Opaque RGB sentinel; capture analysis must reject any matching pixel.
		if (!(all(greaterThanEqual(i, vec2(0.0))) && all(lessThan(i, finalExtent)) &&
		      all(greaterThan(N, vec2(0.0))) && all(lessThan(N, M*finalTexture)))) {
			gl_FragColor = vec4(1.0, 0.0, 253.0/255.0, 1.0);
			return;
		}
		highp vec2 uv = N * finalInvM * finalInvT;
		if (finalPresentationHighp == 2) {
			highp vec2 q = floor(N * finalInvM);
			if (q.x*M.x > N.x) q.x -= 1.0;
			else if ((q.x+1.0)*M.x <= N.x) q.x += 1.0;
			if (q.y*M.y > N.y) q.y -= 1.0;
			else if ((q.y+1.0)*M.y <= N.y) q.y += 1.0;
			if (!(all(greaterThanEqual(q, vec2(0.0))) && all(lessThan(q, finalTexture)) &&
			      all(lessThanEqual(q*M, N)) && all(lessThan(N, (q+1.0)*M)))) {
				gl_FragColor = vec4(1.0, 0.0, 253.0/255.0, 1.0);
				return;
			}
			uv = (q+0.5) * finalInvT;
		}
		gl_FragColor = texture2D(texture, uv);
	} else if (finalPresentationHighp == 1)
		gl_FragColor = texture2D(texture, v_finalTexCoord);
	else
#endif
	gl_FragColor = texture2D(texture, v_texCoord);
}
