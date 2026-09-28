#pragma once
// Adapted from Qualcomm's sgsr/v1/include/glsl/sgsr1_shader_mobile.frag
// and sgsr1_shader_mobile_edge_direction.frag (UseEdgeDirection variant).
// Source: https://github.com/SnapdragonGameStudios/snapdragon-gsr
// Changes: GLES 3.1, per-eye crop bounds, alpha preservation, VR edge threshold,
// and a guarded normalization denominator. See LICENSE-SGSR.txt.
namespace xrimmersive::windowsvr {
inline constexpr const char *kSgsrVertex = R"glsl(#version 310 es
layout(location=0) in vec2 p;
layout(location=1) in vec2 t;
layout(location=0) out highp vec2 uv;
uniform vec4 sourceTransform;
void main() { uv = sourceTransform.xy + t * sourceTransform.zw; gl_Position = vec4(p, 0, 1); }
)glsl";
inline constexpr const char *kSgsrFragment = R"glsl(#version 310 es

//============================================================================================================
//
//
//                  Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

precision mediump float;
precision highp int;

////////////////////////
// USER CONFIGURATION //
////////////////////////

/*
* Operation modes:
* RGBA -> 1
* RGBY -> 3
* LERP -> 4
*/
#define OperationMode 1

#define EdgeThreshold 4.0/255.0

uniform float sgsrSharpness;

// #define UseUniformBlock

////////////////////////
////////////////////////
////////////////////////

#if defined(UseUniformBlock)
layout (set=0, binding = 0) uniform UniformBlock
{
	highp vec4 ViewportInfo[1];
};
layout(set = 0, binding = 1) uniform mediump sampler2D ps0;
#else
uniform highp vec4 ViewportInfo[1];
uniform mediump sampler2D ps0;
#endif

layout(location=0) in highp vec2 uv;
uniform highp ivec4 eyeBounds;
uniform bool fullTexture;
layout(location=0) out vec4 out_Target0;

float fastLanczos2(float x)
{
	float wA = x-4.0;
	float wB = x*wA-wA;
	wA *= wA;
	return wB*wA;
}
#if defined(UseEdgeDirection)
vec2 weightY(float dx, float dy, float c, highp vec3 data)
#else
vec2 weightY(float dx, float dy, float c, float data)
#endif
{
#if defined(UseEdgeDirection)
	highp float std = data.x;
	vec2 dir = data.yz;

	float edgeDis = ((dx*dir.y)+(dy*dir.x));
	float x = (((dx*dx)+(dy*dy))+((edgeDis*edgeDis)*((clamp(((c*c)*std),0.0,1.0)*0.7)+-1.0)));
#else
	float std = data;
	float x = ((dx*dx)+(dy* dy))* 0.55 + clamp(abs(c)*std, 0.0, 1.0);
#endif

	float w = fastLanczos2(x);
	return vec2(w, w * c);	
}

vec2 edgeDirection(vec4 left, vec4 right)
{
	vec2 dir;
	float RxLz = (right.x + (-left.z));
	float RwLy = (right.w + (-left.y));
	vec2 delta;
	delta.x = (RxLz + RwLy);
	delta.y = (RxLz + (-RwLy));
	float lengthInv = inversesqrt((delta.x * delta.x+ 3.075740e-05) + (delta.y * delta.y));
	dir.x = (delta.x * lengthInv);
	dir.y = (delta.y * lengthInv);
	return dir;
}

// Preserve the fast native gather for interior pixels. Only taps crossing
// the submitted eye rectangle need explicit texel clamping (packed stereo).
vec4 gatherEye(highp vec2 coord) {
    if (fullTexture) return textureGather(ps0, coord, 1);
    highp ivec2 p = ivec2(floor(coord * ViewportInfo[0].zw - 0.5));
    if (all(greaterThanEqual(p, eyeBounds.xy)) && all(lessThan(p, eyeBounds.zw)))
        return textureGather(ps0, coord, 1);
    return vec4(
        texelFetch(ps0, clamp(p + ivec2(0, 1), eyeBounds.xy, eyeBounds.zw), 0).g,
        texelFetch(ps0, clamp(p + ivec2(1, 1), eyeBounds.xy, eyeBounds.zw), 0).g,
        texelFetch(ps0, clamp(p + ivec2(1, 0), eyeBounds.xy, eyeBounds.zw), 0).g,
        texelFetch(ps0, clamp(p, eyeBounds.xy, eyeBounds.zw), 0).g);
}

