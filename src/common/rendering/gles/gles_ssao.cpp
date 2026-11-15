/*
** gles_ssao.cpp
**
** Screen space ambient occlusion for the GLES backend
**
**---------------------------------------------------------------------------
**
** Copyright 2016 Magnus Norddahl (ssao.fp / depthblur.fp, which this follows)
** Copyright 2026 HEADoom Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Same algorithm, settings and strength as the OpenGL / Vulkan SSAO, written
** for what GLES 2 / GL 2 hardware has:
**
** - The scene has no normal buffer (no multiple render targets), so normals are
**   rebuilt from the depth buffer. For level geometry (flat walls, floors and
**   ceilings) this gives the same normals the other renderers store.
** - No float render targets, so the AO value is packed into two 8 bit channels
**   and the blur reads its depth weights from the depth texture.
** - Shaders use the GLSL subset shared by GLSL 1.00 and 3.30.
**
** Needs a depth texture (GL 2+, GLES 3, or GLES 2 with OES_depth_texture);
** without one the pass is skipped.
**
*/

#include <random>
#include "gles_system.h"
#include "gles_renderbuffers.h"
#include "gles_renderer.h"
#include "gles_renderstate.h"
#include "gles_postprocessstate.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "flatvertices.h"
#include "v_video.h"
#include "printf.h"

