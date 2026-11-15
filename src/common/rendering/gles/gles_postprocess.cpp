/*
** gles_postprocess.cpp
**
** Postprocessing framework
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include "gles_system.h"
#include "m_png.h"
#include "gles_buffers.h"
#include "gles_framebuffer.h"
#include "gles_renderbuffers.h"
#include "gles_renderer.h"
#include "gles_postprocessstate.h"
#include "gles_renderstate.h"
#include "gles_shaderprogram.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "flatvertices.h"
#include "r_videoscale.h"
#include "v_video.h"

#include "hw_vrmodes.h"
#include "v_draw.h"
#include "i_time.h"
#include "printf.h"
#include "cmdlib.h"
#include "filesystem.h"
#include "texturemanager.h"
#include "hwrenderer/postprocessing/hw_postprocessshader.h"

extern bool vid_hdr_active;
EXTERN_CVAR(Bool, gl_custompost)


namespace OpenGLESRenderer
{

int gl_dither_bpc = -1;

void FGLRenderer::RenderScreenQuad()
{
	auto buffer = static_cast<GLVertexBuffer *>(screen->mVertexData->GetBufferObjects().first);
	buffer->Bind(nullptr);
	glDrawArrays(GL_TRIANGLE_STRIP, FFlatVertexBuffer::PRESENT_INDEX, 3);
}

void FGLRenderer::PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
#ifndef NO_RENDER_BUFFER
	mBuffers->BindCurrentFB();
	RunCustomPostShaders("beforebloom");
#endif
	if (afterBloomDrawEndScene2D) afterBloomDrawEndScene2D();
#ifndef NO_RENDER_BUFFER
	RunCustomPostShaders("scene");
#endif
}

//-----------------------------------------------------------------------------
//
// GLDEFS "PostProcess" shaders on the GLES backend.
//
// The shader source gets a GLSL 1.00 (GLES 2 / GL 2) or GLSL 3.30 prolog, so a
// shader written with the portable subset works on every backend. The image
// ping-pongs between the scene buffer and a second buffer and ends up in the
// scene buffer again.
//
//-----------------------------------------------------------------------------

FGLESCustomPostShader::~FGLESCustomPostShader()
{
	if (Program != 0)
		glDeleteProgram(Program);
	for (auto h : TextureHandles)
		if (h != 0)
			glDeleteTextures(1, &h);
}

static GLuint CompileGLESShaderStage(GLenum type, const FString &code, const char *name)
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
		Printf(TEXTCOLOR_RED "Post-process shader '%s' does not compile on this renderer:\n%s\n", name, log);
		glDeleteShader(handle);
		return 0;
	}
	return handle;
}

bool FGLRenderer::CompileCustomPostShader(int index, FGLESCustomPostShader &out)
{
	const PostProcessShader &desc = PostProcessShaders[index];
	// gles_force_glsl_v100 also forces GLSL 1.00 here (to test the GLES 2 path on desktop GL)
	const char *version = gles.forceGLSLv100 ? "100" : gles.shaderVersionString;
	const bool es100 = strcmp(version, "100") == 0;

	int lump = fileSystem.CheckNumForFullName(desc.ShaderLumpName.GetChars());
	if (lump == -1)
	{
		Printf(TEXTCOLOR_RED "Post-process shader '%s' not found\n", desc.ShaderLumpName.GetChars());
		return false;
	}

	FString prolog;
	prolog.Format("#version %s\n", version);
	prolog += "#ifdef GL_ES\nprecision highp float;\nprecision highp int;\n#endif\n";

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

	fp += "uniform sampler2D InputTexture;\n";
	TMap<FString, FString>::ConstIterator itTex(desc.Textures);
	TMap<FString, FString>::ConstPair *texPair;
	while (itTex.NextPair(texPair))
	{
		fp.AppendFormat("uniform sampler2D %s;\n", texPair->Key.GetChars());
		out.TextureNames.Push(texPair->Key);
	}
	fp += "uniform sampler2D LastInputTexture;\n";
	fp += "uniform float InputTimeDelta;\nuniform float InputTime;\nuniform float InputTimeGame;\n";

	TMap<FString, PostProcessUniformValue>::ConstIterator itUni(desc.Uniforms);
	TMap<FString, PostProcessUniformValue>::ConstPair *uniPair;
	while (itUni.NextPair(uniPair))
	{
		const char *type = nullptr;
		switch (uniPair->Value.Type)
		{
		case PostProcessUniformType::Float: type = "float"; break;
		case PostProcessUniformType::Int: type = "int"; break;
		case PostProcessUniformType::Vec2: type = "vec2"; break;
		case PostProcessUniformType::Vec3: type = "vec3"; break;
		case PostProcessUniformType::Vec4: type = "vec4"; break;
		default: break;
		}
		if (type)
			fp.AppendFormat("uniform %s %s;\n", type, uniPair->Key.GetChars());
	}
	fp += "#line 1\n";
	fp += GetStringFromLump(lump);

	GLuint vs = CompileGLESShaderStage(GL_VERTEX_SHADER, vp, desc.ShaderLumpName.GetChars());
	GLuint fs = vs ? CompileGLESShaderStage(GL_FRAGMENT_SHADER, fp, desc.ShaderLumpName.GetChars()) : 0;
	if (!vs || !fs)
	{
		if (vs) glDeleteShader(vs);
		return false;
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
		Printf(TEXTCOLOR_RED "Post-process shader '%s' does not link on this renderer:\n%s\n", desc.ShaderLumpName.GetChars(), log);
		glDeleteProgram(program);
		return false;
	}
	out.Program = program;

	// sampler units: 0 = input, 1.. = GLDEFS textures, then LastInputTexture
	glUseProgram(program);
	GLint loc = glGetUniformLocation(program, "InputTexture");
	if (loc != -1) glUniform1i(loc, 0);
	for (unsigned i = 0; i < out.TextureNames.Size(); i++)
	{
		loc = glGetUniformLocation(program, out.TextureNames[i].GetChars());
		if (loc != -1) glUniform1i(loc, 1 + i);

		GLuint handle = 0;
		const FString *texName = desc.Textures.CheckKey(out.TextureNames[i]);
		auto gtex = texName ? TexMan.GetGameTexture(TexMan.CheckForTexture(texName->GetChars(), ETextureType::Any), true) : nullptr;
		if (gtex && gtex->isValid())
		{
			auto buffer = gtex->GetTexture()->CreateTexBuffer(0);
			const int count = buffer.mWidth * buffer.mHeight;
			TArray<uint8_t> pixels(count * 4, true);
			for (int p = 0; p < count; p++)
			{
				pixels[p * 4] = buffer.mBuffer[p * 4 + 2];
				pixels[p * 4 + 1] = buffer.mBuffer[p * 4 + 1];
				pixels[p * 4 + 2] = buffer.mBuffer[p * 4];
				pixels[p * 4 + 3] = buffer.mBuffer[p * 4 + 3];
			}
			glGenTextures(1, &handle);
			glBindTexture(GL_TEXTURE_2D, handle);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, buffer.mWidth, buffer.mHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.Data());
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
		out.TextureHandles.Push(handle);
	}
	out.LastInputLoc = glGetUniformLocation(program, "LastInputTexture");
	if (out.LastInputLoc != -1) glUniform1i(out.LastInputLoc, 1 + out.TextureNames.Size());
	out.TimeLoc = glGetUniformLocation(program, "InputTime");
	out.TimeDeltaLoc = glGetUniformLocation(program, "InputTimeDelta");
	out.TimeGameLoc = glGetUniformLocation(program, "InputTimeGame");
	return true;
}

void FGLRenderer::RunCustomPostShaders(const char *target)
{
	if (!gl_custompost || PostProcessShaders.Size() == 0)
		return;

	bool any = false;
	for (auto &desc : PostProcessShaders)
		if (desc.Enabled && desc.Target.CompareNoCase(target) == 0)
			any = true;
	if (!any)
		return;

	if (mCustomPostShaders.Size() != PostProcessShaders.Size())
	{
		for (auto s : mCustomPostShaders) delete s;
		mCustomPostShaders.Clear();
		for (unsigned i = 0; i < PostProcessShaders.Size(); i++)
			mCustomPostShaders.Push(new FGLESCustomPostShader);
	}

	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(8);

	const int width = mBuffers->GetWidth();
	const int height = mBuffers->GetHeight();
	const float time = static_cast<float>(screen->FrameTime / 1000.0);
	const float timeDelta = static_cast<float>(GetDeltaTime());

	bool inScene = true;	// where the current image is
	for (unsigned i = 0; i < PostProcessShaders.Size(); i++)
	{
		const PostProcessShader &desc = PostProcessShaders[i];
		if (!desc.Enabled || desc.Target.CompareNoCase(target) != 0)
			continue;

		FGLESCustomPostShader &sh = *mCustomPostShaders[i];
		if (sh.Program == 0)
		{
			if (sh.Failed)
				continue;
			if (!CompileCustomPostShader(i, sh))
			{
				sh.Failed = true;
				continue;
			}
		}

		if (inScene)
		{
			mBuffers->BindPPFB();
			mBuffers->BindCurrentTexture(0, GL_NEAREST, GL_CLAMP_TO_EDGE);
		}
		else
		{
			mBuffers->BindCurrentFB();
			mBuffers->BindPPTexture(0, GL_NEAREST, GL_CLAMP_TO_EDGE);
		}
		for (unsigned t = 0; t < sh.TextureHandles.Size(); t++)
		{
			glActiveTexture(GL_TEXTURE1 + t);
			glBindTexture(GL_TEXTURE_2D, sh.TextureHandles[t]);
		}
		if (sh.LastInputLoc != -1)
		{
			// no history buffer here: the previous image is the current input
			if (inScene) mBuffers->BindCurrentTexture(1 + sh.TextureHandles.Size());
			else mBuffers->BindPPTexture(1 + sh.TextureHandles.Size());
		}
		glActiveTexture(GL_TEXTURE0);

		glUseProgram(sh.Program);
		if (sh.TimeLoc != -1) glUniform1f(sh.TimeLoc, time);
		if (sh.TimeDeltaLoc != -1) glUniform1f(sh.TimeDeltaLoc, timeDelta);
		if (sh.TimeGameLoc != -1) glUniform1f(sh.TimeGameLoc, time);

		TMap<FString, PostProcessUniformValue>::ConstIterator it(desc.Uniforms);
		TMap<FString, PostProcessUniformValue>::ConstPair *pair;
		while (it.NextPair(pair))
		{
			const GLint loc = glGetUniformLocation(sh.Program, pair->Key.GetChars());
			if (loc == -1)
				continue;
			const double *v = pair->Value.Values;
			switch (pair->Value.Type)
			{
			case PostProcessUniformType::Float: glUniform1f(loc, (float)v[0]); break;
			case PostProcessUniformType::Int: glUniform1i(loc, (int)v[0]); break;
			case PostProcessUniformType::Vec2: glUniform2f(loc, (float)v[0], (float)v[1]); break;
			case PostProcessUniformType::Vec3: glUniform3f(loc, (float)v[0], (float)v[1], (float)v[2]); break;
			case PostProcessUniformType::Vec4: glUniform4f(loc, (float)v[0], (float)v[1], (float)v[2], (float)v[3]); break;
			default: break;
			}
		}

		glViewport(0, 0, width, height);
		RenderScreenQuad();
		inScene = !inScene;
	}

	if (!inScene)
		mBuffers->CopyPPToScene();
	mBuffers->BindCurrentFB();
	glViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);
}

//-----------------------------------------------------------------------------
//
// Stretches a 3D view rendered at a lower resolution (vid_scalefactor) back
// to its full size, like the OpenGL and Vulkan backends do. GLES 2 has no
// glBlitFramebuffer, so this draws a quad into the second buffer and copies
// the result back.
//
//-----------------------------------------------------------------------------

void FGLRenderer::UpscaleScene(const IntRect &src, const IntRect &dst, bool linear)
{
	if (mUpscaleFailed || dst.width <= 0 || dst.height <= 0)
		return;

	if (mUpscaleProgram == 0)
	{
		const char *version = gles.forceGLSLv100 ? "100" : gles.shaderVersionString;
		const bool es100 = strcmp(version, "100") == 0;
		FString prolog;
		prolog.Format("#version %s\n", version);
		prolog += "#ifdef GL_ES\nprecision highp float;\n#endif\n";
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
		fp += "uniform sampler2D InputTexture;\nuniform vec4 SourceRect;\n";
		fp += "void main()\n{\n\tFragColor = texture(InputTexture, SourceRect.xy + TexCoord * SourceRect.zw);\n}\n";

		GLuint vs = CompileGLESShaderStage(GL_VERTEX_SHADER, vp, "scene upscale");
		GLuint fs = vs ? CompileGLESShaderStage(GL_FRAGMENT_SHADER, fp, "scene upscale") : 0;
		if (!vs || !fs)
		{
			if (vs) glDeleteShader(vs);
			mUpscaleFailed = true;
			return;
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
			glDeleteProgram(program);
			mUpscaleFailed = true;
			return;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "InputTexture"), 0);
		mUpscaleProgram = program;
	}

	FGLPostProcessState savedState;
	GLboolean stencilEnabled = glIsEnabled(GL_STENCIL_TEST);
	glDisable(GL_STENCIL_TEST);

	const float w = (float)mBuffers->GetWidth();
	const float h = (float)mBuffers->GetHeight();
	mBuffers->BindPPFB();
	glViewport(0, 0, mBuffers->GetWidth(), mBuffers->GetHeight());
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport(dst.left, dst.top, dst.width, dst.height);
	mBuffers->BindCurrentTexture(0, linear ? GL_LINEAR : GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(mUpscaleProgram);
	glUniform4f(glGetUniformLocation(mUpscaleProgram, "SourceRect"), src.left / w, src.top / h, src.width / w, src.height / h);
	RenderScreenQuad();
	mBuffers->CopyPPToScene();

	mBuffers->BindCurrentFB();
	if (stencilEnabled) glEnable(GL_STENCIL_TEST);
	glViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);
	static_cast<FGLRenderState *>(screen->RenderState())->ResetVertexBuffer();
}



//-----------------------------------------------------------------------------
//
// Copies the rendered screen to its final destination
//
//-----------------------------------------------------------------------------

void FGLRenderer::Flush()
{
	CopyToBackbuffer(nullptr, true);
}

//-----------------------------------------------------------------------------
//
// Gamma correct while copying to frame buffer
//
//-----------------------------------------------------------------------------

void FGLRenderer::CopyToBackbuffer(const IntRect *bounds, bool applyGamma)
{
#ifdef NO_RENDER_BUFFER
	mBuffers->BindOutputFB();
#endif
	screen->Draw2D();	// draw all pending 2D stuff before copying the buffer
	twod->Clear();

	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);

	mBuffers->BindOutputFB();

	IntRect box;
	if (bounds)
	{
		box = *bounds;
	}
	else
	{
		ClearBorders();
		box = screen->mOutputLetterbox;
	}

	mBuffers->BindCurrentTexture(0);
#ifndef NO_RENDER_BUFFER
	DrawPresentTexture(box, applyGamma);
#endif
}

void FGLRenderer::DrawPresentTexture(const IntRect &box, bool applyGamma)
{
	glViewport(box.left, box.top, box.width, box.height);

	mBuffers->BindDitherTexture(1);

	glActiveTexture(GL_TEXTURE0);
	if (ViewportLinearScale())
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	mPresentShader->Bind();
	if (!applyGamma || framebuffer->IsHWGammaActive())
	{
		mPresentShader->Uniforms->InvGamma = 1.0f;
		mPresentShader->Uniforms->Contrast = 1.0f;
		mPresentShader->Uniforms->Saturation = 1.0f;
		mPresentShader->Uniforms->BlackPoint = 0.0f;
		mPresentShader->Uniforms->WhitePoint = 1.0f;
	}
	else
	{
		mPresentShader->Uniforms->InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
		mPresentShader->Uniforms->Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		mPresentShader->Uniforms->Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
		mPresentShader->Uniforms->BlackPoint = clamp<float>(vid_i_blackpoint, 0.f, 1.f);
		mPresentShader->Uniforms->WhitePoint = clamp<float>(vid_i_whitepoint, 0.f, 5.f);
		mPresentShader->Uniforms->GrayFormula = static_cast<int>(gl_satformula);
	}
	if (vid_hdr_active && framebuffer->IsFullscreen())
	{
		// Full screen exclusive mode treats a rgba16f frame buffer as linear.
		// It probably will eventually in desktop mode too, but the DWM doesn't seem to support that.
		mPresentShader->Uniforms->HdrMode = 1;
		mPresentShader->Uniforms->ColorScale = (gl_dither_bpc == -1) ? 1023.0f : (float)((1 << gl_dither_bpc) - 1);
	}
	else
	{
		mPresentShader->Uniforms->HdrMode = 0;
		mPresentShader->Uniforms->ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);
	}

	mPresentShader->Uniforms->Scale = { screen->mScreenViewport.width / (float)mBuffers->GetWidth(), screen->mScreenViewport.height / (float)mBuffers->GetHeight() };
	mPresentShader->Uniforms->Offset = { 0.0f, 0.0f };
	mPresentShader->Uniforms.SetData();

	for (size_t n = 0; n < mPresentShader->Uniforms.mFields.size(); n++)
	{
		UniformFieldDesc desc = mPresentShader->Uniforms.mFields[n];
		int loc = mPresentShader->Uniforms.UniformLocation[n];
		switch (desc.Type)
		{
		case UniformType::Int:
			glUniform1i(loc, *((GLint*)(((char*)(&mPresentShader->Uniforms)) + desc.Offset)));
			break;
		case UniformType::Float:
			glUniform1f(loc, *((GLfloat*)(((char*)(&mPresentShader->Uniforms)) + desc.Offset)));
			break;
		case UniformType::Vec2:
			glUniform2fv(loc,1 , ((GLfloat*)(((char*)(&mPresentShader->Uniforms)) + desc.Offset)));
			break;
		default:
			break;
		}
	}

	RenderScreenQuad();
}

//-----------------------------------------------------------------------------
//
// Fills the black bars around the screen letterbox
//
//-----------------------------------------------------------------------------

void FGLRenderer::ClearBorders()
{
	const auto &box = screen->mOutputLetterbox;

	int clientWidth = framebuffer->GetClientWidth();
	int clientHeight = framebuffer->GetClientHeight();
	if (clientWidth == 0 || clientHeight == 0)
		return;

	glViewport(0, 0, clientWidth, clientHeight);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glEnable(GL_SCISSOR_TEST);
	if (box.top > 0)
	{
		glScissor(0, 0, clientWidth, box.top);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (clientHeight - box.top - box.height > 0)
	{
		glScissor(0, box.top + box.height, clientWidth, clientHeight - box.top - box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (box.left > 0)
	{
		glScissor(0, box.top, box.left, box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (clientWidth - box.left - box.width > 0)
	{
		glScissor(box.left + box.width, box.top, clientWidth - box.left - box.width, box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
}

}