void main()
{
	int mode = OperationMode;
	float edgeThreshold = EdgeThreshold;
	float edgeSharpness = max(sgsrSharpness, 1.0);

    highp vec2 center = clamp(uv, (vec2(eyeBounds.xy) + 0.5) * ViewportInfo[0].xy,
                                  (vec2(eyeBounds.zw) + 0.5) * ViewportInfo[0].xy);
    vec4 inputColor = textureLod(ps0, center, 0.0);
    vec4 color = inputColor;

    // Eye-local ellipse, independent of scene FFR: 38% SGSR region, 2% smooth
    // transition, 60% pure bilinear by image area. Squared-radius thresholds
    // are 4*area/pi for normalized coordinates in [-1,1]^2.
    highp vec2 eyeSize = vec2(eyeBounds.zw - eyeBounds.xy + ivec2(1));
    highp vec2 eyePosition = (uv * ViewportInfo[0].zw - vec2(eyeBounds.xy)) / eyeSize;
    highp vec2 radial = eyePosition * 2.0 - 1.0;
    highp float radiusSquared = dot(radial, radial);
    float sgsrWeight = (1.0 - smoothstep(0.4838310270, 0.5092958179, radiusSquared))
        * min(sgsrSharpness, 1.0);

    // Skip SGSR gathers/filter work entirely in the outer 60% of each eye.
    if (mode != 4 && radiusSquared < 0.5092958179)
	{
		highp vec2 imgCoord = ((uv*ViewportInfo[0].zw)+vec2(-0.5,0.5));
		highp vec2 imgCoordPixel = floor(imgCoord);
		highp vec2 coord = (imgCoordPixel*ViewportInfo[0].xy);
		vec2 pl = (imgCoord+(-imgCoordPixel));
		vec4  left = gatherEye(coord);

		float edgeVote = abs(left.z - left.y) + abs(color[mode] - left.y)  + abs(color[mode] - left.z) ;
		if(edgeVote > edgeThreshold)
		{
			coord.x += ViewportInfo[0].x;

			vec4 right = gatherEye(coord + vec2(ViewportInfo[0].x, 0.0));
			vec4 upDown;
			upDown.xy = gatherEye(coord + vec2(0.0, -ViewportInfo[0].y)).wz;
			upDown.zw  = gatherEye(coord + vec2(0.0, ViewportInfo[0].y)).yx;

			float mean = (left.y+left.z+right.x+right.w)*0.25;
			left = left - vec4(mean);
			right = right - vec4(mean);
			upDown = upDown - vec4(mean);
			color.w =color[mode] - mean;

			float sum = (((((abs(left.x)+abs(left.y))+abs(left.z))+abs(left.w))+(((abs(right.x)+abs(right.y))+abs(right.z))+abs(right.w)))+(((abs(upDown.x)+abs(upDown.y))+abs(upDown.z))+abs(upDown.w)));				
			#if defined(UseEdgeDirection)
            // Squaring the normalization can overflow mediump near the edge
            // threshold. Keep this small part of the directional math highp.
            highp float sumMean = 10.14185 / max(float(sum), 0.0001);
            highp vec3 data = vec3(sumMean * sumMean, edgeDirection(left, right));
#else
            float data = 2.181818/max(sum, 0.0001);
#endif
			
			vec2 aWY = weightY(pl.x, pl.y+1.0, upDown.x,data);				
			aWY += weightY(pl.x-1.0, pl.y+1.0, upDown.y,data);
			aWY += weightY(pl.x-1.0, pl.y-2.0, upDown.z,data);
			aWY += weightY(pl.x, pl.y-2.0, upDown.w,data);			
			aWY += weightY(pl.x+1.0, pl.y-1.0, left.x,data);
			aWY += weightY(pl.x, pl.y-1.0, left.y,data);
			aWY += weightY(pl.x, pl.y, left.z,data);
			aWY += weightY(pl.x+1.0, pl.y, left.w,data);
			aWY += weightY(pl.x-1.0, pl.y-1.0, right.x,data);
			aWY += weightY(pl.x-2.0, pl.y-1.0, right.y,data);
			aWY += weightY(pl.x-2.0, pl.y, right.z,data);
			aWY += weightY(pl.x-1.0, pl.y, right.w,data);

			float finalY = aWY.y/aWY.x;

			float maxY = max(max(left.y,left.z),max(right.x,right.w));
			float minY = min(min(left.y,left.z),min(right.x,right.w));
			finalY = clamp(edgeSharpness*finalY, minY, maxY);
					
			float deltaY = finalY -color.w;	
			
			//smooth high contrast input
			deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

			// Blend final clamped RGB, so 0.5 is a true halfway result even
            // when an SGSR channel saturates. Alpha stays unchanged.
            color.rgb = mix(inputColor.rgb, clamp(color.rgb + vec3(deltaY), 0.0, 1.0), sgsrWeight);
		}
	}

	color.w = inputColor.a;  // Preserve the submitted eye alpha.
	out_Target0.xyzw = color;
})glsl";
}