namespace OpenGLESRenderer
{

static const char *SSAOCommonSource = R"(
uniform sampler2D DepthTexture;
uniform sampler2D ColorTexture;
uniform float LinearizeDepthA;
uniform float LinearizeDepthB;
uniform vec2 Scale;
uniform vec2 Offset;
uniform vec2 InvFullResolution;

// Raw depth of the scene at an AO buffer coordinate (1.0 where nothing was drawn)
float SceneDepthRaw(vec2 uv)
{
	vec2 suv = Offset + uv * Scale;
	return texture(ColorTexture, suv).a != 0.0 ? texture(DepthTexture, suv).r : 1.0;
}

float LinearDepth(vec2 uv)
{
	return 1.0 / (clamp(SceneDepthRaw(uv), 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
}

// 16 bit value in two 8 bit channels (stays correct under linear filtering)
vec2 PackAO(float v)
{
	v = clamp(v, 0.0, 1.0) * 255.0;
	float hi = floor(v);
	return vec2(hi / 255.0, v - hi);
}

float UnpackAO(vec2 p)
{
	return (p.x * 255.0 + p.y) / 255.0;
}
)";

static const char *SSAOSource = R"(
uniform sampler2D RandomTexture;
uniform vec2 UVToViewA;
uniform vec2 UVToViewB;
uniform float NDotVBias;
uniform float NegInvR2;
uniform float RadiusToScreen;
uniform float AOMultiplier;
uniform float AOStrength;
uniform float FogDensity;
uniform int FogRadial;

#define PI 3.14159265358979323846

vec3 FetchViewPos(vec2 uv)
{
	float z = LinearDepth(uv);
	return vec3((UVToViewA * uv + UVToViewB) * z, z);
}

// Normal from the neighbouring depth values, taking the side with the smaller
// depth step so silhouette edges do not bend it
vec3 ReconstructNormal(vec2 uv, vec3 p)
{
	vec3 pr = FetchViewPos(uv + vec2(InvFullResolution.x, 0.0));
	vec3 pl = FetchViewPos(uv - vec2(InvFullResolution.x, 0.0));
	vec3 pt = FetchViewPos(uv + vec2(0.0, InvFullResolution.y));
	vec3 pb = FetchViewPos(uv - vec2(0.0, InvFullResolution.y));
	vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
	vec3 dy = abs(pt.z - p.z) < abs(p.z - pb.z) ? pt - p : p - pb;
	vec3 n = cross(dx, dy);
	if (dot(n, n) < 1.0e-12)
		return vec3(0.0);
	n = normalize(n);
	return dot(n, p) > 0.0 ? -n : n;	// face the camera
}

vec2 RotateDirection(vec2 dir, vec2 cossin)
{
	return vec2(dir.x * cossin.x - dir.y * cossin.y, dir.x * cossin.y + dir.y * cossin.x);
}

float ComputeSampleAO(vec3 kernelPos, vec3 normal, vec3 samplePos)
{
	vec3 v = samplePos - kernelPos;
	float distanceSquare = dot(v, v);
	float nDotV = dot(normal, v) * inversesqrt(distanceSquare);
	return clamp(nDotV - NDotVBias, 0.0, 1.0) * clamp(distanceSquare * NegInvR2 + 1.0, 0.0, 1.0);
}

float ComputeAO(vec3 viewPosition, vec3 viewNormal)
{
	vec4 rand = texture(RandomTexture, gl_FragCoord.xy / 4.0) * 2.0 - 1.0;

	float radiusPixels = RadiusToScreen / viewPosition.z;
	float stepSizePixels = radiusPixels / (NUM_STEPS + 1.0);

	float directionAngleStep = 2.0 * PI / NUM_DIRECTIONS;
	float ao = 0.0;

	for (float directionIndex = 0.0; directionIndex < NUM_DIRECTIONS; directionIndex += 1.0)
	{
		float angle = directionAngleStep * directionIndex;

		vec2 direction = RotateDirection(vec2(cos(angle), sin(angle)), rand.xy);
		float rayPixels = (rand.z * stepSizePixels + 1.0);

		for (float stepIndex = 0.0; stepIndex < NUM_STEPS; stepIndex += 1.0)
		{
			vec2 sampleUV = floor(rayPixels * direction + 0.5) * InvFullResolution + TexCoord;
			vec3 samplePos = FetchViewPos(sampleUV);
			ao += ComputeSampleAO(viewPosition, viewNormal, samplePos);
			rayPixels += stepSizePixels;
		}
	}

	ao *= AOMultiplier / (NUM_DIRECTIONS * NUM_STEPS);
	return clamp(1.0 - ao * 2.0, 0.0, 1.0);
}

void main()
{
	float occlusion = 1.0;
	float fogfactor = 1.0;	// 1 = no fog, 0 = all fog (same equation as the scene shaders)
	if (SceneDepthRaw(TexCoord) < 1.0)
	{
		vec3 viewPosition = FetchViewPos(TexCoord);
		vec3 viewNormal = ReconstructNormal(TexCoord, viewPosition);
		if (viewNormal != vec3(0.0))
			occlusion = ComputeAO(viewPosition, viewNormal) * AOStrength + (1.0 - AOStrength);
		float fogdist = max(16.0, FogRadial != 0 ? length(viewPosition) : viewPosition.z);
		fogfactor = exp2(FogDensity * fogdist);
	}
	FragColor = vec4(PackAO(occlusion), fogfactor, 1.0);
}
)";

static const char *BlurSource = R"(
uniform sampler2D AOTexture;
uniform float BlurSharpness;
uniform float PowExponent;

#define KERNEL_RADIUS 3.0

void AddSample(vec2 uv, float r, float centerDepth, inout float totalAO, inout float totalW)
{
	float blurSigma = KERNEL_RADIUS * 0.5;
	float blurFalloff = 1.0 / (2.0 * blurSigma * blurSigma);

	float ao = UnpackAO(texture(AOTexture, uv).xy);
	float z = LinearDepth(uv);

	float deltaZ = (z - centerDepth) * BlurSharpness;
	float w = exp2(-r * r * blurFalloff - deltaZ * deltaZ);

	totalAO += w * ao;
	totalW += w;
}

void main()
{
#if defined(BLUR_HORIZONTAL)
	vec2 stepUV = vec2(InvFullResolution.x, 0.0);
#else
	vec2 stepUV = vec2(0.0, InvFullResolution.y);
#endif
	float centerDepth = LinearDepth(TexCoord);
	vec4 center = texture(AOTexture, TexCoord);
	float totalAO = UnpackAO(center.xy);
	float totalW = 1.0;

	AddSample(TexCoord - 3.0 * stepUV, 3.0, centerDepth, totalAO, totalW);
	AddSample(TexCoord - 2.0 * stepUV, 2.0, centerDepth, totalAO, totalW);
	AddSample(TexCoord - 1.0 * stepUV, 1.0, centerDepth, totalAO, totalW);
	AddSample(TexCoord + 1.0 * stepUV, 1.0, centerDepth, totalAO, totalW);
	AddSample(TexCoord + 2.0 * stepUV, 2.0, centerDepth, totalAO, totalW);
	AddSample(TexCoord + 3.0 * stepUV, 3.0, centerDepth, totalAO, totalW);

	float fragAO = totalAO / totalW;
#if defined(BLUR_HORIZONTAL)
	FragColor = vec4(PackAO(fragAO), center.z, 1.0);
#else
	FragColor = vec4(PackAO(pow(clamp(fragAO, 0.0, 1.0), PowExponent)), center.z, 1.0);
#endif
}
)";

static const char *CombineSource = R"(
uniform sampler2D AOTexture;
uniform int DebugMode;
uniform vec3 FogColor;

void main()
{
	vec4 ao = texture(AOTexture, TexCoord);
	float attenuation = UnpackAO(ao.xy);
	// Occluded color: black up close, the fog color further into fog, so the
	// AO fades out with distance in fog (as with the fog buffer elsewhere)
	if (DebugMode == 0)
		FragColor = vec4(FogColor * (1.0 - ao.z), 1.0 - attenuation);
	else
		FragColor = vec4(attenuation, attenuation, attenuation, 1.0);
}
)";

class FGLESAmbientOcclusion
{
public:
	~FGLESAmbientOcclusion();
	void Render(FGLRenderer *renderer, FGLRenderBuffers *buffers, float m5);

private:
	bool CreatePrograms();
	void DeletePrograms();
	GLuint Link(const char *name, const char *defines, const char *body);
	void UpdateTargets(int width, int height);
	void CreateRandomTextures();

	enum { PROG_AO, PROG_BLUR_H, PROG_BLUR_V, PROG_COMBINE, NUM_PROGS };
	GLuint Programs[NUM_PROGS] = {};
	int Quality = -1;
	bool Failed = false;

	GLuint RandomTex[3] = {};
	GLuint AOTex[2] = {};
	GLuint AOFB[2] = {};
	int AOWidth = 0, AOHeight = 0;
};

void DeleteGLESAmbientOcclusion(FGLESAmbientOcclusion *&ao)
{
	delete ao;
	ao = nullptr;
}

FGLESAmbientOcclusion::~FGLESAmbientOcclusion()
{
	DeletePrograms();
	for (auto &t : RandomTex) if (t) glDeleteTextures(1, &t);
	for (auto &t : AOTex) if (t) glDeleteTextures(1, &t);
	for (auto &f : AOFB) if (f) glDeleteFramebuffers(1, &f);
}

void FGLESAmbientOcclusion::DeletePrograms()
{
	for (auto &p : Programs)
	{
		if (p) glDeleteProgram(p);
		p = 0;
	}
}

static GLuint CompileStage(GLenum type, const FString &code, const char *name)
{
	GLuint handle = glCreateShader(type);
	const char *src = code.GetChars();
	GLint len = (GLint)code.Len();
	glShaderSource(handle, 1, &src, &len);
	glCompileShader(handle);
	GLint status = 0;
	glGetShaderiv(handle, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE)
	{
		static char log[4096];
		GLsizei loglen = 0;
		glGetShaderInfoLog(handle, sizeof(log), &loglen, log);
		Printf(TEXTCOLOR_RED "GLES SSAO shader '%s' does not compile:\n%s\n", name, log);
		glDeleteShader(handle);
		return 0;
	}
	return handle;
}

GLuint FGLESAmbientOcclusion::Link(const char *name, const char *defines, const char *body)
{
	const char *version = gles.forceGLSLv100 ? "100" : gles.shaderVersionString;
	const bool es100 = strcmp(version, "100") == 0;

	FString prolog;
	prolog.Format("#version %s\n", version);
	// GLSL ES 1.00 samplers default to lowp, too coarse for depth
	prolog += "#ifdef GL_ES\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n#endif\n";

	FString vp = prolog, fp = prolog;
	if (es100)
	{
		vp += "attribute vec4 PositionInProjection;\nattribute vec2 UV;\nvarying vec2 TexCoord;\n";
		fp += "#define texture texture2D\n#define FragColor gl_FragColor\nvarying vec2 TexCoord;\n";
	}
	else
	{
		vp += "in vec4 PositionInProjection;\nin vec2 UV;\nout vec2 TexCoord;\n";
		fp += "in vec2 TexCoord;\nout vec4 FragColor;\n";
	}
	vp += "void main()\n{\n\tgl_Position = PositionInProjection;\n\tTexCoord = UV;\n}\n";
	fp += defines;
	fp += SSAOCommonSource;
	fp += body;

	GLuint vs = CompileStage(GL_VERTEX_SHADER, vp, name);
	GLuint fs = vs ? CompileStage(GL_FRAGMENT_SHADER, fp, name) : 0;
	if (!vs || !fs)
	{
		if (vs) glDeleteShader(vs);
		return 0;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glBindAttribLocation(program, VATTR_VERTEX, "PositionInProjection");
	glBindAttribLocation(program, VATTR_TEXCOORD, "UV");
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE)
	{
		static char log[4096];
		GLsizei loglen = 0;
		glGetProgramInfoLog(program, sizeof(log), &loglen, log);
		Printf(TEXTCOLOR_RED "GLES SSAO shader '%s' does not link:\n%s\n", name, log);
		glDeleteProgram(program);
		return 0;
	}

	// fixed sampler units: 0 = depth, 1 = scene color, 2 = AO input / random
	glUseProgram(program);
	GLint loc;
	if ((loc = glGetUniformLocation(program, "DepthTexture")) != -1) glUniform1i(loc, 0);
	if ((loc = glGetUniformLocation(program, "ColorTexture")) != -1) glUniform1i(loc, 1);
	if ((loc = glGetUniformLocation(program, "RandomTexture")) != -1) glUniform1i(loc, 2);
	if ((loc = glGetUniformLocation(program, "AOTexture")) != -1) glUniform1i(loc, 2);
	return program;
}

bool FGLESAmbientOcclusion::CreatePrograms()
{
	if (Quality == gl_ssao)
		return !Failed;

	DeletePrograms();
	Quality = gl_ssao;
	Failed = false;

	// Must match PPAmbientOcclusion::CreateShaders
	int numDirections;
	switch (gl_ssao)
	{
	default:
	case 1: numDirections = 2; break;
	case 2: numDirections = 4; break;
	case 3: numDirections = 8; break;
	}
	FString aoDefines;
	aoDefines.Format("#define NUM_DIRECTIONS %d.0\n#define NUM_STEPS 4.0\n", numDirections);

	Programs[PROG_AO] = Link("ssao", aoDefines.GetChars(), SSAOSource);
	Programs[PROG_BLUR_H] = Link("ssao blur", "#define BLUR_HORIZONTAL\n", BlurSource);
	Programs[PROG_BLUR_V] = Link("ssao blur", "", BlurSource);
	Programs[PROG_COMBINE] = Link("ssao combine", "", CombineSource);

	for (auto p : Programs)
		if (p == 0)
			Failed = true;
	return !Failed;
}

// Same random rotations as PPAmbientOcclusion (same generator and seed)
void FGLESAmbientOcclusion::CreateRandomTextures()
{
	if (RandomTex[0])
		return;

	double numDirections[3] = { 2.0, 4.0, 8.0 };
	std::mt19937 generator(1337);
	std::uniform_real_distribution<double> distribution(0.0, 1.0);
	for (int quality = 0; quality < 3; quality++)
	{
		uint8_t data[16 * 4];
		for (int i = 0; i < 16; i++)
		{
			double angle = 2.0 * M_PI * distribution(generator) / numDirections[quality];
			double v[4] = { cos(angle), sin(angle), distribution(generator), distribution(generator) };
			for (int c = 0; c < 4; c++)
				data[i * 4 + c] = (uint8_t)clamp((v[c] * 0.5 + 0.5) * 255.0 + 0.5, 0.0, 255.0);
		}
		glGenTextures(1, &RandomTex[quality]);
		glBindTexture(GL_TEXTURE_2D, RandomTex[quality]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
}

void FGLESAmbientOcclusion::UpdateTargets(int width, int height)
{
	if (width == AOWidth && height == AOHeight && AOTex[0])
		return;

	for (int i = 0; i < 2; i++)
	{
		if (AOTex[i]) glDeleteTextures(1, &AOTex[i]);
		if (AOFB[i]) glDeleteFramebuffers(1, &AOFB[i]);
		glGenTextures(1, &AOTex[i]);
		glBindTexture(GL_TEXTURE_2D, AOTex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenFramebuffers(1, &AOFB[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, AOFB[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, AOTex[i], 0);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	AOWidth = width;
	AOHeight = height;
}

static void SetFilter(GLuint tex, GLint filter)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

void FGLESAmbientOcclusion::Render(FGLRenderer *renderer, FGLRenderBuffers *buffers, float m5)
{
	const int sceneWidth = screen->mSceneViewport.width;	// the 3D view, scaled or not
	const int sceneHeight = screen->mSceneViewport.height;
	if (gl_ssao == 0 || sceneWidth <= 0 || sceneHeight <= 0 || hw_postprocess.ssao.IsDisabledForLevel())
		return;
	if (!buffers->HasSceneDepthTexture())
		return;

	// Save what the scene renderer has set up; everything is put back below
	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(3);
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	GLboolean stencilEnabled = glIsEnabled(GL_STENCIL_TEST);
	GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);
	GLboolean colorMask[4];
	glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	if (CreatePrograms())
	{
		CreateRandomTextures();
		const int aoWidth = (sceneWidth + 1) / 2;
		const int aoHeight = (sceneHeight + 1) / 2;
		UpdateTargets(aoWidth, aoHeight);

		// Uniforms, as in PPAmbientOcclusion::Render
		const float tanHalfFovy = 1.0f / m5;
		const float invFocalLenX = tanHalfFovy * (sceneWidth / (float)sceneHeight);
		const float invFocalLenY = tanHalfFovy;
		const float nDotVBias = clamp((float)gl_ssao_bias, 0.0f, 1.0f);
		const float aoRadius = gl_ssao_radius;
		const float r2 = aoRadius * aoRadius;
		const FVector2 sceneScale = screen->SceneScale();
		const FVector2 sceneOffset = screen->SceneOffset();
		const float linearA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
		const float linearB = max(1.0f / screen->GetZNear(), 1.e-8f);

		auto setCommon = [&](GLuint prog)
		{
			glUseProgram(prog);
			glUniform1f(glGetUniformLocation(prog, "LinearizeDepthA"), linearA);
			glUniform1f(glGetUniformLocation(prog, "LinearizeDepthB"), linearB);
			glUniform2f(glGetUniformLocation(prog, "Scale"), sceneScale.X, sceneScale.Y);
			glUniform2f(glGetUniformLocation(prog, "Offset"), sceneOffset.X, sceneOffset.Y);
			glUniform2f(glGetUniformLocation(prog, "InvFullResolution"), 1.0f / aoWidth, 1.0f / aoHeight);
		};

		buffers->BindSceneDepthTexture(0);
		buffers->BindCurrentTexture(1, GL_NEAREST, GL_CLAMP_TO_EDGE);

		// 1. ambient occlusion at half resolution
		GLuint prog = Programs[PROG_AO];
		setCommon(prog);
		glUniform2f(glGetUniformLocation(prog, "UVToViewA"), 2.0f * invFocalLenX, 2.0f * invFocalLenY);
		glUniform2f(glGetUniformLocation(prog, "UVToViewB"), -invFocalLenX, -invFocalLenY);
		glUniform1f(glGetUniformLocation(prog, "NDotVBias"), nDotVBias);
		glUniform1f(glGetUniformLocation(prog, "NegInvR2"), -1.0f / r2);
		glUniform1f(glGetUniformLocation(prog, "RadiusToScreen"), aoRadius * 0.5f / tanHalfFovy * aoHeight);
		glUniform1f(glGetUniformLocation(prog, "AOMultiplier"), 1.0f / (1.0f - nDotVBias));
		glUniform1f(glGetUniformLocation(prog, "AOStrength"), gl_ssao_strength);
		glUniform1f(glGetUniformLocation(prog, "FogDensity"), ssaoViewFog.Density);
		glUniform1i(glGetUniformLocation(prog, "FogRadial"), ssaoViewFog.Radial ? 1 : 0);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, RandomTex[clamp((int)gl_ssao - 1, 0, 2)]);
		glBindFramebuffer(GL_FRAMEBUFFER, AOFB[0]);
		glViewport(0, 0, aoWidth, aoHeight);
		renderer->RenderScreenQuad();

		// 2. depth-aware blur
		if (gl_ssao_debug < 2)
		{
			for (int pass = 0; pass < 2; pass++)
			{
				prog = Programs[pass == 0 ? PROG_BLUR_H : PROG_BLUR_V];
				setCommon(prog);
				glUniform1f(glGetUniformLocation(prog, "BlurSharpness"), 1.0f / gl_ssao_blur);
				glUniform1f(glGetUniformLocation(prog, "PowExponent"), gl_ssao_exponent);
				glActiveTexture(GL_TEXTURE2);
				SetFilter(AOTex[pass], GL_NEAREST);
				glBindFramebuffer(GL_FRAMEBUFFER, AOFB[1 - pass]);
				renderer->RenderScreenQuad();
			}
		}

		// 3. darken the scene
		prog = Programs[PROG_COMBINE];
		glUseProgram(prog);
		glUniform1i(glGetUniformLocation(prog, "DebugMode"), (int)gl_ssao_debug);
		glUniform3f(glGetUniformLocation(prog, "FogColor"), ssaoViewFog.Color[0], ssaoViewFog.Color[1], ssaoViewFog.Color[2]);
		glActiveTexture(GL_TEXTURE2);
		SetFilter(AOTex[0], GL_LINEAR);
		buffers->BindSceneFB(true);
		const IntRect &vp = screen->mSceneViewport;
		glViewport(vp.left, vp.top, vp.width, vp.height);
		if (gl_ssao_debug == 0)
		{
			glEnable(GL_BLEND);
			glBlendEquation(GL_FUNC_ADD);
			glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
		}
		// As on OpenGL and Vulkan, the scene's stencil test stays on here, so the
		// pass for a portal only darkens that portal. Without it every portal pass
		// darkened the whole view again.
		GLint stencilOps[3] = { GL_KEEP, GL_KEEP, GL_KEEP };
		if (stencilEnabled)
		{
			glGetIntegerv(GL_STENCIL_FAIL, &stencilOps[0]);
			glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &stencilOps[1]);
			glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &stencilOps[2]);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			glEnable(GL_STENCIL_TEST);
		}
		renderer->RenderScreenQuad();
		if (stencilEnabled)
		{
			glDisable(GL_STENCIL_TEST);
			glStencilOp(stencilOps[0], stencilOps[1], stencilOps[2]);
		}
		glDisable(GL_BLEND);
		glActiveTexture(GL_TEXTURE0);
	}

	buffers->BindSceneFB(true);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if (stencilEnabled) glEnable(GL_STENCIL_TEST);
	if (cullEnabled) glEnable(GL_CULL_FACE);
	glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
	// the screen quad changed the vertex buffer binding
	static_cast<FGLRenderState *>(screen->RenderState())->ResetVertexBuffer();
}

void FGLRenderer::AmbientOccludeScene(float m5)
{
	if (!mSSAO)
		mSSAO = new FGLESAmbientOcclusion;
	mSSAO->Render(this, mBuffers, m5);
}

}
